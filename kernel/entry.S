; Fobos OS — входная точка ядра (long mode, higher-half).
;
; Приходит от boot/uefi/longmode.S:
;   RDI = физический адрес struct fbpi_boot_info (соглашение SysV).
; Страницы уже включены (identity < 4 ГиБ + higher-half окно 1 ГиБ),
; загрузочный CR3 сохраняется до FND-2 (собственный VMM).
;
; Константы KBASE/KPHYS дублируют kernel/kernel.ld; согласованность
; проверяет scripts/check_kernel_elf.py при каждой сборке.

BITS 64
DEFAULT REL

%define KBASE       0xFFFFFFFF80000000
%define KSTACK_SIZE 65536

extern kmain

section .text.boot
global _start
global kstack_top
_start:
    mov rsp, kstack_top         ; собственный стек ядра (64 КиБ, выровнен на 16)
    xor ebp, ebp                ; конец цепочки кадров
    add rdi, KBASE              ; физический адрес boot_info -> виртуальный (higher-half)
    call kmain
    ; kmain не возвращается; на случай возврата — управляемая остановка.

.hang:
    cli
    hlt
    jmp .hang

section .bss
alignb 16
kstack_bottom:
    resb KSTACK_SIZE
kstack_top:
