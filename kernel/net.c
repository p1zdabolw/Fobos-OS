#include "net.h"
#include "rtl8139.h"
#include "timer.h"
#include "../lib/mem.h"
#include "../lib/string.h"
#include "../lib/printf.h"

#define ETH_ARP   0x0806
#define ETH_IPV4  0x0800

#define ARP_REQ   1
#define ARP_REP   2

#define IP_ICMP   1
#define IP_TCP    6
#define IP_UDP    17

#define ICMP_ECHO_REQ 8
#define ICMP_ECHO_REP 0

#define TCP_RX_SIZE 65536

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

enum {
    TCP_ST_CLOSED = 0,
    TCP_ST_SYN_SENT,
    TCP_ST_ESTABLISHED,
    TCP_ST_CLOSE_WAIT,
    TCP_ST_LAST_ACK
};

static u32 g_our_ip;
static u32 g_gateway;
static u32 g_dns;
static u8  g_mac[6];
static int g_ready;

static u8  g_last_mac[6];
static u32 g_last_ip;
static int g_last_valid;

static volatile int g_ping_reply;
static u32 g_ping_dest;
static u64 g_ping_sent;
static u64 g_ping_rtt;

static u16 g_dns_txid;

static u16 g_udp_our_port;
static volatile int g_udp_got;
static u16 g_udp_recv_port;
static u8  g_udp_buf[2048];
static usize g_udp_len;

static int  g_tcp_state;
static u32  g_tcp_remote_ip;
static u16  g_tcp_remote_port;
static u16  g_tcp_local_port;
static u32  g_tcp_snd_nxt;
static u32  g_tcp_rcv_nxt;
static u32  g_tcp_initial_snd;
static u8   g_tcp_rx[TCP_RX_SIZE];
static usize g_tcp_rx_len;
static int  g_tcp_got_fin;

static u16 rd16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 rd32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3]; }
static void wr16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void wr32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

static u32 ip_from(u8 a, u8 b, u8 c, u8 d) {
    return ((u32)a << 24) | ((u32)b << 16) | ((u32)c << 8) | (u32)d;
}

static u32 csum_add(u32 acc, const u8 *data, usize len) {
    usize i = 0;
    while (i + 1 < len) {
        acc += ((u32)data[i] << 8) | data[i + 1];
        i += 2;
    }
    if (i < len) acc += (u32)data[i] << 8;
    return acc;
}

static u16 csum_finish(u32 acc) {
    while (acc >> 16) acc = (acc & 0xFFFFu) + (acc >> 16);
    return (u16)~acc;
}

static u16 csum16(const u8 *data, usize len) {
    return csum_finish(csum_add(0, data, len));
}

static void cache_arp(u32 ip, const u8 *mac) {
    memcpy(g_last_mac, mac, 6);
    g_last_ip = ip;
    g_last_valid = 1;
}

static int lookup_arp(u32 ip, u8 *out_mac) {
    if (!g_last_valid) return 0;
    if (g_last_ip != ip) return 0;
    memcpy(out_mac, g_last_mac, 6);
    return 1;
}

static void send_arp_request(u32 target) {
    u8 f[42];
    memset(f, 0, sizeof(f));
    for (int i = 0; i < 6; i++) f[i] = 0xFF;
    memcpy(f + 6, g_mac, 6);
    wr16(f + 12, ETH_ARP);
    wr16(f + 14, 0x0001);
    wr16(f + 16, ETH_IPV4);
    f[18] = 6;
    f[19] = 4;
    wr16(f + 20, ARP_REQ);
    memcpy(f + 22, g_mac, 6);
    wr32(f + 28, g_our_ip);
    memset(f + 32, 0, 6);
    wr32(f + 38, target);
    rtl8139_send(f, 42);
}

static void send_ipv4(u32 dst, u8 proto, const u8 *payload, usize plen) {
    u8 frame[14 + 20 + 1500];
    if (plen + 20 + 14 > sizeof(frame)) return;

    u8 dmac[6];
    u32 next_hop;
    if ((g_our_ip & 0xFFFFFF00u) == (dst & 0xFFFFFF00u)) next_hop = dst;
    else next_hop = g_gateway;

    if (!lookup_arp(next_hop, dmac)) {
        send_arp_request(next_hop);
        return;
    }

    memcpy(frame + 0, dmac, 6);
    memcpy(frame + 6, g_mac, 6);
    wr16(frame + 12, ETH_IPV4);

    u8 *ip = frame + 14;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    ip[1] = 0;
    wr16(ip + 2, (u16)(20 + plen));
    wr16(ip + 4, 0);
    wr16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = proto;
    wr16(ip + 10, 0);
    wr32(ip + 12, g_our_ip);
    wr32(ip + 16, dst);
    wr16(ip + 10, csum16(ip, 20));
    memcpy(ip + 20, payload, plen);

    rtl8139_send(frame, 14 + 20 + plen);
}

