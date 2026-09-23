#ifndef FOS_KEYBOARD_H
#define FOS_KEYBOARD_H

#include "types.h"

#define KEY_NONE   0
#define KEY_ESC    0x01
#define KEY_ENTER  0x1C
#define KEY_BS     0x0E
#define KEY_TAB    0x0F
#define KEY_LSHIFT 0x2A
#define KEY_RSHIFT 0x36
#define KEY_CTRL   0x1D
#define KEY_ALT    0x38

#define LAYOUT_EN  0
#define LAYOUT_RU  1

void keyboard_init(void);
int  keyboard_has_char(void);
char keyboard_getchar(void);
int  keyboard_has_key(void);
int  keyboard_get_key(void);
int  keyboard_shift_down(void);

int  keyboard_get_layout(void);
void keyboard_set_layout(int layout);
const char *keyboard_layout_name(void);

#endif