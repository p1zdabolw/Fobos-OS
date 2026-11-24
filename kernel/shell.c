/*
 * Fobos OS — реализация ядерной оболочки.
 *
 * Строка: до 96 символов, backspace, Ctrl+C (сброс строки), Ctrl+L (очистка).
 * Разбор: split по пробелам, до SHELL_MAX_ARGS аргументов.
 * Вывод: дублируется в консоль (экран) и klog (serial/CI).
 *
 * Контекст сессии: в kernel console приглашение "fobos(kconsole)>";
 * в сессии пользователя — "<user>@<hostname>:~$" (как в классических
 * POSIX-оболочках). Команды users/useradd/passwd/logout зависят от сессии.
 */
#include "shell.h"

#include <fobos/errors.h>

#include "gfx/console.h"
#include "gfx/ui.h"
#include "gfx/desktop.h"
#include "fs/blk.h"
#include "fs/fat32.h"
#include "drivers/fb.h"
#include "drivers/timer.h"
#include "drivers/kbd.h"
#include "drivers/mouse.h"
#include "drivers/ps2.h"
#include "drivers/rtc.h"
#include "mm/phys.h"
#include "mm/vmm.h"
#include "sys/users.h"
#include "sys/session.h"
#include "sys/power.h"
#include "sys/task.h"
#include "sys/syscall.h"
#include "sys/elf.h"
#include "version.h"
#include "lib/log.h"
#include "lib/string.h"
#include "lib/vsnprintf.h"
#include "selftest/selftest.h"
#include "lib/io.h"

#define SHELL_LINE_MAX  96u
#define SHELL_MAX_ARGS   8u
#define SHELL_OUT_BUF  256u

typedef int (*cmd_fn)(int argc, char **argv);

struct cmd_entry {
    const char *name;
    cmd_fn fn;
};

struct shell_state {
    char line[SHELL_LINE_MAX];
    u32 len;
    int ready;
    int logout_requested;
};

static struct shell_state g_sh;
static const char *const SH_TAG = "shell";

/* Вывод на экран + зеркалирование в serial через klog (для CI). */
static void sh_out(const char *fmt, ...)
{
    char buf[SHELL_OUT_BUF];
    va_list ap;
    va_start(ap, fmt);
    (void)kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    con_write(buf);
    con_write("\n");
}

/* Только в serial (машиночитаемые строки для qemu_selftest). */
static void sh_log(const char *fmt, ...)
{
    char buf[SHELL_OUT_BUF];
    va_list ap;
    va_start(ap, fmt);
    (void)kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    klog(KLOG_INFO, SH_TAG, "%s", buf);
}

/* ------------------------------------------------------------------ */
/* Команды                                                             */
/* ------------------------------------------------------------------ */

static int cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sh_out("Fobos kshell %s commands:", FOBOS_VERSION_STR);
    sh_out("  help      - this list");
    sh_out("  about     - version and build stage");
    sh_out("  uptime    - time since boot");
    sh_out("  date      - current date/time (RTC)");
    sh_out("  ticks     - PIT tick counter");
    sh_out("  mem       - physical memory state");
    sh_out("  whoami    - current session user");
    sh_out("  hostname  - show/set system hostname");
    sh_out("  users     - list user accounts");
    sh_out("  useradd N - add user N (admin session only)");
    sh_out("  passwd    - change own password");
    sh_out("  logout    - end session (returns to login screen)");
    sh_out("  kbd       - keyboard diagnostics");
    sh_out("  mouse     - mouse diagnostics");
    sh_out("  fbtest    - framebuffer test (any key to exit)");
    sh_out("  font      - font glyph table");
    sh_out("  condump N - dump N console rows to serial");
    sh_out("  selftest  - rerun kselftest");
    sh_out("  echo ...  - print arguments");
    sh_out("  fos == gui|tui [--lock]");
    sh_out("            - desktop mode: gui desktop or text console");
    sh_out("  amnezia [on|off|status]");
    sh_out("            - amnesia mode: writes to RAM only until reboot");
    sh_out("  storage   - disk subsystem status (AHCI/FAT32/amnesia)");
    sh_out("  ls [path] - list FAT32 directory");
    sh_out("  cat FILE  - print file content (first 4 KiB)");
    sh_out("  write FILE [text] - create/overwrite text file");
    sh_out("  rm FILE   - delete file");
    sh_out("  cp SRC DST - copy file (up to 64 KiB)");
    sh_out("  sleep MS  - pause for MS milliseconds (max 60000)");
    sh_out("  notepad   - open Notepad (GUI)");
    sh_out("  calc      - open Calculator (GUI)");
    sh_out("  settings  - open Settings (GUI)");
    sh_out("  start     - toggle the Start menu (GUI)");
    sh_out("  apps      - list installed apps");
    sh_out("  pins      - list taskbar pins");
    sh_out("  clear     - clear the screen");
    sh_out("  run NAME  - start user program \\fobos\\NAME.ELF (FND-3a)");
    sh_out("  ps        - process/scheduler diagnostics");
    sh_out("  crash     - controlled kernel panic test");
    sh_out("  reboot    - reboot the machine");
    sh_out("  poweroff  - power off the machine");
    return 0;
}

