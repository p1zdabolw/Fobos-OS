#ifndef FOS_ELF_H
#define FOS_ELF_H

#include "types.h"

#define ELF_MAGIC 0x464C457F

struct elf64_hdr {
    u32 magic;
    u8  class_;
    u8  data;
    u8  version;
    u8  osabi;
    u8  abiversion;
    u8  pad[7];
    u16 type;
    u16 machine;
    u32 version2;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
} __attribute__((packed));

int elf_load(const void *image, usize size, u64 *entry_out);

#endif