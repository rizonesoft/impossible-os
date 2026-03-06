/* ============================================================================
 * string.h — String and memory functions
 * ============================================================================ */

#pragma once

#include "types.h"

/* Memory operations */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int   memcmp(const void *s1, const void *s2, size_t n);

/* String length */
size_t strlen(const char *s);

/* String comparison */
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);

/* String copy */
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);

/* String concatenation */
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);

/* String search */
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
