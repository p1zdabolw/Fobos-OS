/*
 * Fobos OS — оверлей "режима амнезии" (ramdisk с заполнением из диска).
 *
 * Слой: fs. Чистый модуль без прямого доступа к железу: операции чтения/
 * записи диска передаются через struct ovl_io (в ядре — AHCI-блочный слой,
 * в host-тестах — мок). Пока оверлей активен, ВСЕ записи оседают только
 * в RAM: диск остаётся нетронутым, а чтения видят согласованный микс
 * "диск + оверлей". После перезагрузки/выключения оверлей исчезает вместе
 * с RAM — все изменения сеанса забываются (семантика Tails-like amnesia).
 *
 * Гранулярность — блок 4 КиБ (8 секторов по 512). Частичная запись в блок
 * делает read-modify-write из диска. Кадры данных выделяет вызывающий
 * слой через phys-аллокатор; сам оверлей оперирует номерами кадров.
 *
 * Предел: ovl_max_blocks блоков (по умолчанию 4096 = 16 МиБ изменённых
 * данных). Переполнение — K_ENOMEM на запись (диск НЕ затрагивается).
 */
#ifndef FOBOS_FS_OVL_H
#define FOBOS_FS_OVL_H

#include <fobos/errors.h>
#include <fobos/types.h>

#define OVL_SECTOR      512u
#define OVL_SECT_PER_BL 8u    /* секторов в блоке */
#define OVL_BLOCK_BYTES (OVL_SECTOR * OVL_SECT_PER_BL)

/* Бэкенд диска: read/write count секторов с lba (512 Б/сектор). */
struct ovl_io {
    err_t (*read)(u64 lba, u32 count, void *buf);
    err_t (*write)(u64 lba, u32 count, const void *buf);
    /* Аллокация/освобождение кадра 4 КиБ (номер кадра = phys >> 12). */
    err_t (*frame_alloc)(u32 *out_frame);
    void  (*frame_free)(u32 frame);
    /* Доступ к байту кадра: возвращает указатель на данные кадра 4 КиБ. */
    u8 *  (*frame_ptr)(u32 frame);
};

/* Инициализация: лимит блоков и бэкенд. io обязателен. */
err_t ovl_init(const struct ovl_io *io, u32 max_blocks);

/* Включение/выключение. ovl_disable() освобождает ВСЕ кадры (изменения
 * теряются — это и есть семантика выхода из амнезии). */
int   ovl_active(void);
void  ovl_enable(void);
void  ovl_disable(void);

/* Интерцепторы блочных операций (сектор 512 Б, count <= OVL_SECT_PER_BL). */
err_t ovl_read(u64 lba, u32 count, void *buf);
err_t ovl_write(u64 lba, u32 count, const void *buf);

/* Диагностика. */
u32   ovl_blocks_used(void);
u32   ovl_max_blocks(void);

#endif /* FOBOS_FS_OVL_H */
