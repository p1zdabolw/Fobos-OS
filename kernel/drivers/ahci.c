/*
 * Fobos OS — AHCI-драйвер (реализация).
 *
 * Раскладка MMIO (AHCI 1.3.1):
 *   HBA:  CAP 0x00, GHC 0x04, IS 0x08, PI 0x0C
 *   Port: 0x100 + n*0x80: CLB 0x00, FB 0x08, IS 0x10, IE 0x14,
 *         CMD 0x18, TFD 0x20, SIG 0x24, SSTS 0x28, SCTL 0x2C, CI 0x38
 *
 * Командный слот 0: заголовок 32 Б в командном списке; командная таблица
 * 4 КиБ: CFIS @0x00, ACMD @0x40, PRDT @0x80 (1 запись, до 4 КиБ).
 * Порт держит движок запущенным (FRE+ST) постоянно; операции — запись
 * CFIS/PRDT + CI.bit0, ожидание сброса бита.
 */
#include "ahci.h"

#include "pci.h"
#include "../mm/phys.h"
#include "timer.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../lib/io.h"
#include <stddef.h>

#define A_TAG "ahci"

/* --- HBA --- */
#define AHCI_REG_CAP  0x00u
#define AHCI_REG_GHC  0x04u
#define AHCI_REG_PI   0x0Cu
#define GHC_AE        0x80000000u

/* --- Порт (смещения внутри блока 0x80) --- */
#define P_CLB  0x00u
#define P_FB   0x08u
#define P_IS   0x10u
#define P_CMD  0x18u
#define P_TFD  0x20u
#define P_SIG  0x24u
#define P_SSTS 0x28u
#define P_SCTL 0x2Cu
#define P_CI   0x38u

#define CMD_ST   0x01u
#define CMD_SUD  0x02u
#define CMD_FRE  0x10u
#define CMD_CR   0x8000u
#define CMD_FR   0x4000u

#define SCTL_DET_RESET  0x1u
#define SCTL_DET_NONE   0x0u

#define SSTS_DET_MASK   0x0Fu     /* bits 0..3 = device detection */
#define SSTS_IPM_SHIFT  8u
#define SSTS_IPM_MASK   0x0Fu

#define IS_TFES 0x40000000u

/* --- Команды ATA --- */
#define ATA_CMD_READ_DMA_EXT  0x25u
#define ATA_CMD_WRITE_DMA_EXT 0x35u
#define ATA_CMD_IDENTIFY      0xECu

/* --- FIS host-to-device --- */
#define FIS_TYPE_H2D 0x27u
#define FIS_H2D_CBIT 0x80u
#define FIS_H2D_DEV_LBA 0x40u   /* device: LBA-режим, device 0 (master) */

/* Тайм-ауты (мс). TCG медленный, поэтому с большим запасом. */
#define AHCI_TIMEOUT_MS  8000u
#define AHCI_RESET_MS    8000u

#define AHCI_SLOT      0u
#define AHCI_SECT_MAX  8u   /* 4 КиБ за одну команду (1 PRDT-запись) */

struct ahci_port {
    u64 mmio;            /* база регистров порта */
    u64 cmd_list;        /* физ. адрес командного списка (1 КиБ, выравнен) */
    u64 fis_recv;        /* физ. адрес FIS-приёма (256 Б..1 стр) */
    u64 cmd_table;       /* физ. адрес командной таблицы (4 КиБ) */
    u64 *cmd_list_v;     /* виртуальные (identity) адреса */
    u8  *cmd_table_v;
    u8  *scratch;        /* DMA-буфер 4 КиБ, выравнен */
    int ready;
    u16 identify[256];
    char model[AHCI_MODEL_LEN];
    u64 sectors;
};

static struct {
    u64 mmio;
    u32 port_mask;
    int ready;
    struct ahci_port p[AHCI_MAX_PORTS];
    u64 rd_ops, wr_ops;
} g_a;

