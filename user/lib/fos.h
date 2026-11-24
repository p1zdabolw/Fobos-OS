/*
 * Fobos OS — user-space рантайм: syscall-обёртки и мини-printf.
 *
 * Freestanding, без libc. SIMD запрещён на уровне сборки (-mgeneral-regs-only):
 * состояние FPU между задачами не сохраняется (FND-3b), потому user-код
 * обязан быть целочисленным — это проверяется компиляцией, не договорённостью.
 */
#ifndef FOBOS_USER_FOS_H
#define FOBOS_USER_FOS_H

#include <fobos/syscall.h>

typedef long long           s64u;
typedef unsigned long long  u64u;

/* --- syscall-обёртки --- */

#define FOS_SYSCALL0(n) \
    fos_syscall1(n, 0)
#define FOS_SYSCALL1(n, a) \
    fos_syscall1(n, (u64u)(a))
#define FOS_SYSCALL2(n, a, b) \
    fos_syscall2(n, (u64u)(a), (u64u)(b))
#define FOS_SYSCALL3(n, a, b, c) \
    fos_syscall3(n, (u64u)(a), (u64u)(b), (u64u)(c))

static inline s64u fos_syscall1(u64u n, u64u a)
{
    s64u ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a)
                      : "rcx", "r11", "memory");
    return ret;
}

static inline s64u fos_syscall2(u64u n, u64u a, u64u b)
{
    s64u ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b)
                      : "rcx", "r11", "memory");
    return ret;
}

static inline s64u fos_syscall3(u64u n, u64u a, u64u b, u64u c)
{
    s64u ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return ret;
}

static inline void fos_exit(int code)
{
    (void)FOS_SYSCALL1(FOS_SYS_exit, code);
    for (;;) {
        __asm__ volatile ("ud2");
    }
}

static inline s64u fos_write(const char *buf, u64u len)
{
    return FOS_SYSCALL3(FOS_SYS_write, FOS_FD_CONSOLE, buf, len);
}

static inline s64u fos_read(void *buf, u64u len)
{
    return FOS_SYSCALL3(FOS_SYS_read, FOS_FD_KBD, buf, len);
}

static inline int fos_getpid(void)
{
    return (int)FOS_SYSCALL0(FOS_SYS_getpid);
}

static inline unsigned fos_uptime(void)
{
    return (unsigned)FOS_SYSCALL0(FOS_SYS_uptime);
}

static inline void fos_sleep(unsigned ms)
{
    (void)FOS_SYSCALL1(FOS_SYS_sleep, ms);
}

static inline void *fos_brk(void *addr)
{
    return (void *)(long long)FOS_SYSCALL1(FOS_SYS_brk, addr);
}

static inline unsigned fos_abi_version(void)
{
    return (unsigned)FOS_SYSCALL0(FOS_SYS_abi_version);
}

/* --- вывод --- */

void fos_puts(const char *s);
void fos_printf(const char *fmt, ...);

/* --- клавиатура --- */

/* Коды клавиш, значимые для user-программ (см. kernel/drivers/kbd.h). */
#define FOS_KEY_ENTER 38u
#define FOS_KEY_ESC   39u

/* Блокирующее чтение одного события. Возвращает 0 (успех) или -1. */
int fos_getkey(struct fos_keyevent *out);

#endif /* FOBOS_USER_FOS_H */
