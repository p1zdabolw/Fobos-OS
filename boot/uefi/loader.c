/*
 * Fobos OS — UEFI-загрузчик (этап FND-2b).
 *
 * Обязанности:
 *  1. Прочитать \fobos\kernel.elf с системного раздела (ESP, FAT32).
 *  2. Валидировать ELF64 (ET_EXEC, x86_64, higher-half по FBPI).
 *  3. Разместить ядро по фиксированной физической базе (p_paddr) — см.
 *     docs/boot-protocol.md, "размещение ядра".
 *  4. Получить framebuffer через GOP и карту памяти через GetMemoryMap.
 *  5. Фаза System Setup: окно boot manager (ESC = сброс настроек,
 *     I = установщик, Shift = консоль ядра), установщик системы
 *     (FND-3b: выбор диска, разметка, копирование), OOBE-мастер,
 *     сохранение настроек (\fobos\setup.cfg на ESP, UEFI-переменная —
 *     фолбэк).
 *  6. Построить начальные таблицы страниц: identity-отображение первых 4 ГиБ
 *     (+ точечное покрытие защищаемых регионов выше 4 ГиБ) и higher-half
 *     окно 1 ГиБ на виртуальной базе ядра (страницы 2 МиБ, NX не выставляется —
 *     временная загрузочная раскладка, заменяется VMM в FND-2).
 *  7. Заполнить struct fbpi_boot_info (FBPI v0.1), вызвать ExitBootServices
 *     и передать управление на виртуальный e_entry ядра.
 *
 * ABI: компилируется с -mabi=ms (UEFI x64 = Microsoft x64).
 * Память: все собственные размещения — через AllocatePages с лимитом 4 ГиБ,
 * чтобы после прыжка ядро видело их через identity-окно. Освобождение не требуется:
 * память загрузчика помечена типами EfiLoaderCode/EfiLoaderData/EfiBootServicesData
 * и на FND-2 трактуется ядром как зарезервированная (см. kernel/mm/phys.c).
 */
#include "efi.h"
#include "elf64.h"
#include "lserial.h"
#include "lgfx.h"
#include "lin.h"
#include "oobe.h"
#include "bootctx.h"
#include "espfile.h"
#include "setup.h"
#include "disk.h"

#include <fobos/boot_info.h>
#include <fobos/errors.h>

#include "../../common/foboscfg.h"
#include "../../common/cfglite.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/lib/vsnprintf.h"

static void setup_serial(const char *s);   /* определено ниже (Setup) */

/* Константы размещения ядра. Дублируются в kernel/kernel.ld и kernel/entry.S —
 * согласованность проверяет scripts/check_kernel_elf.py на каждой сборке. */
#define FOBOS_KBASE       0xFFFFFFFF80000000ULL
#define FOBOS_KPHYS_MIN   0x0000000000200000ULL
#define FOBOS_KPHYS_MAX   0x000000003FFFFFFFULL   /* ядро обязано лежать < 4 ГиБ */

#define IDENTITY_MAP_BYTES  0x100000000ULL        /* identity: первые 4 ГиБ */
#define MAP_LIMIT_512G      0x800000000ULL        /* предел ленивых PD в PML4[0] */
#define HH_WINDOW_BYTES     0x40000000ULL         /* higher-half окно: 1 ГиБ */

#define LOADER_STACK_BYTES  0x8000ULL             /* 32 КиБ стека на переход */
#define MMAP_BUF_BYTES      0x8000ULL             /* стартовый буфер карты памяти: 32 КиБ */
#define MMAP_MAX_ATTEMPTS   4
#define KERNEL_PATH_A       "\\fobos\\kernel.elf"   /* ASCII; расширяется в CHAR16 на месте */

static EFI_HANDLE g_image;
static EFI_SYSTEM_TABLE *g_st;
static EFI_BOOT_SERVICES *g_bs;
static struct EFI_LOADED_IMAGE_PROTOCOL *g_loaded_image;

/* Физические адреса всего, что обязано пережить переключение CR3
 * (все ниже 4 ГиБ; regions >= 4 ГиБ до-маппируются через map_2mb_range). */
struct protected_regions {
    u64 image_base, image_size;      /* сам загрузчик (может быть загружен прошивкой где угодно) */
    u64 kernel_base, kernel_size;    /* образ ядра */
    u64 bi_base, bi_size;            /* struct fbpi_boot_info */
    u64 stack_base, stack_size;      /* стек перехода */
    u64 mmap_base, mmap_size;        /* буфер карты памяти */
    u64 cfg_base, cfg_size;          /* блоб настроек (foboscfg) для ядра */
    u64 fb_base, fb_size;            /* GOP-framebuffer (может быть и выше 4 ГиБ) */
    u64 tables[16];                  /* страницы таблиц страниц (2 МиБ-страничные PD/PDPT/PML4) */
    u32 table_count;
};
static struct protected_regions g_prot;

/* Сообщения загрузчика — ASCII: ConOut прошивки до инициализации ядерного
 * serial не гарантирует корректную передачу UTF-8 (widen16 байт-в-байт). */

static void widen16(CHAR16 *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src[i] != '\0' && i + 1u < cap; i++) {
        dst[i] = (CHAR16)(u8)src[i];
    }
    dst[i] = 0;
}

static void con_puts(const char *s)
{
    CHAR16 buf[192];
    widen16(buf, sizeof(buf) / sizeof(buf[0]), s);
    g_st->con_out->output_string(g_st->con_out, buf);
}

static void con_put_u64_hex_raw(u64 v)
{
    static const char digits[] = "0123456789abcdef";
    char s[17];
    int i;

    for (i = 0; i < 16; i++) {
        s[i] = digits[(v >> (60 - i * 4)) & 0xFu];
    }
    s[16] = '\0';
    con_puts(s);
}

static void con_put_status(const char *what, EFI_STATUS st)
{
    con_puts(what);
    con_puts(" 0x");
    con_put_u64_hex_raw(st);
    con_puts("\r\n");
}

/* Аварийный выход: печатает причину и выключает машину через ResetSystem.
 * ResetSystem — runtime-сервис, доступен до ExitBootServices. */
