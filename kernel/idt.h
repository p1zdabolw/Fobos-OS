#ifndef FOS_IDT_H
#define FOS_IDT_H

#include "types.h"

struct registers {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector, error;
    u64 rip, cs, rflags, rsp, ss;
};

typedef void (*irq_handler_t)(struct registers *);

void idt_init(void);
void irq_register(int irq, irq_handler_t h);
void pic_remap(void);

#endif