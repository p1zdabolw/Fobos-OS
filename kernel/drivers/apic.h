/*
 * Fobos OS — APIC: Local APIC + I/O APIC (FND-2g) (прототипы).
 *
 * Режимы LAPIC: xAPIC (MMIO @ база из MSR 0x1B) и x2APIC (MSR 0x800+).
 * Режим определяется СОСТОЯНИЕМ ПРОШИВКИ, а не выбором: переход
 * x2APIC -> xAPIC архитектурно запрещён, поэтому ядро поддерживает оба
 * (реальное железо: firmware может включить x2APIC до загрузки).
 * I/O APIC всегда MMIO (вне зависимости от режима LAPIC).
 *
 * Источник истины о GSI — ACPI MADT (acpi.c). Без ACPI — K_ENODEV,
 * ядро остаётся на dual-PIC. Базовые вектора не меняются: 0x20..0x2F,
 * поэтому обработчики IRQ0/1/12 (timer/kbd/mouse) переезжают без изменений.
 */
#ifndef FOBOS_DRV_APIC_H
#define FOBOS_DRV_APIC_H

#include <fobos/errors.h>
#include <fobos/types.h>

/* Смещения регистров LAPIC (xAPIC MMIO; x2APIC — MSR 0x800 + (reg >> 4)). */
#define APIC_REG_ID        0x020u
#define APIC_REG_VERSION   0x030u
#define APIC_REG_TPR       0x080u
#define APIC_REG_EOI       0x0B0u
#define APIC_REG_SPURIOUS  0x0F0u
#define APIC_REG_ESR       0x280u
#define APIC_REG_LVT_TIMER 0x320u
#define APIC_REG_LVT_LINT0 0x350u
#define APIC_REG_LVT_LINT1 0x360u
#define APIC_REG_TIMER_ICR 0x380u   /* initial count (таймер, убывающий) */
#define APIC_REG_TIMER_DCR 0x3E0u   /* divide configuration */

/* Биты LVT/регистров. */
#define APIC_SPURIOUS_VECTOR 0xFFu
#define APIC_SPURIOUS_ENABLE (1u << 8)
#define APIC_LVT_MASK        (1u << 16)
#define APIC_LVT_PERIODIC    (1u << 17)

/* DCR-коды делителя (SDM 11.5.4). */
#define APIC_DCR_DIV1   0xBu
#define APIC_DCR_DIV16  0x3u

/* Инициализация LAPIC + программирование I/O APIC REDTBL для всех 16
 * ISA-IRQ (все замаскированы — размаскирует timer_switch_apic_source).
 * Требует: acpi_init, idt_init (включая spurious-гейт 0xFF), handlers
 * IRQ0/1/12 зарегистрированы. Прерывания включать не обязан и не включает. */
err_t apic_init(void);

int   apic_ready(void);
const char *apic_mode_name(void);   /* "xAPIC" / "x2APIC" / "-" */
u32   apic_id(void);                /* нормализованный ID */

/* Доступ для драйверов/самопроверок (значения регистров, не адреса). */
u32   apic_read_reg(u32 reg);
void  apic_write_reg(u32 reg, u32 val);

/* Маршрутизация ISA IRQ -> GSI (из ACPI override) -> вектор 0x20+IRQ
 * через REDTBL. mask=1 — замаскирована. */
err_t apic_route_isa(u8 isa_irq, u8 vector, u8 mask);

void  apic_eoi(void);

#endif /* FOBOS_DRV_APIC_H */
