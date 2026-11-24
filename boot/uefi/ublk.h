/*
 * Fobos OS — blk/rtc/klog-адаптер для FAT32-драйвера в загрузчике.
 *
 * ЗАЧЕМ: kernel/fs/fat32.c — ЕДИНЫЙ FAT32-драйвер проекта (mount, LFN,
 * чтение, запись, удаление). Установщику нужна ПОЛНАЯ запись файлов на
 * свежесформатированный целевой том до ExitBootServices. Вместо второй
 * копии FAT-логики драйвер компилируется в загрузчик как есть, а
 * платформенные зависимости подменяются этим адаптером:
 *
 *   blk_read_sectors / blk_write_sectors -> EFI_BLOCK_IO_PROTOCOL
 *   rtc_ready / rtc_read_time            -> Runtime Services GetTime
 *   klog                                 -> загрузочный serial (lserial)
 *
 * Изменения в драйвере по этой причине запрещены: он общий для ядра,
 * установщика и host-тестов.
 */
#ifndef FOBOS_UEFI_UBLK_H
#define FOBOS_UEFI_UBLK_H

#include "efi.h"

#include <fobos/errors.h>

struct fat_vol;

/* Привязка адаптера к BlockIO-протоколу (handle целевого раздела).
 * 0 = успех, -1 = протокол недоступен. Перепривязка разрешена. */
int ublk_bind(EFI_HANDLE bio_handle);

/* 1 = адаптер привязан к устройству. */
int ublk_bound(void);

/* Смонтированный том адаптера (для fat_mount/fat_* вызовов setup). */
struct fat_vol *ublk_fat(void);

/* Flush BlockIO (завершение серии записей форматирования/копирования). */
err_t ublk_flush(void);

#endif /* FOBOS_UEFI_UBLK_H */
