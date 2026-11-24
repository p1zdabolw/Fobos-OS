/*
 * Fobos OS — физический менеджер памяти (bitmap, FND-1).
 *
 * Алгоритм:
 *  1. По карте EFI считается max_paddr -> размер bitmap (1 бит на кадр 4 КиБ).
 *  2. Bitmap размещается в НИЗШЕЙ conventional-области >= размера bitmap
 *     (область ниже 4 ГиБ); все биты инициализируются 1 (занято).
 *  3. Освобождаются (сбрасываются в 0) только кадры EfiConventionalMemory
 *     ниже 4 ГиБ; кадр 0 всегда резервируется (real-mode IVT/EBDA).
 *  4. Резервируются: span ядра, страница boot_info, буфер карты EFI,
 *     сам bitmap.
 *
 * Память выше 4 ГиБ на FND-1 НЕ выдаётся: ядро работает через загрузочное
 * identity-отображение (< 4 ГиБ). Лимит снимается в FND-2 вместе с VMM.
 *
 * Сложность: alloc — O(1) амортизированно (hint на последнее сканированное
 * слово), init — O(n) по кадрам. Потокобезопасность: spinlock (irqsave).
 */
#include "phys.h"
#include "../lib/log.h"
#include "../lib/spinlock.h"
#include "../lib/string.h"

#define PHYS_IDENTITY_LIMIT 0x100000000ULL  /* 4 ГиБ: предел аллокатора FND-1 */

struct phys_state {
    struct spinlock lock;
    u64 *bitmap;          /* 1 бит/кадр: 1 = занято */
    u64 frames_total;     /* ёмкость bitmap (по max_paddr карты) */
    u64 frames_free;      /* allocatable сейчас */
    u64 bitmap_paddr;
    u64 bitmap_bytes;
    u64 high_free_bytes;  /* conventional >= 4 ГиБ */
    u64 scan_hint_word;   /* позиция последнего успешного скана */
    int ready;
};

static struct phys_state g_phys = {
    .lock = SPINLOCK_INIT("phys"),
    .ready = 0
};

static const char *const PHYS_TAG = "mm/phys";

static inline int frame_is_free(u64 frame)
{
    return (g_phys.bitmap[frame >> 6] >> (frame & 63u) & 1u) == 0u;
}

static inline void frame_set(u64 frame)
{
    g_phys.bitmap[frame >> 6] |= 1ull << (frame & 63u);
}

static inline void frame_clear(u64 frame)
{
    g_phys.bitmap[frame >> 6] &= ~(1ull << (frame & 63u));
}

static void reserve_locked(u64 paddr, u64 bytes)
{
    u64 first = paddr >> 12;
    u64 last = (paddr + bytes + PAGE_SIZE - 1u) >> 12;

    for (u64 f = first; f < last && f < g_phys.frames_total; f++) {
        if (frame_is_free(f)) {
            g_phys.frames_free--;
            frame_set(f);
        }
    }
}

/* Статистика по карте EFI: суммарная RAM и свободная RAM (для лога). */
struct map_summary {
    u64 conventional_bytes;
    u64 reserved_bytes;
    u64 high_free;
    u64 max_paddr;
    u32 desc_count;
};

static void map_summarize(const struct fbpi_boot_info *bi, struct map_summary *out)
{
    const u8 *base = (const u8 *)(u64)bi->efi_mmap_phys;
    memset(out, 0, sizeof(*out));

    for (u64 off = 0; off + sizeof(struct fbpi_efi_memdesc) <= bi->efi_mmap_size;
         off += bi->efi_desc_size) {
        const struct fbpi_efi_memdesc *d = (const struct fbpi_efi_memdesc *)(u64)(base + off);
        u64 span = d->number_of_pages << 12;
        u64 end = d->physical_start + span;
        out->desc_count++;
        if (end > out->max_paddr) {
            out->max_paddr = end;
        }
        if (d->type == FBPI_EFI_CONVENTIONAL) {
            out->conventional_bytes += span;
            if (d->physical_start >= PHYS_IDENTITY_LIMIT) {
                out->high_free += span;
            }
        } else if (d->physical_start < PHYS_IDENTITY_LIMIT) {
            /* В RAM-сводку попадают только резервы ниже 4 ГиБ: дыры
             * PCIe/флеш выше (0xfd00000000 и т.п.) памятью не являются. */
            out->reserved_bytes += span;
        }
    }
}

