/*
 * Fobos OS — задачи, планировщик, жизненный цикл процессов (FND-3a).
 *
 * Ключевые решения (почему так):
 *  - Пул задач фиксированного размера (TASK_MAX): ядерные структуры без
 *    динамической памяти — нет пути «alloc не удался посреди switch».
 *  - Переключение — единый механизм (sched_switch_asm): вытеснение по
 *    таймеру (intr_c_handler), добровольный yield и блокировка ходят через
 *    одну и ту же точку сохранения контекста => один путь = меньше ошибок.
 *  - Ядерный код не вытесняется: прерывания переключают только задачи
 *    из ring 3. Гонок с GUI/консолью ядра нет по построению.
 *  - Стек ядра каждой задачи — 64 КиБ из физ-пула; syscall и прерывания
 *    из ring 3 исполняются на нём (TSS.RSP0 / g_syscall_kstack).
 *  - Zombie: ресурсы (user space, кадры стека) освобождаются тем, кто
 *    следующим войдёт в sched_schedule после switch от умершего — стек
 *    умершего к этому моменту уже не используется.
 */
#include "task.h"

#include <fobos/errors.h>
#include <fobos/syscall.h>

#include "syscall.h"
#include "elf.h"
#include "../arch/tss.h"
#include "../mm/vmm.h"
#include "../mm/phys.h"
#include "../lib/log.h"
#include "../lib/string.h"

/* Раскладка user-пространства — из task.h (USER_*). */

/* RPL3-селекторы сегментов (gdt.c: UCODE=0x18, UDATA=0x20). */
#define SEL_UCODE3 0x1Bu
#define SEL_UDATA3 0x23u

/* RFLAGS первого входа в user: IF=1, bit1=1. */
#define USER_RFLAGS 0x202ull

static struct task g_tasks[TASK_MAX];
static struct task *g_current;
static int g_sched_ready;
static int g_need_resched;
static struct task *g_zombie_reap;   /* освобождается после следующего switch */
static u32 g_next_pid;
static u64 g_ctx_switches;
static u64 g_spawns;
static u64 g_exits;
static const char *const TASK_TAG = "sys/task";

/* Активные wait-очереди (v0: клавиатура). */
static struct task *g_wq_kbd;

extern void sched_switch_asm(struct task_ctx *save_to,
                             const struct task_ctx *load_from);
extern void sched_user_entry(void);
extern u64 g_entry_frame;   /* switch.S */

/* Драйвер клавиатуры пробуждает читателей (hook из kbd.c). */
static void kbd_wake_hook(void)
{
    task_wakeup(&g_wq_kbd);
}

static const char *state_name(enum task_state st)
{
    switch (st) {
    case TASK_READY:   return "ready";
    case TASK_RUNNING: return "running";
    case TASK_BLOCKED: return "blocked";
    case TASK_ZOMBIE:  return "zombie";
    default:           return "free";
    }
}

struct task *task_current(void)
{
    return g_current;
}

int sched_need_resched(void)
{
    return (g_sched_ready != 0 && g_need_resched != 0) ? 1 : 0;
}

static u64 read_rflags(void)
{
    u64 f;
    __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(f));
    return f;
}

static void cli(void)
{
    __asm__ volatile ("cli" : : : "memory");
}

static void sti(void)
{
    __asm__ volatile ("sti" : : : "memory");
}

/* Кладёт на kernel-стек задачи стартовый intr_frame для входа в ring 3
 * и готовит switch-кадр (раскладка обязана совпадать с sched_switch_asm:
 * [rbx][rbp][r12][r13][r14][r15][ret=sched_user_entry]). */
