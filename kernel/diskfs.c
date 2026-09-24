#include "diskfs.h"
#include "ata.h"
#include "../lib/mem.h"
#include "../lib/string.h"
#include "../lib/printf.h"

struct dfs_super {
    u32 magic;
    u32 version;
    u32 total_sectors;
    u32 data_start;
    u32 next_free;
    u32 file_count;
    u8  reserved[488];
};

struct dfs_entry {
    char name[DFS_NAME_LEN];
    u32  start_sector;
    u32  size_bytes;
    u32  used;
    u32  reserved[7];
};

static struct dfs_super g_super;
static struct dfs_entry g_dir[DFS_MAX_FILES];
static int g_mounted = 0;
static int g_dirty = 0;

static int dfs_read_dir_sector(int i, u8 *buf) {
    return ata_read_sector(1 + (u32)i, buf);
}

static int dfs_write_dir_sector(int i, const u8 *buf) {
    return ata_write_sector(1 + (u32)i, buf);
}

static int dfs_write_super(void) {
    u8 buf[512];
    memset(buf, 0, 512);
    memcpy(buf, &g_super, sizeof(g_super));
    return ata_write_sector(0, buf);
}

static int dfs_load_dir(void) {
    u8 buf[512];
    for (int s = 0; s < DFS_DIR_SECTORS; s++) {
        if (dfs_read_dir_sector(s, buf) != 0) return -1;
        u32 entries_per_sector = 512 / DFS_ENTRY_SIZE;
        for (u32 e = 0; e < entries_per_sector; e++) {
            u32 idx = (u32)s * entries_per_sector + e;
            if (idx >= DFS_MAX_FILES) break;
            memcpy(&g_dir[idx], buf + e * DFS_ENTRY_SIZE, DFS_ENTRY_SIZE);
        }
    }
    return 0;
}

static int dfs_save_dir(void) {
    u8 buf[512];
    u32 entries_per_sector = 512 / DFS_ENTRY_SIZE;
    for (int s = 0; s < DFS_DIR_SECTORS; s++) {
        memset(buf, 0, 512);
        for (u32 e = 0; e < entries_per_sector; e++) {
            u32 idx = (u32)s * entries_per_sector + e;
            if (idx >= DFS_MAX_FILES) break;
            memcpy(buf + e * DFS_ENTRY_SIZE, &g_dir[idx], DFS_ENTRY_SIZE);
        }
        if (dfs_write_dir_sector(s, buf) != 0) return -1;
    }
    return 0;
}

static struct dfs_entry *dfs_find(const char *name) {
    for (int i = 0; i < DFS_MAX_FILES; i++) {
        if (!g_dir[i].used) continue;
        if (strcmp(g_dir[i].name, name) == 0) return &g_dir[i];
    }
    return (struct dfs_entry*)0;
}

static struct dfs_entry *dfs_find_free(void) {
    for (int i = 0; i < DFS_MAX_FILES; i++) {
        if (!g_dir[i].used) return &g_dir[i];
    }
    return (struct dfs_entry*)0;
}

int dfs_available(void) { return g_mounted; }

int dfs_format(void) {
    if (!ata_available()) return -1;
    if (ata_sector_count() < DFS_DATA_START + 8) return -1;

    memset(&g_super, 0, sizeof(g_super));
    g_super.magic = DFS_MAGIC;
    g_super.version = DFS_VERSION;
    g_super.total_sectors = ata_sector_count();
    g_super.data_start = DFS_DATA_START;
    g_super.next_free = DFS_DATA_START;
    g_super.file_count = 0;

    memset(g_dir, 0, sizeof(g_dir));

    if (dfs_write_super() != 0) return -1;
    if (dfs_save_dir() != 0) return -1;

    g_mounted = 1;
    kprintf("DFS: formatted %u MB disk\n", g_super.total_sectors / 2048);
    return 0;
}

