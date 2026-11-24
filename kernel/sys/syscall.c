/*
 * Fobos OS — syscall-диспетчер x86_64 (FND-3a).
 *
 * Механика: SYSCALL (EFER.SCE) -> syscall_entry_asm (sys/switch.S)
 * -> syscall_c_handler -> таблица -> sysretq. FMASK очищает TF/IF/DF/NT/AC
 * на входе; RCX=user RIP, R11=user RFLAGS сохраняет asm-вход.
 *
 * Контекст обработчика: IF=0 (FMASK), kernel-стек текущей задачи,
 * CR3 — пространство процесса. Прямой доступ к user-страницам — только
 * через copy_to/from_user (SMAP: STAC/CLAC; изоляция: проверка диапазона).
 */
#include "syscall.h"
#include "task.h"
#include "elf.h"
#include "../arch/tss.h"
#include "../mm/vmm.h"
#include "../mm/phys.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../lib/io.h"
#include "../drivers/timer.h"
#include "../drivers/kbd.h"
#include "../gfx/console.h"

#include <fobos/syscall.h>

#define MSR_STAR   0xC0000081u
#define MSR_LSTAR  0xC0000082u
#define MSR_FMASK  0xC0000084u

/* STAR: [47:32] SYSCALL -> CS/SS (KCODE/KDATA), [63:48] SYSRET -> CS/SS
 * (UCODE; CPU сам ставит RPL=3 => CS=0x1B, SS=0x23). */
#define STAR_VALUE (((u64)0x18u << 48) | ((u64)0x08u << 32))

/* FMASK: очищаемые биты RFLAGS на входе: TF(8) IF(9) DF(10) NT(14) AC(18). */
#define FMASK_VALUE ((1ull << 8) | (1ull << 9) | (1ull << 10) | \
                     (1ull << 14) | (1ull << 18))

#define SYSCALL_MAX_LEN 4096u
#define COPY_CHUNK      512u

static u64 g_syscall_count;
static const char *const SC_TAG = "sys/syscall";
extern void syscall_entry_asm(void);

static u64 rdmsr(u32 msr)
{
    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static void wrmsr(u32 msr, u64 val)
{
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((u32)val), "d"((u32)(val >> 32)));
}

void syscall_set_kstack(u64 kstack_top)
{
    extern u64 g_syscall_kstack;   /* switch.S */
    g_syscall_kstack = kstack_top;
}

u64 syscall_count(void)
{
    return g_syscall_count;
}

/* --- копирование user<->kernel --- */

static inline void stac(void)
{
    if (cpu_smap_enabled() != 0) {
        __asm__ volatile ("stac" : : : "memory");
    }
}

static inline void clac(void)
{
    if (cpu_smap_enabled() != 0) {
        __asm__ volatile ("clac" : : : "memory");
    }
}

err_t copy_from_user(u64 uaddr, u64 len, void *kbuf)
{
    if (len == 0u) {
        return K_OK;
    }
    if (kbuf == NULL) {
        return K_EINVAL;
    }
    err_t rc = vmm_user_range_ok(task_current()->cr3, uaddr, len);
    if (rc != K_OK) {
        return rc;
    }
    u8 *dst = kbuf;
    u64 done = 0;
    stac();
    while (done < len) {
        u64 n = len - done;
        if (n > COPY_CHUNK) {
            n = COPY_CHUNK;
        }
        memcpy(dst + done, (const void *)(uaddr + done), (size_t)n);
        done += n;
    }
    clac();
    return K_OK;
}

err_t copy_to_user(u64 uaddr, u64 len, const void *kbuf)
{
    if (len == 0u) {
        return K_OK;
    }
    if (kbuf == NULL) {
        return K_EINVAL;
    }
    err_t rc = vmm_user_range_ok(task_current()->cr3, uaddr, len);
    if (rc != K_OK) {
        return rc;
    }
    const u8 *src = kbuf;
    u64 done = 0;
    stac();
    while (done < len) {
        u64 n = len - done;
        if (n > COPY_CHUNK) {
            n = COPY_CHUNK;
        }
        memcpy((void *)(uaddr + done), src + done, (size_t)n);
        done += n;
    }
    clac();
    return K_OK;
}

/* --- обработчики (аргументы из кадра) --- */

/* SYS_exit(0) — не возвращаем управление. */
static s64 sys_exit(struct syscall_frame *f)
{
    (void)f;
    task_exit_current((s64)f->rdi);
    /* недостижимо */
    return 0;
}

