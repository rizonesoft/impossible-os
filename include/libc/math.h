/* ============================================================================
 * math.h -- Freestanding floating-point math library (kernel libc)
 *
 * Software double-precision math for the freestanding kernel: no libm, no
 * <math.h>, no SSE. Companion to include/kernel/kmath.h, which keeps a small
 * set of font-grade helpers (kmath_fabs/floor/ceil/fmod/sqrt/pow/cos/acos)
 * used directly by stb_truetype + cJSON.
 *
 * HEADER-ONLY / static inline by necessity: the kernel builds -mno-sse/
 * -mno-sse2, and on the x86-64 SysV ABI a non-inline function returning
 * float/double must hand it back in an SSE register (xmm0). With SSE disabled
 * there is no legal return path, so these cannot live in a .c file -- they must
 * be inlined into each caller (doubles then stay in x87). This is the same
 * reason kernel/kmath.h is all static inline.
 *
 * Symbol-set rule: this header defines ONLY names NOT in kernel/kmath.h, so a
 * translation unit may include both without an ODR clash. Float variants of
 * functions already in kmath.h (cosf/acosf/powf/sqrtf) are new names and
 * delegate to the kmath.h inlines.
 *
 * Accuracy: a documented FEW-ULP bound over the supported finite range, NOT a
 * 1-ULP guarantee. sin/tan use Cody-Waite range reduction with a split pi/2 and
 * fdlibm-provenance minimax kernels. Domain is deliberately narrower than C99:
 * |x| >= KMATH_TRIG_REDUCE_MAX (and +/-inf) return NaN rather than risk an
 * out-of-range reduction cast -- callers needing huge arguments must pre-reduce.
 * Signed zero is preserved at every entry point (sin/tan/atan/atan2/round/trunc).
 *
 * XREF: 02-kernel-core/TODO-03-kernel-libraries.md
 * ============================================================================ */
#ifndef KERNEL_LIBC_MATH_H
#define KERNEL_LIBC_MATH_H

#include "kernel/types.h"
#include "kernel/kmath.h"

/* always_inline is mandatory, not an optimization: the kernel builds -mno-sse,
 * so any OUT-OF-LINE copy of a function with float/double parameters or return
 * would have to use xmm registers (illegal here). Forcing full inlining at every
 * call site guarantees the compiler never emits such a copy, even when a function
 * grows past its inline-cost heuristic. */
#define KM_AI static inline __attribute__((always_inline))

/* Largest |x| for which the two-part Cody-Waite reduction stays few-ULP:
 * 2^20 * pi/2. The split constant KM_PIO2_HI keeps only the high ~30 mantissa
 * bits, so n*KM_PIO2_HI is EXACT for n up to 2^20; past that the product rounds
 * and the remainder degrades, so sin/tan return NaN rather than a plausible-but-
 * wrong finite value. Callers needing larger arguments must pre-reduce. */
#define KMATH_TRIG_REDUCE_MAX 1647099.0

/* ---- constants ---------------------------------------------------------- */

#define KM_PI       3.14159265358979311600    /* pi (nearest double) */
#define KM_PIO2     1.57079632679489655800    /* pi/2 */
#define KM_PIO4     0.78539816339744827900    /* pi/4 */
#define KM_TWO_PI   6.28318530717958623200    /* 2*pi */
#define KM_2_OVER_PI 0.63661977236758134308   /* 2/pi */
/* Two-part pi/2 (Cody-Waite), fdlibm pio2_1 / pio2_1t: KM_PIO2_HI carries only
 * the high ~30 mantissa bits so n*KM_PIO2_HI is exact for |n| <= 2^20; KM_PIO2_LO
 * is pi/2 - KM_PIO2_HI. Their sum is pi/2 to full double precision (used by both
 * the trig reducer and atan's 1/x reconstruction). */
#define KM_PIO2_HI  1.57079632673412561417e+00
#define KM_PIO2_LO  6.07710050650619224932e-11
/* Two-part ln2 (fdlibm). */
#define KM_LN2_HI   6.93147180369123816490e-01
#define KM_LN2_LO   1.90821492927058770002e-10
#define KM_LOG2E    1.44269504088896340736     /* 1/ln2 */
#define KM_LOG10E   0.43429448190325182765     /* 1/ln10 */

/* ---- IEEE-754 bit helpers ----------------------------------------------- */

KM_AI double km_inf_(int neg)
{
    union { double d; uint64_t u; } v;
    v.u = neg ? 0xFFF0000000000000ULL : 0x7FF0000000000000ULL;
    return v.d;
}

