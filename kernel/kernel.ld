/*
 * Fobos OS — скрипт линковки ядра (FBPI v0: fixed-load higher-half).
 *
 * KBASE / KPHYS дублируются в:
 *   - boot/uefi/loader.c   (FOBOS_KBASE / FOBOS_KPHYS_MIN)
 *   - kernel/entry.S       (KBASE)
 *   - scripts/check_kernel_elf.py (полная проверка раскладки ELF)
 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KBASE = 0xFFFFFFFF80000000;
KPHYS = 0x0000000000200000;

/* Два сегмента вместо одного RWX: RX для кода/данных чтения, RW для данных.
 * Полноценный W^X (NX на пользовательских страницах) — FND-2; здесь мы хотя бы
 * не собираем единый RWX-сегмент. */
PHDRS
{
    text PT_LOAD FLAGS(5);   /* R + X */
    data PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
    . = KBASE + KPHYS;
    __kernel_vbase = .;
    __kernel_pbase = . - KBASE;

    /* _start обязан быть первым байтом образа: e_entry == KBASE + KPHYS. */
    .text : AT(ADDR(.text) - KBASE)
    {
        KEEP(*(.text.boot))
        *(.text .text.*)
    } :text

    .rodata ALIGN(16) : AT(ADDR(.rodata) - KBASE)
    {
        *(.rodata .rodata.*)
    } :text

    .data ALIGN(16) : AT(ADDR(.data) - KBASE)
    {
        *(.data .data.*)
    } :data

    .bss ALIGN(16) : AT(ADDR(.bss) - KBASE)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    __kernel_end = .;
    __kernel_size = __kernel_end - __kernel_vbase;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
