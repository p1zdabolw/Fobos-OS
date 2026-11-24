/*
 * Fobos OS — таблица разделов MBR (реализация, см. common/mbr.h).
 */
#include "mbr.h"

#include <stddef.h>

static u32 rd32le(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

int mbr_parse(const u8 sector[MBR_SECTOR_SZ], struct mbr_part out[MBR_ENTRY_COUNT])
{
    if (sector == NULL) {
        return -1;
    }
    if (sector[MBR_SIG_OFF] != 0x55u || sector[MBR_SIG_OFF + 1u] != 0xAAu) {
        return -1;
    }

    int count = 0;
    for (u32 i = 0; i < MBR_ENTRY_COUNT; i++) {
        const u8 *e = sector + MBR_ENTRY_OFF + i * 16u;
        u8 type = e[4];
        u64 lba = rd32le(e + 8);
        u64 n = rd32le(e + 12);

        if (out != NULL) {
            out[i].boot = e[0];
            out[i].type = type;
            out[i].lba = lba;
            out[i].sectors = n;
        }
        if (type != MBR_PART_EMPTY && n != 0u) {
            count++;
        }
    }
    return count;
}

err_t mbr_find_fat32(const u8 sector[MBR_SECTOR_SZ], u64 *out_lba,
                     u64 *out_sectors)
{
    if (sector == NULL || out_lba == NULL || out_sectors == NULL) {
        return K_EINVAL;
    }
    if (sector[MBR_SIG_OFF] != 0x55u || sector[MBR_SIG_OFF + 1u] != 0xAAu) {
        return K_EINVAL;   /* нет сигнатуры MBR */
    }
    for (u32 i = 0; i < MBR_ENTRY_COUNT; i++) {
        const u8 *e = sector + MBR_ENTRY_OFF + i * 16u;
        u8 type = e[4];
        if (type == MBR_PART_FAT32_CHS || type == MBR_PART_FAT32_LBA) {
            u64 lba = rd32le(e + 8);
            u64 n = rd32le(e + 12);
            if (n == 0u) {
                continue;
            }
            *out_lba = lba;
            *out_sectors = n;
            return K_OK;
        }
    }
    return K_ENODEV;
}

void mbr_write(u8 sector[MBR_SECTOR_SZ],
               const struct mbr_part entries[MBR_ENTRY_COUNT])
{
    if (sector == NULL || entries == NULL) {
        return;
    }
    for (u32 i = 0; i < MBR_ENTRY_COUNT; i++) {
        u8 *e = sector + MBR_ENTRY_OFF + i * 16u;
        for (u32 k = 0; k < 16u; k++) {
            e[k] = 0u;
        }
        if (entries[i].type == MBR_PART_EMPTY || entries[i].sectors == 0u) {
            continue;
        }
        e[0] = entries[i].boot;
        e[1] = MBR_CHS_DUMMY;
        e[2] = MBR_CHS_DUMMY;
        e[3] = MBR_CHS_DUMMY;
        e[4] = entries[i].type;
        e[5] = MBR_CHS_DUMMY;
        e[6] = MBR_CHS_DUMMY;
        e[7] = MBR_CHS_DUMMY;
        u32 lba = (u32)entries[i].lba;
        e[8]  = (u8)(lba & 0xFFu);
        e[9]  = (u8)((lba >> 8) & 0xFFu);
        e[10] = (u8)((lba >> 16) & 0xFFu);
        e[11] = (u8)((lba >> 24) & 0xFFu);
        u32 n = (u32)entries[i].sectors;
        e[12] = (u8)(n & 0xFFu);
        e[13] = (u8)((n >> 8) & 0xFFu);
        e[14] = (u8)((n >> 16) & 0xFFu);
        e[15] = (u8)((n >> 24) & 0xFFu);
    }
    sector[MBR_SIG_OFF] = 0x55u;
    sector[MBR_SIG_OFF + 1u] = 0xAAu;
}
