/*
 * Fobos OS — IDT и обработчики исключений x86_64.
 *
 * FND-2a: устанавливаются 32 вектора исключений + 16 векторов IRQ
 * (0x20..0x2F, dual-PIC). IRQ-вектора маршрутизируются в irq_c_dispatch().
 * Все вектора без кода ошибки получают синтетический 0,
 * поэтому кадр intr_frame единообразен.
 *
 * FND-3a: #DF обслуживается на IST1 (отдельный стек — защита от каскадного
 * #DF при переполнении стеков ring0/ring3). Исключения из ring 3 (#UD,
 * #NP, #SS, #GP, #PF) завершают виновный user-процесс (task_kill_fault)
 * вместо паники ядра; исключения из ring 0 — как раньше, дамп + паника.
 *
 * Особый случай: #BP (int3, вектор 3) — возвращает управление (используется
 * kselftest для проверки сквозного пути IDT). Остальные ring0-исключения —
 * дамп состояния + остановка системы.
 */
#include "arch.h"
#include "irq.h"
#include "../sys/task.h"
#include "../lib/log.h"
#include "../lib/io.h"
#include "../lib/string.h"

struct idt_entry {
    u16 offset0;
    u16 selector;
    u8  ist;
    u8  flags;
    u16 offset1;
    u32 offset2;
    u32 zero;
};

_Static_assert(sizeof(struct idt_entry) == 16, "IDT entry ABI");

/* lidt m16&64: base обязан идти сразу за limit (см. gdt.c) — packed. */
struct idt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

#define IDT_ENTRIES 256
#define IDT_FLAG_INT64 0x8Eu  /* present, DPL0, 64-bit interrupt gate */

#define IDT_STUB_COUNT 48u /* 32 исключения + 16 IRQ */

#define IDT_IST_DF 1u      /* IST1 — стек Double Fault (tss.c) */

extern const void *const isr_stub_table[IDT_STUB_COUNT];

static struct idt_entry g_idt[IDT_ENTRIES];
static struct idt_ptr g_idtp;
static const char *const IDT_TAG = "arch/idt";

static void set_gate(u16 vec, u64 handler, u8 ist)
{
    struct idt_entry *e = &g_idt[vec];
    e->offset0 = (u16)(handler & 0xFFFFu);
    e->selector = 0x08u;
    e->ist = ist;
    e->flags = IDT_FLAG_INT64;
    e->offset1 = (u16)((handler >> 16) & 0xFFFFu);
    e->offset2 = (u32)((handler >> 32) & 0xFFFFFFFFu);
    e->zero = 0;
}

