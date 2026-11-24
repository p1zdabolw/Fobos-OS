/*
 * Fobos OS — ACPI (FND-2g) (прототипы).
 *
 * Назначение: единственный источник истины о конфигурации железа для
 * APIC-маршрутизации (MADT), таймеров и питания (FADT) вместо хардкода
 * QEMU-машин (правило проекта: никаких магических чисел железа).
 * Поиск RSDP: EFI Configuration Table (GUID ACPI 2.0/1.0 из boot_info),
 * fallback — скан EBDA и [0xE0000, 0x100000) с проверкой контрольных сумм.
 * Парсер отказоустойчив: повреждённая таблица пропускается с WARN; при
 * полном отсутствии ACPI система живёт на legacy-путях (PIC/PIT/8042).
 *
 * Хост-режим (FOBOS_HOST_TEST): ядро парсера acpi_parse_rsdp() гоняется
 * в host-тестах на синтетических наборах таблиц — та же схема обкатки,
 * что FAT32 в tests/host (правило проекта: чистая логика — сначала host).
 */
#ifndef FOBOS_DRV_ACPI_H
#define FOBOS_DRV_ACPI_H

#include <fobos/errors.h>
#include <fobos/types.h>

/* Лимиты дескрипторов: типовой ПК имеет 1 I/O APIC и <= 16 CPU. */
#define ACPI_IOAPIC_MAX   4u
#define ACPI_LAPIC_MAX    32u
#define ACPI_OVERRIDE_MAX 24u

/* Полярность/триггер прерывания (MADT override -> I/O APIC REDTBL). */
#define ACPI_POL_ACTIVE_HIGH 0u
#define ACPI_POL_ACTIVE_LOW  1u
#define ACPI_TRIG_EDGE       0u
#define ACPI_TRIG_LEVEL      1u

/* FADT.flags bit 20: аппаратно-сокращённый ACPI (нет 8259/PM-блоков). */
#define ACPI_FADT_F_HW_REDUCED (1u << 20)

struct acpi_ioapic {
    u8  id;
    u64 paddr;        /* физический адрес MMIO-регистров (IOREGSEL/IOWIN) */
    u32 gsi_base;
};

struct acpi_lapic {
    u32 id;           /* APIC ID (x2APIC допускает > 255) */
    u32 acpi_uid;
    u32 flags;        /* bit0 enabled, bit1 online-capable (rev 5+) */
};

struct acpi_gsi_override {
    u8  isa_irq;
    u32 gsi;
    u8  pol;          /* ACPI_POL_* */
    u8  trig;         /* ACPI_TRIG_* */
};

struct acpi_info {
    int  present;            /* 1 = RSDP найден и прошёл валидацию */
    u8   rsdp_revision;      /* 1 / 2 */
    u8   acpi_version;       /* 2 = XSDT (ACPI 2.0+), 1 = только RSDT */
    char oem_id[7];          /* из RSDP, NUL-terminated */

    /* MADT ("APIC") */
    u64  madt_lapic_paddr;   /* база LAPIC из MADT (0 = не указана) */
    u32  madt_flags;
    u32  madt_lapic_count;   /* включённые Processor Local APIC */
    u32  ioapic_count;
    u32  override_count;
    struct acpi_ioapic ioapics[ACPI_IOAPIC_MAX];
    struct acpi_lapic lapics[ACPI_LAPIC_MAX];
    struct acpi_gsi_override overrides[ACPI_OVERRIDE_MAX];

    /* FADT ("FACP") */
    int  fadt_present;
    u8   fadt_revision;
    u16  sci_irq;
    u32  smi_cmd;            /* 0 = SMM-переход не требуется (UEFI-путь) */
    u16  pm1a_cnt_port;      /* IO-порт PM1a_CNT_BLK (0 = нет) */
    u16  pm1b_cnt_port;
    u16  pm_timer_port;      /* IO-порт PM_TMR_BLK (0 = нет) */
    u8   pm_timer_len;       /* 3 (24-бит) или 4 (32-бит); 0 = непригоден */
    u32  fadt_flags;
    int  reset_reg_present;
    u8   reset_reg_space;    /* GAS address space: 0 = sysmem, 1 = IO */
    u64  reset_reg_addr;
    u8   reset_value;
    int  hw_reduced;         /* FADT.flags bit 20 */
};

/* Инициализация: поиск RSDP -> разбор XSDT/RSDT -> FADT/MADT + дамп всех
 * дескрипторов в журнал (критерий этапа FND-2). Не критично для загрузки:
 * при K_ENODEV ядро остаётся на legacy-путях (PIC/PIT/8042-reboot). */
#ifndef FOBOS_HOST_TEST
err_t acpi_init(const struct fbpi_boot_info *bi);
#endif

const struct acpi_info *acpi_get_info(void);

/* ISA IRQ -> GSI + полярность/триггер. Без override: GSI == IRQ,
 * active high, edge (типовая ISA-семантика). */
err_t acpi_isa_irq_gsi(u8 isa_irq, u32 *out_gsi, u8 *out_pol, u8 *out_trig);

/* Ядро парсера (host-тесты + acpi_init): валидирует RSDP, находит
 * XSDT/RSDT, разбирает все дескрипторы. На ядре rsdp — identity-указатель
 * на физический адрес (таблицы выше 4 ГиБ до-маппятся vmm_kernel_map_mmio);
 * на хосте — указатель на синтетический набор в памяти процесса. */
err_t acpi_parse_rsdp(const void *rsdp, struct acpi_info *out);

#endif /* FOBOS_DRV_ACPI_H */
