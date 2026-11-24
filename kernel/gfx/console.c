/*
 * Fobos OS — реализация fbcon.
 *
 * Раскладка буфера: u16 на ячейку = (attr << 8) | char, где attr = (bg << 4) | fg.
 * Прокрутка: memmove строк вверх, очистка последней строки, полный перерасчёт.
 * Курсор — инверсия ячейки (fg/bg меняются местами при отрисовке).
 */
#include "console.h"
#include "font8x8.h"
#include "../drivers/fb.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../lib/vsnprintf.h"

#define CON_MAX_COLS 256u
#define CON_MAX_ROWS 160u

#define CON_TAB_WIDTH 4u
#define CON_PRINTF_BUF 256u

struct con_state {
    u16 cells[CON_MAX_ROWS][CON_MAX_COLS];
    u32 cols, rows;
    u32 row, col;          /* позиция курсора */
    u32 origin_px_x, origin_px_y; /* левый верхний угол области в пикселях */
    u8  fg, bg;
    int dirty;
    int ready;
};

static struct con_state g_con;

static const u32 con_palette[16] = {
    [CON_BLACK]     = 0x0E1116,
    [CON_RED]       = 0xE5484D,
    [CON_GREEN]     = 0x3FB950,
    [CON_YELLOW]    = 0xF0D000,
    [CON_BLUE]      = 0x4F8CCF,
    [CON_MAGENTA]   = 0x9D5CF2,
    [CON_CYAN]      = 0x38BDF8,
    [CON_LIGHTGRAY] = 0xB8C0CC,
    [CON_GRAY]      = 0x8FA3BF,
    [CON_BRED]      = 0xFF6B70,
    [CON_BGREEN]    = 0x4BD16B,
    [CON_BYELLOW]   = 0xFFE066,
    [CON_BBLUE]     = 0x6FA8F5,
    [CON_BMAGENTA]  = 0xB984F5,
    [CON_BCYAN]     = 0x5CD6F5,
    [CON_WHITE]     = 0xE8EAED,
};

static u16 cell_make(char c, u8 fg, u8 bg)
{
    return (u16)(((u16)(bg & 0x0Fu) << 12) | ((u16)(fg & 0x0Fu) << 8) |
                 (u16)(u8)c);
}

static void mark_dirty(void)
{
    g_con.dirty = 1;
}

static void newline(void)
{
    if (g_con.row + 1u < g_con.rows) {
        g_con.row++;
        return;
    }
    /* Прокрутка: строки 1..N-1 -> 0..N-2. */
    memmove(g_con.cells[0], g_con.cells[1],
            (size_t)(g_con.rows - 1u) * sizeof(g_con.cells[0]));
    for (u32 x = 0; x < g_con.cols; x++) {
        g_con.cells[g_con.rows - 1u][x] = cell_make(' ', g_con.fg, g_con.bg);
    }
    mark_dirty();
}

static void put_char(char c)
{
    if (c == '\n') {
        g_con.col = 0;
        newline();
        return;
    }
    if (c == '\r') {
        g_con.col = 0;
        return;
    }
    if (c == '\b') {
        if (g_con.col > 0u) {
            g_con.col--;
        } else if (g_con.row > 0u) {
            g_con.row--;
            g_con.col = g_con.cols - 1u;
        }
        g_con.cells[g_con.row][g_con.col] = cell_make(' ', g_con.fg, g_con.bg);
        mark_dirty();
        return;
    }
    if (c == '\t') {
        u32 next = (g_con.col + CON_TAB_WIDTH) & ~(CON_TAB_WIDTH - 1u);
        while (g_con.col < next && g_con.col < g_con.cols) {
            g_con.cells[g_con.row][g_con.col] = cell_make(' ', g_con.fg, g_con.bg);
            g_con.col++;
        }
        if (g_con.col >= g_con.cols) {
            g_con.col = 0;
            newline();
        }
        mark_dirty();
        return;
    }

    if (c < (char)0x20 || c > (char)0x7Eu) {
        c = '.';
    }

    g_con.cells[g_con.row][g_con.col] = cell_make(c, g_con.fg, g_con.bg);
    g_con.col++;
    if (g_con.col >= g_con.cols) {
        g_con.col = 0;
        newline();
    }
    mark_dirty();
}