KM_AI double km_nan_(void)
{
    union { double d; uint64_t u; } v;
    v.u = 0x7FF8000000000000ULL;
    return v.d;
}

KM_AI int km_isinf_(double x)
{
    union { double d; uint64_t u; } v;
    v.d = x;
    return (v.u & 0x7fffffffffffffffULL) == 0x7ff0000000000000ULL;
}

KM_AI int km_signbit_(double x)
{
    union { double d; uint64_t u; } v;
    v.d = x;
    return (int)(v.u >> 63);            /* distinguishes -0.0 from +0.0 */
}

/* Accurate sqrt regardless of magnitude: a magic-constant bit seed (~3% error)
 * then 5 Newton steps (quadratic convergence -> full double precision). Unlike
 * the font-grade kmath.h kmath_sqrt (guess x*0.5, capped iterations), this stays
 * accurate for tiny radicands -- needed by asin near +/-1. */
KM_AI double km_sqrt_(double x)
{
    if (x != x) return x;              /* NaN */
    if (x == 0.0) return x;            /* +/-0 preserved */
    if (x < 0.0) return km_nan_();     /* domain error */
    if (km_isinf_(x)) return x;        /* sqrt(+inf) = +inf */
    union { double d; uint64_t u; } v;
    v.d = x;
    v.u = 0x1ff7a3bea91d9b1bULL + (v.u >> 1);   /* seed within ~3.5% of sqrt(x) */
    double y = v.d;
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    return y;
}

/* x * 2^n via the biased exponent, two-step so the 2^n constant never over/
 * underflows (musl scalbn). */
KM_AI double km_scalbn_(double x, int n)
{
    union { double d; uint64_t u; } v;
    if (n > 1023) {
        x *= 0x1p1023;
        n -= 1023;
        if (n > 1023) { x *= 0x1p1023; n -= 1023; if (n > 1023) n = 1023; }
    } else if (n < -1022) {
        x *= 0x1p-1022 * 0x1p53;          /* 2^-969 */
        n += 1022 - 53;
        if (n < -1022) {
            x *= 0x1p-1022 * 0x1p53;
            n += 1022 - 53;
            if (n < -1022) n = -1022;
        }
    }
    v.u = (uint64_t)(0x3ff + n) << 52;
    return x * v.d;
}

/* ---- trig kernels (no reduction; |x| <= pi/4) --------------------------- */

KM_AI double km_ksin_(double x)
{
    /* fdlibm __kernel_sin coefficients. */
    const double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                 S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                 S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
    double z = x * x;
    double p = S1 + z * (S2 + z * (S3 + z * (S4 + z * (S5 + z * S6))));
    return x + x * z * p;
}

KM_AI double km_kcos_(double x)
{
    /* fdlibm __kernel_cos coefficients. */
    const double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                 C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                 C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
    double z = x * x;
    double p = C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6))));
    return 1.0 - 0.5 * z + z * z * p;
}

/* Reduce x to the primary octant; return sin and cos together so tan stays
 * consistent (one reduction feeds numerator and denominator). */
KM_AI void km_sincos_(double x, double *sp, double *cp)
{
    double ax = kmath_fabs(x);
    if (ax < KM_PIO4) { *sp = km_ksin_(x); *cp = km_kcos_(x); return; }
    /* Callers guarantee ax < KMATH_TRIG_REDUCE_MAX, so z*2/pi fits a long long
     * and the round-to-nearest cast below is well-defined. */
    double z = x * KM_2_OVER_PI;
    long long n = (long long)(z >= 0.0 ? z + 0.5 : z - 0.5);
    double nd = (double)n;
    double y = (x - nd * KM_PIO2_HI) - nd * KM_PIO2_LO;
    double sy = km_ksin_(y), cy = km_kcos_(y);
    switch ((int)(n & 3)) {
    case 0:  *sp = sy;  *cp = cy;  break;
    case 1:  *sp = cy;  *cp = -sy; break;
    case 2:  *sp = -sy; *cp = -cy; break;
    default: *sp = -cy; *cp = sy;  break;
    }
}

/* ---- trigonometric ------------------------------------------------------ */

KM_AI double kmath_sin(double x)
{
    if (x != x) return x;
    if (x == 0.0) return x;             /* sin(+/-0) = +/-0 (signed zero) */
    /* +/-inf and the unsupported huge-argument range both return NaN -- avoids
     * an out-of-range float->integer cast in the reduction. */
    if (kmath_fabs(x) >= KMATH_TRIG_REDUCE_MAX) return km_nan_();
    double s, c;
    km_sincos_(x, &s, &c);
    return s;
}

