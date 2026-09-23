#include "browser.h"
#include "../gui/window.h"
#include "../gui/font.h"
#include "../kernel/fb.h"
#include "../kernel/fs.h"
#include "../kernel/timer.h"
#include "../kernel/net.h"
#include "../kernel/http.h"
#include "../kernel/html.h"
#include "../lib/string.h"
#include "../lib/mem.h"
#include "../lib/printf.h"

#define BR_WIN_W 760
#define BR_WIN_H 520
#define BR_MENU_H 20
#define BR_NAV_H 26
#define BR_CONTENT_H 420
#define BR_STATUS_H 20
#define BR_CONTENT_Y (BR_MENU_H + BR_NAV_H)
#define BR_STATUS_Y (BR_CONTENT_Y + BR_CONTENT_H)
#define BR_LINES 26
#define BR_LINE_LEN 94
#define BR_HIST 16
#define BR_URL_LEN 256

#define BTN_W 22
#define BTN_H 22
#define BTN_BACK 4
#define BTN_FWD 28
#define BTN_REF 52
#define BTN_HOME 76
#define BTN_GO 668
#define ADDR_X 104
#define ADDR_W 556
#define ADDR_Y 22
#define ADDR_H 22

#define PAGE_BUF 131072

struct br_line {
    char text[BR_LINE_LEN + 1];
    char link[BR_URL_LEN];
    int  is_header;
};

struct browser_state {
    char url[BR_URL_LEN];
    char addr[BR_URL_LEN];
    int  addr_len;
    int  addr_focus;
    struct br_line lines[BR_LINES];
    int  line_count;
    int  scroll;
    char hist[BR_HIST][BR_URL_LEN];
    int  hist_count;
    int  hist_pos;
    char status[128];
    u64  status_until;
    u8  *page_buf;
};

static struct browser_state g_browsers[MAX_WINDOWS];
static u8 g_page_buf[MAX_WINDOWS][PAGE_BUF];

struct idx_ent {
    const char *title;
    const char *url;
    const char *desc;
    const char *keywords;
};

static const struct idx_ent g_index[] = {
    {"Fobos Operating System",    "fos.local",             "A minimal x86_64 graphical operating system.",  "fos fobos operating system os kernel graphical"},
    {"x86_64 Kernel Development", "docs.local/kernel",     "Long mode, paging, GDT, IDT.",                  "x86_64 kernel long mode paging gdt idt developer"},
    {"PS/2 Mouse Driver",         "docs.local/mouse",      "Three-byte packets, sync bit, IRQ 12.",         "ps2 mouse driver input cursor click"},
    {"Linear Framebuffer",        "docs.local/fb",         "32bpp, 24bpp fallback, double buffering.",      "framebuffer graphics double buffering pixels"},
    {"Multiboot2 Bootloader Spec","docs.local/multiboot2", "Boot tags, framebuffer tag, mmap tag.",         "multiboot2 grub bootloader spec tags"},
    {"Batch Scripting on FOS",    "docs.local/scripts",    "How .bat, .cmd and .exe scripts run.",          "batch script cmd bat exe shell run"},
    {"Terminal Emulator",         "docs.local/terminal",   "The FOS terminal shell.",                       "terminal shell command line bash"},
    {"Window Manager",            "docs.local/wm",         "Overlapping windows and widgets.",              "window manager wm gui ui compositor"},
    {"Real Networking on FOS",    "docs.local/network",    "RTL8139, ARP, IPv4, UDP, DNS, TCP, HTTP.",       "network tcp udp dns http rtl8139 internet real"},
    {"Internet Explorer",         "about:browser",         "How to use the FOS browser.",                   "browser internet explorer http url navigate"},
    {"HTTP and Chunked Encoding", "docs.local/http",       "HTTP/1.1, chunked, redirects, cookies.",        "http chunked redirect cookie http1.1 web"},
};

#define IDX_LEN ((int)(sizeof(g_index) / sizeof(g_index[0])))

