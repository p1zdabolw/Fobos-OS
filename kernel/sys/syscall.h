/*
 * Fobos OS — syscall-слой ядра (FND-3a, прототипы).
 */
#ifndef FOBOS_SYS_SYSCALL_H
#define FOBOS_SYS_SYSCALL_H

#include <fobos/errors.h>
#include <fobos/types.h>

/* Кадр SYSCALL — раскладка обязана совпадать с sys/switch.S
 * (syscall_entry_asm) байт-в-байт. */
struct syscall_frame {
    u64 rax;         /* номер syscall */
    u64 rdi;         /* arg0 */
    u64 rsi;         /* arg1 */
    u64 rdx;         /* arg2 */
    u64 r10;         /* arg3 */
    u64 r8;          /* arg4 */
    u64 r9;          /* arg5 */
    u64 user_rip;
    u64 user_rflags;
    u64 user_rsp;
};

/* Включение syscall/sysret (MSR STAR/LSTAR/FMASK, EFER.SCE).
 * Требует: tss_init выполнен (kstack известен), sched_init выполнен. */
err_t syscall_init(void);

/* Обновляет kernel-стек syscall-входа (вызывается планировщиком при
 * каждом переключении задач). */
void syscall_set_kstack(u64 kstack_top);

/* C-диспетчер из switch.S: возвращает значение RAX для sysret. */
s64 syscall_c_handler(struct syscall_frame *f);

/* Диагностика. */
u64 syscall_count(void);

/* Копирование между ядром и user-памятью (с SMAP-обходом STAC/CLAC
 * и проверкой диапазона через vmm_user_range_ok). */
err_t copy_from_user(u64 uaddr, u64 len, void *kbuf);
err_t copy_to_user(u64 uaddr, u64 len, const void *kbuf);

#endif /* FOBOS_SYS_SYSCALL_H */
