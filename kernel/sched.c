#include "sched.h"
#include "heap.h"
#include "idt.h"
#include "../lib/mem.h"
#include "../lib/string.h"
#include "../lib/printf.h"

volatile u64 sched_switch_rsp = 0;

extern void sched_do_yield(void);
extern void sched_start_first(u64 rsp);
extern u64  timer_ticks(void);

static u8 g_stack_pool[SCHED_MAX_TASKS * SCHED_STACK_SIZE] __attribute__((aligned(16)));

static struct tcb g_tasks[SCHED_MAX_TASKS];
static int g_task_count;
static int g_current;

static void task_entry_stub(void);

static struct tcb *pick_next(void) {
    if (g_task_count == 0) return (struct tcb*)0;
    int start = (g_current + 1) % g_task_count;
    int best_prio = 999;
    struct tcb *best = (struct tcb*)0;
    for (int k = 0; k < g_task_count; k++) {
        int i = (start + k) % g_task_count;
        struct tcb *t = &g_tasks[i];
        if (t->state != TASK_READY) continue;
        if (t->priority < best_prio) {
            best_prio = t->priority;
            best = t;
        }
    }
    return best;
}

static struct tcb *pick_higher_than(int prio) {
    struct tcb *best = (struct tcb*)0;
    int best_prio = prio;
    for (int i = 0; i < g_task_count; i++) {
        struct tcb *t = &g_tasks[i];
        if (t->state != TASK_READY) continue;
        if (t->priority < best_prio) {
            best_prio = t->priority;
            best = t;
        }
    }
    return best;
}

static void build_initial_frame(struct tcb *t) {
    u64 top = (t->stack_base + t->stack_size) & ~15ULL;
    u64 frame = top - 176;
    u64 *f = (u64*)frame;
    for (int i = 0; i < 22; i++) f[i] = 0;
    f[15] = 128;
    f[16] = 0;
    f[17] = (u64)task_entry_stub;
    f[18] = 0x08;
    f[19] = 0x202;
    f[20] = frame - 8;
    f[21] = 0x10;
    t->rsp = frame;
}

void sched_init(void) {
    memset(g_tasks, 0, sizeof(g_tasks));
    memset(g_stack_pool, 0, sizeof(g_stack_pool));
    g_task_count = 0;
    g_current = -1;
    sched_switch_rsp = 0;
}

static int sched_create_common(const char *name, void (*entry)(void), int priority, u32 period_ms) {
    if (g_task_count >= SCHED_MAX_TASKS) return -1;
    if (priority < SCHED_PRIO_MIN) priority = SCHED_PRIO_MIN;
    if (priority > SCHED_PRIO_MAX) priority = SCHED_PRIO_MAX;

    int id = g_task_count++;
    struct tcb *t = &g_tasks[id];
    memset(t, 0, sizeof(*t));
    t->id = (u64)id;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = 0;
    t->stack_size = SCHED_STACK_SIZE;
    t->stack_base = (u64)(g_stack_pool + (usize)id * SCHED_STACK_SIZE);
    t->priority = priority;
    t->quantum_ms = SCHED_DEFAULT_QUANTUM_MS;
    t->remaining_ms = t->quantum_ms;
    t->state = TASK_READY;
    t->entry = entry;
    t->period_ms = period_ms;
    build_initial_frame(t);
    return id;
}

int sched_create(const char *name, void (*entry)(void), int priority) {
    return sched_create_common(name, entry, priority, 0);
}

int sched_create_periodic(const char *name, void (*entry)(void), int priority, u32 period_ms) {
    if (period_ms == 0) return -1;
    return sched_create_common(name, entry, priority, period_ms);
}

static void task_entry_stub(void) {
    struct tcb *t = &g_tasks[g_current];
    if (t->entry) t->entry();
    t->state = TASK_DEAD;
    sched_do_yield();
    for (;;) __asm__ volatile("hlt");
}

void sched_yield_impl(struct registers *r) {
    struct tcb *cur = (g_current >= 0) ? &g_tasks[g_current] : (struct tcb*)0;
    if (!cur) return;

    cur->rsp = (u64)r;

    if (cur->state == TASK_RUNNING) {
        cur->state = TASK_READY;
        cur->remaining_ms = cur->quantum_ms;
    }
    cur->switch_count++;

    struct tcb *next = pick_next();
    if (!next || next == cur) {
        cur->state = TASK_RUNNING;
        return;
    }

    next->state = TASK_RUNNING;
    sched_switch_rsp = next->rsp;
    g_current = (int)next->id;
}

void sched_yield(void) {
    sched_do_yield();
}