static void send_icmp_echo(u32 dst) {
    u8 pkt[8 + 32];
    pkt[0] = ICMP_ECHO_REQ;
    pkt[1] = 0;
    wr16(pkt + 2, 0);
    wr16(pkt + 4, 0x1234);
    wr16(pkt + 6, 1);
    for (int i = 0; i < 32; i++) pkt[8 + i] = (u8)('a' + (i % 26));
    wr16(pkt + 2, csum16(pkt, sizeof(pkt)));
    send_ipv4(dst, IP_ICMP, pkt, sizeof(pkt));
}

static void handle_arp(const u8 *pkt, usize len) {
    if (len < 28) return;
    u16 oper = rd16(pkt + 6);
    u32 spa = rd32(pkt + 14);
    u32 tpa = rd32(pkt + 24);
    if (oper == ARP_REQ) {
        cache_arp(spa, pkt + 8);
        if (tpa == g_our_ip) {
            u8 r[42];
            memcpy(r + 0, pkt + 8, 6);
            memcpy(r + 6, g_mac, 6);
            wr16(r + 12, ETH_ARP);
            wr16(r + 14, 0x0001);
            wr16(r + 16, ETH_IPV4);
            r[18] = 6;
            r[19] = 4;
            wr16(r + 20, ARP_REP);
            memcpy(r + 22, g_mac, 6);
            wr32(r + 28, g_our_ip);
            memcpy(r + 32, pkt + 8, 6);
            wr32(r + 38, spa);
            rtl8139_send(r, 42);
        }
    } else if (oper == ARP_REP) {
        cache_arp(spa, pkt + 8);
    }
}

static void handle_icmp(const u8 *pkt, usize len, u32 src) {
    if (len < 8) return;
    u8 type = pkt[0];
    if (type == ICMP_ECHO_REP) {
        if (src == g_ping_dest) {
            g_ping_rtt = (timer_ticks() - g_ping_sent) * 10;
            g_ping_reply = 1;
        }
    } else if (type == ICMP_ECHO_REQ) {
        u8 reply[8 + 32];
        if (len > sizeof(reply)) return;
        memcpy(reply, pkt, len);
        reply[0] = ICMP_ECHO_REP;
        wr16(reply + 2, 0);
        wr16(reply + 2, csum16(reply, len));
        send_ipv4(src, IP_ICMP, reply, len);
    }
}

static void udp_send(u16 src_port, u32 dst_ip, u16 dst_port, const u8 *data, usize len) {
    u8 pkt[8 + 1472];
    if (len > 1472) return;
    wr16(pkt + 0, src_port);
    wr16(pkt + 2, dst_port);
    wr16(pkt + 4, (u16)(8 + len));
    wr16(pkt + 6, 0);
    memcpy(pkt + 8, data, len);

    u8 pseudo[12];
    wr32(pseudo + 0, g_our_ip);
    wr32(pseudo + 4, dst_ip);
    pseudo[8] = 0;
    pseudo[9] = IP_UDP;
    wr16(pseudo + 10, (u16)(8 + len));
    u32 acc = 0;
    acc = csum_add(acc, pseudo, 12);
    acc = csum_add(acc, pkt, 8 + len);
    u16 cs = csum_finish(acc);
    if (cs == 0) cs = 0xFFFF;
    wr16(pkt + 6, cs);

    send_ipv4(dst_ip, IP_UDP, pkt, 8 + len);
}

static void handle_udp(const u8 *pkt, usize len, u32 src_ip) {
    (void)src_ip;
    if (len < 8) return;
    u16 dst_port = rd16(pkt + 2);
    if (dst_port != g_udp_our_port) return;
    u16 ulen = rd16(pkt + 4);
    if (ulen < 8 || ulen > len) return;
    usize payload_len = ulen - 8;
    if (payload_len > sizeof(g_udp_buf)) payload_len = sizeof(g_udp_buf);
    memcpy(g_udp_buf, pkt + 8, payload_len);
    g_udp_len = payload_len;
    g_udp_recv_port = dst_port;
    g_udp_got = 1;
}

