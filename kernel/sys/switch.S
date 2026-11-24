; Fobos OS — контекст-свитч задач и syscall-вход x86_64 (FND-3a).
;
; Модель переключения:
;   Callee-saved регистры (rbx, rbp, r12..r15) сохраняются на стеке задачи
;   в switch-кадр; rsp и адрес продолжения — в struct task_ctx.
;   Caller-saved регистры в точке switch мертвы (C-семантика), а регистры
;   user-кода живут в intr_frame/syscall_frame на kernel-стеке задачи —
;   их восстанавливает общий хвост (POP_REGS + iretq / sysretq).
;
; Раскладка switch-кадра (от младших адресов к старшим):
;   [rbx][rbp][r12][r13][r14][r15][ret]
; Для новой задачи кадр готовится в task.c: 6 нулей + ret=sched_user_entry.
;
; Все ссылки на данные — RIP-relative ([rel sym]).

BITS 64

extern syscall_c_handler

section .text

; ------------------------------------------------------------------
; void sched_switch_asm(struct task_ctx *save_to,  ; rdi
;                       struct task_ctx *load_from); rsi
; Не возвращает управление в вызывающий поток: возвращается в точку
; продолжения load_from (ret по сохранённому адресу).
; ------------------------------------------------------------------
global sched_switch_asm
sched_switch_asm:
    mov rax, [rsp]          ; адрес возврата в C (точка продолжения save_to)
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rdi + 0], rsp      ; save_to->rsp
    mov [rdi + 8], rax      ; save_to->ret
    mov rsp, [rsi + 0]      ; load_from->rsp
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret                     ; -> load_from->ret

; ------------------------------------------------------------------
; Точка первого запуска задачи (ret из sched_switch_asm):
; g_entry_frame указывает на подготовленный intr_frame на kernel-стеке
; этой же задачи; хвост идентичен хвосту idt_stub.
; ------------------------------------------------------------------
global sched_user_entry
sched_user_entry:
    mov rsp, [rel g_entry_frame]
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    add rsp, 16             ; vector + errcode (синтетические нули)
    iretq                   ; -> ring 3

; ------------------------------------------------------------------
; Вход SYSCALL (LSTAR). Предусловия: EFER.SCE=1; FMASK уже очистил
; IF/TF/DF/NT/AC; CPU положил RIP->RCX, RFLAGS->R11; CS/SS = KCODE/KDATA.
; Аргументы SysV: rdi, rsi, rdx, r10 (вместо rcx), r8, r9; номер — rax.
;
; Синтаксис sysretq: rcx = user rip, r11 = user rflags, CS/SS из
; IA32_STAR[63:48] (+RPL3 налагается CPU) — поэтому rcx/r11 сохраняются
; в кадре и восстанавливаются до sysretq.
;
; Раскладка syscall-кадра (от младших адресов к старшим):
;   [rax][rdi][rsi][rdx][r10][r8][r9][user_rip][user_rflags][user_rsp]
;   (совпадает с struct syscall_frame в sys/syscall.h)
; ------------------------------------------------------------------
global syscall_entry_asm
syscall_entry_asm:
    ; user rsp сохраняем в ячейку до переключения стека (rsp ещё user)
    mov [rel g_user_rsp_scratch], rsp
    mov rsp, [rel g_syscall_kstack]     ; kernel-стек текущей задачи
    push qword [rel g_user_rsp_scratch] ; user_rsp
    push r11                            ; user rflags
    push rcx                            ; user rip
    push r9
    push r8
    push r10
    push rdx
    push rsi
    push rdi
    push rax
    mov rdi, rsp                        ; struct syscall_frame *
    call syscall_c_handler              ; s64 rc = handler(frame)
    mov [rsp], rax                      ; результат -> слот rax кадра
    ; восстановление и sysret
    pop rax
    pop rdi
    pop rsi
    pop rdx
    pop r10
    pop r8
    pop r9
    pop rcx                             ; user rip
    pop r11                             ; user rflags
    pop rsp                             ; user rsp
    o64 sysret                          ; 64-bit SYSRET (NASM: sysretq = label!)

section .bss
align 8
global g_entry_frame
g_entry_frame:      resq 1     ; intr_frame первого входа текущей новой задачи
global g_syscall_kstack
g_syscall_kstack:   resq 1     ; kernel-стек (вершина) текущей задачи
g_user_rsp_scratch: resq 1     ; user rsp на время перехода SYSCALL
