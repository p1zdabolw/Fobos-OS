#ifndef FOS_TYPES_H
#define FOS_TYPES_H

typedef unsigned char      u8;
typedef signed char        i8;
typedef unsigned short     u16;
typedef signed short       i16;
typedef unsigned int       u32;
typedef signed int         i32;
typedef unsigned long long u64;
typedef signed long long   i64;
typedef unsigned long      usize;
typedef signed long        isize;
typedef u64                phys_t;
typedef u64                virt_t;

typedef int                bool_t;

#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)

#define KERNEL_VMA 0xFFFFFFFF80000000ULL

#endif