static void build_entry_frame(struct task *t, u64 entry_rip, u64 user_rsp,
                              u64 arg_rdi, u64 arg_rsi)
{
    /* Switch-кадр: 6 нулевых callee-saved + ret, от вершины стека:
     * [rbx@-56][rbp@-48][r12@-40][r13@-32][r14@-24][r15@-16][ret@-8]. */
    u64 *ret_slot = (u64 *)(t->kstack_top - sizeof(u64));
    u64 *regs0 = (u64 *)(t->kstack_top - 7u * sizeof(u64));
    *ret_slot = (u64)(u64 *)sched_user_entry;
    for (u32 i = 0; i < 6u; i++) {
        regs0[i] = 0;
    }
    t->ctx.rsp = (u64)regs0;
    t->ctx.ret = 0;   /* не используется: ret берётся со стека */

    struct intr_frame *f = (struct intr_frame *)(t->kstack_top -
                                                 7u * sizeof(u64) -
                                                 sizeof(struct intr_frame));
    memset(f, 0, sizeof(*f));
    f->vector = 0;
    f->errcode = 0;
    f->rax = 0;
    f->rbx = 0;
    f->rcx = 0;
    f->rdx = 0;
    f->rsi = arg_rsi;   /* argv */
    f->rdi = arg_rdi;   /* argc */
    f->rbp = 0;
    f->r8 = 0;
    f->r9 = 0;
    f->r10 = 0;
    f->r11 = 0;
    f->r12 = 0;
    f->r13 = 0;
    f->r14 = 0;
    f->r15 = 0;
    f->rip = entry_rip;
    f->cs = SEL_UCODE3;
    f->rflags = USER_RFLAGS;
    f->rsp = user_rsp;
    f->ss = SEL_UDATA3;

    t->entry_frame = (u64)f;
    t->fresh = 1;
}

/* Освобождение ресурсов задачи (стек ядра, слот). Задача не исполняется. */
static void task_free(struct task *t)
{
    if (t->kstack != 0u) {
        for (u32 i = 0; i < TASK_KSTACK_FRAMES; i++) {
            (void)phys_free_frame(t->kstack + (u64)i * PAGE_SIZE);
        }
        t->kstack = 0;
        t->kstack_top = 0;
    }
    t->state = TASK_FREE;
    t->pid = 0;
}

/* Выбор следующей READY-задачи (RR: следующий слот после текущего).
 * Возвращает NULL, если кроме текущей исполняющихся нет. */
static struct task *pick_next(void)
{
    if (g_current == NULL) {
        return NULL;
    }
    int start = g_current->slot;
    for (u32 i = 1u; i < TASK_MAX; i++) {
        int idx = (start + (int)i) % TASK_MAX;
        struct task *t = &g_tasks[idx];
        if (t->state == TASK_READY) {
            return t;
        }
    }
    return NULL;
}

/* Ядро переключения. Контекст: IF=0 (вызывается из IRQ или с cli). */
void sched_schedule(void)
{
    if (g_sched_ready == 0 || g_current == NULL) {
        return;
    }

    struct task *next = pick_next();
    if (next == NULL) {
        g_need_resched = 0;
        return;
    }
    if (next == g_current) {
        g_need_resched = 0;
        return;
    }

    struct task *prev = g_current;
    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
    }
    next->state = TASK_RUNNING;
    next->ticks_used = 0;
    g_current = next;
    g_need_resched = 0;
    g_ctx_switches++;

    /* Аппаратные/ручные контексты: TSS.RSP0 (прерывания из ring 3) и
     * стек syscall-входа. */
    tss_set_rsp0(next->kstack_top);
    syscall_set_kstack(next->kstack_top);

    /* Первая активация: подготовленный intr_frame. */
    if (next->fresh != 0) {
        g_entry_frame = next->entry_frame;
        next->fresh = 0;
    }

    /* CR3: kernel-задачи живут в пространстве прототипа. */
    vmm_space_load((next->cr3 != 0u) ? next->cr3 : vmm_kernel_cr3());

    sched_switch_asm(&prev->ctx, &next->ctx);

    /* Возврат сюда = ЭТА задача снова выбрана. Освобождаем zombie,
     * если предыдущий switch был её последним актом. */
    if (g_zombie_reap != NULL) {
        struct task *z = g_zombie_reap;
        g_zombie_reap = NULL;
        klog(KLOG_INFO, TASK_TAG,
             "task: reap pid=%u name=%s (стек и слот освобождены)",
             (unsigned int)z->pid, z->name);
        task_free(z);
    }
}

