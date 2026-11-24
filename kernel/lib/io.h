/*
 * Fobos OS — примитивы порт-ввода/вывода x86_64 и TSC.
 * Только для ядра (ring 0). Компилируется с -mgeneral-regs-only: SIMD не используется.
 */
#ifndef FOBOS_LIB_IO_H
#define FOBOS_LIB_IO_H

#include <fobos/types.h>

static inline void outb(u16 port, u8 val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb(u16 port)
{
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(u16 port, u16 val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline u16 inw(u16 port)
{
    u16 ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_out32(u16 port, u32 val)
{
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline u32 io_in32(u16 port)
{
    u32 ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void)
{
    /* Задержка на несуществующем порту 0x80 — стандартный для x86 post-код канал. */
    outb(0x80, 0);
}

static inline u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* ------------------------------------------------------------------ */
/* QEMU debug-exit: управляемый выход из эмулятора (isa-debug-exit).   */
/* Статус выхода QEMU = (code << 1) | 1; на реальном железе порт 0xF4  */
/* не включается (см. cpu_hypervisor_present()), запись безвредна.     */
/* ------------------------------------------------------------------ */

#define QEMU_DEBUG_EXIT_PORT  0xF4u
#define QEMU_EXIT_OK          0x10u   /* -> статус 33 */
#define QEMU_EXIT_FAIL        0x20u   /* -> статус 65 */

static inline void qemu_debug_exit(u8 code)
{
    outb(QEMU_DEBUG_EXIT_PORT, code);
}

#endif /* FOBOS_LIB_IO_H */
