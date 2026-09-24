#ifndef FOS_SCHED_H
#define FOS_SCHED_H

#include "types.h"
#include "idt.h"

#define SCHED_MAX_TASKS          16
#define SCHED_STACK_SIZE         16384
#define SCHED_PRIO_MAX           31
#define SCHED_PRIO_MIN           0
#define SCHED_DEFAULT_QUANTUM_MS 20
#define SCHED_IDLE_PRIORITY      SCHED_PRIO_MAX

enum {
    TASK_FREE = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_DEAD
};

struct tcb {
    u64  id;
    char name[24];
    u64  rsp;
    u64  stack_base;
    u64  stack_size;
    int  state;
    int  priority;
    u32  quantum_ms;
    u32  remaining_ms;
    u64  sleep_until_ms;
    u64  wake_count;
    u64  switch_count;
    u32  period_ms;
    u64  next_wake_ms;
    u64  periods_run;
    u64  periods_missed;
    u32  max_jitter_ms;
    u64  total_jitter_ms;
    u64  jitter_samples;
    void (*entry)(void);
};

void sched_init(void);
int  sched_create(const char *name, void (*entry)(void), int priority);
int  sched_create_periodic(const char *name, void (*entry)(void), int priority, u32 period_ms);
void sched_start(void);

void sched_yield(void);
void sched_sleep(u32 ms);
void sched_wait_period(void);
void sched_wake(u64 id);
void sched_exit(void);

u64  sched_ticks_ms(void);
u64  sched_current_id(void);
int  sched_current_priority(void);
int  sched_task_count(void);
struct tcb *sched_task(u64 id);

void sched_timer_tick(struct registers *r);
void sched_set_priority(u64 id, int priority);

#endif