#ifndef FOS_PRINTF_H
#define FOS_PRINTF_H

#include "../kernel/types.h"

void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, __builtin_va_list ap);
void snprintf(char *buf, usize cap, const char *fmt, ...);

#endif