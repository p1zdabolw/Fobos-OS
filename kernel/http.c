#include "http.h"
#include "net.h"
#include "../lib/mem.h"
#include "../lib/string.h"
#include "../lib/printf.h"

struct cookie_entry {
    char host[80];
    char name[48];
    char value[128];
    int  used;
};

static struct cookie_entry g_cookies[32];
static int g_cookie_count;

void cookie_jar_init(void) {
    memset(g_cookies, 0, sizeof(g_cookies));
    g_cookie_count = 0;
}

void cookie_jar_clear(void) {
    memset(g_cookies, 0, sizeof(g_cookies));
    g_cookie_count = 0;
}

int cookie_jar_count(void) { return g_cookie_count; }

static struct cookie_entry *cookie_find(const char *host, const char *name) {
    for (int i = 0; i < 32; i++) {
        if (!g_cookies[i].used) continue;
        if (strcmp(g_cookies[i].host, host) != 0) continue;
        if (strcmp(g_cookies[i].name, name) != 0) continue;
        return &g_cookies[i];
    }
    return NULL;
}

static void cookie_store(const char *host, const char *set_cookie) {
    if (!set_cookie || !set_cookie[0]) return;
    const char *p = set_cookie;
    while (*p == ' ' || *p == '\t') p++;
    if ((p[0] == 'S' || p[0] == 's') && strncmp(p + 1, "et-Cookie:", 10) == 0) {
        p += 11;
        while (*p == ' ' || *p == '\t') p++;
    }
    char name[48];
    char value[128];
    usize n = 0;
    while (*p && *p != '=' && *p != ';' && n < sizeof(name) - 1) name[n++] = *p++;
    name[n] = 0;
    if (*p != '=') return;
    p++;
    usize v = 0;
    while (*p && *p != ';' && v < sizeof(value) - 1) value[v++] = *p++;
    value[v] = 0;
    if (n == 0) return;

    struct cookie_entry *e = cookie_find(host, name);
    if (!e) {
        for (int i = 0; i < 32; i++) {
            if (!g_cookies[i].used) { e = &g_cookies[i]; break; }
        }
    }
    if (!e) return;
    int was_used = e->used;
    memset(e, 0, sizeof(*e));
    strncpy(e->host, host, sizeof(e->host) - 1);
    strncpy(e->name, name, sizeof(e->name) - 1);
    strncpy(e->value, value, sizeof(e->value) - 1);
    e->used = 1;
    if (!was_used) g_cookie_count++;
}

static int cookie_build(const char *host, char *out, usize cap) {
    usize o = 0;
    int n = 0;
    for (int i = 0; i < 32; i++) {
        if (!g_cookies[i].used) continue;
        if (!net_host_matches(g_cookies[i].host, host)) continue;
        if (n > 0 && o + 2 < cap) { out[o++] = ';'; out[o++] = ' '; }
        const char *nm = g_cookies[i].name;
        while (*nm && o < cap - 1) out[o++] = *nm++;
        if (o < cap - 1) out[o++] = '=';
        const char *vl = g_cookies[i].value;
        while (*vl && o < cap - 1) out[o++] = *vl++;
        n++;
    }
    out[o] = 0;
    return n;
}

static void parse_url(const char *url, char *host, usize host_cap,
                      char *path, usize path_cap, u16 *port, int *use_ssl) {
    const char *p = url;
    *port = 80;
    *use_ssl = 0;
    if (strncmp(p, "https://", 8) == 0) { p += 8; *port = 443; *use_ssl = 1; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }
    usize h = 0;
    while (*p && *p != '/' && *p != ':' && h < host_cap - 1) host[h++] = *p++;
    host[h] = 0;
    if (*p == ':') {
        p++;
        u16 pn = 0;
        while (*p >= '0' && *p <= '9') { pn = pn * 10 + (u16)(*p - '0'); p++; }
        if (pn) *port = pn;
    }
    if (*p == '/') {
        usize i = 0;
        while (*p && i < path_cap - 1) path[i++] = *p++;
        path[i] = 0;
    } else {
        path[0] = '/';
        path[1] = 0;
    }
}

static const char *find_header_end(const char *s, usize len, usize *hdr_len) {
    for (usize i = 0; i + 3 < len; i++) {
        if (s[i] == '\r' && s[i+1] == '\n' && s[i+2] == '\r' && s[i+3] == '\n') {
            *hdr_len = i + 4;
            return s + i;
        }
    }
    for (usize i = 0; i + 1 < len; i++) {
        if (s[i] == '\n' && s[i+1] == '\n') {
            *hdr_len = i + 2;
            return s + i;
        }
    }
    return NULL;
}

