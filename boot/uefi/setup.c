/*
 * Fobos OS — системный установщик (реализация мастера).
 *
 * Экраны (7 шагов, как в классических установщиках настольных ОС):
 *   1 welcome  2 license  3 disks  4 drive options  5 confirm
 *   6 working (прогресс)  7 done
 * Управление: Enter/Esc/Up/Down и буквы (y/n/c/e/d/f/a) — по подсказкам
 * внизу каждого экрана. Все разрушительные операции требуют y/n.
 */
#include "setup.h"

#include "bootctx.h"
#include "disk.h"
#include "espfile.h"
#include "fmt.h"
#include "lgfx.h"
#include "lin.h"
#include "lserial.h"
#include "ublk.h"
#include "uisup.h"

#include "../../common/fmtcalc.h"
#include "../../common/mbr.h"

#include "../kernel/fs/fat32.h"

/* ------------------------------------------------------------------ */
/* Константы разметки                                                  */
/* ------------------------------------------------------------------ */

/* Первый сектор, доступный для разделов (1 МиБ — стандарт выравнивания). */
#define SETUP_FIRST_LBA      2048u
/* Выравнивание старта новых разделов: 1 МиБ. */
#define SETUP_ALIGN_SECTORS  2048u

/* Размер ESP: 64 МиБ (Windows 7 использовала 100 МиБ, 200 МиБ с Win10;
 * для Fobos 64 МиБ достаточно: BOOTX64.EFI + kernel.elf < 8 МиБ, но
 * оставляем запас под обновления ядра). */
#define SETUP_ESP_MIB        64u
#define SETUP_ESP_SECTORS    ((u64)SETUP_ESP_MIB * 1048576u / 512u)

/* Ширина экрана списков: бокс 640 px = 76 символов 8x8. */
#define SETUP_BOX_W 640u
#define SETUP_BOX_H 400u

/* Максимальный размер копируемого файла (kernel.elf). */
#define SETUP_COPY_MAX (64u * 1024u * 1024u)

/* Метки томов (ровно 11 байт, как требует FAT). */
static const char SETUP_LABEL_ESP[11]  = { 'F', 'O', 'B', 'O', 'S', 'E', 'S', 'P', ' ', ' ', ' ' };
static const char SETUP_LABEL_DATA[11] = { 'F', 'O', 'B', 'O', 'S', 'D', 'I', 'S', 'K', ' ', ' ' };

/* Конфиг режима рабочего стола по умолчанию (если на носителе нет
 * \fobos\fobos.cfg): GUI-десктоп — целевое состояние продукта. */
static const char SETUP_FOBOS_CFG_DEFAULT[] = "mode=gui\n";

enum setup_vol {
    VOL_ESP = 0,
    VOL_DATA = 1
};

/* Манифест копирования: носитель -> целевой том. required=1 обязателен:
 * ошибка его копирования прерывает установку. */
struct copy_item {
    const char *src;
    const char *dst;
    u64 max_bytes;
    int required;
    int vol;
};

static const struct copy_item SETUP_MANIFEST[] = {
    { "\\fobos\\kernel.elf",      "\\fobos\\kernel.elf",      SETUP_COPY_MAX, 1, VOL_ESP  },
    { "\\EFI\\BOOT\\BOOTX64.EFI", "\\EFI\\BOOT\\BOOTX64.EFI", 8u * 1024u * 1024u, 1, VOL_ESP },
    { "\\fobos\\HELLO.ELF",       "\\fobos\\HELLO.ELF",       8u * 1024u * 1024u, 1, VOL_DATA },
    { "\\fobos\\MTEST.ELF",       "\\fobos\\MTEST.ELF",       8u * 1024u * 1024u, 1, VOL_DATA },
    { "\\fobos\\KECHO.ELF",       "\\fobos\\KECHO.ELF",       8u * 1024u * 1024u, 1, VOL_DATA },
    { "\\fobos\\NOTES.TXT",       "\\fobos\\NOTES.TXT",       1u * 1024u * 1024u, 0, VOL_DATA },
};
#define SETUP_MANIFEST_N (sizeof(SETUP_MANIFEST) / sizeof(SETUP_MANIFEST[0]))

/* ------------------------------------------------------------------ */
/* Состояние мастера                                                   */
/* ------------------------------------------------------------------ */

enum setup_step {
    ST_WELCOME = 1,
    ST_LICENSE = 2,
    ST_DISKS = 3,
    ST_DRIVES = 4,
    ST_CONFIRM = 5,
    ST_WORKING = 6,
    ST_DONE = 7
};
#define SETUP_STEPS_TOTAL 7u

struct setup_state {
    u8 disk;                          /* индекс рабочего диска в disk_list */
    struct mbr_part lay[MBR_ENTRY_COUNT];
    u8 row;                           /* курсор списка разделов (0..4) */
    s8 esp_part;                      /* существующий ESP или -1 */
    s8 data_part;                     /* выбранный системный том или -1 */
    int esp_create;                   /* 1 = ESP будет создан при установке */
};

static const struct disk_list *g_disks;

static void setup_log(const char *s)
{
    lserial_puts("[setup] ");
    lserial_puts(s);
    lserial_puts("\n");
}

static void setup_log_u64(const char *pre, u64 v, const char *post)
{
    lserial_puts("[setup] ");
    lserial_puts(pre);
    lserial_put_hex(v);
    lserial_puts(post);
    lserial_puts("\n");
}

/* MiB -> секторы; ёмкость диска в MiB (для подписей). */
static u64 mib_to_sectors(u64 mib)
{
    return mib * 1048576u / 512u;
}

static u64 sectors_to_mib(u64 sectors)
{
    return sectors * 512u / 1048576u;
}

/* ------------------------------------------------------------------ */
/* Разметка: геометрия и операции                                      */
/* ------------------------------------------------------------------ */

/* Конец последнего занятого региона + размер максимального свободного
 * промежутка [gap_lba, gap_end). Промежуток ищется по всем разрывам
 * между занятыми диапазонами (включая хвост). */
