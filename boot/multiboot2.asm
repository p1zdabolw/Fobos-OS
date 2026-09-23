BITS 32

MB2_MAGIC      equ 0xE85250D6
MB2_ARCH_I386  equ 0
MB2_TAG_END    equ 0
MB2_TAG_FB     equ 5

section .multiboot2
align 8
mb2_header:
    dd MB2_MAGIC
    dd MB2_ARCH_I386
    dd mb2_header_end - mb2_header
    dd -(MB2_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header))

    dw MB2_TAG_FB
    dw 0
    dd 20
    dd 0
    dd 0
    dd 0
    dd 0

    dw MB2_TAG_END
    dw 0
    dd 8
mb2_header_end:

section .note.GNU-stack noalloc noexec nowrite progbits

section .bss
align 4096
boot_pml4:   resq 512
boot_pdpt:   resq 512
boot_pd_0:   resq 512
boot_pd_1:   resq 512
boot_pd_2:   resq 512
boot_pd_3:   resq 512
boot_stack:  resb 32768
boot_stack_top:

section .rodata
align 8
gdt64:
    dq 0
gdt64_code:
    dq 0x00209A0000000000
gdt64_data:
    dq 0x0000920000000000
gdt64_ptr:
    dw gdt64_ptr - gdt64 - 1
    dq gdt64

section .text
global _start
extern kmain

_start:
    cli
    mov esp, boot_stack_top
    mov edi, eax
    mov esi, ebx
    cmp eax, 0x36D76289
    jne boot_fail
    call setup_paging
    lgdt [gdt64_ptr]
    jmp 0x08:long_entry

setup_paging:
    mov eax, boot_pd_0
    or eax, 0x03
    mov [boot_pdpt], eax
    mov eax, boot_pd_1
    or eax, 0x03
    mov [boot_pdpt + 8], eax
    mov eax, boot_pd_2
    or eax, 0x03
    mov [boot_pdpt + 16], eax
    mov eax, boot_pd_3
    or eax, 0x03
    mov [boot_pdpt + 24], eax

    mov eax, boot_pdpt
    or eax, 0x03
    mov [boot_pml4], eax

    xor ecx, ecx
.map_pd0:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83
    mov [boot_pd_0 + ecx*8], eax
    inc ecx
    cmp ecx, 512
    jne .map_pd0

    xor ecx, ecx
.map_pd1:
    mov eax, ecx
    shl eax, 21
    add eax, 0x40000000
    or eax, 0x83
    mov [boot_pd_1 + ecx*8], eax
    inc ecx
    cmp ecx, 512
    jne .map_pd1

    xor ecx, ecx
.map_pd2:
    mov eax, ecx
    shl eax, 21
    add eax, 0x80000000
    or eax, 0x83
    mov [boot_pd_2 + ecx*8], eax
    inc ecx
    cmp ecx, 512
    jne .map_pd2

    xor ecx, ecx
.map_pd3:
    mov eax, ecx
    shl eax, 21
    add eax, 0xC0000000
    or eax, 0x83
    mov [boot_pd_3 + ecx*8], eax
    inc ecx
    cmp ecx, 512
    jne .map_pd3

    mov eax, boot_pml4
    mov cr3, eax
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    ret

boot_fail:
    hlt

BITS 64
long_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov rsp, boot_stack_top
    mov rdi, rsi
    call kmain
.halt:
    hlt
    jmp .halt