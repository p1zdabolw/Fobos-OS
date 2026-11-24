/*
 * Fobos OS — framebuffer: прямой доступ к линейному GOP-фреймбуферу.
 *
 * Слой: drivers. Рисование через identity-отображение (< 4 ГиБ).
 *
 * FND-2d: двойная буферизация. Все примитивы пишут в активную поверхность —
 * задний буфер в RAM (выделяется phys_alloc_contig, геометрия/pitch совпадают
 * с VRAM). fb_present() переносит готовый кадр в VRAM сплошным row-copy,
 * поэтому наблюдатель видит только целые кадры: мерцание (промежуточные
 * состояния градиента/окон/курсора) устранено по построению.
 *
 * Потокобезопасность: рисует только kmain; блокировки добавит fbdev-сервис
 * в GUI-1.
 */
#include "fb.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../mm/phys.h"

static const struct fbpi_framebuffer *g_fb;

/* Задний буфер. NULL = не выделен (рисуем напрямую в VRAM, как до FND-2d). */
static u32 *g_back;
static u64  g_back_paddr;
static u64  g_back_bytes;

/* Кэш обоев (FND-2e). NULL = нет слоя (фон рисуется процедурно). */
static u32 *g_wall;
static u64  g_wall_paddr;
static int  g_wall_dirty;

/* Dirty-флаг: взводится примитивами, сбрасывается fb_present(). */
static int g_dirty;

/* Clip-прямоугольник рендера (FND-2e). Все примитивы рисуют только
 * пересечение с ним; по умолчанию — весь экран (fb_init). */
static u32 g_clip_x, g_clip_y, g_clip_w, g_clip_h;

err_t fb_init(const struct fbpi_boot_info *bi)
{
    if (bi == NULL) {
        return K_EINVAL;
    }
    if (bi->fb.pixel_format == FBPI_PF_BLTONLY || bi->fb.bpp != 32u) {
        return K_EUNSUPPORTED;
    }
    g_fb = &bi->fb;
    g_clip_x = 0;
    g_clip_y = 0;
    g_clip_w = g_fb->width;
    g_clip_h = g_fb->height;
    return K_OK;
}

err_t fb_backbuffer_init(void)
{
    if (g_fb == NULL) {
        return K_ESTATE;
    }
    if (g_back != NULL) {
        return K_ESTATE;   /* повторная инициализация — ошибка состояния */
    }

    /* Тот же pitch, что у VRAM: строки back-буфера и VRAM совпадают байт в
     * байт, present — простой memcpy по строкам. */
    g_back_bytes = (u64)g_fb->pitch_bytes * g_fb->height;
    u64 frames = (g_back_bytes + PAGE_SIZE - 1u) / PAGE_SIZE;
    if (frames > 0xFFFFFFFFu) {
        return K_ERANGE;
    }

    u64 paddr = 0;
    err_t rc = phys_alloc_contig((u32)frames, &paddr);
    if (rc != K_OK) {
        return rc;
    }

    g_back_paddr = paddr;
    g_back = (u32 *)(u64)paddr;
    g_dirty = 1;   /* первый present обязан отработать */

    klog(KLOG_INFO, "drv/fb",
         "backbuffer: %llu КиБ (%llu кадров) @ 0x%llx — двойная буферизация включена",
         (unsigned long long)(g_back_bytes >> 10),
         (unsigned long long)frames,
         (unsigned long long)paddr);
    return K_OK;
}

int fb_backbuffer_active(void)
{
    return (g_back != NULL) ? 1 : 0;
}

static inline void fb_touch(void)
{
    g_dirty = 1;
}

/* --- Кэш обоев --- */

err_t fb_walllayer_init(void)
{
    if (g_fb == NULL) {
        return K_ESTATE;
    }
    if (g_wall != NULL) {
        return K_ESTATE;   /* повторная инициализация — ошибка состояния */
    }
    u64 bytes = (u64)g_fb->pitch_bytes * g_fb->height;
    u64 frames = (bytes + PAGE_SIZE - 1u) / PAGE_SIZE;
    if (frames > 0xFFFFFFFFu) {
        return K_ERANGE;
    }
    u64 paddr = 0;
    err_t rc = phys_alloc_contig((u32)frames, &paddr);
    if (rc != K_OK) {
        return rc;
    }
    g_wall_paddr = paddr;
    g_wall = (u32 *)(u64)paddr;
    g_wall_dirty = 1;
    klog(KLOG_INFO, "drv/fb",
         "walllayer: %llu КиБ (%llu кадров) @ 0x%llx — кэш обоев включён",
         (unsigned long long)(bytes >> 10),
         (unsigned long long)frames,
         (unsigned long long)paddr);
    return K_OK;
}

int fb_walllayer_active(void)
{
    return (g_wall != NULL) ? 1 : 0;
}

void fb_walllayer_invalidate(void)
{
    g_wall_dirty = 1;
}

