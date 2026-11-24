/*
 * Fobos OS — PCI-энумерация (порт 0xCF8/0xCFC, bus 0).
 *
 * Слой: drivers. Даёт список устройств и поиск по классу. MMIO-бары
 * возвращаются как физические адреса (< 4 ГиБ) — ядро работает с ними
 * напрямую через identity-отображение загрузчика (тот же путь, что и
 * framebuffer). Виртуальный VMM — FND-2.
 *
 * Ограничение этапа: проверяется только шина 0 (q35: все нужные устройства
 * сидят на ней; мультифункциональные устройства перебираются по func 0..7).
 */
#ifndef FOBOS_DRV_PCI_H
#define FOBOS_DRV_PCI_H

#include <fobos/errors.h>
#include <fobos/types.h>

#define PCI_MAX_DEVICES 64u

struct pci_dev {
    u8  bus, dev, func;
    u16 vendor, device;
    u8  class_code, subclass, prog_if;
    u32 bar[6];              /* физические адреса MMIO/IO (bit0 = IO) */
};

/* Побайтовый доступ к конфигурационному пространству (32-битные слова). */
u32 pci_read_config32(u8 bus, u8 dev, u8 func, u8 reg_offset);
void pci_write_config32(u8 bus, u8 dev, u8 func, u8 reg_offset, u32 value);

/* Энумерация шины 0. Повторный вызов допустим (перечитывает). */
err_t pci_init(void);

u32 pci_dev_count(void);
/* Указатель живёт до следующего pci_init(). NULL вне диапазона. */
const struct pci_dev *pci_dev(u32 index);

/* Первое устройство с точным class/subclass (prog_if любой). NULL если нет. */
const struct pci_dev *pci_find_class(u8 class_code, u8 subclass);

/* Включает Memory Space + Bus Master в командном регистре (для MMIO-DMA).
 * Читает текущее значение, добавляет биты, пишет обратно. */
err_t pci_enable_mem_busmaster(const struct pci_dev *d);

/* Самопроверка чистой логики BDF->CFGA (ядро + host). */
u32 pci_config_address(u8 bus, u8 dev, u8 func, u8 reg_offset);

#endif /* FOBOS_DRV_PCI_H */