static inline u32 hba_read(u32 off)
{
    return *(volatile u32 *)(u64)(g_a.mmio + off);
}

static inline void hba_write(u32 off, u32 v)
{
    *(volatile u32 *)(u64)(g_a.mmio + off) = v;
}

static inline u32 pr_read(const struct ahci_port *p, u32 off)
{
    return *(volatile u32 *)(u64)(p->mmio + off);
}

static inline void pr_write(const struct ahci_port *p, u32 off, u32 v)
{
    *(volatile u32 *)(u64)(p->mmio + off) = v;
}

static u32 now_ms(void)
{
    return (u32)(timer_uptime_ms() & 0xFFFFFFFFu);
}

static int wait_cleared(const struct ahci_port *p, u32 reg, u32 mask, u32 tmo_ms)
{
    u32 t0 = now_ms();
    for (;;) {
        if ((pr_read(p, reg) & mask) == 0u) {
            return 0;
        }
        if (now_ms() - t0 > tmo_ms) {
            return -1;
        }
    }
}

static int wait_set(const struct ahci_port *p, u32 reg, u32 mask, u32 tmo_ms)
{
    u32 t0 = now_ms();
    for (;;) {
        if ((pr_read(p, reg) & mask) != 0u) {
            return 0;
        }
        if (now_ms() - t0 > tmo_ms) {
            return -1;
        }
    }
}

/* Останавливает движок порта (ST/FRE). */
static int port_stop(struct ahci_port *p)
{
    u32 cmd = pr_read(p, P_CMD);
    cmd &= ~(CMD_ST | CMD_FRE);
    pr_write(p, P_CMD, cmd);
    if (wait_cleared(p, P_CMD, CMD_CR | CMD_FR, AHCI_TIMEOUT_MS) != 0) {
        return -1;
    }
    return 0;
}

static int port_start(struct ahci_port *p)
{
    u32 cmd = pr_read(p, P_CMD);
    cmd |= CMD_ST | CMD_FRE | CMD_SUD;
    pr_write(p, P_CMD, cmd);
    return 0;
}

/* Полный сброс порта + запуск движков. */
static int port_reset_and_start(struct ahci_port *p)
{
    if (port_stop(p) != 0) {
        klog(KLOG_WARN, A_TAG, "порт: движок не остановился");
    }

    /* COMRESET: DET=1 на >= 1 мс, затем 0. */
    pr_write(p, P_SCTL, SCTL_DET_RESET);
    timer_sleep_ms(2);
    pr_write(p, P_SCTL, SCTL_DET_NONE);

    if (wait_set(p, P_SSTS, SSTS_DET_MASK, AHCI_RESET_MS) != 0) {
        return -1; /* устройства нет */
    }
    u32 ssts = pr_read(p, P_SSTS);
    u32 det = ssts & SSTS_DET_MASK;
    u32 ipm = (ssts >> SSTS_IPM_SHIFT) & SSTS_IPM_MASK;
    if (det != 3u || ipm == 0u || ipm > 6u) {
        return -1; /* не активное физическое соединение */
    }

    if (port_start(p) != 0) {
        return -1;
    }
    return 0;
}

