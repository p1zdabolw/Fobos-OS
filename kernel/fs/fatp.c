/*
 * Fobos OS — FAT32: чистая логика (реализация).
 */
#include "fatp.h"

#include "../lib/string.h"
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* BPB                                                                 */
/* ------------------------------------------------------------------ */

static u16 rd16(const u8 *p) { return (u16)(p[0] | ((u16)p[1] << 8)); }
static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

err_t fat_parse_bpb(const u8 *buf, u32 buf_len, struct fat_bpb *out)
{
    if (buf == NULL || out == NULL || buf_len < 512u) {
        return K_EINVAL;
    }
    if (buf[510] != 0x55u || buf[511] != 0xAAu) {
        return K_EINVAL;
    }

    u32 bps = rd16(buf + 11);
    u32 spc = buf[13];
    u32 reserved = rd16(buf + 14);
    u32 nfats = buf[16];
    u32 total16 = rd16(buf + 19);
    u32 total32 = rd32(buf + 32);
    u32 fatsz32 = rd32(buf + 36);
    u32 root_clus = rd32(buf + 44);
    u32 fsinfo = rd16(buf + 48);

    if (bps != 512u) {
        return K_EUNSUPPORTED;   /* этап: только 512 Б/сектор */
    }
    if (spc == 0u || (spc & (spc - 1u)) != 0u || spc > 128u) {
        return K_EINVAL;
    }
    if (nfats < 1u || nfats > 4u) {
        return K_EINVAL;
    }
    if (fatsz32 == 0u) {
        return K_EUNSUPPORTED;   /* FAT12/FAT16 не монтируем */
    }
    u32 total = (total32 != 0u) ? total32 : total16;
    if (total == 0u || reserved == 0u || root_clus < 2u) {
        return K_EINVAL;
    }
    /* FAT32-признак: кластеров > 65525. */
    u64 data = (u64)total - reserved - nfats * fatsz32;
    if (data / spc <= 65525u) {
        return K_EUNSUPPORTED;
    }

    out->bytes_per_sector = bps;
    out->sectors_per_cluster = spc;
    out->reserved_sectors = reserved;
    out->num_fats = nfats;
    out->fatsz_sectors = fatsz32;
    out->root_cluster = root_clus;
    out->fsinfo_sector = fsinfo;
    out->total_sectors = total;

    for (u32 i = 0; i < 11u; i++) {
        out->label[i] = (char)buf[71 + i];
    }
    out->label[11] = '\0';
    return K_OK;
}

/* ------------------------------------------------------------------ */
/* Имена 8.3                                                           */
/* ------------------------------------------------------------------ */

/* Символы, разрешённые в 8.3 (кроме букв/цифр) — OEM-набор FAT. */
static int fat83_char_ok(char c)
{
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '(': case ')': case '-': case '@': case '^': case '_':
    case '`': case '{': case '}': case '~':
        return 1;
    default:
        return 0;
    }
}

static char up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 0x20) : c;
}

err_t fat83_from_text(const char *text, u8 out[FAT83_RAW_LEN])
{
    if (text == NULL || out == NULL) {
        return K_EINVAL;
    }
    for (u32 i = 0; i < FAT83_RAW_LEN; i++) {
        out[i] = (u8)' ';
    }

    /* base[8].ext[3] */
    u32 base = 0;
    u32 ext = 0;
    int in_ext = 0;
    for (u32 i = 0; text[i] != '\0'; i++) {
        char c = up(text[i]);
        if (c == '.') {
            if (in_ext != 0) {
                return K_EINVAL;      /* вторая точка */
            }
            in_ext = 1;
            continue;
        }
        if (fat83_char_ok(c) == 0) {
            return K_EINVAL;
        }
        if (in_ext == 0) {
            if (base >= 8u) {
                return K_ERANGE;
            }
            out[base++] = (u8)c;
        } else {
            if (ext >= 3u) {
                return K_ERANGE;
            }
            out[8u + ext++] = (u8)c;
        }
    }
    if (base == 0u && ext == 0u) {
        return K_EINVAL;
    }
    return K_OK;
}

