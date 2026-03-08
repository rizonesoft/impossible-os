/* ============================================================================
 * kmath.h — Kernel math functions (floating-point, freestanding)
 *
 * Provides the subset of <math.h> needed by stb_truetype in a freestanding
 * kernel environment.  Uses software implementations — no libm dependency.
 *
 * These are NOT high-precision — they're sufficient for TrueType rasterization.
 * ============================================================================ */

#pragma once

/* --- fabs --- */
static inline double kmath_fabs(double x) { return x < 0.0 ? -x : x; }

/* --- floor / ceil --- */
static inline double kmath_floor(double x)
{
    long i = (long)x;
    return (double)(x < (double)i ? i - 1 : i);
}

static inline double kmath_ceil(double x)
{
    long i = (long)x;
    return (double)(x > (double)i ? i + 1 : i);
}

/* --- fmod --- */
static inline double kmath_fmod(double x, double y)
{
    if (y == 0.0) return 0.0;
    return x - kmath_floor(x / y) * y;
}

/* --- sqrt (Newton-Raphson, ~15 iterations for double precision) --- */
static inline double kmath_sqrt(double x)
{
    double guess, prev;
    int i;
    if (x <= 0.0) return 0.0;
    guess = x * 0.5;
    for (i = 0; i < 20; i++) {
        prev = guess;
        guess = 0.5 * (guess + x / guess);
        if (kmath_fabs(guess - prev) < 1e-12) break;
    }
    return guess;
}

/* --- pow (integer exponent fast path, general via exp/log approximation) --- */
static inline double kmath_pow(double base, double exp)
{
    /* Handle common cases */
    if (exp == 0.0) return 1.0;
    if (exp == 1.0) return base;
    if (exp == 2.0) return base * base;
    if (exp == 0.5) return kmath_sqrt(base);
    if (base == 0.0) return 0.0;

    /* For integer exponents */
    {
        int iexp = (int)exp;
        if ((double)iexp == exp && iexp >= 0) {
            double result = 1.0;
            int i;
            for (i = 0; i < iexp; i++)
                result *= base;
            return result;
        }
    }

    /* General case: exp(exp * ln(base)) via Taylor series approximation.
     * This is approximate but sufficient for font rasterization. */
    {
        /* ln(x) approximation for x > 0 */
        double ln_base = 0.0;
        double y = (base - 1.0) / (base + 1.0);
        double y2 = y * y;
        double term = y;
        int k;
        for (k = 0; k < 20; k++) {
            ln_base += term / (double)(2 * k + 1);
            term *= y2;
        }
        ln_base *= 2.0;

        /* exp(z) Taylor series */
        {
            double z = exp * ln_base;
            double result = 1.0;
            double factorial_term = 1.0;
            int n;
            for (n = 1; n < 20; n++) {
                factorial_term *= z / (double)n;
                result += factorial_term;
                if (kmath_fabs(factorial_term) < 1e-12) break;
            }
            return result;
        }
    }
}

/* --- cos (Taylor series) --- */
static inline double kmath_cos(double x)
{
    double x2, term, sum;
    int i;
    /* Reduce to [-pi, pi] */
    while (x > 3.14159265358979323846) x -= 6.28318530717958647692;
    while (x < -3.14159265358979323846) x += 6.28318530717958647692;
    x2 = x * x;
    sum = 1.0;
    term = 1.0;
    for (i = 1; i <= 10; i++) {
        term *= -x2 / (double)(2 * i * (2 * i - 1));
        sum += term;
    }
    return sum;
}

/* --- acos (polynomial approximation) --- */
static inline double kmath_acos(double x)
{
    /* Clamp to [-1, 1] */
    if (x > 1.0) x = 1.0;
    if (x < -1.0) x = -1.0;

    /* acos(x) = pi/2 - asin(x), asin via polynomial */
    {
        double pi_2 = 1.5707963267948966;
        double x2 = x * x;
        double asin_approx = x + x * x2 * (1.0/6.0 + x2 * (3.0/40.0 +
            x2 * (15.0/336.0 + x2 * (105.0/3456.0))));
        return pi_2 - asin_approx;
    }
}
