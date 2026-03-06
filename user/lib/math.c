/* ============================================================================
 * math.c — Integer math helpers
 * ============================================================================ */

#include "math.h"

int iabs(int x) { return x < 0 ? -x : x; }

long labs_math(long x) { return x < 0 ? -x : x; }

int imin(int a, int b) { return a < b ? a : b; }
int imax(int a, int b) { return a > b ? a : b; }
int iclamp(int x, int lo, int hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

long lmin(long a, long b) { return a < b ? a : b; }
long lmax(long a, long b) { return a > b ? a : b; }

long ipow(long base, int exp)
{
    long result = 1;
    int i;

    if (exp < 0)
        return 0;  /* integer division would give 0 */

    for (i = 0; i < exp; i++)
        result *= base;

    return result;
}

unsigned long isqrt(unsigned long n)
{
    unsigned long x, y;

    if (n == 0)
        return 0;

    x = n;
    y = (x + 1) / 2;

    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }

    return x;
}