KM_AI double kmath_tan(double x)
{
    if (x != x) return x;
    if (x == 0.0) return x;             /* tan(+/-0) = +/-0 (signed zero) */
    if (kmath_fabs(x) >= KMATH_TRIG_REDUCE_MAX) return km_nan_();
    double s, c;
    km_sincos_(x, &s, &c);
    return s / c;                       /* +/-inf near a pole (cos -> 0) */
}

KM_AI double kmath_atan(double x)
{
    /* fdlibm atan kernel coefficients, |x| <= 1. */
    static const double aT[11] = {
        3.33333333333329318027e-01, -1.99999999998764832476e-01,
        1.42857142725034663711e-01, -1.11111104054623557880e-01,
        9.09088713343650656196e-02, -7.69187620504482999495e-02,
        6.66107313738753120669e-02, -5.83357013379057348645e-02,
        4.97687799461593236017e-02, -3.65315727442169155270e-02,
        1.62858201153657823623e-02,
    };
    if (x != x) return x;
    if (x == 0.0) return x;            /* atan(+/-0) = +/-0 (preserve signed zero) */
    int neg = x < 0.0, recip = 0;
    double ax = kmath_fabs(x);
    if (ax > 1.0) { ax = 1.0 / ax; recip = 1; }    /* atan(x) = pi/2 - atan(1/x) */

    /* pi/6 breakpoint: shrink the polynomial argument to |t| <= 2-sqrt(3) so the
     * minimax kernel (accurate only for small args) never sees x near 1.
     * atan(ax) = pi/6 + atan((ax*sqrt3 - 1)/(ax + sqrt3)). */
    double add = 0.0;
    if (ax > 0.26794919243112270647) {             /* 2 - sqrt(3) */
        const double SQRT3 = 1.73205080756887719318;
        ax = (ax * SQRT3 - 1.0) / (ax + SQRT3);
        add = 0.52359877559829887308;              /* pi/6 */
    }

    double z = ax * ax, w = z * z;
    double s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    double s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    double r = ax - ax * (s1 + s2);                /* atan(t), |t| <= 2-sqrt(3) */
    r += add;

    if (recip) r = (KM_PIO2_HI - r) + KM_PIO2_LO;
    return neg ? -r : r;
}

KM_AI double kmath_atan2(double y, double x)
{
    if (x != x || y != y) return x + y;
    /* Infinities: y/x would be inf/inf = NaN, so resolve by sign/quadrant
     * (IEEE / C99 atan2 semantics) before the divide. */
    if (km_isinf_(x) || km_isinf_(y)) {
        double sy = km_signbit_(y) ? -1.0 : 1.0;     /* sign-of-y, -0.0 aware */
        if (km_isinf_(x) && km_isinf_(y))
            return (x > 0.0) ? sy * KM_PIO4 : sy * (3.0 * KM_PI / 4.0);
        if (km_isinf_(y)) return sy * KM_PIO2;        /* y inf, x finite */
        /* x inf, y finite: +/-0 toward +inf, +/-pi toward -inf */
        return (x > 0.0) ? (km_signbit_(y) ? -0.0 : 0.0) : sy * KM_PI;
    }
    if (x == 0.0) {
        if (y > 0.0) return KM_PIO2;
        if (y < 0.0) return -KM_PIO2;
        /* y is +/-0: sign follows y; x = -0 flips to +/-pi (C99 atan2). */
        if (km_signbit_(x)) return km_signbit_(y) ? -KM_PI : KM_PI;
        return km_signbit_(y) ? -0.0 : 0.0;
    }
    if (y == 0.0) {
        /* finite nonzero x: atan(y/x) would drop the sign of -0.0. */
        if (x > 0.0) return km_signbit_(y) ? -0.0 : 0.0;
        return km_signbit_(y) ? -KM_PI : KM_PI;
    }
    double a = kmath_atan(y / x);
    if (x > 0.0) return a;             /* quadrants I / IV */
    if (y >= 0.0) return a + KM_PI;    /* quadrant II */
    return a - KM_PI;                  /* quadrant III */
}

