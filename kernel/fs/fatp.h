/*
 * Fobos OS — FAT32: чистая логика (без ввода-вывода).
 *
 * Разделено ради тестируемости: этот файл компилируется и в host-тестах,
 * и в ядре; весь доступ к диску — в fat32.c. Здесь: BPB, имена 8.3
 * (сборка/сравнение/рендер), разбор/сборка 32-байтовых записей каталога,
 * LFN-записи, метки времени FAT, кластерная арифметика.
 */
#ifndef FOBOS_FS_FATP_H
#define FOBOS_FS_FATP_H

#include <fobos/errors.h>
#include <fobos/types.h>

#define FAT83_RAW_LEN    11u   /* сырая запись имени в 32-байтовой записи */
#define FAT_NAME_MAX     64u   /* собранное имя (8.3 или LFN), + NUL */
#define FAT_LFN_CHARS    13u   /* символов UTF-16 в одной LFN-записи */
#define FAT_ATTR_RO      0x01u
#define FAT_ATTR_HIDDEN  0x02u
#define FAT_ATTR_SYSTEM  0x04u
#define FAT_ATTR_VOLUME  0x08u
#define FAT_ATTR_DIR     0x10u
#define FAT_ATTR_ARCHIVE 0x20u
#define FAT_ATTR_LFN     0x0Fu /* RO|HIDDEN|SYSTEM|VOLUME */
#define FAT_ENTRY_FREE   0xE5u
#define FAT_ENTRY_END    0x00u
#define FAT_EOC          0x0FFFFFFFu
#define FAT_EOC_MIN      0x0FFFFFF8u

/* --- Время FAT --- */

struct fat_timestamp {
    u16 date;   /* битами: год-1980 << 9 | месяц << 5 | день */
    u16 time;   /* час << 11 | минута << 5 | сек/2 */
};


/* --- BPB (загрузочный сектор тома) --- */

struct fat_bpb {
    u32 bytes_per_sector;
    u32 sectors_per_cluster;
    u32 reserved_sectors;
    u32 num_fats;
    u32 fatsz_sectors;        /* FAT32 (смещение 36) */
    u32 root_cluster;
    u32 fsinfo_sector;        /* относительно начала тома; 0xFFFF = нет */
    u32 total_sectors;        /* total32, при 0 — total16 */
    char label[12];           /* OEM/метка из BPB (11 байт + NUL) */
};

/* Валидация BPB для FAT32: 512 Б/сектор, >= 1 сектор/кластер,
 * 2 FAT, разумные поля. buf = 512 байт загрузочного сектора. */
err_t fat_parse_bpb(const u8 *buf, u32 buf_len, struct fat_bpb *out);

/* --- Имена 8.3 --- */

/* "FOBOS.CFG" -> raw "FOBOS  CFG". Возвращает K_OK/K_EINVAL/K_ERANGE
 * (длиннее 8.3). Допустимые символы: буквы/цифры и
 * "!#$%&'()-@^_`{}~" (без точки — она отдаляется в расширение). */
err_t fat83_from_text(const char *text, u8 out[FAT83_RAW_LEN]);

/* Сравнение raw-записи с текстовым именем без учёта регистра. */
int fat83_match(const u8 raw[FAT83_RAW_LEN], const char *text);

/* raw -> текст "NAME.EXT" (учёт NT-флагов имени: byte 12). */
void fat83_render(const u8 raw[FAT83_RAW_LEN], u8 name_nt_flags, char *out,
                  u32 cap);

/* --- LFN (VFAT, чтение) --- */

/* Разбор LFN-записи (32 байта). Возвращает:
 *   1  — последняя часть (ord без 0x40): имя собрано в out;
 *   0  — не последняя часть: часть записана в out (FAT_LFN_CHARS);
 *   -1 — это не LFN-запись (или битый порядок). */
int fat_lfn_part(const u8 *raw32, u16 out[FAT_LFN_CHARS]);

/* Контрольная сумма короткого имени (для проверки пары 83+LFN). */
u8 fat_lfn_checksum(const u8 raw83[FAT83_RAW_LEN]);

/* --- LFN (VFAT, запись, FND-2f) --- */

/* Сборка серии LFN-записей для длинного имени (ASCII-подмножество).
 * out[i] — физический порядок следования в каталоге (ord N с 0x40
 * первым, ord 1 последним, сразу перед 8.3-записью). terminator 0x0000
 * и заполнитель 0xFFFF расставляются по спецификации. Максимум — max
 * записей (20 => имя до 260 симв.). out_count — число записей. */
err_t fat_lfn_build_entries(const char *name, u8 checksum,
                            u8 out[][32], u32 max, u32 *out_count);

/* Генератор 8.3-алиаса длинного имени ("my document.txt" ->
 * "MYDOCU~1.TXT"). tail — номер попытки 1..9 (коллизии разрешаются
 * увеличением номера, как в Windows). Чистая функция: проверку занятости
 * алиаса в каталоге делает вызывающий слой (fat32.c). */
err_t fat83_alias_from_lfn(const char *text, u32 tail, u8 out[FAT83_RAW_LEN]);

/* --- Записи каталога (32 байта) --- */

struct fat_dirent_raw {
    u8  first_byte;
    u8  attr;
    u8  name_nt;
    u32 first_cluster;
    u32 size;
};

/* Разбор 32-байтовой записи. is_lfn(): attr == 0x0F. */
void fat_dirent_parse(const u8 *raw32, struct fat_dirent_raw *out);
int  fat_dirent_is_lfn(const u8 *raw32);
int  fat_dirent_is_volume(const u8 *raw32);

/* Сборка 32-байтовой записи каталога (8.3-имя; даты из t). */
void fat_dirent_build(const u8 raw83[FAT83_RAW_LEN], u8 attr,
                      u32 first_cluster, u32 size,
                      const struct fat_timestamp *t, u8 *out32);

/* RTC -> FAT-время (чистая функция). */
void fat_time_from_rtc(u16 year, u8 month, u8 day,
                       u8 hour, u8 minute, u8 second,
                       struct fat_timestamp *out);

/* --- Кластеры --- */

/* LBA первого сектора кластера (cluster >= 2). */
u64 fat_cluster_lba(u64 part_lba, u32 reserved, u32 spc, u32 cluster);

/* Значение FAT-записи: K_OK/K_EIO/K_EINVAL(EOC-область → out=FAT_EOC). */
err_t fat_fat_entry_value(u32 raw, u32 *out_next);

#endif /* FOBOS_FS_FATP_H */