static int parse_status_line(const char *line, int *out_code) {
    if (strncmp(line, "HTTP/", 5) != 0) return -1;
    const char *p = line + 5;
    while (*p && *p != ' ') p++;
    if (*p != ' ') return -1;
    p++;
    int code = 0;
    for (int i = 0; i < 3; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        code = code * 10 + (p[i] - '0');
    }
    *out_code = code;
    return 0;
}

static int header_value(const char *line, const char *name, char *out, usize cap) {
    usize nl = strlen(name);
    for (usize i = 0; i < nl; i++) {
        char a = line[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return -1;
    }
    const char *p = line + nl;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    usize n = 0;
    while (*p && *p != '\r' && *p != '\n' && n < cap - 1) out[n++] = *p++;
    out[n] = 0;
    return 0;
}

static int contains_token(const char *hay, const char *tok) {
    usize tl = strlen(tok);
    if (tl == 0) return 0;
    for (const char *p = hay; *p; p++) {
        usize i = 0;
        while (i < tl && p[i]) {
            char a = p[i];
            char b = tok[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            i++;
        }
        if (i == tl) return 1;
    }
    return 0;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static usize decode_chunked(const u8 *in, usize in_len, u8 *out, usize out_cap) {
    usize ip = 0;
    usize op = 0;
    while (ip < in_len) {
        u64 size = 0;
        int digits = 0;
        while (ip < in_len) {
            int h = hexval(in[ip]);
            if (h < 0) break;
            size = size * 16 + (u64)h;
            ip++;
            digits++;
            if (digits > 16) return op;
        }
        while (ip < in_len && in[ip] != '\n') ip++;
        if (ip < in_len) ip++;
        if (size == 0) break;
        usize take = (usize)size;
        if (take > in_len - ip) take = in_len - ip;
        if (op + take > out_cap) take = out_cap - op;
        if (take == 0) break;
        memcpy(out + op, in + ip, take);
        op += take;
        ip += (usize)size;
        while (ip < in_len && in[ip] != '\n') ip++;
        if (ip < in_len) ip++;
    }
    return op;
}

int http_get_raw(const char *host, u16 port, const char *path,
                 const char *extra_headers,
                 struct http_response *out, u8 *body_buf, usize body_cap) {
    memset(out, 0, sizeof(*out));
    out->body = body_buf;
    out->body_len = 0;
    out->status = -1;

    u32 ip = 0;
    int dr = net_dns_lookup(host, &ip);
    if (dr != 0) {
        kprintf("HTTP: DNS failed (%d) for %s\n", dr, host);
        return -1;
    }

    int cr = net_tcp_connect(ip, port, 6000);
    if (cr != 0) {
        kprintf("HTTP: TCP connect failed (%d)\n", cr);
        return -2;
    }

    char req[1024];
    usize rl = 0;
    const char *p1 = "GET ";
    while (*p1 && rl < sizeof(req) - 4) req[rl++] = *p1++;
    while (*path && rl < sizeof(req) - 128) req[rl++] = *path++;
    const char *p2 = " HTTP/1.1\r\nHost: ";
    while (*p2 && rl < sizeof(req) - 128) req[rl++] = *p2++;
    while (*host && rl < sizeof(req) - 128) req[rl++] = *host++;
    const char *p3 = "\r\nUser-Agent: FOS/0.2 (x86_64)\r\nAccept: text/html,*/*;q=0.1\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
    while (*p3 && rl < sizeof(req) - 128) req[rl++] = *p3++;

    char cookie_hdr[768];
    if (cookie_build(host, cookie_hdr, sizeof(cookie_hdr)) > 0) {
        const char *p4 = "Cookie: ";
        while (*p4 && rl < sizeof(req) - 32) req[rl++] = *p4++;
        const char *p5 = cookie_hdr;
        while (*p5 && rl < sizeof(req) - 4) req[rl++] = *p5++;
        const char *p6 = "\r\n";
        while (*p6 && rl < sizeof(req) - 4) req[rl++] = *p6++;
    }

    if (extra_headers && extra_headers[0]) {
        const char *p7 = extra_headers;
        while (*p7 && rl < sizeof(req) - 4) req[rl++] = *p7++;
    }

    const char *p8 = "\r\n";
    while (*p8 && rl < sizeof(req) - 4) req[rl++] = *p8++;

    net_tcp_send((const u8*)req, rl);

    u8 *buf = body_buf;
    usize total = 0;
    int recvd;
    while ((recvd = net_tcp_recv(buf + total, body_cap - total - 1, 5000)) > 0) {
        total += (usize)recvd;
        if (total >= body_cap - 1) break;
    }
    if (total < body_cap) buf[total] = 0;

    if (total == 0) {
        net_tcp_close();
        return -3;
    }

    usize hdr_len = 0;
    const char *hdr_end = find_header_end((const char*)buf, total, &hdr_len);
    if (!hdr_end) {
        net_tcp_close();
        return -4;
    }

    const char *line = (const char*)buf;
    int status = 0;
    if (parse_status_line(line, &status) == 0) out->status = status;
    while (*line && *line != '\r' && *line != '\n') line++;
    while (*line == '\r' || *line == '\n') line++;

    while (line < hdr_end && *line) {
        char val[HTTP_MAX_LOCATION];
        if (header_value(line, "Content-Type", val, sizeof(val)) == 0) {
            strncpy(out->content_type, val, sizeof(out->content_type) - 1);
        }
        if (header_value(line, "Location", val, sizeof(val)) == 0) {
            strncpy(out->location, val, sizeof(out->location) - 1);
        }
        if (header_value(line, "Set-Cookie", val, sizeof(val)) == 0) {
            if (out->cookie_count < HTTP_MAX_COOKIES) {
                strncpy(out->cookies[out->cookie_count], val, HTTP_MAX_COOKIE_LEN - 1);
                out->cookies[out->cookie_count][HTTP_MAX_COOKIE_LEN - 1] = 0;
                out->cookie_count++;
            }
            cookie_store(host, val);
        }
        if (header_value(line, "Transfer-Encoding", val, sizeof(val)) == 0) {
            if (contains_token(val, "chunked")) out->chunked = 1;
        }
        while (*line && *line != '\r' && *line != '\n') line++;
        while (*line == '\r' || *line == '\n') line++;
    }

    const u8 *raw_body = buf + hdr_len;
    usize raw_len = total - hdr_len;

    if (out->chunked) {
        u8 *decode_buf = buf + (body_cap / 2);
        usize cap2 = body_cap - (body_cap / 2) - 1;
        usize out_len = decode_chunked(raw_body, raw_len, decode_buf, cap2);
        memmove(buf, decode_buf, out_len);
        buf[out_len] = 0;
        out->body_len = out_len;
    } else {
        memmove(buf, raw_body, raw_len);
        buf[raw_len] = 0;
        out->body_len = raw_len;
    }

    net_tcp_close();
    return 0;
}

int http_get_follow(char *url_io, usize url_cap,
                    struct http_response *out, u8 *body_buf, usize body_cap) {
    char cur_url[HTTP_MAX_LOCATION * 2];
    strncpy(cur_url, url_io, sizeof(cur_url) - 1);
    cur_url[sizeof(cur_url) - 1] = 0;

    out->redirect_count = 0;

    for (int hop = 0; hop < HTTP_MAX_REDIRECTS; hop++) {
        char host[128];
        char path[384];
        u16 port = 80;
        int use_ssl = 0;
        parse_url(cur_url, host, sizeof(host), path, sizeof(path), &port, &use_ssl);

        if (use_ssl) {
            memset(out, 0, sizeof(*out));
            out->body = body_buf;
            out->body_len = 0;
            out->status = -1;
            strncpy(out->location, cur_url, sizeof(out->location) - 1);
            strncpy(url_io, cur_url, url_cap - 1);
            url_io[url_cap - 1] = 0;
            return -100;
        }

        int rc = http_get_raw(host, port, path, NULL, out, body_buf, body_cap);
        if (rc != 0) {
            strncpy(url_io, cur_url, url_cap - 1);
            url_io[url_cap - 1] = 0;
            return rc;
        }

        if (out->status >= 300 && out->status < 400 && out->location[0]) {
            if (out->redirect_count < HTTP_MAX_REDIRECTS) {
                strncpy(out->redirect_chain[out->redirect_count], cur_url,
                        HTTP_MAX_LOCATION - 1);
                out->redirect_chain[out->redirect_count][HTTP_MAX_LOCATION - 1] = 0;
                out->redirect_count++;
            }

            const char *loc = out->location;
            if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0) {
                strncpy(cur_url, loc, sizeof(cur_url) - 1);
                cur_url[sizeof(cur_url) - 1] = 0;
            } else if (loc[0] == '/') {
                char new_url[HTTP_MAX_LOCATION * 2];
                snprintf(new_url, sizeof(new_url), "http://%s%s", host, loc);
                strncpy(cur_url, new_url, sizeof(cur_url) - 1);
                cur_url[sizeof(cur_url) - 1] = 0;
            } else {
                snprintf(cur_url, sizeof(cur_url), "http://%s/%s", host, loc);
            }
            continue;
        }

        strncpy(url_io, cur_url, url_cap - 1);
        url_io[url_cap - 1] = 0;
        return 0;
    }

    strncpy(url_io, cur_url, url_cap - 1);
    url_io[url_cap - 1] = 0;
    return -101;
}

const char *http_status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}