static void __attribute__((noreturn)) die(const char *msg, EFI_STATUS st)
{
    con_puts("[uefi] FATAL: ");
    con_puts(msg);
    con_put_status(" (status", st);
    g_st->runtime_services->reset_system(EFI_RESET_SHUTDOWN, st, 0, NULL);
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* ------------------------------------------------------------------ */
/* Работа с памятью до ExitBootServices                                */
/* ------------------------------------------------------------------ */

/* Размещает size байт не выше 4 ГиБ (обязательное условие identity-окна
 * ядра). Реализация — общий модуль bootctx.c (используется и установщиком). */
static EFI_STATUS alloc_below_4g(u32 mem_type, u64 size, u64 *out_phys)
{
    return bootctx_alloc_below_4g(mem_type, size, out_phys);
}

/* ------------------------------------------------------------------ */
/* Чтение ядра с ESP                                                   */
/* ------------------------------------------------------------------ */

static EFI_STATUS read_kernel_file(u64 *out_size, u64 *out_buf_phys)
{
    struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    struct EFI_FILE_PROTOCOL *root = NULL;
    struct EFI_FILE_PROTOCOL *file = NULL;
    CHAR16 kernel_path16[64];
    EFI_STATUS st;
    u64 size = 0;
    u64 buf = 0;
    u64 got = 0;
    u64 remaining = 0;
    u64 cursor = 0;

    st = g_bs->handle_protocol(g_image, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&g_loaded_image);
    if (EFI_ERROR(st)) {
        die("LoadedImage protocol missing", st);
    }

    widen16(kernel_path16, sizeof(kernel_path16) / sizeof(kernel_path16[0]), KERNEL_PATH_A);

    st = g_bs->handle_protocol(g_loaded_image->device_handle,
                               &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void **)&sfs);
    if (EFI_ERROR(st)) {
        die("SimpleFileSystem not found on boot device", st);
    }

    st = sfs->open_volume(sfs, &root);
    if (EFI_ERROR(st)) {
        die("OpenVolume failed", st);
    }

    st = root->open(root, &file, kernel_path16, 0x1u /* EFI_FILE_MODE_READ */, 0u);
    if (EFI_ERROR(st)) {
        die("cannot open \\fobos\\kernel.elf on ESP", st);
    }

    /* Трюк из спецификации: SetPosition(0xFFFFFFFFFFFFFFFF) ставит позицию в EOF;
     * GetPosition после этого возвращает размер файла. */
    st = file->set_position(file, 0xFFFFFFFFFFFFFFFFULL);
    if (EFI_ERROR(st)) {
        die("SetPosition(EOF) failed", st);
    }
    st = file->get_position(file, &size);
    if (EFI_ERROR(st)) {
        die("GetPosition failed", st);
    }
    if (size == 0u || size > 64u * 1024u * 1024u) {
        die("kernel.elf: bad size (0 or > 64 MiB)", EFI_LOAD_ERROR);
    }

    /* Позиция после EOF-трюка — в конце файла: перематываем на начало перед Read. */
    st = file->set_position(file, 0u);
    if (EFI_ERROR(st)) {
        die("SetPosition(0) failed", st);
    }

    st = alloc_below_4g(EFI_LOADER_DATA, size, &buf);
    if (EFI_ERROR(st)) {
        die("cannot allocate buffer for kernel image", st);
    }

    remaining = size;
    cursor = buf;
    while (remaining > 0u) {
        got = remaining;
        st = file->read(file, &got, (void *)(u64)cursor);
        if (EFI_ERROR(st) || got == 0u) {
            die("Read(kernel.elf) failed or short read", st);
        }
        cursor += got;
        remaining -= got;
    }

    (void)file->close(file);
    (void)root->close(root);

    *out_size = size;
    *out_buf_phys = buf;
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Валидация и размещение ELF-образа ядра                              */
/* ------------------------------------------------------------------ */

static EFI_STATUS place_kernel(u64 img_phys, u64 img_size,
                               u64 *out_entry, u64 *out_pbase, u64 *out_size)
{
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)(u64)img_phys;
    const struct elf64_phdr *ph;
    u64 min_paddr = ~0ull;
    u64 max_end = 0;
    u64 alloc_base, alloc_pages_span;
    EFI_STATUS st;

    if (img_size < sizeof(*eh) ||
        eh->e_ident[0] != ELF_MAGIC0 || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        die("kernel.elf: ELF magic missing", EFI_LOAD_ERROR);
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_ident[6] != EV_CURRENT) {
        die("kernel.elf: expected ELF64 little-endian v1", EFI_LOAD_ERROR);
    }
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64) {
        die("kernel.elf: expected ET_EXEC x86_64 (fixed higher-half)",
            EFI_LOAD_ERROR);
    }
    if (eh->e_phentsize != sizeof(struct elf64_phdr) || eh->e_phnum == 0u ||
        eh->e_phoff + (u64)eh->e_phnum * sizeof(struct elf64_phdr) > img_size) {
        die("kernel.elf: corrupt program header table", EFI_LOAD_ERROR);
    }

    ph = (const struct elf64_phdr *)(img_phys + eh->e_phoff);

    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0u) {
            continue;
        }
        if (ph[i].p_filesz > ph[i].p_memsz) {
            die("kernel.elf: filesz > memsz in PT_LOAD", EFI_LOAD_ERROR);
        }
        if (ph[i].p_offset + ph[i].p_filesz > img_size) {
            die("kernel.elf: PT_LOAD exceeds file size", EFI_LOAD_ERROR);
        }
        u64 delta = ph[i].p_vaddr - ph[i].p_paddr;
        if (delta != FOBOS_KBASE) {
            die("kernel.elf: vaddr - paddr != KBASE (not built per FBPI v0)",
                EFI_LOAD_ERROR);
        }
        if (ph[i].p_paddr < min_paddr) {
            min_paddr = ph[i].p_paddr;
        }
        if (ph[i].p_paddr + ph[i].p_memsz > max_end) {
            max_end = ph[i].p_paddr + ph[i].p_memsz;
        }
    }

    if (min_paddr == ~0ull) {
        die("kernel.elf: no PT_LOAD segments", EFI_LOAD_ERROR);
    }
    if (min_paddr < FOBOS_KPHYS_MIN || max_end > FOBOS_KPHYS_MAX) {
        die("kernel.elf: image outside [1 MiB; 4 GiB) window", EFI_LOAD_ERROR);
    }

    /* Размещение по фиксированному физическому адресу (p_paddr). */
    alloc_base = min_paddr & ~EFI_PAGE_MASK;
    alloc_pages_span = ((max_end - alloc_base) + EFI_PAGE_MASK) & ~EFI_PAGE_MASK;
    st = g_bs->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                              EFI_SIZE_TO_PAGES(alloc_pages_span), &alloc_base);
    if (EFI_ERROR(st)) {
        die("kernel phys range busy (AllocateAddress failed)", st);
    }

    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0u) {
            continue;
        }
        u8 *dst = (u8 *)(u64)ph[i].p_paddr;
        const u8 *src = (const u8 *)(img_phys + ph[i].p_offset);
        for (u64 n = 0; n < ph[i].p_filesz; n++) {
            dst[n] = src[n];
        }
        for (u64 n = ph[i].p_filesz; n < ph[i].p_memsz; n++) {
            dst[n] = 0; /* BSS */
        }
    }

    *out_entry = eh->e_entry;
    *out_pbase = min_paddr;
    *out_size = max_end - min_paddr;
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Framebuffer через GOP                                               */
/* ------------------------------------------------------------------ */

