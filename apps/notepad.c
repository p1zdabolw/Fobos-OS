#include "../gui/window.h"
#include "../gui/font.h"
#include "../gui/desktop.h"
#include "../kernel/fb.h"
#include "../kernel/fs.h"
#include "../kernel/timer.h"
#include "../lib/string.h"
#include "../lib/mem.h"
#include "../lib/printf.h"

#define NP_BUF 4096
#define NP_VISIBLE_LINES 15
#define NP_VISIBLE_COLS 48
#define SAVE_H 26

struct notepad_state {
    char text[NP_BUF];
    int  len;
    int  cursor;
    char filename[32];
    int  filename_len;
    int  edit_focus;
    u64  saved_at;
};

static struct notepad_state g_pads[MAX_WINDOWS];

static void np_set_filename(struct notepad_state *np, const char *name) {
    usize n = strlen(name);
    if (n > 31) n = 31;
    memcpy(np->filename, name, n);
    np->filename[n] = 0;
    np->filename_len = (int)n;
}

static struct notepad_state *np_alloc(void) {
    int used[MAX_WINDOWS];
    for (int i = 0; i < MAX_WINDOWS; i++) used[i] = 0;
    for (int w = 0; w < window_count(); w++) {
        struct window *win = window_get(w);
        if (!win || !win->visible) continue;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (win->user == &g_pads[i]) { used[i] = 1; break; }
        }
    }
    for (int i = 0; i < MAX_WINDOWS; i++) if (!used[i]) return &g_pads[i];
    return NULL;
}

static void np_compute_visual(struct notepad_state *np, int *row, int *col) {
    int r = 0, c = 0;
    for (int i = 0; i < np->cursor; i++) {
        if (np->text[i] == '\n') { r++; c = 0; }
        else c++;
    }
    *row = r;
    *col = c;
}

static void np_draw(struct window *win) {
    struct notepad_state *np = (struct notepad_state*)win->user;
    int body_h = win->h - 22;
    int aw = win->w - 8;
    int ah = body_h - SAVE_H - 8;

    int tx = win->x + 4;
    int ty = win->y + 22 + 4;
    fb_fill_rect(tx, ty, aw, ah, 0xFFFFFF);
    fb_fill_rect(tx, ty, aw, 1, 0x808080);
    fb_fill_rect(tx, ty, 1, ah, 0x808080);
    fb_fill_rect(tx + aw - 1, ty, 1, ah, 0x808080);
    fb_fill_rect(tx, ty + ah - 1, aw, 1, 0x808080);

    int px = tx + 4;
    int py = ty + 4;
    int r = 0, c = 0;
    for (int i = 0; i < np->len; i++) {
        if (np->text[i] == '\n') { r++; c = 0; continue; }
        if (r >= NP_VISIBLE_LINES) break;
        if (c < NP_VISIBLE_COLS) {
            font_draw_char(px + c * FONT_W, py + r * FONT_H,
                           np->text[i], 0x101010, 0xFFFFFF);
        }
        c++;
    }

    if (np->edit_focus == 0) {
        np_compute_visual(np, &r, &c);
        if (r < NP_VISIBLE_LINES && c < NP_VISIBLE_COLS) {
            if ((timer_ticks() / 50) & 1) {
                fb_fill_rect(px + c * FONT_W, py + r * FONT_H, FONT_W, FONT_H, 0x303030);
            }
        }
    }

    int sy = win->y + win->h - SAVE_H;
    fb_fill_rect(win->x, sy, win->w, SAVE_H, 0xD8D8D8);
    fb_fill_rect(win->x, sy, win->w, 1, 0x808080);
    font_draw_string(win->x + 6, sy + 6, "Name:", 0x101010, 0xD8D8D8);

    int fx = win->x + 52;
    int fy = sy + 4;
    int fw = 160;
    int fh = 18;
    fb_fill_rect(fx, fy, fw, fh, 0xFFFFFF);
    u32 fborder = np->edit_focus ? 0x3C78B4 : 0x808080;
    fb_fill_rect(fx, fy, fw, 1, fborder);
    fb_fill_rect(fx, fy + fh - 1, fw, 1, fborder);
    fb_fill_rect(fx, fy, 1, fh, fborder);
    fb_fill_rect(fx + fw - 1, fy, 1, fh, fborder);
    font_draw_string(fx + 4, fy + 1, np->filename, 0x101010, 0xFFFFFF);
    if (np->edit_focus && ((timer_ticks() / 50) & 1)) {
        fb_fill_rect(fx + 4 + np->filename_len * FONT_W, fy + 1, FONT_W, FONT_H, 0x303030);
    }

    int bx = fx + fw + 6;
    int bw = 60;
    int bh = fh;
    fb_fill_rect(bx, fy, bw, bh, 0x4A6A8A);
    fb_fill_rect(bx, fy, bw, 1, 0x6A8AA8);
    fb_fill_rect(bx, fy + bh - 1, bw, 1, 0x2A4056);
    font_draw_string(bx + 16, fy + 1, "Save", 0xFFFFFF, 0x4A6A8A);

    if (np->saved_at != 0 && timer_ticks() - np->saved_at < 150) {
        font_draw_string(bx + bw + 10, fy + 1, "Saved", 0x107010, 0xD8D8D8);
    }
}

