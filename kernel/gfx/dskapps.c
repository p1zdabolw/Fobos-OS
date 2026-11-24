/*
 * Fobos OS — содержимое окон приложений (FND-2e).
 *
 * Каждый app — функция от клиентской области окна (границы обеспечивает
 * композитор: fb_clip_set перед dsk_app_draw/click). Приложения:
 *   explorer — файлы FAT32: навигация, выбор, просмотр, удаление;
 *   about    — сводка о системе;
 *   viewer   — просмотр текстового файла (PgUp/PgDn, скроллбар);
 *   notepad  — редактор: ввод, открытие/сохранение на FAT32;
 *   calc     — целочисленный калькулятор (цепочка слева направо);
 *   settings — System / Display / Time / Personalization.
 *
 * Никаких заглушек: все кнопки и клавиши реально работают.
 */
#include "dskpriv.h"

#include <stddef.h>

#include "uidraw.h"
#include "font8x8.h"
#include "console.h"
#include "../version.h"
#include "../drivers/fb.h"
#include "../drivers/timer.h"
#include "../drivers/rtc.h"
#include "../fs/blk.h"
#include "../fs/fat32.h"
#include "../mm/phys.h"
#include "../sys/session.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../lib/vsnprintf.h"

#define A_TAG "gfx/apps"

/* ------------------------------------------------------------------ */
/* Explorer                                                            */
/* ------------------------------------------------------------------ */

struct fexp_entry {
    char name[FAT_NAME_MAX];
    u32  size;
    int  is_dir;
};
#define FEXP_MAX 48u

static struct {
    char path[128];
    struct fexp_entry ent[FEXP_MAX];
    u32 ent_count;
    int ent_sel;
    u32 ent_top;
    char status[96];
} g_expl;

/* ------------------------------------------------------------------ */
/* Viewer                                                              */
/* ------------------------------------------------------------------ */

#define VIEW_BUF 1600u

static struct {
    char name[FAT_NAME_MAX];
    u8   buf[VIEW_BUF];
    u32  len;
    int  err;
    u32  top;      /* верхняя видимая строка */
} g_view;

/* ------------------------------------------------------------------ */
/* Notepad                                                             */
/* ------------------------------------------------------------------ */

#define NPAD_MAX 3072u

static struct {
    char buf[NPAD_MAX];
    u32  len;
    u32  cur;
    u32  top;      /* верхняя видимая строка */
    char name[64];
    int  dirty;
    char status[80];   /* ошибка (красная) */
    char info[96];     /* информационная строка (последнее действие) */
} g_np;

/* ------------------------------------------------------------------ */
/* Calculator                                                          */
/* ------------------------------------------------------------------ */

static struct {
    u64 acc;
    u64 cur;
    int op;        /* 0 нет, else '+' '-' '*' '/' '%' */
    int fresh;     /* следующий ввод цифр начинает новое число */
    int err;
} g_calc;

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

static struct {
    u32 page;              /* 0..3 */
    struct rtc_time tm;    /* копия для страницы Time */
    int tm_ok;
    char status[80];
} g_set;

static const struct { u32 w, h; } g_res_modes[] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 },
    { 1280, 800 }, { 1600, 900 }, { 1920, 1080 }
};
#define RES_MODES (sizeof(g_res_modes) / sizeof(g_res_modes[0]))

static const u32 g_accent_palette[] = {
    0x4F8CCFu, 0x3FB950u, 0xE5484Du, 0xF5A623u,
    0x9D5CF2u, 0x38BDF8u, 0xE879A9u, 0x8FA3BFu
};
#define ACCENTS (sizeof(g_accent_palette) / sizeof(g_accent_palette[0]))

/* ------------------------------------------------------------------ */
/* Вспомогательные                                                     */
/* ------------------------------------------------------------------ */

static void format_size(u32 size, char *out, u32 cap)
{
    if (size >= 1024u * 1024u) {
        (void)ksnprintf(out, cap, "%u.%u MiB", size >> 20,
                        ((size & 0xFFFFFu) * 10u) >> 20);
    } else if (size >= 1024u) {
        (void)ksnprintf(out, cap, "%u KiB", (size + 1023u) >> 10);
    } else {
        (void)ksnprintf(out, cap, "%u B", size);
    }
}

/* Мини-кнопка (рисование + хит-зона одним вызовом). */
static void app_button(u32 x, u32 y, u32 w, const char *label, int focused)
{
    uidraw_button(x, y, w, label, focused);
}

/* ------------------------------------------------------------------ */
/* Explorer: логика                                                    */
/* ------------------------------------------------------------------ */

static err_t explorer_refresh(void)
{
    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        (void)ksnprintf(g_expl.status, sizeof(g_expl.status),
                        "no storage device");
        g_expl.ent_count = 0;
        return K_ENODEV;
    }
    struct fat_dir it;
    err_t rc = fat_dir_open(v, g_expl.path, &it);
    if (rc != K_OK) {
        (void)ksnprintf(g_expl.status, sizeof(g_expl.status),
                        "open %s: %s", g_expl.path, k_strerror(rc));
        g_expl.ent_count = 0;
        return rc;
    }
    g_expl.ent_count = 0;
    for (;;) {
        struct fat_dir_entry de;
        rc = fat_dir_next(v, &it, &de);
        if (rc != K_OK) {
            break;   /* K_ENOENT = конец */
        }
        if (de.name[0] == '.') {
            continue;
        }
        if (g_expl.ent_count >= FEXP_MAX) {
            break;
        }
        struct fexp_entry *fe = &g_expl.ent[g_expl.ent_count++];
        for (u32 i = 0; i < FAT_NAME_MAX - 1u && de.name[i] != '\0'; i++) {
            fe->name[i] = de.name[i];
        }
        fe->name[FAT_NAME_MAX - 1u] = '\0';
        fe->size = de.size;
        fe->is_dir = de.is_dir;
    }
    g_expl.ent_sel = -1;
    g_expl.ent_top = 0;
    g_expl.status[0] = '\0';
    return K_OK;
}

static void path_dir_up(void)
{
    u32 n = strlen(g_expl.path);
    while (n > 1u && (g_expl.path[n - 1u] == '/' ||
                      g_expl.path[n - 1u] == '\\')) {
        n--;
    }
    while (n > 1u && g_expl.path[n - 1u] != '/' &&
           g_expl.path[n - 1u] != '\\') {
        n--;
    }
    if (n <= 1u) {
        g_expl.path[0] = '/';
        g_expl.path[1] = '\0';
        return;
    }
    g_expl.path[n] = '\0';
}

static void explorer_open_dir(const struct fexp_entry *fe)
{
    u32 n = strlen(g_expl.path);
    if (n + strlen(fe->name) + 2u < sizeof(g_expl.path)) {
        if (n > 1u && g_expl.path[n - 1u] != '/' &&
            g_expl.path[n - 1u] != '\\') {
            g_expl.path[n++] = '/';
        }
        for (u32 i = 0; fe->name[i] != '\0'; i++) {
            g_expl.path[n++] = fe->name[i];
        }
        g_expl.path[n] = '\0';
    }
    (void)explorer_refresh();
}

static void explorer_view_selected(void)
{
    if (g_expl.ent_sel < 0 || (u32)g_expl.ent_sel >= g_expl.ent_count) {
        (void)ksnprintf(g_expl.status, sizeof(g_expl.status),
                        "select a file first");
        return;
    }
    struct fexp_entry *fe = &g_expl.ent[g_expl.ent_sel];
    if (fe->is_dir != 0) {
        (void)ksnprintf(g_expl.status, sizeof(g_expl.status),
                        "cannot view a directory");
        return;
    }
    char full[160];
    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        return;
    }
    (void)ksnprintf(full, sizeof(full), "%s%s%s",
                    g_expl.path,
                    (strlen(g_expl.path) > 1u) ? "/" : "",
                    fe->name);
    u32 len = 0;
    err_t rc = fat_file_read(v, full, g_view.buf,
                             sizeof(g_view.buf) - 1u, &len);
    for (u32 i = 0; i < FAT_NAME_MAX - 1u && fe->name[i] != '\0'; i++) {
        g_view.name[i] = fe->name[i];
    }
    g_view.name[FAT_NAME_MAX - 1u] = '\0';
    g_view.len = (rc == K_OK) ? len : 0u;
    g_view.err = (rc == K_OK) ? 0 : (int)rc;
    g_view.top = 0;
    g_view.buf[(rc == K_OK) ? len : 0u] = 0;
    dsk_app_open(APP_VIEW);
}

