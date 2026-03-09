/* Freestanding <stddef.h> shim */
#ifndef _FREESTANDING_STDDEF_H
#define _FREESTANDING_STDDEF_H
typedef unsigned long  size_t;
typedef long           ptrdiff_t;
#ifndef NULL
#define NULL ((void *)0)
#endif
#endif
