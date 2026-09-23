#ifndef FOS_VMM_H
#define FOS_VMM_H

#include "types.h"

#define PAGE_PRESENT 0x1
#define PAGE_WRITE   0x2
#define PAGE_USER    0x4

void  vmm_init(void);
void *vmm_map_page(virt_t va, phys_t pa, u64 flags);
void  vmm_unmap_page(virt_t va);
phys_t vmm_get_phys(virt_t va);
void *vmm_map_range(virt_t va, phys_t pa, usize pages, u64 flags);

#endif