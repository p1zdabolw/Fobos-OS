/*
 * Fobos OS — реализация драйвера PS/2 клавиатуры.
 *
 * Ключевые решения:
 *  - трансляция в коды клавиш — чистые функции (kbd_selftest гоняет их
 *    и в ядре, и на host);
 *  - ACK (0xFA)/Resend (0xFE) на команду LED (0xED) перехватываются в ISR
 *    через флаг cmd_pending — иначе ACK попал бы в очередь как «нажатие»;
 *  - LED-обновление выполняется в main-контексте при cli (короткий
 *    критический участок, ограниченное ожидание ACK);
 *  - очередь 64 события, переполнение = drop + счётчик (IRQ не блокируем);
 *  - автоповтор программный: удерживаемая клавиша синтезируется в kbd_poll
 *    (QEMU не эмулирует тип-матику, повтор нужен для backspace/строк).
 *
 * Ограничение FND-2a (задокументировано): клавиатурный numpad трактуются
 * как цифры/операторы (numlock-режим); стрелки — через расширенные коды E0.
 */
#include "kbd.h"
#include "ps2.h"
#include "../arch/pic.h"
#include "../arch/irq.h"
#include "../lib/log.h"
#include "../lib/spinlock.h"
#include <stddef.h>

#ifndef FOBOS_HOST_TEST
#include "timer.h"
#include "../lib/io.h"
#endif

#define KBD_QUEUE_CAP 64u

/* Сканкод-набор 1. */
#define SC_EXT_PREFIX 0xE0u
#define SC_BREAK_MASK 0x80u

struct kbd_state {
    struct spinlock lock;
    struct kbd_event queue[KBD_QUEUE_CAP];
    u32 head, tail, count;

    u8  mods;            /* KBD_FL_* текущего состояния */
    u8  ext_next;        /* следующий байт — после 0xE0 */
    u8  led_dirty;       /* нужен перенос LED в контроллер */
    u8  cmd_pending;     /* в полёте команда 0xED */
    u32 led_value;       /* биты LED для 0xED */

    /* программный автоповтор (защищается lock вместе с очередью) */
    struct kbd_event repeat_ev;
    u8  repeat_active;
    u64 repeat_next_ms;

    struct kbd_stats stats;
};

static struct kbd_state g_kbd = {
    .lock = SPINLOCK_INIT("kbd"),
};

static const char *const KBD_TAG = "drv/kbd";

/* ------------------------------------------------------------------ */
/* Раскладка US: make-код -> печатный символ (набор 1)                 */
/* ------------------------------------------------------------------ */

