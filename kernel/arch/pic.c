/*
 * Fobos OS — реализация контроллера 8259A (dual-PIC).
 *
 * Последовательность инициализации (IA-32 SDM, глава про 8259A):
 *   ICW1 (0x11: cascade, edge, ICW4) -> ICW2 (базовые вектора) ->
 *   ICW3 (связка master/slave) -> ICW4 (8086 mode) -> OCW1 (маски).
 *
 * Размаскированы только используемые линии: IRQ0 (PIT), IRQ1 (PS/2 kbd),
 * IRQ2 (cascade — обязан быть открыт для доставки IRQ8..15), IRQ12 (PS/2
 * mouse). Остальные замаскированы до появления драйверов (FND-4+: AHCI
 * и т.д. размаскирует свой линия сам, в своём драйвере).
 */
#include "pic.h"
#include "../lib/io.h"
#include "../lib/log.h"

#define PIC1_CMD 0x20u
#define PIC1_DAT 0x21u
#define PIC2_CMD 0xA0u
#define PIC2_DAT 0xA1u

#define ICW1_INIT      0x10u
#define ICW1_ICW4      0x01u
#define ICW4_8086      0x01u
#define OCW2_EOI       0x20u
#define OCW3_READ_ISR  0x0Bu

static const char *const PIC_TAG = "arch/pic";

static void io_delay_short(void)
{
    /* Три записи на порт 0x80 — историческая задержка между командами PIC.
     * Не для синхронизации с устройством, а для уважения к old-school таймингу. */
    for (int i = 0; i < 3; i++) {
        io_wait();
    }
}

static void remap_one(u8 cmd_port, u8 data_port, u8 vector_base, u8 icw3)
{
    outb(cmd_port, ICW1_INIT | ICW1_ICW4);
    io_delay_short();
    outb(data_port, vector_base);
    io_delay_short();
    outb(data_port, icw3);
    io_delay_short();
    outb(data_port, ICW4_8086);
    io_delay_short();
}

static void set_mask(u8 data_port, u8 mask)
{
    outb(data_port, mask);
}

err_t pic_init(void)
{
    /* 1. Замаскировать всё на время программирования. */
    set_mask(PIC1_DAT, 0xFFu);
    set_mask(PIC2_DAT, 0xFFu);

    /* 2. Remap: master -> 0x20, slave -> 0x28. ICW3: master bit2 (IRQ2),
     *    slave каскад-идентификатор 2. */
    remap_one(PIC1_CMD, PIC1_DAT, PIC_VECTOR_BASE_MASTER, 0x04u);
    remap_one(PIC2_CMD, PIC2_DAT, PIC_VECTOR_BASE_SLAVE, 0x02u);

    /* 3. Маски: master открыт на IRQ0,1,2; slave — на IRQ12 (линия 4 внутри slave). */
    set_mask(PIC1_DAT, (u8)(0xFFu & ~((1u << PIC_IRQ_TIMER) |
                                      (1u << PIC_IRQ_KBD) |
                                      (1u << PIC_IRQ_CASCADE))));
    set_mask(PIC2_DAT, (u8)(0xFFu & ~((1u << (PIC_IRQ_MOUSE - 8u)))));

    klog(KLOG_INFO, PIC_TAG,
         "PIC: remap IRQ0..15 -> вектора 0x20..0x2F; размаскированы IRQ0/1/2/12");
    return K_OK;
}

void pic_eoi(u8 irq)
{
    if (irq >= 8u) {
        outb(PIC2_CMD, OCW2_EOI);
    }
    outb(PIC1_CMD, OCW2_EOI);
}