void sched_sleep(u32 ms) {
    if (g_current < 0) return;
    struct tcb *cur = &g_tasks[g_current];
    cur->sleep_until_ms = sched_ticks_ms() + ms;
    cur->state = TASK_SLEEPING;
    sched_do_yield();
    cur->state = TASK_RUNNING;
}

void sched_wait_period(void) {
    if (g_current < 0) return;
    struct tcb *cur = &g_tasks[g_current];
    if (cur->period_ms == 0) { sched_sleep(1); return; }

    u64 now = sched_ticks_ms();
    if (cur->next_wake_ms == 0) cur->next_wake_ms = now + cur->period_ms;
    else                        cur->next_wake_ms += cur->period_ms;

    u64 deadline = cur->next_wake_ms;
    if (deadline <= now) {
        deadline = now + cur->period_ms;
        cur->next_wake_ms = deadline;
    }

    cur->sleep_until_ms = deadline;
    cur->state = TASK_SLEEPING;
    sched_do_yield();
    cur->state = TASK_RUNNING;

    u64 woke = sched_ticks_ms();
    cur->periods_run++;
    if (woke > deadline) {
        u32 jitter = (u32)(woke - deadline);
        if (jitter > cur->max_jitter_ms) cur->max_jitter_ms = jitter;
        cur->total_jitter_ms += jitter;
        cur->jitter_samples++;
        if (jitter > cur->period_ms / 2) cur->periods_missed++;
        if (jitter >= cur->period_ms) cur->next_wake_ms = woke + cur->period_ms;
    } else {
        cur->jitter_samples++;
    }
}

void sched_wake(u64 id) {
    if (id >= (u64)g_task_count) return;
    struct tcb *t = &g_tasks[id];
    if (t->state == TASK_SLEEPING) {
        t->state = TASK_READY;
        t->wake_count++;
    }
}

void sched_exit(void) {
    if (g_current < 0) return;
    struct tcb *cur = &g_tasks[g_current];
    cur->state = TASK_DEAD;
    sched_do_yield();
}

u64 sched_ticks_ms(void) {
    return timer_ticks();
}

u64 sched_current_id(void) {
    return (g_current >= 0) ? (u64)g_current : (u64)-1;
}

int sched_current_priority(void) {
    return (g_current >= 0) ? g_tasks[g_current].priority : -1;
}

int sched_task_count(void) {
    return g_task_count;
}

struct tcb *sched_task(u64 id) {
    if (id >= (u64)g_task_count) return (struct tcb*)0;
    return &g_tasks[id];
}

void sched_set_priority(u64 id, int priority) {
    if (id >= (u64)g_task_count) return;
    if (priority < SCHED_PRIO_MIN) priority = SCHED_PRIO_MIN;
    if (priority > SCHED_PRIO_MAX) priority = SCHED_PRIO_MAX;
    g_tasks[id].priority = priority;
}

void sched_timer_tick(struct registers *r) {
    u64 now = sched_ticks_ms();

    for (int i = 0; i < g_task_count; i++) {
        struct tcb *t = &g_tasks[i];
        if (t->state == TASK_SLEEPING && t->sleep_until_ms <= now) {
            t->state = TASK_READY;
            t->wake_count++;
        }
    }

    struct tcb *cur = (g_current >= 0) ? &g_tasks[g_current] : (struct tcb*)0;
    if (!cur || cur->state != TASK_RUNNING) return;

    if (cur->remaining_ms > 0) cur->remaining_ms--;

    if (cur->remaining_ms > 0) {
        struct tcb *high = pick_higher_than(cur->priority);
        if (!high) return;
        cur->state = TASK_READY;
        cur->rsp = (u64)r;
        cur->switch_count++;
        high->state = TASK_RUNNING;
        sched_switch_rsp = high->rsp;
        g_current = (int)high->id;
        return;
    }

    cur->remaining_ms = cur->quantum_ms;
    cur->state = TASK_READY;
    struct tcb *next = pick_next();
    if (!next || next == cur) {
        cur->state = TASK_RUNNING;
        return;
    }
    cur->rsp = (u64)r;
    cur->switch_count++;
    next->state = TASK_RUNNING;
    sched_switch_rsp = next->rsp;
    g_current = (int)next->id;
}

void sched_start(void) {
    __asm__ volatile("cli");
    struct tcb *first = pick_next();
    if (!first) {
        __asm__ volatile("sti");
        for (;;) __asm__ volatile("hlt");
    }
    g_current = (int)first->id;
    first->state = TASK_RUNNING;
    sched_start_first(first->rsp);
}