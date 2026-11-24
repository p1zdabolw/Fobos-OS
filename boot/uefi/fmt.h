/*
 * Fobos OS — создание томов на целевом диске (установщик).
 *
 * Запись таблицы разделов MBR и форматирование FAT32 поверх привязанного
 * ublk-устройства (EFI BlockIO). Арифметика тома — общий fmtcalc
 * (совпадает с mkimg.py); сам форматтер — отдельный алгоритм СОЗДАНИЯ
 * тома (VBR/FSInfo/FAT/каталог), а не копия драйвера fat32.c, который
 * работает с уже существующим томом.
 *
 * После форматирования том монтируется штатным fat_mount — то есть
 * результат проверяется тем же кодом, каким том будет читаться.
 */
#ifndef FOBOS_UEFI_FMT_H
#define FOBOS_UEFI_FMT_H

#include "efi.h"

#include "../../common/mbr.h"

/* Пишет таблицу разделов в сектор 0 диска, привязанного к ublk.
 * K_OK / ошибка ввода-вывода. */
err_t fmt_write_mbr(const struct mbr_part entries[MBR_ENTRY_COUNT]);

/* Форматирует раздел [part_lba, part_lba+part_sectors) как FAT32.
 * label — 11 байт (пробелы до 11). Все структуры тома инициализируются:
 * VBR + резервная копия, FSInfo, обнуленные FAT с EOC-записями, чистый
 * корневой каталог. Данные вне служебных областей не затираются:
 * кластеры недостижимы при обнуленном FAT (garbage недостижим). */
err_t fmt_fat32_volume(u64 part_lba, u64 part_sectors, const char label[11]);

/* Раздел слишком мал для FAT32 (fmtcalc вернул K_ERANGE). */
#define FMT_E_TOOSMALL K_ERANGE

#endif /* FOBOS_UEFI_FMT_H */