struct doc_ent {
    const char *url;
    const char *title;
    const char *body[20];
};

static const struct doc_ent g_docs[] = {
    {
        "docs.local/http",
        "HTTP and Chunked Encoding",
        {
            "The FOS browser speaks HTTP/1.1.",
            "",
            "Request headers:",
            "  Host: required for virtual hosting",
            "  User-Agent: FOS/0.2 (x86_64)",
            "  Accept: text/html,*/*;q=0.1",
            "  Accept-Encoding: identity",
            "  Connection: close",
            "  Cookie: <jar contents>",
            "",
            "Response handling:",
            "  - chunked transfer encoding decoded",
            "  - Location followed up to 6 hops",
            "  - Set-Cookie stored in a small jar",
            "  - Content-Type recorded",
            "",
            "HTTPS is not implemented. Sites that",
            "redirect to https will not render.",
            NULL
        }
    },
    {
        "docs.local/kernel",
        "x86_64 Kernel Development",
        {
            "The FOS kernel runs in 64-bit long mode",
            "with paging enabled before C code executes.",
            "",
            "Subsystems:",
            "  - GDT, TSS, IDT",
            "  - Bitmap physical memory manager",
            "  - 4-level paging and heap",
            "  - RTL8139 NIC driver",
            "  - ARP / IPv4 / ICMP / UDP / TCP",
            "  - DNS resolver",
            "  - HTTP/1.1 client",
            "  - HTML text extractor",
            NULL
        }
    },
    {
        "docs.local/mouse",
        "PS/2 Mouse Driver",
        {
            "The mouse sends 3-byte packets on IRQ 12.",
            "Byte 0: flags. Bit 3 is the sync bit.",
            "Bits 6 and 7 signal overflow.",
            "",
            "Byte 1: X delta. Byte 2: Y delta.",
            NULL
        }
    },
    {
        "docs.local/fb",
        "Linear Framebuffer",
        {
            "GRUB hands us the framebuffer via the",
            "Multiboot2 type-8 tag.",
            "",
            "FOS supports 32bpp and 24bpp.",
            "Double buffering is used.",
            NULL
        }
    },
    {
        "docs.local/multiboot2",
        "Multiboot2 Bootloader Spec",
        {
            "GRUB2 loads the kernel via Multiboot2.",
            "The framebuffer tag is type 8.",
            NULL
        }
    },
    {
        "docs.local/scripts",
        "Batch Scripting on FOS",
        {
            "FOS interprets .bat, .cmd and .exe files as",
            "batch scripts.",
            "",
            "Directives: echo, set, goto, pause, exit.",
            NULL
        }
    },
    {
        "docs.local/terminal",
        "Terminal Emulator",
        {
            "Around 45 commands, modeled on bash.",
            "",
            "Network: ifconfig ping nslookup wget curl",
            NULL
        }
    },
    {
        "docs.local/wm",
        "Window Manager",
        {
            "Windows are drawn back-to-front, then the",
            "focused window is drawn last.",
            NULL
        }
    },
    {
        "docs.local/network",
        "Real Networking on FOS",
        {
            "Working network stack:",
            "  Driver: RTL8139",
            "  Layers: Ethernet ARP IPv4 ICMP",
            "          UDP DNS TCP HTTP",
            "",
            "Try from the terminal:",
            "  ifconfig",
            "  ping 10.0.2.2",
            "  nslookup example.com",
            "  wget example.com /",
            "",
            "HTTPS is not implemented. Only plain HTTP",
            "sites render.",
            NULL
        }
    },
};

#define DOC_LEN ((int)(sizeof(g_docs) / sizeof(g_docs[0])))

static void set_status(struct browser_state *s, const char *msg) {
    strncpy(s->status, msg, sizeof(s->status) - 1);
    s->status[sizeof(s->status) - 1] = 0;
    s->status_until = timer_ticks() + 300;
}

