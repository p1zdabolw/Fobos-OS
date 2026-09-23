#ifndef FOS_STRING_H
#define FOS_STRING_H

#include "../kernel/types.h"

usize strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, usize n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, usize n);
char *strcat(char *d, const char *s);

#endif