/*
 * Fobos OS — syscall ABI v0 (общий заголовок ядра и user-space).
 *
 * Соглашение вызова: SysV x86_64. Номер syscall — RAX, аргументы
 * RDI, RSI, RDX, R10, R8, R9 (как в Linux ABI: вместо RCX — R10,
 * потому что SYSCALL кладёт в RCX адрес возврата). Возврат — RAX:
 * >= 0 успех, < 0 — отрицательный err_t (fobos/errors.h).
 *
 * Версионирование: системный вызов SYS_ABI_VERSION возвращает константу
 * FOBOS_SYSCALL_ABI. Номера зафиксированы навсегда: существующие номера
 * не переиспользуются и не меняют семантику; новые добавляются в конец.
 */
#ifndef FOBOS_SYSCALL_H
#define FOBOS_SYSCALL_H

#define FOBOS_SYSCALL_ABI 0u

/* Номера системных вызовов v0. */
#define FOS_SYS_exit         0u  /* exit(code) — не возвращает управление  */
#define FOS_SYS_write        1u  /* write(fd, buf, len) -> len | -err      */
#define FOS_SYS_read         2u  /* read(fd, buf, len) -> len | -err       */
#define FOS_SYS_getpid       3u  /* getpid() -> pid                        */
#define FOS_SYS_uptime       4u  /* uptime() -> мс с загрузки              */
#define FOS_SYS_sleep        5u  /* sleep(ms) -> 0                         */
#define FOS_SYS_brk          6u  /* brk(addr) -> новый барьер | -err       */
#define FOS_SYS_abi_version  7u  /* abi_version() -> FOBOS_SYSCALL_ABI     */

/* Дескрипторы v0. 1 = текстовая консоль (текущий терминал), 0 = клавиатура. */
#define FOS_FD_CONSOLE 1u
#define FOS_FD_KBD     0u

/* Событие клавиатуры, передаваемое через SYS_read(fd=0): буфер читается
 * как массив struct fos_keyevent (len обязан быть кратен 4). Раскладка
 * соответствует kernel/drivers/kbd.h (набор сканкодов 1). */
struct fos_keyevent {
    unsigned char code;   /* KEY_* */
    unsigned char flags;  /* KBD_FL_* */
    char ch;              /* ASCII или 0 */
    unsigned char pad;    /* выравнивание до 4 байт */
};

#endif /* FOBOS_SYSCALL_H */
