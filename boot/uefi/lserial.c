/*
 * Fobos OS — реализация serial-лога загрузчика (см. lserial.h).
 */
#include "lserial.h"

#include <fobos/types.h>
#include <stddef.h>

#define COM1 0x3F8u

static inline void outb8(u16 port, u8 val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb8(u16 port)
{
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static int g_ready;

void lserial_init(void)
{
    outb8(COM1 + 1u, 0x00u);    /* запрет прерываний */
    outb8(COM1 + 3u, 0x80u);    /* DLAB=1 */
    outb8(COM1 + 0u, 0x01u);    /* делитель 1 -> 115200 бод */
    outb8(COM1 + 1u, 0x00u);
    outb8(COM1 + 3u, 0x03u);    /* 8N1 */
    outb8(COM1 + 2u, 0xC7u);    /* FIFO, очистка */
    outb8(COM1 + 4u, 0x0Bu);    /* DTR+RTS+OUT2 */
    g_ready = 1;
}

static void putc_blocking(char c)
{
    if (g_ready == 0) {
        return;
    }
    for (u32 i = 0; i < 100000u; i++) {
        if ((inb8(COM1 + 5u) & 0x20u) != 0u) {
            break;
        }
    }
    outb8(COM1 + 0u, (u8)c);
}

void lserial_puts(const char *s)
{
    if (s == NULL) {
        return;
    }
    for (u32 i = 0; s[i] != '\0'; i++) {
        if (s[i] == '\n') {
            putc_blocking('\r');
        }
        putc_blocking(s[i]);
    }
}

void lserial_put_hex(u64 v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (u32 i = 0; i < 16u; i++) {
        buf[2u + i] = digits[(v >> (60u - i * 4u)) & 0xFu];
    }
    buf[18] = '\0';
    lserial_puts(buf);
}
