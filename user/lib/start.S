; Fobos OS — стартовый код user-программы (FND-3a).
;
; Раскладка стека на входе (подготовлена ядром, sys/task.c):
;   [rsp] = argc, [rsp+8] = argv[0], [rsp+16] = NULL
; Стек 16-байтно выровнен по SysV ABI. Главные регистры: rdi=argc,
; rsi=argv (переданы и в регистрах, и на стеке — v0 main() их не читает).

BITS 64

extern main

global _start

section .text.start
_start:
    xor ebp, ebp                ; конец цепочки кадров
    mov ax, 0x23                ; UDATA|3: DS/ES не наследуют ring0-значения
    mov ds, ax
    mov es, ax
    call main
    mov rdi, rax                ; exit code = main()
    mov eax, 0                  ; FOS_SYS_exit
    syscall
.hang:                          ; exit не возвращает управление
    jmp .hang
