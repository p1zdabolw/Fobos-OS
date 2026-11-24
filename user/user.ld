/*
 * Fobos OS — linker-скрипт user-программ (FND-3a).
 *
 * Раскладка согласована с sys/task.h (USER_*):
 *   0x400000 — начало образа (text/rodata);
 *   0x600000 — heap (sys_brk), в образ не входит;
 *   0x7fffe000 — верх стека (не входит в образ).
 * Два сегмента PT_LOAD: R+X (код/rodata) и R+W (data/bss) —
 * ядро мапит их с NX по признаку PF_X (W^X, mm/vmm.c).
 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* R + X */
    data PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text.start)
        *(.text .text.*)
    } :text

    .rodata : {
        *(.rodata .rodata.*)
    } :text

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