err_t phys_init(const struct fbpi_boot_info *bi)
{
    if (bi == NULL || fbpi_validate(bi) != K_OK) {
        return K_EINVAL;
    }
    if (g_phys.ready != 0) {
        return K_ESTATE;
    }

    struct map_summary sum;
    map_summarize(bi, &sum);

    /* 1. Размер bitmap по максимальному адресу карты. Лимит аллокатора —
     * identity-окно 4 ГиБ (FND-1): память выше не выдаётся, bitmap под
     * MMIO-дыры выше 4 ГиБ не тратим. */
    u64 cap_paddr = (sum.max_paddr > PHYS_IDENTITY_LIMIT)
                        ? PHYS_IDENTITY_LIMIT
                        : sum.max_paddr;
    g_phys.frames_total = (cap_paddr + PAGE_SIZE - 1u) >> 12;
    g_phys.bitmap_bytes = (g_phys.frames_total + 7u) / 8u;
    u64 bitmap_pages = (g_phys.bitmap_bytes + PAGE_SIZE - 1u) >> 12;

    /* 2. Ищем conventional-область под bitmap (ниже 4 ГиБ). */
    const u8 *mmbase = (const u8 *)(u64)bi->efi_mmap_phys;
    u64 chosen = 0;
    int found = 0;
    for (u64 off = 0; off + sizeof(struct fbpi_efi_memdesc) <= bi->efi_mmap_size;
         off += bi->efi_desc_size) {
        const struct fbpi_efi_memdesc *d = (const struct fbpi_efi_memdesc *)(u64)(mmbase + off);
        u64 span = d->number_of_pages << 12;
        if (d->type == FBPI_EFI_CONVENTIONAL &&
            d->physical_start < PHYS_IDENTITY_LIMIT &&
            span >= g_phys.bitmap_bytes) {
            chosen = d->physical_start;
            found = 1;
            break;
        }
    }
    if (found == 0) {
        klog(KLOG_ERROR, PHYS_TAG,
             "нет conventional-области >= %llu байт под bitmap (max_paddr=0x%llx)",
             (unsigned long long)g_phys.bitmap_bytes, (unsigned long long)sum.max_paddr);
        return K_ENOMEM;
    }

    g_phys.bitmap_paddr = chosen;
    g_phys.bitmap = (u64 *)(u64)chosen;
    memset(g_phys.bitmap, 0xFF, g_phys.bitmap_bytes);
    g_phys.frames_free = 0;

    /* 3. Освобождаем conventional-кадры ниже 4 ГиБ. */
    for (u64 off = 0; off + sizeof(struct fbpi_efi_memdesc) <= bi->efi_mmap_size;
         off += bi->efi_desc_size) {
        const struct fbpi_efi_memdesc *d = (const struct fbpi_efi_memdesc *)(u64)(mmbase + off);
        if (d->type != FBPI_EFI_CONVENTIONAL ||
            d->physical_start >= PHYS_IDENTITY_LIMIT) {
            continue;
        }
        u64 first = d->physical_start >> 12;
        u64 end = (d->physical_start + (d->number_of_pages << 12)) >> 12;
        for (u64 f = first; f < end && f < g_phys.frames_total; f++) {
            frame_clear(f);
            g_phys.frames_free++;
        }
    }

    /* 4. Резервы: кадр 0, bitmap, ядро, boot_info, буфер карты EFI. */
    reserve_locked(0, PAGE_SIZE);
    reserve_locked(g_phys.bitmap_paddr, bitmap_pages << 12);
    reserve_locked(bi->kernel_pbase, bi->kernel_size_bytes);

    /* Резерв boot_info: в ядре boot_info живёт в higher-half (vaddr >= KBASE),
     * на host-тестах указатель не подчиняется этой арифметике — пропуск guards. */
    u64 kdelta = bi->kernel_vbase - bi->kernel_pbase;
    if ((u64)(u64 *)bi > kdelta) {
        reserve_locked((u64)(u64 *)bi - kdelta, PAGE_SIZE);
    }
    reserve_locked(bi->efi_mmap_phys, bi->efi_mmap_size);

    g_phys.high_free_bytes = sum.high_free;
    g_phys.scan_hint_word = 0;
    g_phys.ready = 1;

    klog(KLOG_INFO, PHYS_TAG,
         "карта EFI: %u дескрипторов; RAM: %llu МиБ; свободно <4ГиБ: %llu МиБ; >=4ГиБ: %llu МиБ",
         (unsigned int)sum.desc_count,
         (unsigned long long)((sum.conventional_bytes + sum.reserved_bytes) >> 20),
         (unsigned long long)(g_phys.frames_free >> 8),
         (unsigned long long)(sum.high_free >> 20));
    klog(KLOG_INFO, PHYS_TAG,
         "bitmap: 0x%llx (%llu КиБ, %llu кадров), кадры: всего %llu, свободно %llu, резерв %llu",
         (unsigned long long)g_phys.bitmap_paddr,
         (unsigned long long)(g_phys.bitmap_bytes >> 10),
         (unsigned long long)g_phys.frames_total,
         (unsigned long long)g_phys.frames_total,
         (unsigned long long)g_phys.frames_free,
         (unsigned long long)(g_phys.frames_total - g_phys.frames_free));

    return K_OK;
}