void fb_walllayer_restore(void)
{
    if (g_wall == NULL || g_back == NULL) {
        return;
    }
    u64 pitch_px = g_fb->pitch_bytes / 4u;
    u64 row_bytes = (u64)g_fb->width * 4u;
    for (u32 y = 0; y < g_fb->height; y++) {
        memcpy(g_back + (u64)y * pitch_px, g_wall + (u64)y * pitch_px,
               row_bytes);
    }
    fb_touch();
}

u32 *fb_walllayer_base(void)
{
    return g_wall;
}

int fb_walllayer_dirty(void)
{
    return g_wall_dirty;
}

void fb_walllayer_mark_clean(void)
{
    g_wall_dirty = 0;
}

/* --- Временное переключение активной поверхности (кэш обоев) --- */

static u32 *g_surface_override;

void fb_surface_push(u32 *base)
{
    g_surface_override = base;
}

void fb_surface_pop(void)
{
    g_surface_override = NULL;
}

void fb_present(void)
{
    if (g_back == NULL || g_dirty == 0) {
        return;
    }

    u32 *dst = (u32 *)(u64)g_fb->phys_addr;
    u64 pitch_px = g_fb->pitch_bytes / 4u;
    u64 row_bytes = (u64)g_fb->width * 4u;

    for (u32 y = 0; y < g_fb->height; y++) {
        memcpy(dst + (u64)y * pitch_px, g_back + (u64)y * pitch_px,
               row_bytes);
    }
    g_dirty = 0;
}

/* Активная поверхность: переопределение (кэш обоев), back buffer или VRAM. */
static u32 *fb_base(void)
{
    if (g_surface_override != NULL) {
        return g_surface_override;
    }
    return (g_back != NULL) ? g_back : (u32 *)(u64)g_fb->phys_addr;
}

/* Преобразует 0x00RRGGBB в нативный пиксель (единая реализация в common/fbpi.c). */
static u32 fb_pixel(u32 rgb)
{
    return fbpi_pixel_rgb(g_fb, rgb);
}

err_t fb_clear(u32 rgb)
{
    if (g_fb == NULL) {
        return K_ESTATE;
    }
    u32 *base = fb_base();
    u32 px = fb_pixel(rgb);
    u64 pitch_px = g_fb->pitch_bytes / 4u;

    for (u32 y = 0; y < g_fb->height; y++) {
        u32 *row = base + (u64)y * pitch_px;
        for (u32 x = 0; x < g_fb->width; x++) {
            row[x] = px;
        }
    }
    fb_touch();
    return K_OK;
}

/* --- Clip-прямоугольник рендера --- */

void fb_clip_set(u32 x, u32 y, u32 w, u32 h)
{
    if (g_fb == NULL) {
        return;
    }
    /* Клип всегда зажимаем в экран: примитивы дальше полагаются на
     * «clip в границах surface» (нет переполнений при пересечении). */
    if (x >= g_fb->width)  { w = 0u; }  else if (w > g_fb->width - x)  { w = g_fb->width - x; }
    if (y >= g_fb->height) { h = 0u; }  else if (h > g_fb->height - y) { h = g_fb->height - y; }
    g_clip_x = x;
    g_clip_y = y;
    g_clip_w = w;
    g_clip_h = h;
}

void fb_clip_screen(void)
{
    if (g_fb == NULL) {
        return;
    }
    g_clip_x = 0;
    g_clip_y = 0;
    g_clip_w = g_fb->width;
    g_clip_h = g_fb->height;
}

void fb_clip_get(u32 *out_x, u32 *out_y, u32 *out_w, u32 *out_h)
{
    if (out_x != NULL) { *out_x = g_clip_x; }
    if (out_y != NULL) { *out_y = g_clip_y; }
    if (out_w != NULL) { *out_w = g_clip_w; }
    if (out_h != NULL) { *out_h = g_clip_h; }
}

/* Пересечение [x, x+w) с клипом. Возвращает 0, если пусто.
 * Все величины зажаты по экрану/клипу — переполнений нет. */
static int fb_clip_rect(u32 *x, u32 *y, u32 *w, u32 *h)
{
    u32 x0 = *x, y0 = *y;
    u32 x1 = *x + *w, y1 = *y + *h;

    if (x0 < g_clip_x)              { x0 = g_clip_x; }
    if (y0 < g_clip_y)              { y0 = g_clip_y; }
    if (x1 > g_clip_x + g_clip_w)   { x1 = g_clip_x + g_clip_w; }
    if (y1 > g_clip_y + g_clip_h)   { y1 = g_clip_y + g_clip_h; }
    if (x1 <= x0 || y1 <= y0) {
        return 0;
    }
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return 1;
}

