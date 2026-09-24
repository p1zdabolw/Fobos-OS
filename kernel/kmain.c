#include "types.h"
#include "gdt.h"
#include "idt.h"
#include "serial.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "timer.h"
#include "keyboard.h"
#include "mouse.h"
#include "pci.h"
#include "fb.h"
#include "fs.h"
#include "ata.h"
#include "diskfs.h"
#include "rtc.h"
#include "sched.h"
#include "syscall.h"
#include "net.h"
#include "http.h"
#include "speaker.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../lib/i18n.h"
#include "../gui/compositor.h"
#include "../gui/window.h"
#include "../gui/cursor.h"
#include "../gui/font.h"
#include "../gui/desktop.h"
#include "../gui/wallpaper.h"
#include "../gui/settings.h"
#include "../apps/terminal.h"
#include "../apps/files.h"
#include "../apps/notepad.h"
#include "../apps/browser.h"
#include "../apps/photos.h"
#include "../apps/media.h"

static const char HOWTO[] =
    "FOS 0.4 batch scripting howto\n"
    "\n"
    "Scripts are .bat, .cmd or .exe files.\n"
    "Directives: echo, set, goto, pause, exit.\n"
    "Variables are written %NAME%.\n"
    "\n"
    "Keyboard layouts:\n"
    "  Press Shift + Alt to toggle EN / RU.\n"
    "\n"
    "Storage:\n"
    "  Files are persisted to the ATA disk.\n"
    "  Settings survive reboot.\n";

static const char URL_HOME[]   = "about:home";
static const char URL_GOOGLE[] = "google.com";
static const char URL_FOS[]    = "fos.local";
static const char URL_EXAMPLE[]= "http://example.com/";

static void pci_dump(struct pci_device *d) {
    kprintf("PCI %02x:%02x.%x %04x:%04x class=%02x.%02x\n",
            (u32)d->bus, (u32)d->slot, (u32)d->func,
            (u32)d->vendor, (u32)d->device,
            (u32)d->class_code, (u32)d->subclass);
}

static void splash(void) {
    struct fb_info *fb = fb_get();
    if (!fb->back) return;
    fb_fill_rect(0, 0, (int)fb->width, (int)fb->height, 0x101828);
    const char *title = "FOS";
    int tw = font_text_width(title);
    font_draw_string(((int)fb->width - tw) / 2, (int)fb->height / 2 - 20, title, 0xFFFFFF, 0x101828);
    const char *sub = "Fobos Operating System";
    int sw = font_text_width(sub);
    font_draw_string(((int)fb->width - sw) / 2, (int)fb->height / 2 + 8, sub, 0xA8C0D0, 0x101828);
    fb_present();
}

static void idle_task(void) {
    for (;;) __asm__ volatile("hlt");
}

static void rt_task(void) {
    for (;;) {
        sched_wait_period();
    }
}

static void logger_task(void) {
    for (;;) {
        sched_wait_period();
        struct tcb *me = sched_task(sched_current_id());
        struct rtc_time tm;
        rtc_read(&tm);
        kprintf("logger: %04d-%02d-%02d %02d:%02d:%02d  jitter=%u/%u missed=%u\n",
                tm.year, tm.month, tm.day, tm.hour, tm.minute, tm.second,
                me->max_jitter_ms,
                me->jitter_samples ? (u32)(me->total_jitter_ms / me->jitter_samples) : 0,
                (u32)me->periods_missed);
    }
}

static void ui_task(void) {
    for (;;) {
        int mx = mouse_x();
        int my = mouse_y();
        int left = mouse_left();
        int right = mouse_right();

        int consumed = compositor_handle_click(mx, my, left, right);
        if (consumed) {
            desktop_sync_input(left);
            window_sync_input(left);
        } else if (desktop_handle_click(mx, my, left)) {
            window_sync_input(left);
        } else {
            window_handle_mouse(mx, my, left, right);
        }

        net_poll();
        media_tick();

        while (keyboard_has_char()) {
            char c = keyboard_getchar();
            window_handle_key(c);
        }

        compositor_paint();
        sched_sleep(16);
    }
}

void kmain(u64 mb2_info) {
    serial_init();
    serial_write("FOS kernel starting\n");

    gdt_init();
    idt_init();
    pic_remap();

    fb_init(mb2_info);

    pmm_init(mb2_info);
    vmm_init();
    heap_init();

    fb_back_init();

    if (!fb_get()->back) {
        kprintf("FATAL: no usable framebuffer\n");
        for (;;) __asm__ volatile("hlt");
    }

    timer_init(1000);
    rtc_init();
    keyboard_init();
    mouse_init();
    fs_init();
    sched_init();
    syscall_init();
    speaker_init();

    net_init();
    cookie_jar_init();

    pci_scan(pci_dump);

    if (ata_init() == 0) {
        if (dfs_mount() == 0) {
            fs_enable_disk(1);
            fs_load_from_disk();
        }
    } else {
        kprintf("Storage: running in live mode (no ATA)\n");
    }

    font_init();
    compositor_init();
    cursor_init();
    window_init();
    desktop_init();
    wallpaper_init();
    settings_init();
    i18n_init();

    settings_load();

    splash();

    if (!fs_exists("readme.txt")) {
        fs_create("readme.txt", "Welcome to FOS\n", 15);
    }
    if (!fs_exists("howto.txt")) {
        fs_create("howto.txt", HOWTO, sizeof(HOWTO) - 1);
    }

    fs_create("home.url",    URL_HOME,    sizeof(URL_HOME)    - 1);
    fs_create("google.url",  URL_GOOGLE,  sizeof(URL_GOOGLE)  - 1);
    fs_create("fos.url",     URL_FOS,     sizeof(URL_FOS)     - 1);
    fs_create("example.url", URL_EXAMPLE, sizeof(URL_EXAMPLE) - 1);

    desktop_add_icon("home.url");
    desktop_add_icon("google.url");
    desktop_add_icon("fos.url");
    desktop_add_icon("example.url");
    desktop_add_icon("readme.txt");
    desktop_add_icon("howto.txt");

    apps_launch_terminal();

    sched_create_periodic("rt",     rt_task,     0,  100);
    sched_create_periodic("logger", logger_task, 20, 1000);
    sched_create("ui", ui_task, 10);
    sched_create("idle", idle_task, SCHED_IDLE_PRIORITY);

    kprintf("FOS ready. Free: %u KB, storage: %s\n",
            (u32)(pmm_free_bytes() / 1024),
            dfs_available() ? "ATA/DFS" : "live (RAM only)");

    __asm__ volatile("sti");
    sched_start();

    for (;;) __asm__ volatile("hlt");
}