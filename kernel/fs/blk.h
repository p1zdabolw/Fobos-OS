/*
 * Fobos OS — блочная подсистема хранения (этап FND-2c).
 *
 * Слой: fs. Собирает цепочку: PCI -> AHCI -> разбор MBR (партиция FAT32)
 * -> FAT32-том. Все обращения файлового слоя идут через blk_read_sectors/
 * blk_write_sectors: здесь же живёт интерцептор режима амнезии (ovl).
 *
 * Отказы железа не паникуют ядро: storage_init() возвращает err_t,
 * состояние доступно blk_get_status() для shell/About-окна.
 */
#ifndef FOBOS_FS_BLK_H
#define FOBOS_FS_BLK_H

#include <fobos/errors.h>
#include <fobos/types.h>
#include "fat32.h"

struct blk_status {
    int ahci_ok;          /* HBA найден и инициализирован */
    int disk_ok;          /* есть готовый SATA-порт */
    int fat_ok;           /* FAT32-том смонтирован */
    u8  port;             /* номер порта с диском */
    char model[41];       /* модель диска (IDENTIFY) */
    u64 disk_sectors;     /* ёмкость диска, секторы */
    u64 part_lba;         /* LBA начала партиции FAT32 */
    u64 part_sectors;     /* размер партиции, секторы */
    u64 fat_total_kb;     /* объём тома, КиБ */
    u64 fat_free_kb;      /* свободно, КиБ (оценка по FSInfo/скану FAT) */
    u32 amnesia;          /* 1 = режим амнезии включён */
    u32 ovl_blocks;       /* занято блоков оверлея */
    u32 ovl_limit;        /* предел блоков оверлея */
    u64 rd_ops, wr_ops;   /* счётчики операций AHCI */
};

/* Полная инициализация: PCI -> AHCI -> MBR -> FAT32 mount.
 * Идемпотентность не требуется: вызывается один раз из kmain. */
err_t storage_init(void);

/* Текущее состояние подсистемы. */
struct blk_status blk_get_status(void);

/* Блочные операции по АБСОЛЮТНОМУ LBA диска (512 Б/сектор).
 * count <= 8. Проходят через амнезию-оверлей. */
err_t blk_read_sectors(u64 lba, u32 count, void *buf);
err_t blk_write_sectors(u64 lba, u32 count, const void *buf);

/* Режим амнезии. on: 1 = включить (запись только в RAM),
 * 0 = выключить (оверлей сбрасывается, изменения теряются). */
void  blk_amnesia(int on);
int   blk_amnesia_active(void);

/* Смонтированный том для файлового слоя (NULL до успешного mount). */
struct fat_vol *blk_fat(void);

/* Восстановление продакшн-оверхлея после kselftest: ovl-тесты
 * перенастраивают глобальный ovl на мок-бэкенд с крошечным лимитом;
 * без восстановления весь блочный IO ядра идёт в мок. Возврат —
 * результат ovl_init (K_OK при успехе). */
err_t blk_ovl_restore(void);

#endif /* FOBOS_FS_BLK_H */
