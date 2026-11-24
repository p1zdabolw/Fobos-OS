/*
 * Fobos OS — реализация ACPI (FND-2g).
 *
 * Гарантии реализации:
 *  - валидация ВСЕХ контрольных сумм и размеров до разыменования полей;
 *  - каждый доступ к полям таблиц — с проверкой границы length;
 *  - повреждённый дескриптор пропускается с WARN, парс не прерывается;
 *  - отсутствие ACPI — рабочий режим (K_ENODEV), а не паника.
 *
 * Адресация таблиц: физические адреса из RSDP/XSDT. < 4 ГиБ покрыты
 * identity-отображением загрузчика; >= 4 ГиБ до-маппятся через
 * vmm_kernel_map_mmio (UC). Хост-режим: адреса — указатели синтетического
 * набора, маппинг не требуется.
 */
#include "acpi.h"

#include <fobos/errors.h>
#include <fobos/types.h>

#include <stddef.h>

#include "../lib/log.h"
#include "../lib/string.h"

#ifndef FOBOS_HOST_TEST
#include <fobos/boot_info.h>
#include "../mm/vmm.h"
#endif

#include "../mm/phys.h"   /* PAGE_SIZE (host-безопасный заголовок) */

/* ------------------------------------------------------------------ */
/* Структуры спецификации ACPI (UEFI 2.10, ACPI 6.5)                   */
/* ------------------------------------------------------------------ */

struct acpi_rsdp {
    char signature[8];    /* "RSD PTR " */
    u8  checksum;         /* сумма байт 0..19 == 0 (mod 256) */
    char oem_id[6];
    u8  revision;         /* 1 = ACPI 1.0, 2 = ACPI 2.0+ */
    u32 rsdt_phys;        /* 32-битный адрес RSDT */
    u32 length;           /* 36 у rev >= 2 */
    u64 xsdt_phys;        /* 64-битный адрес XSDT (rev >= 2) */
    u8  reserved[3];
    u8  ext_checksum;     /* сумма байт 0..35 == 0 (rev >= 2) */
} __attribute__((packed));
_Static_assert(sizeof(struct acpi_rsdp) == 36, "ACPI RSDP layout");

