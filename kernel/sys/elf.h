/*
 * Fobos OS — ELF64-загрузчик исполняемых образов (FND-3a).
 *
 * Разделение слоёв: elf.c — ЧИСТЫЙ парсер (валидация заголовков,
 * итерация PT_LOAD-сегментов без обращения к памяти/таблицам страниц).
 * Он компилируется и в ядре, и в host-тестах (tests/host). Маппинг
 * сегментов делает ядро через callback (sys/task.c, load_segment_cb).
 *
 * Поддерживается: ET_EXEC, EM_X86_64, PT_LOAD. Не поддерживается
 * (отказ с K_ENOSYS/K_EINVAL): PIE/динамическая линковка (ET_DYN),
 * интерпретатор (PT_INTERP), релокации, сжатие. Это честные отказы.
 */
#ifndef FOBOS_SYS_ELF_H
#define FOBOS_SYS_ELF_H

#include <fobos/errors.h>
#include <fobos/types.h>

/* p_flags (ELF ABI). */
#define ELF_PF_X 0x1u
#define ELF_PF_W 0x2u
#define ELF_PF_R 0x4u

struct elf_load_seg {
    u64 vaddr;    /* виртуальный адрес начала (не выровнен на страницу) */
    u64 filesz;   /* байт из файла */
    u64 memsz;    /* байт в памяти (filesz <= memsz; хвост — нули) */
    u64 offset;   /* смещение в файле */
    u32 flags;    /* ELF_PF_* */
};

/* Коллбек на каждый PT_LOAD-сегмент (в порядке следования в файле). */
typedef err_t (*elf_seg_cb)(void *ctx, const struct elf_load_seg *seg,
                            const u8 *img);

/* Валидация и итерация загрузочных сегментов образа.
 * Возвращает точку входа через out_entry.
 * Коды ошибок: K_EINVAL — некорректные заголовки/диапазоны;
 * K_ENOSYS — образ валиден, но не поддерживается (ET_DYN и пр.);
 * K_EIO — сегменты ссылаются за пределы образа. */
err_t elf64_load_image(const u8 *img, u32 len, u64 *out_entry,
                       elf_seg_cb cb, void *ctx);

/* Быстрая валидация без итерации (для `run` до выделения ресурсов):
 * K_OK если образ пригоден к загрузке. */
err_t elf64_validate(const u8 *img, u32 len);

#endif /* FOBOS_SYS_ELF_H */
