/* ============================================================================
 * string.h -- Freestanding string and memory operations
 *
 * Canonical implementation for the Impossible OS kernel.  Replaces the
 * duplicate weak symbols in image.c and stb_truetype_impl.c, and the
 * hand-rolled str_eq() functions scattered across the codebase.
 *
 * Compile: -ffreestanding -nostdlib -nostdinc -O2
 * Include: #include "libc/string.h"
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Memory operations -------------------------------------------------- */

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

/* ---- String operations -------------------------------------------------- */

size_t strlen(const char *s);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);

/* ---- BSD safe string operations ----------------------------------------- */

size_t strlcpy(char *dst, const char *src, size_t n);
size_t strlcat(char *dst, const char *src, size_t n);

/* ---- Numeric conversions ------------------------------------------------ */

long          strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int           atoi(const char *s);
