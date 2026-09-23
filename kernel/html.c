#include "html.h"
#include "../lib/mem.h"
#include "../lib/string.h"
#include "../lib/printf.h"

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int ci_match(const char *s, const char *kw) {
    while (*kw) {
        char a = *s;
        char b = *kw;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
        s++; kw++;
    }
    return 1;
}

static void push_char(char *buf, int *len, int cap, char c) {
    if (*len < cap - 1) buf[(*len)++] = c;
}

static void push_str(char *buf, int *len, int cap, const char *s) {
    while (*s) push_char(buf, len, cap, *s++);
}

static void push_num(char *buf, int *len, int cap, int n) {
    char tmp[16];
    int t = 0;
    if (n == 0) tmp[t++] = '0';
    while (n > 0) { tmp[t++] = (char)('0' + n % 10); n /= 10; }
    while (t > 0) push_char(buf, len, cap, tmp[--t]);
}

static int extract_title(const char *html, usize len, char *out, usize cap) {
    usize i = 0;
    while (i + 6 < len) {
        if (html[i] == '<' && ci_match(html + i + 1, "title")) {
            usize j = i + 6;
            while (j < len && html[j] != '>') j++;
            if (j < len) j++;
            usize k = 0;
            while (j < len && k < cap - 1 &&
                   !(html[j] == '<' && ci_match(html + j + 1, "/title"))) {
                out[k++] = html[j++];
            }
            while (k > 0 && is_space(out[k-1])) k--;
            out[k] = 0;
            return 1;
        }
        i++;
    }
    out[0] = 0;
    return 0;
}

static void set_base_url(const char *url, struct html_doc *doc) {
    if (!url || !url[0]) return;
    const char *p = url;
    const char *sc = "http";
    if (strncmp(p, "https://", 8) == 0) { sc = "https"; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }
    strncpy(doc->base_scheme, sc, sizeof(doc->base_scheme) - 1);
    usize h = 0;
    while (*p && *p != '/' && *p != ':' && h < sizeof(doc->base_host) - 1) {
        doc->base_host[h++] = *p++;
    }
    doc->base_host[h] = 0;
}

static void absolutize_link(char *link, usize cap, const struct html_doc *doc) {
    if (!link[0]) return;
    if (strncmp(link, "http://", 7) == 0) return;
    if (strncmp(link, "https://", 8) == 0) return;
    if (strncmp(link, "about:", 6) == 0) return;
    if (strncmp(link, "mailto:", 7) == 0) return;
    if (strncmp(link, "javascript:", 11) == 0) { link[0] = 0; return; }
    if (link[0] == '#') { link[0] = 0; return; }

    char tmp[HTML_LINK_LEN];
    if (link[0] == '/') {
        snprintf(tmp, sizeof(tmp), "http://%s%s", doc->base_host, link);
    } else {
        snprintf(tmp, sizeof(tmp), "http://%s/%s", doc->base_host, link);
    }
    strncpy(link, tmp, cap - 1);
    link[cap - 1] = 0;
}

struct parse_state {
    char text[512];
    int  tlen;
    char link[HTML_LINK_LEN];
    int  in_anchor;
    int  in_script;
    int  in_style;
    int  in_pre;
    int  header_level;
    int  list_stack[HTML_LIST_DEPTH];
    int  list_count[HTML_LIST_DEPTH];
    int  list_depth;
    int  blockquote_depth;
    int  pending_break;
};

static void indent_prefix(struct parse_state *st, char *out, int cap, int *out_len) {
    int n = 0;
    for (int i = 0; i < st->blockquote_depth; i++) {
        if (n < cap - 1) out[n++] = ' ';
        if (n < cap - 1) out[n++] = ' ';
    }
    for (int i = 0; i < st->list_depth; i++) {
        if (n < cap - 1) out[n++] = ' ';
        if (n < cap - 1) out[n++] = ' ';
    }
    out[n] = 0;
    *out_len = n;
}

