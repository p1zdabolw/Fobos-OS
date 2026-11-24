/*
 * Fobos OS — блочная подсистема хранения (реализация).
 *
 * Стратегия поиска тома: первый готовый AHCI-порт -> сектор 0 ->
 * таблица разделов MBR -> первая партиция типа 0x0B/0x0C (FAT32 CHS/LBA).
 * GPT — вне этапа (ESP QEMU собирается с MBR; поддержка GPT — FND-4).
 */
#include "blk.h"

#include "ovl.h"
#include "fat32.h"
#include "../../common/mbr.h"
#include "../drivers/pci.h"
#include "../mm/phys.h"
#include "../drivers/ahci.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include <stddef.h>

#define B_TAG "fs"

/* Продакшн-лимит оверлея амнезии: 4096 блоков x 4 КиБ = 16 МиБ RAM. */
#define BLK_OVL_BLOCKS 4096u

static struct {
    int ready;
    u8 port;
    u64 part_lba, part_sectors;
    char model[41];
    u64 disk_sectors;
    struct fat_vol vol;
} g_blk;

/* --- бэкенд для оверлея (AHCI-операции + phys-кадры) --- */

static err_t ovl_backend_read(u64 lba, u32 count, void *buf)
{
    return ahci_read(g_blk.port, lba, count, buf);
}

static err_t ovl_backend_write(u64 lba, u32 count, const void *buf)
{
    return ahci_write(g_blk.port, lba, count, buf);
}

static err_t ovl_backend_frame_alloc(u32 *out_frame)
{
    u64 pa;
    err_t rc = phys_alloc_frame(&pa);
    if (rc != K_OK) {
        return rc;
    }
    *out_frame = (u32)(pa >> 12);
    return K_OK;
}

static void ovl_backend_frame_free(u32 frame)
{
    (void)phys_free_frame((u64)frame << 12);
}

static u8 *ovl_backend_frame_ptr(u32 frame)
{
    return (u8 *)(u64)((u64)frame << 12);
}

static const struct ovl_io g_ovl_io = {
    ovl_backend_read, ovl_backend_write,
    ovl_backend_frame_alloc, ovl_backend_frame_free, ovl_backend_frame_ptr
};

/* --- MBR --- */

/* Разбор MBR-сектора делегирован общему модулю common/mbr.c (один источник
 * истины для ядра, установщика в загрузчике и host-тестов). */
err_t blk_mbr_find_fat32(const u8 *buf, u64 *out_lba, u64 *out_sectors)
{
    return mbr_find_fat32(buf, out_lba, out_sectors);
}