KM_AI double kmath_asin(double x)
{
    if (x != x) return x;
    if (x == 1.0) return KM_PIO2;
    if (x == -1.0) return -KM_PIO2;
    if (x > 1.0 || x < -1.0) return km_nan_();   /* domain error (incl +/-inf) */
    /* atan2 form is stable across all of (-1,1); km_sqrt_ keeps the endpoint
     * radicand accurate (the font-grade kmath_sqrt under-converges for tiny x). */
    return kmath_atan2(x, km_sqrt_((1.0 - x) * (1.0 + x)));
}

/* ---- exponential / logarithmic ------------------------------------------ */

KM_AI double kmath_exp(double x)
{
    if (x != x) return x;
    if (x > 709.782712893384) return km_inf_(0);    /* overflow */
    if (x < -745.133219101941) return 0.0;          /* underflow */

    double kd = x * KM_LOG2E;
    long long k = (long long)(kd >= 0.0 ? kd + 0.5 : kd - 0.5);
    double kdbl = (double)k;
    double r = (x - kdbl * KM_LN2_HI) - kdbl * KM_LN2_LO;   /* |r| <= ln2/2 */

    /* exp(r) via Taylor; |r| <= ~0.347 so 14 terms reach below 1 ULP. */
    double term = 1.0, sum = 1.0;
    for (int n = 1; n <= 14; n++) { term *= r / (double)n; sum += term; }
    return km_scalbn_(sum, (int)k);
}

