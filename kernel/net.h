#ifndef FOS_NET_H
#define FOS_NET_H

#include "types.h"

void net_init(void);
void net_poll(void);
int  net_ready(void);
u32  net_our_ip(void);
u32  net_gateway(void);
u32  net_dns(void);
const u8 *net_our_mac(void);
void net_rx_frame(const u8 *frame, usize len);

int  net_ping(u32 dest, int timeout_ms);
int  net_dns_lookup(const char *host, u32 *out_ip);

int  net_tcp_connect(u32 dst_ip, u16 dst_port, int timeout_ms);
int  net_tcp_send(const u8 *data, usize len);
int  net_tcp_recv(u8 *out, usize max, int timeout_ms);
void net_tcp_close(void);
int  net_tcp_is_open(void);

u32  net_ip_from_string(const char *s, int *ok);
int  net_host_matches(const char *stored_host, const char *request_host);

#endif