err_t storage_init(void)
{
    if (g_blk.ready != 0) {
        return K_ESTATE;
    }

    /* 1. PCI: ищем SATA-контроллер AHCI (class 01h/06h). */
    err_t rc = pci_init();
    if (rc != K_OK) {
        klog(KLOG_ERROR, B_TAG, "pci_init: %s", k_strerror(rc));
        return rc;
    }

    const struct pci_dev *d = pci_find_class(0x01u, 0x06u);
    if (d == NULL) {
        klog(KLOG_WARN, B_TAG, "AHCI-контроллер не найден — хранилище недоступно");
        return K_ENODEV;
    }
    u64 bar5 = d->bar[5] & 0xFFFFFFF0u;   /* MMIO-бар: младшие биты флаги */
    if (bar5 == 0u) {
        klog(KLOG_ERROR, B_TAG, "AHCI: BAR5 нулевой");
        return K_ENODEV;
    }
    rc = pci_enable_mem_busmaster(d);
    if (rc != K_OK) {
        klog(KLOG_ERROR, B_TAG, "AHCI: не включён MEM/BUSMASTER (%s)",
             k_strerror(rc));
        return rc;
    }
    klog(KLOG_INFO, B_TAG,
         "AHCI-контроллер %02x:%02x.%u (%04x:%04x), BAR5=0x%llx",
         (unsigned int)d->bus, (unsigned int)d->dev, (unsigned int)d->func,
         d->vendor, d->device, (unsigned long long)bar5);

    /* 2. AHCI. */
    rc = ahci_init(bar5);
    if (rc != K_OK) {
        klog(KLOG_WARN, B_TAG, "ahci_init: %s", k_strerror(rc));
        return rc;
    }

    u32 mask = ahci_port_mask();
    if (mask == 0u) {
        klog(KLOG_WARN, B_TAG, "нет дисков на портах AHCI");
        return K_ENODEV;
    }
    u8 port = 0xFFu;
    for (u8 p = 0; p < 32u; p++) {
        if (mask & (1u << p)) {
            port = p;
            break;
        }
    }
    if (port == 0xFFu) {
        return K_ENODEV;
    }
    g_blk.port = port;

    char model[41];
    u64 sectors = 0;
    rc = ahci_identify(port, model, &sectors);
    if (rc != K_OK) {
        klog(KLOG_WARN, B_TAG, "IDENTIFY порта %u: %s", port, k_strerror(rc));
        return rc;
    }
    klog(KLOG_INFO, B_TAG,
         "диск: порт %u, \"%s\", %llu МиБ (%llu секторов)",
         port, model,
         (unsigned long long)(sectors >> 11),
         (unsigned long long)sectors);

    /* 3. MBR -> партиция FAT32. */
    u8 mbr[512];
    rc = ahci_read(port, 0u, 1u, mbr);
    if (rc != K_OK) {
        klog(KLOG_WARN, B_TAG, "чтение MBR: %s", k_strerror(rc));
        return rc;
    }
    u64 plba = 0, psects = 0;
    rc = blk_mbr_find_fat32(mbr, &plba, &psects);
    if (rc != K_OK) {
        klog(KLOG_WARN, B_TAG, "FAT32-партиция в MBR не найдена (%s)",
             k_strerror(rc));
        return rc;
    }
    g_blk.part_lba = plba;
    g_blk.part_sectors = psects;
    for (u32 i = 0; i < sizeof(g_blk.model); i++) {
        g_blk.model[i] = 0u;
    }
    for (u32 i = 0; i < sizeof(g_blk.model) - 1u && model[i] != '\0'; i++) {
        g_blk.model[i] = model[i];
    }
    g_blk.disk_sectors = sectors;
    klog(KLOG_INFO, B_TAG,
         "партиция FAT32: LBA %llu, %llu МиБ",
         (unsigned long long)plba,
         (unsigned long long)(psects >> 11));

    /* 4. Оверлей амнезии (выключен по умолчанию). */
    rc = ovl_init(&g_ovl_io, BLK_OVL_BLOCKS);
    if (rc != K_OK) {
        klog(KLOG_WARN, B_TAG, "ovl_init: %s (амнезия недоступна)",
             k_strerror(rc));
    }

    /* 5. FAT32 mount. Дисковый уровень открываем ДО mount: fat_mount
     * сам читает сектора через blk_read_sectors (g_blk.ready должен
     * быть уже выставлен), при неудаче закрываем обратно. */
    g_blk.ready = 1;
    rc = fat_mount(&g_blk.vol, plba, psects);
    if (rc != K_OK) {
        klog(KLOG_WARN, B_TAG, "fat_mount: %s", k_strerror(rc));
        g_blk.ready = 0;
        return rc;
    }
    klog(KLOG_INFO, B_TAG,
         "FAT32: том \"%s\", %llu КиБ свободно, кластер %u секторов",
         g_blk.vol.bpb.label,
         (unsigned long long)(fat_free_bytes(&g_blk.vol) >> 10),
         (unsigned int)g_blk.vol.bpb.sectors_per_cluster);
    return K_OK;
}

struct blk_status blk_get_status(void)
{
    struct blk_status st;
    for (u32 i = 0; i < sizeof(st); i++) {
        ((u8 *)&st)[i] = 0u;
    }
    st.ahci_ok = (g_blk.ready != 0) ? 1 : (ahci_port_mask() != 0u) ? 1 : 0;
    st.disk_ok = (ahci_port_mask() != 0u) ? 1 : 0;
    st.port = g_blk.port;
    st.fat_ok = g_blk.ready;
    st.amnesia = ovl_active();
    st.ovl_blocks = ovl_blocks_used();
    st.ovl_limit = ovl_max_blocks();
    st.rd_ops = ahci_read_ops();
    st.wr_ops = ahci_write_ops();

    for (u32 i = 0; i < sizeof(st.model) - 1u && g_blk.model[i] != '\0'; i++) {
        st.model[i] = g_blk.model[i];
    }
    st.disk_sectors = g_blk.disk_sectors;

    if (g_blk.ready != 0) {
        st.fat_ok = 1;
        st.part_lba = g_blk.part_lba;
        st.part_sectors = g_blk.part_sectors;
        st.fat_total_kb = (u64)g_blk.vol.total_clusters *
                          g_blk.vol.bpb.sectors_per_cluster / 2u;
        st.fat_free_kb = fat_free_bytes(&g_blk.vol) >> 10;
    }
    return st;
}

err_t blk_read_sectors(u64 lba, u32 count, void *buf)
{
    if (g_blk.ready == 0) {
        return K_ENODEV;
    }
    if (count == 0u || count > 8u || buf == NULL) {
        return K_EINVAL;
    }
    return ovl_read(lba, count, buf);
}

err_t blk_write_sectors(u64 lba, u32 count, const void *buf)
{
    if (g_blk.ready == 0) {
        return K_ENODEV;
    }
    if (count == 0u || count > 8u || buf == NULL) {
        return K_EINVAL;
    }
    return ovl_write(lba, count, buf);
}

void blk_amnesia(int on)
{
    if (on != 0) {
        ovl_enable();
    } else {
        ovl_disable();
    }
}

int blk_amnesia_active(void)
{
    return ovl_active();
}

struct fat_vol *blk_fat(void)
{
    return (g_blk.ready != 0) ? &g_blk.vol : NULL;
}

err_t blk_ovl_restore(void)
{
    return ovl_init(&g_ovl_io, BLK_OVL_BLOCKS);
}
