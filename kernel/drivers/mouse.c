/*
 * Fobos OS — реализация драйвера PS/2 мыши.
 *
 * Включение: 0xF6 (defaults) -> 0xF4 (enable data reporting) через порт 2
 * (0xD4). Парсер — конечный автомат на 3 байта; байт 0 без бита 3 = рассинхрон
 * (сброс до поиска синхро-байта). Переполнение (биты 6/7) — движение
 * отбрасывается, кнопки сохраняются.
 */
#include "mouse.h"
#include "ps2.h"
#include "../arch/pic.h"
#include "../arch/irq.h"
#include "../lib/log.h"
#include "../lib/spinlock.h"
#include "../lib/string.h"
#include <stddef.h>

#ifndef FOBOS_HOST_TEST
#include "../lib/io.h"
#endif

#define MOUSE_QUEUE_CAP 64u

#define MBF_YOVERFLOW 0x80u
#define MBF_XOVERFLOW 0x40u
#define MBF_YSIGN     0x20u
#define MBF_XSIGN     0x10u
#define MBF_SYNC      0x08u
#define MBF_BUTTONS   0x07u

struct mouse_state {
    struct spinlock lock;
    struct mouse_event queue[MOUSE_QUEUE_CAP];
    u32 head, tail, count;

    u8  phase;      /* 0: ждём байт 0, 1: байт 1, 2: байт 2 */
    u8  b0, b1;
    u8  last_buttons;

    struct mouse_stats stats;
};

static struct mouse_state g_mouse = {
    .lock = SPINLOCK_INIT("mouse"),
};

static const char *const MOUSE_TAG = "drv/mouse";

/*
 * Чистый шаг парсера: один байт из порта.
 * Возвращает 1, если собран полный пакет (записан в *out); 0 — пакет ещё
 * не готов; -1 — рассинхрон (байт испорчен, счётчик bad увеличивает вызывающий
 * код через возврат, здесь только сигнал).
 */
static int mouse_feed_byte(u8 b, struct mouse_event *out)
{
    switch (g_mouse.phase) {
    case 0:
        if ((b & MBF_SYNC) == 0u) {
            return -1; /* не похоже на байт 0: рассинхрон */
        }
        g_mouse.b0 = b;
        g_mouse.phase = 1;
        return 0;
    case 1:
        g_mouse.b1 = b;
        g_mouse.phase = 2;
        return 0;
    case 2: {
        g_mouse.phase = 0;

        u8 b0 = g_mouse.b0;
        u8 b2 = b;

        /* Знаковое расширение 9-битных дельт. */
        s32 dx = (s32)g_mouse.b1;
        s32 dy = (s32)b2;
        if ((b0 & MBF_XSIGN) != 0u) {
            dx -= 256;
        }
        if ((b0 & MBF_YSIGN) != 0u) {
            dy -= 256;
        }

        out->buttons = (u8)(b0 & MBF_BUTTONS);

        if ((b0 & (MBF_XOVERFLOW | MBF_YOVERFLOW)) != 0u) {
            out->dx = 0;
            out->dy = 0;
            return 2; /* пакет валиден, но движение отброшено */
        }
        if (dx > 32767) { dx = 32767; }
        if (dx < -32768) { dx = -32768; }
        if (dy > 32767) { dy = 32767; }
        if (dy < -32768) { dy = -32768; }
        out->dx = (s16)dx;
        out->dy = (s16)dy;
        return 1;
    }
    default:
        g_mouse.phase = 0;
        return -1;
    }
}

#ifndef FOBOS_HOST_TEST
static void mouse_irq_handler(void)
{
    u8 b = 0;
    if (ps2_read_data(&b) != K_OK) {
        return;
    }

    struct mouse_event ev;
    int rc = mouse_feed_byte(b, &ev);

    if (rc == -1) {
        g_mouse.stats.packets_bad++;
        return;
    }
    if (rc == 0) {
        return;
    }
    g_mouse.last_buttons = ev.buttons; /* u8: атомарность достаточна */
    if (rc == 2) {
        g_mouse.stats.packets_bad++; /* overflow: движение потеряно */
    } else {
        g_mouse.stats.packets_ok++;
    }

    spin_lock(&g_mouse.lock);
    if (g_mouse.count >= MOUSE_QUEUE_CAP) {
        g_mouse.stats.dropped++;
    } else {
        g_mouse.queue[g_mouse.head] = ev;
        g_mouse.head = (g_mouse.head + 1u) % MOUSE_QUEUE_CAP;
        g_mouse.count++;
    }
    spin_unlock(&g_mouse.lock);
}
#endif /* !FOBOS_HOST_TEST */

