/*
 * Fobos OS — форматирование FAT32 (реализация, см. common/fmtfat32.h).
 */
#include "fmtfat32.h"

#include <stddef.h>

#include "fmtcalc.h"

#define FMTFAT_SECTOR 512u

static void set_le32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

/* VBR: BPB по геометрии + сигнатура. Том не грузится через BIOS-бутсектор,
 * но сигнатура 0x55AA обязательна (её валидирует fat_parse_bpb). */
static void build_vbr(u8 *sec, const struct fmtcalc_fat32 *g,
                      u64 part_sectors, const char label[11])
{
    for (u32 i = 0; i < FMTFAT_SECTOR; i++) {
        sec[i] = 0u;
    }
    sec[0] = 0xEBu;                 /* jmp short + nop */
    sec[1] = 0x3Cu;
    sec[2] = 0x90u;
    sec[3] = 'F'; sec[4] = 'O'; sec[5] = 'B'; sec[6] = 'O';
    sec[7] = 'S'; sec[8] = 'O'; sec[9] = 'S'; sec[10] = ' ';

    sec[11] = (u8)(FMTCALC_SECTOR_SZ & 0xFFu);          /* bytes/sector */
    sec[12] = (u8)(FMTCALC_SECTOR_SZ >> 8);
    sec[13] = (u8)g->sectors_per_cluster;
    sec[14] = (u8)(g->reserved_sectors & 0xFFu);
    sec[15] = (u8)(g->reserved_sectors >> 8);
    sec[16] = (u8)g->num_fats;
    sec[17] = 0u; sec[18] = 0u;     /* root entries: FAT32 = 0 */
    sec[19] = 0u; sec[20] = 0u;     /* total16 = 0 */
    sec[21] = 0xF8u;                /* media: fixed disk */
    sec[22] = 0u; sec[23] = 0u;     /* fatsz16 = 0 (FAT32) */
    sec[24] = 63u; sec[25] = 0u;    /* sectors/track (информационно) */
    sec[26] = 255u; sec[27] = 0u;   /* heads (информационно) */
    sec[28] = 0u; sec[29] = 0u; sec[30] = 0u; sec[31] = 0u;  /* hidden */
    set_le32(sec + 32, (u32)part_sectors);
    set_le32(sec + 36, g->fatsz_sectors);
    sec[40] = 0u; sec[41] = 0u;     /* ext flags: mirroring включён */
    sec[42] = 0u; sec[43] = 0u;     /* fs version */
    set_le32(sec + 44, g->root_cluster);
    sec[48] = (u8)(FMTCALC_FSINFO_SECTOR & 0xFFu);
    sec[49] = (u8)(FMTCALC_FSINFO_SECTOR >> 8);
    sec[50] = (u8)(FMTCALC_BACKUP_SECTOR & 0xFFu);
    sec[51] = (u8)(FMTCALC_BACKUP_SECTOR >> 8);
    for (u32 i = 52u; i < 64u; i++) {
        sec[i] = 0u;                /* reserved */
    }
    sec[64] = 0x80u;                /* drive number */
    sec[65] = 0u;                   /* reserved */
    sec[66] = 0x29u;                /* extended boot signature */
    for (u32 i = 67u; i < 71u; i++) {
        sec[i] = 0u;                /* serial тома: детерминированный 0 */
    }
    for (u32 i = 0; i < 11u; i++) {
        sec[71 + i] = (u8)label[i]; /* метка тома (11 байт) */
    }
    sec[82] = 'F'; sec[83] = 'A'; sec[84] = 'T'; sec[85] = '3';
    sec[86] = '2'; sec[87] = ' '; sec[88] = ' '; sec[89] = ' ';
    sec[510] = 0x55u;
    sec[511] = 0xAAu;
}

