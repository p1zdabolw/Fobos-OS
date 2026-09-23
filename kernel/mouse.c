#include "mouse.h"
#include "idt.h"
#include "fb.h"

static int g_x, g_y;
static int g_left, g_right;
static u8  g_cycle;
static u8  g_packet[3];

static inline u8 inb(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }

static void mouse_wait_write(void) {
    for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return;
}
static void mouse_wait_read(void) {
    for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return;
}
static void mouse_write(u8 v) { mouse_wait_write(); outb(0x64, 0xD4); mouse_wait_write(); outb(0x60, v); }
static u8   mouse_read(void)   { mouse_wait_read();  return inb(0x60); }

extern int settings_mouse_speed(void);

static int mouse_scale(void) {
    int s = settings_mouse_speed();
    if (s == 0) return 1;
    if (s == 2) return 4;
    return 2;
}

static void mouse_irq(struct registers *r) {
    (void)r;
    u8 status = inb(0x64);
    if (!(status & 0x20)) return;
    u8 data = inb(0x60);
    g_packet[g_cycle++] = data;
    if (g_cycle < 3) return;
    g_cycle = 0;

    u8 flags = g_packet[0];
    if (!(flags & 0x08)) return;
    if (flags & 0xC0) return;

    int dx = (int)g_packet[1];
    int dy = (int)g_packet[2];
    if (flags & 0x10) dx -= 256;
    if (flags & 0x20) dy -= 256;

    int scale = mouse_scale();
    dx = dx * scale / 2;
    dy = dy * scale / 2;

    g_x += dx;
    g_y -= dy;

    int w = (int)fb_get()->width;
    int h = (int)fb_get()->height;
    if (w <= 0) w = 800;
    if (h <= 0) h = 600;

    if (g_x < 0) g_x = 0;
    if (g_y < 0) g_y = 0;
    if (g_x >= w) g_x = w - 1;
    if (g_y >= h) g_y = h - 1;

    g_left  = flags & 1;
    g_right = flags & 2;
}

void mouse_init(void) {
    mouse_wait_write(); outb(0x64, 0xA8);

    mouse_write(0xF5);
    mouse_read();

    mouse_wait_write(); outb(0x64, 0x20);
    mouse_wait_read();
    u8 st = inb(0x60);
    st |=  0x02;
    st &= ~0x20;
    mouse_wait_write(); outb(0x64, 0x60);
    mouse_wait_write(); outb(0x60, st);

    mouse_write(0xF6);
    mouse_read();
    mouse_write(0xF4);
    mouse_read();

    g_x = (int)fb_get()->width / 2;
    g_y = (int)fb_get()->height / 2;
    g_left = g_right = 0;
    g_cycle = 0;

    irq_register(12, mouse_irq);
}

int mouse_x(void) { return g_x; }
int mouse_y(void) { return g_y; }
int mouse_left(void) { return g_left; }
int mouse_right(void) { return g_right; }