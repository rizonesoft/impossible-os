/* ============================================================================
 * math.h — Integer math helpers
 * ============================================================================ */

#pragma once

/* Absolute value */
int  iabs(int x);
long labs_math(long x);

/* Min / Max / Clamp */
int  imin(int a, int b);
int  imax(int a, int b);
int  iclamp(int x, int lo, int hi);
long lmin(long a, long b);
long lmax(long a, long b);

/* Integer power: base^exp */
long ipow(long base, int exp);

/* Integer square root (floor) */
unsigned long isqrt(unsigned long n);
