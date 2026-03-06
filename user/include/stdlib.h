/* ============================================================================
 * stdlib.h — Standard library functions
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