/* Запрошенное разрешение из fobos.cfg (bootcfg_load, до gop_fill_fbinfo). */
static u32 g_bootcfg_res_w;
static u32 g_bootcfg_res_h;

static void gop_fill_fbinfo(struct fbpi_framebuffer *fb)
{
    struct EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    EFI_STATUS st = g_bs->locate_protocol(&EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, NULL, (void **)&gop);
    if (EFI_ERROR(st) || gop == NULL || gop->mode == NULL) {
        die("GOP not found: no graphics output", st);
    }

    /* Запрошенное разрешение (res=WxH из fobos.cfg): перебираем GOP-режимы
     * и переключаемся на точное совпадение WxH с линейным кадром. Не нашли
     * — работаем в текущем режиме (настройка мягко игнорируется). */
    if (g_bootcfg_res_w != 0u && g_bootcfg_res_h != 0u) {
        u32 found = 0xFFFFFFFFu;
        for (u32 m = 0; m < gop->mode->max_mode; m++) {
            u64 isz = 0;
            struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = NULL;
            if (EFI_ERROR(gop->query_mode(gop, m, &isz, &mi)) || mi == NULL) {
                continue;
            }
            if (mi->pixel_format != EFI_GOP_PIXEL_BLTONLY &&
                mi->horizontal_resolution == g_bootcfg_res_w &&
                mi->vertical_resolution == g_bootcfg_res_h) {
                found = m;
            }
            g_bs->free_pool(mi);
        }
        if (found != 0xFFFFFFFFu) {
            st = gop->set_mode(gop, found);
            if (EFI_ERROR(st)) {
                setup_serial("[loader] gop: set_mode failed (keep current)");
            } else {
                setup_serial("[loader] gop: resolution applied");
            }
        } else {
            setup_serial("[loader] gop: requested mode not found (keep current)");
        }
    }

    info = gop->mode->info;
    if (info == NULL || info->pixel_format == EFI_GOP_PIXEL_BLTONLY) {
        die("GOP BltOnly mode: no linear framebuffer", EFI_UNSUPPORTED);
    }
    if (info->horizontal_resolution == 0u || info->vertical_resolution == 0u) {
        die("GOP returned zero resolution", EFI_DEVICE_ERROR);
    }

    fb->phys_addr = gop->mode->frame_buffer_base;
    fb->pitch_bytes = (u64)info->pixels_per_scanline * 4u;
    fb->width = info->horizontal_resolution;
    fb->height = info->vertical_resolution;
    fb->bpp = 32;
    fb->pixel_format = info->pixel_format;
    fb->reserved[0] = 0;
    fb->reserved[1] = 0;
    fb->mask_red = info->pixel_information.red_mask;
    fb->mask_green = info->pixel_information.green_mask;
    fb->mask_blue = info->pixel_information.blue_mask;

    if (fb->phys_addr >= MAP_LIMIT_512G) {
        die("framebuffer above 512 GiB: outside lazy-PD window", EFI_UNSUPPORTED);
    }

    /* Диапазон фиксируется для раскладки страниц ядра: выше 4 ГиБ —
     * до-маппируется в prot_map_all (ленивые PD), ниже — получает атрибуты
     * UC (PWT|PCD) на своих больших страницах в build_page_tables. */
    g_prot.fb_base = fb->phys_addr;
    g_prot.fb_size = fb->pitch_bytes * (u64)fb->height;
}

/* ------------------------------------------------------------------ */
/* Начальные таблицы страниц                                           */
/* ------------------------------------------------------------------ */
/*
 * Раскладка (все записи — флаги PRESENT|WRITE, страницы 2 МиБ, NX выключен):
 *   PML4[0]   -> PDPT0   : identity-отображение первых 4 ГиБ (4 x PD по 512 x 2 МиБ);
 *   PML4[0]   -> PDPT0[i]: для i >= 4 — ленивые PD по запросу (регионы выше 4 ГиБ);
 *   PML4[511] -> PDPT511 : PDPT511[510] -> PDHH : V 0xFFFFFFFF80000000..+1 ГиБ -> P 0..+1 ГиБ.
 *
 * ВАЖНО: это временная загрузочная раскладка (см. docs/boot-protocol.md):
 *  - она не даёт W^X и отображает user-недоступные регионы глобально;
 *  - заменяется полноценным VMM в FND-2, ядро не хранит ссылку на неё после
 *    инициализации собственной системы отображения.
 */

#define PTE_PRESENT 0x1ull
#define PTE_WRITE   0x2ull
#define PTE_PWT     0x8ull     /* write-through: буферизация записи отключается */
#define PTE_PCD     0x10ull    /* cache-disable: UC-диапазон (MMIO/framebuffer) */
#define PTE_PS      0x080ull   /* 2 МиБ-страница в PDE */

static u64 alloc_table_page(void)
{
    u64 addr = 0;
    EFI_STATUS st = alloc_below_4g(EFI_LOADER_DATA, EFI_PAGE_SIZE, &addr);
    if (EFI_ERROR(st)) {
        die("cannot allocate page-table page", st);
    }
    u8 *p = (u8 *)(u64)addr;
    for (u64 i = 0; i < EFI_PAGE_SIZE; i++) {
        p[i] = 0;
    }
    if (g_prot.table_count < sizeof(g_prot.tables) / sizeof(g_prot.tables[0])) {
        g_prot.tables[g_prot.table_count++] = addr;
    }
    return addr;
}

