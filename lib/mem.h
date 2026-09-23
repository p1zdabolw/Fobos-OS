#ifndef FOS_MEM_H
#define FOS_MEM_H

#include "../kernel/types.h"

void *memset(void *p, int c, usize n);
void *memcpy(void *d, const void *s, usize n);
void *memmove(void *d, const void *s, usize n);
int   memcmp(const void *a, const void *b, usize n);

#endif