err_t mouse_init(void)
{
    klog(KLOG_DEBUG, MOUSE_TAG, "инициализация PS/2 мыши");
#ifdef FOBOS_HOST_TEST
    return K_ENODEV;
#else
    err_t rc = irq_set_handler(PIC_IRQ_MOUSE, mouse_irq_handler);
    if (rc != K_OK) {
        return rc;
    }

    /* defaults (масштаб 1:1, разрешение 4 counts/mm) + enable data reporting,
     * оба с ожиданием ACK (прерывания ещё выключены на этапе init). */
    if (ps2_port2_cmd(0xF6u) != K_OK) {
        klog(KLOG_ERROR, MOUSE_TAG, "мышь: 0xF6 без ACK");
        return K_EIO;
    }
    if (ps2_port2_cmd(0xF4u) != K_OK) {
        klog(KLOG_ERROR, MOUSE_TAG, "мышь: 0xF4 без ACK");
        return K_EIO;
    }

    klog(KLOG_INFO, MOUSE_TAG, "PS/2 мышь: IRQ12, streaming mode включён");
    return K_OK;
#endif
}

err_t mouse_poll(struct mouse_event *out)
{
    if (out == NULL) {
        return K_EINVAL;
    }

    u64 flags = spin_lock_irqsave(&g_mouse.lock);
    if (g_mouse.count == 0u) {
        spin_unlock_irqrestore(&g_mouse.lock, flags);
        return K_EAGAIN;
    }
    struct mouse_event ev = g_mouse.queue[g_mouse.tail];
    g_mouse.tail = (g_mouse.tail + 1u) % MOUSE_QUEUE_CAP;
    g_mouse.count--;
    spin_unlock_irqrestore(&g_mouse.lock, flags);

    *out = ev;
    return K_OK;
}

struct mouse_stats mouse_get_stats(void)
{
    u64 flags = spin_lock_irqsave(&g_mouse.lock);
    struct mouse_stats st = g_mouse.stats;
    st.queue_used = g_mouse.count;
    spin_unlock_irqrestore(&g_mouse.lock, flags);
    return st;
}

u8 mouse_buttons(void)
{
    return g_mouse.last_buttons;
}

/* Доступ selftest к парсеру. */
int mouse_feed_for_test(u8 b, struct mouse_event *out)
{
    return mouse_feed_byte(b, out);
}

int mouse_selftest(void)
{
    int fails = 0;
    struct mouse_event ev;
    int rc;

    /* Сброс фазы: синхро-байт, дельты (25, 0), кнопки 0. */
    g_mouse.phase = 0;
    rc = mouse_feed_for_test(0x08u, &ev);
    rc += mouse_feed_for_test(25u, &ev);
    rc += mouse_feed_for_test(0u, &ev);
    if (rc != 1 || ev.dx != 25 || ev.dy != 0 || ev.buttons != 0u) {
        fails++;
    }

    /* Отрицательная дельта Y: ysign (бит 5), dy=3 -> dy = 3-256 = -253
     * (экранное «вниз» после инверсии в ui — здесь сырые пакеты). */
    g_mouse.phase = 0;
    (void)mouse_feed_for_test((u8)(0x08u | 0x20u), &ev);
    (void)mouse_feed_for_test(0u, &ev);
    rc = mouse_feed_for_test(3u, &ev);
    if (rc != 1 || ev.dx != 0 || ev.dy != -253) {
        fails++;
    }

    /* Отрицательная дельта X: xsign, dx=1 -> 1-256 = -255. */
    g_mouse.phase = 0;
    (void)mouse_feed_for_test((u8)(0x08u | 0x10u), &ev);
    (void)mouse_feed_for_test(1u, &ev);
    rc = mouse_feed_for_test(0u, &ev);
    if (rc != 1 || ev.dx != -255 || ev.dy != 0) {
        fails++;
    }

    /* Кнопка левая: бит 0 байта 0. */
    g_mouse.phase = 0;
    (void)mouse_feed_for_test(0x09u, &ev);
    (void)mouse_feed_for_test(0u, &ev);
    rc = mouse_feed_for_test(0u, &ev);
    if (rc != 1 || ev.buttons != MOUSE_BTN_LEFT) {
        fails++;
    }

    /* Overflow: движение отброшено (dx=dy=0), rc==2, кнопки сохранены. */
    g_mouse.phase = 0;
    (void)mouse_feed_for_test((u8)(0x08u | 0xC0u | MOUSE_BTN_RIGHT), &ev);
    (void)mouse_feed_for_test(100u, &ev);
    rc = mouse_feed_for_test(100u, &ev);
    if (rc != 2 || ev.dx != 0 || ev.dy != 0 ||
        ev.buttons != MOUSE_BTN_RIGHT) {
        fails++;
    }

    /* Рассинхрон: байт без бита 3 -> -1. */
    g_mouse.phase = 0;
    rc = mouse_feed_for_test(0x00u, &ev);
    if (rc != -1) {
        fails++;
    }

    return fails;
}
