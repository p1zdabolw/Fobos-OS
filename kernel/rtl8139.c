#include "rtl8139.h"
#include "pmm.h"
#include "../lib/mem.h"
#include "../lib/printf.h"

#define RTL_VENDOR 0x10EC
#define RTL_DEVICE 0x8139

#define RX_BUF_SIZE 16384
#define TX_BUF_SIZE 2048
#define RX_MAX_FRAME 1536

#define REG_IDR0     0x00
#define REG_TSD0     0x10
#define REG_TSAD0    0x20
#define REG_RBSTART  0x30
#define REG_CR       0x37
#define REG_CAPR     0x38
#define REG_IMR      0x3C
#define REG_ISR      0x3E
#define REG_RCR      0x40
#define REG_TCR      0x44
#define REG_CONFIG1  0x52

static u16 g_io = 0;
static u8  g_mac[6];
static u8 *g_rx = (u8*)0;
static u8 *g_tx = (u8*)0;
static phys_t g_rx_phys = 0;
static phys_t g_tx_phys = 0;
static u16 g_rx_off = 0;
static int g_tx_idx = 0;
static int g_ready = 0;

extern void net_rx_frame(const u8 *frame, usize len);

static inline void outb_r(u16 p, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline u8  inb_r(u16 p)  { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw_r(u16 p, u16 v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline u16 inw_r(u16 p)  { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outl_r(u16 p, u32 v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline u32 inl_r(u16 p)  { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

static int find_rtl8139(void) {
    for (u32 bus = 0; bus < 256; bus++) {
        for (u32 slot = 0; slot < 32; slot++) {
            for (u32 func = 0; func < 8; func++) {
                u32 addr = 0x80000000u | (bus << 16) | (slot << 11) | (func << 8);
                outl_r(0xCF8, addr);
                u32 id = inl_r(0xCFC);
                if (id == 0xFFFFFFFFu) { if (func == 0) break; continue; }
                if ((id & 0xFFFFu) == RTL_VENDOR && (id >> 16) == RTL_DEVICE) {
                    outl_r(0xCF8, addr | 0x04);
                    u32 cmd = inl_r(0xCFC);
                    cmd |= 0x05u;
                    outl_r(0xCFC, cmd);
                    outl_r(0xCF8, addr | 0x10);
                    u32 bar0 = inl_r(0xCFC);
                    if ((bar0 & 1u) == 0) continue;
                    g_io = (u16)(bar0 & 0xFFFCu);
                    return 1;
                }
                if (func == 0) break;
            }
        }
    }
    return 0;
}

int rtl8139_init(void) {
    if (!find_rtl8139()) {
        kprintf("RTL8139: not found\n");
        return 0;
    }

    outb_r(g_io + REG_CONFIG1, 0x00);
    outb_r(g_io + REG_CR, 0x10);
    while (inb_r(g_io + REG_CR) & 0x10) { }

    for (int i = 0; i < 6; i++) g_mac[i] = inb_r(g_io + REG_IDR0 + i);

    g_rx_phys = pmm_alloc_frames(RX_BUF_SIZE / 4096);
    g_tx_phys = pmm_alloc_frames(TX_BUF_SIZE / 4096 + 1);
    if (!g_rx_phys || !g_tx_phys) {
        kprintf("RTL8139: buffer alloc failed\n");
        return 0;
    }
    g_rx = (u8*)g_rx_phys;
    g_tx = (u8*)g_tx_phys;
    memset(g_rx, 0, RX_BUF_SIZE);
    memset(g_tx, 0, TX_BUF_SIZE);

    outl_r(g_io + REG_RBSTART, (u32)g_rx_phys);
    outb_r(g_io + REG_CAPR, 0x00);
    outw_r(g_io + REG_CAPR + 2, 0x0000);
    outw_r(g_io + REG_IMR, 0x0000);
    outw_r(g_io + REG_ISR, 0x0005);
    outl_r(g_io + REG_RCR, 0x0000080F);
    outl_r(g_io + REG_TCR, 0x03000700);
    outl_r(g_io + REG_CAPR, 0x00000000);
    outb_r(g_io + REG_CR, 0x0C);

    g_ready = 1;
    kprintf("RTL8139: io=%x mac=%x:%x:%x:%x:%x:%x\n",
            (u32)g_io,
            g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    return 1;
}

int rtl8139_ready(void) { return g_ready; }
const u8 *rtl8139_mac(void) { return g_mac; }

void rtl8139_send(const u8 *frame, usize len) {
    if (!g_ready || len == 0 || len > TX_BUF_SIZE) return;
    memcpy(g_tx, frame, len);
    if (len < 60) {
        memset(g_tx + len, 0, 60 - len);
        len = 60;
    }
    u32 tsad = REG_TSAD0 + (u32)g_tx_idx * 4;
    u32 tsd  = REG_TSD0  + (u32)g_tx_idx * 4;
    while (inl_r(g_io + tsd) & 0x2000u) { }
    outl_r(g_io + tsad, (u32)g_tx_phys);
    outl_r(g_io + tsd, (u32)len);
    g_tx_idx = (g_tx_idx + 1) & 3;
}

void rtl8139_poll(void) {
    if (!g_ready) return;
    for (int guard = 0; guard < 32; guard++) {
        if (inb_r(g_io + REG_CR) & 0x01) break;
        u16 status = *(u16*)(g_rx + g_rx_off);
        u16 length = *(u16*)(g_rx + g_rx_off + 2);
        if ((status & 0x01) == 0) break;
        if (length < 4 || length > RX_MAX_FRAME + 4) break;
        u8 *data = g_rx + g_rx_off + 4;
        usize data_len = (usize)length - 4;
        net_rx_frame(data, data_len);
        g_rx_off = (u16)((g_rx_off + length + 4 + 3) & ~3u);
        if (g_rx_off > RX_BUF_SIZE - 16) {
            g_rx_off = (u16)(g_rx_off - (RX_BUF_SIZE - 16));
        }
        outw_r(g_io + REG_CAPR, (u16)(g_rx_off - 16));
    }
}