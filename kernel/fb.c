#include "fb.h"
#include "heap.h"
#include "../lib/printf.h"
#include "../lib/mem.h"

static struct fb_info g_fb;

struct mb2_tag { u32 type; u32 size; };
struct mb2_fb {
    u32 type; u32 size; u64 addr; u32 pitch; u32 width; u32 height;
    u8 bpp; u8 fb_type; u16 reserved;
};

void fb_init(u64 mb2_info) {
    u8 *p = (u8*)mb2_info;
    u32 total = *(u32*)p;
    kprintf("MB2 info at %p total=%u\n", (void*)mb2_info, (u32)total);

    struct mb2_tag *tag = (struct mb2_tag*)(p + 8);
    while ((u8*)tag + 8 <= p + total) {
        if (tag->size < 8) {
            kprintf("MB2 tag malformed type=%u size=%u\n", (u32)tag->type, (u32)tag->size);
            break;
        }
        kprintf("MB2 tag type=%u size=%u\n", (u32)tag->type, (u32)tag->size);
        if (tag->type == 0) break;
        if (tag->type == 8) {
            struct mb2_fb *f = (struct mb2_fb*)tag;
            g_fb.addr   = (u8*)(usize)f->addr;
            g_fb.pitch  = f->pitch;
            g_fb.width  = f->width;
            g_fb.height = f->height;
            g_fb.bpp    = f->bpp;
        }
        tag = (struct mb2_tag*)((u8*)tag + ((tag->size + 7) & ~7u));
    }

    if (!g_fb.addr) {
        kprintf("FB: no type-8 tag found in MB2 info\n");
        return;
    }
    kprintf("FB: %ux%u %ubpp pitch=%u addr=%p\n",
            g_fb.width, g_fb.height, (u32)g_fb.bpp, g_fb.pitch, (void*)g_fb.addr);
}

void fb_back_init(void) {
    if (!g_fb.addr || g_fb.back) return;
    g_fb.back_pitch = (usize)g_fb.width * 4;
    g_fb.back = (u32*)kmalloc(g_fb.back_pitch * g_fb.height);
    if (!g_fb.back) {
        kprintf("FB: back buffer allocation failed\n");
        return;
    }
    memset(g_fb.back, 0, g_fb.back_pitch * g_fb.height);
}

struct fb_info *fb_get(void) { return &g_fb; }

static void put_pixel_phys(int x, int y, u32 rgb) {
    if (x < 0 || y < 0 || (u32)x >= g_fb.width || (u32)y >= g_fb.height) return;
    u8 *row = g_fb.addr + (usize)y * g_fb.pitch;
    if (g_fb.bpp == 32) {
        *(u32*)(row + (usize)x * 4) = rgb;
    } else if (g_fb.bpp == 24) {
        row[x * 3 + 0] = (u8)(rgb & 0xFF);
        row[x * 3 + 1] = (u8)((rgb >> 8) & 0xFF);
        row[x * 3 + 2] = (u8)((rgb >> 16) & 0xFF);
    } else if (g_fb.bpp == 16) {
        u16 c = (u16)(((rgb >> 19) & 0x1F) << 11 | ((rgb >> 10) & 0x3F) << 5 | ((rgb >> 3) & 0x1F));
        *(u16*)(row + (usize)x * 2) = c;
    }
}

void fb_put_pixel(int x, int y, u32 rgb) {
    if (!g_fb.back) { put_pixel_phys(x, y, rgb); return; }
    if (x < 0 || y < 0 || (u32)x >= g_fb.width || (u32)y >= g_fb.height) return;
    g_fb.back[(usize)y * (g_fb.back_pitch / 4) + (usize)x] = rgb;
}

void fb_fill_rect(int x, int y, int w, int h, u32 rgb) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            fb_put_pixel(x + i, y + j, rgb);
        }
    }
}

void fb_present(void) {
    if (!g_fb.back) return;
    for (u32 y = 0; y < g_fb.height; y++) {
        u8 *src = (u8*)(g_fb.back + (usize)y * (g_fb.back_pitch / 4));
        u8 *dst = g_fb.addr + (usize)y * g_fb.pitch;
        if (g_fb.bpp == 32) {
            memcpy(dst, src, (usize)g_fb.width * 4);
        } else if (g_fb.bpp == 24) {
            for (u32 x = 0; x < g_fb.width; x++) {
                u32 c = ((u32*)src)[x];
                dst[x*3+0] = (u8)(c & 0xFF);
                dst[x*3+1] = (u8)((c >> 8) & 0xFF);
                dst[x*3+2] = (u8)((c >> 16) & 0xFF);
            }
        } else if (g_fb.bpp == 16) {
            for (u32 x = 0; x < g_fb.width; x++) {
                u32 c = ((u32*)src)[x];
                ((u16*)dst)[x] = (u16)(((c >> 19) & 0x1F) << 11 | ((c >> 10) & 0x3F) << 5 | ((c >> 3) & 0x1F));
            }
        }
    }
}