/* --- Хуки таймера/клавиатуры --- */

void sched_timer_tick(void)
{
    if (g_sched_ready == 0 || g_current == NULL) {
        return;
    }
    struct task *cur = g_current;
    if (cur->state == TASK_RUNNING) {
        cur->ticks_used++;
        cur->ticks_total++;
        /* Вытесняется только user-код: ядерная задача отдаёт квант
         * добровольно (sched_poll в главном цикле). */
        if (cur->is_user != 0 && cur->ticks_used >= TASK_QUANTUM_TICKS) {
            cur->ticks_used = 0;
            g_need_resched = 1;
        }
    }
}

/* Точка добровольной отдачи кванта из главного цикла ядра. */
void sched_poll(void)
{
    if (g_need_resched == 0) {
        return;
    }
    cli();
    sched_schedule();
    sti();
}

/* --- Блокировка/пробуждение --- */

void task_wq_init(struct task **wq)
{
    *wq = NULL;
}

void task_block_on(struct task **wq)
{
    cli();
    struct task *cur = g_current;
    if (cur == NULL) {
        sti();
        return;
    }
    cur->state = TASK_BLOCKED;
    cur->wq_next = *wq;
    *wq = cur;
    sched_schedule();   /* не вернётся, пока не разбудят */
    sti();
}

void task_wakeup(struct task **wq)
{
    struct task *t = *wq;
    *wq = NULL;
    int woke = 0;
    while (t != NULL) {
        struct task *next = t->wq_next;
        t->wq_next = NULL;
        if (t->state == TASK_BLOCKED) {
            t->state = TASK_READY;
            woke = 1;
        }
        t = next;
    }
    if (woke != 0) {
        g_need_resched = 1;
    }
}

struct task **task_kbd_wq(void)
{
    return &g_wq_kbd;
}

void task_yield(void)
{
    cli();
    if (g_current != NULL) {
        g_need_resched = 1;
        sched_schedule();
    }
    sti();
}

/* --- Инициализация --- */

err_t sched_init(void)
{
    memset(g_tasks, 0, sizeof(g_tasks));
    for (u32 i = 0; i < TASK_MAX; i++) {
        g_tasks[i].slot = (int)i;
        g_tasks[i].state = TASK_FREE;
    }

    /* Задача 0 = kmain (idle/kernel): её контекст сохранится при первом
     * switch; стек — текущий стек ядра (entry.S kstack_top). */
    extern char kstack_top[];
    struct task *idle = &g_tasks[0];
    idle->pid = 0;
    idle->state = TASK_RUNNING;
    idle->is_user = 0;
    idle->fresh = 0;
    idle->cr3 = 0;
    idle->kstack = 0;              /* не освобождаем стек ядра */
    idle->kstack_top = (u64)kstack_top;
    memcpy(idle->name, "idle", 5);
    g_current = idle;
    g_next_pid = 1;
    g_sched_ready = 1;

    tss_set_rsp0(idle->kstack_top);
    syscall_set_kstack(idle->kstack_top);

    /* Хук клавиатуры: wakeup читателей sys_read; хук таймера: кванты. */
    extern void kbd_set_wake_hook(void (*fn)(void));
    kbd_set_wake_hook(kbd_wake_hook);
    task_wq_init(&g_wq_kbd);

    extern void timer_set_tick_hook(void (*fn)(void));
    timer_set_tick_hook(sched_timer_tick);

    klog(KLOG_INFO, TASK_TAG,
         "планировщик: пул %u задач, квант %u тиков, idle=pid0 (kmain)",
         (unsigned int)TASK_MAX, (unsigned int)TASK_QUANTUM_TICKS);
    return K_OK;
}

