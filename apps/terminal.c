#include "../gui/window.h"
#include "../gui/font.h"
#include "../kernel/fb.h"
#include "../kernel/fs.h"
#include "../kernel/timer.h"
#include "../kernel/http.h"
#include "../lib/string.h"
#include "../lib/mem.h"
#include "../lib/printf.h"
#include "script.h"

#define TERM_COLS 60
#define TERM_ROWS 20
#define TERM_CHAR_W FONT_W
#define TERM_CHAR_H FONT_H
#define HISTORY_MAX 16

struct term_state {
    char lines[TERM_ROWS][TERM_COLS + 1];
    int  row;
    char input[TERM_COLS + 1];
    int  input_len;
    char history[HISTORY_MAX][TERM_COLS + 1];
    int  history_count;
    int  from_script;
};

static struct term_state g_terms[MAX_WINDOWS];

static void term_push_line(struct term_state *t, const char *s) {
    if (t->row >= TERM_ROWS - 1) {
        for (int i = 1; i < TERM_ROWS; i++) {
            memcpy(t->lines[i-1], t->lines[i], TERM_COLS + 1);
        }
        t->row = TERM_ROWS - 1;
    }
    strncpy(t->lines[t->row], s, TERM_COLS);
    t->lines[t->row][TERM_COLS] = 0;
    t->row++;
}

static void term_print(struct term_state *t, const char *s) {
    term_push_line(t, s);
}

static void term_execute(struct term_state *t, const char *cmd);

static void term_out_cb(void *ctx, const char *line) {
    struct term_state *t = (struct term_state*)ctx;
    term_print(t, line);
}

static void term_cmd_cb(void *ctx, const char *cmd) {
    struct term_state *t = (struct term_state*)ctx;
    t->from_script = 1;
    term_execute(t, cmd);
    t->from_script = 0;
}

static int cmd_match(const char *cmd, const char *name, const char **arg) {
    usize n = strlen(name);
    if (strncmp(cmd, name, n) != 0) return 0;
    if (cmd[n] == 0) { *arg = cmd + n; return 1; }
    if (cmd[n] == ' ') { *arg = cmd + n + 1; return 1; }
    return 0;
}

static void cmd_help(struct term_state *t) {
    term_print(t, "FOS Terminal commands");
    term_print(t, "  Files:     ls cat touch write rm cp mv head tail wc");
    term_print(t, "             grep file stat tree df");
    term_print(t, "  Shell:     echo clear pwd cd which history");
    term_print(t, "  System:    ver uname mem free uptime date hostname");
    term_print(t, "             whoami sleep");
    term_print(t, "  Network:   ifconfig ping nslookup wget curl");
    term_print(t, "  Scripting: run FILE, bash FILE");
    term_print(t, "             FILE may be .bat, .cmd or .exe");
}

static void cmd_ver(struct term_state *t) {
    term_print(t, "FOS 0.2 x86_64 -- Fobos Operating System");
}

static void cmd_uname(struct term_state *t) {
    term_print(t, "FOS 0.2 x86_64");
}

static void cmd_mem(struct term_state *t) {
    extern u64 pmm_free_bytes(void);
    extern u64 pmm_total_bytes(void);
    char buf[64];
    snprintf(buf, sizeof(buf), "Memory free: %u KB / %u KB",
             (u32)(pmm_free_bytes() / 1024), (u32)(pmm_total_bytes() / 1024));
    term_print(t, buf);
}

static void cmd_uptime(struct term_state *t) {
    u64 secs = timer_uptime_seconds();
    u32 hh = (u32)(secs / 3600);
    u32 mm = (u32)((secs / 60) % 60);
    u32 ss = (u32)(secs % 60);
    char buf[64];
    snprintf(buf, sizeof(buf), "up %u:%02u:%02u", hh, mm, ss);
    term_print(t, buf);
}

static void cmd_date(struct term_state *t) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Uptime: %u s", (u32)timer_uptime_seconds());
    term_print(t, buf);
}

static void cmd_hostname(struct term_state *t) {
    term_print(t, "fos");
}

static void cmd_whoami(struct term_state *t) {
    term_print(t, "user");
}

static void cmd_pwd(struct term_state *t) {
    term_print(t, "/");
}