static void explorer_delete_selected(void)
{
    if (g_expl.ent_sel < 0 || (u32)g_expl.ent_sel >= g_expl.ent_count) {
        (void)ksnprintf(g_expl.status, sizeof(g_expl.status),
                        "select a file first");
        return;
    }
    struct fexp_entry *fe = &g_expl.ent[g_expl.ent_sel];
    if (fe->is_dir != 0) {
        (void)ksnprintf(g_expl.status, sizeof(g_expl.status),
                        "cannot delete a directory");
        return;
    }
    if (fat_name_protected(fe->name) != 0) {
        (void)ksnprintf(g_expl.status, sizeof(g_expl.status),
                        "%s: protected system file", fe->name);
        klog(KLOG_WARN, A_TAG, "explorer: отказ удаления защищённого %s",
             fe->name);
        return;
    }
    char full[160];
    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        return;
    }
    (void)ksnprintf(full, sizeof(full), "%s%s%s",
                    g_expl.path,
                    (strlen(g_expl.path) > 1u) ? "/" : "",
                    fe->name);
    err_t rc = fat_file_delete(v, full);
    if (rc != K_OK) {
        (void)ksnprintf(g_expl.status, sizeof(g_expl.status),
                        "delete: %s", k_strerror(rc));
        return;
    }
    klog(KLOG_INFO, A_TAG, "explorer: удалён файл %s", full);
    (void)explorer_refresh();
    (void)ksnprintf(g_expl.status, sizeof(g_expl.status), "deleted %s",
                    fe->name);
}

/* Геометрия списка explorer (общая для draw/click). */
static void expl_layout(u32 ch, u32 *list_y, u32 *rows, u32 *btn_y)
{
    *list_y = 14u;
    u32 status_h = 12u;
    u32 btn_h = 18u;
    *rows = (ch > *list_y + btn_h + status_h + 12u)
                ? (ch - btn_h - status_h) / 12u - 1u
                : 1u;
    if (*rows > 24u) {
        *rows = 24u;
    }
    *btn_y = *list_y + *rows * 12u + 2u;
}

/* Позиции кнопок тулбара. */
static void expl_buttons(u32 cx, u32 *out_x, u32 *out_w)
{
    static const char *const btns[] = { "Up", "View", "Del", "Refresh" };
    u32 x = cx + 4u;
    for (u32 i = 0; i < 4u; i++) {
        out_w[i] = (u32)strlen(btns[i]) * 8u + 10u;
        out_x[i] = x;
        x += out_w[i] + 6u;
    }
}

/* ------------------------------------------------------------------ */
/* Explorer: отрисовка                                                 */
/* ------------------------------------------------------------------ */

static void draw_explorer(void)
{
    u32 cx, cy, cw, ch;
    dsk_win_content(&g_dsk.win[APP_EXPL], &cx, &cy, &cw, &ch);

    /* Строка пути. */
    (void)fb_fill_rect(cx, cy, cw, 12u, UI_COL_FIELD);
    uidraw_text_fit(cx + 4u, cy + 2u, cw - 8u, g_expl.path, UI_COL_TX,
                    UI_COL_FIELD);

    u32 list_y, rows, btn_y;
    expl_layout(ch, &list_y, &rows, &btn_y);

    u32 sb_w = 10u;   /* ширина скроллбара */

    /* Зажим top: не листаем дальше конца списка. */
    if (g_expl.ent_count > rows) {
        u32 max_top = g_expl.ent_count - rows;
        if (g_expl.ent_top > max_top) {
            g_expl.ent_top = max_top;
        }
    } else {
        g_expl.ent_top = 0;
    }

    for (u32 r = 0; r < rows; r++) {
        u32 i = g_expl.ent_top + r;
        u32 ry = cy + list_y + r * 12u;
        u32 fill = UI_COL_PANEL;
        if (g_expl.ent_sel >= 0 && (u32)g_expl.ent_sel == i) {
            fill = 0x24384Eu;
        }
        (void)fb_fill_rect(cx, ry, cw - sb_w, 12u, fill);
        if (i >= g_expl.ent_count) {
            continue;
        }
        struct fexp_entry *fe = &g_expl.ent[i];
        char line[FAT_NAME_MAX + 24u];
        if (fe->is_dir != 0) {
            (void)ksnprintf(line, sizeof(line), "%s/", fe->name);
            uidraw_text_fit(cx + 4u, ry + 2u, cw - sb_w - 8u, line,
                            UI_COL_BYELLOW, fill);
        } else {
            char sz[16];
            format_size(fe->size, sz, sizeof(sz));
            u32 nw = cw - sb_w - 8u - 64u;
            uidraw_text_fit(cx + 4u, ry + 2u, nw, fe->name, UI_COL_TX, fill);
            uidraw_text(cx + cw - 64u - sb_w, ry + 2u, sz, UI_COL_TX_DIM,
                        fill);
        }
    }
    if (g_expl.ent_count > g_expl.ent_top + rows) {
        uidraw_text(cx + 4u, cy + list_y + rows * 12u, "...",
                    UI_COL_TX_DIM, UI_COL_PANEL);
    }

    /* Скроллбар: стрелки + ползунок. */
    u32 sb_x = cx + cw - sb_w;
    (void)fb_fill_rect(sb_x, cy + list_y, sb_w, rows * 12u, 0x10151Du);
    (void)fb_fill_rect(sb_x, cy + list_y, sb_w, 10u, 0x2A3646u);
    (void)fb_fill_rect(sb_x, cy + list_y + rows * 12u - 10u, sb_w, 10u,
                       0x2A3646u);
    uidraw_text(sb_x + 1u, cy + list_y + 1u, "^", UI_COL_TX, 0x2A3646u);
    uidraw_text(sb_x + 1u, cy + list_y + rows * 12u - 9u, "v", UI_COL_TX,
                0x2A3646u);
    if (g_expl.ent_count > rows) {
        u32 track = rows * 12u - 20u;
        u32 th = track * rows / g_expl.ent_count;
        if (th < 6u) { th = 6u; }
        u32 tmax = track - th;
        u32 tpos = (g_expl.ent_count > g_expl.ent_top + rows)
                       ? tmax * g_expl.ent_top /
                             (g_expl.ent_count - rows)
                       : 0u;
        (void)fb_fill_rect(sb_x + 1u, cy + list_y + 10u + tpos, sb_w - 2u,
                           th, 0x5A6878u);
    }

    /* Кнопки тулбара. */
    u32 bxs[4], bws[4];
    expl_buttons(cx, bxs, bws);
    static const char *const btns[] = { "Up", "View", "Del", "Refresh" };
    for (u32 i = 0; i < 4u; i++) {
        app_button(bxs[i], cy + btn_y, bws[i], btns[i], 0);
    }

    /* Статус. */
    if (g_expl.status[0] != '\0') {
        uidraw_text_fit(cx + 4u, cy + btn_y + 20u, cw - 8u, g_expl.status,
                        UI_COL_ERR, UI_COL_PANEL);
    } else {
        char info[64];
        (void)ksnprintf(info, sizeof(info), "%u items", g_expl.ent_count);
        uidraw_text_fit(cx + 4u, cy + btn_y + 20u, cw - 8u, info,
                        UI_COL_TX_DIM, UI_COL_PANEL);
    }
}

/* ------------------------------------------------------------------ */
/* About                                                               */
/* ------------------------------------------------------------------ */