static int cmd_about(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sh_out("Fobos OS %s", FOBOS_VERSION_STR);
    sh_out("Kernel: Fobos kernel, UEFI FBPI v0.1, x86_64 long mode");
    sh_out("Stage: %s", FOBOS_STAGE_STR);
    if (session_active() != 0) {
        sh_out("Session: %s@%s (admin=%d)", session_user(),
               session_hostname(), session_is_admin());
    } else {
        sh_out("Session: none (kernel console)");
    }
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    u64 ms = timer_uptime_ms();
    u32 sec = (u32)((ms / 1000ull) % 60ull);
    u32 min = (u32)((ms / 60000ull) % 60ull);
    u32 hr = (u32)(ms / 3600000ull);
    sh_out("up %u:%02u:%02u (%llu ms)", hr, min, sec, (unsigned long long)ms);
    return 0;
}

static int cmd_date(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct rtc_time t;
    err_t rc = rtc_read_time(&t);
    if (rc != K_OK) {
        sh_out("date: RTC unavailable (%s)", k_strerror(rc));
        return 1;
    }
    char buf[RTC_STR_MIN];
    if (rtc_format(buf, sizeof(buf), &t) < 0) {
        sh_out("date: RTC returned invalid time");
        return 1;
    }
    sh_out("%s", buf);
    return 0;
}

static int cmd_ticks(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sh_out("ticks=%llu hz=%u", (unsigned long long)timer_get_ticks(),
           timer_get_hz());
    return 0;
}

static int cmd_mem(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct phys_stats st = phys_get_stats();
    u64 total = st.total_frames * (u64)PAGE_SIZE;
    u64 free_b = st.free_frames * (u64)PAGE_SIZE;
    u64 reserved = st.reserved_frames * (u64)PAGE_SIZE;
    sh_out("total=%llu MiB free=%llu MiB reserved=%llu MiB",
           (unsigned long long)(total >> 20),
           (unsigned long long)(free_b >> 20),
           (unsigned long long)(reserved >> 20));
    sh_log("mem: total_mib=%llu free_mib=%llu",
           (unsigned long long)(total >> 20),
           (unsigned long long)(free_b >> 20));
    return 0;
}

static int cmd_whoami(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (session_active() != 0) {
        sh_out("%s", session_user());
        sh_log("whoami=%s", session_user());
    } else {
        sh_out("maintenance");
        sh_log("whoami=maintenance");
    }
    return 0;
}

static int cmd_hostname(int argc, char **argv)
{
    if (argc >= 2) {
        if (foboscfg_valid_hostname(argv[1]) == 0) {
            sh_out("hostname: invalid name '%s' ([a-z0-9-], 1..31)", argv[1]);
            return 1;
        }
        session_set_hostname(argv[1]);
        sh_log("hostname: set to %s", argv[1]);
        sh_out("%s", session_hostname());
        return 0;
    }
    sh_out("%s", session_hostname());
    return 0;
}

static int cmd_users(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    u32 n = users_count();
    if (n == 0u) {
        sh_out("no users (setup not completed)");
        return 0;
    }
    sh_out("%u user(s):", (unsigned int)n);
    for (u32 i = 0; i < n; i++) {
        sh_out("  %s%s", users_name(i),
               (users_is_admin(i) == 1) ? "  (admin)" : "");
        sh_log("users-list: %s", users_name(i));
    }
    return 0;
}

static int cmd_useradd(int argc, char **argv)
{
    if (session_is_admin() != 1) {
        sh_out("useradd: permission denied (admin session required)");
        return 1;
    }
    if (argc < 2) {
        sh_out("usage: useradd <name>");
        return 1;
    }
    err_t rc = users_add(argv[1], "fobos123", 0);
    if (rc != K_OK) {
        sh_out("useradd: %s (invalid/duplicate name?)", k_strerror(rc));
        return 1;
    }
    sh_out("user '%s' created with default password 'fobos123' (change it: passwd)",
           argv[1]);
    err_t prc = users_persist();
    if (prc != K_OK) {
        sh_out("note: not persisted (%s)", k_strerror(prc));
    }
    sh_log("useradd: %s", argv[1]);
    return 0;
}

/* Читает строку с клавиатуры с маскированием (для passwd). */
static int read_line_masked(char *dst, u32 cap)
{
    u32 len = 0;
    struct kbd_event ev;
    for (;;) {
        if (kbd_wait(&ev) != K_OK) {
            return -1;
        }
        if (ev.code == KEY_ENTER || ev.ch == '\n' || ev.ch == '\r') {
            (void)con_write("\n");
            dst[len] = '\0';
            return (int)len;
        }
        if (ev.code == KEY_BSPACE || ev.ch == '\b') {
            if (len > 0u) {
                len--;
                (void)con_write("\b \b");
            }
            continue;
        }
        if (ev.ch >= 0x20 && ev.ch <= 0x7E) {
            if (len + 1u < cap) {
                dst[len++] = ev.ch;
                (void)con_write("*");
            }
            continue;
        }
        if ((ev.flags & KBD_FL_CTRL) != 0u && (ev.ch == 'c' || ev.ch == 'C')) {
            (void)con_write("^C\n");
            return -1;
        }
    }
}

