/*
 * Fobos OS — драйвер PS/2 мыши (стандартный протокол, 3 байта/пакет).
 *
 * Формат пакета (streaming mode, defaults 0xF6):
 *   байт 0: [7]=Y overflow [6]=X overflow [5]=Y sign [4]=X sign
 *           [3]=always 1  [2]=middle [1]=right [0]=left
 *   байт 1: X delta; байт 2: Y delta (вверх = положительный —
 *   для экранных координат инвертируется в ui-слое).
 *
 * Контекст ISR: только парсер + очередь; klog запрещён.
 */
#ifndef FOBOS_DRV_MOUSE_H
#define FOBOS_DRV_MOUSE_H

#include <fobos/errors.h>
#include <fobos/types.h>

#define MOUSE_BTN_LEFT   0x01u
#define MOUSE_BTN_RIGHT  0x02u
#define MOUSE_BTN_MIDDLE 0x04u

struct mouse_event {
    u8  buttons;  /* MOUSE_BTN_* */
    s16 dx;       /* экранные пиксели: вправо положительно */
    s16 dy;       /* экранные пиксели: вниз положительно */
};

struct mouse_stats {
    u32 packets_ok;
    u32 packets_bad;   /* рассинхрон/переполнение движения */
    u32 dropped;       /* потеряно событий (переполнение очереди) */
    u32 queue_used;
};

err_t mouse_init(void);

/* Неблокирующее чтение события. K_OK / K_EAGAIN. */
err_t mouse_poll(struct mouse_event *out);

struct mouse_stats mouse_get_stats(void);

/* Последнее известное состояние кнопок (MOUSE_BTN_*). */
u8 mouse_buttons(void);

/* Самопроверка парсера пакетов (чистая функция, работает и на host):
 * возвращает число упавших проверок. */
int mouse_selftest(void);

#endif /* FOBOS_DRV_MOUSE_H */