/* SYS_write(fd=1, buf, len): текст в текущую консоль + зеркалирование
 * в serial (klog) для CI. Двоичные данные не гарантированы (консоль
 * печатает строки; NUL обрезает вывод — задокументированное ограничение). */
static s64 sys_write(struct syscall_frame *f)
{
    u64 fd = f->rdi;
    u64 ubuf = f->rsi;
    u64 len = f->rdx;

    if (fd != FOS_FD_CONSOLE) {
        return -K_EINVAL;
    }
    if (len == 0u) {
        return 0;
    }
    if (len > SYSCALL_MAX_LEN) {
        return -K_ERANGE;
    }

    char kbuf[COPY_CHUNK];
    u64 done = 0;
    while (done < len) {
        u64 n = len - done;
        if (n > COPY_CHUNK - 1u) {
            n = COPY_CHUNK - 1u;
        }
        err_t rc = copy_from_user(ubuf + done, n, kbuf);
        if (rc != K_OK) {
            return -(s64)rc;
        }
        kbuf[n] = '\0';
        con_write(kbuf);
        klog(KLOG_INFO, "user", "%s", kbuf);
        done += n;
    }
    return (s64)len;
}

/* SYS_read(fd=0, buf, len): события клавиатуры по 4 байта
 * (struct fos_keyevent). Блокирующий: ждёт хотя бы одного события.
 * len обязан быть кратен 4. Возвращает число прочитанных байт. */
static s64 sys_read(struct syscall_frame *f)
{
    u64 fd = f->rdi;
    u64 ubuf = f->rsi;
    u64 len = f->rdx;

    if (fd != FOS_FD_KBD) {
        return -K_EINVAL;
    }
    if (len == 0u) {
        return 0;
    }
    if ((len & 3u) != 0u) {
        return -K_EINVAL;
    }
    if (len > SYSCALL_MAX_LEN) {
        return -K_ERANGE;
    }

    u64 count = len / 4u;
    u64 got = 0;

    while (got < count) {
        struct kbd_event ev;
        if (kbd_poll(&ev) != K_OK) {
            if (got > 0u) {
                break;   /* прочитали хоть что-то — возвращаем */
            }
            /* Блокировка до появления события (wakeup из IRQ клавиатуры). */
            task_block_on(task_kbd_wq());
            continue;
        }
        struct fos_keyevent fev;
        fev.code = ev.code;
        fev.flags = ev.flags;
        fev.ch = ev.ch;
        fev.pad = 0;
        err_t rc = copy_to_user(ubuf + got * 4u, sizeof(fev), &fev);
        if (rc != K_OK) {
            return -(s64)rc;
        }
        got++;
    }
    return (s64)(got * 4u);
}

static s64 sys_getpid(struct syscall_frame *f)
{
    (void)f;
    struct task *cur = task_current();
    return (cur != NULL) ? (s64)cur->pid : -K_EPERM;
}

static s64 sys_uptime(struct syscall_frame *f)
{
    (void)f;
    return (s64)timer_uptime_ms();
}

static s64 sys_sleep(struct syscall_frame *f)
{
    u64 ms = f->rdi;
    if (ms > 60000u) {
        return -K_EINVAL;
    }
    /* Активное ожидание ядра запрещено в user-задаче: отдаём кванты.
     * v0 — опрос таймера с yield; таймерные блокировки (wait-очередь на
     * IRQ0) — FND-3b. */
    u64 deadline = timer_get_ticks() + ms;
    while (timer_get_ticks() < deadline) {
        task_yield();
    }
    return 0;
}

