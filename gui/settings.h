#ifndef FOS_SETTINGS_H
#define FOS_SETTINGS_H

#include "../kernel/types.h"

void settings_init(void);
void settings_launch(void);

int  settings_clock_24h(void);
void settings_set_clock_24h(int on);

int  settings_mouse_speed(void);
void settings_set_mouse_speed(int speed);

#endif