int fat83_match(const u8 raw[FAT83_RAW_LEN], const char *text)
{
    u8 want[FAT83_RAW_LEN];
    if (fat83_from_text(text, want) != K_OK) {
        /* Может быть LFN-имя: 8.3-сравнение с ним делается выше уровнем. */
        return 0;
    }
    for (u32 i = 0; i < FAT83_RAW_LEN; i++) {
        if (raw[i] != want[i]) {
            return 0;
        }
    }
    return 1;
}

void fat83_render(const u8 raw[FAT83_RAW_LEN], u8 name_nt_flags, char *out,
                  u32 cap)
{
    if (out == NULL || cap < FAT83_RAW_LEN + 2u) {
        return;
    }
    u32 n = 0;
    u32 base_len = 8u;
    while (base_len > 0u && raw[base_len - 1u] == (u8)' ') {
        base_len--;
    }
    u32 ext_len = 3u;
    while (ext_len > 0u && raw[8u + ext_len - 1u] == (u8)' ') {
        ext_len--;
    }

    for (u32 i = 0; i < base_len && n + 1u < cap; i++) {
        char c = (char)raw[i];
        /* NT-флаг: bit3 = base в нижнем регистре. */
        if ((name_nt_flags & 0x08u) != 0u && c >= 'A' && c <= 'Z') {
            c = (char)(c + 0x20);
        }
        out[n++] = c;
    }
    if (ext_len > 0u && n + 1u < cap) {
        out[n++] = '.';
        for (u32 i = 0; i < ext_len && n + 1u < cap; i++) {
            char c = (char)raw[8u + i];
            /* NT-флаг: bit4 = расширение в нижнем регистре. */
            if ((name_nt_flags & 0x10u) != 0u && c >= 'A' && c <= 'Z') {
                c = (char)(c + 0x20);
            }
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* LFN                                                                 */
/* ------------------------------------------------------------------ */

u8 fat_lfn_checksum(const u8 raw83[FAT83_RAW_LEN])
{
    u8 sum = 0;
    for (u32 i = 0; i < FAT83_RAW_LEN; i++) {
        sum = (u8)(((sum & 1u) << 7) + (sum >> 1) + raw83[i]);
    }
    return sum;
}

int fat_lfn_part(const u8 *raw32, u16 out[FAT_LFN_CHARS])
{
    if (raw32 == NULL || out == NULL) {
        return -1;
    }
    if (raw32[11] != FAT_ATTR_LFN) {
        return -1;
    }
    u8 ord = raw32[0];
    if (ord == FAT_ENTRY_FREE || ord == FAT_ENTRY_END) {
        return -1;
    }
    int is_last = ((ord & 0x40u) != 0u) ? 1 : 0;

    /* Раскладка LFN-записи: name1 = 5 u16 @1..10,
     * name2 = 6 u16 @14..25, name3 = 2 u16 @28..31 (итого 13). */
    static const u32 slots[FAT_LFN_CHARS] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
    };

    for (u32 i = 0; i < FAT_LFN_CHARS; i++) {
        u32 o = slots[i];
        u16 v = (u16)(raw32[o] | ((u16)raw32[o + 1u] << 8));
        out[i] = v;
    }
    return is_last;
}

/* ------------------------------------------------------------------ */
/* LFN (запись, FND-2f)                                                */
/* ------------------------------------------------------------------ */

/* Слоты UTF-16 символов в LFN-записи (общие с чтением). */
static const u32 fat_lfn_slots[FAT_LFN_CHARS] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
};

err_t fat_lfn_build_entries(const char *name, u8 checksum,
                            u8 out[][32], u32 max, u32 *out_count)
{
    if (name == NULL || out == NULL || out_count == NULL) {
        return K_EINVAL;
    }
    u32 len = 0;
    while (name[len] != '\0') {
        if (len >= 255u) {
            return K_EINVAL;   /* максимум LFN — 255 символов */
        }
        len++;
    }
    if (len == 0u) {
        return K_EINVAL;
    }
    u32 n = (len + FAT_LFN_CHARS - 1u) / FAT_LFN_CHARS;
    if (n > max) {
        return K_ERANGE;
    }

    for (u32 i = 0; i < n; i++) {
        u8 *e = out[i];
        for (u32 k = 0; k < 32u; k++) {
            e[k] = 0xFFu;   /* свободные слоты UTF-16 = 0xFFFF */
        }
        u32 chunk = n - 1u - i;              /* ord = chunk + 1 */
        e[0] = (u8)(((i == 0u) ? 0x40u : 0x00u) | (u8)(chunk + 1u));
        e[11] = FAT_ATTR_LFN;
        e[12] = 0u;                          /* type = 0 */
        e[13] = checksum;
        e[26] = 0u;                          /* first cluster (LFN) = 0 */
        e[27] = 0u;

        for (u32 k = 0; k < FAT_LFN_CHARS; k++) {
            u32 idx = chunk * FAT_LFN_CHARS + k;
            u16 w;
            if (idx < len) {
                unsigned char c = (unsigned char)name[idx];
                /* LFN этапа — ASCII: не-ASCII честно заменяем '_'. */
                w = (c < 0x20u || c >= 0x7Fu) ? (u16)'_' : (u16)c;
            } else if (idx == len) {
                w = 0x0000u;   /* терминатор имени */
            } else {
                w = 0xFFFFu;   /* заполнитель (уже выставлен) */
            }
            u32 o = fat_lfn_slots[k];
            e[o] = (u8)(w & 0xFFu);
            e[o + 1u] = (u8)(w >> 8);
        }
    }
    *out_count = n;
    return K_OK;
}

/* Базовые/расширительные символы алиаса: uppercase, допустимые 8.3
 * остаются, прочие (включая пробел) превращаются в '_'. */
static char alias_char(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 0x20);
    }
    if (fat83_char_ok(c) != 0) {
        return c;
    }
    return '_';
}

