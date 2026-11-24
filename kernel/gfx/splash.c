/*
 * Fobos OS — реализация splash (см. splash.h).
 */
#include "splash.h"

#include "uidraw.h"
#include "../version.h"
#include "../drivers/fb.h"
#include "../drivers/timer.h"
#include "../drivers/kbd.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../lib/vsnprintf.h"

static const char *const SP_TAG = "gfx/splash";

int splash_wait(u32 window_ms)
{
    int fb_ok = (uidraw_width() != 0u && uidraw_height() != 0u);

    if (fb_ok != 0) {
        uidraw_background();

        u32 w = uidraw_width();
        u32 h = uidraw_height();

        /* Логотип: акцентный квадрат НАД заголовком (не пересекается). */
        u32 logo_y = h / 3u;
        u32 sq_x = (w / 2u) - 8u;
        (void)fb_fill_rect(sq_x, logo_y, 16u, 16u, UI_COL_ACCENT);
        (void)fb_fill_rect(sq_x + 4u, logo_y + 4u, 8u, 8u, UI_COL_BG_TOP);

        char title[64];
        (void)ksnprintf(title, sizeof(title), "FOBOS OS %s", FOBOS_VERSION_STR);
        uidraw_text_centered(logo_y + 24u, title, UI_COL_TX, UI_COL_BG_TOP);

        u32 hy = logo_y + 48u;
        uidraw_text_centered(hy, "Hold SHIFT now for the kernel console",
                             UI_COL_HEADER_TX, UI_COL_BG_TOP);
        uidraw_text_centered(hy + 12u, "(otherwise Fobos starts normally)",
                             UI_COL_HEADER_TX, UI_COL_BG_TOP);
        fb_present();   /* splash рисуется вне main_loop — кадр сразу на экран */
    }

    klog(KLOG_INFO, SP_TAG,
         "[boot] splash: %u ms (hold SHIFT for kernel console)", window_ms);
    klog(KLOG_INFO, SP_TAG, "splash: mods at entry=0x%02x", kbd_mods());

    u64 deadline = timer_uptime_ms() + window_ms;
    u32 last_sec = 0xFFFFFFFFu;
    struct kbd_event ev;

    for (;;) {
        while (kbd_poll(&ev) == K_OK) {
            /* События в окне не влияют на последующие экраны — гасим. */
        }
        if ((kbd_mods() & KBD_FL_SHIFT) != 0u) {
            klog(KLOG_INFO, SP_TAG, "[boot] console: shift detected");
            return 1;
        }
        if (timer_uptime_ms() >= deadline) {
            return 0;
        }
        if (fb_ok != 0) {
            u64 left = deadline - timer_uptime_ms();
            u32 sec = (u32)(left / 1000u) + 1u;
            if (sec != last_sec) {
                last_sec = sec;
                char line[48];
                (void)ksnprintf(line, sizeof(line),
                                "starting in %u second(s)...", (unsigned int)sec);
                uidraw_text_centered(uidraw_height() / 3u + 72u, line,
                                     UI_COL_TX_DIM, UI_COL_BG_TOP);
                fb_present();
            }
        }
        timer_sleep_ms(10);
    }
}
