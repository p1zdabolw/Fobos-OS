#include "printf.h"
#include "../kernel/serial.h"
#include "string.h"

void kvprintf_ex(char **dst, usize *rem, const char *fmt, __builtin_va_list ap);

static void out_char(char **dst, usize *rem, char c) {
    if (dst && *rem > 1) { **dst = c; (*dst)++; (*rem)--; }
    else if (!dst) { serial_putc(c); }
}

static void out_str(char **dst, usize *rem, const char *s) {
    while (*s) out_char(dst, rem, *s++);
}

static void out_uint(char **dst, usize *rem, u64 v, int base, int pad, char padc) {
    char tmp[32];
    int n = 0;
    const char *digits = "0123456789abcdef";
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % (u64)base]; v /= (u64)base; }
    while (n < pad) tmp[n++] = padc;
    while (n--) out_char(dst, rem, tmp[n]);
}

static void out_int(char **dst, usize *rem, i64 v, int pad) {
    if (v < 0) {
        out_char(dst, rem, '-');
        out_uint(dst, rem, (u64)(-v), 10, pad - 1, ' ');
    } else {
        out_uint(dst, rem, (u64)v, 10, pad, ' ');
    }
}

void kvprintf_ex(char **dst, usize *rem, const char *fmt, __builtin_va_list ap) {
    while (*fmt) {
        if (*fmt != '%') { out_char(dst, rem, *fmt++); continue; }
        fmt++;
        int pad = 0;
        char padc = ' ';
        if (*fmt == '0') { padc = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { pad = pad * 10 + (*fmt - '0'); fmt++; }
        switch (*fmt) {
            case 'd': case 'i': out_int(dst, rem, __builtin_va_arg(ap, i64), pad); break;
            case 'u': out_uint(dst, rem, __builtin_va_arg(ap, u64), 10, pad, padc); break;
            case 'x': out_uint(dst, rem, __builtin_va_arg(ap, u64), 16, pad, padc); break;
            case 'p': out_str(dst, rem, "0x"); out_uint(dst, rem, (u64)__builtin_va_arg(ap, void*), 16, 16, '0'); break;
            case 'c': out_char(dst, rem, (char)__builtin_va_arg(ap, int)); break;
            case 's': {
                const char *s = __builtin_va_arg(ap, const char*);
                out_str(dst, rem, s ? s : "(null)");
                break;
            }
            case '%': out_char(dst, rem, '%'); break;
            default:  out_char(dst, rem, '%'); out_char(dst, rem, *fmt); break;
        }
        fmt++;
    }
    if (dst) **dst = 0;
}

void kvprintf(const char *fmt, __builtin_va_list ap) {
    kvprintf_ex(NULL, NULL, fmt, ap);
}

void kprintf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvprintf(fmt, ap);
    __builtin_va_end(ap);
}

void snprintf(char *buf, usize cap, const char *fmt, ...) {
    char *d = buf;
    usize r = cap;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvprintf_ex(&d, &r, fmt, ap);
    __builtin_va_end(ap);
}