static int cmd_passwd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (session_active() == 0) {
        sh_out("passwd: no session (kernel console has no password)");
        return 1;
    }
    char pw1[USERS_PW_MAX];
    char pw2[USERS_PW_MAX];

    sh_out("new password:");
    if (read_line_masked(pw1, sizeof(pw1)) <= 0) {
        sh_out("passwd: aborted");
        return 1;
    }
    sh_out("confirm password:");
    if (read_line_masked(pw2, sizeof(pw2)) <= 0) {
        sh_out("passwd: aborted");
        return 1;
    }
    if (strcmp(pw1, pw2) != 0) {
        sh_out("passwd: passwords do not match");
        return 1;
    }
    err_t rc = users_set_password(session_user(), pw1);
    if (rc != K_OK) {
        sh_out("passwd: %s", k_strerror(rc));
        return 1;
    }
    sh_out("password changed");
    sh_log("passwd: changed for %s", session_user());
    err_t prc = users_persist();
    if (prc != K_OK) {
        sh_out("note: not persisted (%s)", k_strerror(prc));
    }
    return 0;
}

static int cmd_logout(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (session_active() == 0) {
        sh_out("logout: not in a user session");
        return 1;
    }
    sh_log("logout: %s", session_user());
    g_sh.logout_requested = 1;
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    con_clear();
    return 0;
}

static int cmd_kbd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct kbd_stats st = kbd_get_stats();
    u8 mods = kbd_mods();
    sh_out("events: pushed=%u dropped=%u unmapped=%u queue=%u",
           st.pushed, st.dropped, st.unmapped, st.queue_used);
    sh_out("mods: shift=%d ctrl=%d alt=%d caps=%d",
           (mods & KBD_FL_SHIFT) ? 1 : 0,
           (mods & KBD_FL_CTRL) ? 1 : 0,
           (mods & KBD_FL_ALT) ? 1 : 0,
           (mods & KBD_FL_CAPS) ? 1 : 0);
    sh_log("kbd: pushed=%u dropped=%u unmapped=%u",
           st.pushed, st.dropped, st.unmapped);
    return 0;
}

static int cmd_mouse(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct mouse_stats st = mouse_get_stats();
    u32 mx = 0, my = 0;
    ui_mouse_pos(&mx, &my);
    sh_out("pos: %u,%u", mx, my);
    sh_out("buttons: L=%d R=%d M=%d",
           (mouse_buttons() & MOUSE_BTN_LEFT) ? 1 : 0,
           (mouse_buttons() & MOUSE_BTN_RIGHT) ? 1 : 0,
           (mouse_buttons() & MOUSE_BTN_MIDDLE) ? 1 : 0);
    sh_out("packets: ok=%u bad=%u dropped=%u queue=%u",
           st.packets_ok, st.packets_bad, st.dropped, st.queue_used);
    sh_log("mouse: x=%u y=%u buttons=%u ok=%u bad=%u",
           mx, my, mouse_buttons(), st.packets_ok, st.packets_bad);
    return 0;
}

static int cmd_fbtest(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    err_t rc = fb_demo_pattern();
    if (rc != K_OK) {
        sh_out("fbtest: error %s", k_strerror(rc));
        return 1;
    }
    sh_out("fbtest: pattern shown, press any key to return...");
    struct kbd_event ev;
    (void)kbd_wait(&ev);
    ui_redraw_all();
    sh_out("fbtest: restored");
    sh_log("fbtest: done");
    return 0;
}

static int cmd_font(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (int row = 0; row < 6; row++) {
        char line[17];
        for (int col = 0; col < 16; col++) {
            int c = 0x20 + row * 16 + col;
            line[col] = (c <= 0x7E) ? (char)c : ' ';
        }
        line[16] = '\0';
        sh_out("%s", line);
    }
    sh_out("95 glyphs total, ASCII 0x20..0x7E");
    return 0;
}

static int cmd_condump(int argc, char **argv)
{
    u32 rows = con_rows();
    if (argc >= 2) {
        u32 v = 0;
        const char *s = argv[1];
        while (*s >= '0' && *s <= '9') {
            v = v * 10u + (u32)(*s - '0');
            s++;
        }
        if (v > 0u) {
            rows = v;
        }
    }
    con_debug_dump(rows);
    sh_out("condump: %u rows dumped to serial", rows);
    return 0;
}

static int cmd_selftest(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sh_out("kselftest: rerunning...");
    int failed = kselftest_run();
    sh_out("kselftest: %s (%d failed)", (failed == 0) ? "OK" : "FAIL", failed);
    return (failed == 0) ? 0 : 1;
}

static int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        con_write(argv[i]);
        if (i + 1 < argc) {
            con_write(" ");
        }
    }
    con_write("\n");
    return 0;
}

static int cmd_crash(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "confirm") != 0) {
        sh_out("crash: deliberate kernel panic. Confirmation required:");
        sh_out("  crash confirm");
        return 1;
    }
    sh_log("crash: controlled panic requested");
    klog_panic(SH_TAG, "контролируемая паника по команде оператора (crash confirm)");
    /* klog_panic не возвращается */
}


/* ------------------------------------------------------------------ */
/* fos: режим рабочего стола (tui/gui) + фиксация                      */
/* ------------------------------------------------------------------ */

