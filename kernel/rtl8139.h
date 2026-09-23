#ifndef FOS_RTL8139_H
#define FOS_RTL8139_H

#include "types.h"

int  rtl8139_init(void);
int  rtl8139_ready(void);
void rtl8139_send(const u8 *frame, usize len);
void rtl8139_poll(void);
const u8 *rtl8139_mac(void);

#endif