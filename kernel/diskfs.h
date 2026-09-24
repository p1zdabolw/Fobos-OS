#ifndef FOS_DISKFS_H
#define FOS_DISKFS_H

#include "types.h"

#define DFS_MAGIC        0x53465331u
#define DFS_VERSION      1u
#define DFS_NAME_LEN     32
#define DFS_ENTRY_SIZE   64
#define DFS_DIR_SECTORS  16
#define DFS_MAX_FILES    ((DFS_DIR_SECTORS * 512) / DFS_ENTRY_SIZE)
#define DFS_DATA_START   (1 + DFS_DIR_SECTORS)

int  dfs_mount(void);
int  dfs_format(void);
int  dfs_available(void);

int  dfs_list(void);
const char *dfs_entry_name(int idx);
u32  dfs_entry_size(int idx);
int  dfs_entry_used(int idx);

int  dfs_read(const char *name, void *buf, u32 max, u32 *out_size);
int  dfs_write(const char *name, const void *buf, u32 size);
int  dfs_remove(const char *name);

void dfs_sync(void);

#endif