/* --- Запуск user-процесса --- */

/* Коллбек ELF-загрузчика: мапит и копирует один сегмент. */
struct load_ctx {
    u64 pml4;
    u64 mapped_end;   /* старший мапнутый адрес (для отладки) */
};

static err_t load_segment_cb(void *ctx, const struct elf_load_seg *seg,
                             const u8 *img)
{
    struct load_ctx *lc = ctx;

    u64 vstart = seg->vaddr & ~(u64)(PAGE_SIZE - 1u);
    u64 vend = seg->vaddr + seg->memsz;
    u64 vend_up = (vend + PAGE_SIZE - 1u) & ~(u64)(PAGE_SIZE - 1u);

    int exec = (seg->flags & ELF_PF_X) != 0;

    for (u64 v = vstart; v < vend_up; v += PAGE_SIZE) {
        u64 frame = 0;
        err_t rc = phys_alloc_frame(&frame);
        if (rc != K_OK) {
            return rc;
        }
        memset((void *)(u64)frame, 0, PAGE_SIZE);   /* identity < 4 ГиБ */

        /* Сначала RW+NX: копирование содержимого файла. */
        rc = vmm_map_page(lc->pml4, v, frame,
                          VMM_PF_USER | VMM_PF_RW | VMM_PF_NX);
        if (rc != K_OK) {
            (void)phys_free_frame(frame);
            return rc;
        }

        /* Часть файла внутри этой страницы (через identity-физадрес). */
        u64 seg_off = v - seg->vaddr;   /* >= 0 для v >= vaddr */
        if (seg->vaddr > v) {
            /* Первый сегмент: файл начинается внутри страницы. */
            u64 in_page = seg->vaddr - v;
            u64 file_off = 0;
            u64 avail = PAGE_SIZE - in_page;
            u64 n = seg->filesz;
            if (n > avail) {
                n = avail;
            }
            if (n > 0u) {
                memcpy((void *)(frame + in_page), img + seg->offset + file_off,
                       (size_t)n);
            }
        } else {
            u64 file_off = seg_off;
            if (file_off < seg->filesz) {
                u64 n = seg->filesz - file_off;
                if (n > PAGE_SIZE) {
                    n = PAGE_SIZE;
                }
                memcpy((void *)frame, img + seg->offset + file_off,
                       (size_t)n);
            }
        }

        /* W^X: исполняемые страницы после записи — RX (снимаем RW+NX). */
        if (exec != 0) {
            rc = vmm_map_page(lc->pml4, v, frame, VMM_PF_USER);
            if (rc != K_OK) {
                return rc;
            }
        }
        lc->mapped_end = v + PAGE_SIZE;
    }
    return K_OK;
}