static int cmd_fos(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        const char *cur = (dsk_mode() == DSK_GUI) ? "gui" : "tui";
        int locked_mode = -1;
        err_t lrc = dsk_read_lock(&locked_mode);
        if (lrc == K_OK) {
            sh_out("mode=%s (session) locked=%s", cur,
                   (locked_mode == DSK_GUI) ? "gui" : "tui");
        } else if (lrc == K_ENOENT) {
            sh_out("mode=%s (session) locked=none", cur);
        } else {
            sh_out("mode=%s (session) locked=unknown (%s)", cur,
                   k_strerror(lrc));
        }
        sh_log("fos: mode=%s", cur);
        return 0;
    }

    if (argc < 3 || strcmp(argv[1], "==") != 0) {
        sh_out("usage: fos == gui|tui [--lock]");
        sh_out("       fos status");
        return 1;
    }
    int mode;
    if (strcmp(argv[2], "gui") == 0) {
        mode = DSK_GUI;
    } else if (strcmp(argv[2], "tui") == 0) {
        mode = DSK_TUI;
    } else {
        sh_out("fos: unknown mode '%s' (gui|tui)", argv[2]);
        return 1;
    }

    int locked = 0;
    if (argc >= 4 && strcmp(argv[3], "--lock") == 0) {
        locked = 1;
    } else if (argc >= 4) {
        sh_out("fos: unknown option '%s'", argv[3]);
        return 1;
    }

    if (locked != 0) {
        err_t lrc = dsk_lock_mode(mode);
        if (lrc != K_OK) {
            sh_out("fos: cannot lock (%s) - storage required", k_strerror(lrc));
            sh_log("fos: lock failed (%s)", k_strerror(lrc));
            return 1;
        }
    }

    err_t rc = dsk_switch(mode);
    if (rc != K_OK) {
        sh_out("fos: switch failed (%s)", k_strerror(rc));
        return 1;
    }
    sh_log("fos: mode=%s locked=%d", (mode == DSK_GUI) ? "gui" : "tui", locked);
    return 0;
}

/* ------------------------------------------------------------------ */
/* amnezia: режим амнезии (запись только в RAM до перезагрузки)        */
/* ------------------------------------------------------------------ */

static int cmd_amnezia(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        if (blk_fat() == NULL) {
            sh_out("amnezia: no storage device");
            return 1;
        }
        if (blk_amnesia_active() != 0) {
            struct blk_status st = blk_get_status();
            sh_out("amnesia: ON, %u/%u blocks used",
                   (unsigned int)st.ovl_blocks, (unsigned int)st.ovl_limit);
        } else {
            sh_out("amnesia: off");
        }
        sh_log("amnezia: state=%d", blk_amnesia_active());
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        if (blk_amnesia_active() == 0) {
            sh_out("amnezia: already off");
            return 0;
        }
        blk_amnesia(0);
        sh_out("amnesia disabled: all session changes discarded");
        sh_log("amnezia: off");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "on") != 0) {
        sh_out("usage: amnezia [on|off|status]");
        return 1;
    }
    if (blk_fat() == NULL) {
        sh_out("amnezia: no storage device");
        sh_log("amnezia: failed (no storage)");
        return 1;
    }
    if (blk_amnesia_active() != 0) {
        sh_out("amnezia: already ON");
        sh_log("amnezia: on");
        return 0;
    }
    blk_amnesia(1);
    sh_out("AMNESIA MODE ON: writes go to RAM only;");
    sh_out("disk stays untouched. Everything is forgotten on reboot.");
    sh_log("amnezia: on");
    return 0;
}

/* ------------------------------------------------------------------ */
/* storage: состояние дисковой подсистемы                              */
/* ------------------------------------------------------------------ */

