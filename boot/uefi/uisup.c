/*
 * Fobos OS — общие экранные примитивы фазы Setup (реализация).
 */
#include "uisup.h"

#include "efi.h"
#include "lgfx.h"
#include "lin.h"

static struct EFI_SYSTEM_TABLE *g_uisup_st;

/* Геометрия бокса (defaults — как у OOBE до первого uisup_layout). */
#define UISUP_FIELD_Y_OFF 48u
#define UISUP_MSG_Y_OFF   70u

static u32 g_box_x, g_box_y, g_box_w = 480u, g_box_h = 160u;

void uisup_init(struct EFI_SYSTEM_TABLE *st)
{
    g_uisup_st = st;
}

void uisup_layout(u32 box_w, u32 box_h)
{
    u32 w = lgfx_width();
    u32 h = lgfx_height();
    g_box_w = box_w;
    g_box_h = box_h;
    g_box_x = (w > box_w) ? (w - box_w) / 2u : 0u;
    g_box_y = (h > box_h + 80u) ? (h - box_h) / 2u : 40u;
}

u32 uisup_box_x(void) { return g_box_x; }
u32 uisup_box_y(void) { return g_box_y; }
u32 uisup_box_w(void) { return g_box_w; }
u32 uisup_box_h(void) { return g_box_h; }

void uisup_header(u32 step_no, u32 step_total, const char *step_name)
{
    char line[96];
    u32 w = lgfx_width();
    u32 i = 0;
    const char *brand = " FOBOS OS SETUP";
    while (brand[i] != '\0') {
        line[i] = brand[i];
        i++;
    }
    const char *pre = "   step ";
    for (u32 k = 0; pre[k] != '\0' && i + 1u < sizeof(line); k++) {
        line[i++] = pre[k];
    }
    /* Номер шага: десятично (шагов может быть > 9). */
    char num[4];
    u32 v = step_no;
    u32 nd = 0;
    do {
        num[nd++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && nd < sizeof(num));
    for (u32 k = nd; k > 0u && i + 1u < sizeof(line); k--) {
        line[i++] = num[k - 1u];
    }
    if (i + 1u < sizeof(line)) {
        line[i++] = '/';
    }
    v = step_total;
    nd = 0;
    do {
        num[nd++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && nd < sizeof(num));
    for (u32 k = nd; k > 0u && i + 1u < sizeof(line); k--) {
        line[i++] = num[k - 1u];
    }
    if (i + 1u < sizeof(line)) {
        line[i++] = ' ';
    }
    if (step_name != NULL) {
        for (u32 k = 0; step_name[k] != '\0' && i + 1u < sizeof(line); k++) {
            line[i++] = step_name[k];
        }
    }
    line[i] = '\0';

    lgfx_fill_rect(0, 0, w, 24u, LGFX_COL_PANEL);
    lgfx_fill_rect(0, 24u, w, 1u, LGFX_COL_BORDER);
    lgfx_text(8u, 8u, line, LGFX_COL_TX_DIM, LGFX_COL_PANEL);
}

void uisup_footer(const char *hints)
{
    u32 h = lgfx_height();
    if (hints != NULL) {
        lgfx_text_centered(h - 28u, hints, LGFX_COL_HEADER_TX, LGFX_COL_BG_TOP);
    }
}

void uisup_wrap2(const char *src, u32 max_chars,
                 char *l1, u32 l1_cap, char *l2, u32 l2_cap)
{
    u32 n = 0;
    while (src != NULL && src[n] != '\0') {
        n++;
    }

    l1[0] = '\0';
    l2[0] = '\0';
    if (n <= max_chars) {
        for (u32 i = 0; i < n && i + 1u < l1_cap; i++) {
            l1[i] = src[i];
        }
        l1[(n < l1_cap) ? n : l1_cap - 1u] = '\0';
        return;
    }

    /* Ищем последний пробел в первых max_chars символах. */
    u32 cut = max_chars;
    for (u32 i = max_chars; i > 0u; i--) {
        if (src[i - 1u] == ' ') {
            cut = i - 1u;
            break;
        }
    }
    if (cut == 0u) {
        cut = max_chars;   /* пробелов нет — жёсткий разрез */
    }

    for (u32 i = 0; i < cut && i + 1u < l1_cap; i++) {
        l1[i] = src[i];
    }
    l1[(cut < l1_cap) ? cut : l1_cap - 1u] = '\0';

    u32 rest = n - cut;
    if (src[cut] == ' ') {
        cut++;
        rest--;
    }
    for (u32 i = 0; i < rest && i + 1u < l2_cap; i++) {
        l2[i] = src[cut + i];
    }
    l2[(rest < l2_cap) ? rest : l2_cap - 1u] = '\0';
}

void uisup_screen(u32 step_no, u32 step_total, const char *step_name,
                  const char *title, const char *subtitle,
                  const char *hints)
{
    lgfx_background();
    uisup_header(step_no, step_total, step_name);
    lgfx_box(g_box_x, g_box_y, g_box_w, g_box_h, LGFX_COL_PANEL, LGFX_COL_BORDER);
    lgfx_text(g_box_x + 16u, g_box_y + 12u, title, LGFX_COL_TX, LGFX_COL_PANEL);
    if (subtitle != NULL && subtitle[0] != '\0') {
        /* (box_w - 32) px / 8 px на символ. */
        char l1[96], l2[96];
        uisup_wrap2(subtitle, (g_box_w - 32u) / 8u, l1, sizeof(l1),
                    l2, sizeof(l2));
        lgfx_text(g_box_x + 16u, g_box_y + 28u, l1,
                  LGFX_COL_HEADER_TX, LGFX_COL_PANEL);
        if (l2[0] != '\0') {
            lgfx_text(g_box_x + 16u, g_box_y + 44u, l2,
                      LGFX_COL_HEADER_TX, LGFX_COL_PANEL);
        }
    }
    uisup_footer(hints);
}

int uisup_edit(char *buf, u32 cap, int masked)
{
    u32 len = 0;
    while (buf != NULL && buf[len] != '\0') {
        len++;
    }
    u32 field_y = g_box_y + UISUP_FIELD_Y_OFF;

    for (;;) {
        lgfx_input_field(g_box_x + 16u, field_y, g_box_w - 32u, buf, len, 1,
                         masked);

        u16 scan = 0, ch = 0;
        if (lin_poll_key(&scan, &ch) != 0) {
            if (g_uisup_st != NULL) {
                g_uisup_st->boot_services->stall(1000u);
            }
            continue;
        }

        if (scan == EFI_SCAN_ESC) {
            return -1;
        }
        if (ch == 0x0Du || ch == 0x0Au) {
            lgfx_fill_rect(g_box_x + 16u, field_y + 20u, g_box_w - 32u, 10u,
                           LGFX_COL_PANEL);
            return 0;
        }
        if (ch == 0x08u) {
            if (len > 0u) {
                len--;
                buf[len] = '\0';
            }
            continue;
        }
        if (ch >= 0x20u && ch <= 0x7Eu) {
            if (len + 1u < cap) {
                buf[len++] = (char)ch;
                buf[len] = '\0';
            }
            continue;
        }
    }
}

void uisup_error(const char *msg)
{
    lgfx_text(g_box_x + 16u, g_box_y + UISUP_MSG_Y_OFF, msg, LGFX_COL_ERR,
              LGFX_COL_PANEL);
}
