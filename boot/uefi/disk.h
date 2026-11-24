/*
 * Fobos OS — перечисление дисков и разделов в загрузчике (установщик).
 *
 * Источники данных: EFI_BLOCK_IO_PROTOCOL (LocateHandleBuffer) — целые
 * диски (logical_partition == 0) и разделы (logical_partition == 1);
 * таблица разделов читается из MBR диска (common/mbr.c). Родительский
 * диск раздела определяется сравнением Device Path (путь раздела — путь
 * диска с дочерним узлом, UEFI 2.10, раздел 10).
 *
 * Диск, с которого загрузился загрузчик (device_handle образа), помечается
 * is_boot: установщик запрещает целиться в носитель, с которого работает.
 */
#ifndef FOBOS_UEFI_DISK_H
#define FOBOS_UEFI_DISK_H

#include "efi.h"

#include "../../common/mbr.h"

#define DISK_MAX_DISKS 8u
/* MBR не может описывать больше 4 разделов — ёмкость задана форматом. */
#define DISK_MAX_PARTS MBR_ENTRY_COUNT

/* Требование к геометрии: работают только диски с 512-байтным сектором
 * (все диски QEMU/CI и массовые SATA/NVMe; исключение честно сообщается). */
#define DISK_SECTOR_SZ 512u

struct disk_partition {
    u8  type;      /* тип MBR (0xEF/0x0C/...) */
    u8  boot;      /* active-флаг */
    u64 lba;
    u64 sectors;
};

struct disk_info {
    EFI_HANDLE bio_handle;     /* BlockIO целого диска */
    u32 media_id;
    u64 sectors;               /* ёмкость, секторы по 512 Б */
    int is_boot;               /* диск загрузочного носителя */
    u8  part_count;            /* занятых записей MBR */
    struct disk_partition parts[DISK_MAX_PARTS];
};

struct disk_list {
    u8 count;
    u8 boot_index;             /* индекс загрузочного диска или 0xFF */
    struct disk_info disks[DISK_MAX_DISKS];
};

/* Перечисляет диски. 0 = успех (в т.ч. "дисков нет": count=0);
 * -1 = критическая ошибка (нет BlockIO/DevicePath у образа).
 * Результат кэшируется: повторные вызовы возвращают копию первого
 * перечисления (прошивка не перечисляет диски на лету). */
int disk_enumerate(struct disk_list *out);

/* Кэшированный список (после disk_enumerate). NULL до первого вызова. */
const struct disk_list *disk_cached(void);

#endif /* FOBOS_UEFI_DISK_H */
