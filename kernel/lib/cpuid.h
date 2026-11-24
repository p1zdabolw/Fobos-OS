/*
 * Fobos OS — обёртка над CPUID.
 */
#ifndef FOBOS_LIB_CPUID_H
#define FOBOS_LIB_CPUID_H

#include <fobos/types.h>

static inline void cpuid(u32 leaf, u32 subleaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    u32 la = leaf, lb, lc = subleaf, ld;
    __asm__ volatile ("cpuid"
                      : "=a"(la), "=b"(lb), "=c"(lc), "=d"(ld)
                      : "0"(la), "2"(lc));
    if (a) *a = la;
    if (b) *b = lb;
    if (c) *c = lc;
    if (d) *d = ld;
}

/* Детект гипервизора (QEMU/KVM и совместимые): лист 0x40000000, ненулевая
 * сигнатура в ebx/ecx/edx. На bare metal лист обычно отсутствует (eax=0). */
static inline int cpu_hypervisor_present(void)
{
    u32 a = 0, b = 0, c = 0, d = 0;
    cpuid(0x40000000u, 0u, &a, &b, &c, &d);
    if (a < 0x40000000u) {
        return 0;
    }
    return (b | c | d) != 0u;
}

#endif /* FOBOS_LIB_CPUID_H */
