#include "syscall.h"
#include "sched.h"
#include "../lib/string.h"

static inline void wrmsr(u32 msr, u64 v) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((u32)v), "d"((u32)(v >> 32)));
}

void syscall_init(void) {
    wrmsr(0xC0000081, (0x08ULL << 32) | (0x10ULL << 48));
    wrmsr(0xC0000084, 0x200ULL);
}

void syscall_dispatch(struct registers *r) {
    u64 nr = r->rax;
    u64 a1 = r->rdi;
    u64 a2 = r->rsi;
    u64 a3 = r->rdx;
    (void)a3;
    switch (nr) {
        case SYS_EXIT:  r->rax = 0; break;
        case SYS_WRITE: {
            const char *s = (const char*)a1;
            usize n = (usize)a2;
            for (usize i = 0; i < n; i++) {
                extern void serial_putc(char);
                serial_putc(s[i]);
            }
            r->rax = n;
            break;
        }
        case SYS_READ:  r->rax = 0; break;
        case SYS_GETPID: r->rax = sched_current_id(); break;
        case SYS_YIELD: sched_yield(); r->rax = 0; break;
        default: r->rax = (u64)-1; break;
    }
}