static u64 read_cr2(void)
{
    u64 v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

static u64 read_cr3(void)
{
    u64 v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static const char *const exc_names[32] = {
    "#DE divide error", "#DB debug", "NMI", "#BP breakpoint", "#OF overflow",
    "#BR bound range", "#UD invalid opcode", "#NM device n/a", "#DF double fault",
    "COPROC seg overrun", "#TS invalid TSS", "#NP segment not present",
    "#SS stack fault", "#GP general protection", "#PF page fault", "x87 FP exc",
    "alignment check", "machine check", "#SM SIMD FP", "virtualization exc",
    "control protection", "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "hypervisor injection", "VMM comm exc",
    "security exception", "reserved"
};

static void fault_dump(const struct intr_frame *f)
{
    klog(KLOG_ERROR, IDT_TAG, "исключение %llu: %s", (unsigned long long)f->vector,
         (f->vector < 32u) ? exc_names[f->vector] : "неизвестный вектор");
    klog(KLOG_ERROR, IDT_TAG,
         "  rip=%016llx cs=%04llx rflags=%016llx rsp=%016llx ss=%04llx err=%llx",
         (unsigned long long)f->rip, (unsigned long long)f->cs,
         (unsigned long long)f->rflags, (unsigned long long)f->rsp,
         (unsigned long long)f->ss, (unsigned long long)f->errcode);
    klog(KLOG_ERROR, IDT_TAG,
         "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx",
         (unsigned long long)f->rax, (unsigned long long)f->rbx,
         (unsigned long long)f->rcx, (unsigned long long)f->rdx);
    klog(KLOG_ERROR, IDT_TAG,
         "  rsi=%016llx rdi=%016llx rbp=%016llx r8 =%016llx",
         (unsigned long long)f->rsi, (unsigned long long)f->rdi,
         (unsigned long long)f->rbp, (unsigned long long)f->r8);
    klog(KLOG_ERROR, IDT_TAG,
         "  r9 =%016llx r10=%016llx r11=%016llx r12=%016llx",
         (unsigned long long)f->r9, (unsigned long long)f->r10,
         (unsigned long long)f->r11, (unsigned long long)f->r12);
    klog(KLOG_ERROR, IDT_TAG,
         "  r13=%016llx r14=%016llx r15=%016llx cr3=%016llx",
         (unsigned long long)f->r13, (unsigned long long)f->r14,
         (unsigned long long)f->r15, (unsigned long long)read_cr3());

    if (f->vector == 14u) {
        u64 e = f->errcode;
        klog(KLOG_ERROR, IDT_TAG,
             "  #PF: cr2=%016llx %s%s%s%s%s",
             (unsigned long long)read_cr2(),
             (e & 1u) ? "[present]" : "[not-present]",
             (e & 2u) ? "[write]" : "[read]",
             (e & 4u) ? "[user]" : "[kernel]",
             (e & 8u) ? "[rsvd-write]" : "",
             (e & 16u) ? "[insn-fetch]" : "");
    }
}

void intr_c_handler(struct intr_frame *f)
{
    if (f->vector >= 32u) {
        /* Аппаратное прерывание 8259A: dispatch сам делает EOI.
         * Внутри обработчика прерывания выключены (interrupt gate). */
        irq_c_dispatch(f);
        /* Вытеснение только user-кода (FND-3a): ядерный код не
         * вытесняется по построению (нет блокировок — нет гонок).
         * Вектор 0x20 = IRQ0 таймер. */
        if (f->vector == 0x20u && (f->cs & 3u) != 0u &&
            sched_need_resched() != 0) {
            sched_schedule();
        }
        return;
    }

    if (f->vector == 3u) {
        /* #BP: наблюдаемая точка останова ядра — возвращаем управление.
         * kselftest использует это для проверки сквозного пути IDT. */
        klog(KLOG_DEBUG, IDT_TAG, "#BP по rip=0x%llx (ожидаемо в kselftest)",
             (unsigned long long)f->rip);
        return;
    }

    /* Исключение из ring 3: завершаем процесс-виновник (FND-3a).
     * Список векторов: #UD, #NP, #SS, #GP, #PF — типовые ошибки
     * user-кода (невалидная инструкция/адрес, access violation). */
    if ((f->cs & 3u) != 0u &&
        (f->vector == 6u || f->vector == 11u || f->vector == 12u ||
         f->vector == 13u || f->vector == 14u)) {
        fault_dump(f);
        task_kill_fault(f);
        /* не возвращает управление: планировщик переключает задачи */
    }

    fault_dump(f);
    klog_panic(IDT_TAG, "необработанное исключение, вектор %llu",
               (unsigned long long)f->vector);
}

err_t idt_init(void)
{
    memset(g_idt, 0, sizeof(g_idt));

    for (u16 v = 0; v < IDT_STUB_COUNT; v++) {
        u8 ist = (v == 8u) ? IDT_IST_DF : 0u;   /* #DF -> IST1 */
        set_gate(v, (u64)(u64 *)isr_stub_table[v], ist);
    }

    g_idtp.limit = (u16)(sizeof(g_idt) - 1u);
    g_idtp.base = (u64)(u64 *)&g_idt;
    __asm__ volatile ("lidt %0" : : "m"(g_idtp) : "memory");

    klog(KLOG_INFO, IDT_TAG,
         "IDT: 32 вектора исключений + 16 IRQ-векторов 0x20..0x2F (#DF->IST1, int3 = возврат)");
    return K_OK;
}

/* Доступ для selftest: гейт вектора установлен как interrupt gate. */
int idt_gate_present(u16 vec)
{
    if (vec >= IDT_ENTRIES) {
        return 0;
    }
    return (g_idt[vec].flags & 0x80u) != 0u &&
           (g_idt[vec].flags & 0x0Fu) == 0x0Eu;
}
