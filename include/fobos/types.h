/*
 * Fobos OS — базовые целочисленные типы.
 * Общий заголовок для UEFI-загрузчика, ядра и host-тестов.
 * Специально используем фиксированные типы вместо stdint-псевдонимов ABI,
 * чтобы раскладка структур была идентична при любой цели сборки.
 */
#ifndef FOBOS_TYPES_H
#define FOBOS_TYPES_H

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;

typedef signed char         s8;
typedef signed short        s16;
typedef signed int          s32;
typedef signed long long    s64;

#endif /* FOBOS_TYPES_H */
