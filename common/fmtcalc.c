/*
 * Fobos OS — расчёт геометрии томов (реализация, см. common/fmtcalc.h).
 */
#include "fmtcalc.h"

#include <stddef.h>

u64 fmtcalc_align_up(u64 value, u64 align)
{
    if (align == 0u) {
        return value;
    }
    u64 rem = value % align;
    return (rem == 0u) ? value : value + (align - rem);
}

err_t fmtcalc_fat32_geom(u64 part_sectors, struct fmtcalc_fat32 *out)
{
    if (out == NULL) {
        return K_EINVAL;
    }
    if (part_sectors < FMTCALC_RESERVED + FMTCALC_NUM_FATS) {
        return K_EINVAL;
    }

    /* Итеративный расчёт размера FAT (та же неподвижная точка, что в
     * mkimg.py: fatsz от размера data-области, data от fatsz). */
    for (u32 spc = 1u; spc <= 128u; spc *= 2u) {
        u64 data_sectors = part_sectors - FMTCALC_RESERVED;
        u32 fatsz = (u32)(((data_sectors / (u64)spc) * 4u + FMTCALC_SECTOR_SZ - 1u) /
                          FMTCALC_SECTOR_SZ) + 1u;

        int converged = 0;
        for (u32 iter = 0; iter < 32u; iter++) {
            u64 remain = part_sectors - FMTCALC_RESERVED - FMTCALC_NUM_FATS * fatsz;
            if (remain <= (u64)spc) {
                break;   /* даже FAT не помещается — к следующему spc */
            }
            u64 clusters = remain / spc;
            u32 need = (u32)((clusters * 4u + FMTCALC_SECTOR_SZ - 1u) /
                             FMTCALC_SECTOR_SZ) + 1u;
            if (need == fatsz) {
                converged = 1;
                break;
            }
            fatsz = need;
        }
        if (converged == 0) {
            continue;
        }

        u64 remain = part_sectors - FMTCALC_RESERVED - FMTCALC_NUM_FATS * (u64)fatsz;
        u64 clusters = remain / spc;
        if (clusters < FMTCALC_MIN_CLUSTERS || clusters > FMTCALC_MAX_CLUSTERS) {
            continue;   /* spc слишком велик/мал — пробуем следующий */
        }
        if (FMTCALC_RESERVED + FMTCALC_NUM_FATS * (u64)fatsz +
            clusters * spc > part_sectors) {
            continue;   /* защита от арифметического выхода за раздел */
        }

        out->sectors_per_cluster = spc;
        out->reserved_sectors = FMTCALC_RESERVED;
        out->num_fats = FMTCALC_NUM_FATS;
        out->fatsz_sectors = fatsz;
        out->root_cluster = FMTCALC_ROOT_CLUSTER;
        out->total_clusters = (u32)clusters;
        out->free_clusters = (u32)clusters;
        return K_OK;
    }
    return K_ERANGE;   /* раздел слишком мал для FAT32 */
}