static void layout_gap(const struct setup_state *st, u64 disk_sectors,
                       u64 *gap_lba, u64 *gap_sectors)
{
    /* Границы занятых диапазонов по возрастанию. */
    u64 starts[MBR_ENTRY_COUNT];
    u64 ends[MBR_ENTRY_COUNT];
    u32 n = 0;
    for (u32 i = 0; i < MBR_ENTRY_COUNT; i++) {
        if (st->lay[i].type == MBR_PART_EMPTY || st->lay[i].sectors == 0u) {
            continue;
        }
        starts[n] = st->lay[i].lba;
        ends[n] = st->lay[i].lba + st->lay[i].sectors;
        n++;
    }
    /* Сортировка вставками (n <= 4). */
    for (u32 i = 1; i < n; i++) {
        u64 s = starts[i];
        u64 e = ends[i];
        u32 j = i;
        while (j > 0u && starts[j - 1u] > s) {
            starts[j] = starts[j - 1u];
            ends[j] = ends[j - 1u];
            j--;
        }
        starts[j] = s;
        ends[j] = e;
    }

    u64 best_lba = SETUP_FIRST_LBA;
    u64 best_len = 0;
    u64 cursor = SETUP_FIRST_LBA;
    for (u32 i = 0; i < n; i++) {
        if (starts[i] > cursor) {
            u64 len = starts[i] - cursor;
            if (len > best_len) {
                best_len = len;
                best_lba = cursor;
            }
        }
        if (ends[i] > cursor) {
            cursor = ends[i];
        }
    }
    if (disk_sectors > cursor) {
        u64 len = disk_sectors - cursor;
        if (len > best_len) {
            best_len = len;
            best_lba = cursor;
        }
    }
    *gap_lba = best_lba;
    *gap_sectors = best_len;
}

static u64 layout_free_sectors(const struct setup_state *st, u64 disk_sectors)
{
    u64 lba, len;
    layout_gap(st, disk_sectors, &lba, &len);
    return len;
}

/* Добавляет раздел в первый подходящий промежуток. K_OK / K_ENOMEM
 * (все 4 записи заняты) / K_ERANGE (не помещается / слишком мал).
 * out_idx — индекс созданной записи (может быть NULL). */
static err_t layout_add(struct setup_state *st, u64 disk_sectors,
                        u8 type, u64 sectors, u8 boot, u32 *out_idx)
{
    u32 free_idx = MBR_ENTRY_COUNT;
    for (u32 i = 0; i < MBR_ENTRY_COUNT; i++) {
        if (st->lay[i].type == MBR_PART_EMPTY || st->lay[i].sectors == 0u) {
            free_idx = i;
            break;
        }
    }
    if (free_idx == MBR_ENTRY_COUNT) {
        return K_ENOMEM;
    }
    if (sectors == 0u) {
        return K_EINVAL;
    }

    u64 gap_lba, gap_len;
    layout_gap(st, disk_sectors, &gap_lba, &gap_len);
    u64 start = fmtcalc_align_up(gap_lba, SETUP_ALIGN_SECTORS);
    u64 end_align = gap_lba + gap_len;
    if (start < gap_lba || start >= end_align || end_align - start < sectors) {
        return K_ERANGE;
    }

    st->lay[free_idx].boot = boot;
    st->lay[free_idx].type = type;
    st->lay[free_idx].lba = start;
    st->lay[free_idx].sectors = sectors;
    if (out_idx != NULL) {
        *out_idx = free_idx;
    }
    return K_OK;
}

static void layout_from_disk(struct setup_state *st, u8 disk_idx)
{
    const struct disk_info *d = &g_disks->disks[disk_idx];
    for (u32 i = 0; i < MBR_ENTRY_COUNT; i++) {
        st->lay[i].boot = 0u;
        st->lay[i].type = 0u;
        st->lay[i].lba = 0u;
        st->lay[i].sectors = 0u;
    }
    for (u8 i = 0; i < d->part_count && i < MBR_ENTRY_COUNT; i++) {
        st->lay[i].boot = d->parts[i].boot;
        st->lay[i].type = d->parts[i].type;
        st->lay[i].lba = d->parts[i].lba;
        st->lay[i].sectors = d->parts[i].sectors;
    }
}

/* Пишет текущую разметку на диск (ublk уже привязан). */
static err_t layout_commit(const struct setup_state *st)
{
    setup_log("phase=mbr write");
    err_t rc = fmt_write_mbr(st->lay);
    if (rc != K_OK) {
        setup_log("phase=mbr FAILED");
        return rc;
    }
    setup_log("phase=mbr ok");
    return K_OK;
}

