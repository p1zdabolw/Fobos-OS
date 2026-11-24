/*
 * Fobos OS — user-space рантайм: вывод и клавиатура.
 */
#include "fos.h"

#include <stddef.h>

#define OUT_BUF 256

void fos_puts(const char *s)
{
    u64u n = 0;
    while (s[n] != '\0') {
        n++;
    }
    (void)fos_write(s, n);
}

/* Мини-printf: %s %c %d %u %x %p %% (целочисленные, без ширины/флагов).
 * Достаточно для диагностического вывода user-программ v0. */
static void out_char(char *buf, u64u *pos, char c)
{
    if (*pos + 1u >= OUT_BUF) {
        (void)fos_write(buf, *pos);
        *pos = 0;
    }
    buf[(*pos)++] = c;
}

static void out_str(char *buf, u64u *pos, const char *s)
{
    while (*s != '\0') {
        out_char(buf, pos, *s++);
    }
}

static void out_num(char *buf, u64u *pos, u64u v, unsigned base, int sign,
                    int *neg_taken)
{
    char tmp[24];
    u64u n = 0;
    int neg = 0;

    if (sign != 0 && (s64u)v < 0) {
        neg = 1;
        v = (u64u)(-(s64u)v);
    }
    *neg_taken |= neg;

    if (v == 0u) {
        tmp[n++] = '0';
    }
    while (v != 0u) {
        u64u d = v % base;
        tmp[n++] = (char)((d < 10u) ? ('0' + d) : ('a' + d - 10u));
        v /= base;
    }
    if (neg != 0) {
        out_char(buf, pos, '-');
    }
    while (n > 0u) {
        out_char(buf, pos, tmp[--n]);
    }
}

void fos_printf(const char *fmt, ...)
{
    char buf[OUT_BUF];
    u64u pos = 0;

    /* Чтение аргументов: целочисленный SysV, только регистровый поток
     * (SIMD/varargs-буфер запрещён сборкой). Ограничение: v0-printf
     * читает args как unsigned long long — все значения <= 64 бит. */
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    for (; *fmt != '\0'; fmt++) {
        if (*fmt != '%') {
            out_char(buf, &pos, *fmt);
            continue;
        }
        fmt++;
        if (*fmt == '\0') {
            break;
        }
        switch (*fmt) {
        case 's': {
            const char *s = (const char *)__builtin_va_arg(ap, const char *);
            out_str(buf, &pos, (s != NULL) ? s : "(null)");
            break;
        }
        case 'c':
            out_char(buf, &pos, (char)__builtin_va_arg(ap, int));
            break;
        case 'd': {
            int neg = 0;
            out_num(buf, &pos, (u64u)(s64u)__builtin_va_arg(ap, int), 10u, 1, &neg);
            break;
        }
        case 'u':
            out_num(buf, &pos, (u64u)__builtin_va_arg(ap, unsigned), 10u, 0, &(int){0});
            break;
        case 'x':
            out_num(buf, &pos, (u64u)__builtin_va_arg(ap, unsigned), 16u, 0, &(int){0});
            break;
        case 'p': {
            void *p = __builtin_va_arg(ap, void *);
            out_str(buf, &pos, "0x");
            out_num(buf, &pos, (u64u)p, 16u, 0, &(int){0});
            break;
        }
        case '%':
            out_char(buf, &pos, '%');
            break;
        default:
            out_char(buf, &pos, '%');
            out_char(buf, &pos, *fmt);
            break;
        }
    }
    __builtin_va_end(ap);

    if (pos > 0u) {
        (void)fos_write(buf, pos);
    }
}

int fos_getkey(struct fos_keyevent *out)
{
    s64u n = fos_read(out, sizeof(struct fos_keyevent));
    return (n == (s64u)sizeof(struct fos_keyevent)) ? 0 : -1;
}
