/*
 * Fobos OS — файловые операции с загрузочным ESP (общий модуль).
 *
 * Выделено из loader.c (FND-3b): операции нужны и самому загрузчику
 * (fobos.cfg/setup.cfg), и установщику (чтение дистрибутива при копировании
 * на целевой диск). Один источник истины для доступа к SimpleFileSystem
 * загрузочного устройства.
 */
#ifndef FOBOS_UEFI_ESPFILE_H
#define FOBOS_UEFI_ESPFILE_H

#include "efi.h"

/* Читает файл ESP целиком. Буфер — ниже 4 ГиБ (bootctx_alloc_below_4g);
 * освобождение — espfile_free() с тем же размером. 0 = успех. */
int espfile_read(const char *path_a, u64 max_size, u8 **out_buf, u32 *out_size);

/* Освобождает буфер, полученный от espfile_read. */
void espfile_free(u8 *buf, u32 size);

/* Записывает файл (существующий удаляется — атомарность не требуется
 * на этапе настройки). 0 = успех. */
int espfile_put(const char *path_a, const u8 *data, u32 len);

/* Удаляет файл, если он есть (ошибки отсутствия — не ошибка). */
void espfile_delete(const char *path_a);

#endif /* FOBOS_UEFI_ESPFILE_H */
