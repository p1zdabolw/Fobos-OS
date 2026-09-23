#include "pmm.h"
#include "../lib/mem.h"
#include "../lib/printf.h"

#define PMM_BITMAP_SIZE (1 << 16)
#define PAGE_SIZE 4096ULL

static u8  g_bitmap[PMM_BITMAP_SIZE];
static u64 g_total_pages;
static u64 g_free_pages;
static u64 g_last_index;

struct mb2_tag { u32 type; u32 size; };
struct mb2_mmap_entry { u64 base; u64 length; u32 type; u32 reserved; };
struct mb2_mmap { u32 type; u32 size; u32 entry_size; u32 entry_version; struct mb2_mmap_entry entries[]; };

static inline void bit_set(u64 i)   { g_bitmap[i >> 3] |=  (u8)(1u << (i & 7)); }
static inline void bit_clear(u64 i) { g_bitmap[i >> 3] &= (u8)~(1u << (i & 7)); }
static inline int  bit_get(u64 i)   { return (g_bitmap[i >> 3] >> (i & 7)) & 1; }

static void mark_used(phys_t base, u64 len) {
    u64 start = base >> 12;
    u64 end   = (base + len + PAGE_SIZE - 1) >> 12;
    for (u64 i = start; i < end; i++) {
        if (i < g_total_pages && !bit_get(i)) { bit_set(i); g_free_pages--; }
    }
}

void pmm_init(u64 mb2_info) {
    memset(g_bitmap, 0xFF, sizeof(g_bitmap));
    g_total_pages = (128ULL * 1024 * 1024) / PAGE_SIZE;
    g_free_pages = 0;
    g_last_index = 1;

    u8 *p = (u8*)mb2_info;
    u32 total = *(u32*)p;
    struct mb2_tag *tag = (struct mb2_tag*)(p + 8);
    while ((u8*)tag < p + total && tag->type != 0) {
        if (tag->type == 6) {
            struct mb2_mmap *mm = (struct mb2_mmap*)tag;
            u32 n = (mm->size - 16) / mm->entry_size;
            for (u32 i = 0; i < n; i++) {
                struct mb2_mmap_entry *e = (struct mb2_mmap_entry*)((u8*)mm->entries + i * mm->entry_size);
                if (e->type == 1) {
                    u64 top = (e->base + e->length) / PAGE_SIZE;
                    if (top > g_total_pages) g_total_pages = top;
                }
            }
        }
        tag = (struct mb2_tag*)((u8*)tag + ((tag->size + 7) & ~7u));
    }

    if (g_total_pages > PMM_BITMAP_SIZE * 8) g_total_pages = PMM_BITMAP_SIZE * 8;

    for (u64 i = 0; i < g_total_pages; i++) {
        bit_clear(i);
        g_free_pages++;
    }

    tag = (struct mb2_tag*)(p + 8);
    while ((u8*)tag < p + total && tag->type != 0) {
        if (tag->type == 6) {
            struct mb2_mmap *mm = (struct mb2_mmap*)tag;
            u32 n = (mm->size - 16) / mm->entry_size;
            for (u32 i = 0; i < n; i++) {
                struct mb2_mmap_entry *e = (struct mb2_mmap_entry*)((u8*)mm->entries + i * mm->entry_size);
                if (e->type != 1) mark_used(e->base, e->length);
            }
        }
        tag = (struct mb2_tag*)((u8*)tag + ((tag->size + 7) & ~7u));
    }

    mark_used(0, 0x100000);
    extern u8 kernel_end[];
    mark_used(0x100000, ((u64)kernel_end - 0x100000));
    mark_used((u64)g_bitmap, sizeof(g_bitmap));

    if (mb2_info && total > 0 && total < 0x100000) {
        u64 start = mb2_info & ~0xFFFULL;
        u64 end   = (mb2_info + (u64)total + 0xFFFULL) & ~0xFFFULL;
        mark_used(start, end - start);
    }

    kprintf("PMM: %u MB total, %u MB free\n",
            (u32)(g_total_pages * PAGE_SIZE / (1024*1024)),
            (u32)(g_free_pages * PAGE_SIZE / (1024*1024)));
}

phys_t pmm_alloc_frame(void) {
    for (u64 i = g_last_index; i < g_total_pages; i++) {
        if (!bit_get(i)) {
            bit_set(i); g_free_pages--; g_last_index = i + 1;
            return i * PAGE_SIZE;
        }
    }
    for (u64 i = 1; i < g_last_index; i++) {
        if (!bit_get(i)) {
            bit_set(i); g_free_pages--; g_last_index = i + 1;
            return i * PAGE_SIZE;
        }
    }
    return 0;
}

phys_t pmm_alloc_frames(usize n) {
    for (u64 i = 1; i + n < g_total_pages; i++) {
        usize k = 0;
        while (k < n && !bit_get(i + k)) k++;
        if (k == n) {
            for (usize j = 0; j < n; j++) { bit_set(i + j); g_free_pages--; }
            return i * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free_frame(phys_t p) {
    u64 i = p >> 12;
    if (i < g_total_pages && bit_get(i)) { bit_clear(i); g_free_pages++; }
}

void pmm_free_frames(phys_t p, usize n) {
    u64 start = p >> 12;
    for (usize k = 0; k < n; k++) {
        if (start + k < g_total_pages && bit_get(start + k)) {
            bit_clear(start + k);
            g_free_pages++;
        }
    }
}

u64 pmm_total_bytes(void) { return g_total_pages * PAGE_SIZE; }
u64 pmm_free_bytes(void)  { return g_free_pages  * PAGE_SIZE; }