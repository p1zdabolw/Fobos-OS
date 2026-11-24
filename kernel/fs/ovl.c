/*
 * Fobos OS — оверлей режима амнезии (реализация).
 *
 * Индекс: статические таблицы (ключ, кадр, цепочка, хеш-корзины) —
 * ~72 КиБ при OVL_MAX_BLOCKS 4096. Данные — кадры 4 КиБ из phys-пула,
 * выделяются при первом касании блока. Хеш — мультипликативный по
 * block_index (старшие биты ключа).
 */
#include "ovl.h"

#include "../lib/log.h"
#include "../lib/string.h"
#include <stddef.h>

#define OVL_TAG "ovl"

#define OVL_MAX_BLOCKS_DEFAULT 4096u
#define OVL_BUCKET_COUNT       1024u   /* степень двойки */
#define OVL_NOBLOCK            0xFFFFFFFFu

static struct {
    const struct ovl_io *io;
    u32 max_blocks;
    int ready;
    int active;

    u32 used;                       /* число занятых записей индекса */
    u64 keys[OVL_MAX_BLOCKS_DEFAULT];
    u32 frames[OVL_MAX_BLOCKS_DEFAULT];
    u32 next[OVL_MAX_BLOCKS_DEFAULT];   /* цепочка внутри корзины */
    u32 buckets[OVL_BUCKET_COUNT];      /* head: индекс записи или NOBLOCK */
} g_ovl;

static u32 hash_block(u64 block_index)
{
    u64 h = block_index * 0x9E3779B97F4A7C15ull;
    return (u32)((h >> 32) & (OVL_BUCKET_COUNT - 1u));
}

/* Поиск записи по индексу блока. Возвращает индекс записи или OVL_NOBLOCK. */
static u32 find_entry(u64 block_index)
{
    u32 b = hash_block(block_index);
    u32 e = g_ovl.buckets[b];
    while (e != OVL_NOBLOCK) {
        if (g_ovl.keys[e] == block_index) {
            return e;
        }
        e = g_ovl.next[e];
    }
    return OVL_NOBLOCK;
}

err_t ovl_init(const struct ovl_io *io, u32 max_blocks)
{
    if (io == NULL || io->read == NULL || io->write == NULL ||
        io->frame_alloc == NULL || io->frame_free == NULL ||
        io->frame_ptr == NULL) {
        return K_EINVAL;
    }
    g_ovl.io = io;
    g_ovl.max_blocks = (max_blocks != 0u) ? max_blocks : OVL_MAX_BLOCKS_DEFAULT;
    if (g_ovl.max_blocks > OVL_MAX_BLOCKS_DEFAULT) {
        return K_EINVAL;
    }
    g_ovl.ready = 1;
    g_ovl.active = 0;
    return K_OK;
}

int ovl_active(void)
{
    return g_ovl.ready && g_ovl.active;
}

void ovl_enable(void)
{
    if (g_ovl.ready == 0) {
        return;
    }
    if (g_ovl.active == 0) {
        for (u32 i = 0; i < OVL_BUCKET_COUNT; i++) {
            g_ovl.buckets[i] = OVL_NOBLOCK;
        }
        g_ovl.used = 0;
        g_ovl.active = 1;
        klog(KLOG_INFO, OVL_TAG,
             "амнезия включена: запись в RAM, диск не изменяется (лимит %u МиБ)",
             (unsigned int)((u64)g_ovl.max_blocks * OVL_BLOCK_BYTES >> 20));
    }
}

void ovl_disable(void)
{
    if (g_ovl.ready == 0 || g_ovl.active == 0) {
        return;
    }
    for (u32 i = 0; i < g_ovl.used; i++) {
        g_ovl.io->frame_free(g_ovl.frames[i]);
    }
    g_ovl.used = 0;
    for (u32 i = 0; i < OVL_BUCKET_COUNT; i++) {
        g_ovl.buckets[i] = OVL_NOBLOCK;
    }
    g_ovl.active = 0;
    klog(KLOG_INFO, OVL_TAG, "амнезия выключена: оверлей сброшен (изменения забыты)");
}

