#include "pci.h"

static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline u32  inl(u16 p) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

static u32 pci_addr(u8 bus, u8 slot, u8 func, u8 off) {
    return (u32)(0x80000000u | ((u32)bus << 16) | ((u32)slot << 11) | ((u32)func << 8) | (off & 0xFC));
}

u32 pci_read_config(struct pci_device *d, u8 off, u8 len) {
    outl(0xCF8, pci_addr(d->bus, d->slot, d->func, off));
    u32 v = inl(0xCFC);
    if (len == 1) return (v >> ((off & 3) * 8)) & 0xFF;
    if (len == 2) return (v >> ((off & 2) * 8)) & 0xFFFF;
    return v;
}

void pci_scan(pci_enum_fn fn) {
    for (u16 bus = 0; bus < 256; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            for (u8 func = 0; func < 8; func++) {
                struct pci_device d = { (u8)bus, slot, func, 0, 0, 0, 0, 0, 0 };
                outl(0xCF8, pci_addr(d.bus, d.slot, d.func, 0));
                u32 id = inl(0xCFC);
                d.vendor = (u16)(id & 0xFFFF);
                d.device = (u16)(id >> 16);
                if (d.vendor == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }
                u32 cls = pci_read_config(&d, 0x08, 4);
                d.revision   = (u8)(cls & 0xFF);
                d.prog_if    = (u8)((cls >> 8) & 0xFF);
                d.subclass   = (u8)((cls >> 16) & 0xFF);
                d.class_code = (u8)((cls >> 24) & 0xFF);
                if (fn) fn(&d);
                if (func == 0) {
                    u32 hdr = pci_read_config(&d, 0x0C, 2);
                    if (!((hdr >> 16) & 0x80)) break;
                }
            }
        }
    }
}