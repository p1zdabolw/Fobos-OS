#ifndef FOS_COMPOSITOR_H
#define FOS_COMPOSITOR_H

#include "../kernel/types.h"

void compositor_init(void);
void compositor_paint(void);
void compositor_mark_dirty(void);
void compositor_toggle_menu(void);
int  compositor_menu_open(void);
void compositor_close_menu(void);
int  compositor_taskbar_h(void);
int  compositor_start_w(void);
int  compositor_handle_click(int mx, int my, int left, int right);

#endif