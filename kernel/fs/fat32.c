/*
 * Fobos OS — FAT32-том (реализация ввода-вывода).
 *
 * Все обращения к диску — через blk_read_sectors/blk_write_sectors
 * (абсолютный LBA диска). Кэш FAT — write-through: правка в кэше,
 * затем полный сектор пишется на ОБЕ копии FAT.
 *
 * Каталог — цепочка кластеров с записями 32 байта. LFN-записи
 * (attr 0x0F) собираются в длинное имя при чтении; запись (FND-2f)
 * создаёт LFN-серию + 8.3-алиас "~N" для имён вне 8.3, перезапись
 * сохраняет исходные имена. Защита от циклов в FAT — счётчик шагов
 * на каждую цепочку.
 */
#include "fat32.h"

#include "blk.h"
#include "../drivers/rtc.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include <stddef.h>

#define F_TAG "fat32"

/* ------------------------------------------------------------------ */
/* Защита системных имён                                               */
/* ------------------------------------------------------------------ */

/* Единая политика (FND-2d): загрузчик/конфиг/ядро не удаляются и не
 * перезаписываются ни explorer'ом, ни shell-командами rm/write/cp.
 * Один список — один источник истины; ранее копипастился в desktop.c. */
static const char *const FAT_PROTECTED_NAMES[] = {
    "KERNEL.ELF", "SETUP.CFG", "FOBOS.CFG", "BOOTX64.EFI", NULL
};