err_t con_init(u32 px, u32 py, u32 pw, u32 ph)
{
    u32 w = 0, h = 0;
    err_t rc = fb_get_dims(&w, &h);
    if (rc != K_OK) {
        return rc;
    }
    /* Область обязана лежать внутри экрана. */
    if (px >= w || py >= h || pw == 0u || ph == 0u ||
        px + pw > w || py + ph > h) {
        return K_ERANGE;
    }

    u32 cols = pw / 8u;
    u32 rows = ph / 8u;
    if (cols == 0u || rows == 0u) {
        return K_ERANGE;
    }
    if (cols > CON_MAX_COLS || rows > CON_MAX_ROWS) {
        return K_ERANGE; /* за пределами статического буфера */
    }

    g_con.cols = cols;
    g_con.rows = rows;
    g_con.row = 0;
    g_con.col = 0;
    g_con.origin_px_x = px;
    g_con.origin_px_y = py;
    g_con.fg = CON_LIGHTGRAY;
    g_con.bg = CON_BLACK;
    g_con.ready = 1;
    con_clear();
    return K_OK;
}

void con_clear(void)
{
    for (u32 y = 0; y < g_con.rows; y++) {
        for (u32 x = 0; x < g_con.cols; x++) {
            g_con.cells[y][x] = cell_make(' ', g_con.fg, g_con.bg);
        }
    }
    g_con.row = 0;
    g_con.col = 0;
    mark_dirty();
}

/* Смена геометрии текстовой области БЕЗ потери содержимого (FND-2e:
 * растягивание окна терминала). Сетка пересчитывается; контент в
 * верхнем левом углу сохраняется (переносов строк не делаем — честное
 * ограничение этапа); новая зона очищается; курсор зажимается.
 * При неизменной сетке ведёт себя как con_reanchor. */
err_t con_resize(u32 px, u32 py, u32 pw, u32 ph)
{
    u32 w = 0, h = 0;
    err_t rc = fb_get_dims(&w, &h);
    if (rc != K_OK) {
        return rc;
    }
    if (px >= w || py >= h || pw == 0u || ph == 0u ||
        px + pw > w || py + ph > h) {
        return K_ERANGE;
    }
    u32 cols = pw / 8u;
    u32 rows = ph / 8u;
    if (cols == 0u || rows == 0u || cols > CON_MAX_COLS || rows > CON_MAX_ROWS) {
        return K_ERANGE;
    }
    if (g_con.ready == 0) {
        return con_init(px, py, pw, ph);
    }
    if (cols == g_con.cols && rows == g_con.rows) {
        return con_reanchor(px, py);
    }

    /* Сохранённая часть — верхний левый угол; строки не сдвигаются,
     * поэтому копирование не нужно: достаточно очистить новые зоны
     * (за пределами пересечения старой и новой сеток). */
    u32 keep_cols = (cols < g_con.cols) ? cols : g_con.cols;
    u32 keep_rows = (rows < g_con.rows) ? rows : g_con.rows;
    for (u32 y = 0; y < rows; y++) {
        u32 x0 = (y < keep_rows) ? keep_cols : 0u;
        for (u32 x = x0; x < cols; x++) {
            g_con.cells[y][x] = cell_make(' ', g_con.fg, g_con.bg);
        }
    }

    g_con.cols = cols;
    g_con.rows = rows;
    if (g_con.row >= rows) { g_con.row = rows - 1u; }
    if (g_con.col >= cols) { g_con.col = cols - 1u; }
    g_con.origin_px_x = px;
    g_con.origin_px_y = py;
    mark_dirty();
    return K_OK;
}

void con_set_color(u8 fg_idx, u8 bg_idx)
{
    g_con.fg = fg_idx & 0x0Fu;
    g_con.bg = bg_idx & 0x0Fu;
}

void con_write(const char *s)
{
    if (s == NULL || g_con.ready == 0) {
        return;
    }
    for (u32 i = 0; s[i] != '\0'; i++) {
        put_char(s[i]);
    }
}

void con_printf(const char *fmt, ...)
{
    char buf[CON_PRINTF_BUF];
    va_list ap;
    va_start(ap, fmt);
    (void)kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    con_write(buf);
}

void con_prompt(const char *text)
{
    con_set_color(CON_BBLUE, CON_BLACK);
    con_write(text);
    con_set_color(CON_LIGHTGRAY, CON_BLACK);
}

void con_flush(void)
{
    if (g_con.ready == 0 || g_con.dirty == 0) {
        return;
    }
    con_flush_rect_px(g_con.origin_px_x, g_con.origin_px_y,
                      g_con.cols * 8u, g_con.rows * 8u);
    g_con.dirty = 0;
}

/* Отрисовка ячеек диапазона [c0..c1) x [r0..r1) в пиксельную область
 * (общий код con_flush и точечной перекраски зоны курсора). */
