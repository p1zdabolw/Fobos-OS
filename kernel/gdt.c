#include "gdt.h"
#include "../lib/mem.h"

struct gdt_entry { u16 limit_lo; u16 base_lo; u8 base_mid; u8 access; u8 gran; u8 base_hi; } __attribute__((packed));
struct tss_entry {
    u32 reserved0;
    u64 rsp0, rsp1, rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} __attribute__((packed));

struct gdt_ptr { u16 limit; u64 base; } __attribute__((packed));

static struct gdt_entry g_gdt[7];
static struct tss_entry g_tss;
static struct gdt_ptr   g_gp;
static u8 g_tss_stack[16384] __attribute__((aligned(16)));

static void set_entry(int i, u32 base, u32 limit, u8 access, u8 gran) {
    g_gdt[i].limit_lo = (u16)(limit & 0xFFFF);
    g_gdt[i].base_lo  = (u16)(base & 0xFFFF);
    g_gdt[i].base_mid = (u8)((base >> 16) & 0xFF);
    g_gdt[i].access   = access;
    g_gdt[i].gran     = (u8)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    g_gdt[i].base_hi  = (u8)((base >> 24) & 0xFF);
}

void gdt_init(void) {
    memset(g_gdt, 0, sizeof(g_gdt));
    memset(&g_tss, 0, sizeof(g_tss));

    set_entry(0, 0, 0, 0, 0);
    set_entry(1, 0, 0, 0x9A, 0x20);
    set_entry(2, 0, 0, 0x92, 0x00);
    set_entry(3, 0, 0, 0xFA, 0x20);
    set_entry(4, 0, 0, 0xF2, 0x00);

    u64 tss_base  = (u64)&g_tss;
    u32 tss_limit = sizeof(g_tss) - 1;
    set_entry(5, (u32)(tss_base & 0xFFFFFFFF), tss_limit, 0x89, 0x00);
    u64 hi = tss_base >> 32;
    g_gdt[6].limit_lo = (u16)(hi & 0xFFFF);
    g_gdt[6].base_lo  = (u16)((hi >> 16) & 0xFFFF);
    g_gdt[6].base_mid = (u8)((hi >> 32) & 0xFF);
    g_gdt[6].access   = 0;
    g_gdt[6].gran     = 0;
    g_gdt[6].base_hi  = (u8)((hi >> 40) & 0xFF);

    g_tss.rsp0 = (u64)(g_tss_stack + sizeof(g_tss_stack));
    g_tss.iomap_base = sizeof(g_tss);

    g_gp.limit = sizeof(g_gdt) - 1;
    g_gp.base  = (u64)&g_gdt;

    __asm__ volatile(
        "lgdt %0\n"
        "push $0x08\n"
        "lea 1f(%%rip), %%rax\n"
        "push %%rax\n"
        "lretq\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov $0x28, %%ax\n"
        "ltr %%ax\n"
        :: "m"(g_gp) : "rax", "memory"
    );
}