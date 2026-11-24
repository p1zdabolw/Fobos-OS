/*
 * Fobos OS — управление питанием (reboot / poweroff).
 *
 * Слой: sys. Вызывается из kshell и глобальных хоткеев (Ctrl+Alt+Del).
 *
 * reboot: 8042 pulse 0xFE (стандартный x86-путь), фallback — тройная ошибка.
 * poweroff: запись SLP_TYP|SLP_EN в PM1a_CNT. Адрес порта пока зашит для
 * QEMU q35 (ICH9: PMBASE 0x600, PM1a_CNT = 0x604, S5-значение 0x2000 —
 * соответствует \_S5 в DSDT QEMU). Парсинг ACPI FADT (порт на реальном
 * железе) — этап FND-2 (см. roadmap); до тех пор на реальных машинах
 * poweroff может не работать (задокументированное ограничение).
 */
#ifndef FOBOS_SYS_POWER_H
#define FOBOS_SYS_POWER_H

/* Перезагрузка (не возвращает управление). */
void machine_reboot(void) __attribute__((noreturn));

/* Выключение (не возвращает управление; на реальном железе до FND-2
 * может остаться в hlt-цикле). */
void machine_poweroff(void) __attribute__((noreturn));

#endif /* FOBOS_SYS_POWER_H */
