BITS 64

section .note.GNU-stack noalloc noexec nowrite progbits

extern sched_yield_impl
extern sched_switch_rsp

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

section .text

global sched_do_yield
sched_do_yield:
    cli
    pop rax
    push qword 0x10
    lea rcx, [rsp + 8]
    push rcx
    push qword 0x202
    push qword 0x08
    push rax
    push qword 0
    push qword 128
    PUSH_REGS
    mov rdi, rsp
    call sched_yield_impl
    mov rax, [sched_switch_rsp]
    test rax, rax
    jz .no_switch
    mov rsp, rax
    mov qword [sched_switch_rsp], 0
.no_switch:
    POP_REGS
    add rsp, 16
    iretq

global sched_start_first
sched_start_first:
    cli
    mov rsp, rdi
    POP_REGS
    add rsp, 16
    iretq