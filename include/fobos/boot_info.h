/*
 * Fobos OS — FBPI (Fobos Boot Protocol Interface), версия 0.1.
 *
 * Контракт между UEFI-загрузчиком (boot/uefi) и ядром (kernel/).
 * Единственный канал передачи состояния машины на момент передачи управления:
 * карта памяти EFI, framebuffer GOP, размещение ядра, указатели на таблицы EFI,
 * блоб настроек системы (foboscfg, создан OOBE в загрузчике).
 *
 * Инварианты ABI:
 *  - структура fbpi_boot_info имеет фиксированный размер 192 байта (см. assert ниже);
 *  - все указатели в структуре — ФИЗИЧЕСКИЕ адреса ниже 4 ГиБ (см. docs/boot-protocol.md);
 *  - целостность защищена суммой-с-дополнением (checksum) по всем словам заголовка.
 *
 * Версионирование: при изменении структуры увеличить FBPI_VERSION и
 * задокументировать диффы в docs/boot-protocol.md.
 */
#ifndef FOBOS_BOOT_INFO_H
#define FOBOS_BOOT_INFO_H

#include <fobos/types.h>
#include <fobos/errors.h>

#define FBPI_MAGIC       0x31504246u   /* "FBP1" little-endian */
#define FBPI_VERSION     1u            /* v0.1: + cfg_phys/cfg_size/boot_flags */
#define FBPI_VENDOR_MAX  64u

/* Биты bi->boot_flags. */
#define FBPI_BOOTF_MASK        0x00000003u
#define FBPI_BOOTF_CONSOLE     0x00000001u  /* загрузчик: запрос консоли ядра (Shift в boot manager) */
#define FBPI_BOOTF_GUI         0x00000002u  /* fobos.cfg: рабочий стол по умолчанию в GUI-режиме */
#define FBPI_BOOTF_CFG_MAX     0x00010000u  /* максимум cfg_size, проверяется в fbpi_validate */

/* Форматы пикселей. Значения совпадают с EFI_GRAPHICS_OUTPUT_PIXEL_FORMAT (UEFI 2.10). */
#define FBPI_PF_RGBX     0u   /* байты: R,G,B,Rsvd */
#define FBPI_PF_BGRX     1u   /* байты: B,G,R,Rsvd */
#define FBPI_PF_BITMASK  2u   /* маски в mask_red/green/blue */
#define FBPI_PF_BLTONLY  3u   /* прямой доступ невозможен — загрузчик отвергает такой режим */

/* Типы памяти EFI_MEMORY_TYPE (UEFI 2.10, таблица 7.1). */
#define FBPI_EFI_RESERVED              0u
#define FBPI_EFI_LOADER_CODE           1u
#define FBPI_EFI_LOADER_DATA           2u
#define FBPI_EFI_BOOT_SERVICES_CODE    3u
#define FBPI_EFI_BOOT_SERVICES_DATA    4u
#define FBPI_EFI_RUNTIME_SERVICES_CODE 5u
#define FBPI_EFI_RUNTIME_SERVICES_DATA 6u
#define FBPI_EFI_CONVENTIONAL          7u   /* единственный тип, считающийся свободной RAM */
#define FBPI_EFI_UNUSABLE              8u
#define FBPI_EFI_ACPI_RECLAIM          9u
#define FBPI_EFI_ACPI_NVS             10u
#define FBPI_EFI_MMIO                 11u
#define FBPI_EFI_MMIO_PORT            12u
#define FBPI_EFI_PAL_CODE             13u

/* Дескриптор карты памяти EFI (UEFI 2.10, EFI_MEMORY_DESCRIPTOR, 40 байт).
 * Прошивка вправе возвращать desc_size > 40 (с хвостовыми полями) — шаг
 * обхода задаётся bi->efi_desc_size, раскладка первых 40 байт фиксирована спецификацией. */
struct fbpi_efi_memdesc {
    u32 type;             /* FBPI_EFI_* */
    u32 pad;
    u64 physical_start;
    u64 virtual_start;
    u64 number_of_pages;  /* размер в страницах 4 КиБ */
    u64 attribute;        /* EFI_MEMORY_* атрибуты (на FND-1 не используются) */
};

struct fbpi_framebuffer {
    u64 phys_addr;        /* база линейного framebuffer (physical) */
    u64 pitch_bytes;      /* шаг строки в байтах */
    u32 width;            /* пикселей по горизонтали */
    u32 height;           /* пикселей по вертикали */
    u8  bpp;              /* бит на пиксель (ожидается 32) */
    u8  pixel_format;     /* FBPI_PF_* */
    u8  reserved[2];
    u32 mask_red;         /* актуальны при FBPI_PF_BITMASK */
    u32 mask_green;
    u32 mask_blue;
};

struct fbpi_boot_info {
    u32 magic;                       /* FBPI_MAGIC */
    u16 version;                     /* FBPI_VERSION */
    u16 header_size;                 /* sizeof(struct fbpi_boot_info) == 176 */
    u32 checksum;                    /* дополнение суммы u32-слов до нуля (поле checksum при расчёте = 0) */
    u32 kernel_size_bytes;           /* полный span образа ядра (все PT_LOAD) */
    u32 reserved0;
    u64 kernel_vbase;                /* виртуальная база ядра (higher-half) */
    u64 kernel_pbase;                /* физическая база ядра */
    u64 efi_mmap_phys;               /* физический адрес буфера карты памяти EFI */
    u32 efi_mmap_size;               /* байт данных карты (не размер буфера) */
    u32 efi_desc_size;               /* шаг дескриптора (>= 40) */
    u32 efi_desc_version;            /* версия дескриптора от прошивки */
    u32 reserved1;
    u64 efi_system_table_phys;       /* физ. адрес EFI System Table — только для FND-2
                                        (SetVirtualAddressMap); на FND-1 ядро НЕ разыменовывает */
    u64 cfg_phys;                    /* физ. адрес блога настроек foboscfg (0 = нет);
                                        память EfiLoaderData, ниже 4 ГиБ, время жизни = ядру */
    u32 cfg_size;                    /* размер блога (0, если cfg_phys == 0) */
    u32 boot_flags;                  /* FBPI_BOOTF_* (резерв) */
    struct fbpi_framebuffer fb;
    char firmware_vendor[FBPI_VENDOR_MAX]; /* NUL-terminated, скопирован загрузчиком */
};

/* Свертка заголовка: вызывать при заполнении ПОСЛЕ всех полей, checksum игнорируется. */
u32 fbpi_compute_checksum(const struct fbpi_boot_info *bi);

/* Преобразует 0x00RRGGBB в нативный пиксель формата framebuffer.
 * Единый модуль для ядра (drivers/fb.c) и загрузчика (lgfx).
 * Для FBPI_PF_BLTONLY возвращает 0 (прямой доступ невозможен). */
u32 fbpi_pixel_rgb(const struct fbpi_framebuffer *fb, u32 rgb);

/* Валидация магии, версии, размера, checksum и здравомыслия полей.
 * Возвращает K_OK либо K_EINVAL; аварийных выходов нет. */
err_t fbpi_validate(const struct fbpi_boot_info *bi);

#endif /* FOBOS_BOOT_INFO_H */