err_t task_spawn_user(const char *name, const u8 *elf_img, u32 img_len, u32 *out_pid)
{
    if (g_sched_ready == 0 || name == NULL || elf_img == NULL ||
        img_len == 0u || out_pid == NULL) {
        return K_EINVAL;
    }
    if (strlen(name) >= TASK_NAME_MAX) {
        return K_EINVAL;
    }

    /* Свободный слот + стек ядра. */
    struct task *t = NULL;
    for (u32 i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_FREE) {
            t = &g_tasks[i];
            break;
        }
    }
    if (t == NULL) {
        return K_ENOMEM;
    }

    cli();
    u64 kstack_phys = 0;
    err_t rc = phys_alloc_contig(TASK_KSTACK_FRAMES, &kstack_phys);
    if (rc != K_OK) {
        sti();
        return rc;
    }
    memset((void *)kstack_phys, 0, TASK_KSTACK_FRAMES * PAGE_SIZE);

    t->kstack = kstack_phys;
    t->kstack_top = kstack_phys + TASK_KSTACK_FRAMES * PAGE_SIZE;
    t->is_user = 1;

    /* Адресное пространство + загрузка ELF. */
    u64 pml4 = 0;
    rc = vmm_space_create(&pml4);
    if (rc != K_OK) {
        goto fail;
    }
    t->cr3 = pml4;

    u64 entry = 0;
    struct load_ctx lc = { .pml4 = pml4, .mapped_end = 0 };
    rc = elf64_load_image(elf_img, img_len, &entry, load_segment_cb, &lc);
    if (rc != K_OK) {
        goto fail;
    }

    /* Стек user: USER_STACK_PAGES страниц, растёт вниз от USER_STACK_TOP.
     * На стеке — argv[0] ("name"), argc=1 (SysV: rdi=argc, rsi=argv). */
    u64 stack_bottom = USER_STACK_TOP - (u64)USER_STACK_PAGES * PAGE_SIZE;
    for (u64 v = stack_bottom; v < USER_STACK_TOP; v += PAGE_SIZE) {
        u64 frame = 0;
        rc = phys_alloc_frame(&frame);
        if (rc != K_OK) {
            goto fail;
        }
        memset((void *)(u64)frame, 0, PAGE_SIZE);
        rc = vmm_map_page(pml4, v, frame,
                          VMM_PF_USER | VMM_PF_RW | VMM_PF_NX);
        if (rc != K_OK) {
            (void)phys_free_frame(frame);
            goto fail;
        }
    }

    /* argv-блок на вершине стека: [argc][argv0][NULL]["name\0"]. */
    u64 sp = USER_STACK_TOP;
    sp -= TASK_NAME_MAX;
    u64 argv0_str = sp;
    memcpy((void *)(u64)sp, name, strlen(name) + 1u);
    sp -= sizeof(u64);
    u64 argv_null = sp;
    *(u64 *)(u64)sp = 0;
    sp -= sizeof(u64);
    u64 argv0_ptr = sp;
    *(u64 *)(u64)sp = argv0_str;
    sp -= sizeof(u64);
    u64 argc_cell = sp;
    *(u64 *)(u64)sp = 1;
    sp -= sizeof(u64);   /* выравнивание rsp на 16 */

    (void)argv_null;
    (void)argc_cell;
    (void)argv0_ptr;

    build_entry_frame(t, entry, sp, 1, argv0_str);

    /* Heap-барьер и состояние. */
    t->heap_end = USER_HEAP_BASE;
    t->ticks_used = 0;
    t->ticks_total = 0;
    t->exit_code = 0;
    t->wq_next = NULL;
    memcpy(t->name, name, strlen(name) + 1u);
    t->pid = g_next_pid;
    g_next_pid++;
    t->state = TASK_READY;
    g_spawns++;

    *out_pid = t->pid;
    sti();
    klog(KLOG_INFO, TASK_TAG,
         "task: spawn pid=%u name=%s entry=0x%llx cr3=0x%llx img=%u Б",
         (unsigned int)t->pid, t->name, (unsigned long long)entry,
         (unsigned long long)pml4, (unsigned int)img_len);
    g_need_resched = 1;
    return K_OK;

fail:
    if (t->cr3 != 0u) {
        (void)vmm_space_destroy(t->cr3);
        t->cr3 = 0;
    }
    if (t->kstack != 0u) {
        for (u32 i = 0; i < TASK_KSTACK_FRAMES; i++) {
            (void)phys_free_frame(t->kstack + (u64)i * PAGE_SIZE);
        }
        t->kstack = 0;
        t->kstack_top = 0;
    }
    t->state = TASK_FREE;
    sti();
    return rc;
}

/* --- Завершение --- */

/* Полный путь выхода текущей задачи (sys_exit и kill по фолту).
 * Контекст: IF=0. Не возвращает управление. */