static u64 *ensure_pdpt_entry(u64 *pml4, u64 idx)
{
    if (pml4[idx] == 0u) {
        pml4[idx] = alloc_table_page() | PTE_PRESENT | PTE_WRITE;
    }
    return (u64 *)(u64)(pml4[idx] & ~0xFFFull);
}

/* Отображает [start, end) (выравниваются по 2 МиБ) в identity через ленивые PD.
 * Диапазоны ниже 4 ГиБ уже покрыты — функция используется только выше.
 * extra_flags добавляется к каждому PDE (PWT|PCD для UC-регионов). */
static void map_high_region(u64 *pml4, u64 start, u64 end, u64 extra_flags)
{
    const u64 MB2 = 0x200000ull;

    start &= ~(MB2 - 1u);
    end = (end + MB2 - 1u) & ~(MB2 - 1u);

    for (u64 a = start; a < end; a += MB2) {
        if (a < IDENTITY_MAP_BYTES) {
            continue; /* уже в базовой identity-раскладке */
        }
        if (a >= MAP_LIMIT_512G) {
            die("map_high_region: region above 512 GiB", EFI_UNSUPPORTED);
        }
        u64 gb_index = a >> 30;
        u64 *pdpt0 = ensure_pdpt_entry(pml4, 0);
        if (pdpt0[gb_index] == 0u) {
            pdpt0[gb_index] = alloc_table_page() | PTE_PRESENT | PTE_WRITE;
        }
        u64 *pd = (u64 *)(u64)(pdpt0[gb_index] & ~0xFFFull);
        u64 pde = (a >> 21) & 511u;
        pd[pde] = a | PTE_PRESENT | PTE_WRITE | PTE_PS | extra_flags;
    }
}

static void prot_map_all(u64 *pml4)
{
    map_high_region(pml4, g_prot.image_base, g_prot.image_base + g_prot.image_size, 0u);
    map_high_region(pml4, g_prot.kernel_base, g_prot.kernel_base + g_prot.kernel_size, 0u);
    map_high_region(pml4, g_prot.bi_base, g_prot.bi_base + g_prot.bi_size, 0u);
    map_high_region(pml4, g_prot.stack_base, g_prot.stack_base + g_prot.stack_size, 0u);
    map_high_region(pml4, g_prot.mmap_base, g_prot.mmap_base + g_prot.mmap_size, 0u);
    if (g_prot.cfg_base != 0u) {
        map_high_region(pml4, g_prot.cfg_base, g_prot.cfg_base + g_prot.cfg_size, 0u);
    }
    /* Framebuffer выше 4 ГиБ: real GOP на многих платах размещает буфер
     * над 4 ГиБ (PCIe BAR) — обязателен к отображению, атрибут UC. */
    if (g_prot.fb_base != 0u && g_prot.fb_base >= IDENTITY_MAP_BYTES) {
        map_high_region(pml4, g_prot.fb_base,
                        g_prot.fb_base + g_prot.fb_size,
                        PTE_PWT | PTE_PCD);
    }
    for (u32 i = 0; i < g_prot.table_count; i++) {
        map_high_region(pml4, g_prot.tables[i], g_prot.tables[i] + EFI_PAGE_SIZE, 0u);
    }
}

static u64 build_page_tables(void)
{
    u64 pml4_phys = alloc_table_page();
    u64 *pml4 = (u64 *)(u64)pml4_phys;

    /* --- identity: первые 4 ГиБ --- */
    u64 *pdpt0 = ensure_pdpt_entry(pml4, 0);
    for (u32 gb = 0; gb < 4u; gb++) {
        u64 pd_phys = alloc_table_page();
        pdpt0[gb] = pd_phys | PTE_PRESENT | PTE_WRITE;
        u64 *pd = (u64 *)(u64)pd_phys;
        for (u32 j = 0; j < 512u; j++) {
            pd[j] = (((u64)gb << 30) + ((u64)j << 21)) | PTE_PRESENT | PTE_WRITE | PTE_PS;
        }
    }

    /* --- higher-half: 1 ГиБ по V = 0xFFFFFFFF80000000 -> P 0..1 ГиБ --- */
    u64 *pdpt511 = ensure_pdpt_entry(pml4, 511);
    u64 pdhh_phys = alloc_table_page();
    pdpt511[510] = pdhh_phys | PTE_PRESENT | PTE_WRITE; /* V>>30 & 511 == 510 */
    u64 *pdhh = (u64 *)(u64)pdhh_phys;
    for (u32 j = 0; j < 512u; j++) {
        pdhh[j] = ((u64)j << 21) | PTE_PRESENT | PTE_WRITE | PTE_PS;
    }

    prot_map_all(pml4);

    /* Framebuffer ниже 4 ГиБ: атрибуты UC (PWT|PCD) на больших страницах,
     * накрывающих диапазон GOP. WB-запись в MMIO на реальных платах даёт
     * спекулятивные транзакции к устройству и медленный доступ; QEMU это
     * прощает, железо — нет. Выше 4 ГиБ атрибуты ставит prot_map_all. */
    if (g_prot.fb_base != 0u && g_prot.fb_size != 0u) {
        const u64 MB2 = 0x200000ull;
        u64 a = g_prot.fb_base & ~(MB2 - 1u);
        u64 fb_end = (g_prot.fb_base + g_prot.fb_size + MB2 - 1u) & ~(MB2 - 1u);
        if (fb_end > IDENTITY_MAP_BYTES) {
            fb_end = IDENTITY_MAP_BYTES;
        }
        for (; a < fb_end; a += MB2) {
            u64 *pd = (u64 *)(u64)(pdpt0[a >> 30] & ~0xFFFull);
            pd[(a >> 21) & 511u] |= PTE_PWT | PTE_PCD;
        }
    }

    return pml4_phys;
}

/* ------------------------------------------------------------------ */
/* Карта памяти и boot info                                            */
/* ------------------------------------------------------------------ */

