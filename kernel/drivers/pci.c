/*
 * Fobos OS — PCI-энумерация (реализация).
 *
 * Конфигурационный механизм #1: адрес 0xCF8 (CFGA), данные 0xCFC.
 * CFGA = [enable:1][reserved:7][bus:8][dev:5][func:3][reg:6][offset/4:2].
 * Чтение несуществующего устройства возвращает 0xFFFFFFFF в vendor id.
 *
 * Энумерация РЕКУРСИВНАЯ: мосты PCI-to-PCI (класс 06/04, заголовок типа 1)
 * раскрываются по регистру secondary bus (0x19). Без этого на реальных
 * платах устройства за чипсетными мостами (частый случай для SATA/USB/LAN)
 * не находятся: в QEMU вся периферия живёт на шине 0, на железе — нет.
 */
#include "pci.h"

#include "../lib/log.h"
#include "../lib/io.h"
#include <stddef.h>

#define PCI_TAG "pci"

#define PCI_PORT_ADDR 0xCF8u
#define PCI_PORT_DATA 0xCFCu

#define PCI_CFG_ENABLE 0x80000000u

#define PCI_CFG_VENDOR 0x00u
#define PCI_CFG_COMMAND 0x04u
#define PCI_CFG_CLASS 0x08u
#define PCI_CFG_BAR0 0x10u
#define PCI_CFG_HEADER_TYPE 0x0Cu
#define PCI_CFG_SEC_BUS 0x18u

#define PCI_CMD_MEM_SPACE 0x0002u
#define PCI_CMD_BUS_MASTER 0x0004u

#define PCI_INVALID_VENDOR 0xFFFFu

/* Классы, влияющие на обход. */
#define PCI_CLASS_BRIDGE 0x06u
#define PCI_SUBCLASS_P2P 0x04u
#define PCI_HEADER_TYPE_1 0x01u
#define PCI_HEADER_MASK 0x7Fu
#define PCI_MULTIFUNC_BIT 0x80u

/* Лимиты обхода: шины — по диапазону CFGA (8 бит), глубина — защита от
 * цикла мостов (прошивка с некорректной таблицей маршрутизации). */
#define PCI_MAX_BUSES 256u
#define PCI_MAX_DEPTH 8u

static struct pci_dev g_devs[PCI_MAX_DEVICES];
static u32 g_dev_count;
static int g_ready;
static u8 g_bus_seen[(PCI_MAX_BUSES + 7u) / 8u];

static int bus_seen_test_and_set(u8 bus)
{
    u8 mask = (u8)(1u << (bus & 7u));
    if ((g_bus_seen[bus >> 3] & mask) != 0u) {
        return 1;
    }
    g_bus_seen[bus >> 3] |= mask;
    return 0;
}

static void scan_bus(u8 bus, u32 depth);

u32 pci_config_address(u8 bus, u8 dev, u8 func, u8 reg_offset)
{
    /* reg_offset — байтовое смещение, кратное 4. */
    return PCI_CFG_ENABLE |
           ((u32)bus << 16) |
           ((u32)dev << 11) |
           ((u32)func << 8) |
           ((u32)reg_offset & 0xFCu);
}

u32 pci_read_config32(u8 bus, u8 dev, u8 func, u8 reg_offset)
{
    io_out32(PCI_PORT_ADDR, pci_config_address(bus, dev, func, reg_offset));
    return io_in32(PCI_PORT_DATA);
}

void pci_write_config32(u8 bus, u8 dev, u8 func, u8 reg_offset, u32 value)
{
    io_out32(PCI_PORT_ADDR, pci_config_address(bus, dev, func, reg_offset));
    io_out32(PCI_PORT_DATA, value);
}

