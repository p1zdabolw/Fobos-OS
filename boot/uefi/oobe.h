/*
 * Fobos OS — OOBE-мастер первого запуска в загрузчике (FND-2b).
 *
 * Почему Setup живёт в загрузчике: только он имеет доступ к UEFI Runtime
 * Services (SetVariable) и может СОХРАНИТЬ результаты до ExitBootServices.
 * Ядро после ExitBootServices сохранить ничего не может (дисковая
 * подсистема появится в FND-2c). Графика — общий шрифт + lgfx-примитивы
 * поверх GOP; ввод — ConIn (прошивочный PS/2-драйвер).
 *
 * Шаги: welcome -> hostname -> [username -> password -> confirm -> more]*
 * -> save. По завершении формируется struct fobos_config для записи в
 * UEFI-переменную FobosSetup и передачи ядру через FBPI.
 */
#ifndef FOBOS_UEFI_OOBE_H
#define FOBOS_UEFI_OOBE_H

#include <fobos/types.h>
#include "../../common/foboscfg.h"

struct EFI_SYSTEM_TABLE;

/* Привязка (ConIn/ConOut). 0 = успех. */
int oobe_init(struct EFI_SYSTEM_TABLE *st);

/* Окно boot manager (1 с): подсказка и ловушки ESC/Shift.
 * 0 = ничего, 1 = ESC (сброс Setup), 2 = Shift (консоль ядра). */
int oobe_esc_window(void);

/* Запуск мастера; заполняет out_cfg. 0 = успех. */
int oobe_run(struct fobos_config *out_cfg);

#endif /* FOBOS_UEFI_OOBE_H */
