/*
 * Fobos OS — framebuffer (UEFI GOP linear mode) (прототипы).
 */
#ifndef FOBOS_DRV_FB_H
#define FOBOS_DRV_FB_H

#include <fobos/boot_info.h>
#include <fobos/errors.h>

/* Принимает уже провалидированную boot_info. Сохраняет ссылку (время жизни = ядру). */
err_t fb_init(const struct fbpi_boot_info *bi);

/* --- Двойная буферизация (FND-2d) ---
 *
 * Вся отрисовка идёт в задний буфер в RAM (та же геометрия/pitch, что у
 * VRAM); fb_present() переносит его в видеопамять ОДНОЙ процедурой —
 * промежуточные состояния кадра (градиент фона, окна по одному, курсор)
 * на экране не появляются, мерцание исчезает.
 *
 * fb_backbuffer_init() вызывается после phys_init/fb_init и ПОСЛЕ kselftest
 * (аллокатор выдаёт 768+ смежных кадров; тест исчерпания обязан видеть пул
 * без нашего резерва). При нехватке памяти слой честно работает напрямую
 * с VRAM (режим до FND-2d), fb_backbuffer_active() == 0. */
err_t fb_backbuffer_init(void);
int   fb_backbuffer_active(void);

/* Переносит задний буфер в VRAM. No-op, если буфера нет или ничего
 * не менялось с прошлого present (dirty-флаг взводят все примитивы). */
void  fb_present(void);

/* Кэш обоев (FND-2e): процедурный фон дорог (тысячи fill_rect под TCG),
 * поэтому он рисуется РАЗ и хранится в отдельном слое; каждый кадр
 * начинается с быстрого row-copy слоя в активную поверхность.
 * fb_walllayer_init() после fb_backbuffer_init(); при нехватке памяти
 * слой нет (0) — фон рисуется процедурно каждый кадр (как раньше). */
err_t fb_walllayer_init(void);
int   fb_walllayer_active(void);
void  fb_walllayer_invalidate(void);   /* заставить перерисовать слой */
/* Копирует слой обоев в активную поверхность (весь экран). */
void  fb_walllayer_restore(void);
/* Базовый адрес слоя (NULL, если слоя нет) — рисование фона в него. */
u32  *fb_walllayer_base(void);

void fb_surface_push(u32 *base);
void fb_surface_pop(void);
/* Dirty-флаг кэша обоев (для uidraw_background). */
int   fb_walllayer_dirty(void);
void  fb_walllayer_mark_clean(void);

/* Заполнение цветом 0x00RRGGBB. Координаты в пикселях.
 * Оба примитива уважают clip-прямоугольник (см. fb_clip_set): рисуют
 * только его пересечение с запрошенной областью. Выход за клип — не
 * ошибка (легальный случай «рисуем содержимое окна»); выход за экран
 * тоже зажимается. */
err_t fb_clear(u32 rgb);
err_t fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb);

/* --- Clip-прямоугольник рендера (FND-2e) ---
 *
 * Системное ограничение вывода: пока активен клип окна, ЛЮБОЙ контент
 * (заливки, текст, глифы) физически не может выйти за его границы —
 * класс «текст вылезает за рамку окна» устранён по построению, а не
 * разбором каждой строки.
 *
 * fb_clip_set задаёт клип (например, клиентскую область окна);
 * fb_clip_screen возвращает «рисовать везде». Вложенности нет —
 * композитор сохраняет/восстанавливает сам. По умолчанию весь экран. */
void  fb_clip_set(u32 x, u32 y, u32 w, u32 h);
void  fb_clip_screen(void);
void  fb_clip_get(u32 *out_x, u32 *out_y, u32 *out_w, u32 *out_h);

/* Размеры активного режима (пиксели). */
err_t fb_get_dims(u32 *out_w, u32 *out_h);

/* Активная поверхность рисования (задний буфер или VRAM) для слоёв,
 * которым нужен прямой пиксельный доступ (курсор uidraw: сохранение/
 * восстановление фона). */
u32  *fb_surface_base(void);
u64   fb_surface_pitch_px(void);

/* Прямой доступ к описанию VRAM-режима. Только для чтения/диагностики. */
const struct fbpi_framebuffer *fb_debug_info(void);

/* Отрисовка глифа 8x8: rows[строка], бит i = столбец i (LSB слева).
 * Фон ячейки заливается всегда (сплошной текст). out вне экрана -> K_ERANGE. */
err_t fb_blit_glyph8(u32 x, u32 y, const u8 rows[8], u32 fg_rgb, u32 bg_rgb);

/* Демо-паттерн FND-1: тёмный фон, цветные полосы, рамка, акцентный квадрат.
 * Используется как визуальная проверка маппинга framebuffer и командой
 * `fbtest` в shell (FND-2a). */
err_t fb_demo_pattern(void);

#endif /* FOBOS_DRV_FB_H */