static void scan_device(u8 bus, u8 dev, u32 depth)
{
    u8 func_max = 1u;
    for (u8 f = 0; f < func_max; f++) {
        u32 vend = pci_read_config32(bus, dev, f, PCI_CFG_VENDOR);
        if ((vend & 0xFFFFu) == PCI_INVALID_VENDOR) {
            continue;
        }

        /* Мультифункциональность определяется битом 7 заголовка func 0. */
        u32 hdr = 0;
        if (f == 0u) {
            hdr = pci_read_config32(bus, dev, 0u, PCI_CFG_HEADER_TYPE);
            if ((hdr >> 16) & PCI_MULTIFUNC_BIT) {
                func_max = 8u;
            }
        }

        if (g_dev_count >= PCI_MAX_DEVICES) {
            klog(KLOG_WARN, PCI_TAG, "список устройств переполнен (>= %u)",
                 (unsigned int)PCI_MAX_DEVICES);
            return;
        }

        struct pci_dev *pd = &g_devs[g_dev_count];
        pd->bus = bus;
        pd->dev = dev;
        pd->func = f;
        pd->vendor = (u16)(vend & 0xFFFFu);
        pd->device = (u16)(vend >> 16);

        /* Классовый регистр (offset 0x08): [7:0] revision,
         * [15:8] prog_if, [23:16] subclass, [31:24] base class. */
        u32 cls = pci_read_config32(bus, dev, f, PCI_CFG_CLASS);
        pd->prog_if = (u8)((cls >> 8) & 0xFFu);
        pd->subclass = (u8)((cls >> 16) & 0xFFu);
        pd->class_code = (u8)((cls >> 24) & 0xFFu);

        for (u8 b = 0; b < 6u; b++) {
            pd->bar[b] = pci_read_config32(bus, dev, f,
                                           PCI_CFG_BAR0 + b * 4u);
        }

        klog(KLOG_INFO, PCI_TAG,
             "устройство %02x:%02x.%u vendor=%04x device=%04x class=%02x%02x/%02x",
             (unsigned int)pd->bus, (unsigned int)pd->dev,
             (unsigned int)pd->func, pd->vendor, pd->device,
             pd->class_code, pd->subclass, pd->prog_if);

        g_dev_count++;

        /* Мост PCI-to-PCI (тип заголовка 1): уходим на вторичную шину.
         * Secondary bus уже прошит прошивкой на POST; ноль/повтор —
         * отсекается bitmap'ом посещённых шин. */
        if (pd->class_code == PCI_CLASS_BRIDGE &&
            pd->subclass == PCI_SUBCLASS_P2P &&
            ((hdr >> 16) & PCI_HEADER_MASK) == PCI_HEADER_TYPE_1 &&
            depth + 1u < PCI_MAX_DEPTH) {
            u32 sec = pci_read_config32(bus, dev, f, PCI_CFG_SEC_BUS);
            u8 sb = (u8)((sec >> 8) & 0xFFu);
            if (bus_seen_test_and_set(sb) == 0) {
                scan_bus(sb, depth + 1u);
            }
        }
    }
}

static void scan_bus(u8 bus, u32 depth)
{
    for (u8 d = 0; d < 32u; d++) {
        scan_device(bus, d, depth);
    }
}

err_t pci_init(void)
{
    g_dev_count = 0;
    for (u32 i = 0; i < sizeof(g_bus_seen); i++) {
        g_bus_seen[i] = 0u;
    }

    (void)bus_seen_test_and_set(0u);
    scan_bus(0u, 0u);

    g_ready = 1;
    klog(KLOG_INFO, PCI_TAG, "PCI: %u устройств(а) на пройденных шинах",
         (unsigned int)g_dev_count);
    return K_OK;
}

u32 pci_dev_count(void)
{
    return g_ready ? g_dev_count : 0u;
}

const struct pci_dev *pci_dev(u32 index)
{
    if (g_ready == 0 || index >= g_dev_count) {
        return NULL;
    }
    return &g_devs[index];
}

const struct pci_dev *pci_find_class(u8 class_code, u8 subclass)
{
    for (u32 i = 0; i < g_dev_count; i++) {
        if (g_devs[i].class_code == class_code &&
            g_devs[i].subclass == subclass) {
            return &g_devs[i];
        }
    }
    return NULL;
}

err_t pci_enable_mem_busmaster(const struct pci_dev *d)
{
    if (d == NULL) {
        return K_EINVAL;
    }
    u32 cmd = pci_read_config32(d->bus, d->dev, d->func, PCI_CFG_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    pci_write_config32(d->bus, d->dev, d->func, PCI_CFG_COMMAND, cmd);
    return K_OK;
}