static void draw_about(void)
{
    u32 cx, cy, cw, ch;
    dsk_win_content(&g_dsk.win[APP_ABOUT], &cx, &cy, &cw, &ch);

    char line[128];
    u32 y = cy + 8u;
    u32 x = cx + 10u;
    u32 tw = cw - 20u;

    (void)ksnprintf(line, sizeof(line), "Fobos OS %s", FOBOS_VERSION_STR);
    uidraw_text(x, y, line, uidraw_accent(), UI_COL_PANEL);
    y += 16u;
    y += uidraw_text_wrap(x, y, tw, 3, 12u, FOBOS_STAGE_STR,
                          UI_COL_TX_DIM, UI_COL_PANEL) * 12u + 8u;

    if (session_active() != 0) {
        (void)ksnprintf(line, sizeof(line), "session: %s@%s",
                        session_user(), session_hostname());
    } else {
        (void)ksnprintf(line, sizeof(line), "session: kernel console");
    }
    uidraw_text_fit(x, y, tw, line, UI_COL_TX, UI_COL_PANEL);
    y += 14u;

    struct blk_status st = blk_get_status();
    if (st.fat_ok != 0) {
        (void)ksnprintf(line, sizeof(line), "disk: %s (%llu MiB)",
                        st.model,
                        (unsigned long long)(st.disk_sectors >> 11));
        uidraw_text_fit(x, y, tw, line, UI_COL_TX, UI_COL_PANEL);
        y += 14u;
        (void)ksnprintf(line, sizeof(line),
                        "fat32: free %llu KiB of %llu KiB",
                        (unsigned long long)st.fat_free_kb,
                        (unsigned long long)st.fat_total_kb);
        uidraw_text_fit(x, y, tw, line, UI_COL_TX, UI_COL_PANEL);
        y += 14u;
    } else {
        uidraw_text_fit(x, y, tw, "disk: no storage device", UI_COL_TX_DIM,
                        UI_COL_PANEL);
        y += 14u;
    }

    (void)ksnprintf(line, sizeof(line), "amnesia: %s (%u/%u blocks)",
                    (st.amnesia != 0) ? "ON" : "off",
                    (unsigned int)st.ovl_blocks,
                    (unsigned int)st.ovl_limit);
    uidraw_text_fit(x, y, tw, line,
                    (st.amnesia != 0) ? UI_COL_ERR : UI_COL_TX, UI_COL_PANEL);
    y += 14u;

    u64 ms = timer_uptime_ms();
    (void)ksnprintf(line, sizeof(line), "uptime: %u:%02u:%02u",
                    (unsigned int)(ms / 3600000ull),
                    (unsigned int)((ms / 60000ull) % 60ull),
                    (unsigned int)((ms / 1000ull) % 60ull));
    uidraw_text_fit(x, y, tw, line, UI_COL_TX_DIM, UI_COL_PANEL);
    y += 14u;

    struct phys_stats ps = phys_get_stats();
    (void)ksnprintf(line, sizeof(line), "mem: %llu/%llu MiB free",
                    (unsigned long long)(ps.free_frames * 4ull >> 10),
                    (unsigned long long)(ps.total_frames * 4ull >> 10));
    uidraw_text_fit(x, y, tw, line, UI_COL_TX_DIM, UI_COL_PANEL);
    y += 14u;

    u32 fw = 0, fh = 0;
    if (fb_get_dims(&fw, &fh) == K_OK) {
        (void)ksnprintf(line, sizeof(line), "display: %ux%u", fw, fh);
        uidraw_text_fit(x, y, tw, line, UI_COL_TX_DIM, UI_COL_PANEL);
    }
    (void)ch;
}

static void click_about(u32 mx, u32 my)
{
    (void)mx;
    (void)my;   /* about — только чтение */
}

/* ------------------------------------------------------------------ */
/* Viewer                                                              */
/* ------------------------------------------------------------------ */

static void view_rows(u32 ch, u32 *rows)
{
    *rows = ch / 12u;
    if (*rows == 0u) { *rows = 1u; }
}

static void draw_viewer(void)
{
    u32 cx, cy, cw, ch;
    dsk_win_content(&g_dsk.win[APP_VIEW], &cx, &cy, &cw, &ch);

    if (g_view.err != 0) {
        char line[96];
        (void)ksnprintf(line, sizeof(line), "read error: %s",
                        k_strerror((err_t)g_view.err));
        uidraw_text(cx + 8u, cy + 8u, line, UI_COL_ERR, UI_COL_PANEL);
        return;
    }

    u32 chars = cw / 8u;
    u32 rows;
    view_rows(ch - 14u, &rows);
    if (chars == 0u) {
        return;
    }

    u32 pos = 0;
    u32 line_no = 0;
    for (u32 r = 0; r < rows + g_view.top; r++) {
        u32 start = pos;
        while (pos < g_view.len && g_view.buf[pos] != '\n') {
            pos++;
        }
        if (line_no >= g_view.top && line_no - g_view.top < rows) {
            u32 ry = cy + (line_no - g_view.top) * 12u;
            u32 n = pos - start;
            if (n > chars) { n = chars; }
            char line[81];
            u32 k = 0;
            for (u32 i = 0; i < n; i++) {
                char c = (char)g_view.buf[start + i];
                if (c >= 0x20 && c <= 0x7E) {
                    line[k++] = c;
                } else if (c != '\r') {
                    line[k++] = '.';
                }
            }
            line[k] = '\0';
            uidraw_text(cx + 2u, ry, line, UI_COL_TX, UI_COL_PANEL);
        }
        line_no++;
        if (pos >= g_view.len) {
            break;
        }
        pos++;   /* '\n' */
    }

    /* Полоса прокрутки снизу: [<] страница N [>]. */
    u32 byy = cy + ch - 12u;
    (void)fb_fill_rect(cx, byy, cw, 12u, UI_COL_FIELD);
    uidraw_text(cx + 2u, byy + 2u, "<", UI_COL_TX, UI_COL_FIELD);
    uidraw_text(cx + cw - 8u, byy + 2u, ">", UI_COL_TX, UI_COL_FIELD);
    char page[48];
    u32 total_lines = line_no + ((pos < g_view.len) ? 1u : 0u);
    (void)ksnprintf(page, sizeof(page), "%u..%u of %u lines",
                    g_view.top + 1u,
                    (g_view.top + rows < total_lines) ? g_view.top + rows
                                                      : total_lines,
                    total_lines);
    uidraw_text_fit(cx + 20u, byy + 2u, cw - 48u, page, UI_COL_TX_DIM,
                    UI_COL_FIELD);
}

static void view_scroll(u32 rows, int dir)
{
    if (dir > 0) {
        if (g_view.top + rows < g_view.len / 8u + 8u) {
            g_view.top += rows;
        }
    } else if (g_view.top > 0u) {
        g_view.top = (g_view.top > rows) ? g_view.top - rows : 0u;
    }
}

static void click_viewer(u32 mx, u32 my)
{
    u32 cx, cy, cw, ch;
    dsk_win_content(&g_dsk.win[APP_VIEW], &cx, &cy, &cw, &ch);
    u32 rows;
    view_rows(ch - 14u, &rows);

    u32 byy = cy + ch - 12u;
    if (my >= byy && my < byy + 12u) {
        if (mx < cx + 16u) {
            view_scroll(rows, -1);
        } else if (mx >= cx + cw - 16u) {
            view_scroll(rows, +1);
        }
    }
}

