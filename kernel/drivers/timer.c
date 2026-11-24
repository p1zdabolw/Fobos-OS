/*
 * Fobos OS — реализация драйвера PIT (канал 0).
 *
 * Делитель подбирается из 1193182 Гц; режим 2 (rate generator) даёт
 * периодические импульсы OUT0 -> IRQ0. Счётчик тиков ведётся в ISR,
 * читается атомарно (u64 на x86_64 атомарен по выравниванию, но для
 * честности используем __atomic).
 */
#include "timer.h"
#include "../arch/pic.h"
#include "../arch/irq.h"
#include "../arch/arch.h"
#include "../lib/io.h"
#include "../lib/log.h"

#include <stddef.h>

#define PIT_HZ        1193182u
#define PIT_CH0_DATA  0x40u
#define PIT_CMD       0x43u
#define PIT_CMD_CH0   0x36u   /* channel 0, lobyte/hibyte, mode 2, binary */

static volatile u64 g_ticks;
static u32 g_hz;
static u32 g_divisor;

/* Хук тика (планировщик: кванты/вытеснение). IF=0: только счётчики. */
static void (*g_tick_hook)(void);

void timer_set_tick_hook(void (*fn)(void))
{
    g_tick_hook = fn;
}

static void timer_irq_handler(void)
{
    g_ticks++;
    if (g_tick_hook != NULL) {
        g_tick_hook();
    }
}

err_t timer_init(u32 hz)
{
    if (hz == 0u || hz > PIT_HZ) {
        return K_EINVAL;
    }

    g_divisor = PIT_HZ / hz;
    if (g_divisor == 0u) {
        g_divisor = 1u;
    }
    g_hz = PIT_HZ / g_divisor;

    err_t rc = irq_set_handler(PIC_IRQ_TIMER, timer_irq_handler);
    if (rc != K_OK) {
        return rc;
    }

    outb(PIT_CMD, PIT_CMD_CH0);
    io_wait();
    outb(PIT_CH0_DATA, (u8)(g_divisor & 0xFFu));
    io_wait();
    outb(PIT_CH0_DATA, (u8)((g_divisor >> 8) & 0xFFu));

    klog(KLOG_INFO, "drv/timer",
         "PIT: делитель %u, частота %u Гц (цель %u Гц)", g_divisor, g_hz, hz);
    return K_OK;
}

u64 timer_get_ticks(void)
{
    return __atomic_load_n(&g_ticks, __ATOMIC_RELAXED);
}

u32 timer_get_hz(void)
{
    return g_hz;
}

static inline int interrupts_enabled(void)
{
    u64 flags;
    __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(flags) : : "memory");
    return (flags & 0x200ull) != 0ull; /* EFLAGS.IF */
}

u64 timer_uptime_ms(void)
{
    if (g_hz == 0u) {
        return 0u;
    }
    /* Округление к ближайшему миллисекунду. */
    return (timer_get_ticks() * 1000ull + (u64)g_hz / 2u) / (u64)g_hz;
}

void timer_sleep_ms(u64 ms)
{
    if (ms == 0u) {
        return;
    }
    u64 target = timer_get_ticks() + ms; /* г_hz ~ 1000 => тик ~ мс */

    if (interrupts_enabled()) {
        while (timer_get_ticks() < target) {
            __asm__ volatile ("hlt");
        }
        return;
    }

    /* Прерывания выключены: тики не идут, считаем циклами pause (оценочно:
     * ~2-4 нс на итерацию на современном x86; для инициализации точности
     * не требуется, важно только «не мгновенно»). */
    for (u64 i = 0; i < ms * 200000ull; i++) {
        __asm__ volatile ("pause");
    }
}
