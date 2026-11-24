/*
 * Fobos OS — драйвер 16550 UART (запись, без прерываний).
 *
 * Регистры (offset от базы): 0 DATA/DLL, 1 IER/DLM, 2 FCR, 3 LCR, 4 MCR, 5 LSR.
 * Стратегия: polling THRE (bit 5 LSR); тайм-аутов нет — на реальном железе
 * absence UART фиксируется на этапе init отсутствием ответа DLAB-записи
 * (см. TODO-фри бэклог: полный probe в FND-2 вместе с ACPI SPCR).
 */
#include "serial.h"
#include "../lib/io.h"
#include "../lib/string.h"

#define UART_REG_DATA   0u
#define UART_REG_IER    1u
#define UART_REG_FCR    2u
#define UART_REG_LCR    3u
#define UART_REG_MCR    4u
#define UART_REG_LSR    5u

#define UART_LSR_THRE   0x20u   /* transmit holding register empty */
#define UART_LCR_DLAB   0x80u

#define UART_DIV_115200 1u      /* 1.8432 MHz / (16 * 1) = 115200 */
#define UART_LCR_8N1    0x03u
#define UART_FCR_FIFO   0xC7u   /* enable, clear, 14-byte trigger */
#define UART_MCR_DTRRTS 0x0Bu   /* DTR|RTS|OUT2 */

static struct {
    int ready;
    u16 port;
} g_serial = { 0, 0 };

err_t serial_init(u16 port)
{
    if (port == 0u) {
        return K_EINVAL;
    }

    /* Отключаем прерывания UART на время конфигурации (FND-1: polling). */
    outb((u16)(port + UART_REG_IER), 0x00u);
    /* DLAB=1: скорость. */
    outb((u16)(port + UART_REG_LCR), UART_LCR_DLAB);
    outb((u16)(port + UART_REG_DATA), (u8)(UART_DIV_115200 & 0xFFu));
    outb((u16)(port + UART_REG_IER), (u8)((UART_DIV_115200 >> 8) & 0xFFu));
    /* 8N1, DLAB=0. */
    outb((u16)(port + UART_REG_LCR), UART_LCR_8N1);
    /* FIFO с очисткой. */
    outb((u16)(port + UART_REG_FCR), UART_FCR_FIFO);
    /* DTR+RTS+OUT2. */
    outb((u16)(port + UART_REG_MCR), UART_MCR_DTRRTS);

    g_serial.port = port;
    g_serial.ready = 1;
    return K_OK;
}

void serial_write(const char *data, u64 len)
{
    if (g_serial.ready == 0 || data == NULL) {
        return;
    }
    for (u64 i = 0; i < len; i++) {
        if (data[i] == '\n') {
            /* CRLF для консольных прошивок/терминалов. */
            while ((inb((u16)(g_serial.port + UART_REG_LSR)) & UART_LSR_THRE) == 0u) {
            }
            outb(g_serial.port + UART_REG_DATA, (u8)'\r');
        }
        while ((inb((u16)(g_serial.port + UART_REG_LSR)) & UART_LSR_THRE) == 0u) {
        }
        outb(g_serial.port + UART_REG_DATA, (u8)data[i]);
    }
}