err_t fat83_alias_from_lfn(const char *text, u32 tail, u8 out[FAT83_RAW_LEN])
{
    if (text == NULL || out == NULL || text[0] == '\0') {
        return K_EINVAL;
    }
    if (tail < 1u || tail > 9u) {
        return K_EINVAL;   /* алиас вмещает "~1".."~9" */
    }
    for (u32 i = 0; i < FAT83_RAW_LEN; i++) {
        out[i] = (u8)' ';
    }

    /* Расширение — после ПОСЛЕДНЕЙ точки. */
    u32 last_dot = 0xFFFFFFFFu;
    for (u32 i = 0; text[i] != '\0'; i++) {
        if (text[i] == '.') {
            last_dot = i;
        }
    }

    /* База: до 6 символов + "~N". Пробелы выбрасываются (как в
     * Windows-алиасах), прочие недопустимые символы -> '_'. */
    u32 base = 0;
    for (u32 i = 0; text[i] != '\0' && base < 6u; i++) {
        if (i == last_dot) {
            break;   /* точка отделяет расширение, в базе не считается */
        }
        if (text[i] == '.' || text[i] == ' ') {
            continue;
        }
        out[base++] = (u8)alias_char(text[i]);
    }
    if (base == 0u) {
        out[0] = 'F';   /* имя без валидных символов: FILE~N */
        out[1] = 'I';
        out[2] = 'L';
        out[3] = 'E';
        base = 4u;
    }
    /* "~N" ставится СРАЗУ после базы (позиции зависят от её длины). */
    out[base] = (u8)'~';
    out[base + 1u] = (u8)('0' + tail);

    /* Расширение: до 3 символов. */
    if (last_dot != 0xFFFFFFFFu) {
        u32 ext = 0;
        for (u32 i = last_dot + 1u; text[i] != '\0' && ext < 3u; i++) {
            out[8u + ext++] = (u8)alias_char(text[i]);
        }
    }
    return K_OK;
}

