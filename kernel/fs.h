#ifndef FOS_FS_H
#define FOS_FS_H

#include "types.h"

#define FS_MAX_FILES 64
#define FS_MAX_NAME  32

void  fs_init(void);
int   fs_create(const char *name, const void *data, usize size);
int   fs_write(const char *name, const void *data, usize size);
int   fs_remove(const char *name);
void *fs_read(const char *name, usize *out_size);
int   fs_exists(const char *name);
int   fs_list(char names[][FS_MAX_NAME], int max);
void fs_enable_disk(int on);
void fs_load_from_disk(void);
void fs_sync(void);

#endif