KM_AI double kmath_log(double x)
{
    if (x != x) return x;
    if (x < 0.0) return km_nan_();
    if (x == 0.0) return km_inf_(1);               /* log(0) = -inf */

    union { double d; uint64_t u; } v;
    v.d = x;
    if (((v.u >> 52) & 0x7ff) == 0x7ff) return x;  /* +inf -> +inf (NaN already handled) */
    int e = (int)((v.u >> 52) & 0x7ff) - 1023;
    if (e == -1023) {                              /* subnormal: scale up */
        x *= 0x1p54;
        v.d = x;
        e = (int)((v.u >> 52) & 0x7ff) - 1023 - 54;
    }
    v.u = (v.u & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;   /* mantissa in [1,2) */
    double m = v.d;
    if (m > 1.41421356237309514547) { m *= 0.5; e++; }   /* center around 1 */

    /* log(m) = 2*atanh(s), s = (m-1)/(m+1); |s| <= ~0.172 so the series is short. */
    double s = (m - 1.0) / (m + 1.0);
    double s2 = s * s;
    double sum = s * (1.0 + s2 * (1.0 / 3 + s2 * (1.0 / 5 + s2 * (1.0 / 7 +
                  s2 * (1.0 / 9 + s2 * (1.0 / 11 + s2 * (1.0 / 13)))))));
    double logm = 2.0 * sum;
    return (double)e * KM_LN2_HI + ((double)e * KM_LN2_LO + logm);
}

KM_AI double kmath_log2(double x)  { return kmath_log(x) * KM_LOG2E; }
KM_AI double kmath_log10(double x) { return kmath_log(x) * KM_LOG10E; }

/* ---- rounding / roots --------------------------------------------------- */

KM_AI double kmath_round(double x)
{
    if (x != x) return x;
    if (x == 0.0) return x;            /* preserve +/-0 */
    double ax = kmath_fabs(x);
    if (ax >= 0x1p52) return x;                    /* no fractional bits */
    double r = kmath_floor(ax + 0.5);              /* half away from zero */
    return x < 0.0 ? -r : r;
}

KM_AI double kmath_trunc(double x)
{
    if (x != x) return x;
    if (x == 0.0) return x;            /* preserve +/-0 */
    double ax = kmath_fabs(x);
    if (ax >= 0x1p52) return x;
    double t = (double)(long long)ax;              /* toward zero */
    return x < 0.0 ? -t : t;
}

KM_AI double kmath_cbrt(double x)
{
    if (x != x || x == 0.0) return x;
    if (km_isinf_(x)) return x;                    /* cbrt(+/-inf) = +/-inf */
    int neg = x < 0.0;
    double ax = kmath_fabs(x);
    /* exp(log(ax)/3) seeds a near-correct guess; two Newton steps polish it. */
    double y = kmath_exp(kmath_log(ax) * (1.0 / 3.0));
    y = (2.0 * y + ax / (y * y)) * (1.0 / 3.0);
    y = (2.0 * y + ax / (y * y)) * (1.0 / 3.0);
    return neg ? -y : y;
}

/* Hardened pow (the kmath.h kmath_pow inline is font-grade and mishandles zero
 * bases, negative bases, and infinities). Used by kmath_powf; not exported as a
 * double `kmath_pow` because that name is owned by kernel/kmath.h. */
KM_AI double km_pow_(double x, double y)
{
    if (y == 0.0) return 1.0;                      /* pow(x, +/-0) = 1, even for NaN x */
    if (x == 1.0) return 1.0;                      /* pow(1, y) = 1, even for NaN y */
    if (x != x || y != y) return km_nan_();
    /* Infinite exponent FIRST -- classifying oddness via trunc/fmod would cast
     * inf to an integer (UB) if y were not yet known finite. */
    if (km_isinf_(y)) {
        double ax = kmath_fabs(x);
        if (ax == 1.0) return 1.0;                 /* pow(+/-1, +/-inf) = 1 */
        if (y > 0.0) return (ax > 1.0) ? km_inf_(0) : 0.0;
        return (ax > 1.0) ? 0.0 : km_inf_(0);
    }

    /* y is finite here; classify integer-ness / parity without fmod/floor (those
     * cast the quotient to long, UB for large integral exponents). Any double
     * with |v| >= 2^53 has no unit bit and is even, so only smaller magnitudes
     * -- which fit a long long -- need the low-bit test. */
    double yt = kmath_trunc(y);
    int y_is_int = (yt == y);
    int y_odd_int = 0;
    if (y_is_int) {
        double ay = kmath_fabs(yt);
        if (ay < 0x1p53) y_odd_int = ((long long)ay & 1LL) != 0;
    }

    if (x == 0.0) {
        int neg = km_signbit_(x) && y_odd_int;     /* -0 only survives an odd power */
        if (y < 0.0) return neg ? km_inf_(1) : km_inf_(0);
        return neg ? -0.0 : 0.0;
    }
    if (km_isinf_(x)) {
        if (x > 0.0) return (y > 0.0) ? km_inf_(0) : 0.0;
        if (y > 0.0) return y_odd_int ? km_inf_(1) : km_inf_(0);
        return y_odd_int ? -0.0 : 0.0;
    }
    if (x < 0.0) {
        if (!y_is_int) return km_nan_();           /* negative base, non-integer exp */
        double r = kmath_exp(y * kmath_log(-x));
        return y_odd_int ? -r : r;
    }
    return kmath_exp(y * kmath_log(x));            /* positive finite base */
}

/* ---- float variants (compute in double, narrow on return) --------------- */

KM_AI float kmath_sinf(float x)            { return (float)kmath_sin((double)x); }
KM_AI float kmath_cosf(float x)
{
    /* Must NOT delegate to kmath.h kmath_cos: its while-loop reduction never
     * terminates for +/-inf or very large finite floats. Use the guarded path. */
    double d = (double)x;
    if (d != d) return (float)d;
    if (kmath_fabs(d) >= KMATH_TRIG_REDUCE_MAX) return (float)km_nan_();
    double s, c;
    km_sincos_(d, &s, &c);
    return (float)c;
}
KM_AI float kmath_tanf(float x)            { return (float)kmath_tan((double)x); }
KM_AI float kmath_asinf(float x)           { return (float)kmath_asin((double)x); }
KM_AI float kmath_acosf(float x)
{
    /* Accurate path (pi/2 - asin) + domain check; the kmath.h kmath_acos inline
     * is font-grade and clamps, so it is unfit for an accurate float acos. */
    double d = (double)x;
    if (d != d) return (float)d;
    if (d > 1.0 || d < -1.0) return (float)km_nan_();
    return (float)(KM_PIO2 - kmath_asin(d));
}
KM_AI float kmath_atanf(float x)           { return (float)kmath_atan((double)x); }
KM_AI float kmath_atan2f(float y, float x) { return (float)kmath_atan2((double)y, (double)x); }
KM_AI float kmath_expf(float x)            { return (float)kmath_exp((double)x); }
KM_AI float kmath_logf(float x)            { return (float)kmath_log((double)x); }
KM_AI float kmath_log2f(float x)           { return (float)kmath_log2((double)x); }
KM_AI float kmath_log10f(float x)          { return (float)kmath_log10((double)x); }
KM_AI float kmath_powf(float b, float e)   { return (float)km_pow_((double)b, (double)e); }
KM_AI float kmath_sqrtf(float x)           { return (float)km_sqrt_((double)x); }
KM_AI float kmath_cbrtf(float x)           { return (float)kmath_cbrt((double)x); }
KM_AI float kmath_roundf(float x)          { return (float)kmath_round((double)x); }
KM_AI float kmath_truncf(float x)          { return (float)kmath_trunc((double)x); }

#endif /* KERNEL_LIBC_MATH_H */
