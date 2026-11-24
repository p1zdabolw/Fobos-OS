/*
 * Fobos OS — драйвер 16550 UART (прототипы).
 */
#ifndef FOBOS_DRV_SERIAL_H
#define FOBOS_DRV_SERIAL_H

#include <fobos/types.h>
#include <fobos/errors.h>

#define SERIAL_COM1 0x3F8u

/* Настраивает порт: 115200 8N1, FIFO включён, прерывания выключены (FND-1).
 * Повторный вызов безопасен (перенастраивает регистры). */
err_t serial_init(u16 port);

/* Синхронная запись. До успешного serial_init() — no-op (защита от ранних вызовов). */
void serial_write(const char *data, u64 len);

#endif /* FOBOS_DRV_SERIAL_H */
