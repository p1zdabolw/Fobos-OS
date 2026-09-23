#include "vmm.h"
#include "pmm.h"
#include "../lib/mem.h"

static u64 *g_pml4 = (u64*)0;

static u64 *next_table(u64 *tbl, int idx, u64 flags) {
    if (!(tbl[idx] & PAGE_PRESENT)) {
        phys_t p = pmm_alloc_frame();
        if (!p) return NULL;
        memset((void*)p, 0, 4096);
        tbl[idx] = p | PAGE_PRESENT | PAGE_WRITE | (flags & PAGE_USER);
    }
    return (u64*)(tbl[idx] & 0x000FFFFFFFFFF000ULL);
}

void vmm_init(void) {
    u64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    g_pml4 = (u64*)(cr3 & 0x000FFFFFFFFFF000ULL);
}

void *vmm_map_page(virt_t va, phys_t pa, u64 flags) {
    u64 i4 = (va >> 39) & 0x1FF;
    u64 i3 = (va >> 30) & 0x1FF;
    u64 i2 = (va >> 21) & 0x1FF;
    u64 i1 = (va >> 12) & 0x1FF;
    u64 *p3 = next_table(g_pml4, (int)i4, flags);
    if (!p3) return NULL;
    u64 *p2 = next_table(p3, (int)i3, flags);
    if (!p2) return NULL;
    u64 *p1 = next_table(p2, (int)i2, flags);
    if (!p1) return NULL;
    p1[i1] = (pa & 0x000FFFFFFFFFF000ULL) | flags | PAGE_PRESENT;
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
    return (void*)va;
}

void vmm_unmap_page(virt_t va) {
    u64 i4 = (va >> 39) & 0x1FF;
    u64 i3 = (va >> 30) & 0x1FF;
    u64 i2 = (va >> 21) & 0x1FF;
    u64 i1 = (va >> 12) & 0x1FF;
    if (!(g_pml4[i4] & PAGE_PRESENT)) return;
    u64 *p3 = (u64*)(g_pml4[i4] & 0x000FFFFFFFFFF000ULL);
    if (!(p3[i3] & PAGE_PRESENT)) return;
    u64 *p2 = (u64*)(p3[i3] & 0x000FFFFFFFFFF000ULL);
    if (!(p2[i2] & PAGE_PRESENT)) return;
    u64 *p1 = (u64*)(p2[i2] & 0x000FFFFFFFFFF000ULL);
    p1[i1] = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
}

phys_t vmm_get_phys(virt_t va) {
    u64 i4 = (va >> 39) & 0x1FF;
    u64 i3 = (va >> 30) & 0x1FF;
    u64 i2 = (va >> 21) & 0x1FF;
    u64 i1 = (va >> 12) & 0x1FF;
    if (!(g_pml4[i4] & PAGE_PRESENT)) return 0;
    u64 *p3 = (u64*)(g_pml4[i4] & 0x000FFFFFFFFFF000ULL);
    if (!(p3[i3] & PAGE_PRESENT)) return 0;
    u64 *p2 = (u64*)(p3[i3] & 0x000FFFFFFFFFF000ULL);
    if (!(p2[i2] & PAGE_PRESENT)) return 0;
    u64 *p1 = (u64*)(p2[i2] & 0x000FFFFFFFFFF000ULL);
    if (!(p1[i1] & PAGE_PRESENT)) return 0;
    return (p1[i1] & 0x000FFFFFFFFFF000ULL) + (va & 0xFFF);
}

void *vmm_map_range(virt_t va, phys_t pa, usize pages, u64 flags) {
    for (usize i = 0; i < pages; i++) {
        vmm_map_page(va + i * 4096, pa + i * 4096, flags);
    }
    return (void*)va;
}