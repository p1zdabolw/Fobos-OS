/*
 * Fobos OS — кадр прерывания x86_64.
 *
 * Раскладка обязана совпадать с idt_stub.S байт-в-байт:
 *   [CPU: SS, RSP, RFLAGS, CS, RIP] (+ [errcode] у части векторов)
 *   [stub: vector (+ errcode=0 для векторов без кода ошибки)]
 *   [stub: r15..r8, rbp, rdi, rsi, rdx, rcx, rbx, rax]  <- RDI указывает сюда
 */
#ifndef FOBOS_ARCH_INTR_H
#define FOBOS_ARCH_INTR_H

#include <fobos/types.h>

struct intr_frame {
    u64 r15;
    u64 r14;
    u64 r13;
    u64 r12;
    u64 r11;
    u64 r10;
    u64 r9;
    u64 r8;
    u64 rbp;
    u64 rdi;
    u64 rsi;
    u64 rdx;
    u64 rcx;
    u64 rbx;
    u64 rax;
    u64 vector;
    u64 errcode;
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};

#endif /* FOBOS_ARCH_INTR_H */
