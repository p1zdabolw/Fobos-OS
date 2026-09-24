#include "fs.h"
#include "heap.h"
#include "diskfs.h"
#include "../lib/string.h"
#include "../lib/mem.h"

struct fs_file {
    char  name[FS_MAX_NAME];
    void *data;
    usize size;
    int   used;
};

static struct fs_file g_files[FS_MAX_FILES];
static int g_disk_enabled = 0;

void fs_init(void) {
    memset(g_files, 0, sizeof(g_files));
}

void fs_enable_disk(int on) {
    g_disk_enabled = on ? 1 : 0;
}

static struct fs_file *find(const char *name) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (g_files[i].used && strcmp(g_files[i].name, name) == 0) return &g_files[i];
    }
    return NULL;
}

static int fs_load_one(const char *name) {
    u32 size = 0;
    u32 cap = 65536;
    u8 *buf = (u8*)kmalloc(cap);
    if (!buf) return -1;
    if (dfs_read(name, buf, cap, &size) != 0) {
        kfree(buf);
        return -1;
    }
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (g_files[i].used) continue;
        strncpy(g_files[i].name, name, FS_MAX_NAME - 1);
        g_files[i].data = buf;
        g_files[i].size = size;
        g_files[i].used = 1;
        return 0;
    }
    kfree(buf);
    return -1;
}

void fs_load_from_disk(void) {
    if (!dfs_available()) return;
    int n = dfs_list();
    for (int i = 0; i < n; i++) {
        const char *name = dfs_entry_name(i);
        if (!name) continue;
        fs_load_one(name);
    }
}

int fs_create(const char *name, const void *data, usize size) {
    if (strlen(name) >= FS_MAX_NAME) return -1;
    if (find(name)) return fs_write(name, data, size);
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!g_files[i].used) {
            strncpy(g_files[i].name, name, FS_MAX_NAME - 1);
            g_files[i].data = kmalloc(size ? size : 1);
            if (!g_files[i].data) return -1;
            if (data) memcpy(g_files[i].data, data, size);
            g_files[i].size = size;
            g_files[i].used = 1;
            if (g_disk_enabled) dfs_write(name, data ? data : "", (u32)size);
            return 0;
        }
    }
    return -1;
}

int fs_write(const char *name, const void *data, usize size) {
    struct fs_file *f = find(name);
    if (!f) return fs_create(name, data, size);
    void *nd = krealloc(f->data, size ? size : 1);
    if (!nd) return -1;
    f->data = nd;
    f->size = size;
    if (data) memcpy(f->data, data, size);
    if (g_disk_enabled) dfs_write(name, data ? data : "", (u32)size);
    return 0;
}

int fs_remove(const char *name) {
    struct fs_file *f = find(name);
    if (!f) return -1;
    kfree(f->data);
    f->used = 0;
    if (g_disk_enabled) dfs_remove(name);
    return 0;
}

void *fs_read(const char *name, usize *out_size) {
    struct fs_file *f = find(name);
    if (!f) return NULL;
    if (out_size) *out_size = f->size;
    return f->data;
}

int fs_exists(const char *name) { return find(name) != NULL; }

int fs_list(char names[][FS_MAX_NAME], int max) {
    int n = 0;
    for (int i = 0; i < FS_MAX_FILES && n < max; i++) {
        if (g_files[i].used) {
            strncpy(names[n], g_files[i].name, FS_MAX_NAME - 1);
            names[n][FS_MAX_NAME - 1] = 0;
            n++;
        }
    }
    return n;
}

void fs_sync(void) {
    if (g_disk_enabled) dfs_sync();
}