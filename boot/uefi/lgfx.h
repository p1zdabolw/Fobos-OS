/*
 * Fobos OS — графика загрузчика поверх GOP framebuffer (для OOBE-мастера).
 *
 * Преобразование цвета — через общий fbpi_pixel_rgb() (единый модуль с
 * ядром). Здесь только примитивы, нужные Setup: заливка, глиф 8x8, текст
 * (в т.ч. центрированный), панель с рамкой, поле ввода.
 */
#ifndef FOBOS_UEFI_LGFX_H
#define FOBOS_UEFI_LGFX_H

#include <fobos/types.h>
#include <fobos/boot_info.h>

/* Цвета 0x00RRGGBB (совпадают с палитрой ядра — единый стиль). */
#define LGFX_COL_BG_TOP     0x1A1F28u
#define LGFX_COL_BG_BOT     0x0B0D11u
#define LGFX_COL_PANEL      0x141A23u
#define LGFX_COL_FIELD      0x0E1116u
#define LGFX_COL_BORDER     0x2E3542u
#define LGFX_COL_BORDER_SEL 0x4F8CCFu
#define LGFX_COL_ACCENT     0x4F8CCFu
#define LGFX_COL_HEADER_TX  0x8FA3BFu
#define LGFX_COL_TX         0xE8EAEDu
#define LGFX_COL_TX_DIM     0xB8C0CCu
#define LGFX_COL_OK         0x3FB950u
#define LGFX_COL_ERR        0xE5484Du

/* Привязка к данным GOP (структура из boot info). 0 = успех. */
int lgfx_init(const struct fbpi_framebuffer *fb);

u32 lgfx_width(void);
u32 lgfx_height(void);

void lgfx_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb);
void lgfx_background(void);

/* Глиф 8x8 (контракт общий с fb_blit_glyph8). */
void lgfx_glyph(u32 x, u32 y, const u8 rows[8], u32 fg, u32 bg);
void lgfx_text(u32 x, u32 y, const char *s, u32 fg, u32 bg);
void lgfx_text_centered(u32 y, const char *s, u32 fg, u32 bg);
void lgfx_box(u32 x, u32 y, u32 w, u32 h, u32 fill, u32 border);

/* Поле ввода: рамка (акцент при focus), текст, блок-курсор. */
void lgfx_input_field(u32 x, u32 y, u32 w, const char *text,
                      u32 cursor_pos, int focused, int masked);

#endif /* FOBOS_UEFI_LGFX_H */
