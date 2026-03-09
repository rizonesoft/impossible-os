/* Freestanding <string.h> shim — actual implementations are weak symbols
 * in stb_truetype_impl.c and image.c */
#ifndef _FREESTANDING_STRING_H
#define _FREESTANDING_STRING_H
typedef unsigned long __fs_size_t;
void *memcpy(void *dst, const void *src, __fs_size_t n);
void *memset(void *s, int c, __fs_size_t n);
void *memmove(void *dst, const void *src, __fs_size_t n);
int   memcmp(const void *a, const void *b, __fs_size_t n);
__fs_size_t strlen(const char *s);
#endif