static void np_do_save(struct notepad_state *np) {
    if (np->filename_len == 0) {
        np_set_filename(np, "untitled.txt");
    }
    fs_write(np->filename, np->text, (usize)np->len);
    desktop_add_icon(np->filename);
    np->saved_at = timer_ticks();
}

static void np_key(struct window *win, char c) {
    struct notepad_state *np = (struct notepad_state*)win->user;

    if (np->edit_focus) {
        if (c == 0x0E) {
            if (np->filename_len > 0) {
                np->filename_len--;
                np->filename[np->filename_len] = 0;
            }
            return;
        }
        if (c == 0x1C || c == '\n') {
            np_do_save(np);
            np->edit_focus = 0;
            return;
        }
        if (c >= 32 && c < 127 && np->filename_len < 31) {
            np->filename[np->filename_len++] = c;
            np->filename[np->filename_len] = 0;
        }
        return;
    }

    if (c == 0x0E) {
        if (np->len > 0) {
            np->len--;
            np->text[np->len] = 0;
            np->cursor = np->len;
        }
        return;
    }
    if (c == 0x1C || c == '\n') {
        if (np->len < NP_BUF - 1) {
            np->text[np->len++] = '\n';
            np->text[np->len] = 0;
            np->cursor = np->len;
        }
        return;
    }
    if (c >= 32 && c < 127 && np->len < NP_BUF - 1) {
        np->text[np->len++] = c;
        np->text[np->len] = 0;
        np->cursor = np->len;
    }
}

static void np_click(struct window *win, int x, int y) {
    struct notepad_state *np = (struct notepad_state*)win->user;
    int body_h = win->h - 22;
    int sy = body_h - SAVE_H;

    if (y >= sy) {
        int fx = 52;
        int fw = 160;
        int bx = fx + fw + 6;
        int bw = 60;
        int fh = 18;
        int fy = 4;
        if (x >= fx && x < fx + fw && y >= sy + fy && y < sy + fy + fh) {
            np->edit_focus = 1;
            return;
        }
        if (x >= bx && x < bx + bw && y >= sy + fy && y < sy + fy + fh) {
            np_do_save(np);
            np->edit_focus = 0;
            return;
        }
        return;
    }

    np->edit_focus = 0;
}

static void np_show(struct notepad_state *np, const char *title, const char *content) {
    memset(np, 0, sizeof(*np));
    if (content) {
        usize n = strlen(content);
        if (n > NP_BUF - 1) n = NP_BUF - 1;
        memcpy(np->text, content, n);
        np->text[n] = 0;
        np->len = (int)n;
        np->cursor = (int)n;
    }
    int id = window_create(180, 100, 420, 300, title);
    if (id < 0) return;
    struct window *win = window_get(id);
    win->user = np;
    win->on_draw = np_draw;
    win->on_key = np_key;
    win->on_click = np_click;
}

void apps_launch_notepad(void) {
    struct notepad_state *np = np_alloc();
    if (!np) return;
    np_show(np, "Notepad", NULL);
}

void apps_launch_notepad_file(const char *filename) {
    struct notepad_state *np = np_alloc();
    if (!np) return;
    usize sz = 0;
    void *data = fs_read(filename, &sz);
    char buf[NP_BUF];
    usize n = sz;
    if (n > NP_BUF - 1) n = NP_BUF - 1;
    if (data) memcpy(buf, data, n);
    buf[n] = 0;
    np_show(np, filename, buf);
    np_set_filename(np, filename);
}

void apps_launch_about(void) {
    struct notepad_state *np = np_alloc();
    if (!np) return;
    np_show(np, "About FOS",
        "FOS 0.1 -- Fobos Operating System\n"
        "\n"
        "Architecture: x86_64 (long mode)\n"
        "Kernel: monolithic, custom\n"
        "Bootloader: GRUB2 via Multiboot2\n"
        "Language: C11 + NASM\n"
        "\n"
        "Graphics: linear framebuffer\n"
        "Compositor: software, double buffered\n"
        "Input: PS/2 keyboard + mouse\n"
        "Filesystem: in-memory (initramfs-like)\n"
        "\n"
        "Desktop icons:\n"
        "  .txt   white page\n"
        "  .exe   blue window\n"
        "  .cmd   green prompt\n"
        "\n"
        "Click icon once to select, again to open.\n"
        "Drag icon to move it.\n"
        "Right-click desktop for a menu.\n");
}