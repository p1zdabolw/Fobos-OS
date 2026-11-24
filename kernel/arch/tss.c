/*
 * Fobos OS — TSS x86_64, IST для #DF, включение NXE/SMAP/SMEP.
 *
 * FND-3a: первая задача ring 3. Обязательные архитектурные шаги:
 *   1. TSS-дескриптор в GDT (selector 0x28) + ltr;
 *   2. RSP0 = стек ядра (планировщик обновляет при каждом переключении);
 *   3. IST1 = отдельный стек Double Fault — #DF при переполнении стеков
 *      ring0/ring3 обязан иметь гарантированный стек, иначе каскад #DF;
 *   4. EFER.NXE — до создания любых PTE с NX-битом (W^X user-памяти);
 *   5. CR4.SMEP/SMAP — при поддержке CPUID.7.EBX[20]/[21]. SMAP требует
 *      STAC/CLAC вокруг легального доступа ядра к user-памяти
 *      (sys/syscall.c, copy_to/from_user).
 */
#include "tss.h"
#include "arch.h"
#include "../lib/log.h"
#include "../lib/cpuid.h"
#include "../lib/string.h"
#include "../lib/io.h"
#include "../mm/phys.h"  /* PAGE_SIZE */

/* Разметка TSS64 (IA-32 SDM, таблица 7-11; x86_64 — без полей SS0/ESP0
 * 32-битной модели). Двойные слова split'ятся только для точной раскладки. */
struct tss64 {
    u32 reserved0;
    u32 rsp0_lo;
    u32 rsp0_hi;
    u32 rsp1_lo;
    u32 rsp1_hi;
    u32 rsp2_lo;
    u32 rsp2_hi;
    u32 reserved1;
    u32 reserved2;
    u32 ist1_lo;
    u32 ist1_hi;
    u32 ist2_lo;
    u32 ist2_hi;
    u32 ist3_lo;
    u32 ist3_hi;
    u32 ist4_lo;
    u32 ist4_hi;
    u32 ist5_lo;
    u32 ist5_hi;
    u32 ist6_lo;
    u32 ist6_hi;
    u32 ist7_lo;
    u32 ist7_hi;
    u32 reserved3;
    u32 reserved4;
    u16 io_bitmap_base;
    u16 reserved5;
};

_Static_assert(sizeof(struct tss64) == 104, "TSS64 layout");

static struct tss64 g_tss;
static int g_smap_on;
static const char *const TSS_TAG = "arch/tss";

/* Стек #DF: 8 КиБ, отдельный от всех задач. Double fault по IST не может
 * рекурсироваться: при fault внутри IST-стека — shutdown (triple fault). */
#define DF_STACK_FRAMES 2u
static u64 g_df_stack[DF_STACK_FRAMES * (PAGE_SIZE / sizeof(u64))];

static u64 read_cr4(void)
{
    u64 v;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    return v;
}

static void write_cr4(u64 v)
{
    __asm__ volatile ("mov %0, %%cr4" : : "r"(v) : "memory");
}

static u64 rdmsr(u32 msr)
{
    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static void wrmsr(u32 msr, u64 val)
{
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((u32)val), "d"((u32)(val >> 32)));
}

/* MSR EFER: SCE (bit 0) — syscall/sysret; NXE (bit 11) — исполнение NX. */
#define MSR_EFER         0xC0000080u
#define EFER_SCE         (1ull << 0)
#define EFER_NXE         (1ull << 11)

void tss_set_rsp0(u64 kstack_top)
{
    g_tss.rsp0_lo = (u32)(kstack_top & 0xFFFFFFFFu);
    g_tss.rsp0_hi = (u32)(kstack_top >> 32);
}

u64 tss_rsp0_value(void)
{
    return ((u64)g_tss.rsp0_hi << 32) | g_tss.rsp0_lo;
}

int cpu_smap_enabled(void)
{
    return g_smap_on;
}

err_t tss_init(void)
{
    memset(&g_tss, 0, sizeof(g_tss));

    /* RSP0 до первого переключения задач — стек ядра из entry.S
     * (задача kmain). Планировщик перезапишет при каждом свитче. */
    tss_set_rsp0(0);

    /* IST1: вершина стека #DF. */
    u64 df_top = (u64)g_df_stack + sizeof(g_df_stack);
    g_tss.ist1_lo = (u32)(df_top & 0xFFFFFFFFu);
    g_tss.ist1_hi = (u32)(df_top >> 32);

    /* I/O bitmap: base за пределами TSS -> все порты запрещены из ring 3. */
    g_tss.io_bitmap_base = (u16)sizeof(g_tss);

    gdt_install_tss((u64)(u64 *)&g_tss, sizeof(g_tss) - 1u);
    __asm__ volatile ("ltr %0" : : "r"((u16)TSS_SELECTOR) : "memory");

    /* EFER.NXE — обязателен ДО первого PTE с NX-битом (vmm user-маппинг). */
    u64 efer = rdmsr(MSR_EFER);
    efer |= EFER_NXE;
    wrmsr(MSR_EFER, efer);

    /* SMEP/SMAP — по поддержке CPUID leaf 7 subleaf 0. */
    u32 a = 0, b = 0, c = 0, d = 0;
    cpuid(0x7u, 0u, &a, &b, &c, &d);
    u64 cr4 = read_cr4();
    u64 added = 0;
    if ((b & (1u << 20)) != 0u) {   /* SMEP: запрет exec user-страниц из ring0 */
        cr4 |= (1ull << 20);
        added |= (1ull << 20);
    }
    if ((b & (1u << 21)) != 0u) {   /* SMAP: запрет чтения/записи user-страниц из ring0 */
        cr4 |= (1ull << 21);
        added |= (1ull << 21);
        g_smap_on = 1;
    }
    write_cr4(cr4);

    klog(KLOG_INFO, TSS_TAG,
         "TSS: selector=0x%02x rsp0=0x%llx ist1=0x%llx, NXE=1%s%s",
         (unsigned int)TSS_SELECTOR,
         (unsigned long long)tss_rsp0_value(),
         (unsigned long long)df_top,
         (added & (1ull << 20)) ? ", SMEP=on" : "",
         (added & (1ull << 21)) ? ", SMAP=on" : "");
    return K_OK;
}
