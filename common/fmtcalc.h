/*
 * Fobos OS — расчёт геометрии файловых томов (общий модуль).
 *
 * Чистая арифметика без ввода-вывода: используется форматтером установщика
 * (boot/uefi/fmt.c), моком хранилища в host-тестах и пригодна ядру.
 * Алгоритм подбора sectors_per_cluster соответствует классической схеме
 * FAT32 (требование > 65525 кластеров) и совпадает по результату с
 * scripts/mkimg.py для образа 256 МиБ (spc=1).
 */
#ifndef FOBOS_COMMON_FMTCALC_H
#define FOBOS_COMMON_FMTCALC_H

#include <fobos/types.h>
#include <fobos/errors.h>

/* Фиксированные параметры томов Fobos (совпадают с mkimg.py и с тем,
 * что валидирует fat_parse_bpb в ядре). */
#define FMTCALC_SECTOR_SZ     512u
#define FMTCALC_RESERVED      32u
#define FMTCALC_NUM_FATS      2u
#define FMTCALC_ROOT_CLUSTER  2u
#define FMTCALC_FSINFO_SECTOR 1u
#define FMTCALC_BACKUP_SECTOR 6u

/* FAT32: минимум 65526 кластеров данных; максимум записей FAT — 2^28. */
#define FMTCALC_MIN_CLUSTERS  65526u
#define FMTCALC_MAX_CLUSTERS  0x0FFFFFEFu

struct fmtcalc_fat32 {
    u32 sectors_per_cluster;
    u32 reserved_sectors;
    u32 num_fats;
    u32 fatsz_sectors;
    u32 root_cluster;
    u32 total_clusters;   /* кластеры данных (номера 2..total+1) */
    u32 free_clusters;    /* после форматирования = total_clusters */
};

/* Подбирает геометрию FAT32 для раздела размером part_sectors секторов:
 * минимальный spc из {1,2,...,128}, при котором кластеров > 65525.
 * K_OK, K_EINVAL (аргументы) или K_ERANGE (раздел слишком мал даже при
 * spc=128 — для FAT32 это < ~33 МиБ). */
err_t fmtcalc_fat32_geom(u64 part_sectors, struct fmtcalc_fat32 *out);

/* Выравнивание вверх (для стартовых LBA новых разделов, шаг 1 МиБ). */
u64 fmtcalc_align_up(u64 value, u64 align);

#endif /* FOBOS_COMMON_FMTCALC_H */
