#ifndef FOS_DESKTOP_H
#define FOS_DESKTOP_H

#include "../kernel/types.h"

#define ICON_TYPE_UNKNOWN 0
#define ICON_TYPE_TXT     1
#define ICON_TYPE_EXE     2
#define ICON_TYPE_CMD     3
#define ICON_TYPE_URL     4

#define MAX_DESKTOP_ICONS 32

void desktop_init(void);
void desktop_paint(void);
int  desktop_handle_click(int mx, int my, int left);
void desktop_sync_input(int left);
void desktop_add_icon(const char *filename);
void desktop_remove_icon(const char *filename);
int  desktop_icon_count(void);
void desktop_reclamp(void);

#endif