/* Заполняет командный слот 0 (H2D FIS + PRDT) и исполняет его. */
static int issue(struct ahci_port *p, u8 ata_cmd, u64 lba, u16 count,
                 u64 data_phys, int is_write)
{
    (void)is_write;

    /* Ожидаем свободный слот 0. */
    if (wait_cleared(p, P_CI, 1u << AHCI_SLOT, AHCI_TIMEOUT_MS) != 0) {
        return K_EIO;
    }

    /* Командная таблица: CFIS @0, ACMD @0x40, PRDT @0x80. */
    u8 *cfis = p->cmd_table_v;
    for (u32 i = 0; i < 64u; i++) {
        cfis[i] = 0u;
    }
    /* H2D Register FIS (20 байт) — раскладка как в Linux ata_tf_to_fis:
     *  0: 0x27  1: C=1  2: command  3: features[7:0]
     *  4..6: LBA[23:0]  7: device (LBA-режим)
     *  8..10: LBA[47:24]  11: features[15:8] (не используется)
     * 12..13: sector count  14: ICC  15: control */
    cfis[0] = FIS_TYPE_H2D;
    cfis[1] = FIS_H2D_CBIT;
    cfis[2] = ata_cmd;
    cfis[3] = 0u;                          /* features: не используем */
    cfis[4] = (u8)(lba & 0xFFu);
    cfis[5] = (u8)((lba >> 8) & 0xFFu);
    cfis[6] = (u8)((lba >> 16) & 0xFFu);
    cfis[7] = FIS_H2D_DEV_LBA;             /* device: LBA-режим, master */
    cfis[8] = (u8)((lba >> 24) & 0xFFu);
    cfis[9] = (u8)((lba >> 32) & 0xFFu);
    cfis[10] = (u8)((lba >> 40) & 0xFFu);
    cfis[11] = 0u;
    cfis[12] = (u8)(count & 0xFFu);
    cfis[13] = (u8)((count >> 8) & 0xFFu);

    /* PRDT-запись (16 байт): DW0 = DBA[31:0], DW1 = DBAU[63:32],
     * DW2 = резерв, DW3 = DBC[21:0] (байт-1). Одна запись на 4 КиБ. */
    u32 *prdt = (u32 *)(p->cmd_table_v + 0x80u);
    u32 bytes = (u32)count * 512u;
    prdt[0] = (u32)(data_phys & 0xFFFFFFFFu);
    prdt[1] = (u32)((data_phys >> 32) & 0xFFFFFFFFu);
    prdt[2] = 0u;
    prdt[3] = (bytes - 1u) & 0x3FFFFFu;

    /* Заголовок слота 0 (32 Б) по AHCI 1.3.1 §4.2.2 (так же читает QEMU):
     * DW0: [4:0] CFL=5, [5] ATAPI=0, [6] W (запись), [31:16] PRDTL=1;
     * DW1: [31:16] PRDBC — RO-счётчик, заполнит HBA;
     * DW2/3: CTBA (командная таблица, 128-байтное выравнивание). */
    volatile u32 *hdr = (volatile u32 *)p->cmd_list_v;
    hdr[0] = 0x5u | (is_write ? 0x40u : 0x0u) | (1u << 16);
    /* hdr[1] = PRDBC — RO-счётчик, заполняет HBA; гостем не пишется. */
    hdr[2] = (u32)(p->cmd_table & 0xFFFFFFFFu);
    hdr[3] = (u32)((p->cmd_table >> 32) & 0xFFFFFFFFu);
    hdr[4] = 0u;
    hdr[5] = 0u;
    hdr[6] = 0u;
    hdr[7] = 0u;

    /* Сбросить статус прерываний и стартовать. */
    pr_write(p, P_IS, 0xFFFFFFFFu);
    pr_write(p, P_CI, 1u << AHCI_SLOT);

    /* Ожидание завершения: CI-бит сброшен ИЛИ ошибка. */
    u32 t0 = now_ms();
    for (;;) {
        u32 ci = pr_read(p, P_CI);
        u32 is = pr_read(p, P_IS);
        if ((is & IS_TFES) != 0u) {
            u32 tfd = pr_read(p, P_TFD);
            klog(KLOG_ERROR, A_TAG, "команда 0x%02x: TFES, TFD=0x%04x",
                 ata_cmd, tfd);
            pr_write(p, P_IS, IS_TFES);
            return K_EIO;
        }
        if ((ci & (1u << AHCI_SLOT)) == 0u) {
            break;
        }
        if (now_ms() - t0 > AHCI_TIMEOUT_MS) {
            klog(KLOG_ERROR, A_TAG, "команда 0x%02x: тайм-аут", ata_cmd);
            (void)port_stop(p);
            (void)port_start(p);
            return K_EIO;
        }
    }
    return K_OK;
}