u32 ovl_blocks_used(void)
{
    return g_ovl.used;
}

u32 ovl_max_blocks(void)
{
    return g_ovl.max_blocks;
}

err_t ovl_read(u64 lba, u32 count, void *buf)
{
    if (g_ovl.ready == 0 || g_ovl.io == NULL) {
        return K_ESTATE;
    }
    if (count == 0u || count > OVL_SECT_PER_BL || buf == NULL) {
        return K_EINVAL;
    }
    if (g_ovl.active == 0) {
        return g_ovl.io->read(lba, count, buf);
    }

    u8 *out = (u8 *)buf;
    for (u32 s = 0; s < count; s++) {
        u64 l = lba + s;
        u64 bi = l >> 3;                 /* индекс блока 4 КиБ */
        u32 off = (u32)(l & 7u) * OVL_SECTOR;
        u32 e = find_entry(bi);
        if (e == OVL_NOBLOCK) {
            err_t rc = g_ovl.io->read(l, 1, out + s * OVL_SECTOR);
            if (rc != K_OK) {
                return rc;
            }
        } else {
            u8 *fr = g_ovl.io->frame_ptr(g_ovl.frames[e]);
            if (fr == NULL) {
                return K_EIO;
            }
            for (u32 i = 0; i < OVL_SECTOR; i++) {
                out[s * OVL_SECTOR + i] = fr[off + i];
            }
        }
    }
    return K_OK;
}

err_t ovl_write(u64 lba, u32 count, const void *buf)
{
    if (g_ovl.ready == 0 || g_ovl.io == NULL) {
        return K_ESTATE;
    }
    if (count == 0u || count > OVL_SECT_PER_BL || buf == NULL) {
        return K_EINVAL;
    }
    if (g_ovl.active == 0) {
        return g_ovl.io->write(lba, count, buf);
    }

    const u8 *src = (const u8 *)buf;

    /* Разрезаем диапазон на 4-КиБ блоки; каждый — RMW в оверлей. */
    u64 first = lba >> 3;
    u64 last = (lba + count - 1u) >> 3;

    for (u64 bi = first; bi <= last; bi++) {
        u32 e = find_entry(bi);
        if (e == OVL_NOBLOCK) {
            if (g_ovl.used >= g_ovl.max_blocks) {
                klog(KLOG_ERROR, OVL_TAG,
                     "переполнение оверлея (%u блоков) — запись отклонена",
                     (unsigned int)g_ovl.max_blocks);
                return K_ENOMEM;
            }
            u32 frame = 0;
            err_t rc = g_ovl.io->frame_alloc(&frame);
            if (rc != K_OK) {
                return rc;
            }
            u8 *fr = g_ovl.io->frame_ptr(frame);
            if (fr == NULL) {
                g_ovl.io->frame_free(frame);
                return K_EIO;
            }
            /* Наполняем блок с диска (RMW), затем вставляем в индекс. */
            rc = g_ovl.io->read(bi * OVL_SECT_PER_BL, OVL_SECT_PER_BL, fr);
            if (rc != K_OK) {
                g_ovl.io->frame_free(frame);
                return rc;
            }
            e = g_ovl.used;
            g_ovl.keys[e] = bi;
            g_ovl.frames[e] = frame;
            u32 b = hash_block(bi);
            g_ovl.next[e] = g_ovl.buckets[b];
            g_ovl.buckets[b] = e;
            g_ovl.used++;
        }

        u8 *fr = g_ovl.io->frame_ptr(g_ovl.frames[e]);
        if (fr == NULL) {
            return K_EIO;
        }
        for (u32 s = 0; s < count; s++) {
            u64 l = lba + s;
            if ((l >> 3) != bi) {
                continue;
            }
            u32 off = (u32)(l & 7u) * OVL_SECTOR;
            for (u32 i = 0; i < OVL_SECTOR; i++) {
                fr[off + i] = src[(s) * OVL_SECTOR + i];
            }
        }
    }
    return K_OK;
}
