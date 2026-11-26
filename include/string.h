// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/* caOS string.h - bare metal, no libc (-nostdlib).
 *
 * On the include path of every caOS build (hw/common), so #include <string.h>
 * from caOS, LVGL, FatFs or TinyUSB lands here. Declarations only: the
 * functions are in caos/src/libc_shim.c, thin wrappers on the ca_* functions
 * of memaux.c (23/11/2026). Until then this header carried its own static
 * inline byte loops, a second copy of memaux that GCC could turn back into
 * library calls anyway.
 *
 * A function not declared here does not exist in caOS: add it to
 * libc_shim.c (on a ca_* function, written in memaux.c if missing). */

#ifndef _STRING_H
#define _STRING_H

void         *memcpy(void *dst, const void *src, unsigned int n);
void         *memmove(void *dst, const void *src, unsigned int n);
void         *memset(void *s, int c, unsigned int n);
int           memcmp(const void *a, const void *b, unsigned int n);
unsigned int  strlen(const char *s);
int           strcmp(const char *a, const char *b);
int           strncmp(const char *a, const char *b, unsigned int n);
char         *strchr(const char *s, int c);

#endif /* _STRING_H */