err_t ahci_init(u64 mmio_phys)
{
    if (g_a.ready != 0) {
        return K_ESTATE;
    }
    if (mmio_phys == 0u || (mmio_phys & 0xFFFu) != 0u) {
        return K_EINVAL;
    }
    g_a.mmio = mmio_phys;

    /* Включаем AHCI-режим HBA. */
    u32 ghc = hba_read(AHCI_REG_GHC);
    hba_write(AHCI_REG_GHC, ghc | GHC_AE);

    u32 cap = hba_read(AHCI_REG_CAP);
    u32 pi = hba_read(AHCI_REG_PI);
    klog(KLOG_INFO, A_TAG,
         "HBA: CAP=0x%08x PI=0x%02x ports=%u",
         cap, pi,
         (unsigned int)((cap & 0x1Fu) + 1u));

    u32 nports = (cap & 0x1Fu) + 1u;
    if (nports > AHCI_MAX_PORTS) {
        nports = AHCI_MAX_PORTS;
    }

    for (u32 i = 0; i < nports; i++) {
        if ((pi & (1u << i)) == 0u) {
            continue;
        }
        struct ahci_port *p = &g_a.p[i];
        p->mmio = mmio_phys + 0x100u + i * 0x80u;

        /* Структуры порта: физкадры (время жизни = ядру). */
        u64 pa;
        if (phys_alloc_frame(&pa) != K_OK) {
            return K_ENOMEM;
        }
        p->cmd_list = pa;
        if (phys_alloc_frame(&pa) != K_OK) {
            return K_ENOMEM;
        }
        p->fis_recv = pa;
        if (phys_alloc_frame(&pa) != K_OK) {
            return K_ENOMEM;
        }
        p->cmd_table = pa;

        p->cmd_list_v = (u64 *)(u64)p->cmd_list;
        p->cmd_table_v = (u8 *)(u64)p->cmd_table;
        if (phys_alloc_frame(&pa) != K_OK) {
            return K_ENOMEM;
        }
        p->scratch = (u8 *)(u64)pa;

        for (u32 z = 0; z < 4096u; z++) {
            p->cmd_table_v[z] = 0u;
        }

        pr_write(p, P_CLB, (u32)(p->cmd_list & 0xFFFFFFFFu));
        pr_write(p, P_CLB + 4u, (u32)((p->cmd_list >> 32) & 0xFFFFFFFFu));
        pr_write(p, P_FB, (u32)(p->fis_recv & 0xFFFFFFFFu));
        pr_write(p, P_FB + 4u, (u32)((p->fis_recv >> 32) & 0xFFFFFFFFu));

        if (port_reset_and_start(p) != 0) {
            p->ready = 0;
            continue;
        }

        u32 sig = pr_read(p, P_SIG);
        if (sig != 0x00000101u) {          /* SATA */
            p->ready = 0;
            klog(KLOG_WARN, A_TAG, "порт %u: сигнатура 0x%08x (не SATA)",
                 i, sig);
            continue;
        }

        p->ready = 1;
        g_a.port_mask |= (1u << i);
        klog(KLOG_INFO, A_TAG, "порт %u: SATA-устройство готово", i);
    }

    g_a.ready = 1;
    if (g_a.port_mask == 0u) {
        klog(KLOG_WARN, A_TAG, "готовых SATA-портов нет");
        return K_ENODEV;
    }
    return K_OK;
}

u32 ahci_port_mask(void)
{
    return g_a.ready ? g_a.port_mask : 0u;
}

int ahci_port_ready(u8 port)
{
    if (g_a.ready == 0 || port >= AHCI_MAX_PORTS) {
        return 0;
    }
    return g_a.p[port].ready;
}

