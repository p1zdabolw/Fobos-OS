#ifndef FOS_WALLPAPER_H
#define FOS_WALLPAPER_H

#include "../kernel/types.h"

enum {
    WALL_SOLID,
    WALL_GRADIENT_V,
    WALL_GRADIENT_H,
    WALL_CHECKER,
    WALL_PLASMA,
    WALL_STARFIELD,
    WALL_DIAGONAL,
    WALL_PHOTO,
    WALL_COUNT
};

void wallpaper_init(void);
void wallpaper_set(int mode);
void wallpaper_set_photo(int photo_index);
int  wallpaper_get(void);
int  wallpaper_photo_index(void);
const char *wallpaper_name(int mode);
void wallpaper_draw(int x, int y, int w, int h);
void wallpaper_launch_settings(void);

#endif