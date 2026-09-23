#include "../kernel/syscall.h"
#include "../kernel/types.h"

static inline long syscall3(long nr, long a1, long a2, long a3) {
    long ret;
    register long rax __asm__("rax") = nr;
    register long rdi __asm__("rdi") = a1;
    register long rsi __asm__("rsi") = a2;
    register long rdx __asm__("rdx") = a3;
    __asm__ volatile("syscall"
        : "=r"(ret)
        : "r"(rax), "r"(rdi), "r"(rsi), "r"(rdx)
        : "rcx", "r11", "memory");
    return ret;
}

void user_init(void) {
    const char msg[] = "user_init running\n";
    syscall3(SYS_WRITE, (long)msg, (long)(sizeof(msg) - 1), 0);
    for (;;) {
        syscall3(SYS_YIELD, 0, 0, 0);
    }
}