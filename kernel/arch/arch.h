/*
 * Fobos OS — архитектурные инициализаторы x86_64 (прототипы).
 */
#ifndef FOBOS_ARCH_ARCH_H
#define FOBOS_ARCH_ARCH_H

#include <fobos/errors.h>
#include <fobos/types.h>
#include "intr.h"

err_t gdt_init(void);
err_t idt_init(void);
err_t tss_init(void);

/* Устанавливает 16-байтовый TSS-дескриптор в GDT (селектор 0x28).
 * Вызывается tss_init ДО ltr. */
void gdt_install_tss(u64 base, u32 limit);

/* Единая точка входа обработчиков исключений из idt_stub.S.
 * f описывает полный кадр прерывания; функция обязана сохранить контекст
 * вызывающего кода (либо остановить систему — см. fault_dump). */
void intr_c_handler(struct intr_frame *f);

/* Доступ для kselftest: 1 = вектор установлен (present, interrupt gate). */
int idt_gate_present(u16 vec);

#endif /* FOBOS_ARCH_ARCH_H */