static void copy_firmware_vendor(char *dst, const CHAR16 *src)
{
    u32 i = 0;
    while (src != NULL && src[i] != 0u && i < FBPI_VENDOR_MAX - 1u) {
        dst[i] = (char)(u8)src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Запрашивает карту памяти в предвыделенный буфер (< 4 ГиБ, покрыт identity).
 * При нехватке буфера — переразмещение с до-маппированием нового буфера. */
static void get_mmap(u64 *buf_phys, u64 *buf_cap, u64 *map_size, u64 *map_key,
                     u64 *desc_size, u32 *desc_version, u64 *pml4)
{
    EFI_STATUS st;

    for (int attempt = 0; attempt < MMAP_MAX_ATTEMPTS; attempt++) {
        u64 need = *buf_cap;
        st = g_bs->get_memory_map(&need, (struct EFI_MEMORY_DESCRIPTOR *)(u64)*buf_phys,
                                  map_key, desc_size, desc_version);
        if (!EFI_ERROR(st)) {
            *map_size = need;
            return;
        }
        if (st != EFI_BUFFER_TOO_SMALL) {
            die("GetMemoryMap failed", st);
        }

        /* Расширяем буфер вдвое; старый освобождаем (его отображение безвредно). */
        u64 new_buf = 0;
        u64 new_cap = *buf_cap * 2u;
        st = alloc_below_4g(EFI_BOOT_SERVICES_DATA, new_cap, &new_buf);
        if (EFI_ERROR(st)) {
            die("cannot grow memory-map buffer", st);
        }
        (void)g_bs->free_pages(*buf_phys, EFI_SIZE_TO_PAGES(*buf_cap));
        *buf_phys = new_buf;
        *buf_cap = new_cap;
        g_prot.mmap_base = new_buf;
        g_prot.mmap_size = new_cap;
        prot_map_all(pml4);
    }

    die("GetMemoryMap: buffer retry limit exceeded", EFI_OUT_OF_RESOURCES);
}

/* Дамп карты памяти в ConOut (диагностика размещения ядра; остаётся в коде —
 * полезен при отладке на реальном железе). */
static const char *const mem_type_names[16] = {
    "Reserved", "LdCode ", "LdData ", "BSCode ", "BSData ", "RTCode ", "RTData ",
    "Convent ", "Unusable", "ACPIRec ", "ACPINVS ", "MMIO    ", "MMIOPort",
    "PalCode ", "Max     ", "?       "
};

static void dump_memory_map(void)
{
    static u8 buf[0x8000];
    u64 map_size = 0;
    u64 map_key = 0;
    u64 desc_size = 0;
    u32 desc_version = 0;
    EFI_STATUS st;

    map_size = sizeof(buf);
    st = g_bs->get_memory_map(&map_size, (EFI_MEMORY_DESCRIPTOR *)(u64)buf,
                              &map_key, &desc_size, &desc_version);
    if (EFI_ERROR(st)) {
        con_puts("[uefi] GetMemoryMap (dump) failed, status ");
        con_put_u64_hex_raw(st);
        con_puts("\r\n");
        return;
    }

    con_puts("[uefi] memory map:\r\n");
    for (u64 off = 0; off + sizeof(EFI_MEMORY_DESCRIPTOR) <= map_size; off += desc_size) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)(u64)(buf + off);
        u32 t = d->type;
        if (t > 14u) {
            t = 15u;
        }
        con_puts("  ");
        con_puts(mem_type_names[t]);
        con_puts(" 0x");
        con_put_u64_hex_raw(d->physical_start);
        con_puts(" +");
        con_put_u64_hex_raw(d->number_of_pages << 12);
        con_puts("\r\n");
    }
}


/* ------------------------------------------------------------------ */
/* Файловые операции на ESP (конфигурация FND-2c)                      */
/* ------------------------------------------------------------------ */


#define FOBOS_CFG_PATH_A   "\\fobos\\fobos.cfg"    /* режим рабочего стола */
#define SETUP_CFG_PATH_A   "\\fobos\\setup.cfg"    /* пользователи/hostname */

/* Открывает корень ESP (том загрузчика). 0 = успех. */
/* bootcfg: \fobos\fobos.cfg -> FBPI_BOOTF_GUI (режим рабочего стола
 * по умолчанию) + желаемое разрешение (res=WxH). Разбор — через общий
 * cfglite (тот же парсер, что в ядре). Отсутствие файла — штатно. */

static u32 bootcfg_parse_u32(const char *s)
{
    u32 v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (u32)(*s - '0');
        s++;
    }
    return v;
}

static void bootcfg_load(struct fbpi_boot_info *bi)
{
    u8 *buf = NULL;
    u32 size = 0;
    if (espfile_read(FOBOS_CFG_PATH_A, 4096u, &buf, &size) != 0) {
        setup_serial("[loader] bootcfg: not found (default tui)");
        return;
    }
    const char *text = (const char *)(u64)(u64 *)buf;

    struct cfglite_kv kvs[12];
    u32 n = cfglite_parse(text, size, kvs, 12);

    const char *s = cfglite_get(kvs, n, "mode");
    if (s != NULL) {
        if (strncmp(s, "gui", 3) == 0) {
            bi->boot_flags |= FBPI_BOOTF_GUI;
            setup_serial("[loader] bootcfg: mode=gui");
        } else if (strncmp(s, "tui", 3) == 0) {
            setup_serial("[loader] bootcfg: mode=tui");
        }
    }

    s = cfglite_get(kvs, n, "res");
    if (s != NULL) {
        /* "1280x800" */
        u32 w = bootcfg_parse_u32(s);
        while (*s >= '0' && *s <= '9') {
            s++;
        }
        if (*s == 'x') {
            s++;
            u32 h = bootcfg_parse_u32(s);
            if (w >= 320u && w <= 7680u && h >= 240u && h <= 4320u) {
                g_bootcfg_res_w = w;
                g_bootcfg_res_h = h;
                setup_serial("[loader] bootcfg: res requested");
            } else {
                setup_serial("[loader] bootcfg: res out of range (ignored)");
            }
        }
    }

    /* Освобождение строго после последнего обращения к buf (kvs указывают
     * внутрь буфера): FreePages отладочной прошивки заливает страницы
     * паттерном 0xAF — разбор по освобождённой памяти давал kvs=0. */
    espfile_free(buf, size);
}

/* ------------------------------------------------------------------ */
/* System Setup: UEFI-переменная FobosSetup (создание/чтение)          */
/* ------------------------------------------------------------------ */

/* Fobos vendor GUID переменной настроек (произвольный vendor-GUID проекта). */
static const struct EFI_GUID FOBOS_SETUP_GUID = {
    0x4f626f46u, 0x6f73u, 0x4f53u,
    { 0x8c, 0xd1, 0x4c, 0x55, 0x21, 0x58, 0xf1, 0x0bu }
};

