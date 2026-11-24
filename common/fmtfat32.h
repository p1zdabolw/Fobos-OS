/*
 * Fobos OS — форматирование FAT32-тома (общий модуль, чистая логика).
 *
 * СОЗДАНИЕ тома: VBR + резервная копия, FSInfo, обнуление FAT с
 * служебными записями, чистый корневой каталог. Ввод-вывод — через
 * колбэк-интерфейс (в загрузчике это EFI BlockIO через ublk, в host-тестах
 * — RAM-диск). Геометрия — общий fmtcalc (совпадает с scripts/mkimg.py).
 *
 * Данные вне служебных областей не затираются: при обнулённом FAT все
 * кластеры свободны, старое содержимое недостижимо средствами FAT.
 */
#ifndef FOBOS_COMMON_FMTFAT32_H
#define FOBOS_COMMON_FMTFAT32_H

#include <fobos/types.h>
#include <fobos/errors.h>

/* Абсолютный LBA диска; count в секторах по 512 Б. */
struct fmtfat32_io {
    err_t (*write)(void *ctx, u64 lba, u32 count, const void *buf);
    err_t (*read)(void *ctx, u64 lba, u32 count, void *buf);
};

/* Форматирует раздел [part_lba, part_lba + part_sectors).
 * label — 11 байт (добиваются пробелами при меньшей длине).
 * K_OK; K_EINVAL; K_ERANGE (раздел слишком мал для FAT32);
 * K_EIO — ошибки ввода-вывода через io. */
err_t fmtfat32_format(const struct fmtfat32_io *io, void *ctx,
                      u64 part_lba, u64 part_sectors, const char label[11]);

#endif /* FOBOS_COMMON_FMTFAT32_H */
