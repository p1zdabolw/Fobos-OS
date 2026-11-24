/*
 * Fobos OS — реализация APIC (FND-2g).
 *
 * LAPIC: xAPIC (MMIO-окно, регистры 32-битные по 16-байтовому шагу) либо
 * x2APIC (MSR 0x800 + reg/16). Выбор — по MSR IA32_APIC_BASE bit 10
 * (состояние прошивки); переход x2APIC->xAPIC запрещён SDM, поэтому
 * другого пути, кроме поддержки обоих, нет.
 *
 * I/O APIC: REDTBL[GSI] -> вектор; полярность/триггер — из MADT override
 * (ISA: по умолчанию active high + edge). Адреса берутся из MADT, не
 * хардкодятся (0xFEC00000 — типовое, но не гарантированное).
 *
 * Порядок вызовов: acpi_init -> (kbd/mouse handlers) -> apic_init ->
 * timer_switch_apic_source (он размаскирует линии под cli).
 */
#include "apic.h"
#include "acpi.h"
#include "../arch/irq.h"
#include "../lib/io.h"
#include "../lib/log.h"
#include "../lib/cpuid.h"
#include "../mm/vmm.h"

/* MSR IA32_APIC_BASE (SDM 11.4.3). */
#define MSR_IA32_APIC_BASE 0x1Bu
#define APIC_MSR_EN        (1ull << 11)
#define APIC_MSR_X2APIC    (1ull << 10)
#define APIC_MSR_BASE_MASK 0xFFFFF000ull   /* биты 12..35 — физическая база */

/* I/O APIC MMIO-окно: IOREGSEL @0x00, IOWIN @0x10. */
#define IOAPIC_REGSEL      0x00u
#define IOAPIC_IOWIN       0x10u
#define IOAPIC_REG_VER     0x01u
#define IOAPIC_REDTBL_BASE 0x10u   /* low энтри i: 0x10+2i, high: 0x11+2i */

/* Биты REDTBL low (SDM 3.10.10): delivery fixed=000, dest mode physical=0,
 * polarity bit13, trigger bit15, mask bit16. */
#define REDTBL_POL_LOW     (1u << 13)
#define REDTBL_TRIG_LEVEL  (1u << 15)
#define REDTBL_MASK        (1u << 16)

/* CPUID.1:EDX[9] — наличие LAPIC. */
#define CPUID_FEAT_EDX_APIC (1u << 9)

struct ioapic_state {
    int  used;
    u64  paddr;
    u32  gsi_base;
    u32  entries;      /* из I/O APIC version reg bits 16..23 */
};

#define APIC_MMIO_SIZE 0x1000u

static u64 g_lapic_base;      /* физическая база xAPIC (0 при x2APIC) */
static int g_x2apic;
static int g_ready;
static struct ioapic_state g_ioapics[ACPI_IOAPIC_MAX];
static u32 g_ioapic_count;
static const char *const APIC_TAG = "drv/apic";

int apic_ready(void)
{
    return g_ready;
}

const char *apic_mode_name(void)
{
    if (g_ready == 0) {
        return "-";
    }
    return g_x2apic ? "x2APIC" : "xAPIC";
}

u32 apic_read_reg(u32 reg)
{
    if (g_x2apic != 0) {
        return (u32)rdmsr(0x800u + (reg >> 4));
    }
    return *(volatile u32 *)(u64)(g_lapic_base + (u64)reg);
}

void apic_write_reg(u32 reg, u32 val)
{
    if (g_x2apic != 0) {
        wrmsr(0x800u + (reg >> 4), (u64)val);
        return;
    }
    *(volatile u32 *)(u64)(g_lapic_base + (u64)reg) = val;
}

u32 apic_id(void)
{
    if (g_ready == 0) {
        return 0u;
    }
    u32 v = apic_read_reg(APIC_REG_ID);
    /* xAPIC хранит ID в битах 24..31, x2APIC — во всём слове. */
    return g_x2apic ? v : (v >> 24);
}

void apic_eoi(void)
{
    apic_write_reg(APIC_REG_EOI, 0u);
}

/* ------------------------------------------------------------------ */
/* I/O APIC                                                            */
/* ------------------------------------------------------------------ */

static u32 ioapic_read(u32 idx, u32 reg)
{
    io_out32((u16)(g_ioapics[idx].paddr + IOAPIC_REGSEL), reg);
    return io_in32((u16)(g_ioapics[idx].paddr + IOAPIC_IOWIN));
}