/* SYS_brk(addr): барьер heap. addr=0 -> текущий барьер. */
static s64 sys_brk(struct syscall_frame *f)
{
    u64 addr = f->rdi;
    struct task *cur = task_current();
    u64 cur_brk = cur->heap_end;

    if (addr == 0u) {
        return (s64)cur_brk;
    }
    if (addr < USER_HEAP_BASE || addr >= USER_HEAP_LIMIT) {
        return -K_EINVAL;
    }
    if (addr == cur_brk) {
        return (s64)cur_brk;
    }

    u64 new_brk = (addr + PAGE_SIZE - 1u) & ~(u64)(PAGE_SIZE - 1u);
    u64 old_brk = (cur_brk + PAGE_SIZE - 1u) & ~(u64)(PAGE_SIZE - 1u);

    if (new_brk > old_brk) {
        for (u64 v = old_brk; v < new_brk; v += PAGE_SIZE) {
            u64 frame = 0;
            err_t rc = phys_alloc_frame(&frame);
            if (rc != K_OK) {
                /* Откат уже выделенных страниц этой операции. */
                for (u64 r = old_brk; r < v; r += PAGE_SIZE) {
                    (void)vmm_unmap_page(cur->cr3, r);
                }
                return -K_ENOMEM;
            }
            memset((void *)(u64)frame, 0, PAGE_SIZE);
            rc = vmm_map_page(cur->cr3, v, frame,
                              VMM_PF_USER | VMM_PF_RW | VMM_PF_NX);
            if (rc != K_OK) {
                (void)phys_free_frame(frame);
                for (u64 r = old_brk; r < v; r += PAGE_SIZE) {
                    (void)vmm_unmap_page(cur->cr3, r);
                }
                return -(s64)rc;
            }
        }
    } else if (new_brk < old_brk) {
        for (u64 v = new_brk; v < old_brk; v += PAGE_SIZE) {
            (void)vmm_unmap_page(cur->cr3, v);
        }
    }

    cur->heap_end = addr;
    return (s64)addr;
}

static s64 sys_abi_version(struct syscall_frame *f)
{
    (void)f;
    return (s64)FOBOS_SYSCALL_ABI;
}

typedef s64 (*sys_fn)(struct syscall_frame *f);

struct sysent {
    const char *name;
    sys_fn fn;
};

static const struct sysent SYSTAB[] = {
    [FOS_SYS_exit]        = { "exit",        sys_exit },
    [FOS_SYS_write]       = { "write",       sys_write },
    [FOS_SYS_read]        = { "read",        sys_read },
    [FOS_SYS_getpid]      = { "getpid",      sys_getpid },
    [FOS_SYS_uptime]      = { "uptime",      sys_uptime },
    [FOS_SYS_sleep]       = { "sleep",       sys_sleep },
    [FOS_SYS_brk]         = { "brk",         sys_brk },
    [FOS_SYS_abi_version] = { "abi_version", sys_abi_version },
};

#define SYSTAB_N (sizeof(SYSTAB) / sizeof(SYSTAB[0]))

s64 syscall_c_handler(struct syscall_frame *f)
{
    g_syscall_count++;

    struct task *cur = task_current();
    if (cur == NULL || cur->is_user == 0) {
        /* SYSCALL из ядра — ошибка программирования: FMASK выключил IF,
         * мы на kernel-стеке. Честный отказ вместо молчания. */
        klog(KLOG_ERROR, SC_TAG, "SYSCALL вне user-задачи (rax=%llu)",
             (unsigned long long)f->rax);
        return -K_EPERM;
    }

    u64 no = f->rax;
    if (no >= SYSTAB_N || SYSTAB[no].fn == NULL) {
        klog(KLOG_WARN, SC_TAG, "pid=%u: неизвестный syscall %llu",
             (unsigned int)cur->pid, (unsigned long long)no);
        return -K_ENOSYS;
    }

    return SYSTAB[no].fn(f);
}

err_t syscall_init(void)
{
    if (task_current() == NULL) {
        return K_ESTATE;   /* sched_init обязан быть выполнен */
    }

    wrmsr(MSR_STAR, STAR_VALUE);
    wrmsr(MSR_LSTAR, (u64)(u64 *)&syscall_entry_asm);
    wrmsr(MSR_FMASK, FMASK_VALUE);

    /* EFER.SCE (bit 0). NXE уже включён tss_init. */
    u64 efer = rdmsr(0xC0000080u);
    efer |= 1ull;
    wrmsr(0xC0000080u, efer);

    /* Контрольная перечитка MSR (FND-3a): значение STAR обязано дать
     * SYSCALL: CS=0x08/SS=0x10, SYSRET: CS=0x1B/SS=0x23. */
    klog(KLOG_DEBUG, SC_TAG, "STAR=0x%llx LSTAR=0x%llx FMASK=0x%llx",
         (unsigned long long)rdmsr(MSR_STAR),
         (unsigned long long)rdmsr(MSR_LSTAR),
         (unsigned long long)rdmsr(MSR_FMASK));

    klog(KLOG_INFO, SC_TAG,
         "syscall ABI v%u: LSTAR=0x%llx, FMASK=0x%x, записей в таблице: %u",
         (unsigned int)FOBOS_SYSCALL_ABI,
         (unsigned long long)(u64)(u64 *)&syscall_entry_asm,
         (unsigned int)FMASK_VALUE, (unsigned int)SYSTAB_N);
    return K_OK;
}
