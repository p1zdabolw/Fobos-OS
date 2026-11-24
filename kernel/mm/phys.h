/*
 * Fobos OS — физический менеджер памяти (прототипы).
 */
#ifndef FOBOS_MM_PHYS_H
#define FOBOS_MM_PHYS_H

#include <fobos/boot_info.h>
#include <fobos/errors.h>
#include <fobos/types.h>

#define PAGE_SIZE 4096u

/* Стратегия FND-1: bitmap-аллокатор 1 бит/кадр, carved из свободной RAM.
 * Замена на buddy+slab — FND-2 (см. docs/roadmap.md); bitmap остаётся
 * рабочим и корректным до этого момента, это не заглушка. */

struct phys_stats {
    u64 total_frames;     /* весь диапазон, покрытый bitmap */
    u64 free_frames;      /* доступные для alloc (conventional, < 4 ГиБ) */
    u64 reserved_frames;  /* занятые прошивкой/ядром/самим bitmap */
    u64 high_free_bytes;  /* conventional-память >= 4 ГиБ (не используется на FND-1) */
};

err_t phys_init(const struct fbpi_boot_info *bi);

/* Выделяет один кадр 4 КиБ. Выравнивание гарантируется природой кадра. */
err_t phys_alloc_frame(u64 *out_paddr);

/* Выделяет count ПОДРЯД идущих кадров (нужно поверхностям вроде заднего
 * буфера framebuffer, blit'ящимся одной процедурой). Физически смежные
 * кадры обязаны лежать ниже 4 ГиБ — там, где действует identity-отображение
 * загрузчика. Освобождение — phys_free_frame по каждому кадру. */
err_t phys_alloc_contig(u32 count, u64 *out_paddr);

/* Возвращает кадр в пул. Двойное освобождение — ошибка (K_ESTATE), не молчаливый no-op. */
err_t phys_free_frame(u64 paddr);

/* Резервирует диапазон [paddr, paddr+bytes): используется ядром/драйверами
 * для памяти, не учтённой в карте EFI как занятая. Диапазон выравнивается наружу. */
err_t phys_reserve_range(u64 paddr, u64 bytes);

struct phys_stats phys_get_stats(void);

#endif /* FOBOS_MM_PHYS_H */