static void ioapic_write(u32 idx, u32 reg, u32 val)
{
    io_out32((u16)(g_ioapics[idx].paddr + IOAPIC_REGSEL), reg);
    io_out32((u16)(g_ioapics[idx].paddr + IOAPIC_IOWIN), val);
}

/* Ищет I/O APIC, обслуживающий GSI. */
static int ioapic_find_gsi(u32 gsi, u32 *out_idx, u32 *out_pin)
{
    for (u32 i = 0; i < g_ioapic_count; i++) {
        if (g_ioapics[i].used != 0 &&
            gsi >= g_ioapics[i].gsi_base &&
            gsi < g_ioapics[i].gsi_base + g_ioapics[i].entries) {
            *out_idx = i;
            *out_pin = gsi - g_ioapics[i].gsi_base;
            return 1;
        }
    }
    return 0;
}

static err_t ioapic_route(u32 gsi, u8 vector, u8 pol, u8 trig, u8 mask)
{
    u32 idx, pin;
    if (ioapic_find_gsi(gsi, &idx, &pin) == 0) {
        klog(KLOG_ERROR, APIC_TAG, "GSI %u не покрывается ни одним I/O APIC", gsi);
        return K_ERANGE;
    }

    const u32 reg = IOAPIC_REDTBL_BASE + pin * 2u;
    u32 low = vector;
    if (pol == ACPI_POL_ACTIVE_LOW) {
        low |= REDTBL_POL_LOW;
    }
    if (trig == ACPI_TRIG_LEVEL) {
        low |= REDTBL_TRIG_LEVEL;
    }
    if (mask != 0u) {
        low |= REDTBL_MASK;
    }
    const u32 high = (apic_id() & 0xFFu) << 24;   /* physical dest: биты 24..31 */

    ioapic_write(idx, reg, low);
    ioapic_write(idx, reg + 1u, high);
    return K_OK;
}

err_t apic_route_isa(u8 isa_irq, u8 vector, u8 mask)
{
    u32 gsi;
    u8 pol, trig;
    err_t rc = acpi_isa_irq_gsi(isa_irq, &gsi, &pol, &trig);
    if (rc != K_OK) {
        return rc;
    }
    return ioapic_route(gsi, vector, pol, trig, mask);
}

/* ------------------------------------------------------------------ */
/* Инициализация                                                       */
/* ------------------------------------------------------------------ */

