/*
 * Fobos OS — контроллер прерываний 8259A (PIC) (прототипы).
 *
 * FND-2a: классический dual-PIC, вектора remap на 0x20..0x2F.
 * SMP/APIC — этап FND-2 (см. docs/roadmap.md); до этого PIC — рабочий
 * путь доставки IRQ0/1/12, а не заглушка.
 */
#ifndef FOBOS_ARCH_PIC_H
#define FOBOS_ARCH_PIC_H

#include <fobos/errors.h>
#include <fobos/types.h>

/* Базовые вектора после remap. */
#define PIC_VECTOR_BASE_MASTER 0x20u  /* IRQ0..IRQ7  -> 0x20..0x27 */
#define PIC_VECTOR_BASE_SLAVE  0x28u  /* IRQ8..IRQ15 -> 0x28..0x2F */

/* Линии, размаскированные на FND-2a. */
#define PIC_IRQ_TIMER    0u
#define PIC_IRQ_KBD      1u
#define PIC_IRQ_CASCADE  2u
#define PIC_IRQ_MOUSE   12u

err_t pic_init(void);

/* EOI: для IRQ >= 8 обязана подтверждаться и ведомая, и ведущая PIC. */
void pic_eoi(u8 irq);

#endif /* FOBOS_ARCH_PIC_H */
