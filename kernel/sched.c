#include "sched.h"
#include "heap.h"
#include "pmm.h"
#include "../lib/mem.h"
#include "../lib/string.h"
#include "../lib/printf.h"

#define MAX_TASKS 8
#define STACK_SIZE 16384

static struct task g_tasks[MAX_TASKS];
static int g_task_count;
static int g_current;

void sched_init(void) {
    memset(g_tasks, 0, sizeof(g_tasks));
    g_task_count = 1;
    g_current = 0;
    g_tasks[0].pid = 0;
    g_tasks[0].state = 1;
    strcpy(g_tasks[0].name, "kernel");
}

int sched_create(void (*entry)(void), const char *name) {
    if (g_task_count >= MAX_TASKS) return -1;
    int id = g_task_count++;
    g_tasks[id].pid = (u64)id;
    u64 stack = (u64)kmalloc(STACK_SIZE);
    if (!stack) return -1;
    g_tasks[id].rsp = stack + STACK_SIZE - 8;
    g_tasks[id].rip = (u64)entry;
    g_tasks[id].state = 1;
    strncpy(g_tasks[id].name, name, 31);
    return id;
}

void sched_yield(void) {
    g_current = (g_current + 1) % g_task_count;
}

u64 sched_current(void) { return (u64)g_current; }