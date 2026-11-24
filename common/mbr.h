/*
 * Fobos OS — таблица разделов MBR (общий модуль).
 *
 * Один источник истины для трёх потребителей:
 *   - ядро (kernel/fs/blk.c): поиск тома FAT32 при storage_init;
 *   - загрузчик (boot/uefi/disk.c, fmt.c): установщик — чтение и запись
 *     таблицы разделов целевого диска;
 *   - host-тесты (tests/host): unit-покрытие без QEMU.
 *
 * Этап FND-3b: только MBR (GPT — FND-4, см. docs/roadmap.md). Формат
 * записей — классический DOS: 16 байт на запись, сигнатура 0x55AA.
 * CHS-поля заполняются стандартной заглушкой FE FF FF (как в mkimg.py):
 * и UEFI, и ядро адресуют разделы исключительно по LBA.
 */
#ifndef FOBOS_COMMON_MBR_H
#define FOBOS_COMMON_MBR_H

#include <fobos/types.h>
#include <fobos/errors.h>

#define MBR_SECTOR_SZ    512u
#define MBR_ENTRY_COUNT  4u
#define MBR_ENTRY_OFF    0x1BEu
#define MBR_SIG_OFF      510u

/* Типы разделов, значимые для проекта. */
#define MBR_PART_EMPTY      0x00u
#define MBR_PART_FAT32_CHS  0x0Bu   /* FAT32 CHS */
#define MBR_PART_FAT32_LBA  0x0Cu   /* FAT32 LBA — том данных Fobos */
#define MBR_PART_EFI_SYS    0xEFu   /* EFI System Partition — ESP */

/* CHS-заглушка (FE FF FF): "начало/конец недоступны в CHS". */
#define MBR_CHS_DUMMY       0xFEu

struct mbr_part {
    u8  boot;     /* 0x00 / 0x80 (active) */
    u8  type;     /* тип раздела; 0 = запись не занята */
    u64 lba;      /* начальный LBA */
    u64 sectors;  /* размер в секторах */
};

/* Разбор MBR-сектора. Возвращает число непустых записей (0..4) или -1
 * (NULL-аргумент либо нет сигнатуры 0x55AA). out может быть NULL — тогда
 * только подсчёт. Записи копируются в out в исходном порядке. */
int mbr_parse(const u8 sector[MBR_SECTOR_SZ], struct mbr_part out[MBR_ENTRY_COUNT]);

/* Первая партиция FAT32 (0x0B/0x0C) — семантика ядра (storage_init):
 * сигнатура обязана быть, записи с sectors==0 пропускаются.
 * K_OK / K_ENODEV (нет FAT32) / K_EINVAL (нет сигнатуры). */
err_t mbr_find_fat32(const u8 sector[MBR_SECTOR_SZ], u64 *out_lba,
                     u64 *out_sectors);

/* Сериализует таблицу разделов в MBR-сектор: загрузочный код не трогается
 * (нули, если сектор был обнулён), сигнатура 0x55AA ставится всегда.
 * NULL-записи (type==0) записываются как пустые. */
void mbr_write(u8 sector[MBR_SECTOR_SZ],
               const struct mbr_part entries[MBR_ENTRY_COUNT]);

#endif /* FOBOS_COMMON_MBR_H */