static void tcp_send_segment(u8 flags, const u8 *data, usize len) {
    u8 pkt[20 + 1500];
    if (len > 1500) return;
    wr16(pkt + 0, g_tcp_local_port);
    wr16(pkt + 2, g_tcp_remote_port);
    wr32(pkt + 4, g_tcp_snd_nxt);
    wr32(pkt + 8, g_tcp_rcv_nxt);
    pkt[12] = 5 << 4;
    pkt[13] = flags;
    wr16(pkt + 14, 65535);
    wr16(pkt + 16, 0);
    wr16(pkt + 18, 0);
    if (len) memcpy(pkt + 20, data, len);

    u8 pseudo[12];
    wr32(pseudo + 0, g_our_ip);
    wr32(pseudo + 4, g_tcp_remote_ip);
    pseudo[8] = 0;
    pseudo[9] = IP_TCP;
    wr16(pseudo + 10, (u16)(20 + len));
    u32 acc = 0;
    acc = csum_add(acc, pseudo, 12);
    acc = csum_add(acc, pkt, 20 + len);
    wr16(pkt + 16, csum_finish(acc));

    send_ipv4(g_tcp_remote_ip, IP_TCP, pkt, 20 + len);
}

static void handle_tcp(const u8 *pkt, usize len, u32 src_ip) {
    if (len < 20) return;
    u16 src_port = rd16(pkt + 0);
    u16 dst_port = rd16(pkt + 2);
    u32 seq = rd32(pkt + 4);
    u8  data_off = (u8)((pkt[12] >> 4) * 4);
    u8  flags = pkt[13];

    if (g_tcp_state == TCP_ST_CLOSED) return;
    if (src_ip != g_tcp_remote_ip) return;
    if (src_port != g_tcp_remote_port) return;
    if (dst_port != g_tcp_local_port) return;
    if (data_off < 20 || data_off > len) return;

    const u8 *data = pkt + data_off;
    usize data_len = len - data_off;

    if (flags & TCP_RST) {
        g_tcp_state = TCP_ST_CLOSED;
        return;
    }

    if (g_tcp_state == TCP_ST_SYN_SENT) {
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            g_tcp_rcv_nxt = seq + 1;
            g_tcp_snd_nxt = g_tcp_initial_snd + 1;
            g_tcp_state = TCP_ST_ESTABLISHED;
            tcp_send_segment(TCP_ACK, NULL, 0);
        }
        return;
    }

    if (g_tcp_state == TCP_ST_ESTABLISHED || g_tcp_state == TCP_ST_CLOSE_WAIT) {
        if (data_len > 0) {
            if (seq == g_tcp_rcv_nxt) {
                usize space = TCP_RX_SIZE - g_tcp_rx_len;
                usize n = data_len > space ? space : data_len;
                memcpy(g_tcp_rx + g_tcp_rx_len, data, n);
                g_tcp_rx_len += n;
                g_tcp_rcv_nxt += (u32)data_len;
            }
            tcp_send_segment(TCP_ACK, NULL, 0);
        }
        if (flags & TCP_FIN) {
            g_tcp_rcv_nxt++;
            g_tcp_got_fin = 1;
            if (g_tcp_state == TCP_ST_ESTABLISHED) g_tcp_state = TCP_ST_CLOSE_WAIT;
            tcp_send_segment(TCP_ACK, NULL, 0);
        }
    }
}

static void handle_ipv4(const u8 *pkt, usize len) {
    if (len < 20) return;
    u8 ihl = (u8)((pkt[0] & 0x0F) * 4);
    if (ihl < 20 || (usize)ihl > len) return;
    u8 proto = pkt[9];
    u32 dst = rd32(pkt + 16);
    if (dst != g_our_ip && dst != 0xFFFFFFFFu) return;
    u32 src = rd32(pkt + 12);
    const u8 *payload = pkt + ihl;
    usize plen = len - ihl;
    if (proto == IP_ICMP)      handle_icmp(payload, plen, src);
    else if (proto == IP_UDP)  handle_udp(payload, plen, src);
    else if (proto == IP_TCP)  handle_tcp(payload, plen, src);
}

void net_rx_frame(const u8 *frame, usize len) {
    if (len < 14) return;
    u16 type = rd16(frame + 12);
    if (type == ETH_ARP)       handle_arp(frame + 14, len - 14);
    else if (type == ETH_IPV4) handle_ipv4(frame + 14, len - 14);
}

void net_init(void) {
    g_ready = 0;
    g_last_valid = 0;
    g_ping_reply = 0;
    g_dns_txid = 0x4a17;
    g_udp_our_port = 0xC001;
    g_tcp_state = TCP_ST_CLOSED;
    if (!rtl8139_init()) return;
    memcpy(g_mac, rtl8139_mac(), 6);
    g_our_ip  = ip_from(10, 0, 2, 15);
    g_gateway = ip_from(10, 0, 2, 2);
    g_dns     = ip_from(10, 0, 2, 3);
    g_ready = 1;
    kprintf("NET: ip=10.0.2.15 gw=10.0.2.2 dns=10.0.2.3\n");
}