static int cmd_storage(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct blk_status st = blk_get_status();
    sh_out("ahci: %s | disk: %s | fat32: %s",
           (st.ahci_ok != 0) ? "ok" : "no",
           (st.disk_ok != 0) ? "ok" : "no",
           (st.fat_ok != 0) ? "mounted" : "none");
    if (st.disk_ok != 0) {
        sh_out("model: \"%s\" port=%u sectors=%llu",
               st.model, (unsigned int)st.port,
               (unsigned long long)st.disk_sectors);
    }
    if (st.fat_ok != 0) {
        sh_out("part: lba=%llu sectors=%llu free=%llu KiB",
               (unsigned long long)st.part_lba,
               (unsigned long long)st.part_sectors,
               (unsigned long long)st.fat_free_kb);
    }
    sh_out("amnesia: %s (overlay %u/%u blocks)",
           (st.amnesia != 0) ? "ON" : "off",
           (unsigned int)st.ovl_blocks, (unsigned int)st.ovl_limit);
    sh_out("io: reads=%llu writes=%llu",
           (unsigned long long)st.rd_ops, (unsigned long long)st.wr_ops);
    sh_log("storage: ahci=%d fat=%d amnesia=%d",
           st.ahci_ok, st.fat_ok, st.amnesia);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ls/cat: файлы FAT32                                                 */
/* ------------------------------------------------------------------ */

static int cmd_ls(int argc, char **argv)
{
    const char *path = (argc >= 2) ? argv[1] : "/";
    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        sh_out("ls: no storage device");
        return 1;
    }
    struct fat_dir it;
    err_t rc = fat_dir_open(v, path, &it);
    if (rc != K_OK) {
        sh_out("ls: %s: %s", path, k_strerror(rc));
        sh_log("ls: failed path=%s rc=%d", path, rc);
        return 1;
    }
    u32 count = 0;
    for (;;) {
        struct fat_dir_entry de;
        rc = fat_dir_next(v, &it, &de);
        if (rc != K_OK) {
            break;
        }
        if (de.name[0] == '.') {
            continue;
        }
        if (de.is_dir != 0) {
            sh_out("%s/", de.name);
        } else {
            char sz[24];
            u32 b = de.size;
            if (b >= 1024u) {
                (void)ksnprintf(sz, sizeof(sz), "%u KiB", (b + 1023u) >> 10);
            } else {
                (void)ksnprintf(sz, sizeof(sz), "%u B", b);
            }
            sh_out("%-20s %s", de.name, sz);
        }
        count++;
    }
    sh_out("%u entr%s", count, (count == 1u) ? "y" : "ies");
    sh_log("ls: path=%s count=%u", path, count);
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        sh_out("usage: cat <file>");
        return 1;
    }
    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        sh_out("cat: no storage device");
        return 1;
    }
    static u8 buf[4096];
    u32 len = 0;
    err_t rc = fat_file_read(v, argv[1], buf, sizeof(buf), &len);
    if (rc != K_OK) {
        sh_out("cat: %s: %s", argv[1], k_strerror(rc));
        sh_log("cat: failed path=%s rc=%d", argv[1], rc);
        return 1;
    }
    for (u32 i = 0; i < len; i++) {
        char c = (char)buf[i];
        if (c == '\n' || (c >= 0x20 && c <= 0x7E)) {
            char s[2] = { c, '\0' };
            con_write(s);
        } else if (c == '\r') {
            /* skip */
        } else {
            con_write(".");
        }
    }
    con_write("\n");
    sh_log("cat: path=%s bytes=%u", argv[1], len);
    return 0;
}

/* ------------------------------------------------------------------ */
/* write/rm/cp: запись, удаление, копирование файлов FAT32             */
/* ------------------------------------------------------------------ */

/* Базовое имя пути (после последнего '/' или '\\'). */
static const char *path_base(const char *path)
{
    const char *b = path;
    if (path == NULL) {
        return NULL;
    }
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            b = p + 1;
        }
    }
    return b;
}

#define CP_BUF_SZ (64u * 1024u)
static u8 g_cp_buf[CP_BUF_SZ];   /* однопоточный main-контекст */

static int cmd_write(int argc, char **argv)
{
    if (argc < 2) {
        sh_out("usage: write FILE [text...] (no text = empty file)");
        return 1;
    }
    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        sh_out("write: no storage device");
        return 1;
    }
    if (fat_name_protected(path_base(argv[1])) != 0) {
        sh_out("write: %s: protected system file", path_base(argv[1]));
        return 1;
    }

    /* Текст — все аргументы через пробел + '\n'. */
    static u8 wbuf[SHELL_OUT_BUF];
    u32 len = 0;
    for (int i = 2; i < argc; i++) {
        for (u32 k = 0; argv[i][k] != '\0'; k++) {
            if (len + 1u < sizeof(wbuf)) {
                wbuf[len++] = (u8)argv[i][k];
            }
        }
        if (i + 1 < argc && len + 1u < sizeof(wbuf)) {
            wbuf[len++] = ' ';
        }
    }
    if (argc >= 3 && len + 1u < sizeof(wbuf)) {
        wbuf[len++] = '\n';   /* пустой файл (без текста) остаётся пустым */
    }

    err_t rc = fat_file_write(v, argv[1], wbuf, len);
    if (rc != K_OK) {
        sh_out("write: %s: %s", argv[1], k_strerror(rc));
        sh_log("write: failed path=%s rc=%d", argv[1], rc);
        return 1;
    }
    sh_log("write: path=%s bytes=%u", argv[1], len);
    sh_out("write: %u bytes to %s", len, argv[1]);
    return 0;
}

static int cmd_rm(int argc, char **argv)
{
    if (argc < 2) {
        sh_out("usage: rm FILE");
        return 1;
    }
    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        sh_out("rm: no storage device");
        return 1;
    }
    if (fat_name_protected(path_base(argv[1])) != 0) {
        sh_out("rm: %s: protected system file", path_base(argv[1]));
        sh_log("rm: refused protected path=%s", argv[1]);
        return 1;
    }
    err_t rc = fat_file_delete(v, argv[1]);
    if (rc != K_OK) {
        sh_out("rm: %s: %s", argv[1], k_strerror(rc));
        sh_log("rm: failed path=%s rc=%d", argv[1], rc);
        return 1;
    }
    sh_log("rm: path=%s", argv[1]);
    sh_out("deleted %s", argv[1]);
    return 0;
}

