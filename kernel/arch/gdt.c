/*
 * Fobos OS — GDT ядра x86_64.
 *
 * Плоская модель: код/данные ring0 и ring3 + TSS (FND-3a).
 *
 * Дескрипторы (canonical для long mode):
 *   0x08 KCODE 0x00AF9A000000FFFF : G=1, L=1, DPL0, exec/read
 *   0x10 KDATA 0x00CF92000000FFFF : G=1, B=1, DPL0, read/write
 *   0x18 UCODE 0x00AFFA000000FFFF : G=1, L=1, DPL3, exec/read (compat-слот)
 *   0x20 UDATA 0x00CFF200000000FFFF : G=1, B=1, DPL3, read/write
 *   0x28 UCODE-дубль (тот же DPL3 L=1)
 *   0x30 TSS   16 байт (слоты 6-7)
 *
 * Почему UCODE дублируется в 0x28: семантика SYSRET различается между
 * производителями — AMD/QEMU грузят CS = STAR[63:48]+16, SS = +8;
 * Intel: CS = STAR[63:48], SS = +8. С UCODE в ОБОИХ слотах и UDATA в +8
 * оба варианта дают валидные сегменты: QEMU/AMD -> CS=0x2B, SS=0x23;
 * Intel -> CS=0x1B, SS=0x23. Ядро не зависит от производителя CPU.
 */
#include "arch.h"
#include "../lib/log.h"
#include "../lib/string.h"

struct gdt_entry {
    u16 limit0;
    u16 base0;
    u8  base1;
    u8  access;
    u8  limit1_flags;
    u8  base2;
};

_Static_assert(sizeof(struct gdt_entry) == 8, "GDT entry ABI");

/* lgdt m16&64 читает 10 байт ПОДРЯД: limit (2) + base (8) со смещением 2.
 * Естественное выравнивание положило бы base на смещение 8 — это баг:
 * base обязан быть ровно за limit, поэтому packed. */
struct gdt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

enum gdt_selectors {
    GDT_NULL  = 0x00,
    GDT_KCODE = 0x08,
    GDT_KDATA = 0x10,
    GDT_UCODE = 0x18,
    GDT_UDATA = 0x20
};

#define GDT_ENTRIES 8  /* null + KCODE + KDATA + UCODE + UDATA + TSS(2) + резерв */

static struct gdt_entry g_gdt[GDT_ENTRIES];
static struct gdt_ptr g_gdtp;
static const char *const GDT_TAG = "arch/gdt";

static void set_entry(u16 idx, u8 access, u8 flags)
{
    struct gdt_entry *e = &g_gdt[idx];
    e->limit0 = 0xFFFF;
    e->base0 = 0;
    e->base1 = 0;
    e->access = access;
    /* Байт 6: старший ниббл — флаги (G/D/L/AVL), младший — limit[19:16].
     * Перепутанный порядок даёт D=L=1 (резерв), и валидация дескриптора
     * при доставке прерываний падает с #GP (проверено трассировкой QEMU). */
    e->limit1_flags = (u8)((flags << 4) | 0x0Fu);
    e->base2 = 0;
}

/* Системный дескриптор (TSS): base 64-бит, limit 20 бит, G=0, access 0x89. */
static void set_tss_entry(u16 idx, u64 base, u32 limit)
{
    struct gdt_entry *lo = &g_gdt[idx];
    struct gdt_entry *hi = &g_gdt[idx + 1];
    memset(lo, 0, sizeof(*lo));
    memset(hi, 0, sizeof(*hi));
    lo->limit0 = (u16)(limit & 0xFFFFu);
    lo->base0 = (u16)(base & 0xFFFFu);
    lo->base1 = (u8)((base >> 16) & 0xFFu);
    lo->access = 0x89u;                 /* present, TSS available, 64-bit */
    lo->limit1_flags = (u8)((limit >> 16) & 0x0Fu);  /* G=0, D/L=0 */
    lo->base2 = (u8)((base >> 24) & 0xFFu);
    hi->limit0 = (u16)((base >> 32) & 0xFFFFu);      /* base[47:32] */
    hi->base0 = (u16)(base >> 48);                   /* base[63:48] */
}

void gdt_install_tss(u64 base, u32 limit)
{
    /* Слот 6 (байты 0x30..0x37) + старшая половина base в слоте 7. */
    set_tss_entry(6, base, limit);
    /* Дескриптор обновляется на живой GDT: base/limit корректны с самого
     * начала записи (16 байт подряд), ltr читает после возврата. */
    __asm__ volatile ("lgdt %0" : : "m"(g_gdtp) : "memory");
}

err_t gdt_init(void)
{
    memset(g_gdt, 0, sizeof(g_gdt));

    /* NULL-дескриптор обязан остаться нулевым. */
    set_entry(1, 0x9Au, 0xAu); /* KCODE: P=1 DPL=0 S=1 type=1010, G=1 L=1 */
    set_entry(2, 0x92u, 0xCu); /* KDATA: P=1 DPL=0 S=1 type=0010, G=1 B=1 */
    set_entry(3, 0xFAu, 0xAu); /* UCODE (0x18): DPL=3 */
    set_entry(4, 0xF2u, 0xCu); /* UDATA (0x20): DPL=3 */
    set_entry(5, 0xFAu, 0xAu); /* UCODE-дубль (0x28): SYSRET AMD/QEMU +16 */

    g_gdtp.limit = (u16)(sizeof(g_gdt) - 1u);
    g_gdtp.base = (u64)(u64 *)&g_gdt;

    __asm__ volatile ("lgdt %0" : : "m"(g_gdtp) : "memory");

    /* Перезагрузка CS через lretq на 64-битный селектор. */
    __asm__ volatile (
        "pushq %0\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:"
        :
        : "i"(GDT_KCODE)
        : "rax", "memory");

    __asm__ volatile (
        "mov %0, %%ds\n\t"
        "mov %0, %%es\n\t"
        "mov %0, %%ss\n\t"
        "xor %%eax, %%eax\n\t"
        "mov %%eax, %%fs\n\t"
        "mov %%eax, %%gs"
        :
        : "r"((u16)GDT_KDATA)
        : "eax", "memory");

    klog(KLOG_INFO, GDT_TAG,
         "GDT перезагружена: cs=0x%02x ds=0x%02x, ring3-сегменты 0x1b/0x23, TSS-слот 0x28 (записей: %d)",
         (unsigned int)GDT_KCODE, (unsigned int)GDT_KDATA, GDT_ENTRIES);
    return K_OK;
}
