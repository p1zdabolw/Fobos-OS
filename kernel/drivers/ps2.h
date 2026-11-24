/*
 * Fobos OS — контроллер клавиатуры/мыши 8042 (i8042) (прототипы).
 *
 * Отвечает за программирование контроллера и транспорт байт к портам 1/2.
 * Протоколы устройств (сканкоды, пакеты мыши) — drivers/kbd.c и drivers/mouse.c.
 */
#ifndef FOBOS_DRV_PS2_H
#define FOBOS_DRV_PS2_H

#include <fobos/errors.h>
#include <fobos/types.h>

struct ps2_info {
    int dual_channel;   /* второй порт физически существует */
    int kbd_ok;         /* порт 1 прошёл interface test */
    int mouse_ok;       /* порт 2 прошёл interface test (0 если нет порта) */
    u8  config;         /* итоговый конфигурационный байт */
};

err_t ps2_init(struct ps2_info *out);

/* Низкоуровневый транспорт (для драйверов и selftest).
 * Все функции синхронные, с ограниченным числом попыток. */
err_t ps2_write_port1(u8 b);   /* клавиатура */
err_t ps2_write_port2(u8 b);   /* мышь (через 0xD4) */
err_t ps2_read_data(u8 *out);  /* чтение из 0x60, если есть данные */

/* Самопроверка контроллера (0xAA): 0x55 = OK. Для kselftest. */
err_t ps2_controller_selftest(u8 *out);

/* Команды устройств с ожиданием ACK (0xFA): включение сканирования и т.п.
 * Использовать только с выключенными прерываниями (init-фаза). */
err_t ps2_port1_cmd(u8 b);
err_t ps2_port2_cmd(u8 b);

#endif /* FOBOS_DRV_PS2_H */
