/*
 * Fobos OS — последовательный порт загрузчика (COM1, 16550).
 *
 * Независимая инициализация UART: OVMF/прошивка не обязаны оставлять COM1
 * настроенным. Используется для машиночитаемых маркеров CI (qemu_selftest)
 * и диагностики загрузки.
 */
#ifndef FOBOS_UEFI_LSERIAL_H
#define FOBOS_UEFI_LSERIAL_H

#include <fobos/types.h>

void lserial_init(void);
void lserial_puts(const char *s);
void lserial_put_hex(u64 v);

#endif /* FOBOS_UEFI_LSERIAL_H */
