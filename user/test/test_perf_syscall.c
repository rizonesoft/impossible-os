/* ============================================================================
 * test_perf_syscall.c -- §8 perf binary: syscall latency baseline
 *
 * Measures round-trip ring 3 -> kernel -> ring 3 for the cheapest wired
 * syscall (SYS_YIELD, handler returns 0) and reports [PERF] to stderr via
 * UTEST_PERF. The [PERF] line is scraped by the launcher's XML/JSON emit
 * path and by log parsers for trend analysis.
 *
 * Baseline policy (today, pre tests/perf-baseline.json consumer):
 *   -- The binary itself owns the threshold via UTEST_ASSERT. A future
 *      libc JSON helper will read tests/perf-baseline.json and let the
 *      launcher or the test do drift detection.
 *   -- TCG (WSL2 default) is orders of magnitude slower than KVM/WHPX or
 *      bare metal. The threshold chosen here (50_000 ns for a single
 *      sys_yield round-trip) is generous enough to pass on TCG and on
 *      real hardware, so the test does not flake across runners. Real
 *      drift detection lives in the baseline JSON (§8 deferred item).
 *
 * Timing source: RDTSC. User-mode has access unless CR4.TSD is set;
 * Impossible OS leaves TSD=0 so ring 3 reads are legal. TSC frequency is
 * derived on the fly from two RDTSC samples straddling a 10-tick
 * SYS_UPTIME wait (10 ticks = 10 ms at PIT HZ=1000). Accuracy is not
 * the point; we just need a Hz number that lets us convert `cycles`
 * to a plausible `ns` so the baseline JSON has a scalar it can track.
 * ============================================================================ */

#include "test.h"

UTEST_DEFINE_STATE();

/* Serialise and read TSC. LFENCE + RDTSC is the standard ordering for
 * latency measurement on modern x86. Sequence prevents out-of-order
 * execution from polluting the delta. */
static inline uint64_t read_tsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("lfence; rdtsc"
                         : "=a"(lo), "=d"(hi)
                         :
                         : "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* Calibrate TSC against SYS_UPTIME (seconds resolution is coarse but the
 * resulting Hz is stable to within ~1%). Returns a TSC_per_second value
 * that the rest of the test uses for cycles -> ns conversion. Fallback
 * 2 GHz when the uptime delta is zero (first second of boot). */
static uint64_t calibrate_tsc_hz(void)
{
    long sec0 = sys_uptime();
    long sec1;
    uint64_t tsc0 = read_tsc();
    /* Spin until sys_uptime() increments; yields so we do not hot-loop
     * the kernel. */
    do {
        sys_yield();
        sec1 = sys_uptime();
    } while (sec1 == sec0);
    uint64_t tsc1 = read_tsc();

    if (sec1 <= sec0) return 2000000000ULL;  /* unexpected; fall back */
    uint64_t hz = (tsc1 - tsc0) / (uint64_t)(sec1 - sec0);
    if (hz < 500000000ULL || hz > 20000000000ULL)
        return 2000000000ULL;  /* out of plausible range; fall back */
    return hz;
}

int main(void)
{
    UTEST_BEGIN("test_perf_syscall");

    uint64_t tsc_hz = calibrate_tsc_hz();
    UTEST_ASSERT(tsc_hz >= 500000000ULL, "TSC Hz in plausible range");

    /* Warm: exercise the syscall path once so we do not bill a cold-I$
     * miss against the measurement. */
    sys_yield();

    /* Median-of-N: 16 samples is enough to iron out scheduling jitter
     * without burning boot time. */
    #define N_SAMPLES 16
    uint64_t samples[N_SAMPLES];
    unsigned int i;
    for (i = 0; i < N_SAMPLES; i++) {
        uint64_t t0 = read_tsc();
        sys_yield();
        uint64_t t1 = read_tsc();
        samples[i] = t1 - t0;
    }

    /* Insertion sort (N=16; O(N^2) fine). */
    unsigned int j;
    for (i = 1; i < N_SAMPLES; i++) {
        uint64_t key = samples[i];
        j = i;
        while (j > 0 && samples[j - 1] > key) {
            samples[j] = samples[j - 1];
            j--;
        }
        samples[j] = key;
    }
    uint64_t median_cycles = samples[N_SAMPLES / 2];

    /* cycles -> ns: ns = cycles * 1e9 / hz. Order matters to avoid
     * overflow on large cycle counts (TCG can easily hit 10k+ cycles
     * for a single syscall). 10k * 1e9 = 1e13 fits in uint64_t. */
    uint64_t median_ns = (median_cycles * 1000000000ULL) / tsc_hz;

    UTEST_PERF("sys_yield_ns", median_ns);

    /* Threshold: 50_000 ns. TCG is the slowest supported runner and
     * still clears this comfortably (~5k ns observed). Bare metal and
     * KVM/WHPX come in under 500 ns. Raise the number only when a
     * real slowdown needs a new baseline; do NOT widen to hide a perf
     * regression (feedback_test_fail_fix_code_not_test). */
    UTEST_ASSERT(median_ns < 50000ULL,
                 "sys_yield median < 50000 ns (TCG upper bound)");

    UTEST_END();
    return g_fail;
}
