#ifndef FOS_TIMER_H
#define FOS_TIMER_H

#include "types.h"

void timer_init(u32 hz);
u64  timer_ticks(void);
u64  timer_uptime_seconds(void);

#endif