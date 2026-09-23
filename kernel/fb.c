#include "fb.h"
#include "pmm.h"
#include "../lib/printf.h"
#include "../lib/mem.h"

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF
#define VBE_DISPI_INDEX_ID     0
#define VBE_DISPI_INDEX_XRES   1
#define VBE_DISPI_INDEX_YRES   2
#define VBE_DISPI_INDEX_BPP    3
#define VBE_DISPI_INDEX_ENABLE 4

#define VBE_DISPI_DISABLED     0x00
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_LFB_ENABLED  0x40

static struct fb_info g_fb;
static phys_t g_back_phys = 0;
static usize  g_back_frames = 0;

struct mb2_tag { u32 type; u32 size; };
struct mb2_fb {
    u32 type; u32 size; u64 addr; u32 pitch; u32 width; u32 height;
    u8 bpp; u8 fb_type; u16 reserved;
};

static inline void outw_v(u16 p, u16 v) {
    __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p));
}
static inline u16 inw_v(u16 p) {
    u16 v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}

static void vbe_write(u16 idx, u16 val) {
    outw_v(VBE_DISPI_IOPORT_INDEX, idx);
    outw_v(VBE_DISPI_IOPORT_DATA, val);
}
static u16 vbe_read(u16 idx) {
    outw_v(VBE_DISPI_IOPORT_INDEX, idx);
    return inw_v(VBE_DISPI_IOPORT_DATA);
}

int fb_vbe_available(void) {
    u16 id = vbe_read(VBE_DISPI_INDEX_ID);
    return (id >= 0xB0C0 && id <= 0xB0C5);
}

static int vbe_set_mode(int w, int h, int bpp) {
    if (!fb_vbe_available()) return -1;
    if (w <= 0 || h <= 0) return -1;
    if (bpp != 32) return -1;

    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    vbe_write(VBE_DISPI_INDEX_XRES, (u16)w);
    vbe_write(VBE_DISPI_INDEX_YRES, (u16)h);
    vbe_write(VBE_DISPI_INDEX_BPP, (u16)bpp);
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    u16 got_x = vbe_read(VBE_DISPI_INDEX_XRES);
    u16 got_y = vbe_read(VBE_DISPI_INDEX_YRES);
    if (got_x != (u16)w || got_y != (u16)h) return -1;
    return 0;
}

static void alloc_back_buffer(void) {
    usize needed = (usize)g_fb.width * (usize)g_fb.height * 4;
    if (needed == 0) return;
    g_back_frames = (needed + 4095) / 4096;
    g_back_phys = pmm_alloc_frames(g_back_frames);
    if (!g_back_phys) {
        g_fb.back = (u32*)0;
        g_fb.back_pitch = 0;
        return;
    }
    g_fb.back = (u32*)g_back_phys;
    g_fb.back_pitch = (usize)g_fb.width * 4;
    memset(g_fb.back, 0, needed);
}

void fb_init(u64 mb2_info) {
    u8 *p = (u8*)mb2_info;
    u32 total = *(u32*)p;
    struct mb2_tag *tag = (struct mb2_tag*)(p + 8);
    while ((u8*)tag + 8 <= p + total) {
        if (tag->size < 8) break;
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
        kprintf("FB: no framebuffer tag present\n");
        return;
    }
    kprintf("FB: %ux%u %ubpp pitch=%u addr=%p (vbe=%s)\n",
            g_fb.width, g_fb.height, (u32)g_fb.bpp, g_fb.pitch,
            (void*)g_fb.addr, fb_vbe_available() ? "yes" : "no");
}

void fb_back_init(void) {
    if (!g_fb.addr || g_fb.back) return;
    alloc_back_buffer();
}

int fb_resize(int w, int h) {
    if (!g_fb.addr) return -1;
    if (!fb_vbe_available()) return -1;

    if (vbe_set_mode(w, h, 32) != 0) return -1;

    if (g_back_phys && g_back_frames) {
        pmm_free_frames(g_back_phys, g_back_frames);
        g_back_phys = 0;
        g_back_frames = 0;
        g_fb.back = (u32*)0;
    }

    g_fb.width  = (u32)w;
    g_fb.height = (u32)h;
    g_fb.bpp    = 32;
    g_fb.pitch  = (u32)w * 4;

    alloc_back_buffer();
    if (!g_fb.back) return -1;
    return 0;
}

u32 fb_bytes_used(void) {
    return (u32)g_back_frames * 4096;
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