static void con_blit_cells(u32 c0, u32 r0, u32 c1, u32 r1)
{
    u32 origin_x = g_con.origin_px_x;
    u32 origin_y = g_con.origin_px_y;

    for (u32 y = r0; y < r1; y++) {
        for (u32 x = c0; x < c1; x++) {
            u16 cell = g_con.cells[y][x];
            char c = (char)(cell & 0xFFu);
            u8 fg = (u8)((cell >> 8) & 0x0Fu);
            u8 bg = (u8)((cell >> 12) & 0x0Fu);

            if (y == g_con.row && x == g_con.col) {
                /* Курсор: инверсия цветов ячейки. */
                u8 t = fg;
                fg = bg;
                bg = t;
            }

            u8 glyph = (u8)c;
            if (glyph < FONT8X8_FIRST || glyph > FONT8X8_LAST) {
                glyph = (u8)' ';
            }
            (void)fb_blit_glyph8(origin_x + x * 8u, origin_y + y * 8u,
                                 font8x8[glyph - FONT8X8_FIRST],
                                 con_palette[fg], con_palette[bg]);
        }
    }
}

/* Точечная перекраска прямоугольника ЭКРАНА из ячеек консоли (FND-2f):
 * кадровый конвейер TUI стирает спрайт курсора, перекрашивая его зону
 * из авторитетных данных ячеек — следы курсора невозможны по построению.
 * Вне области консоли — легальный no-op. */
void con_flush_rect_px(u32 px, u32 py, u32 pw, u32 ph)
{
    if (g_con.ready == 0 || pw == 0u || ph == 0u) {
        return;
    }
    u32 ox = g_con.origin_px_x;
    u32 oy = g_con.origin_px_y;
    u32 gw = g_con.cols * 8u;
    u32 gh = g_con.rows * 8u;

    u32 x0 = (px > ox) ? px : ox;
    u32 y0 = (py > oy) ? py : oy;
    u32 x1 = px + pw;
    u32 y1 = py + ph;
    if (x1 > ox + gw) { x1 = ox + gw; }
    if (y1 > oy + gh) { y1 = oy + gh; }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    u32 c0 = (x0 - ox) / 8u;
    u32 r0 = (y0 - oy) / 8u;
    u32 c1 = (x1 - ox + 7u) / 8u;
    u32 r1 = (y1 - oy + 7u) / 8u;
    if (c1 > g_con.cols) { c1 = g_con.cols; }
    if (r1 > g_con.rows) { r1 = g_con.rows; }
    con_blit_cells(c0, r0, c1, r1);
}

void con_mark_dirty(void)
{
    if (g_con.ready != 0) {
        g_con.dirty = 1;
    }
}

int con_dirty(void)
{
    return (g_con.ready != 0 && g_con.dirty != 0) ? 1 : 0;
}

u32 con_bg_rgb(void)
{
    return con_palette[g_con.bg & 0x0Fu];
}

err_t con_reanchor(u32 px, u32 py)
{
    if (g_con.ready == 0) {
        return K_ESTATE;
    }
    u32 w = 0, h = 0;
    err_t rc = fb_get_dims(&w, &h);
    if (rc != K_OK) {
        return rc;
    }
    if (px + g_con.cols * 8u > w || py + g_con.rows * 8u > h) {
        return K_ERANGE;
    }
    g_con.origin_px_x = px;
    g_con.origin_px_y = py;
    g_con.dirty = 1;
    return K_OK;
}

u32 con_cols(void) { return g_con.cols; }
u32 con_rows(void) { return g_con.rows; }

void con_debug_dump(u32 max_row)
{
    char buf[192];
    if (max_row > g_con.rows) {
        max_row = g_con.rows;
    }
    klog_raw("--- condump: cursor=(row,col)=(");
    (void)ksnprintf(buf, sizeof(buf), "%u,%u", g_con.row, g_con.col);
    klog_raw(buf);
    klog_raw(")\n");
    for (u32 y = 0; y < max_row; y++) {
        u32 len = (u32)ksnprintf(buf, sizeof(buf), "%02u|", y);
        for (u32 x = 0; x < g_con.cols && len < sizeof(buf) - 2u; x++) {
            char c = (char)(g_con.cells[y][x] & 0xFFu);
            if (c == '\0') {
                c = '~';
            }
            buf[len++] = c;
        }
        buf[len++] = '\n';
        buf[len] = '\0';
        klog_raw(buf);
    }
}
