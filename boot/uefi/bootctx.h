/*
 * Fobos OS — общий контекст загрузчика (модули фазы Setup).
 *
 * До ExitBootServices все модули загрузчика работают с одними и теми же
 * таблицами EFI. Вместо собственных копий глобальных указателей каждый
 * модуль (espfile, disk, fmt, setup, ublk) получает их отсюда. Аллокатор
 * "ниже 4 ГиБ" — тоже одна точка: после прыжка ядро видит эти размещения
 * через identity-окно (см. docs/boot-protocol.md).
 */
#ifndef FOBOS_UEFI_BOOTCTX_H
#define FOBOS_UEFI_BOOTCTX_H

#include "efi.h"

/* Привязка контекста. Вызывается один раз из efi_main до любых операций. */
void bootctx_init(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);

EFI_HANDLE bootctx_image(void);
struct EFI_SYSTEM_TABLE *bootctx_st(void);
struct EFI_BOOT_SERVICES *bootctx_bs(void);

/* Размещение size байт не выше 4 ГиБ (требование identity-окна ядра).
 * EFI_SUCCESS или код ошибки прошивки. */
EFI_STATUS bootctx_alloc_below_4g(u32 mem_type, u64 size, u64 *out_phys);

/* Освобождение размещения bootctx_alloc_below_4g (тот же размер). */
EFI_STATUS bootctx_free(u64 phys, u64 size);

/* AllocatePool/FreePool (Boot Services Data). */
EFI_STATUS bootctx_alloc_pool(u64 size, void **out_buf);
void bootctx_free_pool(void *buf);

#endif /* FOBOS_UEFI_BOOTCTX_H */