struct acpi_sdt_header {
    char signature[4];
    u32  length;          /* полный размер таблицы, >= 36 */
    u8   revision;
    u8   checksum;        /* сумма всех length байт == 0 */
    char oem_id[6];
    char oem_table_id[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
} __attribute__((packed));
_Static_assert(sizeof(struct acpi_sdt_header) == 36, "ACPI SDT header layout");

/* Generic Address Structure (FADT RESET_REG и др.). */
struct acpi_gas {
    u8  address_space;    /* 0 = system memory, 1 = system I/O */
    u8  bit_width;
    u8  bit_offset;
    u8  access_size;
    u64 address;
} __attribute__((packed));
_Static_assert(sizeof(struct acpi_gas) == 12, "ACPI GAS layout");

/* Заголовок EFI-таблицы и System Table — только чтение Configuration Table
 * (единственный способ получить RSDP на UEFI без сканирования). */
struct efi_table_header {
    u64 signature;
    u32 revision;
    u32 header_size;
    u32 crc32;
    u32 reserved;
} __attribute__((packed));
_Static_assert(sizeof(struct efi_table_header) == 24, "EFI table header");

struct efi_system_table {
    struct efi_table_header hdr;
    u64 firmware_vendor;        /* CHAR16* (не используется) */
    u32 firmware_revision;
    u32 pad;
    u64 console_in_handle;
    u64 con_in;
    u64 console_out_handle;
    u64 con_out;
    u64 std_err_handle;
    u64 std_err;
    u64 runtime_services;
    u64 boot_services;
    u64 number_of_table_entries;
    u64 configuration_table;
} __attribute__((packed));
_Static_assert(sizeof(struct efi_system_table) == 120, "EFI System Table layout");

struct efi_config_entry {
    u64 guid_hi;          /* EFI_GUID в little-endian: первые 8 байт */
    u64 guid_lo;          /* последние 8 байт */
    u64 vendor_table;
} __attribute__((packed));
_Static_assert(sizeof(struct efi_config_entry) == 24, "EFI config entry layout");

#define EFI_ST_SIGNATURE 0x5453595320494249ull   /* "IBI SYST" little-endian */

/* GUID из Configuration Table: ACPI 2.0 {8868e871-e4f1-11d3-bc22-0080c73c8881},
 * ACPI 1.0 {eb9d2d30-2d88-11d3-9a16-0090273fc14d}. */
#define EFI_ACPI20_GUID_HI 0x11d3e4f18868e871ull
#define EFI_ACPI20_GUID_LO 0x81883cc7800222bcull
#define EFI_ACPI10_GUID_HI 0x11d3882deb9d2d30ull
#define EFI_ACPI10_GUID_LO 0x4dc13f279000169aull

static struct acpi_info g_acpi;
static const char *const ACPI_TAG = "drv/acpi";

#define FOUR_GIB 0x100000000ull

/* Оборонительная граница обхода Configuration Table: у типовой прошивки
 * 10-30 энтри (UEFI 2.10); аномально большое число означает мусор в ST. */
#define EFI_CONFIG_TABLE_MAX 4096u

/* ------------------------------------------------------------------ */
/* Утилиты чтения                                                      */
/* ------------------------------------------------------------------ */

static u8 sum_bytes(const u8 *p, u32 n)
{
    u8 s = 0;
    for (u32 i = 0; i < n; i++) {
        s = (u8)(s + p[i]);
    }
    return s;
}

/* Безопасные чтения полей таблицы: за границей length возвращают 0. */
static u8 sdt_u8(const u8 *t, u32 off, u32 len)
{
    return (off + 1u <= len) ? t[off] : 0u;
}
static u16 sdt_u16(const u8 *t, u32 off, u32 len)
{
    if (off + 2u > len) {
        return 0u;
    }
    u16 v;
    memcpy(&v, t + off, sizeof(v));
    return v;
}
static u32 sdt_u32(const u8 *t, u32 off, u32 len)
{
    if (off + 4u > len) {
        return 0u;
    }
    u32 v;
    memcpy(&v, t + off, sizeof(v));
    return v;
}
static u64 sdt_u64(const u8 *t, u32 off, u32 len)
{
    if (off + 8u > len) {
        return 0ull;
    }
    u64 v;
    memcpy(&v, t + off, sizeof(v));
    return v;
}

/* Физический адрес -> рабочий указатель. Ядро: < 4 ГиБ — готовое
 * identity-окно; выше — до-маппинг UC на [paddr, paddr+size). Хост:
 * адрес — это указатель синтетического набора (см. tests/host).
 * size — объём, который будет прочитан (выравнивание внутри функции). */
static const void *tbl_p2v(u64 paddr, u64 size)
{
#ifndef FOBOS_HOST_TEST
    if (paddr >= FOUR_GIB) {
        if (vmm_kernel_map_mmio(paddr & ~0xFFFull,
                                size + (paddr & 0xFFFull)) != K_OK) {
            return NULL;
        }
    }
#else
    (void)size;
#endif
    return (const void *)(u64)paddr;
}

/* ------------------------------------------------------------------ */
/* Валидация RSDP и SDT-заголовков                                     */
/* ------------------------------------------------------------------ */

static int rsdp_valid(const struct acpi_rsdp *rp)
{
    if (memcmp(rp->signature, "RSD PTR ", 8) != 0) {
        return 0;
    }
    if (sum_bytes((const u8 *)rp, 20) != 0u) {
        return 0;
    }
    if (rp->revision < 2u) {
        return rp->rsdt_phys != 0u;
    }
    if (rp->length != 36u) {
        return 0;
    }
    if (sum_bytes((const u8 *)rp, 36) != 0u) {
        return 0;
    }
    return 1;
}

static int sdt_header_valid(const struct acpi_sdt_header *h, const char *sig)
{
    if (h->length < sizeof(struct acpi_sdt_header) || h->length > 0x100000u) {
        return 0;
    }
    if (sig != NULL && memcmp(h->signature, sig, 4) != 0) {
        return 0;
    }
    if (sum_bytes((const u8 *)h, h->length) != 0u) {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Разбор FADT ("FACP")                                                */
/* ------------------------------------------------------------------ */

static void parse_fadt(const u8 *t, u32 len, struct acpi_info *out)
{
    out->fadt_present = 1;
    out->fadt_revision = t[8];
    out->sci_irq = sdt_u16(t, 46, len);
    out->smi_cmd = sdt_u32(t, 48, len);
    out->fadt_flags = sdt_u32(t, 112, len);
    out->hw_reduced = (out->fadt_flags & ACPI_FADT_F_HW_REDUCED) ? 1 : 0;

    /* PM-блоки: legacy 32-битные поля; у rev >= 2 есть 64-битные X_варианты
     * (X_PM1a_CNT @164, X_PM_TMR @188) — применяются, если legacy = 0/0xFFFFFFFF
     * (часть прошивок заполняет только их). */
    u32 pm1a = sdt_u32(t, 64, len);
    u32 pm1b = sdt_u32(t, 68, len);
    u32 pmtmr = sdt_u32(t, 76, len);
    u8  pmtmr_len = sdt_u8(t, 91, len);

    if (out->fadt_revision >= 2u) {
        u64 x_pm1a = sdt_u64(t, 164, len);
        u64 x_pmtmr = sdt_u64(t, 188, len);
        if (pm1a == 0u || pm1a == 0xFFFFFFFFu) {
            pm1a = (x_pm1a != 0u && x_pm1a <= 0xFFFFull) ? (u32)x_pm1a : 0u;
        }
        if (pmtmr == 0u || pmtmr == 0xFFFFFFFFu) {
            pmtmr = (x_pmtmr != 0u && x_pmtmr <= 0xFFFFull) ? (u32)x_pmtmr : 0u;
        }
    }

    out->pm1a_cnt_port = (pm1a != 0u && pm1a < 0x10000u) ? (u16)pm1a : 0u;
    out->pm1b_cnt_port = (pm1b != 0u && pm1b < 0x10000u) ? (u16)pm1b : 0u;
    /* PM_TMR_LEN обязан быть 3 (24-бит) или 4 (32-бит), иначе блок непригоден
     * (ACPI 6.5, §4.8.4): неожиданная длина -> таймер не используется. */
    out->pm_timer_len = (pmtmr_len == 3u || pmtmr_len == 4u) ? pmtmr_len : 0u;
    out->pm_timer_port = (pmtmr != 0u && pmtmr < 0x10000u && out->pm_timer_len != 0u)
                         ? (u16)pmtmr : 0u;

    /* RESET_REG (GAS @116) + RESET_VALUE (@128) — с ACPI 2.0 (FADT >= 129 байт). */
    if (out->fadt_revision >= 2u && len >= 129u) {
        struct acpi_gas gas;
        memcpy(&gas, t + 116, sizeof(gas));
        out->reset_reg_space = gas.address_space;
        out->reset_reg_addr = gas.address;
        out->reset_value = sdt_u8(t, 128, len);
        out->reset_reg_present = (gas.address != 0u) ? 1 : 0;
    }
}

/* ------------------------------------------------------------------ */
/* Разбор MADT ("APIC")                                                */
/* ------------------------------------------------------------------ */

static void parse_madt(const u8 *t, u32 len, struct acpi_info *out)
{
    out->madt_lapic_paddr = sdt_u32(t, 36, len);
    out->madt_flags = sdt_u32(t, 40, len);

    u32 off = 44u;
    while (off + 2u <= len) {
        u8 type = t[off];
        u8 elen = t[off + 1u];
        if (elen < 2u || off + elen > len) {
            klog(KLOG_WARN, ACPI_TAG,
                 "MADT: повреждённая запись type=%u len=%u на смещении %u — обход прерван",
                 (unsigned int)type, (unsigned int)elen, (unsigned int)off);
            return;
        }

        switch (type) {
        case 0: {   /* Processor Local APIC (len 8) */
            if (elen < 8u) {
                break;
            }
            u32 flags = sdt_u32(t, off + 4u, len);
            if ((flags & 1u) != 0u || (flags & 2u) != 0u) {
                if (out->madt_lapic_count < ACPI_LAPIC_MAX) {
                    struct acpi_lapic *l = &out->lapics[out->madt_lapic_count];
                    l->id = t[off + 3u];
                    l->acpi_uid = t[off + 2u];
                    l->flags = flags;
                }
                out->madt_lapic_count++;
                klog(KLOG_INFO, ACPI_TAG,
                     "  LAPIC id=%u uid=%u flags=0x%x%s",
                     (unsigned int)t[off + 3u], (unsigned int)t[off + 2u],
                     (unsigned int)flags,
                     (out->madt_lapic_count > ACPI_LAPIC_MAX) ? " (лимит массива)" : "");
            }
            break;
        }
        case 1: {   /* I/O APIC (len 12) */
            if (elen < 12u) {
                break;
            }
            u32 addr = sdt_u32(t, off + 4u, len);
            u32 base = sdt_u32(t, off + 8u, len);
            if (out->ioapic_count < ACPI_IOAPIC_MAX) {
                struct acpi_ioapic *io = &out->ioapics[out->ioapic_count];
                io->id = t[off + 2u];
                io->paddr = addr;
                io->gsi_base = base;
            }
            out->ioapic_count++;
            klog(KLOG_INFO, ACPI_TAG,
                 "  IOAPIC id=%u addr=0x%x gsi_base=%u%s",
                 (unsigned int)t[off + 2u], (unsigned int)addr, (unsigned int)base,
                 (out->ioapic_count > ACPI_IOAPIC_MAX) ? " (лимит массива)" : "");
            break;
        }
        case 2: {   /* Interrupt Source Override (len 10) */
            if (elen < 10u) {
                break;
            }
            u8 src = t[off + 3u];
            u32 gsi = sdt_u32(t, off + 4u, len);
            u16 flags = sdt_u16(t, off + 8u, len);
            /* Полярность биты [1:0], триггер биты [3:2]; 0 = соответствие
             * (для ISA: active high / edge), 3 = явное значение. */
            u8 pol = ((flags & 3u) == 3u) ? ACPI_POL_ACTIVE_LOW : ACPI_POL_ACTIVE_HIGH;
            u8 trig = (((flags >> 2) & 3u) == 3u) ? ACPI_TRIG_LEVEL : ACPI_TRIG_EDGE;
            if (out->override_count < ACPI_OVERRIDE_MAX) {
                struct acpi_gsi_override *ov = &out->overrides[out->override_count];
                ov->isa_irq = src;
                ov->gsi = gsi;
                ov->pol = pol;
                ov->trig = trig;
            }
            out->override_count++;
            klog(KLOG_INFO, ACPI_TAG,
                 "  OVERRIDE isa_irq=%u -> gsi=%u pol=%s trig=%s",
                 (unsigned int)src, (unsigned int)gsi,
                 (pol == ACPI_POL_ACTIVE_LOW) ? "low" : "high",
                 (trig == ACPI_TRIG_LEVEL) ? "level" : "edge");
            break;
        }
        case 4:     /* Local APIC NMI (len 6) — логируем для дампа */
            if (elen >= 6u) {
                klog(KLOG_INFO, ACPI_TAG,
                     "  LAPIC_NMI cpu=%u lint=%u flags=0x%x",
                     (unsigned int)t[off + 2u], (unsigned int)(t[off + 5u] & 1u),
                     (unsigned int)sdt_u16(t, off + 4u, len));
            }
            break;
        case 5: {   /* Local APIC Address Override (len 12) */
            if (elen >= 12u) {
                out->madt_lapic_paddr = sdt_u64(t, off + 4u, len);
                klog(KLOG_INFO, ACPI_TAG,
                     "  LAPIC_OVERRIDE addr=0x%llx",
                     (unsigned long long)out->madt_lapic_paddr);
            }
            break;
        }
        case 9: {   /* Processor Local x2APIC (len 24) */
            if (elen < 16u) {
                break;
            }
            u32 flags = sdt_u32(t, off + 12u, len);
            if ((flags & 1u) != 0u) {
                if (out->madt_lapic_count < ACPI_LAPIC_MAX) {
                    struct acpi_lapic *l = &out->lapics[out->madt_lapic_count];
                    l->id = sdt_u32(t, off + 8u, len);
                    l->acpi_uid = sdt_u32(t, off + 4u, len);
                    l->flags = flags;
                }
                out->madt_lapic_count++;
                klog(KLOG_INFO, ACPI_TAG,
                     "  X2APIC id=%u uid=%u flags=0x%x",
                     (unsigned int)sdt_u32(t, off + 8u, len),
                     (unsigned int)sdt_u32(t, off + 4u, len),
                     (unsigned int)flags);
            }
            break;
        }
        default:    /* неизвестные типы пропускаются по elen (forward-compat) */
            klog(KLOG_DEBUG, ACPI_TAG, "  MADT type=%u len=%u (пропущен)",
                 (unsigned int)type, (unsigned int)elen);
            break;
        }
        off += elen;
    }
}

/* ------------------------------------------------------------------ */
/* Ядро парсера: RSDP -> XSDT/RSDT -> дескрипторы                      */
/* ------------------------------------------------------------------ */

err_t acpi_parse_rsdp(const void *rsdp, struct acpi_info *out)
{
    if (rsdp == NULL || out == NULL) {
        return K_EINVAL;
    }
    memset(out, 0, sizeof(*out));

    const struct acpi_rsdp *rp = (const struct acpi_rsdp *)rsdp;
    if (rsdp_valid(rp) == 0) {
        klog(KLOG_WARN, ACPI_TAG, "RSDP не прошёл валидацию (сигнатура/суммы)");
        return K_EINVAL;
    }

    out->present = 1;
    out->rsdp_revision = rp->revision;
    memcpy(out->oem_id, rp->oem_id, 6);
    out->oem_id[6] = '\0';

    const int use_xsdt = (rp->revision >= 2u && rp->xsdt_phys != 0u);
    const u64 root_phys = use_xsdt ? rp->xsdt_phys : (u64)rp->rsdt_phys;

    klog(KLOG_INFO, ACPI_TAG,
         "RSDP rev=%u OEM=\"%s\" root=%s @0x%llx",
         (unsigned int)rp->revision, out->oem_id,
         use_xsdt ? "XSDT" : "RSDT", (unsigned long long)root_phys);

    const struct acpi_sdt_header *root = (const struct acpi_sdt_header *)tbl_p2v(root_phys, PAGE_SIZE);
    if (root == NULL) {
        out->present = 0;
        return K_EIO;
    }
    if (sdt_header_valid(root, use_xsdt ? "XSDT" : "RSDT") == 0) {
        klog(KLOG_WARN, ACPI_TAG, "корневая таблица не прошла валидацию");
        out->present = 0;
        return K_EIO;
    }
    out->acpi_version = use_xsdt ? 2u : 1u;

    /* Полный размер корневой таблицы (энтри могут лежать за первой страницей). */
    if (tbl_p2v(root_phys, root->length) == NULL) {
        out->present = 0;
        return K_EIO;
    }

    const u32 rlen = root->length;
    const u32 entry_size = use_xsdt ? 8u : 4u;
    const u32 entry_count = (rlen - 36u) / entry_size;

    for (u32 i = 0; i < entry_count; i++) {
        u64 child_phys;
        if (use_xsdt) {
            child_phys = sdt_u64((const u8 *)root, 36u + i * 8u, rlen);
        } else {
            child_phys = sdt_u32((const u8 *)root, 36u + i * 4u, rlen);
        }
        if (child_phys == 0u) {
            continue;
        }

        /* Заголовок ребёнка: валидируем до разбора. */
        const struct acpi_sdt_header *h =
            (const struct acpi_sdt_header *)tbl_p2v(child_phys, PAGE_SIZE);
        if (h == NULL) {
            klog(KLOG_WARN, ACPI_TAG, "дескриптор #%u @0x%llx недоступен",
                 (unsigned int)i, (unsigned long long)child_phys);
            continue;
        }
        if (sdt_header_valid(h, NULL) == 0) {
            char sig[5];
            memcpy(sig, h->signature, 4);
            sig[4] = '\0';
            klog(KLOG_WARN, ACPI_TAG, "дескриптор %s @0x%llx не прошёл валидацию (пропущен)",
                 sig, (unsigned long long)child_phys);
            continue;
        }
        /* Полный размер дескриптора — до разбора полей за заголовком. */
        if (tbl_p2v(child_phys, h->length) == NULL) {
            klog(KLOG_WARN, ACPI_TAG, "дескриптор #%u @0x%llx недомаппился целиком",
                 (unsigned int)i, (unsigned long long)child_phys);
            continue;
        }

        char sig[5];
        memcpy(sig, h->signature, 4);
        sig[4] = '\0';
        klog(KLOG_INFO, ACPI_TAG,
             "  %s rev=%u len=%u OEM=\"%s\"",
             sig, (unsigned int)h->revision, (unsigned int)h->length, h->oem_id);

        if (memcmp(sig, "FACP", 4) == 0) {
            parse_fadt((const u8 *)h, h->length, out);
        } else if (memcmp(sig, "APIC", 4) == 0) {
            parse_madt((const u8 *)h, h->length, out);
        }
    }

    if (out->fadt_present != 0) {
        klog(KLOG_INFO, ACPI_TAG,
             "FADT: sci=%u smi_cmd=0x%x pm1a_cnt=0x%x pm_tmr=0x%x len=%u "
             "reset=%s:0x%llx<-0x%02x flags=0x%x%s",
             (unsigned int)out->sci_irq, (unsigned int)out->smi_cmd,
             (unsigned int)out->pm1a_cnt_port, (unsigned int)out->pm_timer_port,
             (unsigned int)out->pm_timer_len,
             (out->reset_reg_space == 1u) ? "IO" : "MEM",
             (unsigned long long)out->reset_reg_addr,
             (unsigned int)out->reset_value,
             (unsigned int)out->fadt_flags,
             out->hw_reduced ? " (hw-reduced)" : "");
        if (out->smi_cmd != 0u) {
            /* Переход в ACPI-режим через SMI_CMD не реализован: он нужен
             * только BIOS-загрузке (UEFI оставляет ACPI уже включённым). */
            klog(KLOG_WARN, ACPI_TAG,
                 "SMI_CMD=0x%x: включение ACPI-режима не реализовано "
                 "(актуально только для BIOS-пути загрузки)",
                 (unsigned int)out->smi_cmd);
        }
    } else {
        klog(KLOG_WARN, ACPI_TAG, "FADT не найден в корневой таблице");
    }

    if (out->ioapic_count == 0u) {
        klog(KLOG_WARN, ACPI_TAG, "MADT не содержит I/O APIC — маршрутизация останется на PIC");
    }

    return K_OK;
}

const struct acpi_info *acpi_get_info(void)
{
    return &g_acpi;
}

err_t acpi_isa_irq_gsi(u8 isa_irq, u32 *out_gsi, u8 *out_pol, u8 *out_trig)
{
    if (out_gsi == NULL || out_pol == NULL || out_trig == NULL) {
        return K_EINVAL;
    }
    /* ISA-семантика по умолчанию; перекрывается явными override из MADT. */
    u32 gsi = isa_irq;
    u8 pol = ACPI_POL_ACTIVE_HIGH;
    u8 trig = ACPI_TRIG_EDGE;

    for (u32 i = 0; i < g_acpi.override_count && i < ACPI_OVERRIDE_MAX; i++) {
        if (g_acpi.overrides[i].isa_irq == isa_irq) {
            gsi = g_acpi.overrides[i].gsi;
            pol = g_acpi.overrides[i].pol;
            trig = g_acpi.overrides[i].trig;
            break;
        }
    }
    *out_gsi = gsi;
    *out_pol = pol;
    *out_trig = trig;
    return K_OK;
}

/* ------------------------------------------------------------------ */
/* Поиск RSDP (только ядро: нужны boot_info и физическая память)       */
/* ------------------------------------------------------------------ */

#ifndef FOBOS_HOST_TEST

/* RSDP в EFI Configuration Table. Приоритет ACPI 2.0 GUID; ACPI 1.0 —
 * fallback. Таблица прошивки валидируется по сигнатуре до обхода. */
static const struct acpi_rsdp *rsdp_from_efi(const struct fbpi_boot_info *bi)
{
    if (bi->efi_system_table_phys == 0u || bi->efi_system_table_phys >= FOUR_GIB) {
        return NULL;
    }
    const struct efi_system_table *st =
        (const struct efi_system_table *)(u64)bi->efi_system_table_phys;
    if (st->hdr.signature != EFI_ST_SIGNATURE) {
        klog(KLOG_WARN, ACPI_TAG,
             "EFI System Table @0x%llx: сигнатура не совпала — fallback на скан",
             (unsigned long long)bi->efi_system_table_phys);
        return NULL;
    }

    if (st->configuration_table == 0u || st->configuration_table >= FOUR_GIB) {
        klog(KLOG_WARN, ACPI_TAG,
             "EFI Configuration Table @0x%llx вне identity-окна — fallback на скан",
             (unsigned long long)st->configuration_table);
        return NULL;
    }

    const struct efi_config_entry *entries =
        (const struct efi_config_entry *)(u64)st->configuration_table;
    const struct acpi_rsdp *acpi10 = NULL;

    u64 count = st->number_of_table_entries;
    if (count > EFI_CONFIG_TABLE_MAX) {
        klog(KLOG_WARN, ACPI_TAG,
             "EFI: аномальное число энтри Configuration Table (%llu) — ограничено %u",
             (unsigned long long)count, (unsigned int)EFI_CONFIG_TABLE_MAX);
        count = EFI_CONFIG_TABLE_MAX;
    }

    for (u64 i = 0; i < count; i++) {
        const struct efi_config_entry *e = &entries[i];
        if (e->guid_hi == EFI_ACPI20_GUID_HI && e->guid_lo == EFI_ACPI20_GUID_LO) {
            return (const struct acpi_rsdp *)(u64)e->vendor_table;
        }
        if (e->guid_hi == EFI_ACPI10_GUID_HI && e->guid_lo == EFI_ACPI10_GUID_LO) {
            acpi10 = (const struct acpi_rsdp *)(u64)e->vendor_table;
        }
    }
    return acpi10;
}

/* Fallback для нестандартных прошивок: скан EBDA (сегмент из BDA 0x40E)
 * и области BIOS [0xE0000, 0x100000) на 16-байтовом выравнивании. */
static const struct acpi_rsdp *rsdp_scan_low(void)
{
    const u16 ebda_seg = *(const volatile u16 *)0x40Eu;
    const u64 ebda_start = (u64)ebda_seg << 4;
    const u64 ebda_end = (ebda_start != 0u) ? ebda_start + 0x800u : 0u;

    struct { u64 start, end; } ranges[2] = {
        { ebda_start, ebda_end },
        { 0xE0000ull, 0x100000ull }
    };

    for (u32 r = 0; r < 2u; r++) {
        for (u64 a = ranges[r].start; a + 20u <= ranges[r].end; a += 16u) {
            const struct acpi_rsdp *rp = (const struct acpi_rsdp *)(u64)a;
            if (memcmp(rp->signature, "RSD PTR ", 8) == 0 && rsdp_valid(rp) != 0) {
                klog(KLOG_INFO, ACPI_TAG, "RSDP найден сканом @0x%llx",
                     (unsigned long long)a);
                return rp;
            }
        }
    }
    return NULL;
}

err_t acpi_init(const struct fbpi_boot_info *bi)
{
    memset(&g_acpi, 0, sizeof(g_acpi));

    const struct acpi_rsdp *rp = (bi != NULL) ? rsdp_from_efi(bi) : NULL;
    if (rp == NULL) {
        rp = rsdp_scan_low();
    }
    if (rp == NULL) {
        klog(KLOG_WARN, ACPI_TAG,
             "ACPI: RSDP не найден (EFI Configuration Table + скан) — "
             "legacy-пути: PIC/PIT, reboot 8042, poweroff по QEMU-портам");
        return K_ENODEV;
    }

    err_t rc = acpi_parse_rsdp(rp, &g_acpi);
    if (rc != K_OK) {
        memset(&g_acpi, 0, sizeof(g_acpi));
        klog(KLOG_WARN, ACPI_TAG, "ACPI: разбор не удался (%s) — legacy-пути",
             k_strerror(rc));
        return rc;
    }
    return K_OK;
}

#endif /* !FOBOS_HOST_TEST */
