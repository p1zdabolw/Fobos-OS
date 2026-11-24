/*
 * Fobos OS — AHCI-драйвер (HBA 1.3.1, синхронный поллинг, слот 0).
 *
 * Слой: drivers. Работает поверх MMIO-бара от PCI (физический адрес
 * < 4 ГиБ, identity-отображение). Прерывания не используются: каждая
 * операция — программный поллинг CI/TFD с тайм-аутом по PIT.
 *
 * Передача данных: DMA, порциями до 4 КиБ через внутренний выровненный
 * буфер-скретч (гарантированная физическая непрерывность и границы
 * PRDT). LBA48 (READ/WRITE DMA EXT). IDENTIFY — та же команда-таблица.
 *
 * Структуры на порт (командный список, FIS-приём, командная таблица)
 * выделяются физкадрами через phys_alloc — живы всё время работы ядра.
 */
#ifndef FOBOS_DRV_AHCI_H
#define FOBOS_DRV_AHCI_H

#include <fobos/errors.h>
#include <fobos/types.h>

#define AHCI_MAX_PORTS 6u
#define AHCI_MODEL_LEN 41u

/* Инициализация HBA по MMIO-базе. Не требует прерываний. */
err_t ahci_init(u64 mmio_phys);

/* Маска реализованных портов и готовность диска на порту. */
u32 ahci_port_mask(void);
int ahci_port_ready(u8 port);

/* IDENTIFY: модель (NUL-terminated, пробелы срезаны) и число секторов LBA48. */
err_t ahci_identify(u8 port, char out_model[AHCI_MODEL_LEN], u64 *out_sectors);

/* Чтение/запись count <= 8 секторов (512 Б) с LBA. Поллинг, тайм-ауты. */
err_t ahci_read(u8 port, u64 lba, u32 count, void *buf);
err_t ahci_write(u8 port, u64 lba, u32 count, const void *buf);

/* Счётчики успешных операций (диагностика). */
u64 ahci_read_ops(void);
u64 ahci_write_ops(void);

#endif /* FOBOS_DRV_AHCI_H */