static void task_exit_impl(s64 code, const char *reason) __attribute__((noreturn));
static void task_exit_impl(s64 code, const char *reason)
{
    struct task *cur = g_current;

    klog(KLOG_INFO, TASK_TAG,
         "task: exit pid=%u name=%s code=%lld reason=%s",
         (unsigned int)cur->pid, cur->name, (long long)code,
         (reason != NULL) ? reason : "requested");

    g_exits++;

    /* Сначала уходим с адресного пространства процесса: CR3 -> kernel.
     * Дальнейшие обращения к user-памяти недопустимы. */
    vmm_space_load(vmm_kernel_cr3());
    if (cur->cr3 != 0u) {
        (void)vmm_space_destroy(cur->cr3);
        cur->cr3 = 0;
    }

    cur->exit_code = code;
    cur->state = TASK_ZOMBIE;
    g_zombie_reap = cur;
    g_need_resched = 0;

    sched_schedule();   /* уходим навсегда; reap — у следующей задачи */

    /* Недостижимо. */
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void task_kill_fault(const struct intr_frame *f)
{
    /* Мы внутри обработчика исключения (IF=0) на kernel-стеке задачи. */
    struct task *cur = g_current;
    klog(KLOG_ERROR, TASK_TAG,
         "task: fault kill pid=%u name=%s rip=0x%llx vec=%llu",
         (unsigned int)cur->pid, cur->name, (unsigned long long)f->rip,
         (unsigned long long)f->vector);
    task_exit_impl(-1, "fault");
}

void task_exit_current(s64 code)
{
    /* Мы в syscall-обработчике (IF=0) на kernel-стеке задачи. */
    task_exit_impl(code, "requested");
}

struct task *sys_current_task(void)
{
    return g_current;
}

/* --- Диагностика --- */

struct task_stats task_get_stats(void)
{
    struct task_stats st;
    memset(&st, 0, sizeof(st));
    for (u32 i = 0; i < TASK_MAX; i++) {
        switch (g_tasks[i].state) {
        case TASK_FREE:    break;
        case TASK_READY:   st.ready++; st.slots_used++; break;
        case TASK_RUNNING: st.slots_used++; break;
        case TASK_BLOCKED: st.blocked++; st.slots_used++; break;
        case TASK_ZOMBIE:  st.zombies++; st.slots_used++; break;
        }
    }
    st.ctx_switches = g_ctx_switches;
    st.spawns = g_spawns;
    st.exits = g_exits;
    return st;
}

void task_iterate(task_iter_cb cb)
{
    if (cb == NULL) {
        return;
    }
    for (u32 i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_FREE) {
            continue;
        }
        cb(g_tasks[i].pid, g_tasks[i].name, state_name(g_tasks[i].state),
           g_tasks[i].is_user, g_tasks[i].ticks_total);
    }
}

/* --- Самопроверка (вызывается из kselftest, до старта user-задач) --- */

int task_selftest(void)
{
    int fails = 0;

    /* Пул: инварианты имеют смысл только после sched_init (kselftest
     * вызывается ДО него в kmain — до инициализации проверяется лишь
     * раскладка). */
    if (g_sched_ready != 0) {
        if (g_tasks[0].state != TASK_RUNNING || g_tasks[0].pid != 0u) {
            fails++;
        }
        for (u32 i = 1u; i < TASK_MAX; i++) {
            if (g_tasks[i].state != TASK_FREE) {
                fails++;
            }
        }
        if (g_current != &g_tasks[0]) {
            fails++;
        }
    }

    /* RFLAGS/чтение состояния: бит IF корректно отражается. */
    if ((read_rflags() & USER_RFLAGS & 0x200ull) == 0u) {
        fails++;   /* kselftest вызывается с включёнными прерываниями */
    }

    /* USER-раскладка: сегменты не пересекаются. */
    if (!(USER_ELF_BASE < USER_HEAP_BASE && USER_HEAP_BASE < USER_HEAP_LIMIT &&
          USER_HEAP_LIMIT < USER_STACK_TOP)) {
        fails++;
    }
    if (USER_STACK_TOP + PAGE_SIZE >= 0x0000800000000000ull) {
        fails++;
    }

    return fails;
}