int fat_name_protected(const char *name)
{
    if (name == NULL) {
        return 0;
    }
    for (u32 i = 0; FAT_PROTECTED_NAMES[i] != NULL; i++) {
        const char *a = name;
        const char *b = FAT_PROTECTED_NAMES[i];
        u32 j = 0;
        int equal = 1;
        for (;;) {
            char ca = a[j];
            char cb = b[j];
            if (ca >= 'a' && ca <= 'z') {
                ca = (char)(ca - 0x20);   /* сравнение без регистра */
            }
            if (ca != cb) {
                equal = 0;
                break;
            }
            if (ca == '\0') {
                break;
            }
            j++;
        }
        if (equal != 0) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Секторные операции (LBA относительно партиции)                      */
/* ------------------------------------------------------------------ */

static err_t read_sec(struct fat_vol *v, u64 part_rel_lba, u8 *buf)
{
    if (part_rel_lba >= v->part_sectors) {
        return K_ERANGE;
    }
    return blk_read_sectors(v->part_lba + part_rel_lba, 1u, buf);
}

static err_t write_sec(struct fat_vol *v, u64 part_rel_lba, const u8 *buf)
{
    if (part_rel_lba >= v->part_sectors) {
        return K_ERANGE;
    }
    return blk_write_sectors(v->part_lba + part_rel_lba, 1u, buf);
}

/* LBA (внутри партиции) первого сектора кластера. */
static u64 clus_rel_lba(const struct fat_vol *v, u32 cluster)
{
    /* data_lba = reserved + num_fats*fatsz — LBA кластера 2 (задаётся в mount). */
    return v->data_lba + (u64)(cluster - 2u) * v->bpb.sectors_per_cluster;
}

static err_t read_cluster(struct fat_vol *v, u32 cluster, u8 *buf)
{
    u64 lba = clus_rel_lba(v, cluster);
    for (u32 s = 0; s < v->bpb.sectors_per_cluster; s++) {
        err_t rc = read_sec(v, lba + s, buf + (u32)s * FAT_SECTOR_SZ);
        if (rc != K_OK) {
            return rc;
        }
    }
    return K_OK;
}

static err_t write_cluster(struct fat_vol *v, u32 cluster, const u8 *buf)
{
    u64 lba = clus_rel_lba(v, cluster);
    for (u32 s = 0; s < v->bpb.sectors_per_cluster; s++) {
        err_t rc = write_sec(v, lba + s, buf + (u32)s * FAT_SECTOR_SZ);
        if (rc != K_OK) {
            return rc;
        }
    }
    return K_OK;
}

/* ------------------------------------------------------------------ */
/* FAT: кэш, чтение, запись                                            */
/* ------------------------------------------------------------------ */

static u32 g_cache_hand;

static u8 *fat_cache_get(struct fat_vol *v, u64 fat_rel_lba)
{
    for (u32 i = 0; i < FAT_CACHE_SECTORS; i++) {
        if (v->cache[i].valid != 0u && v->cache[i].lba == fat_rel_lba) {
            return v->cache[i].data;
        }
    }
    u32 slot = g_cache_hand % FAT_CACHE_SECTORS;
    g_cache_hand++;
    v->cache[slot].valid = 0;
    err_t rc = read_sec(v, v->bpb.reserved_sectors + fat_rel_lba,
                        v->cache[slot].data);
    if (rc != K_OK) {
        return NULL;
    }
    v->cache[slot].valid = 1;
    v->cache[slot].lba = fat_rel_lba;
    return v->cache[slot].data;
}

static err_t fat_get_next(struct fat_vol *v, u32 cluster, u32 *out_next)
{
    if (cluster < 2u || cluster >= v->total_clusters + 2u) {
        return K_EINVAL;
    }
    u32 entry_off = cluster * 4u;
    u64 sec = entry_off / FAT_SECTOR_SZ;
    u32 off = entry_off % FAT_SECTOR_SZ;
    u8 *data = fat_cache_get(v, sec);
    if (data == NULL) {
        return K_EIO;
    }
    u32 raw = (u32)data[off] | ((u32)data[off + 1u] << 8) |
              ((u32)data[off + 2u] << 16) | ((u32)data[off + 3u] << 24);
    return fat_fat_entry_value(raw, out_next);
}

/* Пишет запись FAT на обе копии (полным сектором) и в кэш. */
static err_t fat_set_next(struct fat_vol *v, u32 cluster, u32 value)
{
    if (cluster < 2u || cluster >= v->total_clusters + 2u) {
        return K_EINVAL;
    }
    u32 entry_off = cluster * 4u;
    u64 sec = entry_off / FAT_SECTOR_SZ;
    u32 off = entry_off % FAT_SECTOR_SZ;

    u8 secbuf[FAT_SECTOR_SZ];
    u8 *data = fat_cache_get(v, sec);
    int had_cache = (data != NULL);
    if (data == NULL) {
        err_t rc = read_sec(v, v->bpb.reserved_sectors + sec, secbuf);
        if (rc != K_OK) {
            return rc;
        }
        data = secbuf;
    }

    /* FAT32: старший ниббл записи сохраняется (reserved). */
    u32 raw = (value & 0x0FFFFFFFu) | ((u32)(data[off + 3u] & 0xF0u) << 24);
    data[off]     = (u8)(raw & 0xFFu);
    data[off + 1] = (u8)((raw >> 8) & 0xFFu);
    data[off + 2] = (u8)((raw >> 16) & 0xFFu);
    data[off + 3] = (u8)((raw >> 24) & 0xFFu);

    for (u32 f = 0; f < v->bpb.num_fats; f++) {
        u64 lba = v->bpb.reserved_sectors +
                  (u64)f * v->bpb.fatsz_sectors + sec;
        err_t rc = write_sec(v, lba, data);
        if (rc != K_OK) {
            return rc;
        }
    }

    if (had_cache == 0) {
        u32 slot = g_cache_hand % FAT_CACHE_SECTORS;
        g_cache_hand++;
        v->cache[slot].valid = 1;
        v->cache[slot].lba = sec;
        for (u32 i = 0; i < FAT_SECTOR_SZ; i++) {
            v->cache[slot].data[i] = secbuf[i];
        }
    }
    return K_OK;
}

/* ------------------------------------------------------------------ */
/* Аллокация/освобождение кластеров                                    */
/* ------------------------------------------------------------------ */

static err_t fat_find_free(struct fat_vol *v, u32 *out_clus)
{
    for (u32 c = 2u; c < v->total_clusters + 2u; c++) {
        u32 next = 0;
        err_t rc = fat_get_next(v, c, &next);
        if (rc != K_OK) {
            continue;
        }
        if (next == 0u) {
            *out_clus = c;
            return K_OK;
        }
    }
    return K_ENOMEM;
}

err_t fat_free_chain(struct fat_vol *v, u32 first)
{
    if (v == NULL || v->mounted == 0) {
        return K_EINVAL;
    }
    if (first < 2u || first >= v->total_clusters + 2u) {
        return K_OK;   /* пустой файл: кластера нет */
    }
    u32 c = first;
    u32 freed = 0;
    u32 guard = v->total_clusters + 2u;
    for (;;) {
        if (guard == 0u) {
            return K_EIO;   /* цикл в FAT — отказ, не зацикливаемся */
        }
        guard--;
        u32 next = 0;
        err_t rc = fat_get_next(v, c, &next);
        if (rc != K_OK) {
            return rc;
        }
        rc = fat_set_next(v, c, 0u);
        if (rc != K_OK) {
            return rc;
        }
        freed++;
        if (next == FAT_EOC) {
            break;
        }
        if (next < 2u) {
            return K_EIO;   /* указание в никуда */
        }
        c = next;
    }
    if (v->free_clusters_known != 0u) {
        v->free_clusters += freed;
    }
    return K_OK;
}

static err_t fat_alloc_chain(struct fat_vol *v, u32 n, u32 *out_first)
{
    if (n == 0u) {
        return K_EINVAL;
    }
    u32 first = 0;
    u32 prev = 0;
    for (u32 i = 0; i < n; i++) {
        u32 c = 0;
        err_t rc = fat_find_free(v, &c);
        if (rc != K_OK) {
            if (first != 0u) {
                (void)fat_free_chain(v, first);
            }
            return rc;
        }
        rc = fat_set_next(v, c, FAT_EOC);
        if (rc != K_OK) {
            (void)fat_free_chain(v, c);
            if (first != 0u) {
                (void)fat_free_chain(v, first);
            }
            return rc;
        }
        if (i == 0u) {
            first = c;
        } else {
            rc = fat_set_next(v, prev, c);
            if (rc != K_OK) {
                (void)fat_free_chain(v, first);
                return rc;
            }
        }
        prev = c;
    }
    if (v->free_clusters_known != 0u && v->free_clusters >= n) {
        v->free_clusters -= n;
    }
    *out_first = first;
    return K_OK;
}

/* ------------------------------------------------------------------ */
/* Монтирование                                                        */
/* ------------------------------------------------------------------ */

err_t fat_mount(struct fat_vol *v, u64 part_lba, u64 part_sectors)
{
    if (v == NULL || part_sectors == 0u) {
        return K_EINVAL;
    }
    for (u32 i = 0; i < sizeof(*v); i++) {
        ((u8 *)v)[i] = 0u;
    }
    v->part_lba = part_lba;
    v->part_sectors = part_sectors;

    u8 boot[FAT_SECTOR_SZ];
    err_t rc = read_sec(v, 0u, boot);
    if (rc != K_OK) {
        return rc;
    }
    rc = fat_parse_bpb(boot, sizeof(boot), &v->bpb);
    if (rc != K_OK) {
        klog(KLOG_ERROR, F_TAG, "BPB: %s (это не FAT32 с 512Б сектором?)",
             k_strerror(rc));
        return rc;
    }

    v->fat_start_lba = v->bpb.reserved_sectors;
    v->data_lba = v->bpb.reserved_sectors +
                  (u64)v->bpb.num_fats * v->bpb.fatsz_sectors;
    if (v->data_lba >= part_sectors) {
        klog(KLOG_ERROR, F_TAG, "data-область за пределами партиции");
        return K_EINVAL;
    }
    v->total_clusters = (u32)((part_sectors - v->data_lba) /
                              v->bpb.sectors_per_cluster);

    /* FSInfo: свободные кластеры (best-effort; при мусоре — пересчёт). */
    if (v->bpb.fsinfo_sector != 0u && v->bpb.fsinfo_sector != 0xFFFFu &&
        (u64)v->bpb.fsinfo_sector < part_sectors) {
        u8 fi[FAT_SECTOR_SZ];
        if (read_sec(v, v->bpb.fsinfo_sector, fi) == K_OK) {
            u32 sig = (u32)fi[0] | ((u32)fi[1] << 8) | ((u32)fi[2] << 16) |
                      ((u32)fi[3] << 24);
            u32 magic = (u32)fi[484] | ((u32)fi[485] << 8) |
                        ((u32)fi[486] << 16) | ((u32)fi[487] << 24);
            u32 free_cnt = (u32)fi[488] | ((u32)fi[489] << 8) |
                           ((u32)fi[490] << 16) | ((u32)fi[491] << 24);
            if (sig == 0x41615252u && magic == 0x61417272u &&
                free_cnt != 0xFFFFFFFFu && free_cnt <= v->total_clusters) {
                v->free_clusters = free_cnt;
                v->free_clusters_known = 1u;
            }
        }
    }

    v->mounted = 1;
    char label[12];
    u32 li = 0;
    for (; li < 11u; li++) {
        label[li] = v->bpb.label[li];
    }
    label[li] = '\0';
    klog(KLOG_INFO, F_TAG,
         "mount: кластер=%u сект, FAT=%u x %u сект, кластеров %u, метка \"%s\"",
         (unsigned int)v->bpb.sectors_per_cluster,
         (unsigned int)v->bpb.num_fats,
         (unsigned int)v->bpb.fatsz_sectors,
         (unsigned int)v->total_clusters,
         label);
    return K_OK;
}

u64 fat_free_bytes(struct fat_vol *v)
{
    if (v == NULL || v->mounted == 0) {
        return 0u;
    }
    if (v->free_clusters_known == 0u) {
        u32 free_c = 0;
        for (u32 c = 2u; c < v->total_clusters + 2u; c++) {
            u32 next = 0;
            if (fat_get_next(v, c, &next) == K_OK && next == 0u) {
                free_c++;
            }
        }
        v->free_clusters = free_c;
        v->free_clusters_known = 1u;
    }
    return (u64)v->free_clusters * v->bpb.sectors_per_cluster * FAT_SECTOR_SZ;
}

/* ------------------------------------------------------------------ */
/* Пути и имена                                                        */
/* ------------------------------------------------------------------ */

static u32 path_split(const char *path, char comps[][FAT_NAME_MAX],
                      u32 max_comps)
{
    u32 n = 0;
    u32 i = 0;
    while (path[i] == '/' || path[i] == '\\') {
        i++;
    }
    while (path[i] != '\0' && n < max_comps) {
        u32 c = 0;
        while (path[i] != '\0' && path[i] != '/' && path[i] != '\\' &&
               c + 1u < FAT_NAME_MAX) {
            comps[n][c++] = path[i++];
        }
        comps[n][c] = '\0';
        if (c != 0u) {
            n++;
        }
        while (path[i] == '/' || path[i] == '\\') {
            i++;
        }
    }
    return n;
}

/* Регистронезависимое сравнение имён (ASCII). */
static int name_ieq(const char *a, const char *b)
{
    u32 i = 0;
    for (;;) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'a' && ca <= 'z') { ca = (char)(ca - 0x20); }
        if (cb >= 'a' && cb <= 'z') { cb = (char)(cb - 0x20); }
        if (ca != cb) {
            return 0;
        }
        if (ca == '\0') {
            return 1;
        }
        i++;
    }
}