err_t phys_alloc_frame(u64 *out_paddr)
{
    if (out_paddr == NULL) {
        return K_EINVAL;
    }
    if (g_phys.ready == 0) {
        return K_ESTATE;
    }

    u64 flags = spin_lock_irqsave(&g_phys.lock);

    err_t rc = K_ENOMEM;
    u64 words = (g_phys.frames_total + 63u) >> 6;

    /* Начинаем с hint; при провале — полный проход. Амортизированно O(1). */
    for (u64 attempt = 0; attempt < 2u; attempt++) {
        for (u64 w = g_phys.scan_hint_word; w < words; w++) {
            u64 word = g_phys.bitmap[w];
            if (word == ~0ull) {
                continue;
            }
            u64 bit = (u64)__builtin_ctzll(~word);
            u64 frame = (w << 6) + bit;
            if (frame >= g_phys.frames_total) {
                break; /* хвостовой неполный бит */
            }
            frame_set(frame);
            g_phys.frames_free--;
            g_phys.scan_hint_word = w;
            *out_paddr = frame << 12;
            rc = K_OK;
            goto out;
        }
        g_phys.scan_hint_word = 0;
    }

out:
    spin_unlock_irqrestore(&g_phys.lock, flags);
    return rc;
}

err_t phys_alloc_contig(u32 count, u64 *out_paddr)
{
    if (count == 0u || out_paddr == NULL) {
        return K_EINVAL;
    }
    if (g_phys.ready == 0) {
        return K_ESTATE;
    }

    u64 flags = spin_lock_irqsave(&g_phys.lock);

    err_t rc = K_ENOMEM;
    u64 frames_total = g_phys.frames_total;
    u64 run_start = 0;
    u64 run_len = 0;

    /* Скан по кадрам с учётом run'а свободных. O(frames_total) — вызывается
     * единожды на инициализацию поверхности, сложность приемлема. */
    for (u64 f = 0; f < frames_total; f++) {
        if (frame_is_free(f)) {
            if (run_len == 0u) {
                run_start = f;
            }
            run_len++;
            if (run_len == (u64)count) {
                for (u64 k = run_start; k < run_start + (u64)count; k++) {
                    frame_set(k);
                }
                g_phys.frames_free -= count;
                if ((run_start >> 6) < g_phys.scan_hint_word) {
                    g_phys.scan_hint_word = 0;
                }
                *out_paddr = run_start << 12;
                rc = K_OK;
                break;
            }
        } else {
            run_len = 0;
        }
    }

    spin_unlock_irqrestore(&g_phys.lock, flags);

    if (rc != K_OK) {
        klog(KLOG_WARN, PHYS_TAG,
             "contig: нет %u смежных свободных кадров (free=%llu)",
             (unsigned int)count,
             (unsigned long long)g_phys.frames_free);
    }
    return rc;
}

err_t phys_free_frame(u64 paddr)
{
    if ((paddr & (PAGE_SIZE - 1u)) != 0u) {
        return K_EINVAL;
    }
    if (g_phys.ready == 0) {
        return K_ESTATE;
    }

    u64 frame = paddr >> 12;
    if (frame >= g_phys.frames_total) {
        return K_ERANGE;
    }

    u64 flags = spin_lock_irqsave(&g_phys.lock);

    if (frame_is_free(frame)) {
        /* Двойное освобождение — ошибка состояния, никогда не молчаливый no-op. */
        spin_unlock_irqrestore(&g_phys.lock, flags);
        klog(KLOG_ERROR, PHYS_TAG, "double free: кадр 0x%llx", (unsigned long long)paddr);
        return K_ESTATE;
    }
    frame_clear(frame);
    g_phys.frames_free++;
    if ((frame >> 6) < g_phys.scan_hint_word) {
        g_phys.scan_hint_word = frame >> 6;
    }

    spin_unlock_irqrestore(&g_phys.lock, flags);
    return K_OK;
}

err_t phys_reserve_range(u64 paddr, u64 bytes)
{
    if (bytes == 0u) {
        return K_EINVAL;
    }
    if (g_phys.ready == 0) {
        return K_ESTATE;
    }

    u64 flags = spin_lock_irqsave(&g_phys.lock);
    reserve_locked(paddr, bytes);
    spin_unlock_irqrestore(&g_phys.lock, flags);
    return K_OK;
}

struct phys_stats phys_get_stats(void)
{
    struct phys_stats s;
    u64 flags = spin_lock_irqsave(&g_phys.lock);
    s.total_frames = g_phys.frames_total;
    s.free_frames = g_phys.frames_free;
    s.reserved_frames = g_phys.frames_total - g_phys.frames_free;
    s.high_free_bytes = g_phys.high_free_bytes;
    spin_unlock_irqrestore(&g_phys.lock, flags);
    return s;
}