void net_poll(void) {
    if (g_ready) rtl8139_poll();
}

int net_ready(void)   { return g_ready; }
u32 net_our_ip(void)  { return g_our_ip; }
u32 net_gateway(void) { return g_gateway; }
u32 net_dns(void)     { return g_dns; }
const u8 *net_our_mac(void) { return g_mac; }

u32 net_ip_from_string(const char *s, int *ok) {
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
    return ip_from((u8)parts[0], (u8)parts[1], (u8)parts[2], (u8)parts[3]);
}

int net_host_matches(const char *stored, const char *request) {
    if (!stored || !request) return 0;
    usize sl = strlen(stored);
    usize rl = strlen(request);
    if (sl == 0 || rl == 0) return 0;
    if (strcmp(stored, request) == 0) return 1;
    if (sl < rl) {
        usize off = rl - sl;
        if (request[off - 1] == '.' && strcmp(stored, request + off) == 0) return 1;
    }
    if (sl > rl) {
        usize off = sl - rl;
        if (stored[off - 1] == '.' && strcmp(stored + off, request) == 0) return 1;
    }
    return 0;
}

int net_ping(u32 dest, int timeout_ms) {
    if (!g_ready) return -1;

    u8 dmac[6];
    if (!lookup_arp(dest, dmac)) {
        send_arp_request(dest);
        u64 arp_deadline = timer_ticks() + 50;
        while (timer_ticks() < arp_deadline) net_poll();
        if (!lookup_arp(dest, dmac)) return -1;
    }

    g_ping_reply = 0;
    g_ping_dest = dest;
    g_ping_sent = timer_ticks();
    send_icmp_echo(dest);

    u64 deadline = timer_ticks() + (u64)(timeout_ms / 10);
    while (timer_ticks() < deadline) {
        net_poll();
        if (g_ping_reply) return (int)g_ping_rtt;
        for (volatile int i = 0; i < 50000; i++) { }
    }
    return -1;
}

static int dns_encode_name(const char *host, u8 *out, int cap) {
    int o = 0;
    const char *p = host;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int n = (int)(dot - p);
        if (n == 0 || n > 63) return -1;
        if (o + 1 + n >= cap) return -1;
        out[o++] = (u8)n;
        for (int i = 0; i < n; i++) out[o++] = (u8)p[i];
        if (*dot == 0) break;
        p = dot + 1;
    }
    if (o + 1 >= cap) return -1;
    out[o++] = 0;
    return o;
}

int net_dns_lookup(const char *host, u32 *out_ip) {
    if (!g_ready) return -1;

    int ok = 0;
    u32 direct = net_ip_from_string(host, &ok);
    if (ok) { *out_ip = direct; return 0; }

    u8 q[256];
    usize qn = 0;
    wr16(q + 0, g_dns_txid);
    wr16(q + 2, 0x0100);
    wr16(q + 4, 1);
    wr16(q + 6, 0);
    wr16(q + 8, 0);
    wr16(q + 10, 0);
    qn = 12;
    int nl = dns_encode_name(host, q + qn, (int)(sizeof(q) - qn - 4));
    if (nl < 0) return -1;
    qn += (usize)nl;
    wr16(q + qn + 0, 1);
    wr16(q + qn + 2, 1);
    qn += 4;

    g_udp_got = 0;
    g_udp_len = 0;

    u8 dmac[6];
    if (!lookup_arp(g_gateway, dmac)) {
        send_arp_request(g_gateway);
        u64 ad = timer_ticks() + 50;
        while (timer_ticks() < ad) net_poll();
        if (!lookup_arp(g_gateway, dmac)) return -1;
    }

    udp_send(g_udp_our_port, g_dns, 53, q, qn);

    u64 deadline = timer_ticks() + 400;
    while (timer_ticks() < deadline && !g_udp_got) {
        net_poll();
        for (volatile int i = 0; i < 10000; i++) { }
    }
    if (!g_udp_got) return -2;

    const u8 *r = g_udp_buf;
    usize rlen = g_udp_len;
    if (rlen < 12) return -3;
    if (rd16(r + 0) != g_dns_txid) return -4;
    u16 ancount = rd16(r + 6);
    if (ancount < 1) return -5;

    usize off = 12;
    while (off < rlen && r[off] != 0) {
        if ((r[off] & 0xC0) == 0xC0) { off += 2; break; }
        off += 1 + r[off];
    }
    if (off >= rlen) return -6;
    if (r[off] == 0) off++;
    off += 4;
    if (off >= rlen) return -7;

    for (u16 a = 0; a < ancount; a++) {
        if (off >= rlen) return -8;
        if ((r[off] & 0xC0) == 0xC0) off += 2;
        else {
            while (off < rlen && r[off] != 0) off += 1 + r[off];
            off++;
        }
        if (off + 10 > rlen) return -9;
        u16 type = rd16(r + off + 0);
        u16 rdlen = rd16(r + off + 8);
        if (type == 1 && rdlen == 4) {
            *out_ip = rd32(r + off + 10);
            return 0;
        }
        off += 10 + rdlen;
    }
    return -10;
}