/* Собирает длинное имя из накопленных LFN-частей (порядок 1..N).
 * При несовпадении контрольной суммы оставляет out пустым (берём 8.3). */
static void lfn_assemble(const struct fat_dir *it, u8 checksum,
                         const u8 raw83[FAT83_RAW_LEN], char *out, u32 cap)
{
    out[0] = '\0';
    if (it->lfn_count == 0u) {
        return;
    }
    if (fat_lfn_checksum(raw83) != checksum) {
        return;
    }
    u32 n = 0;
    for (u32 p = it->lfn_count; p >= 1u; p--) {
        const u16 *chars = it->lfn_parts[p - 1u];
        for (u32 i = 0; i < FAT_LFN_CHARS; i++) {
            u16 w = chars[i];
            if (w == 0x0000u || w == 0xFFFFu) {
                out[n] = '\0';
                return;
            }
            char c = (w < 0x80u) ? (char)w : '_';
            if (n + 1u < cap) {
                out[n++] = c;
            }
        }
        if (p == 1u) {
            break;
        }
    }
    out[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* Итератор каталога                                                   */
/* ------------------------------------------------------------------ */

/* Позиция записи 8.3 в потоке каталога. */
struct fat_dirent_loc {
    u32 cluster;
    u32 sector_in_clus;
    u32 entry_idx;
};

/* Скачивает текущий сектор каталога, при переходе цепочки — следующий
 * кластер. Возвращает K_OK/K_ENOENT(конец цепочки)/ошибку. */
static err_t dir_load_sector(struct fat_vol *v, struct fat_dir *it, u8 *buf)
{
    if (it->sector_in_clus >= v->bpb.sectors_per_cluster) {
        it->sector_in_clus = 0;
        u32 next = 0;
        err_t rc = fat_get_next(v, it->cluster, &next);
        if (rc != K_OK) {
            return rc;
        }
        if (next == FAT_EOC) {
            it->done = 1;
            return K_ENOENT;
        }
        it->cluster = next;
    }
    u64 lba = clus_rel_lba(v, it->cluster) + it->sector_in_clus;
    return read_sec(v, lba, buf);
}

/* Ищет следующую содержательную запись (не LFN, не метка, не свободная).
 * При успехе копирует 32 байта записи в out_raw32, loc -> позиция 8.3;
 * собранное LFN-имя (если было) — it->lfn_name. */
static err_t dir_advance(struct fat_vol *v, struct fat_dir *it,
                         u8 out_raw32[32], struct fat_dirent_loc *loc)
{
    it->lfn_name[0] = '\0';
    for (;;) {
        u8 sec[FAT_SECTOR_SZ];
        err_t rc = dir_load_sector(v, it, sec);
        if (rc != K_OK) {
            return rc;
        }
        for (; it->entry_idx < 16u; it->entry_idx++) {
            const u8 *raw = sec + it->entry_idx * 32u;
            if (raw[0] == FAT_ENTRY_END) {
                it->done = 1;
                return K_ENOENT;
            }
            if (raw[0] == FAT_ENTRY_FREE) {
                it->lfn_count = 0;
                it->lfn_active = 0;
                continue;
            }
            if (fat_dirent_is_lfn(raw) != 0) {
                if (it->lfn_count < 20u) {
                    (void)fat_lfn_part(raw, it->lfn_parts[it->lfn_count]);
                    it->lfn_count++;
                    it->lfn_checksum = raw[13];
                    it->lfn_active = 1;
                }
                continue;
            }
            if (fat_dirent_is_volume(raw) != 0) {
                it->lfn_count = 0;
                it->lfn_active = 0;
                continue;
            }

            /* Содержательная запись. */
            loc->cluster = it->cluster;
            loc->sector_in_clus = it->sector_in_clus;
            loc->entry_idx = it->entry_idx;

            char lfn[FAT_NAME_MAX];
            lfn[0] = '\0';
            if (it->lfn_active != 0 &&
                fat_lfn_checksum(raw) == it->lfn_checksum) {
                lfn_assemble(it, it->lfn_checksum, raw, lfn, FAT_NAME_MAX);
            }
            it->lfn_count = 0;
            it->lfn_active = 0;

            u32 ln = 0;
            for (; ln < FAT_NAME_MAX - 1u && lfn[ln] != '\0'; ln++) {
                it->lfn_name[ln] = lfn[ln];
            }
            /* Терминатор СРАЗУ после имени: позиции за именем —
             * накопленный мусор структуры (стек итератора не нулевой),
             * и без терминатора он попадал в имена ls/explorer и в
             * LFN-сравнение lookup'а (regress fat32/lfn-dir-entry). */
            it->lfn_name[ln] = '\0';

            for (u32 i = 0; i < 32u; i++) {
                out_raw32[i] = raw[i];
            }
            it->entry_idx++;
            return K_OK;
        }
        it->entry_idx = 0;
        it->sector_in_clus++;
    }
}

/* ------------------------------------------------------------------ */
/* Поиск (lookup)                                                      */
/* ------------------------------------------------------------------ */

/* Разыменовать каталог по компонентам пути (все, кроме последнего,
 * обязаны быть каталогами). Возвращает первый кластер каталога. */
static err_t resolve_dir_chain(struct fat_vol *v,
                               char comps[][FAT_NAME_MAX],
                               u32 n_dirs, u32 *out_cluster)
{
    u32 cur = v->bpb.root_cluster;
    for (u32 i = 0; i < n_dirs; i++) {
        struct fat_dir it;
        (void)fat_dir_iter_reset(v, cur, &it);
        int found = 0;
        for (;;) {
            u8 e32[32];
            struct fat_dirent_loc loc;
            err_t rc = dir_advance(v, &it, e32, &loc);
            if (rc != K_OK) {
                break;
            }
            struct fat_dirent_raw d;
            fat_dirent_parse(e32, &d);
            if ((d.attr & FAT_ATTR_DIR) == 0u) {
                continue;   /* в пути файл — не каталог */
            }
            char rendered[FAT_NAME_MAX];
            fat83_render(e32, d.name_nt, rendered, sizeof(rendered));
            if (name_ieq(rendered, comps[i]) != 0 ||
                (it.lfn_name[0] != '\0' && name_ieq(it.lfn_name, comps[i]))) {
                if (d.first_cluster < 2u) {
                    return K_EIO;
                }
                cur = d.first_cluster;
                found = 1;
                break;
            }
        }
        if (found == 0) {
            return K_ENOENT;
        }
    }
    *out_cluster = cur;
    return K_OK;
}

err_t fat_dir_iter_reset(struct fat_vol *v, u32 first_cluster,
                         struct fat_dir *it)
{
    if (v == NULL || it == NULL) {
        return K_EINVAL;
    }
    it->cluster = first_cluster;
    it->sector_in_clus = 0;
    it->entry_idx = 0;
    it->done = 0;
    it->lfn_count = 0;
    it->lfn_active = 0;
    it->lfn_checksum = 0;
    it->lfn_name[0] = '\0';
    return K_OK;
}

err_t fat_dir_open(struct fat_vol *v, const char *path, struct fat_dir *it)
{
    if (v == NULL || v->mounted == 0 || path == NULL || it == NULL) {
        return K_EINVAL;
    }
    char comps[16][FAT_NAME_MAX];
    u32 n = path_split(path, comps, 16u);
    u32 start = v->bpb.root_cluster;
    err_t rc = resolve_dir_chain(v, comps, n, &start);
    if (rc != K_OK) {
        return rc;
    }
    return fat_dir_iter_reset(v, start, it);
}

err_t fat_dir_next(struct fat_vol *v, struct fat_dir *it,
                   struct fat_dir_entry *out)
{
    if (v == NULL || v->mounted == 0 || it == NULL || out == NULL) {
        return K_EINVAL;
    }
    for (;;) {
        u8 e32[32];
        struct fat_dirent_loc loc;
        err_t rc = dir_advance(v, it, e32, &loc);
        if (rc != K_OK) {
            return rc;
        }
        struct fat_dirent_raw d;
        fat_dirent_parse(e32, &d);

        for (u32 i = 0; i < FAT83_RAW_LEN; i++) {
            out->raw83[i] = e32[i];
        }
        if (it->lfn_name[0] != '\0') {
            for (u32 i = 0; i < FAT_NAME_MAX - 1u; i++) {
                out->name[i] = it->lfn_name[i];
                if (it->lfn_name[i] == '\0') {
                    break;
                }
            }
            out->name[FAT_NAME_MAX - 1u] = '\0';
        } else {
            fat83_render(e32, d.name_nt, out->name, FAT_NAME_MAX);
        }
        out->size = d.size;
        out->is_dir = ((d.attr & FAT_ATTR_DIR) != 0u) ? 1 : 0;
        out->first_cluster = d.first_cluster;
        out->attr = d.attr;
        return K_OK;
    }
}

/* Полный lookup: путь -> запись + позиция + кластер родительского каталога.
 * is_last_dir: путь указывает на каталог целиком (для dir-операций). */
static err_t fat_lookup(struct fat_vol *v, const char *path,
                        struct fat_dirent_raw *out_d,
                        u8 out_raw83[FAT83_RAW_LEN],
                        struct fat_dirent_loc *out_loc,
                        u32 *out_parent_cluster, char *out_lfn)
{
    char comps[16][FAT_NAME_MAX];
    u32 n = path_split(path, comps, 16u);
    if (n == 0u) {
        return K_EINVAL;   /* корень не является файлом */
    }
    u32 parent = v->bpb.root_cluster;
    err_t rc = resolve_dir_chain(v, comps, n - 1u, &parent);
    if (rc != K_OK) {
        return rc;
    }
    if (out_parent_cluster != NULL) {
        *out_parent_cluster = parent;
    }

    struct fat_dir it;
    (void)fat_dir_iter_reset(v, parent, &it);
    for (;;) {
        u8 e32[32];
        struct fat_dirent_loc loc;
        rc = dir_advance(v, &it, e32, &loc);
        if (rc != K_OK) {
            return K_ENOENT;
        }
        struct fat_dirent_raw d;
        fat_dirent_parse(e32, &d);
        char rendered[FAT_NAME_MAX];
        fat83_render(e32, d.name_nt, rendered, sizeof(rendered));
        /* FND-2f: LFN-матч БЕЗ привязки к out_lfn — чтение по длинному
         * имени обязано работать и когда вызывающему само имя не нужно
         * (fat_file_read передаёт NULL). */
        if (fat83_match(e32, comps[n - 1u]) != 0 ||
            name_ieq(rendered, comps[n - 1u]) != 0 ||
            (it.lfn_name[0] != '\0' &&
             name_ieq(it.lfn_name, comps[n - 1u]) != 0)) {
            *out_d = d;
            for (u32 i = 0; i < FAT83_RAW_LEN; i++) {
                out_raw83[i] = e32[i];
            }
            *out_loc = loc;
            if (out_lfn != NULL) {
                for (u32 i = 0; i < FAT_NAME_MAX - 1u; i++) {
                    out_lfn[i] = it.lfn_name[i];
                    if (it.lfn_name[i] == '\0') {
                        break;
                    }
                }
                out_lfn[FAT_NAME_MAX - 1u] = '\0';
            }
            return K_OK;
        }
    }
}

int fat_exists(struct fat_vol *v, const char *path)
{
    if (v == NULL || v->mounted == 0 || path == NULL) {
        return 0;
    }
    char comps[16][FAT_NAME_MAX];
    u32 n = path_split(path, comps, 16u);
    if (n == 0u) {
        return 1;   /* корень существует */
    }
    u32 parent = v->bpb.root_cluster;
    if (resolve_dir_chain(v, comps, n - 1u, &parent) != K_OK) {
        return 0;
    }
    struct fat_dir it;
    (void)fat_dir_iter_reset(v, parent, &it);
    for (;;) {
        u8 e32[32];
        struct fat_dirent_loc loc;
        if (dir_advance(v, &it, e32, &loc) != K_OK) {
            return 0;
        }
        struct fat_dirent_raw d;
        fat_dirent_parse(e32, &d);
        char rendered[FAT_NAME_MAX];
        fat83_render(e32, d.name_nt, rendered, sizeof(rendered));
        if (fat83_match(e32, comps[n - 1u]) != 0 ||
            name_ieq(rendered, comps[n - 1u]) != 0 ||
            (it.lfn_name[0] != '\0' && name_ieq(it.lfn_name, comps[n - 1u]))) {
            return 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Файлы: чтение                                                       */
/* ------------------------------------------------------------------ */

err_t fat_file_read(struct fat_vol *v, const char *path,
                    u8 *buf, u32 cap, u32 *out_len)
{
    if (v == NULL || v->mounted == 0 || path == NULL ||
        buf == NULL || out_len == NULL) {
        return K_EINVAL;
    }
    struct fat_dirent_raw d;
    u8 raw83[FAT83_RAW_LEN];
    struct fat_dirent_loc loc;
    err_t rc = fat_lookup(v, path, &d, raw83, &loc, NULL, NULL);
    if (rc != K_OK) {
        return rc;
    }
    if ((d.attr & FAT_ATTR_DIR) != 0u) {
        return K_EINVAL;   /* каталог так не читается */
    }
    if (d.size == 0u) {
        *out_len = 0;
        return K_OK;
    }
    if (d.size > cap) {
        return K_ERANGE;
    }
    if (d.first_cluster < 2u) {
        *out_len = 0;
        return K_OK;   /* размер 0 был проверен; тут — повреждение */
    }

    u32 got = 0;
    u32 c = d.first_cluster;
    u32 guard = v->total_clusters + 2u;
    u8 tmp[4096];
    while (got < d.size) {
        if (guard == 0u) {
            return K_EIO;
        }
        guard--;
        rc = read_cluster(v, c, tmp);
        if (rc != K_OK) {
            return rc;
        }
        u32 chunk = d.size - got;
        u32 clus_bytes = v->bpb.sectors_per_cluster * FAT_SECTOR_SZ;
        if (chunk > clus_bytes) {
            chunk = clus_bytes;
        }
        for (u32 i = 0; i < chunk; i++) {
            buf[got + i] = tmp[i];
        }
        got += chunk;
        if (got >= d.size) {
            break;
        }
        u32 next = 0;
        rc = fat_get_next(v, c, &next);
        if (rc != K_OK) {
            return rc;
        }
        if (next == FAT_EOC) {
            return K_EIO;   /* размер больше цепочки */
        }
        c = next;
    }
    *out_len = got;
    return K_OK;
}

/* ------------------------------------------------------------------ */
/* Каталог: свободные слоты (серия) и рост                             */
/* ------------------------------------------------------------------ */

/* Максимум записей в одной серии: 20 LFN + 1 8.3. */
#define DIR_RUN_MAX (20u + 1u)

/* Ищет серию из need подряд свободных слотов (0xE5/0x00) — место под
 * LFN-записи + 8.3 (FND-2f). При нехватке места каталог растёт
 * (новый кластер, скан заново). out — позиция ПЕРВОГО слота серии. */
static err_t dir_find_slots(struct fat_vol *v, u32 dir_cluster, u32 need,
                            struct fat_dirent_loc *out)
{
    if (need == 0u || need > DIR_RUN_MAX) {
        return K_EINVAL;
    }
    struct fat_dir it;
    (void)fat_dir_iter_reset(v, dir_cluster, &it);
    u32 run = 0;
    for (;;) {
        u8 sec[FAT_SECTOR_SZ];
        err_t rc = dir_load_sector(v, &it, sec);
        if (rc != K_OK) {
            return rc;   /* K_ENOENT = конец цепочки */
        }
        for (u32 i = 0; i < 16u; i++) {
            u8 b = sec[i * 32u];
            if (b == FAT_ENTRY_END || b == FAT_ENTRY_FREE) {
                if (run == 0u) {
                    out->cluster = it.cluster;
                    out->sector_in_clus = it.sector_in_clus;
                    out->entry_idx = i;
                }
                run++;
                if (run >= need) {
                    return K_OK;
                }
            } else {
                run = 0;
            }
        }
        it.entry_idx = 0;
        it.sector_in_clus++;
        /* dir_load_sector перейдёт в следующий кластер; если цепочка
         * кончилась — растим каталог (после цикла). */
        struct fat_dir probe = it;
        u8 probe_sec[FAT_SECTOR_SZ];
        rc = dir_load_sector(v, &probe, probe_sec);
        if (rc == K_ENOENT) {
            /* Цепочка кончилась: нужен новый кластер каталога. */
            u32 newc = 0;
            rc = fat_alloc_chain(v, 1u, &newc);
            if (rc != K_OK) {
                return rc;
            }
            u8 zsec[FAT_SECTOR_SZ];
            for (u32 i = 0; i < FAT_SECTOR_SZ; i++) {
                zsec[i] = 0u;
            }
            u64 clba = clus_rel_lba(v, newc);
            for (u32 s = 0; s < v->bpb.sectors_per_cluster; s++) {
                rc = write_sec(v, clba + s, zsec);
                if (rc != K_OK) {
                    break;
                }
            }
            if (rc != K_OK) {
                (void)fat_free_chain(v, newc);
                return rc;
            }
            rc = fat_set_next(v, it.cluster, newc);
            if (rc != K_OK) {
                (void)fat_free_chain(v, newc);
                return rc;
            }
            /* Перезапускаем скан: серия может лежать и в старых
             * свободных записях, и в новом кластере. */
            (void)fat_dir_iter_reset(v, dir_cluster, &it);
            run = 0;
        } else if (rc != K_OK) {
            return rc;
        }
    }
}

/* Есть ли в каталоге запись с таким же коротким именем (11 байт raw).
 * dir_advance пропускает LFN/метки/свободные — сравнение только с
 * содержательными 8.3-записями. */
static int dir_has_83(struct fat_vol *v, u32 dir_cluster,
                      const u8 raw83[FAT83_RAW_LEN])
{
    struct fat_dir it;
    (void)fat_dir_iter_reset(v, dir_cluster, &it);
    for (;;) {
        u8 e32[32];
        struct fat_dirent_loc loc;
        if (dir_advance(v, &it, e32, &loc) != K_OK) {
            return 0;
        }
        if (memcmp(e32, raw83, FAT83_RAW_LEN) == 0) {
            return 1;
        }
    }
}

/* Читает сектор каталога, патчит 32-байтовую запись, пишет сектор. */
static err_t dir_write_entry(struct fat_vol *v,
                             const struct fat_dirent_loc *loc,
                             const u8 entry32[32])
{
    u8 sec[FAT_SECTOR_SZ];
    u64 lba = clus_rel_lba(v, loc->cluster) + loc->sector_in_clus;
    err_t rc = read_sec(v, lba, sec);
    if (rc != K_OK) {
        return rc;
    }
    for (u32 i = 0; i < 32u; i++) {
        sec[loc->entry_idx * 32u + i] = entry32[i];
    }
    return write_sec(v, lba, sec);
}

/* Пишет 32-байтовую запись в позицию loc и сдвигает loc на следующую
 * запись каталога (внутри найденной серии — цепочка по построению
 * скана продолжается; выход за неё — повреждение тома). */
static err_t dir_write_entry_at(struct fat_vol *v,
                                struct fat_dirent_loc *loc,
                                const u8 entry32[32])
{
    err_t rc = dir_write_entry(v, loc, entry32);
    if (rc != K_OK) {
        return rc;
    }
    loc->entry_idx++;
    if (loc->entry_idx < 16u) {
        return K_OK;
    }
    loc->entry_idx = 0;
    loc->sector_in_clus++;
    if (loc->sector_in_clus < v->bpb.sectors_per_cluster) {
        return K_OK;
    }
    loc->sector_in_clus = 0;
    u32 next = 0;
    rc = fat_get_next(v, loc->cluster, &next);
    if (rc != K_OK) {
        return rc;
    }
    if (next == FAT_EOC) {
        return K_EIO;
    }
    loc->cluster = next;
    return K_OK;
}

/* Время для записей каталога (RTC; при недоступности — фиксированная дата). */
static void now_timestamp(struct fat_timestamp *t)
{
    struct rtc_time rt;
    if (rtc_ready() != 0 && rtc_read_time(&rt) == K_OK) {
        fat_time_from_rtc(rt.year, rt.month, rt.day, rt.hour, rt.minute,
                          rt.second, t);
    } else {
        fat_time_from_rtc(2025, 1, 1, 0, 0, 0, t);
    }
}

/* ------------------------------------------------------------------ */
/* Каталоги: создание (FND-3b, установщику и ядру)                     */
/* ------------------------------------------------------------------ */

/* Создаёт каталог по пути (все компоненты, кроме последнего, обязаны
 * существовать). Этап: только строгие 8.3-имена (fat83_from_text);
 * LFN-имена каталогов — вместе с rmdir в FND-4. Точки "." и ".."
 * выставляются на созданный и родительский кластеры. */
err_t fat_dir_create(struct fat_vol *v, const char *path)
{
    if (v == NULL || v->mounted == 0 || path == NULL) {
        return K_EINVAL;
    }

    char comps[16][FAT_NAME_MAX];
    u32 n = path_split(path, comps, 16u);
    if (n == 0u) {
        return K_EINVAL;   /* корень уже существует */
    }

    u32 parent = v->bpb.root_cluster;
    err_t rc = resolve_dir_chain(v, comps, n - 1u, &parent);
    if (rc != K_OK) {
        return rc;
    }

    /* Коллизия имени: файл или каталог с тем же именем запрещены. */
    {
        struct fat_dirent_raw probe;
        u8 probe83[FAT83_RAW_LEN];
        struct fat_dirent_loc probe_loc;
        rc = fat_lookup(v, path, &probe, probe83, &probe_loc, NULL, NULL);
        if (rc == K_OK) {
            return K_EEXIST;
        }
        if (rc != K_ENOENT) {
            return rc;
        }
    }

    /* Имя каталога — строго 8.3 (LFN-каталоги — FND-4). */
    u8 raw83[FAT83_RAW_LEN];
    rc = fat83_from_text(comps[n - 1u], raw83);
    if (rc != K_OK) {
        return rc;
    }

    /* Кластер каталога: "." и ".." обязаны ссылаться на себя/родителя. */
    u32 newc = 0;
    rc = fat_alloc_chain(v, 1u, &newc);
    if (rc != K_OK) {
        return rc;
    }

    static u8 dirbuf[4096];   /* max spc=128 x 512 Б; стек не раздуваем */
    for (u32 i = 0; i < v->bpb.sectors_per_cluster * FAT_SECTOR_SZ; i++) {
        dirbuf[i] = 0u;
    }

    struct fat_timestamp ts;
    now_timestamp(&ts);

    /* raw83 для "." / "..": 11 байт — имя + пробелы (fat83_from_text
     * точку не принимает, поэтому собираем напрямую). */
    u8 dot[FAT83_RAW_LEN];
    u8 dotdot[FAT83_RAW_LEN];
    dot[0] = (u8)'.';          /* "."          */
    dotdot[0] = (u8)'.';       /* ".."         */
    dotdot[1] = (u8)'.';
    for (u32 i = 1u; i < FAT83_RAW_LEN; i++) {
        dot[i] = (u8)' ';
        dotdot[i] = (i >= 2u) ? (u8)' ' : dotdot[i];
    }

    fat_dirent_build(dot, FAT_ATTR_DIR, newc, 0, &ts,
                     dirbuf + 0 * 32u);
    fat_dirent_build(dotdot, FAT_ATTR_DIR, parent, 0, &ts,
                     dirbuf + 1 * 32u);
    for (u32 s = 0; s < v->bpb.sectors_per_cluster; s++) {
        rc = write_sec(v, clus_rel_lba(v, newc) + s, dirbuf + s * FAT_SECTOR_SZ);
        if (rc != K_OK) {
            (void)fat_free_chain(v, newc);
            return rc;
        }
    }

    /* Запись в родительский каталог (один слот: имя 8.3 без LFN). */
    struct fat_dirent_loc slot;
    rc = dir_find_slots(v, parent, 1u, &slot);
    if (rc != K_OK) {
        (void)fat_free_chain(v, newc);
        return rc;
    }
    u8 e32[32];
    fat_dirent_build(raw83, FAT_ATTR_DIR, newc, 0, &ts, e32);
    rc = dir_write_entry(v, &slot, e32);
    if (rc != K_OK) {
        (void)fat_free_chain(v, newc);
        return rc;
    }
    return K_OK;
}

/* ------------------------------------------------------------------ */
/* Файлы: запись и удаление                                            */
/* ------------------------------------------------------------------ */

err_t fat_file_write(struct fat_vol *v, const char *path,
                     const u8 *buf, u32 len)
{
    if (v == NULL || v->mounted == 0 || path == NULL ||
        (buf == NULL && len != 0u)) {
        return K_EINVAL;
    }

    char comps[16][FAT_NAME_MAX];
    u32 n = path_split(path, comps, 16u);
    if (n == 0u) {
        return K_EINVAL;
    }

    u32 parent = v->bpb.root_cluster;
    err_t rc = resolve_dir_chain(v, comps, n - 1u, &parent);
    if (rc != K_OK) {
        return rc;
    }

    /* 1. Существующая запись (по 8.3, рендеру или LFN): перезапись
     * сохраняет исходное короткое имя и парные LFN-записи. */
    struct fat_dir it;
    (void)fat_dir_iter_reset(v, parent, &it);
    struct fat_dirent_loc existing;
    int found = 0;
    struct fat_dirent_raw old;
    u8 found83[FAT83_RAW_LEN];
    for (;;) {
        u8 e32[32];
        struct fat_dirent_loc loc;
        rc = dir_advance(v, &it, e32, &loc);
        if (rc != K_OK) {
            break;
        }
        fat_dirent_parse(e32, &old);
        char rendered[FAT_NAME_MAX];
        fat83_render(e32, old.name_nt, rendered, sizeof(rendered));
        if (fat83_match(e32, comps[n - 1u]) != 0 ||
            name_ieq(rendered, comps[n - 1u]) != 0 ||
            (it.lfn_name[0] != '\0' && name_ieq(it.lfn_name, comps[n - 1u]))) {
            existing = loc;
            for (u32 i = 0; i < FAT83_RAW_LEN; i++) {
                found83[i] = e32[i];
            }
            found = 1;
            break;
        }
    }

    /* 2. Короткое имя: точное 8.3 — как раньше; иначе алиас "~N"
     * без коллизий + серия LFN-записей (FND-2f). */
    u8 raw83[FAT83_RAW_LEN];
    u8 lfn[20][32];
    u32 lfn_count = 0;
    if (found != 0) {
        for (u32 i = 0; i < FAT83_RAW_LEN; i++) {
            raw83[i] = found83[i];
        }
    } else if (fat83_from_text(comps[n - 1u], raw83) != K_OK) {
        u32 tail = 1;
        for (;;) {
            rc = fat83_alias_from_lfn(comps[n - 1u], tail, raw83);
            if (rc != K_OK) {
                return rc;
            }
            if (dir_has_83(v, parent, raw83) == 0) {
                break;
            }
            if (tail == 9u) {
                return K_EEXIST;   /* ~1..~9 заняты */
            }
            tail++;
        }
        u8 sum = fat_lfn_checksum(raw83);
        rc = fat_lfn_build_entries(comps[n - 1u], sum, lfn, 20u, &lfn_count);
        if (rc != K_OK) {
            return rc;
        }
    }

    struct fat_timestamp ts;
    now_timestamp(&ts);

    u32 new_chain = 0;
    if (len > 0u) {
        u32 need = (len + v->bpb.sectors_per_cluster * FAT_SECTOR_SZ - 1u) /
                   (v->bpb.sectors_per_cluster * FAT_SECTOR_SZ);
        rc = fat_alloc_chain(v, need, &new_chain);
        if (rc != K_OK) {
            return rc;
        }
        /* Данные: кластеры целиком (последний добиваем нулями). */
        u32 done = 0;
        u32 c = new_chain;
        u32 clus_bytes = v->bpb.sectors_per_cluster * FAT_SECTOR_SZ;
        while (done < need * clus_bytes) {
            static u8 wbuf[4096];
            u32 chunk = clus_bytes;
            if (len - done < chunk) {
                chunk = len - done;
            }
            for (u32 i = 0; i < clus_bytes; i++) {
                wbuf[i] = (i < chunk) ? buf[done + i] : 0u;
            }
            rc = write_cluster(v, c, wbuf);
            if (rc != K_OK) {
                (void)fat_free_chain(v, new_chain);
                return rc;
            }
            done += clus_bytes;
            if (done >= need * clus_bytes) {
                break;
            }
            rc = fat_get_next(v, c, &c);
            if (rc != K_OK || c == FAT_EOC) {
                (void)fat_free_chain(v, new_chain);
                return (rc != K_OK) ? rc : K_EIO;
            }
        }
    }

    if (found != 0) {
        if ((old.attr & FAT_ATTR_DIR) != 0u) {
            if (new_chain != 0u) {
                (void)fat_free_chain(v, new_chain);
            }
            return K_EINVAL;   /* перезапись каталога файлом запрещена */
        }
        (void)fat_free_chain(v, old.first_cluster);
        u8 e32[32];
        fat_dirent_build(raw83, FAT_ATTR_ARCHIVE, new_chain, len, &ts, e32);
        rc = dir_write_entry(v, &existing, e32);
        if (rc != K_OK) {
            (void)fat_free_chain(v, new_chain);
            return rc;
        }
        return K_OK;
    }

    /* 3. Новая запись: серия слотов (LFN-записи, затем 8.3). */
    struct fat_dirent_loc slot;
    rc = dir_find_slots(v, parent, lfn_count + 1u, &slot);
    if (rc != K_OK) {
        (void)fat_free_chain(v, new_chain);
        return rc;
    }
    for (u32 i = 0; i < lfn_count; i++) {
        rc = dir_write_entry_at(v, &slot, lfn[i]);
        if (rc != K_OK) {
            (void)fat_free_chain(v, new_chain);
            return rc;
        }
    }
    u8 e32[32];
    fat_dirent_build(raw83, FAT_ATTR_ARCHIVE, new_chain, len, &ts, e32);
    /* Финальная 8.3-запись — БЕЗ post-advance: шаг к следующему слоту за
     * концом цепочки каталога давал K_EIO ПОСЛЕ успешной записи, вызывающий
     * освобождал кластер — на диске оставалась запись на свободный кластер
     * (потом free_chain падала с «указание в никуда»). */
    rc = dir_write_entry(v, &slot, e32);
    if (rc != K_OK) {
        (void)fat_free_chain(v, new_chain);
        return rc;
    }
    return K_OK;
}

err_t fat_file_delete(struct fat_vol *v, const char *path)
{
    if (v == NULL || v->mounted == 0 || path == NULL) {
        return K_EINVAL;
    }
    struct fat_dirent_raw d;
    u8 raw83[FAT83_RAW_LEN];
    struct fat_dirent_loc loc;
    char lfn[FAT_NAME_MAX];
    u32 parent = 0;
    err_t rc = fat_lookup(v, path, &d, raw83, &loc, &parent, lfn);
    if (rc != K_OK) {
        return rc;
    }
    if ((d.attr & FAT_ATTR_DIR) != 0u) {
        return K_EINVAL;   /* rmdir не поддержан этапом */
    }

    rc = fat_free_chain(v, d.first_cluster);
    if (rc != K_OK) {
        return rc;
    }

    /* Стираем 8.3-запись и предшествующие LFN (та же контрольная сумма).
     * Чексумма — по ИСХОДНОМУ имени (до записи 0xE5 в первый байт!):
     * правка после затирания давала другой sum, LFN-серия не стиралась
     * и накапливала осиротевшие записи в каталоге (regress CI). */
    u8 sec[FAT_SECTOR_SZ];
    u64 lba = clus_rel_lba(v, loc.cluster) + loc.sector_in_clus;
    rc = read_sec(v, lba, sec);
    if (rc != K_OK) {
        return rc;
    }
    u8 sum = fat_lfn_checksum(sec + loc.entry_idx * 32u);
    sec[loc.entry_idx * 32u] = FAT_ENTRY_FREE;
    u32 i = loc.entry_idx;
    while (i > 0u) {
        i--;
        const u8 *e = sec + i * 32u;
        if (fat_dirent_is_lfn(e) == 0) {
            break;
        }
        if (e[13] != sum) {
            break;
        }
        sec[i * 32u] = FAT_ENTRY_FREE;
    }
    return write_sec(v, lba, sec);
}
