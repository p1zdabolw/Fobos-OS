/*
 * Fobos OS — реализация управления питанием (см. power.h).
 */
#include "power.h"

#include "../lib/io.h"
#include "../lib/log.h"
#include "../lib/cpuid.h"

/* ICH9 (QEMU q35): PMBASE 0x600, PM1a_CNT 0x604; S5: SLP_TYP=0, SLP_EN=bit13. */
#define ICH9_PM1A_CNT_PORT 0x604u
#define ICH9_PM1A_CNT_S5   0x2000u
/* PIIX4 (QEMU pc): PMBA 0xB000, PM1a_CNT 0xB004. */
#define PIIX4_PM1A_CNT_PORT 0xB004u
#define PIIX4_PM1A_CNT_S5   0x2000u

void machine_reboot(void)
{
    klog(KLOG_INFO, "power", "reboot: 8042 pulse 0xFE");

    /* 8042: импульс линии reset CPU (команда 0xFE). */
    for (u32 i = 0; i < 100000u && (inb(0x64u) & 0x02u) != 0u; i++) {
        io_wait();
    }
    outb(0x64u, 0xFEu);

    /* Fallback: тройная ошибка (IDT -> 0/0) — гарантированный reset x86. */
    for (volatile u32 i = 0; i < 1000000u; i++) {
        __asm__ volatile ("pause");
    }
    klog(KLOG_WARN, "power", "reboot: 8042 не сработал — тройная ошибка");
    struct { u16 limit; u64 base; } __attribute__((packed)) idtr_zero = { 0, 0 };
    __asm__ volatile ("cli\n\tlidt %0\n\tint3" : : "m"(idtr_zero));

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void machine_poweroff(void)
{
    klog(KLOG_INFO, "power", "poweroff: PM1a_CNT <- SLP_EN|S5 (QEMU q35 ICH9)");

    /* QEMU q35 (ICH9). На реальном железе порт определяется через ACPI FADT
     * (этап FND-2); запись в несуществующий порт безвредна. */
    outw(ICH9_PM1A_CNT_PORT, ICH9_PM1A_CNT_S5);

    /* QEMU pc (PIIX4) — на случай смены машины. */
    outw(PIIX4_PM1A_CNT_PORT, PIIX4_PM1A_CNT_S5);

    for (volatile u32 i = 0; i < 2000000u; i++) {
        __asm__ volatile ("pause");
    }

    if (cpu_hypervisor_present() != 0) {
        klog(KLOG_WARN, "power",
             "poweroff: ACPI-регистры не сработали (FND-2 добавит парсинг FADT) — qemu_debug_exit");
        qemu_debug_exit(QEMU_EXIT_OK);
    }

    klog(KLOG_WARN, "power", "poweroff: не выполнен — останов в hlt (машина остаётся включённой)");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
