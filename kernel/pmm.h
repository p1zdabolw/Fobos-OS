#ifndef FOS_PMM_H
#define FOS_PMM_H

#include "types.h"

void  pmm_init(u64 mb2_info);
phys_t pmm_alloc_frame(void);
void  pmm_free_frame(phys_t p);
phys_t pmm_alloc_frames(usize n);
u64   pmm_total_bytes(void);
u64   pmm_free_bytes(void);

#endif