/*
 * Fobos OS — задачи и планировщик (FND-3a, прототипы).
 *
 * Модель: вытесняющий RR с квантом для user-задач; ядерный код (задача
 * kmain/idle) не вытесняется по построению — синхронизация не требуется
 * (одно CPU, прерывания выключены в критических местах ядра).
 */
#ifndef FOBOS_SYS_TASK_H
#define FOBOS_SYS_TASK_H

#include <fobos/errors.h>
#include <fobos/types.h>
#include "../arch/intr.h"

#define TASK_NAME_MAX 16u
#define TASK_MAX      16u          /* пул задач (без динамической памяти) */
#define TASK_KSTACK_FRAMES 16u     /* стек ядра задачи: 64 КиБ */
#define TASK_QUANTUM_TICKS 10u     /* 10 мс при 1000 Гц */

/* Раскладка user-адресного пространства (общая с sys/task.c, sys/syscall.c
 * и linker-скриптом user/user.ld). */
#define USER_ELF_BASE   0x0000000000400000ull
#define USER_HEAP_BASE  0x0000000000600000ull
#define USER_HEAP_LIMIT 0x0000000070000000ull
#define USER_STACK_TOP  0x000000007fffe000ull
#define USER_STACK_PAGES 16u       /* 64 КиБ */
#define USER_VADDR_LIMIT 0x0000800000000000ull  /* верх canonical user */

/* Состояния задачи. */
enum task_state {
    TASK_FREE = 0,   /* слот пула свободен */
    TASK_READY,      /* в очереди планировщика */
    TASK_RUNNING,    /* сейчас исполняется */
    TASK_BLOCKED,    /* ждёт wait-очереди */
    TASK_ZOMBIE      /* завершена, ресурсы освобождает reap */
};

/* Контекст продолжения (callee-saved + точка возврата). Сохраняется
 * sched_switch_asm; GPR и CPU-часть живут в intr_frame/syscall_frame
 * на kernel-стеке задачи. */
struct task_ctx {
    u64 rsp;        /* вершина switch-кадра ([rbx][rbp][r12..r15]) */
    u64 ret;        /* адрес продолжения (ret из switch) */
};

struct task {
    u32 pid;
    int slot;
    enum task_state state;
    int is_user;                 /* 1 = ring 3 */
    int fresh;                   /* 1 = ни разу не запускалась (entry frame) */
    char name[TASK_NAME_MAX];
    u64 kstack;                  /* физический адрес начала (frames*TASK_KSTACK_FRAMES) */
    u64 kstack_top;              /* виртуальный = физический (identity) */
    u64 entry_frame;             /* адрес intr_frame первого входа (fresh) */
    u64 cr3;                     /* 0 = kernel space */
    u64 heap_end;                /* барьер sys_brk (user-виртуальный) */
    struct task_ctx ctx;
    u64 ticks_used;              /* тиков внутри текущего кванта */
    u64 ticks_total;             /* всего тиков исполнения (ps) */
    s64 exit_code;               /* для ZOMBIE (лог/отладка) */
    struct task *wq_next;        /* цепочка в wait-очереди */
};

/* Инициализация: idle-задача = текущий контекст kmain. Вызывается один раз
 * после vmm_init и до включения таймерных вытеснений (по sti всё уже бежит). */
err_t sched_init(void);

/* Хук тика таймера: учёт кванта текущей задачи, установка need_resched. */
void sched_timer_tick(void);

/* Точка переключения: выбирает следующую READY-задачу и переключается.
 * Вызывается: (а) из intr_c_handler после IRQ при need_resched (preempt
 * user), (б) из task_block/task_exit/task_yield. Возврат из неё происходит
 * в контексте ЭТОЙ задачи, когда её снова выберут. */
void sched_schedule(void);

/* Добровольная точка вытеснения из главного цикла ядра: если запрошено
 * вытеснение (тик/пробуждение) — cli + sched_schedule + sti. */
void sched_poll(void);

/* 1, если запрошено вытеснение (тик исчерпал квант / wakeup). */
int sched_need_resched(void);

/* Инициализация wait-очереди. */
void task_wq_init(struct task **wq);

/* Блокировка текущей задачи на wait-очереди (до task_wakeup).
 * Контекст: kernel (IF уже 0 или будет выключен). */
void task_block_on(struct task **wq);

/* Очередь ожидания клавиатуры (sys_read). Единственная очередь v0;
 * доступна syscall-слою через этот аксессор. */
struct task **task_kbd_wq(void);

/* Добровольная отдача кванта (из syscall-обработчика, например sleep). */
void task_yield(void);

/* Завершение текущей задачи с кодом (sys_exit). Не возвращает управление. */
void task_exit_current(s64 code) __attribute__((noreturn));

/* Пробуждение всех задач очереди (state -> READY, need_resched).
 * Контекст: любой (IRQ-безопасен: не спит, не аллоцирует). */
void task_wakeup(struct task **wq);

/* Текущая задача (NULL до sched_init). */
struct task *task_current(void);

/* Запуск user-процесса из ELF64-образа в памяти. Не блокирует.
 * Возвращает pid. При ошибке загрузки всё выделенное освобождается. */
err_t task_spawn_user(const char *name, const u8 *elf_img, u32 img_len, u32 *out_pid);

/* Завершение текущей user-задачи по исключению ring 3 (idt.c).
 * Не возвращает управление. */
void task_kill_fault(const struct intr_frame *f) __attribute__((noreturn));

/* Диагностика для kshell `ps` / selftest. */
struct task_stats {
    u32 slots_used;
    u32 ready;
    u32 blocked;
    u32 zombies;
    u64 ctx_switches;
    u64 spawns;
    u64 exits;
};
struct task_stats task_get_stats(void);

/* Итератор по живым задачам (для ps). cb вызывается для каждой. */
typedef void (*task_iter_cb)(u32 pid, const char *name, const char *state,
                             int is_user, u64 ticks_total);
void task_iterate(task_iter_cb cb);

/* Самопроверка планировщика (счётчики/пул): число упавших проверок. */
int task_selftest(void);

#endif /* FOBOS_SYS_TASK_H */