err_t apic_init(void)
{
    if (g_ready != 0) {
        return K_OK;
    }
    if (acpi_get_info()->present == 0 || acpi_get_info()->ioapic_count == 0u) {
        klog(KLOG_INFO, APIC_TAG,
             "ACPI/MADT недоступны — APIC не инициализируется (dual-PIC)");
        return K_ENODEV;
    }

    u32 a, b, c, d;
    cpuid(1u, 0u, &a, &b, &c, &d);
    if ((d & CPUID_FEAT_EDX_APIC) == 0u) {
        klog(KLOG_WARN, APIC_TAG, "CPUID: локальный APIC отсутствует — dual-PIC");
        return K_ENODEV;
    }

    u64 base_msr = rdmsr(MSR_IA32_APIC_BASE);
    if ((base_msr & APIC_MSR_EN) == 0u) {
        /* Прошивка выключила глобально: включаем (бит 11), base сохраняется. */
        wrmsr(MSR_IA32_APIC_BASE, base_msr | APIC_MSR_EN);
        base_msr = rdmsr(MSR_IA32_APIC_BASE);
        if ((base_msr & APIC_MSR_EN) == 0u) {
            klog(KLOG_ERROR, APIC_TAG, "включение LAPIC через MSR 0x1B не сработало");
            return K_EIO;
        }
        klog(KLOG_INFO, APIC_TAG, "LAPIC включён через MSR IA32_APIC_BASE");
    }
    g_x2apic = (int)((base_msr & APIC_MSR_X2APIC) != 0u);

    if (g_x2apic == 0) {
        g_lapic_base = base_msr & APIC_MSR_BASE_MASK;
        if (g_lapic_base == 0u) {
            /* Прошивка оставила нулевую базу: единственный авторитетный
             * источник — MADT (Local APIC Address / Override). */
            g_lapic_base = acpi_get_info()->madt_lapic_paddr;
        }
        if (g_lapic_base == 0u) {
            klog(KLOG_ERROR, APIC_TAG, "база LAPIC не определена (MSR+MADT нулевые)");
            return K_ENODEV;
        }
        err_t rc = vmm_kernel_map_mmio(g_lapic_base, APIC_MMIO_SIZE);
        if (rc != K_OK) {
            klog(KLOG_ERROR, APIC_TAG, "маппинг LAPIC @0x%llx: %s",
                 (unsigned long long)g_lapic_base, k_strerror(rc));
            return rc;
        }
    }

    /* TPR=0 (принимать все приоритеты), spurious на 0xFF (гейт ставит idt),
     * LINT0/LINT1 и таймер замаскированы до калибровки. ESR очищается
     * записью (SDM 11.5.3: перед чтением обязана быть запись). */
    apic_write_reg(APIC_REG_TPR, 0u);
    apic_write_reg(APIC_REG_SPURIOUS, APIC_SPURIOUS_VECTOR | APIC_SPURIOUS_ENABLE);
    apic_write_reg(APIC_REG_LVT_LINT0, APIC_LVT_MASK);
    apic_write_reg(APIC_REG_LVT_LINT1, APIC_LVT_MASK);
    apic_write_reg(APIC_REG_LVT_TIMER, APIC_LVT_MASK);
    apic_write_reg(APIC_REG_ESR, 0u);

    /* I/O APIC: маппинг MMIO + версии/ёмкости REDTBL. */
    const struct acpi_info *ai = acpi_get_info();
    for (u32 i = 0; i < ai->ioapic_count && i < ACPI_IOAPIC_MAX; i++) {
        const struct acpi_ioapic *src = &ai->ioapics[i];
        if (src->paddr == 0u) {
            klog(KLOG_WARN, APIC_TAG, "I/O APIC id=%u: нулевой адрес в MADT — пропущен",
                 (unsigned int)src->id);
            continue;
        }
        err_t rc = vmm_kernel_map_mmio(src->paddr, APIC_MMIO_SIZE);
        if (rc != K_OK) {
            klog(KLOG_ERROR, APIC_TAG, "маппинг I/O APIC @0x%llx: %s",
                 (unsigned long long)src->paddr, k_strerror(rc));
            return rc;
        }
        u32 idx = g_ioapic_count;
        g_ioapics[idx].used = 1;
        g_ioapics[idx].paddr = src->paddr;
        g_ioapics[idx].gsi_base = src->gsi_base;
        u32 ver = ioapic_read(idx, IOAPIC_REG_VER);
        g_ioapics[idx].entries = (ver >> 16) & 0xFFu;
        g_ioapic_count++;

        klog(KLOG_INFO, APIC_TAG,
             "IOAPIC id=%u @0x%llx: версия %u, GSI %u..%u",
             (unsigned int)src->id, (unsigned long long)src->paddr,
             (unsigned int)(ver & 0xFFu),
             (unsigned int)src->gsi_base,
             (unsigned int)(src->gsi_base + g_ioapics[idx].entries - 1u));
    }
    if (g_ioapic_count == 0u) {
        klog(KLOG_ERROR, APIC_TAG, "ни один I/O APIC не инициализирован");
        return K_ENODEV;
    }

    /* Все 16 ISA-IRQ маршрутизируются ЗАМАСКИРОВАННЫМИ: размаскирование —
     * атомарный шаг переключения источника тика (timer_switch_apic_source). */
    for (u8 irq = 0; irq < 16u; irq++) {
        u32 gsi;
        u8 pol, trig;
        err_t rc = acpi_isa_irq_gsi(irq, &gsi, &pol, &trig);
        if (rc != K_OK) {
            klog(KLOG_ERROR, APIC_TAG, "GSI-маппинг ISA IRQ %u: %s",
                 (unsigned int)irq, k_strerror(rc));
            return rc;
        }
        rc = ioapic_route(gsi, (u8)(0x20u + irq), pol, trig, 1u);
        if (rc != K_OK) {
            return rc;
        }
    }

    u32 lver = apic_read_reg(APIC_REG_VERSION);
    g_ready = 1;
    klog(KLOG_INFO, APIC_TAG,
         "LAPIC %s: id=%u версия %u.%u max_lvt=%u, ISA-IRQ замаскированы в REDTBL",
         apic_mode_name(), (unsigned int)apic_id(),
         (unsigned int)((lver >> 4) & 0xFu), (unsigned int)(lver & 0xFu),
         (unsigned int)((lver >> 16) & 0xFFu));
    return K_OK;
}