static int lower(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

static int contains_case(const char *hay, const char *needle) {
    usize nl = strlen(needle);
    if (nl == 0) return 0;
    for (const char *p = hay; *p; p++) {
        usize i = 0;
        while (i < nl && p[i] && lower((u8)p[i]) == lower((u8)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

static int has_dot(const char *s) {
    while (*s) { if (*s == '.') return 1; s++; }
    return 0;
}

static int has_space(const char *s) {
    while (*s) { if (*s == ' ') return 1; s++; }
    return 0;
}

static void add_line(struct browser_state *s, const char *text, const char *link, int header) {
    if (s->line_count >= BR_LINES) return;
    struct br_line *l = &s->lines[s->line_count++];
    strncpy(l->text, text, BR_LINE_LEN);
    l->text[BR_LINE_LEN] = 0;
    if (link && link[0]) {
        strncpy(l->link, link, BR_URL_LEN - 1);
        l->link[BR_URL_LEN - 1] = 0;
    } else {
        l->link[0] = 0;
    }
    l->is_header = header;
}

static void blank(struct browser_state *s) { add_line(s, "", NULL, 0); }

static void render_home(struct browser_state *s) {
    add_line(s, "          FOS Internet Explorer 0.2", NULL, 1);
    blank(s);
    add_line(s, " Real network stack: RTL8139, ARP, IPv4, UDP,", NULL, 0);
    add_line(s, " DNS, TCP, HTTP/1.1, chunked transfer, cookies,", NULL, 0);
    add_line(s, " HTML text and link extraction.", NULL, 0);
    blank(s);
    add_line(s, " Internal pages:", NULL, 0);
    add_line(s, "   [Search FOS docs]",         "google.com",     0);
    add_line(s, "   [FOS homepage]",             "fos.local",      0);
    add_line(s, "   [Documentation index]",      "docs.local",     0);
    add_line(s, "   [How HTTP works here]",      "docs.local/http",0);
    add_line(s, "   [About the browser]",        "about:browser",  0);
    blank(s);
    add_line(s, " Real HTTP sites:", NULL, 0);
    add_line(s, "   [example.com]",   "http://example.com/",  0);
    add_line(s, "   [neverssl.com]",  "http://neverssl.com/", 0);
    add_line(s, "   [info.cern.ch]",  "http://info.cern.ch/", 0);
    blank(s);
    add_line(s, " HTTPS is not implemented, so most modern", NULL, 0);
    add_line(s, " sites will redirect and cannot be read.", NULL, 0);
}

static void render_about_browser(struct browser_state *s) {
    add_line(s, " How the FOS browser works", NULL, 1);
    blank(s);
    add_line(s, " Address bar accepts a URL or a query.", NULL, 0);
    blank(s);
    add_line(s, " URL pipeline:", NULL, 0);
    add_line(s, "   1. DNS over UDP to 10.0.2.3", NULL, 0);
    add_line(s, "   2. TCP connect to port 80", NULL, 0);
    add_line(s, "   3. GET path HTTP/1.1", NULL, 0);
    add_line(s, "   4. read response, decode chunked", NULL, 0);
    add_line(s, "   5. follow redirects up to 6 hops", NULL, 0);
    add_line(s, "   6. store Set-Cookie values", NULL, 0);
    add_line(s, "   7. strip HTML tags, extract links", NULL, 0);
    blank(s);
    add_line(s, " Fallback: internal mock index and docs.", NULL, 0);
    blank(s);
    add_line(s, " Home: [about:home]", "about:home", 0);
}

static void render_google_home(struct browser_state *s) {
    add_line(s, "                   FOS Search", NULL, 1);
    blank(s);
    add_line(s, " Type a query in the address bar.", NULL, 0);
    blank(s);
    add_line(s, " Examples:", NULL, 0);
    add_line(s, "   [fos operating system]",  "search:fos operating system",  0);
    add_line(s, "   [x86_64 kernel]",         "search:x86_64 kernel",         0);
    add_line(s, "   [tcp ip stack]",         "search:network tcp",           0);
    add_line(s, "   [http chunked]",         "search:http chunked",          0);
    blank(s);
    add_line(s, " Real google.com is HTTPS and cannot be read.", NULL, 0);
    blank(s);
    add_line(s, " Home: [about:home]", "about:home", 0);
}

static void render_search(struct browser_state *s, const char *query) {
    char header[BR_LINE_LEN + 1];
    snprintf(header, sizeof(header), " Search results: %s", query);
    add_line(s, header, NULL, 1);
    blank(s);

    int scores[IDX_LEN];
    for (int i = 0; i < IDX_LEN; i++) scores[i] = 0;

    char qcopy[BR_URL_LEN];
    strncpy(qcopy, query, BR_URL_LEN - 1);
    qcopy[BR_URL_LEN - 1] = 0;

    char *tok = qcopy;
    while (*tok) {
        char *end = tok;
        while (*end && *end != ' ' && *end != '+') end++;
        char save = *end;
        *end = 0;
        if (*tok) {
            for (int i = 0; i < IDX_LEN; i++) {
                if (contains_case(g_index[i].title, tok))    scores[i] += 3;
                if (contains_case(g_index[i].keywords, tok)) scores[i] += 2;
                if (contains_case(g_index[i].desc, tok))     scores[i] += 1;
            }
        }
        *end = save;
        if (*end == 0) break;
        tok = end + 1;
    }

    int picked[5];
    for (int p = 0; p < 5; p++) picked[p] = -1;
    for (int p = 0; p < 5; p++) {
        int best = -1;
        int bestv = 0;
        for (int i = 0; i < IDX_LEN; i++) {
            int skip = 0;
            for (int k = 0; k < p; k++) if (picked[k] == i) skip = 1;
            if (skip) continue;
            if (scores[i] > bestv) { bestv = scores[i]; best = i; }
        }
        if (best < 0) break;
        picked[p] = best;
    }

    int found = 0;
    for (int p = 0; p < 5; p++) {
        if (picked[p] < 0) continue;
        found++;
        int i = picked[p];
        char ln[BR_LINE_LEN + 1];
        snprintf(ln, sizeof(ln), " %d. %s", found, g_index[i].title);
        add_line(s, ln, g_index[i].url, 0);
        snprintf(ln, sizeof(ln), "    %s", g_index[i].desc);
        add_line(s, ln, g_index[i].url, 0);
        blank(s);
    }

    if (found == 0) {
        add_line(s, " No results.", NULL, 0);
        blank(s);
        add_line(s, " Home: [about:home]", "about:home", 0);
    }
}

static void render_fos_local(struct browser_state *s) {
    add_line(s, " Fobos Operating System", NULL, 1);
    blank(s);
    add_line(s, " Minimal x86_64 operating system.", NULL, 0);
    add_line(s, " Boots via GRUB2 with Multiboot2, enters", NULL, 0);
    add_line(s, " long mode, brings up paging, drives PS/2", NULL, 0);
    add_line(s, " input, and paints a compositor on the", NULL, 0);
    add_line(s, " linear framebuffer.", NULL, 0);
    blank(s);
    add_line(s, " It also has a real network stack and", NULL, 0);
    add_line(s, " a working HTTP client.", NULL, 0);
    blank(s);
    add_line(s, " See also:", NULL, 0);
    add_line(s, "   [Documentation index]",  "docs.local",         0);
    add_line(s, "   [Network overview]",     "docs.local/network", 0);
    add_line(s, "   [HTTP details]",         "docs.local/http",    0);
    add_line(s, "   [Home]",                 "about:home",         0);
}

static void render_docs_index(struct browser_state *s) {
    add_line(s, " FOS Documentation", NULL, 1);
    blank(s);
    for (int i = 0; i < DOC_LEN; i++) {
        char ln[BR_LINE_LEN + 1];
        snprintf(ln, sizeof(ln), "   [%s]", g_docs[i].title);
        add_line(s, ln, g_docs[i].url, 0);
    }
    blank(s);
    add_line(s, " Home: [about:home]", "about:home", 0);
}

static void render_doc(struct browser_state *s, const struct doc_ent *d) {
    char buf[BR_LINE_LEN + 1];
    snprintf(buf, sizeof(buf), " %s", d->title);
    add_line(s, buf, NULL, 1);
    blank(s);
    for (int i = 0; d->body[i]; i++) {
        snprintf(buf, sizeof(buf), " %s", d->body[i]);
        add_line(s, buf, NULL, 0);
    }
    blank(s);
    add_line(s, " Back to [Documentation index]", "docs.local", 0);
}

static const struct doc_ent *find_doc(const char *url) {
    for (int i = 0; i < DOC_LEN; i++) {
        if (strcmp(g_docs[i].url, url) == 0) return &g_docs[i];
    }
    return NULL;
}

static void render_real_page(struct browser_state *s, const char *start_url) {
    char url_buf[BR_URL_LEN];
    strncpy(url_buf, start_url, sizeof(url_buf) - 1);
    url_buf[sizeof(url_buf) - 1] = 0;

    set_status(s, "Fetching...");

    struct http_response resp;
    memset(&resp, 0, sizeof(resp));

    int rc = http_get_follow(url_buf, sizeof(url_buf), &resp, s->page_buf, PAGE_BUF - 1);
    strncpy(s->url, url_buf, BR_URL_LEN - 1);
    s->url[BR_URL_LEN - 1] = 0;

    if (rc == -100) {
        add_line(s, " Redirected to HTTPS", NULL, 1);
        blank(s);
        char buf[BR_LINE_LEN + 1];
        snprintf(buf, sizeof(buf), " HTTPS target: %.70s", resp.location);
        add_line(s, buf, NULL, 0);
        blank(s);
        add_line(s, " FOS does not implement TLS. The TCP/IP", NULL, 0);
        add_line(s, " stack is real, but TLS cryptography is", NULL, 0);
        add_line(s, " not part of this build.", NULL, 0);
        blank(s);
        add_line(s, " Try an HTTP-only site instead:", NULL, 0);
        add_line(s, "   [example.com]",   "http://example.com/",  0);
        add_line(s, "   [neverssl.com]",  "http://neverssl.com/", 0);
        add_line(s, "   [info.cern.ch]",  "http://info.cern.ch/", 0);
        blank(s);
        add_line(s, " Home: [about:home]", "about:home", 0);
        return;
    }

    if (rc != 0) {
        char buf[BR_LINE_LEN + 1];
        snprintf(buf, sizeof(buf), " Failed to fetch (code %d)", rc);
        add_line(s, buf, NULL, 1);
        blank(s);
        add_line(s, " Common reasons:", NULL, 0);
        add_line(s, "   - Host not resolvable", NULL, 0);
        add_line(s, "   - TCP connection refused or timed out", NULL, 0);
        add_line(s, "   - Server requires HTTPS", NULL, 0);
        blank(s);
        add_line(s, " Try: http://example.com/", NULL, 0);
        return;
    }

    char status_line[BR_LINE_LEN + 1];
    snprintf(status_line, sizeof(status_line), " HTTP %d %s",
             resp.status, http_status_text(resp.status));
    add_line(s, status_line, NULL, 1);

    if (resp.redirect_count > 0) {
        char rbuf[BR_LINE_LEN + 1];
        snprintf(rbuf, sizeof(rbuf), " Followed %d redirect(s)", resp.redirect_count);
        add_line(s, rbuf, NULL, 0);
    }
    if (resp.content_type[0]) {
        char cbuf[BR_LINE_LEN + 1];
        snprintf(cbuf, sizeof(cbuf), " Content-Type: %.60s", resp.content_type);
        add_line(s, cbuf, NULL, 0);
    }
    blank(s);

    struct html_doc doc;
    html_parse((const char*)resp.body, resp.body_len, s->url, &doc);

    if (doc.title[0]) {
        char tbuf[BR_LINE_LEN + 1];
        snprintf(tbuf, sizeof(tbuf), " %s", doc.title);
        add_line(s, tbuf, NULL, 1);
        blank(s);
    }

    for (int i = 0; i < doc.line_count && s->line_count < BR_LINES; i++) {
        struct html_line *hl = &doc.lines[i];
        add_line(s, hl->text, hl->is_link ? hl->link : NULL, hl->is_header);
    }

    if (doc.truncated) {
        blank(s);
        add_line(s, " (page truncated at 260 lines)", NULL, 0);
    }
    if (doc.line_count == 0) {
        add_line(s, " (no readable text on this page)", NULL, 0);
    }
}

static void br_render(struct browser_state *s) {
    s->line_count = 0;
    s->scroll = 0;
    const char *u = s->url;

    if (strcmp(u, "about:home") == 0)    { render_home(s); return; }
    if (strcmp(u, "about:browser") == 0) { render_about_browser(s); return; }
    if (strcmp(u, "google.com") == 0)    { render_google_home(s); return; }
    if (strncmp(u, "search:", 7) == 0)   { render_search(s, u + 7); return; }
    if (strcmp(u, "fos.local") == 0)     { render_fos_local(s); return; }
    if (strcmp(u, "docs.local") == 0)    { render_docs_index(s); return; }
    if (strncmp(u, "docs.local/", 11) == 0) {
        const struct doc_ent *d = find_doc(u);
        if (d) { render_doc(s, d); return; }
    }

    if (has_space(u) || !has_dot(u)) {
        render_search(s, u);
        return;
    }

    if (!net_ready()) {
        add_line(s, " No network device.", NULL, 1);
        blank(s);
        add_line(s, " Run QEMU with -nic user,model=rtl8139", NULL, 0);
        add_line(s, " Home: [about:home]", "about:home", 0);
        return;
    }

    render_real_page(s, u);
}

static void br_push(struct browser_state *s, const char *url) {
    if (s->hist_pos >= 0 && s->hist_pos < BR_HIST - 1) {
        s->hist_count = s->hist_pos + 1;
    } else {
        s->hist_count = 0;
    }
    if (s->hist_count >= BR_HIST) {
        for (int i = 1; i < BR_HIST; i++) memcpy(s->hist[i-1], s->hist[i], BR_URL_LEN);
        s->hist_count = BR_HIST - 1;
    }
    strncpy(s->hist[s->hist_count], url, BR_URL_LEN - 1);
    s->hist[s->hist_count][BR_URL_LEN - 1] = 0;
    s->hist_pos = s->hist_count;
    s->hist_count++;
    strncpy(s->url, url, BR_URL_LEN - 1);
    s->url[BR_URL_LEN - 1] = 0;
    strncpy(s->addr, url, BR_URL_LEN - 1);
    s->addr[BR_URL_LEN - 1] = 0;
    s->addr_len = (int)strlen(url);
    br_render(s);
}

static void br_set_hist(struct browser_state *s, int new_pos) {
    s->hist_pos = new_pos;
    strncpy(s->url, s->hist[s->hist_pos], BR_URL_LEN - 1);
    s->url[BR_URL_LEN - 1] = 0;
    strncpy(s->addr, s->url, BR_URL_LEN - 1);
    s->addr[BR_URL_LEN - 1] = 0;
    s->addr_len = (int)strlen(s->url);
    br_render(s);
}

static void draw_button(int x, int y, int w, int h, const char *label) {
    fb_fill_rect(x, y, w, h, 0xE0E8F0);
    fb_fill_rect(x, y, w, 1, 0xFFFFFF);
    fb_fill_rect(x, y, 1, h, 0xFFFFFF);
    fb_fill_rect(x, y + h - 1, w, 1, 0x808890);
    fb_fill_rect(x + w - 1, y, 1, h, 0x808890);
    int tw = font_text_width(label);
    font_draw_string(x + (w - tw) / 2, y + (h - FONT_H) / 2, label, 0x203040, 0xE0E8F0);
}

static void br_draw(struct window *win) {
    struct browser_state *s = (struct browser_state*)win->user;
    int bx = win->x;
    int by = win->y + 22;
    int w = win->w;

    fb_fill_rect(bx, by, w, BR_MENU_H, 0xE8E8E8);
    fb_fill_rect(bx, by + BR_MENU_H - 1, w, 1, 0xA0A0A0);
    font_draw_string(bx + 6, by + 2, "File   Edit   View   History   Help   (HTTPS: no)",
                     0x202020, 0xE8E8E8);

    int ny = by + BR_MENU_H;
    fb_fill_rect(bx, ny, w, BR_NAV_H, 0xD0D8E0);
    fb_fill_rect(bx, ny + BR_NAV_H - 1, w, 1, 0x808890);

    draw_button(bx + BTN_BACK, ny + 2, BTN_W, BTN_H, "<");
    draw_button(bx + BTN_FWD,  ny + 2, BTN_W, BTN_H, ">");
    draw_button(bx + BTN_REF,  ny + 2, BTN_W, BTN_H, "R");
    draw_button(bx + BTN_HOME, ny + 2, BTN_W, BTN_H, "H");
    draw_button(bx + BTN_GO,   ny + 2, BTN_W, BTN_H, "->");

    int ax = bx + ADDR_X;
    int ay = ny + 2;
    fb_fill_rect(ax, ay, ADDR_W, ADDR_H, 0xFFFFFF);
    u32 border = s->addr_focus ? 0x3C78B4 : 0x808890;
    fb_fill_rect(ax, ay, ADDR_W, 1, border);
    fb_fill_rect(ax, ay + ADDR_H - 1, ADDR_W, 1, border);
    fb_fill_rect(ax, ay, 1, ADDR_H, border);
    fb_fill_rect(ax + ADDR_W - 1, ay, 1, ADDR_H, border);
    const char *shown = s->addr_focus ? s->addr : s->url;
    font_draw_string(ax + 4, ay + 3, shown, 0x101010, 0xFFFFFF);
    if (s->addr_focus && ((timer_ticks() / 50) & 1)) {
        int cx = ax + 4 + s->addr_len * FONT_W;
        fb_fill_rect(cx, ay + 3, FONT_W, FONT_H, 0x303030);
    }

    int cy = by + BR_CONTENT_Y;
    fb_fill_rect(bx, cy, w, BR_CONTENT_H, 0xFFFFFF);
    for (int i = 0; i < s->line_count; i++) {
        struct br_line *l = &s->lines[i];
        if (l->text[0] == 0) continue;
        u32 fg = 0x101010;
        if (l->link[0]) fg = 0x1050B0;
        if (l->is_header) fg = 0x902010;
        int ty = cy + 4 + i * FONT_H;
        font_draw_string(bx + 4, ty, l->text, fg, 0xFFFFFF);
        if (l->link[0]) {
            int tw = font_text_width(l->text);
            fb_fill_rect(bx + 4, ty + FONT_H - 1, tw, 1, 0x1050B0);
        }
    }

    int sy = by + BR_STATUS_Y;
    fb_fill_rect(bx, sy, w, BR_STATUS_H, 0xE8E8E8);
    fb_fill_rect(bx, sy, w, 1, 0xA0A0A0);
    const char *msg = s->status;
    if (timer_ticks() > s->status_until || msg[0] == 0) msg = "Done";
    font_draw_string(bx + 4, sy + 2, msg, 0x202020, 0xE8E8E8);
    char cnt[48];
    snprintf(cnt, sizeof(cnt), "Cookies: %d", cookie_jar_count());
    int cw = font_text_width(cnt);
    font_draw_string(bx + w - cw - 8, sy + 2, cnt, 0x404040, 0xE8E8E8);
}

static void br_click(struct window *win, int x, int y) {
    struct browser_state *s = (struct browser_state*)win->user;

    if (y >= BR_MENU_H && y < BR_MENU_H + BR_NAV_H) {
        if (x >= BTN_BACK && x < BTN_BACK + BTN_W) {
            if (s->hist_pos > 0) { br_set_hist(s, s->hist_pos - 1); set_status(s, "Back"); }
            return;
        }
        if (x >= BTN_FWD && x < BTN_FWD + BTN_W) {
            if (s->hist_pos < s->hist_count - 1) { br_set_hist(s, s->hist_pos + 1); set_status(s, "Forward"); }
            return;
        }
        if (x >= BTN_REF && x < BTN_REF + BTN_W) {
            br_render(s);
            set_status(s, "Refreshed");
            return;
        }
        if (x >= BTN_HOME && x < BTN_HOME + BTN_W) {
            br_push(s, "about:home");
            set_status(s, "about:home");
            return;
        }
        if (x >= BTN_GO && x < BTN_GO + BTN_W) {
            s->addr[s->addr_len] = 0;
            br_push(s, s->addr);
            set_status(s, s->addr);
            return;
        }
        if (x >= ADDR_X && x < ADDR_X + ADDR_W) {
            s->addr_focus = 1;
            strncpy(s->addr, s->url, BR_URL_LEN - 1);
            s->addr[BR_URL_LEN - 1] = 0;
            s->addr_len = (int)strlen(s->url);
            return;
        }
        return;
    }

    if (y >= BR_CONTENT_Y && y < BR_CONTENT_Y + BR_CONTENT_H) {
        s->addr_focus = 0;
        int row = (y - BR_CONTENT_Y - 4) / FONT_H;
        if (row < 0 || row >= s->line_count) return;
        if (s->lines[row].link[0] == 0) return;
        char target[BR_URL_LEN];
        strncpy(target, s->lines[row].link, BR_URL_LEN - 1);
        target[BR_URL_LEN - 1] = 0;
        if (target[0] == 0) return;
        br_push(s, target);
        set_status(s, target);
    }
}

static void br_key(struct window *win, char c) {
    struct browser_state *s = (struct browser_state*)win->user;
    if (!s->addr_focus) return;
    if (c == 0x0E) {
        if (s->addr_len > 0) { s->addr_len--; s->addr[s->addr_len] = 0; }
        return;
    }
    if (c == 0x1C || c == '\n') {
        s->addr[s->addr_len] = 0;
        br_push(s, s->addr);
        set_status(s, s->addr);
        return;
    }
    if (c >= 32 && c < 127 && s->addr_len < BR_URL_LEN - 1) {
        s->addr[s->addr_len++] = c;
        s->addr[s->addr_len] = 0;
    }
}

static struct browser_state *br_alloc(void) {
    int used[MAX_WINDOWS];
    for (int i = 0; i < MAX_WINDOWS; i++) used[i] = 0;
    for (int w = 0; w < window_count(); w++) {
        struct window *win = window_get(w);
        if (!win || !win->visible) continue;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (win->user == &g_browsers[i]) { used[i] = 1; break; }
        }
    }
    for (int i = 0; i < MAX_WINDOWS; i++) if (!used[i]) return &g_browsers[i];
    return NULL;
}

static void br_open_with(struct browser_state *s, int slot, const char *url) {
    memset(s, 0, sizeof(*s));
    s->page_buf = g_page_buf[slot];
    s->hist_pos = -1;
    s->hist_count = 0;
    strncpy(s->status, "Done", sizeof(s->status) - 1);
    br_push(s, url);
    int id = window_create(40, 25, BR_WIN_W, BR_WIN_H, "Internet Explorer");
    if (id < 0) return;
    struct window *win = window_get(id);
    win->user = s;
    win->on_draw = br_draw;
    win->on_key = br_key;
    win->on_click = br_click;
}

void apps_launch_browser(void) {
    struct browser_state *s = br_alloc();
    if (!s) return;
    int slot = (int)(s - g_browsers);
    br_open_with(s, slot, "about:home");
}

void apps_launch_browser_url(const char *url) {
    struct browser_state *s = br_alloc();
    if (!s) return;
    int slot = (int)(s - g_browsers);
    br_open_with(s, slot, url);
}