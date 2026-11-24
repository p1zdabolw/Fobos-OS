/*
 * Fobos OS — минимальный читатель ELF64 (System V ABI, amd64 psABI).
 * Используется только UEFI-загрузчиком для загрузки ядра с ESP.
 */
#ifndef FOBOS_UEFI_ELF64_H
#define FOBOS_UEFI_ELF64_H

#include <fobos/types.h>

#define ELF_MAGIC0 0x7f
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1

#define ET_EXEC 2
#define EM_X86_64 62
#define PT_LOAD 1

struct elf64_ehdr {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
};

struct elf64_phdr {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
};

_Static_assert(sizeof(struct elf64_ehdr) == 64, "ELF64 Ehdr ABI");
_Static_assert(sizeof(struct elf64_phdr) == 56, "ELF64 Phdr ABI");

#endif /* FOBOS_UEFI_ELF64_H */
