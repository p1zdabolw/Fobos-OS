/*
 * Fobos OS — системный установщик (FND-3b).
 *
 * Мастер в UEFI-фазе: лицензия -> выбор диска -> Drive options (advanced):
 * создание/удаление/форматирование разделов -> подтверждение -> установка
 * (MBR + два тома FAT32 + копирование дистрибутива) -> завершение.
 *
 * Разметка целевого диска (совместима с ядром и прошивкой):
 *   раздел 1: type 0xEF, ESP — загрузчики и ядро (\EFI\BOOT\BOOTX64.EFI,
 *             \fobos\kernel.elf, fobos.cfg, setup.cfg);
 *   раздел 2: type 0x0C, том данных Fobos (\fobos\... — файлы пользователя).
 * Ядро монтирует первый раздел 0x0B/0x0C (том данных); ESP обслуживается
 * загрузчиком через SimpleFileSystem. GPT — FND-4 (ядро читает только MBR).
 *
 * Диск, с которого загружен установщик, исключается из целей: его ESP
 * обслуживает текущий сеанс (SimpleFileSystem прошивки остаётся открытым).
 */
#ifndef FOBOS_UEFI_SETUP_H
#define FOBOS_UEFI_SETUP_H

#include "efi.h"

struct setup_target {
    int completed;        /* 1 = установка доведена до конца */
    int restart;          /* 1 = пользователь выбрал перезагрузку */
    EFI_HANDLE esp_bio;   /* BlockIO диска с ESP */
    u64 esp_lba;
    u64 esp_sectors;
    EFI_HANDLE data_bio;  /* BlockIO диска с томом данных */
    u64 data_lba;
    u64 data_sectors;
};

/* Мастер установки. 0 = мастер доведён до конца (out заполнен; completed
 * отличает установку от отказа), -1 = мастер отменён (загрузка продолжается
 * по обычному сценарию).
 * cfg_raw/cfg_size — конфиг установочного носителя (NULL/0 = нет): перед
 * подсказкой перезагрузки он переносится на ESP установленной системы,
 * чтобы первая загрузка прошла без OOBE; выбор на экране завершения
 * (перезагрузка/продолжение) на перенос не влияет. */
int setup_run(struct setup_target *out, const u8 *cfg_raw, u32 cfg_size);

#endif /* FOBOS_UEFI_SETUP_H */
