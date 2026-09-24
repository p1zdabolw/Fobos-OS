BITS 64

extern isr_dispatch
extern sched_switch_rsp

section .note.GNU-stack noalloc noexec nowrite progbits

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
global isr%1
isr%1:
    push qword 0
    push qword %1
    PUSH_REGS
    mov rdi, rsp
    call isr_dispatch
    POP_REGS
    add rsp, 16
    iretq
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1
    PUSH_REGS
    mov rdi, rsp
    call isr_dispatch
    POP_REGS
    add rsp, 16
    iretq
%endmacro

%macro IRQ 1
global irq%1
irq%1:
    push qword 0
    push qword (32 + %1)
    PUSH_REGS
    mov rdi, rsp
    call isr_dispatch
    mov rax, [sched_switch_rsp]
    test rax, rax
    jz %%no_switch
    mov rsp, rax
    mov qword [sched_switch_rsp], 0
%%no_switch:
    POP_REGS
    add rsp, 16
    iretq
%endmacro

section .text

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

IRQ 0
IRQ 1
IRQ 2
IRQ 3
IRQ 4
IRQ 5
IRQ 6
IRQ 7
IRQ 8
IRQ 9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15

global isr128
isr128:
    push qword 0
    push qword 128
    PUSH_REGS
    mov rdi, rsp
    call isr_dispatch
    POP_REGS
    add rsp, 16
    iretq