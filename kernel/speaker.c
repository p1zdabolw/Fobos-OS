#include "speaker.h"

#define PIT_FREQ 1193182

static inline void outb_s(u16 p, u8 v) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p));
}

static inline u8 inb_s(u16 p) {
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}

void speaker_init(void) {
    u8 tmp = inb_s(0x61);
    outb_s(0x61, (u8)(tmp & 0xFC));
}

void speaker_tone(u32 hz) {
    if (hz == 0) { speaker_off(); return; }
    if (hz < 20) hz = 20;
    if (hz > 20000) hz = 20000;

    u32 div = PIT_FREQ / hz;
    if (div > 0xFFFF) div = 0xFFFF;
    if (div == 0) div = 1;

    outb_s(0x43, 0xB6);
    outb_s(0x42, (u8)(div & 0xFF));
    outb_s(0x42, (u8)((div >> 8) & 0xFF));

    u8 tmp = inb_s(0x61);
    outb_s(0x61, (u8)(tmp | 3));
}

void speaker_off(void) {
    u8 tmp = inb_s(0x61);
    outb_s(0x61, (u8)(tmp & 0xFC));
}