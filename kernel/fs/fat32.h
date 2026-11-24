/*
 * Fobos OS — FAT32-том (ввод-вывод поверх блочного слоя).
 *
 * Возможности этапа FND-2c/2f/3b:
 *  - mount по партиции из MBR (blk);
 *  - чтение каталогов с LFN-именами (UTF-16 -> ASCII/подмена '_');
 *  - чтение файлов по цепочке кластеров (LBA48 через blk);
 *  - запись файлов (создание/перезапись): точные 8.3-имена и длинные
 *    имена через LFN-серию + 8.3-алиас "~N" (FND-2f), рост каталога;
 *  - удаление файлов (цепочка освобождается, LFN-записи стираются);
 *  - создание каталогов 8.3 (FND-3b — установщик: \EFI\BOOT, \fobos);
 *  - кэш FAT-секторов (write-through, обе копии FAT).
 *
 * Не реализовано (осознанно, см. roadmap FND-4): rmdir/rename,
 * LFN-имена вне ASCII (подмена '_'), FAT12/16, не-512 секторы, журнал.
 */
#ifndef FOBOS_FS_FAT32_H
#define FOBOS_FS_FAT32_H

#include <fobos/errors.h>
#include <fobos/types.h>
#include "fatp.h"

#define FAT_SECTOR_SZ 512u
#define FAT_CACHE_SECTORS 8u

struct fat_vol {
    int mounted;
    u64 part_lba;
    u64 part_sectors;
    struct fat_bpb bpb;

    u32 total_clusters;
    u64 fat_start_lba;          /* первая копия FAT */
    u64 data_lba;               /* LBA кластера 2 */

    u32 free_clusters_known;    /* валидность счётчика FSInfo */
    u32 free_clusters;

    /* Кэш FAT-секторов (write-through). */
    struct {
        u32 valid;
        u64 lba;                /* LBA сектора ВНУТРИ партиции (0 = reserved) */
        u8  data[FAT_SECTOR_SZ];
    } cache[FAT_CACHE_SECTORS];
};

/* Монтирование: читает загрузочный сектор партиции, валидирует BPB,
 * читает FSInfo. part_sectors — размер партиции в секторах. */
err_t fat_mount(struct fat_vol *v, u64 part_lba, u64 part_sectors);

/* Защищённые системные имена (KERNEL.ELF, SETUP.CFG, FOBOS.CFG,
 * BOOTX64.EFI) — сравнение без регистра. 1 = операции удаления/
 * перезаписи запрещены. Общая политика для explorer и shell. */
int fat_name_protected(const char *name);

/* --- Каталоги --- */

struct fat_dir {
    u32 cluster;        /* текущий кластер цепочки каталога */
    u32 sector_in_clus; /* смещение сектора в кластере */
    u32 entry_idx;      /* индекс записи в секторе (0..15) */
    int done;
    /* Накопитель LFN: сборка длинного имени перед 8.3-записью. */
    u16 lfn_parts[20][FAT_LFN_CHARS];  /* до 20 LFN-записей (260 симв.) */
    u8  lfn_count;
    u8  lfn_checksum;
    int lfn_active;
    char lfn_name[FAT_NAME_MAX];       /* собранное LFN-имя текущей записи */
};

struct fat_dir_entry {
    char name[FAT_NAME_MAX];   /* "NAME.EXT" или LFN (ASCII-подмножество) */
    u32  size;
    int  is_dir;
    u32  first_cluster;
    u8   raw83[FAT83_RAW_LEN]; /* короткое имя (для удаления/перезаписи) */
    u8   attr;
};

/* Открыть каталог по пути "/fobos" или "\" (корень). */
err_t fat_dir_open(struct fat_vol *v, const char *path, struct fat_dir *it);

/* Создать каталог по пути (родитель обязан существовать, имя — строгое
 * 8.3; FND-3b: установщик создаёт \EFI\BOOT и \fobos на чистых томах).
 * K_OK / K_EEXIST (коллизия имени) / K_EINVAL / ошибка ввода-вывода. */
err_t fat_dir_create(struct fat_vol *v, const char *path);

/* Сброс итератора на конкретную цепочку (внутренний + для lookup). */
err_t fat_dir_iter_reset(struct fat_vol *v, u32 first_cluster,
                         struct fat_dir *it);

/* Следующая запись. Конец каталога -> K_ENOENT. */
err_t fat_dir_next(struct fat_vol *v, struct fat_dir *it,
                   struct fat_dir_entry *out);

/* --- Файлы --- */

/* Чтение файла целиком (cap = вместимость буфера). K_ERANGE если файл
 * больше буфера. Только файлы; каталог вернёт K_EISDIR через K_EINVAL. */
err_t fat_file_read(struct fat_vol *v, const char *path,
                    u8 *buf, u32 cap, u32 *out_len);

/* Запись файла (создание или полная перезапись). Имя — 8.3 (текст).
 * Родительский каталог должен существовать. */
err_t fat_file_write(struct fat_vol *v, const char *path,
                     const u8 *buf, u32 len);

/* Удаление файла (не каталога). Защита системных имён — на вызывающем
 * слое; тут удаляется любая найденная запись. */
err_t fat_file_delete(struct fat_vol *v, const char *path);

/* Освобождение цепочки кластеров (FAT-записи обнуляются). */
err_t fat_free_chain(struct fat_vol *v, u32 first);

/* Существует ли файл/каталог по пути. 1/0. */
int fat_exists(struct fat_vol *v, const char *path);

/* Оценка свободного места (байты). Из FSInfo или скана FAT. */
u64 fat_free_bytes(struct fat_vol *v);

#endif /* FOBOS_FS_FAT32_H */