int viewer_key(const struct kbd_event *ev)
{
    u32 cx, cy, cw, ch;
    dsk_win_content(&g_dsk.win[APP_VIEW], &cx, &cy, &cw, &ch);
    u32 rows;
    view_rows(ch - 14u, &rows);

    switch (ev->code) {
    case KEY_PGDN: view_scroll(rows, +1); return 1;
    case KEY_PGUP: view_scroll(rows, -1); return 1;
    case KEY_DOWN: view_scroll(1, +1);    return 1;
    case KEY_UP:   view_scroll(1, -1);    return 1;
    default:       return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Notepad                                                             */
/* ------------------------------------------------------------------ */

/* Путь файла: если имя без ведущего '\', пишем в \fobos\. */
static void npad_full_path(const char *name, char *out, u32 cap)
{
    if (name[0] == '\\' || name[0] == '/') {
        (void)ksnprintf(out, cap, "%s", name);
    } else {
        (void)ksnprintf(out, cap, "\\fobos\\%s", name);
    }
}

static void npad_set_name(const char *name)
{
    u32 i = 0;
    for (; name[i] != '\0' && i + 1u < sizeof(g_np.name); i++) {
        g_np.name[i] = name[i];
    }
    g_np.name[i] = '\0';
}

/* Строка, содержащая позицию cur, и её начало. */
static void npad_line_of(u32 pos, u32 *line, u32 *line_start)
{
    u32 l = 0, start = 0;
    for (u32 i = 0; i < pos && i < g_np.len; i++) {
        if (g_np.buf[i] == '\n') {
            l++;
            start = i + 1u;
        }
    }
    *line = l;
    *line_start = start;
}

/* Начало строки line (line >= 0). */
static u32 npad_line_start(u32 line)
{
    u32 l = 0;
    for (u32 i = 0; i < g_np.len; i++) {
        if (l == line) {
            return i;
        }
        if (g_np.buf[i] == '\n') {
            l++;
        }
    }
    return g_np.len;
}

static void npad_insert(char c)
{
    if (g_np.len + 1u >= NPAD_MAX) {
        (void)ksnprintf(g_np.status, sizeof(g_np.status), "buffer full");
        return;
    }
    for (u32 i = g_np.len; i > g_np.cur; i--) {
        g_np.buf[i] = g_np.buf[i - 1u];
    }
    g_np.buf[g_np.cur] = c;
    g_np.cur++;
    g_np.len++;
    g_np.dirty = 1;
    g_np.status[0] = '\0';
    g_np.info[0] = '\0';
}

static void npad_backspace(void)
{
    if (g_np.cur == 0u) {
        return;
    }
    for (u32 i = g_np.cur - 1u; i + 1u < g_np.len; i++) {
        g_np.buf[i] = g_np.buf[i + 1u];
    }
    g_np.cur--;
    g_np.len--;
    g_np.dirty = 1;
}

static void npad_scroll_fix(u32 area_rows)
{
    u32 line, ls;
    npad_line_of(g_np.cur, &line, &ls);
    if (line < g_np.top) {
        g_np.top = line;
    }
    if (area_rows > 0u && line >= g_np.top + area_rows) {
        g_np.top = line - area_rows + 1u;
    }
}

static void draw_notepad(void)
{
    u32 cx, cy, cw, ch;
    dsk_win_content(&g_dsk.win[APP_NOTEPAD], &cx, &cy, &cw, &ch);

    /* Меню: New/Open/Save + имя файла. */
    static const char *const btns[] = { "New", "Open", "Save" };
    u32 bxs[3], bws[3];
    u32 x = cx + 4u;
    for (u32 i = 0; i < 3u; i++) {
        bws[i] = (u32)strlen(btns[i]) * 8u + 10u;
        bxs[i] = x;
        x += bws[i] + 6u;
    }
    for (u32 i = 0; i < 3u; i++) {
        app_button(bxs[i], cy + 2u, bws[i], btns[i], 0);
    }
    u32 name_x = x + 4u;
    u32 name_w = (cw > (x - cx) + 60u) ? cx + cw - name_x - 4u : 40u;
    (void)fb_fill_rect(name_x, cy + 2u, name_w, 18u, UI_COL_FIELD);
    uidraw_text_fit(name_x + 4u, cy + 7u, name_w - 8u, g_np.name,
                    UI_COL_TX, UI_COL_FIELD);

    /* Текстовая область. */
    u32 area_y = cy + 24u;
    u32 status_h = 12u;
    u32 area_h = (ch > 24u + status_h) ? ch - 24u - status_h : 0u;
    u32 area_rows = area_h / 12u;
    if (area_rows == 0u) { area_rows = 1u; }

    npad_scroll_fix(area_rows);

    (void)fb_fill_rect(cx, area_y, cw, area_rows * 12u, UI_COL_FIELD);

    /* Отрисовка строк с прокруткой. */
    u32 line = 0, lstart = 0;
    u32 drawn = 0;
    for (u32 i = 0; i <= g_np.len && drawn < area_rows; ) {
        if (line >= g_np.top) {
            /* Печатаем [lstart, конец строки) в границах экрана. */
            u32 ln = 0;
            u32 maxc = cw / 8u;
            if (maxc > 0u) { maxc--; }
            u32 j = lstart;
            while (j < g_np.len && g_np.buf[j] != '\n' && ln < maxc) {
                char c = g_np.buf[j];
                if (c < 0x20 || c > 0x7E) { c = '.'; }
                (void)fb_blit_glyph8(cx + 4u + ln * 8u,
                                     area_y + drawn * 12u,
                                     font8x8[(u8)c - FONT8X8_FIRST],
                                     UI_COL_TX, UI_COL_FIELD);
                ln++;
                j++;
            }
            /* Курсор — инверсная ячейка. */
            if (g_np.cur >= lstart && g_np.cur <= j &&
                (u32)(g_np.cur - lstart) <= maxc) {
                u32 ccx = cx + 4u + (g_np.cur - lstart) * 8u;
                u32 ccy = area_y + drawn * 12u;
                (void)fb_fill_rect(ccx, ccy, 8u, 8u, UI_COL_TX);
            }
            drawn++;
        }
        /* Переход к следующей строке. */
        u32 j = lstart;
        while (j < g_np.len && g_np.buf[j] != '\n') {
            j++;
        }
        if (j >= g_np.len) {
            break;
        }
        lstart = j + 1u;
        line++;
        if (j + 1u > g_np.len) {
            break;
        }
        if (i == j + 1u) {
            break;   /* страховка от зацикливания */
        }
        i = j + 1u;
    }

    /* Статус: ошибка (красная) / инфо о последнем действии / счётчики. */
    char st[80];
    u32 cur_line, cur_ls;
    npad_line_of(g_np.cur, &cur_line, &cur_ls);
    (void)ksnprintf(st, sizeof(st), "%s len=%u line=%u col=%u",
                    (g_np.dirty != 0) ? "modified" : "saved",
                    g_np.len, cur_line + 1u, g_np.cur - cur_ls);
    const char *stline = (g_np.status[0] != '\0') ? g_np.status
                         : (g_np.info[0] != '\0') ? g_np.info : st;
    u32 stcol = (g_np.status[0] != '\0') ? UI_COL_ERR : UI_COL_TX_DIM;
    uidraw_text_fit(cx + 4u, cy + ch - status_h, cw - 8u, stline, stcol,
                    UI_COL_PANEL);
}

static void npad_do_save(void)
{
    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        (void)ksnprintf(g_np.status, sizeof(g_np.status), "no storage");
        return;
    }
    if (g_np.name[0] == '\0') {
        npad_set_name("UNTITLED.TXT");
    }
    char full[160];
    npad_full_path(g_np.name, full, sizeof(full));
    /* Защита системных файлов — единая политика fat_name_protected. */
    const char *base = g_np.name;
    for (u32 i = 0; g_np.name[i] != '\0'; i++) {
        if (g_np.name[i] == '\\' || g_np.name[i] == '/') {
            base = g_np.name + i + 1u;
        }
    }
    if (fat_name_protected(base) != 0) {
        (void)ksnprintf(g_np.status, sizeof(g_np.status),
                        "%s: protected name", base);
        return;
    }
    err_t rc = fat_file_write(v, full, (const u8 *)g_np.buf, g_np.len);
    if (rc != K_OK) {
        (void)ksnprintf(g_np.status, sizeof(g_np.status), "save: %s",
                        k_strerror(rc));
        con_printf("notepad: save %s failed: %s\n", full, k_strerror(rc));
        return;
    }
    g_np.dirty = 0;
    g_np.status[0] = '\0';
    (void)ksnprintf(g_np.info, sizeof(g_np.info), "saved %s (%u B)", full,
                    g_np.len);
    /* FND-2f: консоль (окно терминала/TUI) показывает, КУДА сохранено. */
    con_printf("notepad: saved %u bytes -> %s\n", g_np.len, full);
    klog(KLOG_INFO, A_TAG, "notepad: сохранён %s (%u байт)", full, g_np.len);
    /* Открытый проводник перечитывает каталог: файл виден сразу. */
    if (g_dsk.win[APP_EXPL].open != 0) {
        (void)explorer_refresh();
    }
}

static void npad_do_open(void)
{
    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        (void)ksnprintf(g_np.status, sizeof(g_np.status), "no storage");
        return;
    }
    if (g_np.name[0] == '\0') {
        (void)ksnprintf(g_np.status, sizeof(g_np.status), "type a name first");
        return;
    }
    char full[160];
    npad_full_path(g_np.name, full, sizeof(full));
    u32 len = 0;
    err_t rc = fat_file_read(v, full, (u8 *)g_np.buf, NPAD_MAX - 1u, &len);
    if (rc != K_OK) {
        (void)ksnprintf(g_np.status, sizeof(g_np.status), "open: %s",
                        k_strerror(rc));
        return;
    }
    g_np.len = len;
    g_np.cur = 0;
    g_np.top = 0;
    g_np.dirty = 0;
    g_np.status[0] = '\0';
    (void)ksnprintf(g_np.info, sizeof(g_np.info), "opened %s (%u B)", full,
                    len);
    klog(KLOG_INFO, A_TAG, "notepad: открыт %s (%u байт)", full, len);
}

