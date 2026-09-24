#include "ata.h"
#include "pci.h"
#include "../lib/printf.h"
#include "../lib/string.h"

#define ATA_PRIMARY_IO    0x1F0
#define ATA_PRIMARY_CTRL  0x3F6

#define ATA_REG_DATA      0
#define ATA_REG_ERR       1
#define ATA_REG_FEAT      1
#define ATA_REG_SECCOUNT  2
#define ATA_REG_LBA_LO    3
#define ATA_REG_LBA_MID   4
#define ATA_REG_LBA_HI    5
#define ATA_REG_DRIVE     6
#define ATA_REG_STATUS    7
#define ATA_REG_COMMAND   7

#define ATA_ST_BSY        0x80
#define ATA_ST_DRDY       0x40
#define ATA_ST_DF         0x20
#define ATA_ST_DRQ        0x08
#define ATA_ST_ERR        0x01

#define ATA_CMD_READ      0x20
#define ATA_CMD_WRITE     0x30
#define ATA_CMD_IDENTIFY  0xEC

static u16 g_io   = ATA_PRIMARY_IO;
static u16 g_ctrl = ATA_PRIMARY_CTRL;
static u32 g_sector_count = 0;
static int g_ready = 0;

static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline u8  inb(u16 p)  { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline u16 inw(u16 p)  { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }

static void ata_400ns(void) {
    for (int i = 0; i < 4; i++) (void)inb(g_ctrl);
}

static int ata_wait_ready(void) {
    for (int i = 0; i < 100000; i++) {
        u8 s = inb(g_io + ATA_REG_STATUS);
        if (!(s & ATA_ST_BSY)) return 0;
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        u8 s = inb(g_io + ATA_REG_STATUS);
        if (s & ATA_ST_BSY) continue;
        if (s & (ATA_ST_ERR | ATA_ST_DF)) return -1;
        if (s & ATA_ST_DRQ) return 0;
    }
    return -1;
}

int ata_init(void) {
    outb(g_ctrl, 0x00);
    outb(g_io + ATA_REG_DRIVE, 0xA0);
    ata_400ns();

    outb(g_io + ATA_REG_SECCOUNT, 0);
    outb(g_io + ATA_REG_LBA_LO,   0);
    outb(g_io + ATA_REG_LBA_MID,  0);
    outb(g_io + ATA_REG_LBA_HI,   0);
    outb(g_io + ATA_REG_COMMAND,  ATA_CMD_IDENTIFY);
    ata_400ns();

    u8 status = inb(g_io + ATA_REG_STATUS);
    if (status == 0) {
        kprintf("ATA: no drive on primary master\n");
        return -1;
    }
    if (ata_wait_ready() != 0) {
        kprintf("ATA: drive not ready\n");
        return -1;
    }
    if (ata_wait_drq() != 0) {
        kprintf("ATA: identify failed\n");
        return -1;
    }

    u16 ident[256];
    for (int i = 0; i < 256; i++) ident[i] = inw(g_io + ATA_REG_DATA);

    if ((ident[49] & (1 << 9)) == 0) {
        kprintf("ATA: LBA not supported\n");
        return -1;
    }

    g_sector_count = ((u32)ident[61] << 16) | (u32)ident[60];
    if (g_sector_count == 0) {
        kprintf("ATA: zero sectors\n");
        return -1;
    }

    char model[41];
    for (int i = 0; i < 20; i++) {
        model[i*2]     = (char)(ident[27 + i] >> 8);
        model[i*2 + 1] = (char)(ident[27 + i] & 0xFF);
    }
    model[40] = 0;

    g_ready = 1;
    kprintf("ATA: %s %u sectors (%u MB)\n",
            model, g_sector_count, g_sector_count / 2048);
    return 0;
}

int ata_available(void) { return g_ready; }
u32 ata_sector_count(void) { return g_sector_count; }

int ata_read_sector(u32 lba, void *buf) {
    if (!g_ready) return -1;
    if (ata_wait_ready() != 0) return -1;

    outb(g_io + ATA_REG_DRIVE, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_400ns();
    outb(g_io + ATA_REG_SECCOUNT, 1);
    outb(g_io + ATA_REG_LBA_LO, (u8)(lba & 0xFF));
    outb(g_io + ATA_REG_LBA_MID, (u8)((lba >> 8) & 0xFF));
    outb(g_io + ATA_REG_LBA_HI, (u8)((lba >> 16) & 0xFF));
    outb(g_io + ATA_REG_COMMAND, ATA_CMD_READ);
    ata_400ns();

    if (ata_wait_drq() != 0) return -1;
    u16 *p = (u16*)buf;
    for (int i = 0; i < 256; i++) p[i] = inw(g_io + ATA_REG_DATA);
    return 0;
}

int ata_write_sector(u32 lba, const void *buf) {
    if (!g_ready) return -1;
    if (ata_wait_ready() != 0) return -1;

    outb(g_io + ATA_REG_DRIVE, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_400ns();
    outb(g_io + ATA_REG_SECCOUNT, 1);
    outb(g_io + ATA_REG_LBA_LO, (u8)(lba & 0xFF));
    outb(g_io + ATA_REG_LBA_MID, (u8)((lba >> 8) & 0xFF));
    outb(g_io + ATA_REG_LBA_HI, (u8)((lba >> 16) & 0xFF));
    outb(g_io + ATA_REG_COMMAND, ATA_CMD_WRITE);
    ata_400ns();

    if (ata_wait_drq() != 0) return -1;
    const u16 *p = (const u16*)buf;
    for (int i = 0; i < 256; i++) outw(g_io + ATA_REG_DATA, p[i]);
    ata_400ns();

    if (ata_wait_ready() != 0) return -1;
    return 0;
}