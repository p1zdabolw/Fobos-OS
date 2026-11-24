/*
 * Fobos OS — ELF64-парсер (общий с host-тестами).
 */
#include "elf.h"

#include "../lib/string.h"

/* --- ELF64 константы и раскладки (System V ABI) --- */

#define EI_NIDENT 16u

#define ELFMAG0 0x7Fu
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS64 2u
#define ELFDATA2LSB 1u

#define ET_EXEC 2u
#define EM_X86_64 62u

#define PT_LOAD 1u
#define PT_INTERP 3u

struct elf64_ehdr {
    u8  e_ident[EI_NIDENT];
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

_Static_assert(sizeof(struct elf64_ehdr) == 64, "ELF64 ehdr layout");

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

_Static_assert(sizeof(struct elf64_phdr) == 56, "ELF64 phdr layout");

/* Верх user-диапазона: ELF обязан линковаться ниже (см. user/user.ld:
 * base 0x400000). Проверяем, что весь образ попадает в canonical user
 * и не залезает в окно ядра/heap-лимит раскладки Fobos. */
#define ELF_USER_TOP 0x0000000080000000ull

static u16 rd16(const u8 *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 rd64(const u8 *p)
{
    return (u64)rd32(p) | ((u64)rd32(p + 4) << 32);
}

err_t elf64_validate(const u8 *img, u32 len)
{
    if (img == NULL || len < sizeof(struct elf64_ehdr)) {
        return K_EINVAL;
    }

    const struct elf64_ehdr *h = (const struct elf64_ehdr *)(const void *)img;

    if (h->e_ident[0] != ELFMAG0 || h->e_ident[1] != ELFMAG1 ||
        h->e_ident[2] != ELFMAG2 || h->e_ident[3] != ELFMAG3) {
        return K_EINVAL;   /* магия */
    }
    if (h->e_ident[4] != ELFCLASS64) {
        return K_ENOSYS;   /* не 64-битный */
    }
    if (h->e_ident[5] != ELFDATA2LSB) {
        return K_ENOSYS;   /* не little-endian */
    }
    if (rd16(img + 16) != ET_EXEC) {
        return K_ENOSYS;   /* e_type: только статический EXEC (без PIE) */
    }
    if (rd16(img + 18) != EM_X86_64) {
        return K_ENOSYS;   /* e_machine */
    }
    if (rd32(img + 20) != 1u) {
        return K_EINVAL;   /* e_version */
    }

    u16 phentsize = rd16(img + 54);
    u16 phnum = rd16(img + 56);
    u64 phoff = rd64(img + 32);

    if (phnum == 0u) {
        return K_EINVAL;   /* исполняемый образ без program headers */
    }
    if (phnum > 16u) {
        return K_EINVAL;   /* разумный потолок для наших статических образов */
    }
    if (phentsize != sizeof(struct elf64_phdr)) {
        return K_EINVAL;
    }
    /* Заголовки целиком внутри образа. */
    if (phoff > (u64)len || phoff + (u64)phnum * (u64)phentsize > (u64)len) {
        return K_EIO;
    }

    /* Точка входа — user-виртуальный адрес. */
    u64 entry = rd64(img + 24);
    if (entry < 0x1000u || entry >= ELF_USER_TOP) {
        return K_EINVAL;
    }

    return K_OK;
}

err_t elf64_load_image(const u8 *img, u32 len, u64 *out_entry,
                       elf_seg_cb cb, void *ctx)
{
    err_t rc = elf64_validate(img, len);
    if (rc != K_OK) {
        return rc;
    }

    u64 entry = rd64(img + 24);
    u16 phnum = rd16(img + 56);
    u64 phoff = rd64(img + 32);
    int have_load = 0;

    for (u16 i = 0; i < phnum; i++) {
        const u8 *ph = img + phoff + (u64)i * (u64)sizeof(struct elf64_phdr);
        u32 p_type = rd32(ph + 0);
        u32 p_flags = rd32(ph + 4);
        u64 p_offset = rd64(ph + 8);
        u64 p_vaddr = rd64(ph + 16);
        u64 p_filesz = rd64(ph + 32);
        u64 p_memsz = rd64(ph + 40);

        if (p_type == PT_INTERP) {
            return K_ENOSYS;   /* динамический интерпретатор не поддержан */
        }
        if (p_type != PT_LOAD) {
            continue;
        }

        /* Диапазоны честного образа. */
        if (p_filesz > p_memsz) {
            return K_EINVAL;
        }
        if (p_memsz == 0u) {
            continue;
        }
        if (p_offset > (u64)len || p_offset + p_filesz > (u64)len) {
            return K_EIO;   /* файловый кусок за пределами образа */
        }
        if (p_vaddr < 0x1000u ||
            p_vaddr + p_memsz < p_vaddr ||
            p_vaddr + p_memsz > ELF_USER_TOP) {
            return K_EINVAL;   /* вне user-диапазона / переполнение */
        }

        struct elf_load_seg seg;
        seg.vaddr = p_vaddr;
        seg.filesz = p_filesz;
        seg.memsz = p_memsz;
        seg.offset = p_offset;
        seg.flags = p_flags;

        if (cb != NULL) {
            rc = cb(ctx, &seg, img);
            if (rc != K_OK) {
                return rc;
            }
        }
        have_load = 1;
    }

    if (have_load == 0) {
        return K_EINVAL;   /* нет ни одного PT_LOAD */
    }

    if (out_entry != NULL) {
        *out_entry = entry;
    }
    return K_OK;
}