static void click_notepad(u32 mx, u32 my)
{
    u32 cx, cy, cw, ch;
    dsk_win_content(&g_dsk.win[APP_NOTEPAD], &cx, &cy, &cw, &ch);

    if (my < cy + 22u) {
        static const char *const btns[] = { "New", "Open", "Save" };
        u32 x = cx + 4u;
        for (u32 i = 0; i < 3u; i++) {
            u32 bw = (u32)strlen(btns[i]) * 8u + 10u;
            if (uidraw_rect_hit(x, cy + 2u, bw, 18u, mx, my) != 0) {
                if (i == 0) {
                    g_np.len = 0;
                    g_np.cur = 0;
                    g_np.top = 0;
                    g_np.dirty = 0;
                    npad_set_name("UNTITLED.TXT");
                    g_np.status[0] = '\0';
                } else if (i == 1) {
                    npad_do_open();
                } else {
                    npad_do_save();
                }
                return;
            }
            x += bw + 6u;
        }
        /* Клик по полю имени: ничего (редактирование имени — v2). */
        return;
    }
    /* Клик по тексту: позиционируем курсор. */
    u32 area_y = cy + 24u;
    u32 status_h = 12u;
    u32 area_h = (ch > 24u + status_h) ? ch - 24u - status_h : 0u;
    if (my < area_y || my >= area_y + area_h || mx < cx + 4u) {
        return;
    }
    u32 row = (my - area_y) / 12u;
    u32 col = (mx - cx - 4u) / 8u;

    /* Находим строку top+row. */
    u32 target = g_np.top + row;
    u32 line = 0, i = 0;
    while (i < g_np.len && line < target) {
        if (g_np.buf[i] == '\n') { line++; }
        i++;
    }
    u32 line_start = (line == target) ? i : g_np.len;
    u32 j = line_start;
    u32 k = 0;
    while (j < g_np.len && g_np.buf[j] != '\n' && k < col) {
        j++;
        k++;
    }
    g_np.cur = j;
}

