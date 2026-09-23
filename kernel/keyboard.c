#include "keyboard.h"
#include "idt.h"

#define KBD_BUF 256

static const char kbd_en[128] = {
    0, KEY_ESC, '1','2','3','4','5','6','7','8','9','0','-','=', KEY_BS,
    KEY_TAB, 'q','w','e','r','t','y','u','i','o','p','[',']', KEY_ENTER,
    KEY_CTRL, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    KEY_LSHIFT, '\\','z','x','c','v','b','n','m',',','.','/', KEY_RSHIFT,
    '*', KEY_ALT, ' ', 0, 0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char kbd_en_shift[128] = {
    0, KEY_ESC, '!','@','#','$','%','^','&','*','(',')','_','+', KEY_BS,
    KEY_TAB, 'Q','W','E','R','T','Y','U','I','O','P','{','}', KEY_ENTER,
    KEY_CTRL, 'A','S','D','F','G','H','J','K','L',':','"','~',
    KEY_LSHIFT, '|','Z','X','C','V','B','N','M','<','>','?', KEY_RSHIFT,
    '*', KEY_ALT, ' ', 0, 0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char kbd_ru[128] = {
    0, KEY_ESC, '1','2','3','4','5','6','7','8','9','0','-','=', KEY_BS,
    KEY_TAB, 0xE9, 0xF6, 0xF3, 0xEA, 0xE5, 0xED, 0xE3, 0xF8, 0xF9, 0xE7, 0xF5, 0xFA, KEY_ENTER,
    KEY_CTRL, 0xF4, 0xFB, 0xE2, 0xE0, 0xEF, 0xF0, 0xEE, 0xEB, 0xE4, 0xE6, 0xFD, '`',
    KEY_LSHIFT, '\\', 0xFF, 0xF7, 0xF1, 0xEC, 0xE8, 0xF2, 0xFC, 0xE1, 0xFE, '.', KEY_RSHIFT,
    '*', KEY_ALT, ' ', 0, 0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char kbd_ru_shift[128] = {
    0, KEY_ESC, '!','@','#','$','%','^','&','*','(',')','_','+', KEY_BS,
    KEY_TAB, 0xC9, 0xD6, 0xD3, 0xCA, 0xC5, 0xCD, 0xC3, 0xD8, 0xD9, 0xC7, 0xD5, 0xDA, KEY_ENTER,
    KEY_CTRL, 0xD4, 0xDB, 0xC2, 0xC0, 0xCF, 0xD0, 0xCE, 0xCB, 0xC4, 0xC6, 0xDD, '~',
    KEY_LSHIFT, '|', 0xDF, 0xD7, 0xD1, 0xCC, 0xC8, 0xD2, 0xDC, 0xC1, 0xDE, ',', KEY_RSHIFT,
    '*', KEY_ALT, ' ', 0, 0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static volatile char g_buf[KBD_BUF];
static volatile int  g_head;
static volatile int  g_tail;
static volatile int  g_shift;
static volatile int  g_alt;
static volatile int  g_ctrl;
static volatile int  g_last_key;
static volatile int  g_layout;
static volatile int  g_layout_latch;

static inline u8 inb(u16 port) {
    u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}

static void check_layout_toggle(void) {
    if (g_shift && g_alt) {
        if (!g_layout_latch) {
            g_layout = (g_layout == LAYOUT_EN) ? LAYOUT_RU : LAYOUT_EN;
            g_layout_latch = 1;
        }
    } else {
        g_layout_latch = 0;
    }
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
        else if (code == KEY_ALT) g_alt = 0;
        else if (code == KEY_CTRL) g_ctrl = 0;
        check_layout_toggle();
        return;
    }

    if (sc == KEY_LSHIFT || sc == KEY_RSHIFT) {
        g_shift = 1;
        check_layout_toggle();
        return;
    }
    if (sc == KEY_ALT) {
        g_alt = 1;
        check_layout_toggle();
        return;
    }
    if (sc == KEY_CTRL) { g_ctrl = 1; return; }

    char c;
    if (g_layout == LAYOUT_RU) c = g_shift ? kbd_ru_shift[sc] : kbd_ru[sc];
    else                       c = g_shift ? kbd_en_shift[sc] : kbd_en[sc];

    g_last_key = (int)sc;
    if (c) {
        int n = (g_head + 1) % KBD_BUF;
        if (n != g_tail) { g_buf[g_head] = c; g_head = n; }
    }
}

void keyboard_init(void) {
    g_head = g_tail = 0;
    g_shift = 0;
    g_alt = 0;
    g_ctrl = 0;
    g_last_key = 0;
    g_layout = LAYOUT_EN;
    g_layout_latch = 0;
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

int keyboard_get_layout(void) { return g_layout; }
void keyboard_set_layout(int layout) {
    if (layout == LAYOUT_EN || layout == LAYOUT_RU) {
        g_layout = layout;
        g_layout_latch = 0;
    }
}
const char *keyboard_layout_name(void) {
    return (g_layout == LAYOUT_RU) ? "RU" : "EN";
}