err_t fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb)
{
    if (g_fb == NULL) {
        return K_ESTATE;
    }
    if (x >= g_fb->width || y >= g_fb->height || w == 0u || h == 0u) {
        return K_ERANGE;
    }
    if (w > g_fb->width - x) {
        w = g_fb->width - x;
    }
    if (h > g_fb->height - y) {
        h = g_fb->height - y;
    }
    if (fb_clip_rect(&x, &y, &w, &h) == 0) {
        return K_OK;   /* вне клипа — легально, ничего не рисуем */
    }

    u32 px = fb_pixel(rgb);
    u32 *base = fb_base();
    u64 pitch_px = g_fb->pitch_bytes / 4u;

    for (u32 yy = 0; yy < h; yy++) {
        u32 *row = base + (u64)(y + yy) * pitch_px + x;
        for (u32 xx = 0; xx < w; xx++) {
            row[xx] = px;
        }
    }
    fb_touch();
    return K_OK;
}

err_t fb_get_dims(u32 *out_w, u32 *out_h)
{
    if (g_fb == NULL) {
        return K_ESTATE;
    }
    if (out_w != NULL) {
        *out_w = g_fb->width;
    }
    if (out_h != NULL) {
        *out_h = g_fb->height;
    }
    return K_OK;
}

u32 *fb_surface_base(void)
{
    if (g_fb == NULL) {
        return NULL;
    }
    return fb_base();
}

u64 fb_surface_pitch_px(void)
{
    if (g_fb == NULL) {
        return 0;
    }
    return g_fb->pitch_bytes / 4u;
}

const struct fbpi_framebuffer *fb_debug_info(void)
{
    return g_fb;
}

err_t fb_blit_glyph8(u32 x, u32 y, const u8 rows[8], u32 fg_rgb, u32 bg_rgb)
{
    if (g_fb == NULL) {
        return K_ESTATE;
    }
    if (rows == NULL) {
        return K_EINVAL;
    }
    if (x >= g_fb->width || y >= g_fb->height) {
        return K_ERANGE;
    }

    /* Глиф 8x8 может частично выходить за экран ИЛИ за клип (нижняя
     * строка текста у границы окна): рисуем видимую часть, а не
     * отбрасываем ячейку целиком — иначе «рваный» нижний ряд. */
    u32 x0 = x, y0 = y;
    u32 x1 = x + 8u, y1 = y + 8u;
    if (x1 > g_fb->width)            { x1 = g_fb->width; }
    if (y1 > g_fb->height)           { y1 = g_fb->height; }
    if (x0 < g_clip_x)               { x0 = g_clip_x; }
    if (y0 < g_clip_y)               { y0 = g_clip_y; }
    if (x1 > g_clip_x + g_clip_w)    { x1 = g_clip_x + g_clip_w; }
    if (y1 > g_clip_y + g_clip_h)    { y1 = g_clip_y + g_clip_h; }
    if (x0 >= x1 || y0 >= y1) {
        return K_OK;   /* невидим целиком — легально */
    }

    u32 *base = fb_base();
    u64 pitch_px = g_fb->pitch_bytes / 4u;
    u32 fg = fb_pixel(fg_rgb);
    u32 bg = fb_pixel(bg_rgb);

    for (u32 ry = y0; ry < y1; ry++) {
        u8 bits = rows[ry - y];
        u32 *row = base + (u64)ry * pitch_px;
        for (u32 rx = x0; rx < x1; rx++) {
            row[rx] = ((bits >> (rx - x)) & 1u) ? fg : bg;
        }
    }
    fb_touch();
    return K_OK;
}

err_t fb_demo_pattern(void)
{
    if (g_fb == NULL) {
        return K_ESTATE;
    }

    static const u32 bars[8] = {
        0xE5484D, 0xF5A623, 0xF0D000, 0x3FB950,
        0x38BDF8, 0x3A7BD5, 0x9D5CF2, 0xF2F4F8
    };

    fb_clear(0x14161A);

    u32 bar_h = g_fb->height / 12u;
    u32 bar_w = g_fb->width / 8u;
    for (u32 i = 0; i < 8u; i++) {
        fb_fill_rect(i * bar_w, g_fb->height - bar_h, bar_w, bar_h, bars[i]);
    }

    /* Рамка: проверка граничных пикселей и pitch. */
    u32 m = 8;
    for (u32 x = m; x < g_fb->width - m; x++) {
        fb_fill_rect(x, m, 1u, 2u, 0xF2F4F8);
        fb_fill_rect(x, g_fb->height - m - 2u, 1u, 2u, 0xF2F4F8);
    }
    for (u32 y = m; y < g_fb->height - m; y++) {
        fb_fill_rect(m, y, 2u, 1u, 0xF2F4F8);
        fb_fill_rect(g_fb->width - m - 2u, y, 2u, 1u, 0xF2F4F8);
    }

    /* Акцентный квадрат в центре. */
    u32 sq = g_fb->height / 8u;
    u32 cx = g_fb->width / 2u - sq / 2u;
    u32 cy = g_fb->height / 2u - sq / 2u;
    fb_fill_rect(cx, cy, sq, sq, 0x3A7BD5);
    fb_fill_rect(cx + sq / 4u, cy + sq / 4u, sq / 2u, sq / 2u, 0x14161A);

    fb_present();   /* паттерн виден сразу: main_loop в этот момент не крутится */
    return K_OK;
}
