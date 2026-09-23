#include "timer.h"
#include "idt.h"

#define PIT_HZ 1193182

static volatile u64 g_ticks;
static u32 g_freq;

static inline void outb(u16 port, u8 val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static void timer_irq(struct registers *r) {
    (void)r;
    g_ticks++;
}

void timer_init(u32 hz) {
    g_freq = hz;
    irq_register(0, timer_irq);
    u32 div = PIT_HZ / hz;
    outb(0x43, 0x36);
    outb(0x40, (u8)(div & 0xFF));
    outb(0x40, (u8)((div >> 8) & 0xFF));
}

u64 timer_ticks(void) { return g_ticks; }
u64 timer_uptime_seconds(void) { return g_ticks / g_freq; }