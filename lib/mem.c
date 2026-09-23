#include "mem.h"

void *memset(void *p, int c, usize n) {
    u8 *d = (u8*)p;
    while (n--) *d++ = (u8)c;
    return p;
}

void *memcpy(void *d, const void *s, usize n) {
    u8 *dd = (u8*)d;
    const u8 *ss = (const u8*)s;
    while (n--) *dd++ = *ss++;
    return d;
}

void *memmove(void *d, const void *s, usize n) {
    u8 *dd = (u8*)d;
    const u8 *ss = (const u8*)s;
    if (dd < ss) {
        while (n--) *dd++ = *ss++;
    } else {
        dd += n; ss += n;
        while (n--) *--dd = *--ss;
    }
    return d;
}

int memcmp(const void *a, const void *b, usize n) {
    const u8 *x = (const u8*)a;
    const u8 *y = (const u8*)b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}