static int cmd_cp(int argc, char **argv)
{
    if (argc < 3) {
        sh_out("usage: cp SRC DST (up to %u KiB)", CP_BUF_SZ >> 10);
        return 1;
    }
    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        sh_out("cp: no storage device");
        return 1;
    }
    if (fat_name_protected(path_base(argv[2])) != 0) {
        sh_out("cp: %s: protected system file", path_base(argv[2]));
        return 1;
    }
    u32 len = 0;
    err_t rc = fat_file_read(v, argv[1], g_cp_buf, sizeof(g_cp_buf), &len);
    if (rc == K_ERANGE) {
        sh_out("cp: %s: larger than %u KiB (limit of this stage)",
               argv[1], CP_BUF_SZ >> 10);
        sh_log("cp: too large src=%s", argv[1]);
        return 1;
    }
    if (rc != K_OK) {
        sh_out("cp: read %s: %s", argv[1], k_strerror(rc));
        sh_log("cp: read failed src=%s rc=%d", argv[1], rc);
        return 1;
    }
    rc = fat_file_write(v, argv[2], g_cp_buf, len);
    if (rc != K_OK) {
        sh_out("cp: write %s: %s", argv[2], k_strerror(rc));
        sh_log("cp: write failed dst=%s rc=%d", argv[2], rc);
        return 1;
    }
    sh_log("cp: src=%s dst=%s bytes=%u", argv[1], argv[2], len);
    sh_out("copied %u bytes: %s -> %s", len, argv[1], argv[2]);
    return 0;
}

static int cmd_sleep(int argc, char **argv)
{
    if (argc < 2) {
        sh_out("usage: sleep MS");
        return 1;
    }
    u32 ms = 0;
    const char *s = argv[1];
    while (*s >= '0' && *s <= '9') {
        ms = ms * 10u + (u32)(*s - '0');
        if (ms > 60000u) {
            ms = 60000u;   /* потолок 60 c: команда не должна подвесить систему */
        }
        s++;
    }
    if (*s != '\0' || ms == 0u) {
        sh_out("sleep: invalid interval '%s' (1..60000 ms)", argv[1]);
        return 1;
    }
    timer_sleep_ms(ms);
    sh_log("sleep: %u ms", ms);
    return 0;
}

/* ------------------------------------------------------------------ */
/* GUI-приложения: notepad/calc/settings/start/apps/pins                */
/* ------------------------------------------------------------------ */

static int cmd_notepad(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    err_t rc = dsk_open_app_by_name("notepad");
    if (rc != K_OK) {
        sh_out("notepad: %s (GUI mode required)", k_strerror(rc));
        return 1;
    }
    sh_log("notepad: opened from shell");
    return 0;
}

static int cmd_calc(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    err_t rc = dsk_open_app_by_name("calc");
    if (rc != K_OK) {
        sh_out("calc: %s (GUI mode required)", k_strerror(rc));
        return 1;
    }
    sh_log("calc: opened from shell");
    return 0;
}

static int cmd_settings(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    err_t rc = dsk_open_app_by_name("settings");
    if (rc != K_OK) {
        sh_out("settings: %s (GUI mode required)", k_strerror(rc));
        return 1;
    }
    sh_log("settings: opened from shell");
    return 0;
}

static int cmd_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (dsk_gui_active() == 0) {
        sh_out("start: GUI mode required (fos == gui)");
        return 1;
    }
    dsk_start_toggle();
    sh_log("start: toggled");
    return 0;
}

static int cmd_apps(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int n = dsk_app_count();
    for (int i = 0; i < n; i++) {
        sh_out("%-10s %s", dsk_app_name(i),
               dsk_app_is_open(i) ? "open" : "");
    }
    sh_log("apps: listed");
    return 0;
}

static int cmd_pins(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int n = dsk_app_count();
    int any = 0;
    for (int i = 0; i < n; i++) {
        if (dsk_app_is_pinned(i) != 0) {
            sh_out("pinned: %s", dsk_app_name(i));
            any = 1;
        }
    }
    if (any == 0) {
        sh_out("no pins (right-click a taskbar icon to pin)");
    }
    sh_log("pins: listed");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sh_log("reboot: requested via 8042 pulse");
    sh_out("rebooting...");
    machine_reboot();
}

static int cmd_poweroff(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sh_log("poweroff: requested");
    sh_out("powering off...");
    machine_poweroff();
}

/* ------------------------------------------------------------------ */
/* FND-3a: user-space процессы                                          */
/* ------------------------------------------------------------------ */

/* Буфер образа: 256 КиБ из физ-пула (выделяется на время команды). */
#define RUN_IMG_FRAMES 64u

