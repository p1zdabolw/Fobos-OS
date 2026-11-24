; Fobos OS — финальный переход из загрузчика в ядро.
;
; ABI: UEFI x64 => Microsoft x64 (загрузчик собран с -mabi=ms):
;   rcx = CR3 (физический адрес PML4)
;   rdx = вершина стека перехода (выровнена на 16)
;   r8  = вход ядра — виртуальный адрес ELF e_entry (higher-half)
;   r9  = физический адрес struct fbpi_boot_info
;
; Все отображения под текущим CR3 уже построены загрузчиком:
;   - код загрузчика и стек идентично отображены (map_high_region);
;   - boot_info и стек видны ядру через identity (< 4 ГиБ);
;   - вход ядра виден через higher-half окно PML4[511]->PDPT[510].
;
; Дальше работает kernel/entry.S: он немедленно устанавливает собственный
; стек, поэтому выравнивание rsp здесь критично только для перехода.

BITS 64
DEFAULT REL

section .text

global boot_jump
boot_jump:
    cli                         ; прошивка больше не получает прерываний
    mov cr3, rcx                ; переключение на загрузочные таблицы ядра
    xor ebp, ebp                ; маркер конца кадра для трассировок стека
    mov rsp, rdx
    mov rdi, r9                 ; SysV: первый аргумент kmain = boot_info
    jmp r8                      ; в kernel/entry.S:_start (higher-half)
