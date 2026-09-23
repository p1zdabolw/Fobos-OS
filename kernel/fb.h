#ifndef FOS_FB_H
#define FOS_FB_H

#include "types.h"

struct fb_info {
    u32  width;
    u32  height;
    u32  pitch;
    u8   bpp;
    u8  *addr;
    u32 *back;
    usize back_pitch;
};

void  fb_init(u64 mb2_info);
void  fb_back_init(void);
struct fb_info *fb_get(void);
void  fb_put_pixel(int x, int y, u32 rgb);
void  fb_fill_rect(int x, int y, int w, int h, u32 rgb);
void  fb_present(void);

#endif