static const char US_UNSHIFTED[0x80] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b',
    [0x0F] = '\t', [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o',
    [0x19] = 'p', [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`', [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' ',
    /* numpad (numlock-режим) */
    [0x47] = '7', [0x48] = '8', [0x49] = '9', [0x4A] = '-',
    [0x4B] = '4', [0x4C] = '5', [0x4D] = '6', [0x4E] = '+',
    [0x4F] = '1', [0x50] = '2', [0x51] = '3', [0x52] = '0', [0x53] = '.',
};

static const char US_SHIFTED[0x80] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = ':',
    [0x28] = '"', [0x29] = '~', [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>', [0x35] = '?',
};

/* make-код -> KEY_* (не-печатные служебные). 0 = печатная (см. таблицы). */
static u8 key_of_make(u8 make)
{
    switch (make) {
    case 0x01: return KEY_ESC;
    case 0x0E: return KEY_BSPACE;
    case 0x0F: return KEY_TAB;
    case 0x1C: return KEY_ENTER;
    case 0x39: return KEY_SPACE;
    case 0x3A: return KEY_CAPS;
    case 0x45: return KEY_NUM;
    case 0x46: return KEY_SCROLL;
    default:   return KEY_NONE;
    }
}

/* расширенные (0xE0 xx) make-коды -> KEY_*; KEY_MAX = неизвестный. */
static u8 key_of_make_ext(u8 make)
{
    switch (make) {
    case 0x47: return KEY_HOME;
    case 0x48: return KEY_UP;
    case 0x49: return KEY_PGUP;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x4F: return KEY_END;
    case 0x50: return KEY_DOWN;
    case 0x51: return KEY_PGDN;
    case 0x52: return KEY_INS;
    case 0x53: return KEY_DEL;
    case 0x1D: return KEY_NONE; /* RCtrl — флаг */
    case 0x38: return KEY_NONE; /* RAlt — флаг */
    case 0x5B: return KEY_NONE; /* LWin */
    case 0x5C: return KEY_NONE; /* RWin */
    default:   return KEY_MAX;
    }
}

static int is_letter_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/*
 * Чистая трансляция make-кода в событие нажатия.
 * 1 — событие сформировано; 0 — модификатор/тумблер (события нет);
 * -1 — неизвестный сканкод.
 */
static int translate_make(u8 make, int extended, u8 mods,
                          struct kbd_event *out)
{
    if (extended != 0) {
        u8 k = key_of_make_ext(make);
        if (k == KEY_MAX) {
            return -1;
        }
        if (k == KEY_NONE) {
            return 0;
        }
        out->code = k;
        out->flags = mods;
        out->ch = 0;
        return 1;
    }

    /* Модификаторы и тумблеры событий не порождают. */
    if (make == 0x2Au || make == 0x36u || make == 0x1Du || make == 0x38u) {
        return 0;
    }

    u8 k = key_of_make(make);
    if (k != KEY_NONE) {
        if (k >= KEY_CAPS) {
            return 0;
        }
        out->code = k;
        out->flags = mods;
        /* Пробел и Enter несут значимый символ из таблицы раскладки. */
        out->ch = (k == KEY_SPACE || k == KEY_ENTER) ? US_UNSHIFTED[make] : 0;
        return 1;
    }

    char c = (mods & KBD_FL_SHIFT) ? US_SHIFTED[make] : US_UNSHIFTED[make];
    if (c == 0) {
        return -1;
    }
    if (is_letter_char(c) && (mods & KBD_FL_CAPS) != 0u) {
        c = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A')
                                   : (char)(c - 'A' + 'a');
    }
    out->code = KEY_NONE;
    out->flags = mods;
    out->ch = c;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Очередь и автоповтор                                                */
/* ------------------------------------------------------------------ */

#ifndef FOBOS_HOST_TEST
static void queue_push_locked(const struct kbd_event *ev)
{
    if (g_kbd.count >= KBD_QUEUE_CAP) {
        g_kbd.stats.dropped++;
        return;
    }
    g_kbd.queue[g_kbd.head] = *ev;
    g_kbd.head = (g_kbd.head + 1u) % KBD_QUEUE_CAP;
    g_kbd.count++;
    g_kbd.stats.pushed++;
}
#endif

#ifndef FOBOS_HOST_TEST
static u64 now_ms(void)
{
    return timer_uptime_ms();
}
#else
static u64 now_ms(void)
{
    return 0u;
}
#endif

/* ------------------------------------------------------------------ */
/* LED-синхронизация (main-контекст, под cli)                          */
/* ------------------------------------------------------------------ */

#ifndef FOBOS_HOST_TEST
static void leds_sync(void)
{
    u64 flags;
    __asm__ volatile ("pushfq\n\tcli\n\tpopq %0"
                      : "=r"(flags) : : "memory");

    g_kbd.cmd_pending = 1;
    int acked = 0;
    if (ps2_write_port1(0xEDu) == K_OK) {
        for (u32 i = 0; i < 100000u; i++) {
            u8 r = 0;
            if (ps2_read_data(&r) == K_OK) {
                if (r == 0xFAu) {
                    acked = 1;
                    break;
                }
                if (r == 0xFEu) {
                    break; /* Resend: LED не критичен, не повторяем */
                }
            }
        }
        if (acked != 0) {
            (void)ps2_write_port1((u8)g_kbd.led_value);
        }
    }
    g_kbd.cmd_pending = 0;
    g_kbd.led_dirty = 0;

    __asm__ volatile ("pushq %0\n\tpopfq" : : "r"(flags) : "memory", "cc");
}
#endif

/* ------------------------------------------------------------------ */
/* IRQ-обработчик (IF=0; klog/спать запрещены)                         */
/* ------------------------------------------------------------------ */

/* Hook: вызывается из IRQ-обработчика после помещения события в очередь
 * (планировщик будит читателей sys_read). IF=0: не спит, не аллоцирует,
 * не логирует. Устанавливается sched_init (sys/task.c). */
static void (*g_wake_hook)(void);

void kbd_set_wake_hook(void (*fn)(void))
{
    g_wake_hook = fn;
}

#ifndef FOBOS_HOST_TEST
static void kbd_irq_handler(void)
{
    u8 sc = 0;
    if (ps2_read_data(&sc) != K_OK) {
        return;
    }

    if (g_kbd.cmd_pending != 0) {
        return; /* ACK/Resend/утечка ответа на нашу команду */
    }

    if (sc == SC_EXT_PREFIX) {
        g_kbd.ext_next = 1;
        return;
    }

    int extended = g_kbd.ext_next;
    g_kbd.ext_next = 0;
    int is_break = (sc & SC_BREAK_MASK) != 0u;
    u8 make = (u8)(sc & (u8)~SC_BREAK_MASK);

    /* 1. Модификаторы: точечно по make-кодам. */
    if (make == 0x2Au || make == 0x36u) {
        if (is_break) {
            g_kbd.mods &= (u8)~KBD_FL_SHIFT;
        } else {
            g_kbd.mods |= KBD_FL_SHIFT;
        }
        g_kbd.repeat_active = 0;
        return;
    }
    if (make == 0x1Du) { /* LCtrl и RCtrl (E0) */
        if (is_break) {
            g_kbd.mods &= (u8)~KBD_FL_CTRL;
        } else {
            g_kbd.mods |= KBD_FL_CTRL;
        }
        g_kbd.repeat_active = 0;
        return;
    }
    if (make == 0x38u) { /* LAlt и RAlt (E0) */
        if (is_break) {
            g_kbd.mods &= (u8)~KBD_FL_ALT;
        } else {
            g_kbd.mods |= KBD_FL_ALT;
        }
        g_kbd.repeat_active = 0;
        return;
    }

    /* 2. CapsLock: тумблер + LED. */
    if (make == 0x3Au && !is_break) {
        g_kbd.mods ^= KBD_FL_CAPS;
        g_kbd.led_dirty = 1;
        g_kbd.led_value = ((g_kbd.mods & KBD_FL_CAPS) != 0u) ? 0x04u : 0x00u;
        return;
    }

    /* 3. Разрыв любой иной клавиши гасит автоповтор. */
    if (is_break) {
        g_kbd.repeat_active = 0;
        return;
    }

    /* 4. Трансляция нажатия. */
    struct kbd_event ev;
    int tr = translate_make(make, extended, g_kbd.mods, &ev);
    if (tr < 0) {
        g_kbd.stats.unmapped++;
        return;
    }
    if (tr == 0) {
        return;
    }

    spin_lock(&g_kbd.lock);
    g_kbd.repeat_ev = ev;
    g_kbd.repeat_active = 1;
    g_kbd.repeat_next_ms = now_ms() + KBD_REPEAT_DELAY_MS;
    queue_push_locked(&ev);
    spin_unlock(&g_kbd.lock);

    if (g_wake_hook != NULL) {
        g_wake_hook();
    }
}
#endif /* !FOBOS_HOST_TEST */

/* ------------------------------------------------------------------ */
/* Публичный API                                                       */
/* ------------------------------------------------------------------ */

err_t kbd_init(void)
{
    klog(KLOG_DEBUG, KBD_TAG, "инициализация PS/2 клавиатуры");
#ifdef FOBOS_HOST_TEST
    return K_ENODEV;
#else
    err_t rc = irq_set_handler(PIC_IRQ_KBD, kbd_irq_handler);
    if (rc != K_OK) {
        return rc;
    }
    klog(KLOG_INFO, KBD_TAG, "PS/2 клавиатура: обработчик IRQ1 установлен, US-раскладка");
    return K_OK;
#endif
}

u8 kbd_mods(void)
{
    return g_kbd.mods;
}

struct kbd_stats kbd_get_stats(void)
{
    u64 flags = spin_lock_irqsave(&g_kbd.lock);
    struct kbd_stats st = g_kbd.stats;
    st.queue_used = g_kbd.count;
    spin_unlock_irqrestore(&g_kbd.lock, flags);
    return st;
}

err_t kbd_poll(struct kbd_event *out)
{
    if (out == NULL) {
        return K_EINVAL;
    }

#ifndef FOBOS_HOST_TEST
    if (g_kbd.led_dirty != 0) {
        leds_sync();
    }
#endif

    u64 flags = spin_lock_irqsave(&g_kbd.lock);

    if (g_kbd.count > 0u) {
        struct kbd_event ev = g_kbd.queue[g_kbd.tail];
        g_kbd.tail = (g_kbd.tail + 1u) % KBD_QUEUE_CAP;
        g_kbd.count--;
        spin_unlock_irqrestore(&g_kbd.lock, flags);
        *out = ev;
        return K_OK;
    }

    /* Автоповтор удерживаемой клавиши. */
    if (g_kbd.repeat_active != 0u && now_ms() >= g_kbd.repeat_next_ms) {
        struct kbd_event ev = g_kbd.repeat_ev;
        g_kbd.repeat_next_ms = now_ms() + KBD_REPEAT_PERIOD_MS;
        spin_unlock_irqrestore(&g_kbd.lock, flags);
        *out = ev;
        return K_OK;
    }

    spin_unlock_irqrestore(&g_kbd.lock, flags);
    return K_EAGAIN;
}

err_t kbd_wait(struct kbd_event *out)
{
    for (;;) {
        err_t rc = kbd_poll(out);
        if (rc != K_EAGAIN) {
            return rc;
        }
        __asm__ volatile ("hlt");
    }
}

/* Доступ selftest к чистой трансляции. */
int kbd_translate_for_test(u8 make, int extended, u8 mods, struct kbd_event *out)
{
    return translate_make(make, extended, mods, out);
}

int kbd_selftest(void)
{
    int fails = 0;
    struct kbd_event ev;

    /* 'a': 0x1E -> ch='a', печатная */
    if (kbd_translate_for_test(0x1Eu, 0, 0, &ev) != 1 || ev.ch != 'a') {
        fails++;
    }
    /* 'a' + Shift -> 'A' */
    if (kbd_translate_for_test(0x1Eu, 0, KBD_FL_SHIFT, &ev) != 1 || ev.ch != 'A') {
        fails++;
    }
    /* 'a' + Caps -> 'A'; 'a' + Caps + Shift -> 'a' */
    if (kbd_translate_for_test(0x1Eu, 0, KBD_FL_CAPS, &ev) != 1 || ev.ch != 'A') {
        fails++;
    }
    if (kbd_translate_for_test(0x1Eu, 0, KBD_FL_CAPS | KBD_FL_SHIFT, &ev) != 1 ||
        ev.ch != 'a') {
        fails++;
    }
    /* Enter: 0x1C -> KEY_ENTER */
    if (kbd_translate_for_test(0x1Cu, 0, 0, &ev) != 1 || ev.code != KEY_ENTER) {
        fails++;
    }
    /* Backspace: 0x0E -> KEY_BSPACE, ch='\b' не нужен (спец-код) */
    if (kbd_translate_for_test(0x0Eu, 0, 0, &ev) != 1 || ev.code != KEY_BSPACE) {
        fails++;
    }
    /* Стрелка вверх: 0xE0 0x48 -> KEY_UP */
    if (kbd_translate_for_test(0x48u, 1, 0, &ev) != 1 || ev.code != KEY_UP) {
        fails++;
    }
    /* RCtrl (E0 0x1D): только флаг, события нет */
    if (kbd_translate_for_test(0x1Du, 1, 0, &ev) != 0) {
        fails++;
    }
    /* Неизвестный базовый сканкод -> -1 */
    if (kbd_translate_for_test(0x7Fu, 0, 0, &ev) != -1) {
        fails++;
    }
    /* Shift сам по себе — не событие */
    if (kbd_translate_for_test(0x2Au, 0, 0, &ev) != 0) {
        fails++;
    }
    /* Цифра с Shift: 0x02 -> '!' */
    if (kbd_translate_for_test(0x02u, 0, KBD_FL_SHIFT, &ev) != 1 || ev.ch != '!') {
        fails++;
    }
    /* Numpad: 0x47 -> '7' */
    if (kbd_translate_for_test(0x47u, 0, 0, &ev) != 1 || ev.ch != '7') {
        fails++;
    }

    return fails;
}
