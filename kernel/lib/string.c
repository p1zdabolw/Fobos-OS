/*
 * Fobos OS — реализация строковых/памятных примитивов.
 * Простые байтовые циклы: производительность критична с FND-4 (FAT32), там
 * будет отдельная оптимизированная реализация с бенчмарком. Сложность всех
 * функций — O(n), дополнительной памяти не требуют.
 */
#include "string.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    u8 *d = (u8 *)dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = (u8)c;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    if (d == s || n == 0u) {
        return dst;
    }
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = n; i > 0u; i--) {
            d[i - 1u] = s[i - 1u];
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = (const u8 *)a;
    const u8 *y = (const u8 *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0') {
            return (int)(u8)a[i] - (int)(u8)b[i];
        }
    }
    return 0;
}

int strcmp(const char *a, const char *b)
{
    return strncmp(a, b, (size_t)-1 / 2);
}