int net_tcp_is_open(void) {
    return g_tcp_state == TCP_ST_ESTABLISHED || g_tcp_state == TCP_ST_CLOSE_WAIT;
}

static u16 next_ephemeral_port(void) {
    static u16 p = 0xC000;
    p++;
    if (p == 0 || p < 0xC000) p = 0xC001;
    return p;
}

int net_tcp_connect(u32 dst_ip, u16 dst_port, int timeout_ms) {
    if (!g_ready) return -1;

    u8 dmac[6];
    if (!lookup_arp(dst_ip, dmac)) {
        send_arp_request(dst_ip);
        u64 ad = timer_ticks() + 50;
        while (timer_ticks() < ad) net_poll();
        if (!lookup_arp(dst_ip, dmac)) return -2;
    }

    g_tcp_remote_ip = dst_ip;
    g_tcp_remote_port = dst_port;
    g_tcp_local_port = next_ephemeral_port();
    g_tcp_initial_snd = (u32)timer_ticks() * 0x10001u + 0x12345u;
    g_tcp_snd_nxt = g_tcp_initial_snd;
    g_tcp_rcv_nxt = 0;
    g_tcp_rx_len = 0;
    g_tcp_got_fin = 0;
    g_tcp_state = TCP_ST_SYN_SENT;

    tcp_send_segment(TCP_SYN, NULL, 0);

    u64 deadline = timer_ticks() + (u64)(timeout_ms / 10);
    u64 resend = timer_ticks() + 50;
    while (timer_ticks() < deadline) {
        net_poll();
        if (g_tcp_state == TCP_ST_ESTABLISHED) return 0;
        if (g_tcp_state == TCP_ST_CLOSED)     return -3;
        if (timer_ticks() > resend) {
            tcp_send_segment(TCP_SYN, NULL, 0);
            resend = timer_ticks() + 50;
        }
        for (volatile int i = 0; i < 50000; i++) { }
    }
    g_tcp_state = TCP_ST_CLOSED;
    return -4;
}

int net_tcp_send(const u8 *data, usize len) {
    if (g_tcp_state != TCP_ST_ESTABLISHED) return -1;
    usize sent = 0;
    while (sent < len) {
        usize chunk = len - sent;
        if (chunk > 1400) chunk = 1400;
        tcp_send_segment(TCP_ACK | TCP_PSH, data + sent, chunk);
        g_tcp_snd_nxt += (u32)chunk;
        sent += chunk;
        for (volatile int i = 0; i < 50000; i++) { }
    }
    return (int)sent;
}

int net_tcp_recv(u8 *out, usize max, int timeout_ms) {
    usize copied = 0;
    u64 deadline = timer_ticks() + (u64)(timeout_ms / 10);
    while (copied < max) {
        usize avail = g_tcp_rx_len - copied;
        if (avail > 0) {
            usize take = avail > max - copied ? max - copied : avail;
            memcpy(out + copied, g_tcp_rx + copied, take);
            copied += take;
            if (copied >= max) break;
        }
        if (g_tcp_got_fin && copied >= g_tcp_rx_len) break;
        if (timer_ticks() > deadline) break;
        net_poll();
        for (volatile int i = 0; i < 20000; i++) { }
    }
    return (int)copied;
}

void net_tcp_close(void) {
    if (g_tcp_state == TCP_ST_ESTABLISHED || g_tcp_state == TCP_ST_CLOSE_WAIT) {
        g_tcp_snd_nxt += 1;
        tcp_send_segment(TCP_FIN | TCP_ACK, NULL, 0);
        g_tcp_state = TCP_ST_LAST_ACK;
        u64 d = timer_ticks() + 20;
        while (timer_ticks() < d) net_poll();
    }
    g_tcp_state = TCP_ST_CLOSED;
}