/* ------------------------------------------------------------------ */
/* Записи каталога                                                     */
/* ------------------------------------------------------------------ */

void fat_dirent_parse(const u8 *raw32, struct fat_dirent_raw *out)
{
    out->first_byte = raw32[0];
    out->attr = raw32[11];
    out->name_nt = raw32[12];
    out->first_cluster = (u32)rd16(raw32 + 20) << 16 | (u32)rd16(raw32 + 26);
    out->size = rd32(raw32 + 28);
}

int fat_dirent_is_lfn(const u8 *raw32)
{
    return raw32[11] == FAT_ATTR_LFN;
}

int fat_dirent_is_volume(const u8 *raw32)
{
    if (raw32[11] == FAT_ATTR_LFN) {
        return 0;
    }
    return (raw32[11] & FAT_ATTR_VOLUME) != 0u;
}

void fat_dirent_build(const u8 raw83[FAT83_RAW_LEN], u8 attr,
                      u32 first_cluster, u32 size,
                      const struct fat_timestamp *t, u8 *out32)
{
    for (u32 i = 0; i < 32u; i++) {
        out32[i] = 0u;
    }
    for (u32 i = 0; i < FAT83_RAW_LEN; i++) {
        out32[i] = raw83[i];
    }
    out32[11] = attr;
    out32[12] = 0u;                       /* NT-флаги: имя пишем UPPERCASE */
    /* crtTime@14, crtDate@16, lastAccDate@18, wrtTime@22, wrtDate@24. */
    out32[14] = (u8)(t->time & 0xFFu);
    out32[15] = (u8)(t->time >> 8);
    out32[16] = (u8)(t->date & 0xFFu);
    out32[17] = (u8)(t->date >> 8);
    out32[18] = out32[16];
    out32[19] = out32[17];
    out32[22] = out32[14];
    out32[23] = out32[15];
    out32[24] = out32[16];
    out32[25] = out32[17];
    out32[20] = (u8)((first_cluster >> 16) & 0xFFu);
    out32[21] = (u8)((first_cluster >> 24) & 0xFFu);
    out32[26] = (u8)(first_cluster & 0xFFu);
    out32[27] = (u8)((first_cluster >> 8) & 0xFFu);
    out32[28] = (u8)(size & 0xFFu);
    out32[29] = (u8)((size >> 8) & 0xFFu);
    out32[30] = (u8)((size >> 16) & 0xFFu);
    out32[31] = (u8)((size >> 24) & 0xFFu);
}

/* ------------------------------------------------------------------ */
/* Время                                                               */
/* ------------------------------------------------------------------ */

void fat_time_from_rtc(u16 year, u8 month, u8 day,
                       u8 hour, u8 minute, u8 second,
                       struct fat_timestamp *out)
{
    if (year < 1980u) {
        year = 1980u;
    }
    if (year > 2107u) {
        year = 2107u;
    }
    u16 date = (u16)(((year - 1980u) << 9) | ((u16)month << 5) | day);
    u8 s2 = (u8)(second / 2u);
    u16 time = (u16)(((u16)hour << 11) | ((u16)minute << 5) | s2);
    out->date = date;
    out->time = time;
}

/* ------------------------------------------------------------------ */
/* Кластеры                                                            */
/* ------------------------------------------------------------------ */

u64 fat_cluster_lba(u64 part_lba, u32 reserved, u32 spc, u32 cluster)
{
    /* Данные начинаются после reserved + все FAT. */
    return part_lba + reserved + (u64)(cluster - 2u) * spc;
}

err_t fat_fat_entry_value(u32 raw, u32 *out_next)
{
    if (out_next == NULL) {
        return K_EINVAL;
    }
    raw &= 0x0FFFFFFFu;
    if (raw >= FAT_EOC_MIN) {
        *out_next = FAT_EOC;
    } else {
        *out_next = raw;
    }
    return K_OK;
}
