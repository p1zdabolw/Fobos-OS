/*
 * Fobos OS — реализация kvsnprintf/ksnprintf.
 * Слой: чистая логика, без зависимостей; тестируется и в ядре (kselftest),
 * и в host-режиме (tests/host).
 */
#include "vsnprintf.h"
#include "string.h"

#include <stdarg.h>

/* Сложность: O(длина вывода), память: только конечный буфер. */

struct out_buf {
    char *buf;
    size_t cap;
    size_t len;      /* фактическая длина (без учёта усечения) */
    int overflow;    /* cap == 0 или уже записан весь буфер */
};

static void ob_put(struct out_buf *ob, char c)
{
    if (ob->buf != NULL && ob->len + 1u < ob->cap) {
        ob->buf[ob->len] = c;
    } else if (ob->buf != NULL) {
        ob->overflow = 1;
    }
    ob->len++;
}

static void ob_puts(struct out_buf *ob, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        ob_put(ob, s[i]);
    }
}

/* Печатает беззнаковое число в базе base. Возвращает число символов. */
static size_t num_to_str(char *tmp, size_t tmp_cap, u64 v, u32 base, int upper)
{
    static const char low[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    const char *alpha = upper ? up : low;
    size_t n = 0;

    if (base < 2u || base > 16u) {
        base = 10u; /* программная ошибка вызывающего: деградируем в десятичную */
    }
    if (v == 0u) {
        tmp[0] = '0';
        return 1u;
    }
    while (v != 0u && n < tmp_cap) {
        tmp[n++] = alpha[v % base];
        v /= base;
    }
    for (size_t i = 0; i < n / 2u; i++) {
        char t = tmp[i];
        tmp[i] = tmp[n - 1u - i];
        tmp[n - 1u - i] = t;
    }
    return n;
}

static void emit_padded(struct out_buf *ob, const char *digits, size_t ndigits,
                        int negative, int zero_pad, int left_align, int width)
{
    /* Общая длина: знак + цифры. */
    int total = (negative ? 1 : 0) + (int)ndigits;
    int pad = (width > total) ? (width - total) : 0;

    if (!left_align) {
        for (int i = 0; i < pad; i++) {
            ob_put(ob, zero_pad ? '0' : ' ');
        }
    }
    if (negative) {
        ob_put(ob, '-');
    }
    ob_puts(ob, digits, ndigits);
    if (left_align) {
        for (int i = 0; i < pad; i++) {
            ob_put(ob, ' ');
        }
    }
}

int kvsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    struct out_buf ob = { buf, cap, 0, 0 };

    if (fmt == NULL) {
        fmt = "(null fmt)"; /* явная деградация вместо разыменования NULL */
    }

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            ob_put(&ob, *p);
            continue;
        }
        p++;

        /* --- флаги --- */
        int zero_pad = 0;
        int left_align = 0;
        for (;;) {
            if (*p == '0') {
                zero_pad = 1;
                p++;
            } else if (*p == '-') {
                left_align = 1;
                p++;
            } else {
                break;
            }
        }

        /* --- ширина --- */
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        /* --- длина --- */
        enum { LEN_INT, LEN_H, LEN_HH, LEN_L, LEN_LL, LEN_Z } len = LEN_INT;
        if (*p == 'h') {
            p++;
            len = (*p == 'h') ? (p++, LEN_HH) : LEN_H;
        } else if (*p == 'l') {
            p++;
            len = (*p == 'l') ? (p++, LEN_LL) : LEN_L;
        } else if (*p == 'z') {
            p++;
            len = LEN_Z;
        }

        /* --- спецификатор --- */
        switch (*p) {
        case 'd':
        case 'i': {
            s64 v;
            if (len == LEN_LL) {
                v = va_arg(ap, s64);
            } else if (len == LEN_L) {
                v = (s64)va_arg(ap, long);
            } else if (len == LEN_Z) {
                v = (s64)va_arg(ap, size_t);
            } else if (len == LEN_HH) {
                v = (s64)(signed char)va_arg(ap, int);
            } else if (len == LEN_H) {
                v = (s64)(short)va_arg(ap, int);
            } else {
                v = (s64)va_arg(ap, int);
            }
            int negative = v < 0;
            u64 mag = negative ? (0u - (u64)v) : (u64)v;
            char tmp[24];
            size_t n = num_to_str(tmp, sizeof(tmp), mag, 10u, 0);
            emit_padded(&ob, tmp, n, negative, zero_pad && !left_align, left_align, width);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            u64 v;
            if (len == LEN_LL) {
                v = va_arg(ap, u64);
            } else if (len == LEN_L) {
                v = (u64)va_arg(ap, unsigned long);
            } else if (len == LEN_Z) {
                v = (u64)va_arg(ap, size_t);
            } else if (len == LEN_HH) {
                v = (u64)(unsigned char)va_arg(ap, unsigned int);
            } else if (len == LEN_H) {
                v = (u64)(unsigned short)va_arg(ap, unsigned int);
            } else {
                v = (u64)va_arg(ap, unsigned int);
            }
            char tmp[24];
            u32 base = (*p == 'u') ? 10u : ((*p == 'o') ? 8u : 16u);
            size_t n = num_to_str(tmp, sizeof(tmp), v, base, *p == 'X');
            emit_padded(&ob, tmp, n, 0, zero_pad && !left_align, left_align, width);
            break;
        }
        case 'p': {
            void *ptr = va_arg(ap, void *);
            if (ptr == NULL) {
                ob_puts(&ob, "(nil)", 5);
            } else {
                ob_puts(&ob, "0x", 2);
                char tmp[24];
                size_t n = num_to_str(tmp, sizeof(tmp), (u64)(size_t)ptr, 16u, 0);
                ob_puts(&ob, tmp, n);
            }
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            size_t n = strlen(s);
            if (!left_align && width > (int)n) {
                for (int i = 0; i < width - (int)n; i++) {
                    ob_put(&ob, ' ');
                }
            }
            ob_puts(&ob, s, n);
            if (left_align && width > (int)n) {
                for (int i = 0; i < width - (int)n; i++) {
                    ob_put(&ob, ' ');
                }
            }
            break;
        }
        case 'c': {
            int ch = va_arg(ap, int);
            if (!left_align && width > 1) {
                for (int i = 0; i < width - 1; i++) {
                    ob_put(&ob, ' ');
                }
            }
            ob_put(&ob, (char)ch);
            if (left_align && width > 1) {
                for (int i = 0; i < width - 1; i++) {
                    ob_put(&ob, ' ');
                }
            }
            break;
        }
        case '%':
            ob_put(&ob, '%');
            break;
        case '\0':
            ob_put(&ob, '%');
            p--; /* конец строки после '%': закрыть цикл корректно */
            break;
        default:
            /* Неизвестный спецификатор: копируем дословно, не падаем. */
            ob_put(&ob, '%');
            ob_put(&ob, *p);
            break;
        }
    }

    if (buf != NULL && cap > 0u) {
        if (ob.len < cap) {
            buf[ob.len] = '\0';
        } else {
            buf[cap - 1u] = '\0';
        }
    }
    return (ob.len <= 0x7FFFFFFF) ? (int)ob.len : 0x7FFFFFFF;
}

int ksnprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}
