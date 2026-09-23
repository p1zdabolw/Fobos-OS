#ifndef FOS_APP_PHOTOS_H
#define FOS_APP_PHOTOS_H

#include "../kernel/types.h"

void apps_launch_photos(void);
void photos_render(int index, u32 *out, int w, int h);
int  photos_count(void);

#endif