/*
 * Fobos OS — реализация диспетчера IRQ.
 *
 * Spurious IRQ: на каскадной конфигурации 8259A ложные запросы возможны
 * на IRQ7 и IRQ15. Алгоритм: читать ISR (OCW3 0x0B) соответствующего
 * контроллера; если бит линии не установлен — прерывание ложное. Для
 * IRQ15 подтверждение выдаётся ТОЛЬКО master (slave не посылал запроса).
 */
#include "irq.h"
#include "arch.h"
#include "pic.h"
#include "../lib/io.h"
#include "../lib/string.h"

#define IRQ_COUNT 16u

#define PIC1_CMD 0x20u
#define PIC2_CMD 0xA0u
#define OCW2_EOI 0x20u
#define OCW3_READ_ISR 0x0Bu

static irq_handler_t g_handlers[IRQ_COUNT];
static u32 g_spurious[IRQ_COUNT];
static int g_ready;

err_t irq_init(void)
{
    memset(g_handlers, 0, sizeof(g_handlers));
    memset(g_spurious, 0, sizeof(g_spurious));
    g_ready = 1;
    return K_OK;
}

err_t irq_set_handler(u8 irq, irq_handler_t handler)
{
    if (irq >= IRQ_COUNT) {
        return K_ERANGE;
    }
    if (g_ready == 0) {
        return K_ESTATE;
    }
    /* Однопроцессорное ядро FND-2a: регистрация происходит до sti либо
     * из main-контекста с cli — гонки нет. SMP-вариант получит спинлок
     * вместе с SMP в FND-2 (зафиксировано в roadmap). */
    g_handlers[irq] = handler;
    return K_OK;
}

u32 irq_spurious_count(u8 irq)
{
    return (irq < IRQ_COUNT) ? g_spurious[irq] : 0u;
}

/* ISR-регистр (in-service) данного контроллера. */
static u8 pic_read_isr(u8 cmd_port)
{
    outb(cmd_port, OCW3_READ_ISR);
    return inb(cmd_port);
}

void irq_c_dispatch(struct intr_frame *f)
{
    u16 vector = (u16)f->vector;
    u8 irq = (u8)(vector - PIC_VECTOR_BASE_MASTER);

    if (vector < PIC_VECTOR_BASE_MASTER || vector > PIC_VECTOR_BASE_MASTER + 15u) {
        /* Не наш диапазон: внешний код обязан был отсечь; безопаснее тихо вернуть. */
        return;
    }

    /* Фильтр ложных IRQ7/IRQ15. */
    if ((irq == 7u || irq == 15u)) {
        u8 cmd_port = (irq == 7u) ? PIC1_CMD : PIC2_CMD;
        u8 line = (u8)(irq & 7u);
        u8 isr = pic_read_isr(cmd_port);
        if ((isr & (1u << line)) == 0u) {
            /* Ложное: EOI только master (для IRQ15 slave молчит). */
            g_spurious[irq]++;
            outb(PIC1_CMD, OCW2_EOI);
            return;
        }
    }

    irq_handler_t h = g_handlers[irq];
    if (h != NULL) {
        h();
    }

    pic_eoi(irq);
}
