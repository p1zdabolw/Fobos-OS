/*
 * Fobos OS — примитивы прямого рисования поверх framebuffer (FND-2b).
 *
 * Общий слой для splash-экрана, экрана входа, статус-бара и будущих
 * kernel-side экранов. Всё рисование — напрямую в fb (без консоли).
 * Курсор мыши (спрайт с сохранением фона) тоже живёт здесь.
 *
 * Ограничение этапа: монохромный шрифт 8x8 (ASCII), координаты в физических
 * пикселях. DPI/масштабирование — в GUI-этапах (roadmap).
 */
#ifndef FOBOS_GFX_UIDRAW_H
#define FOBOS_GFX_UIDRAW_H

#include <fobos/errors.h>
#include <fobos/types.h>

struct mouse_event;

/* Базовая палитра экранов FND-2b (0x00RRGGBB). */
#define UI_COL_BG_TOP     0x1A1F28u
#define UI_COL_BG_BOT     0x0B0D11u
#define UI_COL_PANEL      0x141A23u
#define UI_COL_FIELD      0x0E1116u
#define UI_COL_BORDER     0x2E3542u
#define UI_COL_BORDER_SEL 0x4F8CCFu
#define UI_COL_ACCENT     0x4F8CCFu
#define UI_COL_HEADER_TX  0x8FA3BFu
#define UI_COL_TX         0xE8EAEDu
#define UI_COL_TX_DIM     0xB8C0CCu
#define UI_COL_OK         0x3FB950u
#define UI_COL_ERR        0xE5484Du
#define UI_COL_BYELLOW    0xFFE066u
#define UI_COL_BCYAN      0x5CD6F5u
#define UI_COL_BAR        0x12161Du

/* Размеры текста/кнопок. */
#define UI_TEXT_W   8u
#define UI_TEXT_H   8u
#define UI_BTN_H    18u

/* Инициализация (кэширует размеры fb). Возвращает K_OK/K_ESTATE. */
err_t uidraw_init(void);

u32 uidraw_width(void);
u32 uidraw_height(void);

/* Вертикальный градиент фона. */
void uidraw_background(void);

/* Текст 8x8; символы вне 0x20..0x7E рисуются как '.'. Клиппинг по краю. */
void uidraw_text(u32 x, u32 y, const char *s, u32 fg, u32 bg);

/* Текст по центру строки y. */
void uidraw_text_centered(u32 y, const char *s, u32 fg, u32 bg);

/* Текст с усечением (FND-2e): если строка шире max_w — рисуется
 * умещающаяся часть + "..". Возвращает фактическую занятую ширину. */
u32 uidraw_text_fit(u32 x, u32 y, u32 max_w, const char *s, u32 fg, u32 bg);

/* Перенос по словам (FND-2e): до max_lines строк высотой line_h в полосе
 * max_w. Возвращает число нарисованных строк. Лишний текст отбрасывается
 * (вызывающий отвечает за max_lines с запасом). */
u32 uidraw_text_wrap(u32 x, u32 y, u32 max_w, u32 max_lines, u32 line_h,
                     const char *s, u32 fg, u32 bg);

/* --- Тема (FND-2e): runtime-акцент и обои ---
 *
 * Акцент — переменная состояния (замена константы UI_COL_ACCENT в
 * композиторе): применяется немедленно ко всем экранам без перезагрузки.
 * Обои — процедурные варианты, тоже меняются на лету. */
u32  uidraw_accent(void);
void uidraw_set_accent(u32 rgb);

#define UI_WALL_GRADIENT 0u   /* классический градиент FND-2a */
#define UI_WALL_BLOOM    1u   /* «bloom»: концентрические ленты (стиль Win11) */
#define UI_WALL_HILLS    2u   /* холмы + солнце */
#define UI_WALL_AURORA   3u   /* синус-ленты полярного сияния */
#define UI_WALL_COUNT    4u

void uidraw_set_wall(u32 id);
u32  uidraw_wall(void);

/* --- Процедурные иконки приложений (FND-2e) ---
 *
 * Квадрат size x size в точке (x, y); все иконки рисуются примитивами
 * (никаких ассетов). focused — подсветка рамки акцентом. */
enum {
    UI_ICON_CMD = 0,
    UI_ICON_EXPLORER,
    UI_ICON_ABOUT,
    UI_ICON_VIEWER,
    UI_ICON_NOTEPAD,
    UI_ICON_CALC,
    UI_ICON_SETTINGS,
    UI_ICON_COUNT
};

void uidraw_app_icon(u32 kind, u32 x, u32 y, u32 size, int focused);

/* Залитый диск (аватары, маркеры) — центр/радиус в пикселях. */
void uidraw_disc(u32 cx, u32 cy, u32 r, u32 col);

/* Логотип Fobos (квадрат size x size): тёмная плитка, планета с кольцом
 * и спутником — знак системы на кнопке Пуск. */
void uidraw_logo(u32 x, u32 y, u32 size);

/* Панель с рамкой 1 px. */
void uidraw_box(u32 x, u32 y, u32 w, u32 h, u32 fill, u32 border);

/* Поле ввода: рамка (акцент при focus), текст, блок-курсор на позиции. */
void uidraw_input_field(u32 x, u32 y, u32 w, const char *text,
                        u32 cursor_pos, int focused, int masked);

/* Кнопка; возвращает ширину, которую она занимает (UI_BTN_W_OF). */
void uidraw_button(u32 x, u32 y, u32 w, const char *label, int focused);

/* Попадание точки (mx,my) в кнопку с геометрией (x,y,w,UI_BTN_H). */
int uidraw_button_hit(u32 x, u32 y, u32 w, u32 mx, u32 my);

/* Попадание в поле (x,y,w,h). */
int uidraw_rect_hit(u32 x, u32 y, u32 w, u32 h, u32 mx, u32 my);

/* --- Курсор мыши --- */

/* Пересчитать позицию курсора по событию (клэмп по экрану) и перерисовать.
 * В режиме uidraw_cursor_defer(1) (TUI) координаты обновляются, а спрайт
 * рисует кадровый конвейер: события мыши НЕ пишут в framebuffer. */
void uidraw_mouse_event(const struct mouse_event *ev);

/* Включить/выключить кадровый режим курсора (TUI = 1, GUI/логин = 0). */
void uidraw_cursor_defer(int on);

/* Прямоугольник экрана, где спрайт курсора напечатан прямо сейчас.
 * 1 = напечатан (кадровый конвейер обязан перекрасить эту зону перед
 * новым штампом — иначе остаются следы). */
int uidraw_cursor_stamp_rect(u32 *out_x, u32 *out_y, u32 *out_w, u32 *out_h);

/* Позиция курсора (пиксели). */
void uidraw_mouse_pos(u32 *out_x, u32 *out_y);

/* Кнопки мыши (MOUSE_BTN_*). */
u32 uidraw_mouse_buttons(void);

/* Штамп спрайта (сохранение фона + отрисовка); фиксирует stamp-прямоугольник. */
void uidraw_cursor_draw(void);

/* Восстановление фона из последнего штампа. Корректен только в тех
 * сценариях, где зона под спрайтом не могла измениться после штампа
 * (GUI-скобки erase->draw). В TUI не используется — там зону
 * перекрашивает con_flush_rect_px. */
void uidraw_cursor_erase(void);

#endif /* FOBOS_GFX_UIDRAW_H */
