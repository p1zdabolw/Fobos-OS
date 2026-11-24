/*
 * Fobos OS — создание томов на целевом диске (установщик, обёртка).
 *
 * Запись таблицы разделов MBR и форматирование FAT32. Вся логика
 * форматирования — общий модуль common/fmtfat32.c (тот же код работает
 * в host-тестах поверх RAM-диска); здесь только привязка IO к ublk
 * (EFI BlockIO) и самопроверка штатным драйвером fat_mount.
 */
#include "fmt.h"

#include "ublk.h"
#include "lserial.h"

#include "../kernel/fs/blk.h"

#include "../../common/fmtfat32.h"
#include "../../common/mbr.h"

#include "../kernel/fs/fat32.h"

#define FMT_TAG "[loader] fmt: "

/* IO-адаптер: ublk работает с АБСОЛЮТНЫМ LBA — совпадает с контрактом
 * fmtfat32 (больше ничего преобразовывать не нужно). */
static err_t fmt_io_write(void *ctx, u64 lba, u32 count, const void *buf)
{
    (void)ctx;
    return blk_write_sectors(lba, count, buf);
}

static err_t fmt_io_read(void *ctx, u64 lba, u32 count, void *buf)
{
    (void)ctx;
    return blk_read_sectors(lba, count, buf);
}

static const struct fmtfat32_io FMT_IO = { fmt_io_write, fmt_io_read };

err_t fmt_write_mbr(const struct mbr_part entries[MBR_ENTRY_COUNT])
{
    u8 sec[MBR_SECTOR_SZ];
    for (u32 i = 0; i < MBR_SECTOR_SZ; i++) {
        sec[i] = 0u;   /* загрузочный код MBR не используется (UEFI-загрузка) */
    }
    mbr_write(sec, entries);
    return blk_write_sectors(0u, 1u, sec);
}

err_t fmt_fat32_volume(u64 part_lba, u64 part_sectors, const char label[11])
{
    if (ublk_bound() == 0) {
        return K_ENODEV;
    }

    /* Метка обязана быть ровно 11 байт: добиваем пробелами при меньшей. */
    char label11[11];
    u32 i = 0;
    for (; i < 11u; i++) {
        label11[i] = (label != NULL && label[i] != '\0') ? label[i] : ' ';
    }

    err_t rc = fmtfat32_format(&FMT_IO, NULL, part_lba, part_sectors, label11);
    if (rc != K_OK) {
        return rc;   /* K_ERANGE = слишком мал для FAT32 */
    }
    (void)ublk_flush();

    /* Самопроверка: том обязан смонтироваться штатным драйвером. */
    struct fat_vol probe;
    rc = fat_mount(&probe, part_lba, part_sectors);
    if (rc != K_OK) {
        lserial_puts(FMT_TAG "self-check: fat_mount failed\n");
        return rc;
    }
    lserial_puts(FMT_TAG "self-check: mount ok\n");
    return K_OK;
}
