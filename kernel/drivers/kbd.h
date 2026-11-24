/*
 * Fobos OS — драйвер PS/2 клавиатуры (прототипы).
 *
 * Раскладка US, сканкод-набор 1 (трансляция 8042 включена в ps2.c).
 * Драйвер формирует события {код клавиши, флаги модификаторов, ASCII-символ}
 * и ведёт кольцевую очередь. Автоповтор — программный (в QEMU и на части
 * железа тип-матика отсутствует): удерживаемая печатная клавиша повторяется
 * с задержкой KBD_REPEAT_DELAY_MS и периодом KBD_REPEAT_PERIOD_MS.
 *
 * Контекст: обработчик IRQ НЕ логирует и НЕ спит; вся диагностика —
 * счётчики, читаемые из main-контекста.
 */
#ifndef FOBOS_DRV_KBD_H
#define FOBOS_DRV_KBD_H

#include <fobos/errors.h>
#include <fobos/types.h>

#define KBD_REPEAT_DELAY_MS  400u
#define KBD_REPEAT_PERIOD_MS  40u

/* Коды клавиш (не зависят от раскладки; для навигационных и служебных). */
enum {
    KEY_NONE = 0,
    /* печатные: 'a'..'z' -> KEY_A..KEY_Z (1..26), '0'..'9' -> KEY_0..KEY_9
     * (27..36), прочие печатные — спец-коды 37..58; символ берётся из ch */
    KEY_SPACE = 37,
    KEY_ENTER, KEY_ESC, KEY_BSPACE, KEY_TAB,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_INS, KEY_DEL,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_CAPS, KEY_NUM, KEY_SCROLL,
    KEY_MAX
};

/* Флаги модификаторов события/состояния. */
#define KBD_FL_SHIFT 0x01u
#define KBD_FL_CTRL  0x02u
#define KBD_FL_ALT   0x04u
#define KBD_FL_CAPS  0x08u   /* состояние capslock (не зажат physically) */

struct kbd_event {
    u8  code;   /* KEY_* ; печатные клавиши имеют и ch */
    u8  flags;  /* KBD_FL_* */
    char ch;    /* ASCII-символ с учётом shift/caps; 0 если не печатная */
};

struct kbd_stats {
    u32 pushed;    /* событий помещено в очередь */
    u32 dropped;   /* потеряно при переполнении очереди */
    u32 unmapped;  /* неопознанных сканкодов */
    u32 queue_used;/* текущий размер очереди */
};

err_t kbd_init(void);

/* Неблокирующее чтение события (синтезирует автоповтор). */
err_t kbd_poll(struct kbd_event *out);

/* Хук пробуждения читателей (планировщик): вызывается из IRQ после
 * помещения события в очередь. IF=0. Устанавливается sched_init. */
void kbd_set_wake_hook(void (*fn)(void));

/* Блокирующее чтение: hlt до появления события. Не использовать в IRQ. */
err_t kbd_wait(struct kbd_event *out);

/* Текущее состояние модификаторов (KBD_FL_*). */
u8 kbd_mods(void);

struct kbd_stats kbd_get_stats(void);

/* Самопроверка трансляции (чистая функция, работает и на host):
 * возвращает число упавших проверок. */
int kbd_selftest(void);

#endif /* FOBOS_DRV_KBD_H */