int dfs_mount(void) {
    if (!ata_available()) return -1;

    u8 buf[512];
    if (ata_read_sector(0, buf) != 0) return -1;
    memcpy(&g_super, buf, sizeof(g_super));

    if (g_super.magic != DFS_MAGIC || g_super.version != DFS_VERSION) {
        kprintf("DFS: no filesystem, formatting\n");
        return dfs_format();
    }

    if (dfs_load_dir() != 0) return -1;

    int n = 0;
    for (int i = 0; i < DFS_MAX_FILES; i++) if (g_dir[i].used) n++;

    g_mounted = 1;
    kprintf("DFS: mounted, %d files, next free sector %u\n", n, g_super.next_free);
    return 0;
}

int dfs_list(void) {
    int n = 0;
    for (int i = 0; i < DFS_MAX_FILES; i++) if (g_dir[i].used) n++;
    return n;
}

const char *dfs_entry_name(int idx) {
    int n = 0;
    for (int i = 0; i < DFS_MAX_FILES; i++) {
        if (!g_dir[i].used) continue;
        if (n == idx) return g_dir[i].name;
        n++;
    }
    return (const char*)0;
}

u32 dfs_entry_size(int idx) {
    int n = 0;
    for (int i = 0; i < DFS_MAX_FILES; i++) {
        if (!g_dir[i].used) continue;
        if (n == idx) return g_dir[i].size_bytes;
        n++;
    }
    return 0;
}

int dfs_entry_used(int idx) {
    int n = 0;
    for (int i = 0; i < DFS_MAX_FILES; i++) {
        if (!g_dir[i].used) continue;
        if (n == idx) return 1;
        n++;
    }
    return 0;
}

int dfs_read(const char *name, void *buf, u32 max, u32 *out_size) {
    if (!g_mounted) return -1;
    struct dfs_entry *e = dfs_find(name);
    if (!e) return -1;

    u32 size = e->size_bytes;
    if (size > max) size = max;
    u32 left = size;
    u32 lba = e->start_sector;
    u8 *p = (u8*)buf;
    while (left > 0) {
        u8 sec[512];
        if (ata_read_sector(lba, sec) != 0) return -1;
        u32 chunk = left > 512 ? 512 : left;
        memcpy(p, sec, chunk);
        p += chunk;
        left -= chunk;
        lba++;
    }
    if (out_size) *out_size = e->size_bytes;
    return 0;
}

int dfs_write(const char *name, const void *buf, u32 size) {
    if (!g_mounted) return -1;
    if (strlen(name) >= DFS_NAME_LEN) return -1;

    struct dfs_entry *e = dfs_find(name);
    if (!e) {
        e = dfs_find_free();
        if (!e) return -1;
        memset(e, 0, sizeof(*e));
        strncpy(e->name, name, DFS_NAME_LEN - 1);
        e->used = 1;
        e->start_sector = g_super.next_free;
        e->size_bytes = 0;
        g_super.file_count++;
    }

    u32 needed = (size + 511) / 512;
    u32 allocated = (e->size_bytes + 511) / 512;

    if (needed > allocated) {
        u32 free_sectors = g_super.total_sectors - g_super.next_free;
        if (needed > free_sectors) return -1;
        if (e->size_bytes == 0) {
            e->start_sector = g_super.next_free;
        } else {
            e->start_sector = g_super.next_free;
        }
        g_super.next_free += needed;
        e->size_bytes = size;
    } else {
        e->size_bytes = size;
    }

    u32 left = size;
    u32 lba = e->start_sector;
    const u8 *p = (const u8*)buf;
    while (left > 0) {
        u8 sec[512];
        memset(sec, 0, 512);
        u32 chunk = left > 512 ? 512 : left;
        memcpy(sec, p, chunk);
        if (ata_write_sector(lba, sec) != 0) return -1;
        p += chunk;
        left -= chunk;
        lba++;
    }

    if (dfs_save_dir() != 0) return -1;
    if (dfs_write_super() != 0) return -1;
    g_dirty = 0;
    return 0;
}

int dfs_remove(const char *name) {
    if (!g_mounted) return -1;
    struct dfs_entry *e = dfs_find(name);
    if (!e) return -1;
    memset(e, 0, sizeof(*e));
    g_super.file_count--;
    if (dfs_save_dir() != 0) return -1;
    if (dfs_write_super() != 0) return -1;
    return 0;
}

void dfs_sync(void) {
    if (!g_mounted) return;
    dfs_save_dir();
    dfs_write_super();
}