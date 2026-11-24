/*
 * Fobos OS — TSS x86_64 и переключение привилегий (прототипы).
 *
 * В long mode TSS хранит три поля, реально используемых железом:
 *   RSP0 — стек ядра при входе по прерыванию/исключению из ring 3;
 *   IST1..7 — альтернативные стеки для гейтов с ist != 0 (у нас IST1 = #DF);
 *   I/O bitmap base (не используется).
 *
 * SYSCALL не переключает стек аппаратно — ядро делает это вручную
 * через tss_rsp0_value() (единый источник истины с TSS).
 */
#ifndef FOBOS_ARCH_TSS_H
#define FOBOS_ARCH_TSS_H

#include <fobos/errors.h>
#include <fobos/types.h>

/* Селектор TSS в GDT (см. gdt.c: слоты 6-7). */
#define TSS_SELECTOR 0x30u

err_t tss_init(void);

/* Устанавливает RSP0 (стек ядра текущей задачи). Вызывается планировщиком
 * при каждом переключении ДО перехода в ring 3. */
void tss_set_rsp0(u64 kstack_top);

/* Текущее значение RSP0 — используется syscall-входом (ручное переключение
 * стека, у SYSCALL нет аппаратного). */
u64 tss_rsp0_value(void);

/* 1, если процессор поддерживает и у нас включён SMAP (CR4.SMAP):
 * прямой доступ ядра к user-страницам требует STAC/CLAC. */
int cpu_smap_enabled(void);

#endif /* FOBOS_ARCH_TSS_H */