static void cmd_cd(struct term_state *t, const char *arg) {
    if (!arg[0] || strcmp(arg, "/") == 0 || strcmp(arg, "~") == 0) {
        term_print(t, "already at /");
        return;
    }
    term_print(t, "cd: only / exists in FOS");
}

static void cmd_mkdir(struct term_state *t, const char *arg) {
    if (!arg[0]) { term_print(t, "usage: mkdir NAME"); return; }
    term_print(t, "mkdir: FOS has a flat filesystem, no directories");
}

static void cmd_rmdir(struct term_state *t, const char *arg) {
    if (!arg[0]) { term_print(t, "usage: rmdir NAME"); return; }
    term_print(t, "rmdir: FOS has a flat filesystem, no directories");
}

static void cmd_which(struct term_state *t, const char *arg) {
    if (!arg[0]) { term_print(t, "usage: which COMMAND"); return; }
    char buf[TERM_COLS + 1];
    snprintf(buf, sizeof(buf), "/bin/%s", arg);
    term_print(t, buf);
}

static void cmd_ls(struct term_state *t) {
    char names[FS_MAX_FILES][FS_MAX_NAME];
    int n = fs_list(names, FS_MAX_FILES);
    if (n == 0) { term_print(t, "(empty)"); return; }
    for (int i = 0; i < n; i++) term_print(t, names[i]);
}

static void cmd_tree(struct term_state *t) {
    char names[FS_MAX_FILES][FS_MAX_NAME];
    int n = fs_list(names, FS_MAX_FILES);
    term_print(t, "/");
    for (int i = 0; i < n; i++) {
        char buf[TERM_COLS + 1];
        snprintf(buf, sizeof(buf), "|-- %s", names[i]);
        term_print(t, buf);
    }
}

