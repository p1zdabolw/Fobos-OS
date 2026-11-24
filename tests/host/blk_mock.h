/*
 * Fobos OS — host-мок блочного устройства (RAM-диск) для host-тестов.
 *
 * Покрывает ЕДИНЫЙ fat32-драйвер (kernel/fs/fat32.c) и общий форматтер
 * (common/fmtfat32.c) без QEMU: том форматируется тем же fmtfat32_format,
 * что использует установщик в загрузчике. rtc_* — детерминированная
 * заглушка (fat32-драйвер ставит метки времени записям).
 *
 * Только для host-сборки (FOBOS_HOST_TEST).
 */
#ifndef FOBOS_TESTS_HOST_BLK_MOCK_H
#define FOBOS_TESTS_HOST_BLK_MOCK_H

#include <fobos/types.h>
#include <fobos/errors.h>

/* Создаёт RAM-диск из sectors секторов по 512 Б (обнулён).
 * 0 = успех, -1 = не хватило памяти. */
int blk_mock_init(u64 sectors);

/* Форматирует ВЕСЬ диск как один том FAT32 (раздел = весь диск,
 * part_lba = 0 недопустим для fat_mount, поэтому mock форматирует
 * [1, sectors)). 0 = успех. */
err_t blk_mock_format_fat32(void);

/* Освобождает RAM-диск. */
void blk_mock_done(void);

#endif /* FOBOS_TESTS_HOST_BLK_MOCK_H */