err_t ahci_identify(u8 port, char out_model[AHCI_MODEL_LEN], u64 *out_sectors)
{
    if (g_a.ready == 0 || port >= AHCI_MAX_PORTS || g_a.p[port].ready == 0) {
        return K_ENODEV;
    }
    struct ahci_port *p = &g_a.p[port];

    err_t rc = issue(p, ATA_CMD_IDENTIFY, 0u, 1u, (u64)(u64)p->scratch, 0);
    if (rc != K_OK) {
        return rc;
    }
    for (u32 i = 0; i < 512u; i++) {
        ((u8 *)p->identify)[i] = p->scratch[i];
    }

    /* Модель: слова 27..46, big-endian в каждом слове, пробелы по краям. */
    u32 m = 0;
    for (u32 w = 0; w < 20u && m + 1u < AHCI_MODEL_LEN; w++) {
        u16 v = p->identify[27u + w];
        char c1 = (char)(v >> 8);
        char c2 = (char)(v & 0xFFu);
        p->model[m++] = c1;
        if (m + 1u < AHCI_MODEL_LEN) {
            p->model[m++] = c2;
        }
    }
    p->model[m] = '\0';
    /* Срезаем ведущие/хвостовые пробелы. */
    char *s = p->model;
    while (*s == ' ') {
        s++;
    }
    u32 len = (u32)strlen(s);
    while (len > 0u && s[len - 1u] == ' ') {
        s[--len] = '\0';
    }
    if (out_model != NULL) {
        for (u32 i = 0; i <= len && i + 1u < AHCI_MODEL_LEN; i++) {
            out_model[i] = s[i];
        }
        out_model[AHCI_MODEL_LEN - 1u] = '\0';
    }

    /* Число секторов LBA48: слова 100..103. */
    p->sectors = (u64)p->identify[100] |
                 ((u64)p->identify[101] << 16) |
                 ((u64)p->identify[102] << 32) |
                 ((u64)p->identify[103] << 48);
    if (out_sectors != NULL) {
        *out_sectors = p->sectors;
    }
    return K_OK;
}

err_t ahci_read(u8 port, u64 lba, u32 count, void *buf)
{
    if (g_a.ready == 0 || port >= AHCI_MAX_PORTS || g_a.p[port].ready == 0) {
        return K_ENODEV;
    }
    if (count == 0u || count > AHCI_SECT_MAX || buf == NULL) {
        return K_EINVAL;
    }
    struct ahci_port *p = &g_a.p[port];
    err_t rc = issue(p, ATA_CMD_READ_DMA_EXT, lba, (u16)count,
                     (u64)(u64)p->scratch, 0);
    if (rc != K_OK) {
        return rc;
    }
    u8 *dst = (u8 *)buf;
    for (u32 i = 0; i < count * 512u; i++) {
        dst[i] = p->scratch[i];
    }
    g_a.rd_ops++;
    return K_OK;
}

err_t ahci_write(u8 port, u64 lba, u32 count, const void *buf)
{
    if (g_a.ready == 0 || port >= AHCI_MAX_PORTS || g_a.p[port].ready == 0) {
        return K_ENODEV;
    }
    if (count == 0u || count > AHCI_SECT_MAX || buf == NULL) {
        return K_EINVAL;
    }
    struct ahci_port *p = &g_a.p[port];
    const u8 *src = (const u8 *)buf;
    for (u32 i = 0; i < count * 512u; i++) {
        p->scratch[i] = src[i];
    }
    err_t rc = issue(p, ATA_CMD_WRITE_DMA_EXT, lba, (u16)count,
                     (u64)(u64)p->scratch, 1);
    if (rc != K_OK) {
        return rc;
    }
    g_a.wr_ops++;
    return K_OK;
}

u64 ahci_read_ops(void)
{
    return g_a.rd_ops;
}

u64 ahci_write_ops(void)
{
    return g_a.wr_ops;
}
