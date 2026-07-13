/* ============================================================================
 * stdlib.h -- Standard library functions
 * ============================================================================ */

#pragma once

#include "types.h"

/* String-to-integer conversion */
int  atoi(const char *s);
long atol(const char *s);

/* Integer-to-string conversion */
char *itoa(int value, char *buf, int base);
char *ltoa(long value, char *buf, int base);
char *utoa(unsigned int value, char *buf, int base);

/* Absolute value */
int  abs(int n);
long labs(long n);

/* Memory allocation (static arena) */
void *malloc(size_t size);
void  free(void *ptr);

/* Process control */
void exit(int status);

/* Command-line tokenizer (Windows quoting rules, exact inverse of the kernel's
 * CommandLine encoder / CommandLineToArgvW). Splits `line` IN PLACE into argv
 * tokens: whitespace (space/tab) separates tokens; "..." groups; a "" inside a
 * quoted run is a literal quote; 2n backslashes before a quote emit n
 * backslashes + toggle-quote, 2n+1 emit n backslashes + a literal quote. Writes
 * at most max_argc-1 token pointers into argv[] (NUL-terminating each token
 * within `line`), sets argv[argc]=NULL, and returns argc. Tokens beyond the cap
 * are dropped (the shell prints a "too many arguments" warning). */
int cmd_tokenize(char *line, char **argv, int max_argc);