/* Индекс ESP (0xEF) в разметке или -1. */
static int layout_find_esp(const struct setup_state *st)
{
    for (u32 i = 0; i < MBR_ENTRY_COUNT; i++) {
        if (st->lay[i].type == MBR_PART_EFI_SYS && st->lay[i].sectors != 0u) {
            return (int)i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Экранные помощники                                                  */
/* ------------------------------------------------------------------ */

/* Подсветка строки списка: selected — акцентная рамка/полоса. */
static void list_row(u32 y, const char *text, int selected, int dim)
{
    u32 fg = dim ? LGFX_COL_TX_DIM : LGFX_COL_TX;
    u32 x = uisup_box_x() + 24u;
    u32 w = uisup_box_w() - 48u;
    if (selected) {
        lgfx_fill_rect(x - 4u, y - 2u, w + 8u, 14u, LGFX_COL_FIELD);
        lgfx_fill_rect(x - 4u, y - 2u, 2u, 14u, LGFX_COL_ACCENT);
        fg = LGFX_COL_TX;
    }
    lgfx_text(x, y, text, fg, selected ? LGFX_COL_FIELD : LGFX_COL_PANEL);
}

/* Ожидание клавиши из набора (блокирует). Возвращает код:
 * KEY_ENTER/KEY_ESC/KEY_UP/KEY_DOWN или ASCII-символ (>0x20). */
enum setup_keys {
    SK_NONE = 0,
    SK_ENTER = 0x0D,
    SK_ESC = 0x100,
    SK_UP = 0x101,
    SK_DOWN = 0x102
};

static int wait_key(void)
{
    for (;;) {
        u16 scan = 0, ch = 0;
        if (lin_poll_key(&scan, &ch) == 0) {
            if (scan == EFI_SCAN_ESC) {
                return SK_ESC;
            }
            if (scan == EFI_SCAN_UP) {
                return SK_UP;
            }
            if (scan == EFI_SCAN_DOWN) {
                return SK_DOWN;
            }
            if (ch == 0x0Du || ch == 0x0Au) {
                return SK_ENTER;
            }
            if (ch >= 0x20u && ch <= 0x7Eu) {
                return (int)ch;
            }
        }
        bootctx_st()->boot_services->stall(5000u);
    }
}

/* y/n-подтверждение в боксе (рисует поверх текущего экрана). */
static int confirm_yn(u32 y, const char *question)
{
    char yn[2];
    yn[0] = 'n';
    yn[1] = '\0';
    lgfx_fill_rect(uisup_box_x() + 16u, y, uisup_box_w() - 32u, 22u,
                   LGFX_COL_FIELD);
    lgfx_text(uisup_box_x() + 20u, y + 4u, question, LGFX_COL_TX,
              LGFX_COL_FIELD);
    for (;;) {
        lgfx_input_field(uisup_box_x() + uisup_box_w() - 60u, y + 2u, 44u,
                         yn, 1, 1, 0);
        int k = wait_key();
        if (k == SK_ESC) {
            return -1;
        }
        if (k == SK_ENTER) {
            return (yn[0] == 'y' || yn[0] == 'Y') ? 1 : 0;
        }
        if (k == 'y' || k == 'Y' || k == 'n' || k == 'N') {
            yn[0] = (char)k;
        }
    }
}

/* Десятичная строка -> u64 (0 при ошибке). */
static u64 parse_u64(const char *s)
{
    u64 v = 0;
    u32 digits = 0;
    while (*s != '\0') {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        v = v * 10u + (u64)(*s - '0');
        s++;
        digits++;
        if (digits > 12u) {
            return 0;   /* > 4 ТиБ в МиБ не бывает */
        }
    }
    return v;
}

static void print_u64_dec(char *dst, u32 cap, u64 v)
{
    char tmp[24];
    u32 n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && n < sizeof(tmp));
    u32 i = 0;
    for (; n > 0u && i + 1u < cap; i++) {
        dst[i] = tmp[--n];
    }
    dst[i] = '\0';
}

static const char *part_type_name(u8 type)
{
    switch (type) {
    case MBR_PART_EFI_SYS:    return "EFI System";
    case MBR_PART_FAT32_LBA:  return "FAT32 (Fobos)";
    case MBR_PART_FAT32_CHS:  return "FAT32";
    case MBR_PART_EMPTY:      return "";
    default:                  return "Other";
    }
}

/* ------------------------------------------------------------------ */
/* Экраны                                                              */
/* ------------------------------------------------------------------ */

static int screen_welcome(void)
{
    setup_log("step=welcome");
    uisup_screen(1, SETUP_STEPS_TOTAL, "install",
                 "Install Fobos OS",
                 "This wizard installs Fobos OS onto a disk of this computer. "
                 "Existing partitions on the target disk will be erased.",
                 "Enter: next   Esc: cancel");
    for (;;) {
        int k = wait_key();
        if (k == SK_ESC) {
            return -1;
        }
        if (k == SK_ENTER) {
            return 0;
        }
    }
}

static int screen_license(void)
{
    setup_log("step=license");
    uisup_screen(2, SETUP_STEPS_TOTAL, "license",
                 "License terms (MIT)",
                 "",
                 "y: I accept   Esc: decline and exit");
    /* Полный текст LICENSE (MIT) — единственный источник юридических
     * условий проекта; показывается дословно, без пересказа. */
    static const char *const LINES[] = {
        "Copyright (c) 2026 Fobos OS contributors",
        "",
        "Permission is hereby granted, free of charge, to any person obtaining a",
        "copy of this software and associated documentation files (the",
        "\"Software\"), to deal in the Software without restriction, including",
        "without limitation the rights to use, copy, modify, merge, publish,",
        "distribute, sublicense, and/or sell copies of the Software, and to",
        "permit persons to whom the Software is furnished to do so, subject to",
        "the following conditions:",
        "",
        "The above copyright notice and this permission notice shall be",
        "included in all copies or substantial portions of the Software.",
        "",
        "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND,",
        "EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF",
        "MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND",
        "NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS",
        "BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN",
        "ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN",
        "CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE",
        "SOFTWARE.",
    };
    u32 y = uisup_box_y() + 28u;
    for (u32 i = 0; i < sizeof(LINES) / sizeof(LINES[0]); i++) {
        lgfx_text(uisup_box_x() + 16u, y, LINES[i], LGFX_COL_HEADER_TX,
                  LGFX_COL_PANEL);
        y += 11u;
    }
    for (;;) {
        int k = wait_key();
        if (k == 'y' || k == 'Y') {
            return 0;
        }
        if (k == SK_ESC || k == 'n' || k == 'N') {
            return -1;
        }
    }
}

/* Экран 3: выбор целевого диска. 0 = выбран (state->disk), -1 = отмена. */
static int screen_disks(struct setup_state *st)
{
    setup_log("step=disks");
    u8 sel = 0;
    /* Первый НЕ загрузочный диск. */
    while (sel < g_disks->count && g_disks->disks[sel].is_boot) {
        sel++;
    }
    if (sel >= g_disks->count) {
        /* Некуда устанавливать: только загрузочный носитель. */
        uisup_screen(3, SETUP_STEPS_TOTAL, "disks",
                     "Where do you want to install Fobos?",
                     "No target disk found: the only disk is the Fobos "
                     "installation media. Attach a second disk and retry.",
                     "Enter/Esc: back");
        for (;;) {
            int k = wait_key();
            if (k == SK_ESC || k == SK_ENTER) {
                return -1;
            }
        }
    }

    for (;;) {
        uisup_screen(3, SETUP_STEPS_TOTAL, "disks",
                     "Where do you want to install Fobos?",
                     "Select a disk. Size is shown in MiB. The Fobos "
                     "installation media cannot be a target.",
                     "Up/Down: select   Enter: continue   Esc: back");
        u32 y = uisup_box_y() + 60u;
        for (u8 i = 0; i < g_disks->count; i++) {
            const struct disk_info *d = &g_disks->disks[i];
            char line[80];
            char num[8];
            line[0] = 'D'; line[1] = 'i'; line[2] = 's'; line[3] = 'k';
            line[4] = ' ';
            print_u64_dec(num, sizeof(num), i);
            u32 p = 5;
            for (u32 k = 0; num[k] != '\0'; k++) {
                line[p++] = num[k];
            }
            line[p++] = ':';
            line[p++] = ' ';
            char sz[12];
            print_u64_dec(sz, sizeof(sz), sectors_to_mib(d->sectors));
            for (u32 k = 0; sz[k] != '\0'; k++) {
                line[p++] = sz[k];
            }
            const char *sfx = " MiB";
            for (u32 k = 0; sfx[k] != '\0'; k++) {
                line[p++] = sfx[k];
            }
            if (d->is_boot) {
                sfx = "   [Fobos installation media]";
            } else {
                sfx = "   [target candidate]";
            }
            for (u32 k = 0; sfx[k] != '\0' && p + 1u < sizeof(line); k++) {
                line[p++] = sfx[k];
            }
            line[p] = '\0';
            list_row(y, line, (i == sel), d->is_boot);
            y += 20u;
        }

        int k = wait_key();
        if (k == SK_ESC) {
            return -1;
        }
        if (k == SK_UP || k == SK_DOWN) {
            for (;;) {
                sel = (k == SK_UP) ? (u8)((sel + g_disks->count - 1u) % g_disks->count)
                                   : (u8)((sel + 1u) % g_disks->count);
                if (g_disks->disks[sel].is_boot == 0) {
                    break;
                }
                /* Пропускаем загрузочный носитель; защита от зацикливания —
                 * выше гарантируем наличие хотя бы одного не-boot диска. */
            }
            continue;
        }
        if (k == SK_ENTER) {
            if (g_disks->disks[sel].is_boot != 0) {
                continue;
            }
            st->disk = sel;
            layout_from_disk(st, sel);
            return 0;
        }
    }
}

/* Создание ESP/данных с записью MBR и форматированием.
 * Общая логика клавиш 'e'/'c' экрана Drive options. */
static err_t op_create_partition(struct setup_state *st, u8 type, u64 sectors)
{
    const struct disk_info *d = &g_disks->disks[st->disk];
    u32 idx = 0;
    err_t rc = layout_add(st, d->sectors, type, sectors,
                          (type == MBR_PART_EFI_SYS) ? 0x80u : 0x00u, &idx);
    if (rc != K_OK) {
        return rc;
    }
    if (ublk_bind(d->bio_handle) != 0) {
        return K_ENODEV;
    }
    rc = layout_commit(st);
    if (rc != K_OK) {
        return rc;
    }
    /* Раздел сразу форматируется: пустой FAT32 том без файлов. */
    const char *label = (type == MBR_PART_EFI_SYS) ? SETUP_LABEL_ESP
                                                   : SETUP_LABEL_DATA;
    return fmt_fat32_volume(st->lay[idx].lba, st->lay[idx].sectors, label);
}

static err_t op_format_partition(struct setup_state *st, u8 idx)
{
    const struct disk_info *d = &g_disks->disks[st->disk];
    if (ublk_bind(d->bio_handle) != 0) {
        return K_ENODEV;
    }
    const char *label = (st->lay[idx].type == MBR_PART_EFI_SYS)
                            ? SETUP_LABEL_ESP : SETUP_LABEL_DATA;
    return fmt_fat32_volume(st->lay[idx].lba, st->lay[idx].sectors, label);
}

static err_t op_delete_partition(struct setup_state *st, u8 idx)
{
    const struct disk_info *d = &g_disks->disks[st->disk];
    st->lay[idx].boot = 0u;
    st->lay[idx].type = 0u;
    st->lay[idx].lba = 0u;
    st->lay[idx].sectors = 0u;
    if (ublk_bind(d->bio_handle) != 0) {
        return K_ENODEV;
    }
    return layout_commit(st);
}

/* Строки экрана разделов; возвращает число строк (максимум 5: 4 раздела
 * + свободное место). row_types: 0 = свободное место. */
static u8 drives_rows(const struct setup_state *st, u8 *row_part_idx,
                      char rows[5][84])
{
    u8 n = 0;
    for (u8 i = 0; i < MBR_ENTRY_COUNT; i++) {
        if (st->lay[i].type == MBR_PART_EMPTY || st->lay[i].sectors == 0u) {
            continue;
        }
        char *l = rows[n];
        u32 p = 0;
        const char *pre = "Part ";
        for (u32 k = 0; pre[k] != '\0'; k++) {
            l[p++] = pre[k];
        }
        l[p++] = (char)('1' + i);
        l[p++] = ':';
        l[p++] = ' ';
        const char *tn = part_type_name(st->lay[i].type);
        for (u32 k = 0; tn[k] != '\0'; k++) {
            l[p++] = tn[k];
        }
        const char *mid = ", ";
        for (u32 k = 0; mid[k] != '\0'; k++) {
            l[p++] = mid[k];
        }
        char sz[12];
        print_u64_dec(sz, sizeof(sz), sectors_to_mib(st->lay[i].sectors));
        for (u32 k = 0; sz[k] != '\0'; k++) {
            l[p++] = sz[k];
        }
        const char *sfx = " MiB";
        for (u32 k = 0; sfx[k] != '\0' && p + 1u < 84u; k++) {
            l[p++] = sfx[k];
        }
        l[p] = '\0';
        row_part_idx[n] = i;
        n++;
    }
    /* Свободное место. */
    u64 free_sec = layout_free_sectors(st, g_disks->disks[st->disk].sectors);
    char *l = rows[n];
    u32 p = 0;
    const char *pre = "Unpartitioned space: ";
    for (u32 k = 0; pre[k] != '\0'; k++) {
        l[p++] = pre[k];
    }
    char sz[12];
    print_u64_dec(sz, sizeof(sz), sectors_to_mib(free_sec));
    for (u32 k = 0; sz[k] != '\0'; k++) {
        l[p++] = sz[k];
    }
    const char *sfx = " MiB";
    for (u32 k = 0; sfx[k] != '\0' && p + 1u < 84u; k++) {
        l[p++] = sfx[k];
    }
    l[p] = '\0';
    row_part_idx[n] = 0xFFu;   /* маркер свободного места */
    n++;
    return n;
}

/* Экран 4: Drive options (advanced). 0 = системный том выбран
 * (st->data_part, st->esp_part/esp_create), -1 = назад. */
static int screen_drives(struct setup_state *st)
{
    setup_log("step=drives");
    const struct disk_info *d = &g_disks->disks[st->disk];

    for (;;) {
        st->esp_part = (s8)layout_find_esp(st);

        uisup_screen(4, SETUP_STEPS_TOTAL, "drive options",
                     "Drive options (advanced)",
                     "Partition the disk. The system volume must be FAT32; "
                     "an EFI System Partition is required for boot files.",
                     "c:create data e:create ESP d:delete f:format "
                     "Enter:install here Esc:back");

        u8 row_part[5];
        char rows[5][84];
        u8 n = drives_rows(st, row_part, rows);

        if (st->row >= n) {
            st->row = (n > 1u) ? (u8)(n - 2u) : 0u;
        }
        u32 y = uisup_box_y() + 60u;
        for (u8 i = 0; i < n; i++) {
            list_row(y, rows[i], (i == st->row), 0);
            y += 20u;
        }
        /* Статус ESP/цели. */
        {
            char line[84];
            u32 p = 0;
            const char *pre = "ESP: ";
            for (u32 k = 0; pre[k] != '\0'; k++) {
                line[p++] = pre[k];
            }
            const char *s = (st->esp_part >= 0)
                ? "present" : "will be created on install";
            for (u32 k = 0; s[k] != '\0' && p + 1u < sizeof(line); k++) {
                line[p++] = s[k];
            }
            line[p] = '\0';
            lgfx_text(uisup_box_x() + 24u, y + 6u, line,
                      LGFX_COL_HEADER_TX, LGFX_COL_PANEL);
        }

        int k = wait_key();
        if (k == SK_ESC) {
            return -1;
        }
        if (k == SK_UP || k == SK_DOWN) {
            st->row = (k == SK_UP)
                ? (u8)((st->row + n - 1u) % n)
                : (u8)((st->row + 1u) % n);
            continue;
        }
        if (k == 'c' || k == 'e') {
            u8 type = (k == 'e') ? MBR_PART_EFI_SYS : MBR_PART_FAT32_LBA;
            if ((k == 'e') && layout_find_esp(st) >= 0) {
                uisup_error("ESP already exists on this disk");
                continue;
            }
            u64 sectors;
            if (k == 'e') {
                sectors = SETUP_ESP_SECTORS;
            } else {
                /* Запрос размера в МиБ: по умолчанию всё свободное место. */
                u64 free_mib = sectors_to_mib(
                    layout_free_sectors(st, d->sectors));
                char buf[16];
                print_u64_dec(buf, sizeof(buf), free_mib);
                uisup_screen(4, SETUP_STEPS_TOTAL, "drive options",
                             "Create data partition: size in MiB",
                             "(FAT32; minimum is defined by the FAT32 "
                             "specification, about 35 MiB)",
                             "Enter: create   Esc: back");
                uisup_edit(buf, sizeof(buf), 0);
                if (buf[0] == '\0') {
                    continue;
                }
                u64 mib = parse_u64(buf);
                if (mib == 0u) {
                    continue;
                }
                sectors = mib_to_sectors(mib);
            }
            err_t rc = op_create_partition(st, type, sectors);
            if (rc != K_OK) {
                uisup_error((rc == K_ERANGE)
                                ? "Too small or no space for this partition"
                                : "Partition create failed (I/O)");
                setup_log("phase=create FAILED");
                continue;
            }
            setup_log("phase=create ok");
            /* Курсор на созданный раздел: последняя занятая строка. */
            u8 rp[5];
            char rws[5][84];
            u8 nn = drives_rows(st, rp, rws);
            st->row = (nn > 1u) ? (u8)(nn - 2u) : 0u;
            continue;
        }
        if (k == 'd') {
            if (row_part[st->row] == 0xFFu) {
                continue;   /* свободное место удалить нельзя */
            }
            int ok = confirm_yn(uisup_box_y() + SETUP_BOX_H - 60u,
                                "Delete this partition? ALL DATA WILL BE LOST (y/n)");
            if (ok != 1) {
                continue;
            }
            err_t rc = op_delete_partition(st, row_part[st->row]);
            if (rc != K_OK) {
                uisup_error("Delete failed (I/O)");
                continue;
            }
            setup_log("phase=delete ok");
            continue;
        }
        if (k == 'f') {
            if (row_part[st->row] == 0xFFu) {
                continue;
            }
            u8 idx = row_part[st->row];
            if (st->lay[idx].type != MBR_PART_FAT32_LBA &&
                st->lay[idx].type != MBR_PART_FAT32_CHS &&
                st->lay[idx].type != MBR_PART_EFI_SYS) {
                uisup_error("Format supported for FAT32/ESP only");
                continue;
            }
            int ok = confirm_yn(uisup_box_y() + SETUP_BOX_H - 60u,
                                "Format this partition? ALL DATA WILL BE LOST (y/n)");
            if (ok != 1) {
                continue;
            }
            err_t rc = op_format_partition(st, idx);
            if (rc != K_OK) {
                uisup_error("Format failed (I/O or too small)");
                setup_log("phase=format FAILED");
                continue;
            }
            setup_log("phase=format ok");
            continue;
        }
        if (k == 'a') {
            /* Автоматическая разметка всего диска (рекомендуемый путь). */
            int ok = confirm_yn(uisup_box_y() + SETUP_BOX_H - 60u,
                                "Erase the WHOLE DISK and auto-partition? (y/n)");
            if (ok != 1) {
                continue;
            }
            struct setup_state fresh;
            for (u32 i = 0; i < sizeof(fresh.lay); i++) {
                ((u8 *)&fresh)[i] = 0u;
            }
            fresh.disk = st->disk;
            if (ublk_bind(d->bio_handle) != 0) {
                uisup_error("Bind to disk failed");
                continue;
            }
            u32 esp_idx = 0;
            u32 data_idx = 0;
            err_t rc = layout_add(&fresh, d->sectors, MBR_PART_EFI_SYS,
                                  SETUP_ESP_SECTORS, 0x80u, &esp_idx);
            if (rc == K_OK) {
                u64 free_sec = layout_free_sectors(&fresh, d->sectors);
                rc = layout_add(&fresh, d->sectors, MBR_PART_FAT32_LBA,
                                free_sec, 0x00u, &data_idx);
            }
            if (rc != K_OK) {
                uisup_error("Disk is too small for auto-layout");
                continue;
            }
            rc = layout_commit(&fresh);
            if (rc == K_OK) {
                rc = fmt_fat32_volume(fresh.lay[esp_idx].lba,
                                      fresh.lay[esp_idx].sectors,
                                      SETUP_LABEL_ESP);
            }
            if (rc == K_OK) {
                rc = fmt_fat32_volume(fresh.lay[data_idx].lba,
                                      fresh.lay[data_idx].sectors,
                                      SETUP_LABEL_DATA);
            }
            if (rc != K_OK) {
                uisup_error("Auto-layout failed (I/O)");
                setup_log("phase=auto FAILED");
                continue;
            }
            setup_log("phase=auto ok");
            *st = fresh;
            st->row = 0;
            continue;
        }
        if (k == SK_ENTER) {
            if (row_part[st->row] == 0xFFu) {
                uisup_error("Select a partition, not unpartitioned space");
                continue;
            }
            u8 idx = row_part[st->row];
            if (st->lay[idx].type != MBR_PART_FAT32_LBA &&
                st->lay[idx].type != MBR_PART_FAT32_CHS) {
                uisup_error("The system volume must be a FAT32 partition");
                continue;
            }
            st->data_part = (s8)idx;
            st->esp_create = (layout_find_esp(st) < 0) ? 1 : 0;
            if (st->esp_create != 0) {
                u64 free_sec = layout_free_sectors(st, d->sectors);
                if (free_sec < SETUP_ESP_SECTORS) {
                    uisup_error("No space left for EFI System Partition");
                    continue;
                }
            }
            return 0;
        }
    }
}

/* Экран 5: подтверждение. 0 = начать установку, -1 = назад. */
static int screen_confirm(const struct setup_state *st)
{
    setup_log("step=confirm");
    const struct mbr_part *dp = &st->lay[st->data_part];

    uisup_screen(5, SETUP_STEPS_TOTAL, "confirm",
                 "Ready to install",
                 "",
                 "y: install   Esc: back");
    u32 y = uisup_box_y() + 40u;
    char line[84];
    u32 p = 0;
    const char *pre = "Disk: ";
    for (u32 k = 0; pre[k] != '\0'; k++) { line[p++] = pre[k]; }
    char num[4];
    print_u64_dec(num, sizeof(num), st->disk);
    for (u32 k = 0; num[k] != '\0'; k++) { line[p++] = num[k]; }
    line[p] = '\0';
    lgfx_text(uisup_box_x() + 16u, y, line, LGFX_COL_TX, LGFX_COL_PANEL);
    y += 16u;

    p = 0;
    pre = "EFI System Partition: ";
    for (u32 k = 0; pre[k] != '\0' && p + 1u < sizeof(line); k++) { line[p++] = pre[k]; }
    if (st->esp_create != 0) {
        pre = "new, 64 MiB";
    } else {
        pre = "existing, will be formatted";
    }
    for (u32 k = 0; pre[k] != '\0' && p + 1u < sizeof(line); k++) { line[p++] = pre[k]; }
    line[p] = '\0';
    lgfx_text(uisup_box_x() + 16u, y, line, LGFX_COL_TX, LGFX_COL_PANEL);
    y += 16u;

    p = 0;
    pre = "Fobos system volume: ";
    for (u32 k = 0; pre[k] != '\0' && p + 1u < sizeof(line); k++) { line[p++] = pre[k]; }
    char sz[12];
    print_u64_dec(sz, sizeof(sz), sectors_to_mib(dp->sectors));
    for (u32 k = 0; sz[k] != '\0' && p + 1u < sizeof(line); k++) { line[p++] = sz[k]; }
    pre = " MiB (FAT32, will be formatted)";
    for (u32 k = 0; pre[k] != '\0' && p + 1u < sizeof(line); k++) { line[p++] = pre[k]; }
    line[p] = '\0';
    lgfx_text(uisup_box_x() + 16u, y, line, LGFX_COL_TX, LGFX_COL_PANEL);
    y += 20u;

    pre = "ALL DATA on these partitions will be ERASED.";
    lgfx_text(uisup_box_x() + 16u, y, pre, LGFX_COL_ERR, LGFX_COL_PANEL);

    for (;;) {
        int k = wait_key();
        if (k == 'y' || k == 'Y') {
            return 0;
        }
        if (k == SK_ESC || k == 'n' || k == 'N') {
            return -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Установка (экран 6) и завершение (экран 7)                          */
/* ------------------------------------------------------------------ */

static void progress_draw(u32 done, u32 total, const char *phase)
{
    u32 x = uisup_box_x() + 24u;
    u32 y = uisup_box_y() + SETUP_BOX_H - 64u;
    u32 w = uisup_box_w() - 48u;
    lgfx_fill_rect(x, y, w, 16u, LGFX_COL_FIELD);
    u32 fill = (total == 0u) ? 0u : (w * done) / total;
    if (fill > w) {
        fill = w;
    }
    lgfx_fill_rect(x, y, fill, 16u, LGFX_COL_ACCENT);
    lgfx_text(x, y - 14u, phase, LGFX_COL_TX, LGFX_COL_PANEL);
}

/* Число файлов манифеста для тома. */
static u32 manifest_count(int vol)
{
    u32 n = 0;
    for (u32 i = 0; i < SETUP_MANIFEST_N; i++) {
        if (SETUP_MANIFEST[i].vol == vol) {
            n++;
        }
    }
    return n;
}

/* Каталоги целевых томов (создаются заранее; K_EEXIST — не ошибка). */
static err_t copy_make_dirs(int vol)
{
    static const char *const esp_dirs[] = { "\\EFI", "\\EFI\\BOOT", "\\fobos", NULL };
    static const char *const data_dirs[] = { "\\fobos", NULL };
    const char *const *dirs = (vol == VOL_ESP) ? esp_dirs : data_dirs;
    for (u32 i = 0; dirs[i] != NULL; i++) {
        err_t rc = fat_dir_create(ublk_fat(), dirs[i]);
        if (rc != K_OK && rc != K_EEXIST) {
            return rc;
        }
    }
    return K_OK;
}

static err_t copy_one(const struct copy_item *item, u32 *out_bytes)
{
    u8 *buf = NULL;
    u32 size = 0;
    if (espfile_read(item->src, item->max_bytes, &buf, &size) != 0) {
        return item->required ? K_ENOENT : K_OK;   /* optional отсутствует */
    }
    err_t rc = fat_file_write(ublk_fat(), item->dst, buf, size);
    espfile_free(buf, size);
    if (rc != K_OK) {
        return rc;
    }
    *out_bytes = size;
    return K_OK;
}

static err_t do_install(struct setup_state *st, struct setup_target *out)
{
    const struct disk_info *d = &g_disks->disks[st->disk];
    err_t rc;

    uisup_screen(6, SETUP_STEPS_TOTAL, "working",
                 "Installing Fobos OS...", "", "");
    setup_log("step=working");

    if (ublk_bind(d->bio_handle) != 0) {
        setup_log("error: bind failed");
        return K_ENODEV;
    }

    /* Шаги прогресса: MBR + 2 формата + подготовка данных + файлы данных
     * + fobos.cfg x2 + файлы ESP. */
    u32 total = 3u + 1u + manifest_count(VOL_DATA) + 2u +
                manifest_count(VOL_ESP);

    progress_draw(1, total, "Writing partition table...");
    int need_mbr = 0;
    for (u32 i = 0; i < MBR_ENTRY_COUNT; i++) {
        const struct disk_partition *p = (i < d->part_count)
                                             ? &d->parts[i] : NULL;
        if (p == NULL) {
            need_mbr = (st->lay[i].type != MBR_PART_EMPTY) ? 1 : need_mbr;
            continue;
        }
        if (st->lay[i].type != p->type || st->lay[i].lba != p->lba ||
            st->lay[i].sectors != p->sectors) {
            need_mbr = 1;
        }
    }
    if (need_mbr != 0) {
        rc = layout_commit(st);
        if (rc != K_OK) {
            return rc;
        }
    }
    st->esp_part = (s8)layout_find_esp(st);

    /* 2. ESP: форматирование (существующий или созданный). */
    progress_draw(2, total, "Formatting EFI System Partition...");
    if (st->esp_part < 0) {
        setup_log("error: no ESP after layout");
        return K_EINVAL;
    }
    rc = fmt_fat32_volume(st->lay[st->esp_part].lba,
                          st->lay[st->esp_part].sectors, SETUP_LABEL_ESP);
    if (rc != K_OK) {
        setup_log("phase=format esp FAILED");
        return rc;
    }
    setup_log("phase=format esp ok");

    /* 3. Том данных. */
    progress_draw(3, total, "Formatting Fobos system volume...");
    rc = fmt_fat32_volume(st->lay[st->data_part].lba,
                          st->lay[st->data_part].sectors, SETUP_LABEL_DATA);
    if (rc != K_OK) {
        setup_log("phase=format data FAILED");
        return rc;
    }
    setup_log("phase=format data ok");

    out->esp_bio = d->bio_handle;
    out->esp_lba = st->lay[st->esp_part].lba;
    out->esp_sectors = st->lay[st->esp_part].sectors;
    out->data_bio = d->bio_handle;
    out->data_lba = st->lay[st->data_part].lba;
    out->data_sectors = st->lay[st->data_part].sectors;

    /* 4. Копирование: сначала том данных, затем ESP (файлы загрузки
     * пишутся последними: ESP сразу после копирования загрузоспособен). */
    u32 done2 = 3u;

    progress_draw(done2, total, "Preparing data volume...");
    if (ublk_bind(out->data_bio) != 0) {
        return K_ENODEV;
    }
    rc = fat_mount(ublk_fat(), out->data_lba, out->data_sectors);
    if (rc != K_OK) {
        setup_log("error: data mount failed");
        return rc;
    }
    rc = copy_make_dirs(VOL_DATA);
    if (rc != K_OK) {
        setup_log("error: data mkdir failed");
        return rc;
    }
    done2++;

    for (u32 i = 0; i < SETUP_MANIFEST_N; i++) {
        if (SETUP_MANIFEST[i].vol != VOL_DATA) {
            continue;
        }
        progress_draw(done2, total, "Copying files to data volume...");
        u32 bytes = 0;
        rc = copy_one(&SETUP_MANIFEST[i], &bytes);
        if (rc != K_OK) {
            setup_log("error: data copy failed");
            setup_log_u64("error: item #", i, "");
            return rc;
        }
        setup_log_u64("copy: data bytes=", bytes, "");
        done2++;
    }
    (void)ublk_flush();

    /* fobos.cfg: с носителя или значение по умолчанию; на оба тома. */
    progress_draw(done2, total, "Writing configuration...");
    u8 *cfg = NULL;
    u32 cfg_size = 0;
    int have_media_cfg = (espfile_read("\\fobos\\fobos.cfg", 4096u,
                                       &cfg, &cfg_size) == 0);
    if (have_media_cfg == 0) {
        cfg = (u8 *)(u64)SETUP_FOBOS_CFG_DEFAULT;
        cfg_size = (u32)(sizeof(SETUP_FOBOS_CFG_DEFAULT) - 1u);
    }

    if (ublk_bind(out->esp_bio) != 0) {
        if (have_media_cfg != 0) {
            espfile_free(cfg, cfg_size);
        }
        return K_ENODEV;
    }
    rc = fat_mount(ublk_fat(), out->esp_lba, out->esp_sectors);
    if (rc != K_OK) {
        setup_log("error: esp mount failed");
        if (have_media_cfg != 0) {
            espfile_free(cfg, cfg_size);
        }
        return rc;
    }
    rc = copy_make_dirs(VOL_ESP);
    if (rc != K_OK) {
        setup_log("error: esp mkdir failed");
        if (have_media_cfg != 0) {
            espfile_free(cfg, cfg_size);
        }
        return rc;
    }
    rc = fat_file_write(ublk_fat(), "\\fobos\\fobos.cfg", cfg, cfg_size);
    if (rc != K_OK) {
        setup_log("error: esp fobos.cfg failed");
        if (have_media_cfg != 0) {
            espfile_free(cfg, cfg_size);
        }
        return rc;
    }
    setup_log("cfg: fobos.cfg written (esp)");
    done2++;

    if (ublk_bind(out->data_bio) != 0) {
        if (have_media_cfg != 0) {
            espfile_free(cfg, cfg_size);
        }
        return K_ENODEV;
    }
    rc = fat_mount(ublk_fat(), out->data_lba, out->data_sectors);
    if (rc == K_OK) {
        rc = fat_file_write(ublk_fat(), "\\fobos\\fobos.cfg", cfg, cfg_size);
    }
    if (rc != K_OK) {
        setup_log("error: data fobos.cfg failed");
        if (have_media_cfg != 0) {
            espfile_free(cfg, cfg_size);
        }
        return rc;
    }
    setup_log("cfg: fobos.cfg written (data)");
    if (have_media_cfg != 0) {
        espfile_free(cfg, cfg_size);
    }
    done2++;

    /* 5. Файлы загрузки на ESP. */
    if (ublk_bind(out->esp_bio) != 0) {
        return K_ENODEV;
    }
    rc = fat_mount(ublk_fat(), out->esp_lba, out->esp_sectors);
    if (rc != K_OK) {
        return rc;
    }
    for (u32 i = 0; i < SETUP_MANIFEST_N; i++) {
        if (SETUP_MANIFEST[i].vol != VOL_ESP) {
            continue;
        }
        progress_draw(done2, total, "Copying boot files...");
        u32 bytes = 0;
        rc = copy_one(&SETUP_MANIFEST[i], &bytes);
        if (rc != K_OK) {
            setup_log("error: esp copy failed");
            setup_log_u64("error: item #", i, "");
            return rc;
        }
        setup_log_u64("copy: esp bytes=", bytes, "");
        done2++;
    }
    (void)ublk_flush();

    setup_log("install complete");
    return K_OK;
}

/* Экран 7: завершение. 1 = перезагрузка (Enter), 0 = продолжить загрузку. */
static int screen_done(void)
{
    setup_log("step=done");
    uisup_screen(7, SETUP_STEPS_TOTAL, "done",
                 "Installation complete",
                 "Remove the installation media before restarting. "
                 "The system will boot from the new disk.",
                 "Enter: restart now   Esc: continue boot");
    for (;;) {
        int k = wait_key();
        if (k == SK_ESC) {
            return 0;
        }
        if (k == SK_ENTER) {
            return 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Публичный API                                                       */
/* ------------------------------------------------------------------ */

static int setup_target_write_cfg(const struct setup_target *t,
                                  const u8 *cfg_raw, u32 cfg_size);

int setup_run(struct setup_target *out, const u8 *cfg_raw, u32 cfg_size)
{
    if (out == NULL) {
        return -1;
    }
    for (u32 i = 0; i < sizeof(*out); i++) {
        ((u8 *)out)[i] = 0u;
    }
    if (lgfx_width() == 0u || lgfx_height() == 0u) {
        return -1;   /* графика обязательна */
    }
    uisup_layout(SETUP_BOX_W, SETUP_BOX_H);

    struct disk_list local;
    if (disk_enumerate(&local) != 0) {
        setup_log("error: disk enumeration failed");
        return -1;
    }
    g_disks = disk_cached();

    struct setup_state st;
    for (u32 i = 0; i < sizeof(st); i++) {
        ((u8 *)&st)[i] = 0u;
    }
    st.esp_part = -1;
    st.data_part = -1;

    if (screen_welcome() != 0) {
        return -1;
    }
    if (screen_license() != 0) {
        setup_log("license: declined");
        return -1;
    }
    if (screen_disks(&st) != 0) {
        return -1;
    }
    if (screen_drives(&st) != 0) {
        return -1;
    }
    if (screen_confirm(&st) != 0) {
        return -1;
    }

    err_t rc = do_install(&st, out);
    if (rc != K_OK) {
        setup_log_u64("error: install failed rc=", (u64)(-rc), "");
        uisup_screen(6, SETUP_STEPS_TOTAL, "working",
                     "Installation failed",
                     "See the log on the serial port. The target disk may "
                     "be in a partially written state.",
                     "Enter/Esc: continue");
        for (;;) {
            int k = wait_key();
            if (k == SK_ESC || k == SK_ENTER) {
                break;
            }
        }
        return -1;
    }

    out->completed = 1;
    /* Перенос конфига (пользователи/hostname) на ESP установленной
     * системы — ДО подсказки перезагрузки: выбор на экране завершения
     * (перезагрузка/продолжение) больше не влияет на перенос, и на
     * реальном железе выключение прямо на этом экране безопасно. */
    if (setup_target_write_cfg(out, cfg_raw, cfg_size) != 0) {
        setup_log("error: target setup.cfg write failed (will run OOBE on installed system)");
    }
    out->restart = screen_done();
    return 0;
}

/* Пишет setup.cfg на ESP установленной системы. Вызывается из setup_run
 * перед экраном завершения; cfg_raw == NULL — шаг пропускается (целевой
 * диск получит конфиг при первой загрузке с него, через OOBE). */
static int setup_target_write_cfg(const struct setup_target *t,
                                  const u8 *cfg_raw, u32 cfg_size)
{
    if (t == NULL || t->completed == 0) {
        return -1;
    }
    if (cfg_raw == NULL || cfg_size == 0u) {
        return 0;   /* конфига нет: целевой диск получит его при OOBE */
    }
    if (ublk_bind(t->esp_bio) != 0) {
        return -1;
    }
    err_t rc = fat_mount(ublk_fat(), t->esp_lba, t->esp_sectors);
    if (rc != K_OK) {
        return -1;
    }
    rc = fat_file_write(ublk_fat(), "\\fobos\\setup.cfg", cfg_raw, cfg_size);
    if (rc != K_OK) {
        return -1;
    }
    (void)ublk_flush();
    setup_log_u64("target: setup.cfg written bytes=", cfg_size, "");
    return 0;
}
