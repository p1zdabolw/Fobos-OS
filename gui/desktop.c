#include "desktop.h"
#include "font.h"
#include "window.h"
#include "compositor.h"
#include "../kernel/fb.h"
#include "../kernel/fs.h"
#include "../lib/string.h"
#include "../lib/mem.h"

#define ICON_W      48
#define ICON_H      48
#define ICON_CELL_W 72
#define ICON_CELL_H 76
#define ICON_TOP    16
#define ICON_LEFT   16

#define BR_URL_LEN_LOCAL 128

struct desktop_icon {
    int  x, y;
    char label[32];
    char filename[32];
    int  type;
    int  visible;
    int  selected;
};

static struct desktop_icon g_icons[MAX_DESKTOP_ICONS];
static int g_count;
static int g_drag_id = -1;
static int g_drag_dx;
static int g_drag_dy;
static int g_selected = -1;
static int g_prev_left;

static int ext_is(const char *name, const char *ext) {
    usize n = strlen(name);
    usize e = strlen(ext);
    if (n < e + 1) return 0;
    if (name[n - e - 1] != '.') return 0;
    for (usize i = 0; i < e; i++) {
        char a = name[n - e + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static int detect_type(const char *name) {
    if (ext_is(name, "txt"))  return ICON_TYPE_TXT;
    if (ext_is(name, "exe"))  return ICON_TYPE_EXE;
    if (ext_is(name, "cmd"))  return ICON_TYPE_CMD;
    if (ext_is(name, "bat"))  return ICON_TYPE_CMD;
    if (ext_is(name, "url"))  return ICON_TYPE_URL;
    if (ext_is(name, "html")) return ICON_TYPE_URL;
    if (ext_is(name, "htm"))  return ICON_TYPE_URL;
    return ICON_TYPE_UNKNOWN;
}

static void draw_icon_txt(int x, int y) {
    int px = x + 10, py = y + 2;
    fb_fill_rect(px, py, 28, 40, 0xFFFFFF);
    fb_fill_rect(px, py, 28, 1, 0x404850);
    fb_fill_rect(px, py + 39, 28, 1, 0x404850);
    fb_fill_rect(px, py, 1, 40, 0x404850);
    fb_fill_rect(px + 27, py, 1, 40, 0x404850);
    fb_fill_rect(px + 20, py, 8, 8, 0xB0B8C0);
    fb_fill_rect(px + 20, py, 8, 1, 0x606870);
    fb_fill_rect(px + 20, py, 1, 8, 0x606870);
    for (int i = 0; i < 7; i++) fb_fill_rect(px + 3, py + 13 + i * 4, 22, 1, 0x303840);
}

static void draw_icon_exe(int x, int y) {
    int px = x + 4, py = y + 4;
    fb_fill_rect(px, py, 40, 40, 0xFFFFFF);
    fb_fill_rect(px, py, 40, 8, 0x202060);
    fb_fill_rect(px + 34, py + 2, 4, 4, 0xC04040);
    fb_fill_rect(px + 4, py + 12, 32, 24, 0x4080C0);
    fb_fill_rect(px + 8, py + 20, 24, 8, 0x80D0F0);
    fb_fill_rect(px, py, 40, 1, 0x000000);
    fb_fill_rect(px, py + 39, 40, 1, 0x000000);
    fb_fill_rect(px, py, 1, 40, 0x000000);
    fb_fill_rect(px + 39, py, 1, 40, 0x000000);
}

static void draw_icon_cmd(int x, int y) {
    int px = x + 4, py = y + 4;
    fb_fill_rect(px, py, 40, 40, 0x080810);
    fb_fill_rect(px, py, 40, 1, 0x000000);
    fb_fill_rect(px, py + 39, 40, 1, 0x000000);
    fb_fill_rect(px, py, 1, 40, 0x000000);
    fb_fill_rect(px + 39, py, 1, 40, 0x000000);
    for (int i = 0; i < 6; i++) fb_fill_rect(px + 6 + i, py + 8 + i, 2, 2, 0x40FF40);
    for (int i = 0; i < 6; i++) fb_fill_rect(px + 6 + i, py + 20 - i, 2, 2, 0x40FF40);
    fb_fill_rect(px + 16, py + 24, 10, 2, 0x40FF40);
    for (int i = 0; i < 4; i++) fb_fill_rect(px + 6 + i, py + 30 + i, 2, 2, 0x40FF40);
    for (int i = 0; i < 4; i++) fb_fill_rect(px + 6 + i, py + 38 - i, 2, 2, 0x40FF40);
}

static void draw_icon_unknown(int x, int y) {
    int px = x + 8, py = y + 4;
    fb_fill_rect(px, py, 32, 40, 0xC0C8D0);
    fb_fill_rect(px, py, 32, 1, 0x505860);
    fb_fill_rect(px, py + 39, 32, 1, 0x505860);
    fb_fill_rect(px, py, 1, 40, 0x505860);
    fb_fill_rect(px + 31, py, 1, 40, 0x505860);
    fb_fill_rect(px + 10, py + 10, 12, 4, 0x303840);
    fb_fill_rect(px + 18, py + 14, 4, 8, 0x303840);
    fb_fill_rect(px + 14, py + 22, 6, 4, 0x303840);
    fb_fill_rect(px + 14, py + 30, 4, 4, 0x303840);
}

static void draw_icon_url(int x, int y) {
    int bx = x + 4, by = y + 4;
    int cx = bx + 20, cy = by + 22;
    fb_fill_rect(bx, by, 40, 40, 0xFFFFFF);
    fb_fill_rect(bx, by, 40, 1, 0x202020);
    fb_fill_rect(bx, by + 39, 40, 1, 0x202020);
    fb_fill_rect(bx, by, 1, 40, 0x202020);
    fb_fill_rect(bx + 39, by, 1, 40, 0x202020);

    for (int dy = -15; dy <= 15; dy++) {
        for (int dx = -18; dx <= 18; dx++) {
            int e = dx * dx * 100 / (18 * 18) + dy * dy * 100 / (15 * 15);
            if (e >= 55 && e <= 100) {
                if (dx > 6 && dy < -8) continue;
                fb_fill_rect(cx + dx, cy + dy, 1, 1, 0xE8B020);
            }
        }
    }
    for (int dy = -10; dy <= 10; dy++) {
        for (int dx = -11; dx <= 11; dx++) {
            int outer = dx * dx * 100 / (11 * 11) + dy * dy * 100 / (10 * 10);
            if (outer > 100) continue;
            int inner = dx * dx * 100 / (7 * 7) + dy * dy * 100 / (6 * 6);
            int in_hole = inner <= 100;
            int in_mid = (dy >= -1 && dy <= 2);
            int bottom_right_open = (dx > 4 && dy > 4);
            if (in_hole && !in_mid) continue;
            if (bottom_right_open) continue;
            fb_fill_rect(cx + dx, cy + dy - 2, 1, 1, 0x1050B0);
        }
    }
}

static void draw_label(int cx, int y, const char *label, int selected) {
    char buf[10];
    usize n = strlen(label);
    if (n > 8) {
        memcpy(buf, label, 6);
        buf[6] = '.'; buf[7] = '.'; buf[8] = 0;
    } else {
        memcpy(buf, label, n);
        buf[n] = 0;
    }
    int tw = font_text_width(buf);
    int tx = cx - tw / 2;
    u32 bg = selected ? 0x3C78B4 : 0x102028;
    u32 fg = 0xFFFFFF;
    fb_fill_rect(tx - 2, y, tw + 4, FONT_H, bg);
    font_draw_string(tx, y, buf, fg, bg);
}

static void draw_icon(struct desktop_icon *ic) {
    if (ic->selected) fb_fill_rect(ic->x, ic->y, ICON_W, ICON_H, 0x204060);
    switch (ic->type) {
        case ICON_TYPE_TXT: draw_icon_txt(ic->x, ic->y); break;
        case ICON_TYPE_EXE: draw_icon_exe(ic->x, ic->y); break;
        case ICON_TYPE_CMD: draw_icon_cmd(ic->x, ic->y); break;
        case ICON_TYPE_URL: draw_icon_url(ic->x, ic->y); break;
        default:            draw_icon_unknown(ic->x, ic->y); break;
    }
    draw_label(ic->x + ICON_W / 2, ic->y + ICON_H + 2, ic->label, ic->selected);
}

void desktop_init(void) {
    memset(g_icons, 0, sizeof(g_icons));
    g_count = 0;
    g_drag_id = -1;
    g_selected = -1;
    g_prev_left = 0;
}

void desktop_add_icon(const char *filename) {
    for (int i = 0; i < g_count; i++) {
        if (g_icons[i].visible && strcmp(g_icons[i].filename, filename) == 0) return;
    }
    int id = -1;
    for (int i = 0; i < MAX_DESKTOP_ICONS; i++) {
        if (!g_icons[i].visible) { id = i; break; }
    }
    if (id < 0) return;
    if (id >= g_count) g_count = id + 1;

    int n = 0;
    for (int i = 0; i < g_count; i++) if (g_icons[i].visible && i != id) n++;
    int col = n / 5;
    int row = n % 5;

    struct desktop_icon *ic = &g_icons[id];
    memset(ic, 0, sizeof(*ic));
    ic->x = ICON_LEFT + col * ICON_CELL_W;
    ic->y = ICON_TOP + row * ICON_CELL_H;
    strncpy(ic->label, filename, 31);
    ic->label[31] = 0;
    strncpy(ic->filename, filename, 31);
    ic->filename[31] = 0;
    ic->type = detect_type(filename);
    ic->visible = 1;
    ic->selected = 0;
}

void desktop_remove_icon(const char *filename) {
    for (int i = 0; i < g_count; i++) {
        if (g_icons[i].visible && strcmp(g_icons[i].filename, filename) == 0) {
            g_icons[i].visible = 0;
            if (g_drag_id == i) g_drag_id = -1;
            if (g_selected == i) g_selected = -1;
            return;
        }
    }
}

int desktop_icon_count(void) {
    int n = 0;
    for (int i = 0; i < g_count; i++) if (g_icons[i].visible) n++;
    return n;
}

void desktop_paint(void) {
    for (int i = 0; i < g_count; i++) {
        if (!g_icons[i].visible) continue;
        g_icons[i].selected = (i == g_selected);
        draw_icon(&g_icons[i]);
    }
}

static int point_in_window(int mx, int my) {
    for (int i = 0; i < window_count(); i++) {
        struct window *win = window_get(i);
        if (!win || !win->visible) continue;
        if (mx >= win->x && mx < win->x + win->w &&
            my >= win->y && my < win->y + win->h) return 1;
    }
    return 0;
}

void desktop_sync_input(int left) { g_prev_left = left; }

void desktop_reclamp(void) {
    int w = (int)fb_get()->width;
    int tb = (int)fb_get()->height - compositor_taskbar_h();
    for (int i = 0; i < g_count; i++) {
        struct desktop_icon *ic = &g_icons[i];
        if (!ic->visible) continue;
        if (ic->x + ICON_CELL_W > w) ic->x = w - ICON_CELL_W;
        if (ic->y + ICON_CELL_H > tb) ic->y = tb - ICON_CELL_H;
        if (ic->x < 0) ic->x = 0;
        if (ic->y < 0) ic->y = 0;
    }
}

static void launch_url_file(const char *filename) {
    char url[BR_URL_LEN_LOCAL];
    strncpy(url, "about:home", BR_URL_LEN_LOCAL - 1);
    url[BR_URL_LEN_LOCAL - 1] = 0;
    usize sz = 0;
    void *data = fs_read(filename, &sz);
    if (data && sz > 0) {
        usize n = sz > BR_URL_LEN_LOCAL - 1 ? BR_URL_LEN_LOCAL - 1 : sz;
        memcpy(url, data, n);
        url[n] = 0;
        for (usize i = 0; i < n; i++) {
            if (url[i] == '\n' || url[i] == '\r') { url[i] = 0; break; }
        }
    }
    extern void apps_launch_browser_url(const char *url);
    apps_launch_browser_url(url);
}

int desktop_handle_click(int mx, int my, int left) {
    int new_left = left && !g_prev_left;
    int result = 0;

    if (left && g_drag_id >= 0) {
        struct desktop_icon *ic = &g_icons[g_drag_id];
        int nx = mx - g_drag_dx;
        int ny = my - g_drag_dy;
        int taskbar_y = (int)fb_get()->height - compositor_taskbar_h();
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx + ICON_CELL_W > (int)fb_get()->width) nx = (int)fb_get()->width - ICON_CELL_W;
        if (ny + ICON_CELL_H > taskbar_y) ny = taskbar_y - ICON_CELL_H;
        ic->x = nx;
        ic->y = ny;
        result = 1;
    }

    if (!left && g_prev_left) g_drag_id = -1;

    if (new_left) {
        if (point_in_window(mx, my)) {
            g_selected = -1;
        } else {
            int hit = -1;
            for (int i = g_count - 1; i >= 0; i--) {
                if (!g_icons[i].visible) continue;
                if (mx >= g_icons[i].x && mx < g_icons[i].x + ICON_CELL_W &&
                    my >= g_icons[i].y && my < g_icons[i].y + ICON_CELL_H) {
                    hit = i;
                    break;
                }
            }
            if (hit >= 0) {
                if (g_selected == hit) {
                    if (g_icons[hit].type == ICON_TYPE_TXT && g_icons[hit].filename[0]) {
                        extern void apps_launch_notepad_file(const char *filename);
                        apps_launch_notepad_file(g_icons[hit].filename);
                    } else if (g_icons[hit].type == ICON_TYPE_CMD ||
                               g_icons[hit].type == ICON_TYPE_EXE) {
                        extern void apps_launch_terminal_exec(const char *filename);
                        apps_launch_terminal_exec(g_icons[hit].filename);
                    } else if (g_icons[hit].type == ICON_TYPE_URL) {
                        launch_url_file(g_icons[hit].filename);
                    }
                    g_selected = -1;
                } else {
                    g_selected = hit;
                    g_drag_id = hit;
                    g_drag_dx = mx - g_icons[hit].x;
                    g_drag_dy = my - g_icons[hit].y;
                }
                result = 1;
            } else {
                g_selected = -1;
            }
        }
    }

    g_prev_left = left;
    return result;
}