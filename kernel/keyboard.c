#include "keyboard.h"
#include "idt.h"

#define KBD_BUF 256

static const char kbd_map[128] = {
    0, KEY_ESC, '1','2','3','4','5','6','7','8','9','0','-','=', KEY_BS,
    KEY_TAB, 'q','w','e','r','t','y','u','i','o','p','[',']', KEY_ENTER,
    KEY_CTRL, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    KEY_LSHIFT, '\\','z','x','c','v','b','n','m',',','.','/', KEY_RSHIFT,
    '*', KEY_ALT, ' ', 0, 0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char kbd_shift[128] = {
    0, KEY_ESC, '!','@','#','$','%','^','&','*','(',')','_','+', KEY_BS,
    KEY_TAB, 'Q','W','E','R','T','Y','U','I','O','P','{','}', KEY_ENTER,
    KEY_CTRL, 'A','S','D','F','G','H','J','K','L',':','"','~',
    KEY_LSHIFT, '|','Z','X','C','V','B','N','M','<','>','?', KEY_RSHIFT,
    '*', KEY_ALT, ' ', 0, 0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static volatile char g_buf[KBD_BUF];
static volatile int  g_head;
static volatile int  g_tail;
static volatile int  g_shift;
static volatile int  g_last_key;

static inline u8 inb(u16 port) {
    u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}

static void keyboard_irq(struct registers *r) {
    (void)r;
    u8 status = inb(0x64);
    if (!(status & 0x01)) return;
    if (status & 0x20) return;
    u8 sc = inb(0x60);
    if (sc == 0xE0) return;
    if (sc & 0x80) {
        u8 code = sc & 0x7F;
        if (code == KEY_LSHIFT || code == KEY_RSHIFT) g_shift = 0;
        return;
    }
    if (sc == KEY_LSHIFT || sc == KEY_RSHIFT) { g_shift = 1; return; }
    char c = g_shift ? kbd_shift[sc] : kbd_map[sc];
    g_last_key = (int)sc;
    if (c) {
        int n = (g_head + 1) % KBD_BUF;
        if (n != g_tail) { g_buf[g_head] = c; g_head = n; }
    }
}

void keyboard_init(void) {
    g_head = g_tail = 0;
    g_shift = 0;
    g_last_key = 0;
    irq_register(1, keyboard_irq);
}

int keyboard_has_char(void) { return g_head != g_tail; }
char keyboard_getchar(void) {
    if (g_head == g_tail) return 0;
    char c = g_buf[g_tail];
    g_tail = (g_tail + 1) % KBD_BUF;
    return c;
}
int keyboard_has_key(void) { return g_last_key != 0; }
int keyboard_get_key(void) {
    int k = g_last_key;
    g_last_key = 0;
    return k;
}
int keyboard_shift_down(void) { return g_shift; }