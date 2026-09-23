#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "../lib/mem.h"

#define HEAP_MAGIC 0xF0B05F0B05ULL
#define HEAP_BASE  0xFFFF900000000000ULL
#define HEAP_SIZE  (8ULL * 1024 * 1024)

struct block {
    u64 magic;
    usize size;
    int   used;
    struct block *next;
    struct block *prev;
};

static struct block *g_head;
static virt_t g_heap_end;

void heap_init(void) {
    usize pages = HEAP_SIZE / 4096;
    for (usize i = 0; i < pages; i++) {
        phys_t p = pmm_alloc_frame();
        vmm_map_page(HEAP_BASE + i * 4096, p, PAGE_PRESENT | PAGE_WRITE);
    }
    g_heap_end = HEAP_BASE + HEAP_SIZE;
    g_head = (struct block*)HEAP_BASE;
    g_head->magic = HEAP_MAGIC;
    g_head->size  = HEAP_SIZE - sizeof(struct block);
    g_head->used  = 0;
    g_head->next  = NULL;
    g_head->prev  = NULL;
}

static void split(struct block *b, usize n) {
    if (b->size < n + sizeof(struct block) + 16) return;
    struct block *nb = (struct block*)((u8*)b + sizeof(struct block) + n);
    nb->magic = HEAP_MAGIC;
    nb->size  = b->size - n - sizeof(struct block);
    nb->used  = 0;
    nb->next  = b->next;
    nb->prev  = b;
    if (nb->next) nb->next->prev = nb;
    b->next = nb;
    b->size = n;
}

void *kmalloc(usize n) {
    n = (n + 15) & ~(usize)15;
    for (struct block *b = g_head; b; b = b->next) {
        if (!b->used && b->size >= n) {
            split(b, n);
            b->used = 1;
            return (u8*)b + sizeof(struct block);
        }
    }
    return NULL;
}

void *kzalloc(usize n) {
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

static void coalesce(struct block *b) {
    if (b->next && !b->next->used) {
        struct block *n = b->next;
        b->size += sizeof(struct block) + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
    }
    if (b->prev && !b->prev->used) {
        struct block *p = b->prev;
        p->size += sizeof(struct block) + b->size;
        p->next = b->next;
        if (b->next) b->next->prev = p;
    }
}

void kfree(void *p) {
    if (!p) return;
    struct block *b = (struct block*)((u8*)p - sizeof(struct block));
    if (b->magic != HEAP_MAGIC) return;
    b->used = 0;
    coalesce(b);
}

void *krealloc(void *p, usize n) {
    if (!p) return kmalloc(n);
    struct block *b = (struct block*)((u8*)p - sizeof(struct block));
    if (b->size >= n) return p;
    void *np = kmalloc(n);
    if (!np) return NULL;
    memcpy(np, p, b->size);
    kfree(p);
    return np;
}

u64 heap_used(void) {
    u64 total = 0;
    for (struct block *b = g_head; b; b = b->next) total += b->used ? b->size : 0;
    return total;
}