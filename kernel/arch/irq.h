/*
 * Fobos OS — диспетчер аппаратных прерываний (IRQ 0..15) (прототипы).
 *
 * Модель: каждый IRQ-вектор (0x20..0x2F) может иметь максимум один
 * обработчик. Регистрация до sti; снятие — irq_set_handler(irq, NULL).
 *
 * Правило контекста: обработчик исполняется с выключенными прерываниями
 * (interrupt gate) и НЕ имеет права вызывать klog/аллокаторы/спать —
 * только обновлять счётчики и очереди. Нарушение фиксируется код-ревью
 * и (в FND-2) lockdep'ом.
 */
#ifndef FOBOS_ARCH_IRQ_H
#define FOBOS_ARCH_IRQ_H

#include <fobos/errors.h>
#include <fobos/types.h>

struct intr_frame;

typedef void (*irq_handler_t)(void);

err_t irq_init(void);
err_t irq_set_handler(u8 irq, irq_handler_t handler);

/* Точка входа из intr_c_handler для векторов >= 0x20.
 * Делает: spurious-фильтр IRQ7/15 -> handler() -> EOI. Возвращает управление. */
void irq_c_dispatch(struct intr_frame *f);

/* Число ложных прерываний (spurious), зафиксированных на линии. */
u32 irq_spurious_count(u8 irq);

#endif /* FOBOS_ARCH_IRQ_H */
