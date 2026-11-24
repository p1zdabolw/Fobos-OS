/*
 * Fobos OS — перечисление дисков и разделов (реализация).
 */
#include "disk.h"

#include "bootctx.h"
#include "lserial.h"
#include "../../kernel/lib/vsnprintf.h"

#include <stddef.h>

static struct disk_list g_cache;
static int g_cached;

/* Число узлов Device Path (до End-узла). 0 = некорректный путь. */
static u32 dp_count(const struct EFI_DEVICE_PATH_PROTOCOL *dp)
{
    u32 n = 0;
    while (dp != NULL) {
        u16 len = dp->length;
        if (len < 4u) {
            return 0;
        }
        if (dp->type == EFI_DP_TYPE_END) {
            return n;
        }
        n++;
        if (n > 64u) {
            return 0;   /* защита от зацикленного пути прошивки */
        }
        dp = (const struct EFI_DEVICE_PATH_PROTOCOL *)
             ((const u8 *)dp + len);
    }
    return 0;
}

/* 1 = prefix — строгое начало полного пути (на 1+ узлов короче). */
static int dp_is_strict_prefix(const struct EFI_DEVICE_PATH_PROTOCOL *prefix,
                               const struct EFI_DEVICE_PATH_PROTOCOL *full)
{
    u32 np = dp_count(prefix);
    u32 nf = dp_count(full);
    if (np == 0u || nf <= np) {
        return 0;
    }
    const u8 *a = (const u8 *)prefix;
    const u8 *b = (const u8 *)full;
    for (u32 i = 0; i < np; i++) {
        u16 la = ((const struct EFI_DEVICE_PATH_PROTOCOL *)a)->length;
        u16 lb = ((const struct EFI_DEVICE_PATH_PROTOCOL *)b)->length;
        if (la != lb) {
            return 0;
        }
        for (u16 k = 0; k < la; k++) {
            if (a[k] != b[k]) {
                return 0;
            }
        }
        a += la;
        b += lb;
    }
    return 1;
}

static int get_device_path(EFI_HANDLE handle,
                           const struct EFI_DEVICE_PATH_PROTOCOL **out_dp)
{
    void *iface = NULL;
    EFI_STATUS st = bootctx_bs()->handle_protocol(handle,
        &EFI_DEVICE_PATH_PROTOCOL_GUID, &iface);
    if (EFI_ERROR(st) || iface == NULL) {
        return -1;
    }
    *out_dp = (const struct EFI_DEVICE_PATH_PROTOCOL *)iface;
    return 0;
}

int disk_enumerate(struct disk_list *out)
{
    if (out == NULL) {
        return -1;
    }
    if (g_cached != 0) {
        *out = g_cache;
        return 0;
    }

    for (u32 i = 0; i < sizeof(g_cache); i++) {
        ((u8 *)&g_cache)[i] = 0u;
    }
    g_cache.boot_index = 0xFFu;

    struct EFI_BOOT_SERVICES *bs = bootctx_bs();

    /* Device Path загрузочного раздела (образ установлен прошивкой
     * с ESP; он дочерний для целого диска). */
    struct EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    const struct EFI_DEVICE_PATH_PROTOCOL *boot_dp = NULL;
    EFI_STATUS st = bs->handle_protocol(bootctx_image(),
        &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&li);
    if (EFI_ERROR(st) || li == NULL ||
        get_device_path(li->device_handle, &boot_dp) != 0) {
        return -1;
    }

    EFI_HANDLE *handles = NULL;
    u64 n_handles = 0;
    st = bs->locate_handle_buffer(2u /* ByProtocol */, &EFI_BLOCK_IO_PROTOCOL_GUID,
                                  NULL, &n_handles, &handles);
    if (EFI_ERROR(st) || handles == NULL) {
        lserial_puts("[loader] disk: locate_handle_buffer failed\n");
        return -1;
    }

    /* Прохождение 1: целые диски. */
    for (u64 i = 0; i < n_handles && g_cache.count < DISK_MAX_DISKS; i++) {
        struct EFI_BLOCK_IO_PROTOCOL *bio = NULL;
        st = bs->handle_protocol(handles[i], &EFI_BLOCK_IO_PROTOCOL_GUID,
                                 (void **)&bio);
        if (EFI_ERROR(st) || bio == NULL || bio->media == NULL) {
            continue;
        }
        if (bio->media->logical_partition != 0u ||
            bio->media->media_present == 0 ||
            bio->media->block_size != DISK_SECTOR_SZ) {
            continue;   /* разделы, отсутствующие носители, не-512 секторы */
        }

        struct disk_info *d = &g_cache.disks[g_cache.count];
        d->bio_handle = handles[i];
        d->media_id = bio->media->media_id;
        d->sectors = bio->media->last_block + 1u;

        const struct EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
        if (get_device_path(handles[i], &dp) == 0 &&
            dp_is_strict_prefix(dp, boot_dp)) {
            d->is_boot = 1;
            g_cache.boot_index = g_cache.count;
        }

        /* MBR: разделы целого диска (для загрузочного диска тоже —
         * установщик показывает их в информационных целях). */
        u8 sec[DISK_SECTOR_SZ];
        st = bio->read_blocks(bio, d->media_id, 0u, DISK_SECTOR_SZ, sec);
        if (!EFI_ERROR(st)) {
            struct mbr_part parts[MBR_ENTRY_COUNT];
            int n = mbr_parse(sec, parts);
            for (u32 p = 0; p < MBR_ENTRY_COUNT && n > 0; p++) {
                if (parts[p].type == MBR_PART_EMPTY || parts[p].sectors == 0u) {
                    continue;
                }
                d->parts[d->part_count].type = parts[p].type;
                d->parts[d->part_count].boot = parts[p].boot;
                d->parts[d->part_count].lba = parts[p].lba;
                d->parts[d->part_count].sectors = parts[p].sectors;
                d->part_count++;
            }
        }
        g_cache.count++;
    }

    bs->free_pool(handles);

    lserial_puts("[loader] disk: enumerated disks=");
    lserial_put_hex(g_cache.count);
    lserial_puts("\n");

    g_cached = 1;
    *out = g_cache;
    return 0;
}

const struct disk_list *disk_cached(void)
{
    return (g_cached != 0) ? &g_cache : NULL;
}
