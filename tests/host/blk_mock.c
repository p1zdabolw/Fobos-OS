/*
 * Fobos OS — host-мок блочного устройства (реализация).
 */
#include "blk_mock.h"

#include "../../common/fmtfat32.h"
#include "../../kernel/fs/blk.h"
#include "../../kernel/drivers/rtc.h"

#include <stdlib.h>

#define BLKMOCK_SECTOR 512u

static u8 *g_disk;
static u64 g_sectors;

int blk_mock_init(u64 sectors)
{
    if (g_disk != NULL || sectors == 0u) {
        return -1;
    }
    g_disk = (u8 *)calloc((size_t)sectors * BLKMOCK_SECTOR, 1);
    if (g_disk == NULL) {
        return -1;
    }
    g_sectors = sectors;
    return 0;
}

void blk_mock_done(void)
{
    free(g_disk);
    g_disk = NULL;
    g_sectors = 0;
}

static err_t mock_write(void *ctx, u64 lba, u32 count, const void *buf)
{
    (void)ctx;
    if (g_disk == NULL || count == 0u ||
        lba + count > g_sectors) {
        return K_ERANGE;
    }
    u8 *dst = g_disk + (size_t)lba * BLKMOCK_SECTOR;
    const u8 *src = (const u8 *)buf;
    for (u32 i = 0; i < count * BLKMOCK_SECTOR; i++) {
        dst[i] = src[i];
    }
    return K_OK;
}

static err_t mock_read(void *ctx, u64 lba, u32 count, void *buf)
{
    (void)ctx;
    if (g_disk == NULL || count == 0u ||
        lba + count > g_sectors) {
        return K_ERANGE;
    }
    u8 *dst = (u8 *)buf;
    const u8 *src = g_disk + (size_t)lba * BLKMOCK_SECTOR;
    for (u32 i = 0; i < count * BLKMOCK_SECTOR; i++) {
        dst[i] = src[i];
    }
    return K_OK;
}

err_t blk_mock_format_fat32(void)
{
    if (g_disk == NULL) {
        return K_ESTATE;
    }
    /* Раздел занимает весь диск, кроме сектора 0: том обязан начинаться
     * с ненулевого LBA (fat_mount валидирует VBR по part_lba). */
    const struct fmtfat32_io io = { mock_write, mock_read };
    char label[11] = { 'F', 'O', 'B', 'O', 'S', 'T', 'E', 'S', 'T', ' ', ' ' };
    return fmtfat32_format(&io, NULL, 1u, g_sectors - 1u, label);
}

/* --- blk_* контракт (kernel/fs/blk.h) ------------------------------ */

err_t blk_read_sectors(u64 lba, u32 count, void *buf)
{
    return mock_read(NULL, lba, count, buf);
}

err_t blk_write_sectors(u64 lba, u32 count, const void *buf)
{
    return mock_write(NULL, lba, count, buf);
}

/* --- rtc_* контракт (fat32-драйвер ставит метки времени) ------------ */

int rtc_ready(void)
{
    return 1;
}

err_t rtc_read_time(struct rtc_time *out)
{
    if (out == NULL) {
        return K_EINVAL;
    }
    out->year = 2026;
    out->month = 2;
    out->day = 3;
    out->hour = 12;
    out->minute = 34;
    out->second = 56;
    out->century = 0;
    return K_OK;
}