static int cmd_run(int argc, char **argv)
{
    if (argc < 2) {
        sh_out("usage: run <program> (например: run hello)");
        return 1;
    }
    if (strlen(argv[1]) >= 12u) {
        sh_out("run: имя слишком длинное (8.3)");
        return 1;
    }

    struct fat_vol *v = blk_fat();
    if (v == NULL) {
        sh_out("run: нет хранилища (образы программ в \\fobos)");
        return 1;
    }

    u8 *img = NULL;
    err_t rc = phys_alloc_contig(RUN_IMG_FRAMES, (u64 *)(u64 *)&img);
    if (rc != K_OK) {
        sh_out("run: нет памяти под образ (%s)", k_strerror(rc));
        return 1;
    }

    /* Образы user-программ лежат в \fobos\NAME.ELF. */
    char path[32];
    {
        u32 p = 0;
        const char *pre = "/fobos/";
        while (*pre != '\0') {
            path[p++] = *pre++;
        }
        for (const char *s = argv[1]; *s != '\0'; s++) {
            path[p++] = *s;
        }
        path[p++] = '.';
        path[p++] = 'e';
        path[p++] = 'l';
        path[p++] = 'f';
        path[p] = '\0';
    }

    u32 img_len = 0;
    klog(KLOG_DEBUG, SH_TAG, "run: чтение %s (буфер 256 КиБ)", path);
    rc = fat_file_read(v, path, img, RUN_IMG_FRAMES * PAGE_SIZE, &img_len);
    klog(KLOG_DEBUG, SH_TAG, "run: чтение завершено rc=%d len=%u", (int)rc,
         (unsigned int)img_len);
    if (rc != K_OK) {
        sh_out("run: %s: %s", path, k_strerror(rc));
        sh_log("run: failed path=%s rc=%d", path, (int)rc);
        for (u32 i = 0; i < RUN_IMG_FRAMES; i++) {
            (void)phys_free_frame((u64)(u64 *)img + (u64)i * PAGE_SIZE);
        }
        return 1;
    }

    rc = elf64_validate(img, img_len);
    if (rc != K_OK) {
        sh_out("run: %s — не исполняемый ELF64 (%s)", path, k_strerror(rc));
        sh_log("run: failed-elf path=%s rc=%d", path, (int)rc);
        for (u32 i = 0; i < RUN_IMG_FRAMES; i++) {
            (void)phys_free_frame((u64)(u64 *)img + (u64)i * PAGE_SIZE);
        }
        return 1;
    }

    u32 pid = 0;
    rc = task_spawn_user(argv[1], img, img_len, &pid);
    for (u32 i = 0; i < RUN_IMG_FRAMES; i++) {
        (void)phys_free_frame((u64)(u64 *)img + (u64)i * PAGE_SIZE);
    }
    if (rc != K_OK) {
        sh_out("run: spawn %s: %s", argv[1], k_strerror(rc));
        sh_log("run: failed-spawn name=%s rc=%d", argv[1], (int)rc);
        return 1;
    }
    sh_out("run: %s запущен (pid=%u)", argv[1], (unsigned int)pid);
    sh_log("run: spawned name=%s pid=%u", argv[1], (unsigned int)pid);
    return 0;
}

static void ps_row(u32 pid, const char *name, const char *state,
                   int is_user, u64 ticks)
{
    sh_out("  %-4u %-12s %-8s %-4s %llu",
           (unsigned int)pid, name, state, (is_user != 0) ? "user" : "krnl",
           (unsigned long long)ticks);
}

static int cmd_ps(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct task_stats ts = task_get_stats();
    sh_out("tasks: used=%u ready=%u blocked=%u zombie=%u (pool %u)",
           (unsigned int)ts.slots_used, (unsigned int)ts.ready,
           (unsigned int)ts.blocked, (unsigned int)ts.zombies,
           (unsigned int)TASK_MAX);
    sh_out("  pid  name         state    ring ticks");
    task_iterate(ps_row);
    struct vmm_stats vs = vmm_get_stats();
    sh_out("vmm: spaces=%llu user_frames=%llu tables=%llu",
           (unsigned long long)vs.spaces,
           (unsigned long long)vs.user_frames,
           (unsigned long long)vs.page_tables);
    sh_log("ps: vmm spaces=%llu user_frames=%llu tables=%llu",
           (unsigned long long)vs.spaces,
           (unsigned long long)vs.user_frames,
           (unsigned long long)vs.page_tables);
    sh_out("syscalls=%llu switches=%llu spawns=%llu exits=%llu",
           (unsigned long long)syscall_count(),
           (unsigned long long)ts.ctx_switches,
           (unsigned long long)ts.spawns, (unsigned long long)ts.exits);
    sh_log("ps: tasks=%u ctx=%llu spawns=%llu exits=%llu",
           (unsigned int)ts.slots_used,
           (unsigned long long)ts.ctx_switches,
           (unsigned long long)ts.spawns, (unsigned long long)ts.exits);
    return 0;
}

static const struct cmd_entry CMDS[] = {
    { "help",     cmd_help },
    { "about",    cmd_about },
    { "uptime",   cmd_uptime },
    { "date",     cmd_date },
    { "ticks",    cmd_ticks },
    { "mem",      cmd_mem },
    { "whoami",   cmd_whoami },
    { "hostname", cmd_hostname },
    { "users",    cmd_users },
    { "useradd",  cmd_useradd },
    { "passwd",   cmd_passwd },
    { "logout",   cmd_logout },
    { "kbd",      cmd_kbd },
    { "mouse",    cmd_mouse },
    { "fbtest",   cmd_fbtest },
    { "font",     cmd_font },
    { "condump",  cmd_condump },
    { "selftest", cmd_selftest },
    { "echo",     cmd_echo },
    { "fos",      cmd_fos },
    { "amnezia",  cmd_amnezia },
    { "storage",  cmd_storage },
    { "ls",       cmd_ls },
    { "cat",      cmd_cat },
    { "write",    cmd_write },
    { "rm",       cmd_rm },
    { "cp",       cmd_cp },
    { "sleep",    cmd_sleep },
    { "notepad",  cmd_notepad },
    { "calc",     cmd_calc },
    { "settings", cmd_settings },
    { "start",    cmd_start },
    { "apps",     cmd_apps },
    { "pins",     cmd_pins },
    { "clear",    cmd_clear },
    { "run",      cmd_run },
    { "ps",       cmd_ps },
    { "crash",    cmd_crash },
    { "reboot",   cmd_reboot },
    { "poweroff", cmd_poweroff },
};

