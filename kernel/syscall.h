#ifndef FOS_SYSCALL_H
#define FOS_SYSCALL_H

#include "types.h"
#include "idt.h"

#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_READ   2
#define SYS_GETPID 3
#define SYS_YIELD  4

void syscall_init(void);
void syscall_dispatch(struct registers *r);

#endif