static void flush_line(struct html_doc *doc, struct parse_state *st, int force) {
    if (st->tlen == 0 && !force) return;
    while (st->tlen > 0 && is_space(st->text[st->tlen - 1])) st->tlen--;
    if (st->tlen == 0 && !force) return;

    if (doc->line_count >= HTML_MAX_LINES) {
        doc->truncated = 1;
        st->tlen = 0;
        return;
    }

    char indent[64];
    int indent_len = 0;
    indent_prefix(st, indent, sizeof(indent), &indent_len);

    struct html_line *l = &doc->lines[doc->line_count++];
    memset(l, 0, sizeof(*l));

    int pos = 0;
    for (int i = 0; i < indent_len && pos < HTML_LINE_LEN - 1; i++) {
        l->text[pos++] = indent[i];
    }
    int copy = st->tlen;
    if (copy > HTML_LINE_LEN - 1 - pos) copy = HTML_LINE_LEN - 1 - pos;
    if (copy < 0) copy = 0;
    memcpy(l->text + pos, st->text, (usize)copy);
    l->text[pos + copy] = 0;

    l->is_header = st->header_level;
    l->is_pre = st->in_pre;
    if (st->in_anchor && st->link[0]) {
        strncpy(l->link, st->link, HTML_LINK_LEN - 1);
        l->is_link = 1;
    }

    st->tlen = 0;
}

static void flush_list_item(struct html_doc *doc, struct parse_state *st, int force) {
    if (st->tlen == 0 && !force) return;
    while (st->tlen > 0 && is_space(st->text[st->tlen - 1])) st->tlen--;
    if (st->tlen == 0 && !force) return;

    if (doc->line_count >= HTML_MAX_LINES) {
        doc->truncated = 1;
        st->tlen = 0;
        return;
    }

    char indent[64];
    int indent_len = 0;
    indent_prefix(st, indent, sizeof(indent), &indent_len);

    char marker[16];
    int mlen = 0;
    if (st->list_depth > 0) {
        int kind = st->list_stack[st->list_depth - 1];
        if (kind == 1) {
            st->list_count[st->list_depth - 1]++;
            push_num(marker, &mlen, sizeof(marker), st->list_count[st->list_depth - 1]);
            push_char(marker, &mlen, sizeof(marker), '.');
            push_char(marker, &mlen, sizeof(marker), ' ');
        } else {
            marker[0] = '*';
            marker[1] = ' ';
            mlen = 2;
        }
    }

    struct html_line *l = &doc->lines[doc->line_count++];
    memset(l, 0, sizeof(*l));

    int pos = 0;
    for (int i = 0; i < indent_len && pos < HTML_LINE_LEN - 1; i++) {
        l->text[pos++] = indent[i];
    }
    for (int i = 0; i < mlen && pos < HTML_LINE_LEN - 1; i++) {
        l->text[pos++] = marker[i];
    }
    int copy = st->tlen;
    if (copy > HTML_LINE_LEN - 1 - pos) copy = HTML_LINE_LEN - 1 - pos;
    if (copy < 0) copy = 0;
    memcpy(l->text + pos, st->text, (usize)copy);
    l->text[pos + copy] = 0;

    l->is_header = st->header_level;
    if (st->in_anchor && st->link[0]) {
        strncpy(l->link, st->link, HTML_LINK_LEN - 1);
        l->is_link = 1;
    }

    st->tlen = 0;
}