static const CHAR16 SETUP_VAR_NAME[] = {
    'F', 'o', 'b', 'o', 's', 'S', 'e', 't', 'u', 'p', 0
};

#define SETUP_VAR_MAX 4096u

static void setup_serial(const char *s)
{
    lserial_puts(s);
    lserial_puts("\n");
}

/* Вариант 1: \\fobos\\setup.cfg на ESP (первичный источник FND-2c). */
static int setup_load_file(struct fobos_config *out, u8 **out_raw,
                           u32 *out_raw_size)
{
    u8 *buf = NULL;
    u32 size = 0;
    if (espfile_read(SETUP_CFG_PATH_A, SETUP_VAR_MAX, &buf, &size) != 0) {
        return -1;
    }
    if (foboscfg_decode((const void *)(u64)(u64 *)buf, size, out) != K_OK) {
        setup_serial("[loader] setup: file present but corrupt");
        espfile_free(buf, size);
        return -1;
    }
    lserial_puts("[loader] setup: file found (users=");
    lserial_put_hex(out->user_count);
    lserial_puts(")\n");
    *out_raw = buf;   /* владение буфером переходит вызывающему (см. efi_main) */
    *out_raw_size = size;
    return 0;
}

/* Вариант 2 (фолбэк): UEFI-переменная FobosSetup. */
static int setup_load_var(struct fobos_config *out, u8 **out_raw,
                          u32 *out_raw_size)
{
    u32 attrs = 0;
    u64 size = 0;
    /* Первый вызов с Data=NULL: спецификация обязана вернуть
     * EFI_BUFFER_TOO_SMALL и требуемый размер (переменная есть)
     * либо EFI_NOT_FOUND (переменной нет). */
    EFI_STATUS st = g_st->runtime_services->get_variable(
        (CHAR16 *)SETUP_VAR_NAME, &FOBOS_SETUP_GUID, &attrs, &size, NULL);
    lserial_puts("[loader] setup: get_variable#1 status=0x");
    lserial_put_hex(st);
    lserial_puts(" size=");
    lserial_put_hex(size);
    lserial_puts("\n");
    /* Единственный «успешный» исход запроса размера — BUFFER_TOO_SMALL
     * (переменная есть); NOT_FOUND и прочее — настройки нет. */
    if (st != EFI_BUFFER_TOO_SMALL) {
        return -1;
    }
    if (size == 0u || size > SETUP_VAR_MAX) {
        return -1; /* есть, но неприемлемого размера */
    }

    u64 buf_phys = 0;
    u64 alloc_size = size;
    st = alloc_below_4g(EFI_LOADER_DATA, alloc_size, &buf_phys);
    if (EFI_ERROR(st)) {
        die("cannot allocate buffer for setup variable", st);
    }

    size = alloc_size;
    st = g_st->runtime_services->get_variable(
        (CHAR16 *)SETUP_VAR_NAME, &FOBOS_SETUP_GUID, &attrs, &size,
        (void *)(u64)buf_phys);
    if (EFI_ERROR(st) || size == 0u || size > alloc_size) {
        return -1;
    }

    if (foboscfg_decode((const void *)(u64)buf_phys, (u32)size, out) != K_OK) {
        setup_serial("[loader] setup: variable present but corrupt");
        return -1;
    }

    lserial_puts("[loader] setup: variable found (users=");
    lserial_put_hex(out->user_count);
    lserial_puts(")\n");
    *out_raw = (u8 *)(u64)buf_phys;
    *out_raw_size = (u32)size;
    return 0;
}

/* Единая точка чтения настроек: файл ESP -> UEFI-переменная. */
static int setup_load(struct fobos_config *out, u8 **out_raw, u32 *out_raw_size)
{
    if (setup_load_file(out, out_raw, out_raw_size) == 0) {
        return 0;
    }
    return setup_load_var(out, out_raw, out_raw_size);
}

/* Удаляет настройки (сброс: ESC в boot manager): файл ESP + переменная. */
static void setup_delete(void)
{
    espfile_delete(SETUP_CFG_PATH_A);
    EFI_STATUS st = g_st->runtime_services->set_variable(
        (CHAR16 *)SETUP_VAR_NAME, &FOBOS_SETUP_GUID, 0u, 0u, NULL);
    if (EFI_ERROR(st)) {
        con_puts("[uefi] setup: variable delete failed, status ");
        con_put_u64_hex_raw(st);
        con_puts("\r\n");
    }
    setup_serial("[loader] setup: reset (file+variable cleared)");
}

