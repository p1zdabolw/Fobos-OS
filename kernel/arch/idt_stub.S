; Fobos OS — ассемблерные заглушки векторов исключений.
;
; Раскладка стека обязана совпадать с struct intr_frame (kernel/arch/intr.h):
;   CPU (у векторов с кодом ошибки): errcode
;   stub: [errcode=0 у векторов без кода] [vector] [r15..r8, rbp, rdi, rsi, rdx, rcx, rbx, rax]
; После intr_c_handler всё снимается в обратном порядке; у векторов с кодом
; ошибки errcode снимается самим iretq.
;
; Вектора с кодом ошибки (IA-32 SDM, таблица 6-x): 8, 10, 11, 12, 13, 14, 17, 21.
; IRQ-вектора 0x20..0x2F (32..47) кода ошибки не имеют.

BITS 64
DEFAULT REL

extern intr_c_handler

section .text

%macro PUSH_REGS 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro POP_REGS 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
%endmacro

%macro ISR_NOERR 1
isr_%1:
    push qword 0
    push qword %1
    PUSH_REGS
    mov rdi, rsp
    call intr_c_handler
    POP_REGS
    add rsp, 16
    iretq
%endmacro

%macro ISR_ERR 1
isr_%1:
    push qword %1
    PUSH_REGS
    mov rdi, rsp
    call intr_c_handler
    POP_REGS
    add rsp, 8
    iretq
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8          ; #DF
ISR_NOERR 9
ISR_ERR   10         ; #TS
ISR_ERR   11         ; #NP
ISR_ERR   12         ; #SS
ISR_ERR   13         ; #GP
ISR_ERR   14         ; #PF
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17         ; #AC
ISR_NOERR 18
ISR_NOERR 19
ISR_ERR   20         ; reserved (has errcode)
ISR_ERR   21         ; #CP
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; ------------------------------------------------------------------
; Аппаратные прерывания 8259A: вектора 0x20..0x2F (IRQ0..IRQ15).
; Код ошибки не генерируется — синтетический ноль для единообразия кадра.
; ------------------------------------------------------------------
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

section .rodata
global isr_stub_table
isr_stub_table:
    dq isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7
    dq isr_8,  isr_9,  isr_10, isr_11, isr_12, isr_13, isr_14, isr_15
    dq isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23
    dq isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31
    dq isr_32, isr_33, isr_34, isr_35, isr_36, isr_37, isr_38, isr_39
    dq isr_40, isr_41, isr_42, isr_43, isr_44, isr_45, isr_46, isr_47