static void build_fsinfo(u8 *sec, const struct fmtcalc_fat32 *g)
{
    for (u32 i = 0; i < FMTFAT_SECTOR; i++) {
        sec[i] = 0u;
    }
    sec[0] = 0x52u; sec[1] = 0x52u; sec[2] = 0x61u; sec[3] = 0x41u; /* RRaA */
    sec[484] = 0x72u; sec[485] = 0x72u; sec[486] = 0x41u; sec[487] = 0x61u; /* rrAa */
    set_le32(sec + 488, g->free_clusters);
    set_le32(sec + 492, FMTCALC_ROOT_CLUSTER);
    sec[508] = 0x00u; sec[509] = 0x00u; sec[510] = 0x55u; sec[511] = 0xAAu;
}

err_t fmtfat32_format(const struct fmtfat32_io *io, void *ctx,
                      u64 part_lba, u64 part_sectors, const char label[11])
{
    if (io == NULL || io->write == NULL || io->read == NULL ||
        label == NULL) {
        return K_EINVAL;
    }
    if (part_lba == 0u || part_sectors == 0u) {
        return K_EINVAL;
    }

    struct fmtcalc_fat32 g;
    err_t rc = fmtcalc_fat32_geom(part_sectors, &g);
    if (rc != K_OK) {
        return rc;   /* K_ERANGE = слишком мал для FAT32 */
    }

    /* 1. VBR + резервная копия (копия сохраняется для контроля записи). */
    u8 sec[FMTFAT_SECTOR];
    build_vbr(sec, &g, part_sectors, label);
    u8 vbr[FMTFAT_SECTOR];
    for (u32 i = 0; i < FMTFAT_SECTOR; i++) {
        vbr[i] = sec[i];
    }
    rc = io->write(ctx, part_lba + 0u, 1u, sec);
    if (rc != K_OK) {
        return rc;
    }
    rc = io->write(ctx, part_lba + FMTCALC_BACKUP_SECTOR, 1u, sec);
    if (rc != K_OK) {
        return rc;
    }

    /* 2. FSInfo. */
    build_fsinfo(sec, &g);
    rc = io->write(ctx, part_lba + FMTCALC_FSINFO_SECTOR, 1u, sec);
    if (rc != K_OK) {
        return rc;
    }

    /* 3. FAT: обнуление обеих копий; первый сектор каждой копии —
     * служебные записи 0/1 (media + EOC). */
    static u8 zero[FMTFAT_SECTOR];
    for (u32 i = 0; i < FMTFAT_SECTOR; i++) {
        zero[i] = 0u;
    }
    u8 first[FMTFAT_SECTOR];
    for (u32 i = 0; i < FMTFAT_SECTOR; i++) {
        first[i] = 0u;
    }
    first[0] = 0xF8u; first[1] = 0xFFu;
    first[2] = 0xFFu; first[3] = 0x0Fu;   /* FAT[0]: media + reserved */
    first[4] = 0xFFu; first[5] = 0xFFu;
    first[6] = 0xFFu; first[7] = 0x0Fu;   /* FAT[1]: EOC */

    for (u32 f = 0; f < g.num_fats; f++) {
        u64 fat_lba = part_lba + g.reserved_sectors +
                      (u64)f * g.fatsz_sectors;
        for (u32 s = 0; s < g.fatsz_sectors; s++) {
            rc = io->write(ctx, fat_lba + s, 1u,
                           (s == 0u) ? first : zero);
            if (rc != K_OK) {
                return rc;
            }
        }
    }

    /* 4. Корневой каталог: чистый кластер. */
    u64 root_lba = part_lba + g.reserved_sectors +
                   (u64)g.num_fats * g.fatsz_sectors;
    for (u32 s = 0; s < g.sectors_per_cluster; s++) {
        rc = io->write(ctx, root_lba + s, 1u, zero);
        if (rc != K_OK) {
            return rc;
        }
    }

    /* 5. Контроль: перечитываем VBR — IO-путь обязан отдавать то, что
     * записано (ловит моки/железо с кэшем без записи). */
    u8 verify[FMTFAT_SECTOR];
    rc = io->read(ctx, part_lba, 1u, verify);
    if (rc != K_OK) {
        return rc;
    }
    for (u32 i = 0; i < FMTFAT_SECTOR; i++) {
        if (verify[i] != vbr[i]) {
            return K_EIO;
        }
    }
    return K_OK;
}
