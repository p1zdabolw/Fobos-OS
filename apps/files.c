#include "../gui/window.h"
#include "../gui/font.h"
#include "../kernel/fb.h"
#include "../kernel/fs.h"
#include "../lib/string.h"
#include "../lib/mem.h"
#include "../lib/printf.h"
#include "../lib/i18n.h"

struct files_state {
    char names[FS_MAX_FILES][FS_MAX_NAME];
    int  count;
    int  selected;
};

static struct files_state g_files[MAX_WINDOWS];

static void files_refresh(struct files_state *st) {
    st->count = fs_list(st->names, FS_MAX_FILES);
    if (st->selected >= st->count) st->selected = st->count - 1;
    if (st->selected < 0) st->selected = 0;
}

static void files_draw(struct window *win) {
    struct files_state *st = (struct files_state*)win->user;
    int ox = win->x + 4;
    int oy = win->y + 22 + 4;
    int h = win->h - 30;
    fb_fill_rect(ox, oy, win->w - 8, h, 0xFFFFFF);
    fb_fill_rect(ox, oy, win->w - 8, 18, 0xD8D8D8);
    font_draw_string(ox + 4, oy + 1, tr(STR_FILES_NAME), 0x101010, 0xD8D8D8);

    for (int i = 0; i < st->count; i++) {
        int y = oy + 20 + i * FONT_H;
        if (y + FONT_H > oy + h) break;
        u32 bg = (i == st->selected) ? 0x3C78B4 : 0xFFFFFF;
        u32 fg = (i == st->selected) ? 0xFFFFFF : 0x101010;
        fb_fill_rect(ox, y, win->w - 8, FONT_H, bg);
        font_draw_string(ox + 4, y, st->names[i], fg, bg);
    }

    int fy = win->y + win->h - 22;
    fb_fill_rect(ox, fy, win->w - 8, 18, 0xE0E0E0);
    char buf[64];
    snprintf(buf, sizeof(buf), tr(STR_FILES_COUNT), st->count);
    font_draw_string(ox + 4, fy + 1, buf, 0x202020, 0xE0E0E0);
}

static void files_key(struct window *win, char c) {
    struct files_state *st = (struct files_state*)win->user;
    if (c == 0x1C) {
        if (st->count > 0 && st->selected < st->count) {
            char name[FS_MAX_NAME];
            strncpy(name, st->names[st->selected], FS_MAX_NAME - 1);
            name[FS_MAX_NAME - 1] = 0;
            fs_remove(name);
            files_refresh(st);
        }
    } else if (c == 0x0E) {
        if (st->selected > 0) st->selected--;
    } else if (c == 0x0F || c == ' ') {
        if (st->selected < st->count - 1) st->selected++;
    } else if (c == 'n' || c == 'N') {
        static int ctr = 0;
        char name[FS_MAX_NAME];
        snprintf(name, sizeof(name), "file%d.txt", ctr++);
        fs_create(name, "hello from FOS\n", 15);
        files_refresh(st);
    }
}

void apps_launch_files(void) {
    int used[MAX_WINDOWS];
    for (int i = 0; i < MAX_WINDOWS; i++) used[i] = 0;
    for (int w = 0; w < window_count(); w++) {
        struct window *win = window_get(w);
        if (!win || !win->visible) continue;
        if (win->on_key != files_key) continue;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (win->user == &g_files[i]) { used[i] = 1; break; }
        }
    }
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (used[i]) continue;
        struct files_state *st = &g_files[i];
        memset(st, 0, sizeof(*st));
        files_refresh(st);
        int id = window_create(180, 120, 420, 300, tr(STR_APP_FILES));
        if (id < 0) return;
        struct window *win = window_get(id);
        win->user = st;
        win->on_draw = files_draw;
        win->on_key = files_key;
        return;
    }
}