static void decode_entity(const char *src, int *consumed, char *out, int *olen) {
    *consumed = 0;
    if (src[0] != '&') return;
    if (ci_match(src, "&amp;"))  { out[(*olen)++] = '&';  *consumed = 5; return; }
    if (ci_match(src, "&lt;"))   { out[(*olen)++] = '<';  *consumed = 4; return; }
    if (ci_match(src, "&gt;"))   { out[(*olen)++] = '>';  *consumed = 4; return; }
    if (ci_match(src, "&quot;")) { out[(*olen)++] = '"';  *consumed = 6; return; }
    if (ci_match(src, "&#39;"))  { out[(*olen)++] = '\''; *consumed = 5; return; }
    if (ci_match(src, "&apos;")) { out[(*olen)++] = '\''; *consumed = 6; return; }
    if (ci_match(src, "&nbsp;")) { out[(*olen)++] = ' ';  *consumed = 6; return; }
    if (ci_match(src, "&mdash;")){ out[(*olen)++] = '-';  *consumed = 7; return; }
    if (ci_match(src, "&ndash;")){ out[(*olen)++] = '-';  *consumed = 7; return; }
    if (ci_match(src, "&hellip;")){out[(*olen)++] = '.';  *consumed = 8; return; }
    const char *p = src + 1;
    if (*p == '#') {
        p++;
        int hex = 0;
        if (*p == 'x' || *p == 'X') { hex = 1; p++; }
        int v = 0;
        int digits = 0;
        while (*p && *p != ';' && digits < 8) {
            int d;
            if (*p >= '0' && *p <= '9') d = *p - '0';
            else if (hex && *p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
            else if (hex && *p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
            else break;
            v = v * (hex ? 16 : 10) + d;
            p++; digits++;
        }
        if (*p == ';' && digits > 0 && v > 0 && v < 128) {
            out[(*olen)++] = (char)v;
            *consumed = (int)(p - src) + 1;
        }
    }
}

int html_parse(const char *html, usize len, const char *base_url, struct html_doc *out) {
    memset(out, 0, sizeof(*out));
    extract_title(html, len, out->title, sizeof(out->title));
    set_base_url(base_url, out);

    struct parse_state st;
    memset(&st, 0, sizeof(st));
    st.list_depth = 0;
    st.blockquote_depth = 0;

    usize i = 0;
    while (i < len) {
        char c = html[i];

        if (c == '<') {
            if (i + 3 < len && html[i+1] == '!' && html[i+2] == '-' && html[i+3] == '-') {
                i += 4;
                while (i + 2 < len && !(html[i] == '-' && html[i+1] == '-' && html[i+2] == '>')) i++;
                i += 3;
                continue;
            }
            if (i + 8 < len && html[i+1] == '!' && ci_match(html + i + 2, "DOCTYPE")) {
                while (i < len && html[i] != '>') i++;
                if (i < len) i++;
                continue;
            }

            usize j = i + 1;
            int closing = 0;
            if (j < len && html[j] == '/') { closing = 1; j++; }
            while (j < len && is_space(html[j])) j++;

            if (!closing && ci_match(html + j, "script")) { st.in_script = 1; }
            else if (closing && ci_match(html + j, "script")) { st.in_script = 0; }
            else if (!closing && ci_match(html + j, "style")) { st.in_style = 1; }
            else if (closing && ci_match(html + j, "style")) { st.in_style = 0; }
            else if (!closing && (ci_match(html + j, "head") || ci_match(html + j, "title") ||
                                  ci_match(html + j, "meta") || ci_match(html + j, "link"))) {
                if (ci_match(html + j, "head") || ci_match(html + j, "title")) { }
            }
            else if (!closing && (ci_match(html + j, "p") || ci_match(html + j, "br") ||
                                  ci_match(html + j, "div") || ci_match(html + j, "section") ||
                                  ci_match(html + j, "article") || ci_match(html + j, "header") ||
                                  ci_match(html + j, "footer") || ci_match(html + j, "nav") ||
                                  ci_match(html + j, "main") || ci_match(html + j, "aside"))) {
                flush_line(out, &st, 0);
                st.pending_break = 1;
            }
            else if (closing && (ci_match(html + j, "p") || ci_match(html + j, "div") ||
                                 ci_match(html + j, "section") || ci_match(html + j, "article") ||
                                 ci_match(html + j, "header") || ci_match(html + j, "footer") ||
                                 ci_match(html + j, "nav") || ci_match(html + j, "main") ||
                                 ci_match(html + j, "aside"))) {
                flush_line(out, &st, 0);
                st.pending_break = 1;
            }
            else if (!closing && (ci_match(html + j, "h1") || ci_match(html + j, "h2") ||
                                  ci_match(html + j, "h3") || ci_match(html + j, "h4") ||
                                  ci_match(html + j, "h5") || ci_match(html + j, "h6"))) {
                flush_line(out, &st, 0);
                st.header_level = 1;
            }
            else if (closing && (ci_match(html + j, "h1") || ci_match(html + j, "h2") ||
                                 ci_match(html + j, "h3") || ci_match(html + j, "h4") ||
                                 ci_match(html + j, "h5") || ci_match(html + j, "h6"))) {
                flush_line(out, &st, 0);
                st.header_level = 0;
                st.pending_break = 1;
            }
            else if (!closing && (ci_match(html + j, "ul") || ci_match(html + j, "menu"))) {
                flush_line(out, &st, 0);
                if (st.list_depth < HTML_LIST_DEPTH) {
                    st.list_stack[st.list_depth] = 0;
                    st.list_count[st.list_depth] = 0;
                    st.list_depth++;
                }
            }
            else if (closing && (ci_match(html + j, "ul") || ci_match(html + j, "menu"))) {
                flush_line(out, &st, 0);
                if (st.list_depth > 0) st.list_depth--;
                st.pending_break = 1;
            }
            else if (!closing && (ci_match(html + j, "ol") || ci_match(html + j, "dir"))) {
                flush_line(out, &st, 0);
                if (st.list_depth < HTML_LIST_DEPTH) {
                    st.list_stack[st.list_depth] = 1;
                    st.list_count[st.list_depth] = 0;
                    st.list_depth++;
                }
            }
            else if (closing && (ci_match(html + j, "ol") || ci_match(html + j, "dir"))) {
                flush_line(out, &st, 0);
                if (st.list_depth > 0) st.list_depth--;
                st.pending_break = 1;
            }
            else if (!closing && ci_match(html + j, "li")) {
                flush_line(out, &st, 0);
                st.pending_break = 0;
            }
            else if (closing && ci_match(html + j, "li")) {
                flush_list_item(out, &st, 0);
                st.pending_break = 1;
            }
            else if (!closing && ci_match(html + j, "pre")) {
                flush_line(out, &st, 0);
                st.in_pre = 1;
            }
            else if (closing && ci_match(html + j, "pre")) {
                flush_line(out, &st, 0);
                st.in_pre = 0;
                st.pending_break = 1;
            }
            else if (!closing && ci_match(html + j, "hr")) {
                flush_line(out, &st, 0);
                if (out->line_count < HTML_MAX_LINES) {
                    struct html_line *l = &out->lines[out->line_count++];
                    memset(l, 0, sizeof(*l));
                    for (int k = 0; k < HTML_LINE_LEN - 1; k++) l->text[k] = '-';
                    l->text[HTML_LINE_LEN - 1] = 0;
                }
                st.pending_break = 1;
            }
            else if (!closing && ci_match(html + j, "blockquote")) {
                flush_line(out, &st, 0);
                if (st.blockquote_depth < 4) st.blockquote_depth++;
            }
            else if (closing && ci_match(html + j, "blockquote")) {
                flush_line(out, &st, 0);
                if (st.blockquote_depth > 0) st.blockquote_depth--;
                st.pending_break = 1;
            }
            else if (!closing && ci_match(html + j, "tr")) {
                flush_line(out, &st, 0);
            }
            else if (closing && ci_match(html + j, "tr")) {
                flush_line(out, &st, 0);
            }
            else if ((!closing && (ci_match(html + j, "td") || ci_match(html + j, "th"))) ||
                     (closing && (ci_match(html + j, "td") || ci_match(html + j, "th")))) {
                if (st.tlen > 0) push_str(st.text, &st.tlen, sizeof(st.text), " | ");
            }
            else if (!closing && ci_match(html + j, "img")) {
                usize k = j + 3;
                char alt[128];
                alt[0] = 0;
                while (k < len && html[k] != '>') {
                    if ((html[k] == 'a' || html[k] == 'A') && ci_match(html + k, "alt")) {
                        usize h = k + 3;
                        while (h < len && (html[h] == ' ' || html[h] == '=')) h++;
                        char q = 0;
                        if (h < len && (html[h] == '"' || html[h] == '\'')) { q = html[h]; h++; }
                        usize l = 0;
                        while (h < len && l < sizeof(alt) - 1) {
                            if (q && html[h] == q) break;
                            if (!q && (html[h] == ' ' || html[h] == '>')) break;
                            alt[l++] = html[h++];
                        }
                        alt[l] = 0;
                        break;
                    }
                    k++;
                }
                push_str(st.text, &st.tlen, sizeof(st.text), "[Image");
                if (alt[0]) {
                    push_str(st.text, &st.tlen, sizeof(st.text), ": ");
                    push_str(st.text, &st.tlen, sizeof(st.text), alt);
                }
                push_char(st.text, &st.tlen, sizeof(st.text), ']');
            }
            else if (!closing && ci_match(html + j, "a")) {
                st.in_anchor = 1;
                st.link[0] = 0;
                usize k = j + 1;
                while (k < len && html[k] != '>') {
                    if ((html[k] == 'h' || html[k] == 'H') && ci_match(html + k, "href")) {
                        usize h = k + 4;
                        while (h < len && (html[h] == ' ' || html[h] == '=')) h++;
                        char q = 0;
                        if (h < len && (html[h] == '"' || html[h] == '\'')) { q = html[h]; h++; }
                        usize l = 0;
                        while (h < len && l < HTML_LINK_LEN - 1) {
                            if (q && html[h] == q) break;
                            if (!q && (html[h] == ' ' || html[h] == '>')) break;
                            st.link[l++] = html[h++];
                        }
                        st.link[l] = 0;
                        break;
                    }
                    k++;
                }
                if (st.link[0]) absolutize_link(st.link, HTML_LINK_LEN, out);
            }
            else if (closing && ci_match(html + j, "a")) {
                if (st.tlen > 0) flush_line(out, &st, 0);
                st.in_anchor = 0;
                st.link[0] = 0;
            }

            while (i < len && html[i] != '>') i++;
            if (i < len) i++;
            continue;
        }

        if (st.in_script || st.in_style) { i++; continue; }

        if (c == '&') {
            char dec = 0;
            int consumed = 0;
            decode_entity(html + i, &consumed, &dec, &st.tlen);
            if (consumed > 0 && st.tlen < (int)sizeof(st.text) - 1) {
                st.text[st.tlen] = dec;
                st.tlen++;
                i += (usize)consumed;
                continue;
            }
        }

        if (st.in_pre) {
            if (c == '\n') {
                flush_line(out, &st, 0);
            } else if (c == '\r') {
            } else {
                push_char(st.text, &st.tlen, sizeof(st.text), c);
            }
            i++;
            continue;
        }

        if (is_space(c)) {
            if (st.tlen > 0 && !is_space(st.text[st.tlen-1])) {
                push_char(st.text, &st.tlen, sizeof(st.text), ' ');
            }
            i++;
            continue;
        }

        if (st.tlen >= (int)sizeof(st.text) - 1) {
            flush_line(out, &st, 0);
        }
        push_char(st.text, &st.tlen, sizeof(st.text), c);
        i++;
    }

    if (st.tlen > 0) flush_line(out, &st, 0);
    return out->line_count;
}