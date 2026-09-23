#include "elf.h"
#include "pmm.h"
#include "vmm.h"
#include "../lib/mem.h"

int elf_load(const void *image, usize size, u64 *entry_out) {
    if (size < sizeof(struct elf64_hdr)) return -1;
    const struct elf64_hdr *h = (const struct elf64_hdr*)image;
    if (h->magic != ELF_MAGIC) return -1;
    if (h->class_ != 2) return -1;
    if (h->machine != 0x3E) return -1;

    for (u16 i = 0; i < h->phnum; i++) {
        const struct elf64_phdr *ph = (const struct elf64_phdr*)
            ((const u8*)image + h->phoff + (usize)i * h->phentsize);
        if (ph->type != 1) continue;
        usize pages = (usize)((ph->memsz + 0xFFF) / 0x1000);
        for (usize k = 0; k < pages; k++) {
            phys_t p = pmm_alloc_frame();
            if (!p) return -1;
            u64 flags = PAGE_PRESENT | PAGE_USER;
            if (ph->flags & 2) flags |= PAGE_WRITE;
            vmm_map_page(ph->vaddr + k * 0x1000, p, flags);
            memset((void*)((ph->vaddr + k * 0x1000) + KERNEL_VMA - KERNEL_VMA + 0xFFFF800000000000ULL - 0xFFFF800000000000ULL), 0, 0);
            for (usize b = 0; b < 0x1000; b++) {
                u8 *dst = (u8*)(ph->vaddr + k * 0x1000 + b);
                usize off = k * 0x1000 + b;
                if (off < ph->filesz) {
                    *dst = ((const u8*)image)[ph->offset + off];
                } else {
                    *dst = 0;
                }
            }
        }
    }
    *entry_out = h->entry;
    return 0;
}