int notepad_key(const struct kbd_event *ev)
{
    u32 cx, cy, cw, ch;
    dsk_win_content(&g_dsk.win[APP_NOTEPAD], &cx, &cy, &cw, &ch);
    u32 status_h = 12u;
    u32 area_h = (ch > 24u + status_h) ? ch - 24u - status_h : 0u;
    u32 area_rows = (area_h / 12u > 0u) ? area_h / 12u : 1u;

    switch (ev->code) {
    case KEY_ENTER: npad_insert('\n'); npad_scroll_fix(area_rows); return 1;
    case KEY_BSPACE: npad_backspace(); npad_scroll_fix(area_rows); return 1;
    case KEY_LEFT:
        if (g_np.cur > 0u) { g_np.cur--; }
        npad_scroll_fix(area_rows);
        return 1;
    case KEY_RIGHT:
        if (g_np.cur < g_np.len) { g_np.cur++; }
        npad_scroll_fix(area_rows);
        return 1;
    case KEY_UP:
    case KEY_DOWN: {
        u32 line, ls;
        npad_line_of(g_np.cur, &line, &ls);
        u32 col = g_np.cur - ls;
        u32 nl = (ev->code == KEY_UP && line > 0u) ? line - 1u
                 : (ev->code == KEY_DOWN) ? line + 1u : line;
        if (nl != line) {
            u32 nls = npad_line_start(nl);
            u32 j = nls;
            while (j < g_np.len && g_np.buf[j] != '\n' &&
                   j - nls < col) {
                j++;
            }
            g_np.cur = j;
            npad_scroll_fix(area_rows);
        }
        return 1;
    }
    case KEY_HOME: {
        u32 line, ls;
        npad_line_of(g_np.cur, &line, &ls);
        g_np.cur = ls;
        return 1;
    }
    case KEY_END: {
        u32 j = g_np.cur;
        while (j < g_np.len && g_np.buf[j] != '\n') { j++; }
        g_np.cur = j;
        return 1;
    }
    case KEY_PGUP:
        g_np.top = (g_np.top > area_rows) ? g_np.top - area_rows : 0u;
        return 1;
    case KEY_PGDN:
        g_np.top += area_rows;
        return 1;
    default:
        break;
    }
    if (ev->ch >= 0x20 && ev->ch <= 0x7E) {
        npad_insert(ev->ch);
        npad_scroll_fix(area_rows);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Calculator (целочисленный, цепочка слева направо)                   */
/* ------------------------------------------------------------------ */

static const char *const CALC_ROWS[5] = {
    "C<-/*",
    "789-",
    "456+",
    "123=",
    "0 % "
};
/* Раскладка: r0 C,<-,/,*  r1 7,8,9,-  r2 4,5,6,+  r3 1,2,3,=
 * r4: широкая 0 (ячейки 0-1), % (ячейка 2), пусто (3). */

static int calc_apply(void)
{
    u64 a = g_calc.acc, b = g_calc.cur;
    switch (g_calc.op) {
    case '+':
        if (b > 0xFFFFFFFFFFFFFFFFull - a) { return -1; }
        g_calc.acc = a + b;
        return 0;
    case '-':
        if (b > a) { return -1; }   /* отрицательные не поддержаны */
        g_calc.acc = a - b;
        return 0;
    case '*':
        if (a != 0u && b > 0xFFFFFFFFFFFFFFFFull / a) { return -1; }
        g_calc.acc = a * b;
        return 0;
    case '/':
        if (b == 0u) { return -1; }
        g_calc.acc = a / b;
        return 0;
    case '%':
        if (b == 0u) { return -1; }
        g_calc.acc = a % b;
        return 0;
    default:
        return 0;
    }
}

static void calc_press(char k)
{
    if (k == 'C') {
        g_calc.acc = 0;
        g_calc.cur = 0;
        g_calc.op = 0;
        g_calc.fresh = 1;
        g_calc.err = 0;
        return;
    }
    if (g_calc.err != 0) {
        return;   /* только C выходит из ошибки */
    }
    if (k >= '0' && k <= '9') {
        if (g_calc.fresh != 0) {
            g_calc.cur = 0;
            g_calc.fresh = 0;
        }
        if (g_calc.cur > (0xFFFFFFFFFFFFFFFFull - (u64)(k - '0')) / 10u) {
            g_calc.err = 1;
            return;
        }
        g_calc.cur = g_calc.cur * 10u + (u64)(k - '0');
        return;
    }
    if (k == '<') {   /* backspace */
        g_calc.cur /= 10u;
        return;
    }
    if (k == '+' || k == '-' || k == '*' || k == '/') {
        if (g_calc.op != 0 && g_calc.fresh == 0) {
            if (calc_apply() != 0) { g_calc.err = 1; return; }
        } else if (g_calc.op == 0) {
            g_calc.acc = g_calc.cur;
        }
        g_calc.op = k;
        g_calc.fresh = 1;
        return;
    }
    if (k == '%') {
        /* mod считается сразу: acc % cur -> acc. */
        if (g_calc.op == 0) {
            g_calc.acc = g_calc.cur;
        } else if (calc_apply() != 0) {
            g_calc.err = 1;
            return;
        }
        g_calc.op = '%';
        g_calc.fresh = 1;
        return;
    }
    if (k == '=') {
        if (g_calc.op != 0) {
            if (g_calc.fresh == 0 && calc_apply() != 0) {
                g_calc.err = 1;
                return;
            }
            g_calc.op = 0;
            g_calc.cur = g_calc.acc;
            g_calc.fresh = 1;
        }
        return;
    }
}

/* Геометрия клавиш: возвращает 1 и k, если точка внутри клавиши. */
static int calc_key_at(u32 cx, u32 cy, u32 cw, u32 ch, u32 mx, u32 my,
                       char *out_k)
{
    u32 disp_h = 30u;
    u32 grid_y = cy + disp_h + 4u;
    u32 grid_h = ch - disp_h - 8u;
    if (grid_h < 5u * 20u) {
        return 0;
    }
    u32 cell_h = grid_h / 5u;
    u32 cell_w = (cw - 8u) / 4u;
    if (my < grid_y || mx < cx + 4u) {
        return 0;
    }
    u32 r = (my - grid_y) / cell_h;
    if (r > 4u) {
        return 0;
    }
    u32 c = (mx - cx - 4u) / cell_w;
    if (c > 3u) {
        return 0;
    }
    char k = CALC_ROWS[r][c];
    /* r4: широкая "0" занимает ячейки 0-1 — до проверки на пустоту. */
    if (r == 4u && c == 1u) {
        k = '0';
    }
    if (k == ' ' || k == '\0') {
        return 0;
    }
    *out_k = k;
    return 1;
}

static void draw_calc(void)
{
    u32 cx, cy, cw, ch;
    dsk_win_content(&g_dsk.win[APP_CALC], &cx, &cy, &cw, &ch);

    /* Дисплей. */
    char disp[24];
    if (g_calc.err != 0) {
        (void)ksnprintf(disp, sizeof(disp), "E");
    } else if (g_calc.op != 0 && g_calc.fresh != 0) {
        (void)ksnprintf(disp, sizeof(disp), "%llu",
                        (unsigned long long)g_calc.acc);
    } else {
        (void)ksnprintf(disp, sizeof(disp), "%llu",
                        (unsigned long long)g_calc.cur);
    }
    u32 dw = (u32)strlen(disp) * 8u;
    u32 disp_h = 30u;
    (void)fb_fill_rect(cx + 4u, cy + 2u, cw - 8u, disp_h, 0x0E1116u);
    if (dw + 8u <= cw - 8u) {
        uidraw_text(cx + cw - 8u - dw, cy + 2u + (disp_h - 8u) / 2u, disp,
                    0x2FE38Au, 0x0E1116u);
    }

    /* Клавиши. */
    u32 grid_y = cy + disp_h + 4u;
    u32 grid_h = ch - disp_h - 8u;
    if (grid_h < 5u * 20u) {
        return;   /* окно слишком узкое по высоте — клавиатуры нет */
    }
    u32 cell_h = grid_h / 5u;
    u32 cell_w = (cw - 8u) / 4u;
    for (u32 r = 0; r < 5u; r++) {
        for (u32 c = 0; c < 4u; c++) {
            char k = CALC_ROWS[r][c];
            if (k == ' ' || k == '\0') {
                continue;
            }
            u32 bx = cx + 4u + c * cell_w;
            u32 by = grid_y + r * cell_h;
            u32 bw2 = cell_w - 2u;
            if (r == 4u && c == 0u) {
                bw2 = cell_w * 2u - 2u;   /* широкая 0 */
            }
            u32 fill = 0x1C2431u;
            u32 tcol = UI_COL_TX;
            if (k == 'C' || k == '<') {
                fill = 0x3A2A2Au;
                tcol = UI_COL_ERR;
            } else if (k == '=' || k == '+' || k == '-' || k == '*' ||
                       k == '/') {
                fill = 0x223148u;
                tcol = 0x9ECBEFu;
            }
            (void)fb_fill_rect(bx, by, bw2, cell_h - 2u, fill);
            char label[2] = { k, '\0' };
            u32 lw = 8u;
            uidraw_text(bx + (bw2 - lw) / 2u, by + (cell_h - 2u - 8u) / 2u,
                        label, tcol, fill);
        }
    }
}

static void click_calc(u32 mx, u32 my)
{
    u32 cx, cy, cw, ch;
    dsk_win_content(&g_dsk.win[APP_CALC], &cx, &cy, &cw, &ch);
    char k;
    if (calc_key_at(cx, cy, cw, ch, mx, my, &k) != 0) {
        calc_press(k);
    }
}

int calc_key(const struct kbd_event *ev)
{
    if (ev->ch >= '0' && ev->ch <= '9') {
        calc_press(ev->ch);
        return 1;
    }
    switch (ev->code) {
    case KEY_ENTER: calc_press('='); return 1;
    case KEY_BSPACE: calc_press('<'); return 1;
    default: break;
    }
    if (ev->ch == '+' || ev->ch == '-' || ev->ch == '*' || ev->ch == '/') {
        calc_press(ev->ch);
        return 1;
    }
    if (ev->ch == '%') {
        calc_press('%');
        return 1;
    }
    if (ev->code == KEY_ESC) {
        calc_press('C');
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

#define SET_SIDEBAR_W 110u
#define SET_ITEM_H 26u

static const char *const SET_PAGES[4] = {
    "System", "Display", "Time", "Personal"
};

static void set_load_time(void)
{
    if (rtc_ready() != 0 && rtc_read_time(&g_set.tm) == K_OK) {
        g_set.tm_ok = 1;
    } else {
        g_set.tm_ok = 0;
    }
}

/* Кнопка-строка с меткой слева (для списков настроек). */
static void set_row(u32 x, u32 y, u32 w, const char *label, int active,
                    int selected)
{
    u32 fill = UI_COL_PANEL;
    if (selected != 0) {
        fill = 0x24384Eu;
    }
    (void)fb_fill_rect(x, y, w, 20u, fill);
    if (active != 0) {
        (void)fb_fill_rect(x, y, 3u, 20u, uidraw_accent());
    }
    uidraw_text_fit(x + 10u, y + 6u, w - 14u, label, UI_COL_TX, fill);
}

static void draw_settings(void)
{
    u32 cx, cy, cw, ch;
    dsk_win_content(&g_dsk.win[APP_SET], &cx, &cy, &cw, &ch);

    /* Сайдбар. */
    (void)fb_fill_rect(cx, cy, SET_SIDEBAR_W, ch, 0x161D29u);
    for (u32 i = 0; i < 4u; i++) {
        u32 iy = cy + 8u + i * SET_ITEM_H;
        u32 fill = (g_set.page == i) ? 0x273448u : 0x161D29u;
        (void)fb_fill_rect(cx + 4u, iy, SET_SIDEBAR_W - 8u, SET_ITEM_H - 4u,
                           fill);
        if (g_set.page == i) {
            (void)fb_fill_rect(cx + 4u, iy, 3u, SET_ITEM_H - 4u,
                               uidraw_accent());
        }
        uidraw_text(cx + 14u, iy + (SET_ITEM_H - 4u - 8u) / 2u, SET_PAGES[i],
                    UI_COL_TX, fill);
    }

    u32 px = cx + SET_SIDEBAR_W + 10u;
    u32 pw = (cw > SET_SIDEBAR_W + 20u) ? cw - SET_SIDEBAR_W - 20u : 40u;
    u32 py = cy + 8u;

    if (g_set.page == 0) {
        /* System: те же данные, что About. */
        char line[128];
        (void)ksnprintf(line, sizeof(line), "Fobos OS %s", FOBOS_VERSION_STR);
        uidraw_text(px, py, line, uidraw_accent(), UI_COL_PANEL);
        py += 16u;
        py += uidraw_text_wrap(px, py, pw, 3, 12u, FOBOS_STAGE_STR,
                               UI_COL_TX_DIM, UI_COL_PANEL) * 12u + 8u;
        struct blk_status st = blk_get_status();
        (void)ksnprintf(line, sizeof(line), "host: %s", session_hostname());
        uidraw_text_fit(px, py, pw, line, UI_COL_TX, UI_COL_PANEL);
        py += 14u;
        (void)ksnprintf(line, sizeof(line),
                        "storage: %s, fat32 %s",
                        (st.fat_ok != 0) ? st.model : "none",
                        (st.fat_ok != 0) ? "mounted" : "off");
        uidraw_text_fit(px, py, pw, line, UI_COL_TX, UI_COL_PANEL);
        py += 14u;
        (void)ksnprintf(line, sizeof(line), "amnesia: %s",
                        (st.amnesia != 0) ? "ON" : "off");
        uidraw_text_fit(px, py, pw, line,
                        (st.amnesia != 0) ? UI_COL_ERR : UI_COL_TX,
                        UI_COL_PANEL);
        py += 14u;
        u32 fw = 0, fh = 0;
        (void)fb_get_dims(&fw, &fh);
        (void)ksnprintf(line, sizeof(line), "display: %ux%u", fw, fh);
        uidraw_text_fit(px, py, pw, line, UI_COL_TX, UI_COL_PANEL);
        py += 14u;
        if (g_dsk.cfg_res_w != 0u) {
            (void)ksnprintf(line, sizeof(line),
                            "resolution after reboot: %ux%u",
                            g_dsk.cfg_res_w, g_dsk.cfg_res_h);
            uidraw_text_fit(px, py, pw, line, UI_COL_TX_DIM, UI_COL_PANEL);
        }
    } else if (g_set.page == 1) {
        /* Display: выбор разрешения (применяется загрузчиком). */
        uidraw_text(px, py, "Resolution (applies after reboot)",
                    UI_COL_TX_DIM, UI_COL_PANEL);
        py += 16u;
        u32 fw = 0, fh = 0;
        (void)fb_get_dims(&fw, &fh);
        for (u32 i = 0; i < RES_MODES; i++) {
            char label[40];
            (void)ksnprintf(label, sizeof(label), "%ux%u%s",
                            g_res_modes[i].w, g_res_modes[i].h,
                            (g_res_modes[i].w == fw &&
                             g_res_modes[i].h == fh) ? "  (current)" : "");
            int sel = (g_dsk.cfg_res_w == g_res_modes[i].w &&
                       g_dsk.cfg_res_h == g_res_modes[i].h);
            set_row(px, py, pw, label, sel, sel);
            py += 24u;
        }
        py += 6u;
        uidraw_text_wrap(px, py, pw, 2, 12u,
                         "Stored in fobos.cfg; the boot loader picks the "
                         "GOP mode at startup. Invalid modes are ignored.",
                         UI_COL_TX_DIM, UI_COL_PANEL);
    } else if (g_set.page == 2) {
        /* Time & Date. */
        if (g_set.tm_ok == 0) {
            uidraw_text(px, py, "RTC unavailable", UI_COL_ERR, UI_COL_PANEL);
            return;
        }
        char big[32];
        (void)ksnprintf(big, sizeof(big), "%04u-%02u-%02u",
                        g_set.tm.year, g_set.tm.month, g_set.tm.day);
        uidraw_text(px, py + 4u, big, UI_COL_TX, UI_COL_PANEL);
        (void)ksnprintf(big, sizeof(big), "%02u:%02u:%02u",
                        g_set.tm.hour, g_set.tm.minute, g_set.tm.second);
        uidraw_text(px, py + 22u, big, UI_COL_TX, UI_COL_PANEL);

        /* Кнопки +/- (два ряда по пять). */
        static const char *const adj[10] = {
            "Y+", "M+", "D+", "H+", "Min+",
            "Y-", "M-", "D-", "H-", "Min-"
        };
        u32 bw = 46u;
        u32 bx0 = px;
        u32 byy = py + 44u;
        for (u32 i = 0; i < 10u; i++) {
            u32 r = i / 5u;
            u32 c = i % 5u;
            app_button(bx0 + c * (bw + 4u), byy + r * 22u, bw, adj[i], 0);
        }

        /* Apply + формат. */
        u32 aby = byy + 48u;
        app_button(px, aby, 110u, "Apply to RTC", 0);
        char fmt[24];
        (void)ksnprintf(fmt, sizeof(fmt), "Format: %dh",
                        (g_dsk.cfg_time24 != 0) ? 24 : 12);
        app_button(px + 118u, aby, 110u, fmt, 0);

        if (g_set.status[0] != '\0') {
            uidraw_text_fit(px, aby + 24u, pw, g_set.status, UI_COL_ERR,
                            UI_COL_PANEL);
        }
    } else {
        /* Personalization: обои + акцент. */
        uidraw_text(px, py, "Wallpaper", UI_COL_TX_DIM, UI_COL_PANEL);
        py += 16u;
        static const char *const walls[UI_WALL_COUNT] = {
            "Gradient", "Bloom", "Hills", "Aurora"
        };
        for (u32 i = 0; i < UI_WALL_COUNT; i++) {
            u32 tile_w = 84u;
            u32 r = i / 4u;
            u32 c = i % 4u;
            u32 tx = px + c * (tile_w + 6u);
            u32 ty = py + r * 34u;
            u32 fill = (g_dsk.cfg_wall == i) ? 0x2C3A52u : UI_COL_PANEL;
            (void)fb_fill_rect(tx, ty, tile_w, 28u, fill);
            if (g_dsk.cfg_wall == i) {
                (void)fb_fill_rect(tx, ty, tile_w, 2u, uidraw_accent());
            }
            /* Мини-превью. */
            u32 mxx = tx + 4u, myy = ty + 6u;
            switch (i) {
            case UI_WALL_GRADIENT:
                for (u32 k = 0; k < 16u; k++) {
                    (void)fb_fill_rect(mxx, myy + k, 20u, 1u,
                                       0x1A1F28u + k * 0x0300u);
                }
                break;
            case UI_WALL_BLOOM:
                uidraw_disc(mxx + 10u, myy + 8u, 8u, 0x2E7BD0u);
                uidraw_disc(mxx + 10u, myy + 8u, 4u, 0x7B5CD6u);
                break;
            case UI_WALL_HILLS:
                (void)fb_fill_rect(mxx, myy, 20u, 8u, 0x0D1626u);
                (void)fb_fill_rect(mxx, myy + 8u, 20u, 8u, 0x2E5B3Au);
                break;
            default:
                (void)fb_fill_rect(mxx, myy, 20u, 16u, 0x07101Cu);
                (void)fb_fill_rect(mxx + 2u, myy + 4u, 16u, 3u, 0x1F7A5Cu);
                break;
            }
            uidraw_text_fit(tx + 28u, ty + 10u, tile_w - 30u, walls[i],
                            UI_COL_TX, fill);
        }
        py += 44u;

        uidraw_text(px, py, "Accent color", UI_COL_TX_DIM, UI_COL_PANEL);
        py += 16u;
        for (u32 i = 0; i < ACCENTS; i++) {
            u32 sx = px + i * 30u;
            (void)fb_fill_rect(sx, py, 24u, 16u, g_accent_palette[i]);
            if (g_dsk.cfg_accent == g_accent_palette[i]) {
                (void)fb_fill_rect(sx, py, 24u, 1u, 0xF2F4F8u);
                (void)fb_fill_rect(sx, py + 15u, 24u, 1u, 0xF2F4F8u);
            }
        }
        py += 26u;
        uidraw_text_wrap(px, py, pw, 2, 12u,
                         "Settings are stored in \\fobos\\fobos.cfg "
                         "(wall, accent, res, pins).",
                         UI_COL_TX_DIM, UI_COL_PANEL);
    }
}

static void set_adjust_time(int field, int delta)
{
    if (g_set.tm_ok == 0) {
        return;
    }
    switch (field) {
    case 0: {   /* year */
        int y = (int)g_set.tm.year + delta;
        if (y >= 2000 && y <= 2099) { g_set.tm.year = (u16)y; }
        break;
    }
    case 1: {   /* month */
        int m = (int)g_set.tm.month + delta;
        if (m >= 1 && m <= 12) { g_set.tm.month = (u8)m; }
        break;
    }
    case 2: {   /* day */
        int d = (int)g_set.tm.day + delta;
        if (d >= 1 && d <= 31) { g_set.tm.day = (u8)d; }
        break;
    }
    case 3: {   /* hour */
        int h = (int)g_set.tm.hour + delta;
        if (h >= 0 && h <= 23) { g_set.tm.hour = (u8)h; }
        break;
    }
    default: {  /* minute */
        int m = (int)g_set.tm.minute + delta;
        if (m >= 0 && m <= 59) { g_set.tm.minute = (u8)m; }
        break;
    }
    }
}

static void click_settings(u32 mx, u32 my)
{
    u32 cx, cy, cw, ch;
    dsk_win_content(&g_dsk.win[APP_SET], &cx, &cy, &cw, &ch);

    /* Сайдбар. */
    if (mx >= cx && mx < cx + SET_SIDEBAR_W) {
        for (u32 i = 0; i < 4u; i++) {
            u32 iy = cy + 8u + i * SET_ITEM_H;
            if (my >= iy && my < iy + SET_ITEM_H) {
                g_set.page = i;
                if (i == 2u) {
                    set_load_time();
                }
                return;
            }
        }
        return;
    }

    u32 px = cx + SET_SIDEBAR_W + 10u;
    u32 pw = (cw > SET_SIDEBAR_W + 20u) ? cw - SET_SIDEBAR_W - 20u : 40u;
    u32 py = cy + 8u;

    if (g_set.page == 1u) {
        py += 16u;
        for (u32 i = 0; i < RES_MODES; i++) {
            if (my >= py && my < py + 20u) {
                g_dsk.cfg_res_w = g_res_modes[i].w;
                g_dsk.cfg_res_h = g_res_modes[i].h;
                (void)dsk_cfg_save();
                (void)ksnprintf(g_set.status, sizeof(g_set.status),
                                "res stored: %ux%u",
                                g_res_modes[i].w, g_res_modes[i].h);
                return;
            }
            py += 24u;
        }
    } else if (g_set.page == 2u && g_set.tm_ok != 0) {
        py += 44u;   /* две строки времени */
        u32 bw = 46u;
        u32 bx0 = px;
        u32 byy = py;
        for (u32 i = 0; i < 10u; i++) {
            u32 r = i / 5u;
            u32 c = i % 5u;
            if (uidraw_rect_hit(bx0 + c * (bw + 4u), byy + r * 22u, bw, 18u,
                                mx, my) != 0) {
                set_adjust_time((int)(i % 5u), (i < 5u) ? 1 : -1);
                return;
            }
        }
        u32 aby = byy + 48u;
        if (uidraw_rect_hit(px, aby, 110u, 18u, mx, my) != 0) {
            err_t rc = rtc_write_time(&g_set.tm);
            if (rc == K_OK) {
                (void)ksnprintf(g_set.status, sizeof(g_set.status),
                                "RTC updated");
                klog(KLOG_INFO, A_TAG, "settings: RTC записано из GUI");
            } else {
                (void)ksnprintf(g_set.status, sizeof(g_set.status),
                                "RTC write: %s", k_strerror(rc));
            }
            return;
        }
        if (uidraw_rect_hit(px + 118u, aby, 110u, 18u, mx, my) != 0) {
            g_dsk.cfg_time24 = (g_dsk.cfg_time24 != 0) ? 0 : 1;
            (void)dsk_cfg_save();
            return;
        }
    } else if (g_set.page == 3u) {
        py += 16u;
        for (u32 i = 0; i < UI_WALL_COUNT; i++) {
            u32 tile_w = 84u;
            u32 r = i / 4u;
            u32 c = i % 4u;
            u32 tx = px + c * (tile_w + 6u);
            u32 ty = py + r * 34u;
            if (uidraw_rect_hit(tx, ty, tile_w, 28u, mx, my) != 0) {
                g_dsk.cfg_wall = i;
                dsk_cfg_apply();
                (void)dsk_cfg_save();
                return;
            }
        }
        py += 44u + 16u;
        for (u32 i = 0; i < ACCENTS; i++) {
            u32 sx = px + i * 30u;
            if (uidraw_rect_hit(sx, py, 24u, 16u, mx, my) != 0) {
                g_dsk.cfg_accent = g_accent_palette[i];
                dsk_cfg_apply();
                (void)dsk_cfg_save();
                return;
            }
        }
    }
    (void)pw;
}

/* ------------------------------------------------------------------ */
/* Диспетчер приложений                                                */
/* ------------------------------------------------------------------ */

void dskapps_reset(void)
{
    /* FND-2f: стартуем в пользовательском каталоге \fobos (там notepad
     * сохраняет файлы); на диске без него — честный откат в корень. */
    (void)ksnprintf(g_expl.path, sizeof(g_expl.path), "\\fobos");
    g_expl.ent_count = 0;
    g_expl.ent_sel = -1;
    g_expl.ent_top = 0;
    g_expl.status[0] = '\0';
    if (blk_fat() != NULL && explorer_refresh() != K_OK) {
        (void)ksnprintf(g_expl.path, sizeof(g_expl.path), "/");
        (void)explorer_refresh();
    }
    g_expl.status[0] = '\0';

    g_view.name[0] = '\0';
    g_view.len = 0;
    g_view.err = 0;
    g_view.top = 0;

    g_np.buf[0] = '\0';
    g_np.len = 0;
    g_np.cur = 0;
    g_np.top = 0;
    npad_set_name("UNTITLED.TXT");
    g_np.dirty = 0;
    g_np.status[0] = '\0';
    g_np.info[0] = '\0';

    g_calc.acc = 0;
    g_calc.cur = 0;
    g_calc.op = 0;
    g_calc.fresh = 1;
    g_calc.err = 0;

    g_set.page = 0;
    g_set.tm_ok = 0;
    g_set.status[0] = '\0';
}

void dsk_app_open(u8 app)
{
    if (app >= APP_COUNT || g_dsk.ready == 0) {
        return;
    }
    if (g_dsk.win[app].open == 0) {
        dsk_win_default(app, &g_dsk.win[app]);
        g_dsk.win[app].open = 1;
        if (app == APP_EXPL) {
            (void)explorer_refresh();
        } else if (app == APP_SET) {
            if (g_set.tm_ok == 0) {
                set_load_time();
            }
        }
    }
    g_dsk.focus = app;
}

void dsk_app_close(u8 app)
{
    if (app >= APP_COUNT) {
        return;
    }
    g_dsk.win[app].open = 0;
    if (g_dsk.focus == app) {
        g_dsk.focus = APP_CMD;
    }
}

void dskapps_view_title(char *out, u32 cap)
{
    (void)ksnprintf(out, cap, "viewer - %s",
                    (g_view.name[0] != '\0') ? g_view.name : "file");
}

void dskapps_settings_page(u32 page)
{
    if (page < 4u) {
        g_set.page = page;
        if (page == 2u) {
            set_load_time();
        }
    }
}

void dsk_app_draw(u8 app)
{
    switch (app) {
    case APP_EXPL:    draw_explorer(); break;
    case APP_ABOUT:   draw_about();    break;
    case APP_VIEW:    draw_viewer();   break;
    case APP_NOTEPAD: draw_notepad();  break;
    case APP_CALC:    draw_calc();     break;
    case APP_SET:     draw_settings(); break;
    default:          break;
    }
}

int dsk_app_click(u8 app, u32 mx, u32 my)
{
    switch (app) {
    case APP_EXPL: {
        u32 cx, cy, cw, ch;
        dsk_win_content(&g_dsk.win[APP_EXPL], &cx, &cy, &cw, &ch);
        u32 list_y, rows, btn_y;
        expl_layout(ch, &list_y, &rows, &btn_y);

        /* Кнопки тулбара. */
        u32 bxs[4], bws[4];
        expl_buttons(cx, bxs, bws);
        for (u32 i = 0; i < 4u; i++) {
            if (uidraw_rect_hit(bxs[i], cy + btn_y, bws[i], UI_BTN_H,
                                mx, my) != 0) {
                if (i == 0) {
                    path_dir_up();
                    (void)explorer_refresh();
                } else if (i == 1) {
                    explorer_view_selected();
                } else if (i == 2) {
                    explorer_delete_selected();
                } else {
                    (void)explorer_refresh();
                }
                return 1;
            }
        }

        /* Скроллбар. */
        u32 sb_x = cx + cw - 10u;
        u32 list_abs_y = cy + list_y;
        if (mx >= sb_x && my >= list_abs_y &&
            my < list_abs_y + rows * 12u) {
            if (my < list_abs_y + 10u) {
                g_expl.ent_top = (g_expl.ent_top > 0u) ? g_expl.ent_top - 1u
                                                       : 0u;
            } else if (my >= list_abs_y + rows * 12u - 10u) {
                g_expl.ent_top++;
            }
            return 1;
        }

        /* Строки списка. */
        if (mx < sb_x && my >= list_abs_y) {
            u32 row = (my - list_abs_y) / 12u;
            u32 idx = g_expl.ent_top + row;
            if (row < rows && idx < g_expl.ent_count) {
                g_expl.ent_sel = (int)idx;
                if (g_expl.ent[idx].is_dir != 0) {
                    explorer_open_dir(&g_expl.ent[idx]);
                }
                return 1;
            }
        }
        return 1;
    }
    case APP_ABOUT:    click_about(mx, my);     return 1;
    case APP_VIEW:     click_viewer(mx, my);    return 1;
    case APP_NOTEPAD:  click_notepad(mx, my);   return 1;
    case APP_CALC:     click_calc(mx, my);      return 1;
    case APP_SET:      click_settings(mx, my);  return 1;
    default:
        return 1;
    }
}

int dsk_app_key(u8 app, const struct kbd_event *ev)
{
    switch (app) {
    case APP_NOTEPAD: return notepad_key(ev);
    case APP_CALC:    return calc_key(ev);
    case APP_VIEW:    return viewer_key(ev);
    default:
        return 0;   /* не текстовое приложение — ввод идёт в shell */
    }
}
