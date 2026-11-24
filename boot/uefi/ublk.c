/*
 * Fobos OS — blk/rtc/klog-адаптер для FAT32-драйвера (реализация).
 * Семантика контракта см. в шапке ublk.h.
 */
#include "ublk.h"

#include "bootctx.h"
#include "lserial.h"

#include "../kernel/fs/blk.h"
#include "../kernel/drivers/rtc.h"
#include "../kernel/lib/log.h"
#include "../kernel/lib/vsnprintf.h"

#include <stdarg.h>

/* --- привязка устройства ------------------------------------------ */

static struct EFI_BLOCK_IO_PROTOCOL *g_ublk_bio;
static u32 g_ublk_media_id;
static struct fat_vol g_ublk_vol;

int ublk_bind(EFI_HANDLE bio_handle)
{
    void *iface = NULL;
    EFI_STATUS st = bootctx_bs()->handle_protocol(bio_handle,
        &EFI_BLOCK_IO_PROTOCOL_GUID, &iface);
    if (EFI_ERROR(st) || iface == NULL) {
        return -1;
    }
    g_ublk_bio = (struct EFI_BLOCK_IO_PROTOCOL *)iface;
    if (g_ublk_bio->media == NULL || g_ublk_bio->media->block_size != 512u) {
        g_ublk_bio = NULL;
        return -1;
    }
    g_ublk_media_id = g_ublk_bio->media->media_id;
    return 0;
}

int ublk_bound(void)
{
    return (g_ublk_bio != NULL) ? 1 : 0;
}

struct fat_vol *ublk_fat(void)
{
    return &g_ublk_vol;
}

err_t ublk_flush(void)
{
    if (g_ublk_bio == NULL) {
        return K_ENODEV;
    }
    return EFI_ERROR(g_ublk_bio->flush_blocks(g_ublk_bio)) ? K_EIO : K_OK;
}

/* --- blk_* (контракт kernel/fs/blk.h) ------------------------------ */

/* Лимит на операцию: 1 МиБ = 2048 секторов (fat32-драйвер просит по 1
 * сектору; лимит защищает от случайного переполнения buffer_size). */
#define UBLK_MAX_SECTORS 2048u

static err_t ublk_io(u64 lba, u32 count, void *buf,
                     EFI_STATUS (*op)(struct EFI_BLOCK_IO_PROTOCOL *, u32, u64,
                                      u64, void *))
{
    if (g_ublk_bio == NULL || op == NULL) {
        return K_ENODEV;
    }
    if (buf == NULL || count == 0u || count > UBLK_MAX_SECTORS) {
        return K_EINVAL;
    }
    EFI_STATUS st = op(g_ublk_bio, g_ublk_media_id, lba,
                       (u64)count * 512u, buf);
    if (EFI_ERROR(st)) {
        return K_EIO;
    }
    return K_OK;
}

err_t blk_read_sectors(u64 lba, u32 count, void *buf)
{
    if (g_ublk_bio == NULL) {
        return K_ENODEV;
    }
    /* read_blocks объявлен с void*; константность снимает приведение. */
    EFI_STATUS (*op)(struct EFI_BLOCK_IO_PROTOCOL *, u32, u64, u64, void *) =
        (EFI_STATUS (*)(struct EFI_BLOCK_IO_PROTOCOL *, u32, u64, u64, void *))
            g_ublk_bio->read_blocks;
    return ublk_io(lba, count, buf, op);
}

err_t blk_write_sectors(u64 lba, u32 count, const void *buf)
{
    if (g_ublk_bio == NULL) {
        return K_ENODEV;
    }
    EFI_STATUS (*op)(struct EFI_BLOCK_IO_PROTOCOL *, u32, u64, u64, void *) =
        (EFI_STATUS (*)(struct EFI_BLOCK_IO_PROTOCOL *, u32, u64, u64, void *))
            g_ublk_bio->write_blocks;
    return ublk_io(lba, count, (void *)(u64 *)(u64)buf, op);
}

/* Остальные функции blk.h установщиком не используются (символы не
 * линкуются) — намеренно не определены. */

/* --- rtc_* (контракт kernel/drivers/rtc.h) -------------------------- */

int rtc_ready(void)
{
    return 1;   /* EFI GetTime доступен всегда до ExitBootServices */
}

err_t rtc_read_time(struct rtc_time *out)
{
    if (out == NULL) {
        return K_EINVAL;
    }
    struct EFI_TIME t;
    /* Capabilities (2-й аргумент) по спецификации опционален. */
    EFI_STATUS st = bootctx_st()->runtime_services->get_time(&t, NULL);
    if (EFI_ERROR(st)) {
        return K_EIO;
    }
    if (t.month < 1u || t.month > 12u || t.day < 1u || t.day > 31u ||
        t.hour > 23u || t.minute > 59u || t.second > 59u || t.year < 1980u) {
        return K_EIO;   /* часы прошивки не синхронизированы */
    }
    out->year = t.year;
    out->month = t.month;
    out->day = t.day;
    out->hour = t.hour;
    out->minute = t.minute;
    out->second = t.second;
    out->century = 0;
    return K_OK;
}

/* --- klog (контракт kernel/lib/log.h) ------------------------------- */

void klog(enum klog_level lvl, const char *subsys, const char *fmt, ...)
{
    static const char *const lvl_tags[] = {
        "T", "D", "I", "W", "E", "P"
    };
    char line[256];
    char body[224];
    va_list ap;
    va_start(ap, fmt);
    (void)kvsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    const char *tag = (lvl <= KLOG_PANIC) ? lvl_tags[lvl] : "?";
    (void)ksnprintf(line, sizeof(line), "[loader] %s [%s] %s\n",
                    tag, (subsys != NULL) ? subsys : "-", body);
    lserial_puts(line);
}

void klog_raw(const char *s)
{
    lserial_puts((s != NULL) ? s : "");
}
