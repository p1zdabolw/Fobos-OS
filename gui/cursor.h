#ifndef FOS_CURSOR_H
#define FOS_CURSOR_H

#include "../kernel/types.h"

#define CURSOR_W 12
#define CURSOR_H 19

void cursor_init(void);
void cursor_draw(int x, int y);

#endif