static void cmd_df(struct term_state *t) {
    char names[FS_MAX_FILES][FS_MAX_NAME];
    int n = fs_list(names, FS_MAX_FILES);
    u64 total = 0;
    for (int i = 0; i < n; i++) {
        usize sz = 0;
        fs_read(names[i], &sz);
        total += sz;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%d files, %u bytes used", n, (u32)total);
    term_print(t, buf);
}

static void cmd_cat(struct term_state *t, const char *arg) {
    if (!arg[0]) { term_print(t, "usage: cat FILE"); return; }
    usize sz = 0;
    void *p = fs_read(arg, &sz);
    if (!p) { term_print(t, "cat: not found"); return; }
    if (sz == 0) { term_print(t, "(empty file)"); return; }
    const char *c = (const char*)p;
    char buf[TERM_COLS + 1];
    usize start = 0;
    for (usize i = 0; i <= sz; i++) {
        if (i == sz || c[i] == '\n') {
            usize n = i - start;
            if (n > TERM_COLS) n = TERM_COLS;
            memcpy(buf, c + start, n);
            buf[n] = 0;
            term_print(t, buf);
            start = i + 1;
        }
    }
}

static void cmd_head(struct term_state *t, const char *arg) {
    if (!arg[0]) { term_print(t, "usage: head FILE"); return; }
    usize sz = 0;
    void *p = fs_read(arg, &sz);
    if (!p) { term_print(t, "head: not found"); return; }
    const char *c = (const char*)p;
    char buf[TERM_COLS + 1];
    usize start = 0;
    int line = 0;
    for (usize i = 0; i <= sz && line < 10; i++) {
        if (i == sz || c[i] == '\n') {
            usize n = i - start;
            if (n > TERM_COLS) n = TERM_COLS;
            memcpy(buf, c + start, n);
            buf[n] = 0;
            term_print(t, buf);
            line++;
            start = i + 1;
        }
    }
}

static void cmd_tail(struct term_state *t, const char *arg) {
    if (!arg[0]) { term_print(t, "usage: tail FILE"); return; }
    usize sz = 0;
    void *p = fs_read(arg, &sz);
    if (!p) { term_print(t, "tail: not found"); return; }
    const char *c = (const char*)p;
    int total = 0;
    for (usize i = 0; i < sz; i++) if (c[i] == '\n') total++;
    if (sz > 0 && c[sz - 1] != '\n') total++;
    int skip = total > 10 ? total - 10 : 0;
    char buf[TERM_COLS + 1];
    usize start = 0;
    int line = 0;
    for (usize i = 0; i <= sz; i++) {
        if (i == sz || c[i] == '\n') {
            if (line >= skip) {
                usize n = i - start;
                if (n > TERM_COLS) n = TERM_COLS;
                memcpy(buf, c + start, n);
                buf[n] = 0;
                term_print(t, buf);
            }
            line++;
            start = i + 1;
        }
    }
}

static void cmd_wc(struct term_state *t, const char *arg) {
    if (!arg[0]) { term_print(t, "usage: wc FILE"); return; }
    usize sz = 0;
    void *p = fs_read(arg, &sz);
    if (!p) { term_print(t, "wc: not found"); return; }
    const char *c = (const char*)p;
    int lines = 0;
    for (usize i = 0; i < sz; i++) if (c[i] == '\n') lines++;
    if (sz > 0 && c[sz - 1] != '\n') lines++;
    char buf[64];
    snprintf(buf, sizeof(buf), "%d lines, %u bytes", lines, (u32)sz);
    term_print(t, buf);
}

static void cmd_grep(struct term_state *t, const char *arg) {
    const char *sp = arg;
    while (*sp && *sp != ' ') sp++;
    if (*sp != ' ') { term_print(t, "usage: grep PATTERN FILE"); return; }
    char pattern[64];
    usize pl = (usize)(sp - arg);
    if (pl > 63) pl = 63;
    memcpy(pattern, arg, pl);
    pattern[pl] = 0;
    const char *filename = sp + 1;
    usize sz = 0;
    void *p = fs_read(filename, &sz);
    if (!p) { term_print(t, "grep: not found"); return; }
    const char *c = (const char*)p;
    char buf[TERM_COLS + 1];
    usize start = 0;
    for (usize i = 0; i <= sz; i++) {
        if (i == sz || c[i] == '\n') {
            usize len = i - start;
            int found = 0;
            if (len >= pl) {
                for (usize k = 0; k + pl <= len; k++) {
                    if (strncmp(c + start + k, pattern, pl) == 0) { found = 1; break; }
                }
            }
            if (found) {
                usize n = len > TERM_COLS ? TERM_COLS : len;
                memcpy(buf, c + start, n);
                buf[n] = 0;
                term_print(t, buf);
            }
            start = i + 1;
        }
    }
}

static void cmd_file(struct term_state *t, const char *arg) {
    if (!arg[0]) { term_print(t, "usage: file FILE"); return; }
    if (!fs_exists(arg)) { term_print(t, "file: not found"); return; }
    const char *desc = "data";
    usize n = strlen(arg);
    if (n >= 4) {
        const char *ext = arg + n - 4;
        if (strcmp(ext, ".txt") == 0) desc = "ASCII text";
        else if (strcmp(ext, ".bat") == 0) desc = "batch script";
        else if (strcmp(ext, ".cmd") == 0) desc = "batch script";
        else if (strcmp(ext, ".exe") == 0) desc = "executable script";
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "%s: %s", arg, desc);
    term_print(t, buf);
}

static void cmd_stat(struct term_state *t, const char *arg) {
    if (!arg[0]) { term_print(t, "usage: stat FILE"); return; }
    usize sz = 0;
    void *p = fs_read(arg, &sz);
    if (!p) { term_print(t, "stat: not found"); return; }
    char buf[128];
    snprintf(buf, sizeof(buf), "  File: %s", arg);
    term_print(t, buf);
    snprintf(buf, sizeof(buf), "  Size: %u bytes", (u32)sz);
    term_print(t, buf);
    snprintf(buf, sizeof(buf), "  Data: %p", p);
    term_print(t, buf);
}

static void cmd_touch(struct term_state *t, const char *arg) {
    if (!arg[0]) { term_print(t, "usage: touch FILE"); return; }
    if (fs_create(arg, "", 0) == 0) term_print(t, "created");
    else term_print(t, "touch: cannot create");
}

static void cmd_write(struct term_state *t, const char *arg) {
    if (!arg[0]) { term_print(t, "usage: write FILE TEXT"); return; }
    const char *sp = arg;
    while (*sp && *sp != ' ') sp++;
    if (*sp != ' ') { term_print(t, "usage: write FILE TEXT"); return; }
    char name[FS_MAX_NAME];
    usize nl = (usize)(sp - arg);
    if (nl >= FS_MAX_NAME) nl = FS_MAX_NAME - 1;
    memcpy(name, arg, nl);
    name[nl] = 0;
    const char *text = sp + 1;
    usize tl = strlen(text);
    if (fs_write(name, text, tl) == 0) term_print(t, "written");
    else term_print(t, "write: failed");
}

static void cmd_rm(struct term_state *t, const char *arg) {
    if (!arg[0]) { term_print(t, "usage: rm FILE"); return; }
    if (fs_remove(arg) == 0) term_print(t, "removed");
    else term_print(t, "rm: not found");
}

static void cmd_cp(struct term_state *t, const char *arg) {
    const char *sp = arg;
    while (*sp && *sp != ' ') sp++;
    if (!arg[0] || *sp != ' ') { term_print(t, "usage: cp SRC DST"); return; }
    char src[FS_MAX_NAME];
    usize nl = (usize)(sp - arg);
    if (nl >= FS_MAX_NAME) nl = FS_MAX_NAME - 1;
    memcpy(src, arg, nl);
    src[nl] = 0;
    usize sz = 0;
    void *p = fs_read(src, &sz);
    if (!p) { term_print(t, "cp: source not found"); return; }
    if (fs_create(sp + 1, p, sz) == 0) term_print(t, "copied");
    else term_print(t, "cp: failed");
}

static void cmd_mv(struct term_state *t, const char *arg) {
    const char *sp = arg;
    while (*sp && *sp != ' ') sp++;
    if (!arg[0] || *sp != ' ') { term_print(t, "usage: mv SRC DST"); return; }
    char src[FS_MAX_NAME];
    usize nl = (usize)(sp - arg);
    if (nl >= FS_MAX_NAME) nl = FS_MAX_NAME - 1;
    memcpy(src, arg, nl);
    src[nl] = 0;
    usize sz = 0;
    void *p = fs_read(src, &sz);
    if (!p) { term_print(t, "mv: source not found"); return; }
    if (fs_create(sp + 1, p, sz) != 0) { term_print(t, "mv: failed"); return; }
    fs_remove(src);
    term_print(t, "moved");
}

static void cmd_echo(struct term_state *t, const char *arg) {
    term_print(t, arg);
}

static void cmd_sleep(struct term_state *t, const char *arg) {
    (void)t;
    if (!arg[0]) { term_print(t, "usage: sleep SECONDS"); return; }
    u32 n = 0;
    for (const char *p = arg; *p >= '0' && *p <= '9'; p++) n = n * 10 + (u32)(*p - '0');
    if (n == 0) n = 1;
    if (n > 10) n = 10;
    u64 target = timer_ticks() + (u64)n * 100;
    while (timer_ticks() < target) { }
}

static void cmd_history(struct term_state *t) {
    for (int i = 0; i < t->history_count; i++) {
        char buf[TERM_COLS + 8];
        snprintf(buf, sizeof(buf), "%3d  %s", i + 1, t->history[i]);
        term_print(t, buf);
    }
}

static void cmd_run(struct term_state *t, const char *arg) {
    if (!arg[0]) { term_print(t, "usage: run FILE"); return; }
    if (!fs_exists(arg)) { term_print(t, "run: not found"); return; }
    char msg[TERM_COLS + 1];
    snprintf(msg, sizeof(msg), "Running %s", arg);
    term_print(t, msg);
    script_run(arg, term_out_cb, t, term_cmd_cb, t);
    term_print(t, "Done.");
}

static void cmd_ifconfig(struct term_state *t) {
    extern int  net_ready(void);
    extern u32  net_our_ip(void);
    extern u32  net_gateway(void);
    extern u32  net_dns(void);
    extern const u8 *net_our_mac(void);
    if (!net_ready()) { term_print(t, "ifconfig: no network device"); return; }
    u32 ip = net_our_ip();
    u32 gw = net_gateway();
    u32 dns = net_dns();
    const u8 *m = net_our_mac();
    char buf[80];
    snprintf(buf, sizeof(buf), "link:  ether %02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    term_print(t, buf);
    snprintf(buf, sizeof(buf), "inet:  %u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    term_print(t, buf);
    snprintf(buf, sizeof(buf), "gw:    %u.%u.%u.%u",
             (gw >> 24) & 0xFF, (gw >> 16) & 0xFF, (gw >> 8) & 0xFF, gw & 0xFF);
    term_print(t, buf);
    snprintf(buf, sizeof(buf), "dns:   %u.%u.%u.%u",
             (dns >> 24) & 0xFF, (dns >> 16) & 0xFF, (dns >> 8) & 0xFF, dns & 0xFF);
    term_print(t, buf);
}

static u32 parse_ipv4(const char *s, int *ok) {
    u32 parts[4] = {0, 0, 0, 0};
    int p = 0;
    *ok = 0;
    while (*s && p < 4) {
        if (*s < '0' || *s > '9') return 0;
        u32 v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (u32)(*s - '0'); s++; }
        if (v > 255) return 0;
        parts[p++] = v;
        if (*s == '.') { s++; continue; }
        if (*s == 0) break;
        return 0;
    }
    if (p != 4) return 0;
    *ok = 1;
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

static void cmd_ping(struct term_state *t, const char *arg) {
    extern int net_ping(u32 dest, int timeout_ms);
    extern int net_ready(void);
    if (!net_ready()) { term_print(t, "ping: no network device"); return; }
    if (!arg[0]) { term_print(t, "usage: ping a.b.c.d"); return; }
    int ok = 0;
    u32 ip = parse_ipv4(arg, &ok);
    if (!ok) { term_print(t, "ping: bad address"); return; }
    char buf[64];
    snprintf(buf, sizeof(buf), "PING %u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    term_print(t, buf);
    int rtt = net_ping(ip, 2000);
    if (rtt < 0) {
        term_print(t, "Request timed out.");
    } else {
        snprintf(buf, sizeof(buf), "Reply from %u.%u.%u.%u: time=%u ms",
                 (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
                 (u32)rtt);
        term_print(t, buf);
    }
}

static void cmd_nslookup(struct term_state *t, const char *arg) {
    extern int net_dns_lookup(const char *host, u32 *out_ip);
    extern int net_ready(void);
    if (!net_ready()) { term_print(t, "nslookup: no network device"); return; }
    if (!arg[0]) { term_print(t, "usage: nslookup HOST"); return; }
    u32 ip = 0;
    int rc = net_dns_lookup(arg, &ip);
    if (rc != 0) { term_print(t, "nslookup: lookup failed"); return; }
    char buf[64];
    snprintf(buf, sizeof(buf), "%s -> %u.%u.%u.%u",
             arg, (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    term_print(t, buf);
}

static void cmd_wget(struct term_state *t, const char *arg) {
    extern int net_ready(void);
    if (!net_ready()) { term_print(t, "wget: no network device"); return; }
    if (!arg[0]) { term_print(t, "usage: wget URL"); return; }

    char url[256];
    if (strncmp(arg, "http://", 7) != 0 && strncmp(arg, "https://", 8) != 0) {
        snprintf(url, sizeof(url), "http://%s", arg);
    } else {
        strncpy(url, arg, sizeof(url) - 1);
        url[sizeof(url) - 1] = 0;
    }

    static u8 wget_body[65536];
    struct http_response resp;
    memset(&resp, 0, sizeof(resp));

    char msg[TERM_COLS + 1];
    snprintf(msg, sizeof(msg), "GET %s", url);
    term_print(t, msg);

    int rc = http_get_follow(url, sizeof(url), &resp, wget_body, sizeof(wget_body) - 1);

    if (rc == -100) {
        term_print(t, "Redirected to HTTPS, cannot follow.");
        snprintf(msg, sizeof(msg), "  %.58s", resp.location);
        term_print(t, msg);
        return;
    }
    if (rc != 0) {
        snprintf(msg, sizeof(msg), "wget: failed (code %d)", rc);
        term_print(t, msg);
        return;
    }

    snprintf(msg, sizeof(msg), "HTTP %d, %u bytes", resp.status, (u32)resp.body_len);
    term_print(t, msg);
    if (resp.content_type[0]) {
        snprintf(msg, sizeof(msg), "Type: %s", resp.content_type);
        term_print(t, msg);
    }
    if (resp.redirect_count > 0) {
        snprintf(msg, sizeof(msg), "Redirects: %d", resp.redirect_count);
        term_print(t, msg);
    }

    int shown = 0;
    usize off = 0;
    char line[TERM_COLS + 1];
    while (off < resp.body_len && shown < 20) {
        usize end = off;
        while (end < resp.body_len && wget_body[end] != '\n') end++;
        usize n = end - off;
        if (n > TERM_COLS) n = TERM_COLS;
        memcpy(line, wget_body + off, n);
        line[n] = 0;
        for (usize k = 0; k < n; k++) {
            if (line[k] == '\r') { line[k] = 0; break; }
        }
        term_print(t, line);
        shown++;
        off = end + 1;
    }
    if (resp.body_len > 0) {
        snprintf(msg, sizeof(msg), "... %u bytes total", (u32)resp.body_len);
        term_print(t, msg);
    }
}

static void term_execute(struct term_state *t, const char *cmd) {
    char line[TERM_COLS + 4];
    snprintf(line, sizeof(line), "> %s", cmd);
    term_push_line(t, line);

    if (cmd[0] == 0) return;

    if (!t->from_script) {
        if (t->history_count < HISTORY_MAX) {
            strncpy(t->history[t->history_count], cmd, TERM_COLS);
            t->history[t->history_count][TERM_COLS] = 0;
            t->history_count++;
        } else {
            for (int i = 1; i < HISTORY_MAX; i++) {
                memcpy(t->history[i-1], t->history[i], TERM_COLS + 1);
            }
            strncpy(t->history[HISTORY_MAX-1], cmd, TERM_COLS);
            t->history[HISTORY_MAX-1][TERM_COLS] = 0;
        }
    }

    const char *arg = NULL;
    if (cmd_match(cmd, "help", &arg))     { cmd_help(t); return; }
    if (cmd_match(cmd, "ver", &arg))      { cmd_ver(t); return; }
    if (cmd_match(cmd, "uname", &arg))    { cmd_uname(t); return; }
    if (cmd_match(cmd, "mem", &arg))      { cmd_mem(t); return; }
    if (cmd_match(cmd, "free", &arg))     { cmd_mem(t); return; }
    if (cmd_match(cmd, "uptime", &arg))   { cmd_uptime(t); return; }
    if (cmd_match(cmd, "date", &arg))     { cmd_date(t); return; }
    if (cmd_match(cmd, "hostname", &arg)) { cmd_hostname(t); return; }
    if (cmd_match(cmd, "whoami", &arg))   { cmd_whoami(t); return; }
    if (cmd_match(cmd, "pwd", &arg))      { cmd_pwd(t); return; }
    if (cmd_match(cmd, "cd", &arg))       { cmd_cd(t, arg); return; }
    if (cmd_match(cmd, "mkdir", &arg))    { cmd_mkdir(t, arg); return; }
    if (cmd_match(cmd, "rmdir", &arg))    { cmd_rmdir(t, arg); return; }
    if (cmd_match(cmd, "which", &arg))    { cmd_which(t, arg); return; }
    if (cmd_match(cmd, "clear", &arg)) {
        memset(t->lines, 0, sizeof(t->lines));
        t->row = 0;
        return;
    }
    if (cmd_match(cmd, "echo", &arg))     { cmd_echo(t, arg); return; }
    if (cmd_match(cmd, "ls", &arg))       { cmd_ls(t); return; }
    if (cmd_match(cmd, "tree", &arg))     { cmd_tree(t); return; }
    if (cmd_match(cmd, "df", &arg))       { cmd_df(t); return; }
    if (cmd_match(cmd, "cat", &arg))      { cmd_cat(t, arg); return; }
    if (cmd_match(cmd, "head", &arg))     { cmd_head(t, arg); return; }
    if (cmd_match(cmd, "tail", &arg))     { cmd_tail(t, arg); return; }
    if (cmd_match(cmd, "wc", &arg))       { cmd_wc(t, arg); return; }
    if (cmd_match(cmd, "grep", &arg))     { cmd_grep(t, arg); return; }
    if (cmd_match(cmd, "file", &arg))     { cmd_file(t, arg); return; }
    if (cmd_match(cmd, "stat", &arg))     { cmd_stat(t, arg); return; }
    if (cmd_match(cmd, "touch", &arg))    { cmd_touch(t, arg); return; }
    if (cmd_match(cmd, "write", &arg))    { cmd_write(t, arg); return; }
    if (cmd_match(cmd, "rm", &arg))       { cmd_rm(t, arg); return; }
    if (cmd_match(cmd, "cp", &arg))       { cmd_cp(t, arg); return; }
    if (cmd_match(cmd, "mv", &arg))       { cmd_mv(t, arg); return; }
    if (cmd_match(cmd, "sleep", &arg))    { cmd_sleep(t, arg); return; }
    if (cmd_match(cmd, "history", &arg))  { cmd_history(t); return; }
    if (cmd_match(cmd, "run", &arg))      { cmd_run(t, arg); return; }
    if (cmd_match(cmd, "bash", &arg))     { cmd_run(t, arg); return; }
    if (cmd_match(cmd, "exec", &arg))     { cmd_run(t, arg); return; }
    if (cmd_match(cmd, "ifconfig", &arg)) { cmd_ifconfig(t); return; }
    if (cmd_match(cmd, "ping", &arg))     { cmd_ping(t, arg); return; }
    if (cmd_match(cmd, "arp", &arg))      { cmd_ifconfig(t); return; }
    if (cmd_match(cmd, "nslookup", &arg)) { cmd_nslookup(t, arg); return; }
    if (cmd_match(cmd, "wget", &arg))     { cmd_wget(t, arg); return; }
    if (cmd_match(cmd, "curl", &arg))     { cmd_wget(t, arg); return; }

    term_print(t, "unknown command, type 'help'");
}

static void term_draw(struct window *win) {
    struct term_state *t = (struct term_state*)win->user;
    int ox = win->x + 4;
    int oy = win->y + 22 + 4;
    fb_fill_rect(ox - 2, oy - 2, win->w - 4, win->h - 26, 0x101818);
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; t->lines[r][c]; c++) {
            font_draw_char(ox + c * TERM_CHAR_W, oy + r * TERM_CHAR_H,
                           t->lines[r][c], 0x8CD88C, 0x101818);
        }
    }
    int r = t->row;
    if (r >= TERM_ROWS) r = TERM_ROWS - 1;
    font_draw_string(ox, oy + r * TERM_CHAR_H, "> ", 0xE8E8E8, 0x101818);
    for (int c = 0; c < t->input_len; c++) {
        font_draw_char(ox + (2 + c) * TERM_CHAR_W, oy + r * TERM_CHAR_H,
                       t->input[c], 0xE8E8E8, 0x101818);
    }
    if ((timer_ticks() / 50) & 1) {
        fb_fill_rect(ox + (2 + t->input_len) * TERM_CHAR_W, oy + r * TERM_CHAR_H,
                     FONT_W, FONT_H, 0xC0C0C0);
    }
}

static void term_key(struct window *win, char c) {
    struct term_state *t = (struct term_state*)win->user;
    if (c == 0x1C || c == '\n') {
        t->input[t->input_len] = 0;
        term_execute(t, t->input);
        t->input_len = 0;
        t->input[0] = 0;
        return;
    }
    if (c == 0x0E) {
        if (t->input_len > 0) t->input_len--;
        t->input[t->input_len] = 0;
        return;
    }
    if (c >= 32 && c < 127 && t->input_len < TERM_COLS - 4) {
        t->input[t->input_len++] = c;
        t->input[t->input_len] = 0;
    }
}

static struct term_state *term_alloc(void) {
    int used[MAX_WINDOWS];
    for (int i = 0; i < MAX_WINDOWS; i++) used[i] = 0;
    for (int w = 0; w < window_count(); w++) {
        struct window *win = window_get(w);
        if (!win || !win->visible) continue;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (win->user == &g_terms[i]) { used[i] = 1; break; }
        }
    }
    for (int i = 0; i < MAX_WINDOWS; i++) if (!used[i]) return &g_terms[i];
    return NULL;
}

static void term_open(struct term_state *t, const char *title, const char *initial_cmd) {
    memset(t, 0, sizeof(*t));
    term_push_line(t, "FOS Terminal -- type 'help' for commands.");
    int id = window_create(120, 80, 500, 360, title);
    if (id < 0) return;
    struct window *win = window_get(id);
    win->user = t;
    win->on_draw = term_draw;
    win->on_key = term_key;
    if (initial_cmd) {
        term_execute(t, initial_cmd);
    }
}

void apps_launch_terminal(void) {
    struct term_state *t = term_alloc();
    if (!t) return;
    term_open(t, "Terminal", NULL);
}

void apps_launch_terminal_exec(const char *filename) {
    struct term_state *t = term_alloc();
    if (!t) return;
    char cmd[TERM_COLS + 1];
    snprintf(cmd, sizeof(cmd), "run %s", filename);
    term_open(t, filename, cmd);
}