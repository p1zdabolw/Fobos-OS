#ifndef FOS_HEAP_H
#define FOS_HEAP_H

#include "types.h"

void  heap_init(void);
void *kmalloc(usize n);
void *kzalloc(usize n);
void  kfree(void *p);
void *krealloc(void *p, usize n);
u64   heap_used(void);

#endif