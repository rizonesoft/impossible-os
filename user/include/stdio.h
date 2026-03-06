/* ============================================================================
 * stdio.h — Standard I/O functions
 * ============================================================================ */

#pragma once

#include "types.h"

/* We need __builtin_va_list for variadic printf */
typedef __builtin_va_list va_list;
#define va_start(v, l)  __builtin_va_start(v, l)
#define va_end(v)       __builtin_va_end(v)
#define va_arg(v, t)    __builtin_va_arg(v, t)

/* Standard I/O constants */
#define EOF (-1)

/* Character output */
int putchar(int c);
int puts(const char *s);

/* Character input */
int getchar(void);

/* Formatted output */
int printf(const char *fmt, ...);

/* String formatted output */
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