/* Кодирует и сохраняет конфиг. Возвращает 0 = успех. */
static int setup_save(const struct fobos_config *cfg, u8 **out_raw, u32 *out_raw_size)
{
    static u8 encode_buf[FOBOSCFG_HEADER_SIZE +
                         FOBOSCFG_MAX_USERS * FOBOSCFG_USER_SIZE +
                         FOBOSCFG_TAG_LEN];

    u32 size = 0;
    if (foboscfg_encode(cfg, encode_buf, sizeof(encode_buf), &size) != K_OK) {
        setup_serial("[loader] oobe: encode failed (internal error)");
        return -1;
    }

    /* Первичный носитель: файл \\fobos\\setup.cfg на ESP. */
    int frc = espfile_put(SETUP_CFG_PATH_A, encode_buf, size);
    if (frc == 0) {
        lserial_puts("[loader] oobe: setup.cfg written (users=");
        lserial_put_hex(cfg->user_count);
        lserial_puts(")\n");
        lserial_puts("[loader] oobe: saved users=");
        lserial_put_hex(cfg->user_count);
        lserial_puts(" hostname=");
        lserial_puts(cfg->hostname);
        lserial_puts(" status=0x0\n");
        *out_raw = encode_buf;
        *out_raw_size = size;
        return 0;
    }

    /* Фолбэк: UEFI-переменная (ESP недоступен на запись). */
    lserial_puts("[loader] oobe: file write failed, variable fallback\n");
    u32 attrs = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                EFI_VARIABLE_RUNTIME_ACCESS;
    EFI_STATUS st = g_st->runtime_services->set_variable(
        (CHAR16 *)SETUP_VAR_NAME, &FOBOS_SETUP_GUID, attrs, (u64)size, encode_buf);

    lserial_puts("[loader] oobe: saved users=");
    lserial_put_hex(cfg->user_count);
    lserial_puts(" hostname=");
    lserial_puts(cfg->hostname);
    lserial_puts(" status=0x");
    lserial_put_hex(st);
    lserial_puts("\n");

    if (EFI_ERROR(st)) {
        con_puts("[uefi] setup: SetVariable failed, status ");
        con_put_u64_hex_raw(st);
        con_puts("\r\n");
        return -1;
    }

    *out_raw = encode_buf;
    *out_raw_size = size;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Точка входа                                                         */
/* ------------------------------------------------------------------ */

extern void boot_jump(u64 cr3, u64 stack_top, u64 entry_vaddr, u64 boot_info_phys) __attribute__((noreturn));

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    g_image = image_handle;
    g_st = system_table;
    g_bs = system_table->boot_services;
    bootctx_init(image_handle, system_table);

    /* Runtime-проверка сигнатур: ловит ошибку раскладки заголовка на чужой прошивке. */
    if (g_st->hdr.signature != EFI_SYSTEM_TABLE_SIGNATURE ||
        g_bs->hdr.signature != EFI_BOOT_SERVICES_SIGNATURE) {
        die("EFI tables signature mismatch", EFI_UNSUPPORTED);
    }

    /* Serial-лог загрузчика: маркеры для CI и диагностики. */
    lserial_init();
    lserial_puts("[loader] fobos boot manager: Fobos OS (FND-3b)\n");

    /* Отключаем 5-минутный watchdog прошивки на всё время загрузки. */
    EFI_STATUS st = g_bs->set_watchdog_timer(0u, 0u, 0u, NULL);
    if (EFI_ERROR(st)) {
        die("SetWatchdogTimer(0) failed", st);
    }

    dump_memory_map();

    struct fbpi_boot_info *bi = NULL;
    u64 bi_phys = 0;
    st = alloc_below_4g(EFI_LOADER_DATA, EFI_PAGE_SIZE, &bi_phys);
    if (EFI_ERROR(st)) {
        die("cannot allocate boot-info page", st);
    }
    bi = (struct fbpi_boot_info *)(u64)bi_phys;
    for (u64 i = 0; i < EFI_PAGE_SIZE; i++) {
        ((u8 *)bi)[i] = 0;
    }

    u64 stack_base = 0;
    st = alloc_below_4g(EFI_LOADER_DATA, LOADER_STACK_BYTES, &stack_base);
    if (EFI_ERROR(st)) {
        die("cannot allocate transition stack", st);
    }

    u64 mmap_buf = 0;
    st = alloc_below_4g(EFI_BOOT_SERVICES_DATA, MMAP_BUF_BYTES, &mmap_buf);
    if (EFI_ERROR(st)) {
        die("cannot allocate memory-map buffer", st);
    }

    /* 1. fobos.cfg ДО инициализации графики: res=WxH из него выбирает
     * GOP-режим (настройка разрешения применяется к загрузке). */
    bootcfg_load(bi);

    /* 2. Framebuffer (нужен и Setup-мастеру, и ядру). */
    gop_fill_fbinfo(&bi->fb);
    if (lgfx_init(&bi->fb) != 0) {
        die("lgfx_init failed (bad GOP geometry)", EFI_DEVICE_ERROR);
    }

    /* 2. Фаза System Setup: boot manager / OOBE / UEFI-переменная. */
    if (lin_init(g_st) != 0) {
        con_puts("[uefi] WARNING: ConIn unavailable: setup input disabled\r\n");
    }
    (void)oobe_init(g_st);

    u8 *cfg_raw = NULL;
    u32 cfg_raw_size = 0;

    int bm = 0;
    if (g_st->con_in != NULL) {
        bm = oobe_esc_window();
    }

    struct fobos_config cfg;
    for (u32 i = 0; i < sizeof(cfg); i++) {
        ((u8 *)&cfg)[i] = 0;
    }

    int have_cfg = 0;
    if (bm == 1) {
        setup_serial("[loader] setup: reset requested (ESC)");
        setup_delete();
    } else if (bm == 2) {
        /* Shift в окне boot manager: запрос консоли ядра (FBPI boot flag). */
        setup_serial("[loader] boot manager: kernel console requested (shift)");
        bi->boot_flags |= FBPI_BOOTF_CONSOLE;
    }

    /* fobos.cfg уже прочитан ДО инициализации GOP (см. шаг 1 выше). */

    if (bm != 1 && setup_load(&cfg, &cfg_raw, &cfg_raw_size) == 0) {
        have_cfg = 1;
    }

    /* ------------------------------------------------------------------
     * Установщик (FND-3b). Триггеры:
     *  - I в окне boot manager (bm == 3) — явный запрос установки;
     *  - свежий носитель (настроек нет) и в машине есть второй диск —
     *      установка с дистрибутива (аналог запуска setup.exe).
     * Консоль ядра (Shift) установку пропускает.
     * ------------------------------------------------------------------ */
    struct setup_target tgt;
    for (u32 i = 0; i < sizeof(tgt); i++) {
        ((u8 *)&tgt)[i] = 0;
    }

    if (bm != 1 && bm != 2) {
        int run_installer = (bm == 3);
        if (run_installer == 0 && have_cfg == 0) {
            struct disk_list dl;
            if (disk_enumerate(&dl) == 0) {
                for (u8 i = 0; i < dl.count; i++) {
                    if (dl.disks[i].is_boot == 0) {
                        /* Есть отдельный диск: это установочный носитель. */
                        setup_serial("[loader] setup: fresh media + target disk -> installer");
                        run_installer = 1;
                        break;
                    }
                }
            }
        }
        if (run_installer != 0) {
            /* Конфиг носителя передаётся мастеру: перед экраном завершения
             * он переносится на ESP установленной системы (первая загрузка
             * без OOBE); выбор на экране завершения на перенос не влияет. */
            if (setup_run(&tgt, (have_cfg != 0) ? cfg_raw : NULL,
                          (have_cfg != 0) ? cfg_raw_size : 0u) == 0 &&
                tgt.completed != 0) {
                if (tgt.restart != 0) {
                    setup_serial("[loader] setup: restart requested (cold reset)");
                    g_st->runtime_services->reset_system(EFI_RESET_COLD,
                                                         EFI_SUCCESS, 0, NULL);
                    die("ResetSystem returned", EFI_DEVICE_ERROR);
                }
            } else {
                setup_serial("[loader] setup: installer cancelled/failed - continue boot");
            }
        }
    }

    if (!have_cfg && bm != 2) {
        /* Режим консоли ядра (Shift) не требует настройки: OOBE пропускаем,
         * конфиг не нужен (обслуживание без сессии). После установщика
         * OOBE обязателен: на целевом диске нет учётных записей. */
        if (oobe_run(&cfg) == 0) {
            /* Ошибка сохранения не блокирует загрузку: конфиг всё равно
             * передаётся ядру через память (переживёт только эту загрузку). */
            (void)setup_save(&cfg, &cfg_raw, &cfg_raw_size);
            have_cfg = 1;
        } else {
            setup_serial("[loader] setup: oobe failed - booting without config");
        }
    }

    /* 3. Копируем блоб настроек в постоянную память для ядра. */
    if (have_cfg && cfg_raw != NULL && cfg_raw_size > 0u) {
        u64 cfg_phys = 0;
        st = alloc_below_4g(EFI_LOADER_DATA, cfg_raw_size, &cfg_phys);
        if (EFI_ERROR(st)) {
            die("cannot allocate config blob", st);
        }
        const u8 *src = cfg_raw;
        u8 *dst = (u8 *)(u64)cfg_phys;
        for (u32 i = 0; i < cfg_raw_size; i++) {
            dst[i] = src[i];
        }
        g_prot.cfg_base = cfg_phys;
        g_prot.cfg_size = cfg_raw_size;

        lserial_puts("[loader] cfg: blob ");
        lserial_put_hex(cfg_raw_size);
        lserial_puts(" bytes at 0x");
        lserial_put_hex(cfg_phys);
        lserial_puts("\n");
    }

    /* 4. Читаем и размещаем ядро. */
    u64 img_size = 0;
    u64 img_phys = 0;
    read_kernel_file(&img_size, &img_phys);

    u64 entry = 0;
    u64 kbase = 0;
    u64 ksize = 0;
    place_kernel(img_phys, img_size, &entry, &kbase, &ksize);

    /* 5. Защищаемые регионы для раскладки страниц. */
    g_prot.image_base = g_loaded_image->image_base;
    g_prot.image_size = g_loaded_image->image_size;
    g_prot.kernel_base = kbase;
    g_prot.kernel_size = ksize;
    g_prot.bi_base = bi_phys;
    g_prot.bi_size = EFI_PAGE_SIZE;
    g_prot.stack_base = stack_base;
    g_prot.stack_size = LOADER_STACK_BYTES;
    g_prot.mmap_base = mmap_buf;
    g_prot.mmap_size = MMAP_BUF_BYTES;

    /* 6. Таблицы страниц (после известности всех регионов). */
    u64 pml4 = build_page_tables();

    /* 7. Карта памяти (буфер может расшириться с до-маппированием). */
    u64 map_size = 0;
    u64 map_key = 0;
    u64 desc_size = 0;
    u32 desc_version = 0;
    get_mmap(&mmap_buf, &g_prot.mmap_size, &map_size, &map_key, &desc_size, &desc_version, &pml4);

    /* 8. Заполняем FBPI v0.1. */
    bi->magic = FBPI_MAGIC;
    bi->version = FBPI_VERSION;
    bi->header_size = (u16)sizeof(struct fbpi_boot_info);
    bi->kernel_size_bytes = (u32)ksize;
    bi->reserved0 = 0;
    bi->kernel_vbase = FOBOS_KBASE;
    bi->kernel_pbase = kbase;
    bi->efi_mmap_phys = mmap_buf;
    bi->efi_mmap_size = (u32)map_size;
    bi->efi_desc_size = (u32)desc_size;
    bi->efi_desc_version = desc_version;
    bi->reserved1 = 0;
    bi->efi_system_table_phys = (u64)(u64 *)g_st;
    bi->cfg_phys = g_prot.cfg_base;
    bi->cfg_size = (u32)g_prot.cfg_size;
    /* boot_flags НЕ сбрасываем: FBPI_BOOTF_CONSOLE (Shift) и
     * FBPI_BOOTF_GUI (fobos.cfg) выставлены выше по потоку. */
    copy_firmware_vendor(bi->firmware_vendor, g_st->firmware_vendor);
    bi->checksum = fbpi_compute_checksum(bi);

    if (fbpi_validate(bi) != K_OK) {
        die("FBPI self-check failed (loader bug)", EFI_LOAD_ERROR);
    }

    lserial_puts("[loader] kernel: jumping (cfg_size=");
    lserial_put_hex(bi->cfg_size);
    lserial_puts(")\n");

    /* 9. Финальный ExitBootServices с ретраем по спецификации:
     *    карта памяти может измениться между GetMemoryMap и ExitBootServices. */
    (void)g_bs->set_watchdog_timer(0u, 0u, 0u, NULL);

    int ebs_done = 0;
    for (int attempt = 0; attempt < MMAP_MAX_ATTEMPTS && !ebs_done; attempt++) {
        u64 need = g_prot.mmap_size;
        st = g_bs->get_memory_map(&need, (struct EFI_MEMORY_DESCRIPTOR *)(u64)mmap_buf,
                                  &map_key, &desc_size, &desc_version);
        if (EFI_ERROR(st)) {
            die("final GetMemoryMap failed", st);
        }
        bi->efi_mmap_size = (u32)need;
        bi->efi_desc_size = (u32)desc_size;
        bi->efi_desc_version = desc_version;
        bi->checksum = fbpi_compute_checksum(bi);

        st = g_bs->exit_boot_services(g_image, map_key);
        if (!EFI_ERROR(st)) {
            ebs_done = 1;
        } else if (st != EFI_INVALID_PARAMETER) {
            die("ExitBootServices failed", st);
        }
        /* EFI_INVALID_PARAMETER: карта изменилась — повторяем GetMemoryMap + ExitBootServices. */
    }
    if (!ebs_done) {
        die("ExitBootServices: retry limit exceeded", EFI_DEVICE_ERROR);
    }

    /* 10. Прошивка больше не вызывается: всё дальнейшее — только наше.
     * Прыжок: CR3, стек, вход ядра (виртуальный адрес из ELF). */
    boot_jump(pml4, stack_base + LOADER_STACK_BYTES, entry, bi_phys);
}