#define CMD_COUNT (sizeof(CMDS) / sizeof(CMDS[0]))

/* ------------------------------------------------------------------ */
/* Разбор и исполнение                                                 */
/* ------------------------------------------------------------------ */

static void exec_line(char *line)
{
    /* Убираем завершающие пробелы. */
    u32 n = strlen(line);
    while (n > 0u && line[n - 1u] == ' ') {
        line[--n] = '\0';
    }
    if (n == 0u) {
        return;
    }

    char *argv[SHELL_MAX_ARGS];
    int argc = 0;
    char *p = line;
    while (*p != '\0' && argc < (int)SHELL_MAX_ARGS) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        if (*p == ' ') {
            *p++ = '\0';
        }
    }
    if (argc == 0) {
        return;
    }

    sh_log("exec: %s", argv[0]);

    /* Диспетчер команд регистронезависим (как командные оболочки больших
     * ОС): под нагрузкой клавиатурный ввод может прийти с залипшим Shift
     * (break-событие потеряно в очереди 8042), и "STORAGE" не должен
     * разваливаться в unknown command. Регистр argv[0] не несёт смысла;
     * аргументы (пути, имена файлов) не трогаем — там регистр значим. */
    for (char *q = argv[0]; *q != '\0'; q++) {
        if (*q >= 'A' && *q <= 'Z') {
            *q = (char)(*q - 'A' + 'a');
        }
    }

    for (u32 i = 0; i < CMD_COUNT; i++) {
        if (strcmp(argv[0], CMDS[i].name) == 0) {
            int rc = CMDS[i].fn(argc, argv);
            if (rc != 0) {
                sh_out("command %s exited with code %d", argv[0], rc);
            }
            return;
        }
    }
    sh_out("unknown command: %s (try help)", argv[0]);
}

static void print_prompt(void)
{
    if (session_active() != 0) {
        char prompt[96];
        (void)ksnprintf(prompt, sizeof(prompt), "%s@%s:~$ ",
                        session_user(), session_hostname());
        con_prompt(prompt);
    } else {
        con_prompt("fobos(kconsole)> ");
    }
}

void shell_init(void)
{
    g_sh.len = 0;
    g_sh.ready = 1;
    g_sh.logout_requested = 0;

    con_set_color(CON_BBLUE, CON_BLACK);
    sh_out("==========================================");
    sh_out("  Fobos OS %s - kernel console", FOBOS_VERSION_STR);
    sh_out("  %s", FOBOS_STAGE_STR);
    sh_out("==========================================");
    con_set_color(CON_LIGHTGRAY, CON_BLACK);
    sh_out("type 'help' for the command list");
    print_prompt();
    sh_log("shell: interactive shell ready");
}

void shell_deinit(void)
{
    g_sh.len = 0;
    g_sh.logout_requested = 0;
}

int shell_logout_requested(void)
{
    return g_sh.logout_requested;
}

void shell_request_logout(void)
{
    g_sh.logout_requested = 1;
}

void shell_clear_logout(void)
{
    g_sh.logout_requested = 0;
}

void shell_reset_line(void)
{
    /* Сброс незавершённой строки и повторный вывод приглашения.
     * Вызывается при смене режима рабочего стола (консоль перепривязана
     * и очищена). */
    g_sh.len = 0;
    if (g_sh.ready != 0) {
        print_prompt();
    }
}

void shell_input(const struct kbd_event *ev)
{
    if (g_sh.ready == 0 || ev == NULL) {
        return;
    }

    if (ev->ch == '\b' || ev->code == KEY_BSPACE) {
        if (g_sh.len > 0u) {
            g_sh.len--;
            (void)con_write("\b");
        }
        return;
    }

    if (ev->ch == '\n' || ev->code == KEY_ENTER) {
        char line[SHELL_LINE_MAX + 1u];
        memcpy(line, g_sh.line, g_sh.len);
        line[g_sh.len] = '\0';
        g_sh.len = 0;
        (void)con_write("\n");
        exec_line(line);
        if (g_sh.logout_requested == 0) {
            print_prompt();
        }
        return;
    }

    /* Ctrl+C: сброс строки; Ctrl+L: очистка экрана. */
    if ((ev->flags & KBD_FL_CTRL) != 0u) {
        if (ev->ch == 'c' || ev->ch == 'C') {
            g_sh.len = 0;
            sh_out("^C");
            print_prompt();
            return;
        }
        if (ev->ch == 'l' || ev->ch == 'L') {
            con_clear();
            print_prompt();
            return;
        }
    }

    if (ev->ch >= 0x20 && ev->ch <= 0x7E) {
        if (g_sh.len + 1u < SHELL_LINE_MAX) {
            g_sh.line[g_sh.len++] = ev->ch;
            char s[2] = { ev->ch, '\0' };
            (void)con_write(s);
        }
        return;
    }

    /* Прочие клавиши (стрелки и т.п.) на FND-2b игнорируются. */
}
