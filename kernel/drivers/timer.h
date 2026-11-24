/*
 * Fobos OS — системный таймер (PIT i8254, канал 0) (прототипы).
 *
 * FND-2a: частота 1000 Гц, счётчик тиков — основа sleep/uptime/повторов
 * клавиатуры. Калибровка TSC через ACPI PM-timer и перенос журнала на
 * реальные метки времени — FND-2 (см. docs/roadmap.md).
 */
#ifndef FOBOS_DRV_TIMER_H
#define FOBOS_DRV_TIMER_H

#include <fobos/errors.h>
#include <fobos/types.h>

#define TIMER_DEFAULT_HZ 1000u

/* Программирует PIT канал 0 (режим 2, rate generator) и ставит IRQ0-обработчик.
 * Требует: pic_init/irq_init выполнены, прерывания пока не включены. */
err_t timer_init(u32 hz);

/* Хук тика (вызывается из IRQ0 после инкремента счётчика; IF=0):
 * планировщик регистрирует sched_timer_tick в sched_init. */
void timer_set_tick_hook(void (*fn)(void));

/* Монотонные тики с момента timer_init. Атомарное чтение. */
u64 timer_get_ticks(void);

/* Реальная частота тиков, Гц (1193182 / делитель; ~1000.04 при 1000). */
u32 timer_get_hz(void);

/* Активное ожидание ms миллисекунд. Работает и при выключенных прерываниях
 * (busy-wait с pause), и при включённых (hlt-цикл — экономит хост-ЦП).
 * ms = 0 — валидный no-op. */
void timer_sleep_ms(u64 ms);

/* Uptime в миллисекундах (ticks -> ms по фактической частоте). */
u64 timer_uptime_ms(void);

#endif /* FOBOS_DRV_TIMER_H */
