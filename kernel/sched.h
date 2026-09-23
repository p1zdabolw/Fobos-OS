#ifndef FOS_SCHED_H
#define FOS_SCHED_H

#include "types.h"

struct task {
    u64 pid;
    u64 rsp;
    u64 rip;
    u64 cr3;
    int state;
    char name[32];
};

void sched_init(void);
int  sched_create(void (*entry)(void), const char *name);
void sched_yield(void);
u64  sched_current(void);

#endif