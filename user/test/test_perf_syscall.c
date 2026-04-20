/* ============================================================================
 * test_perf_syscall.c -- §8 perf binary: syscall latency baseline
 *
 * Measures round-trip ring 3 -> kernel -> ring 3 for the cheapest wired
 * syscall (SYS_YIELD, handler returns 0) and reports the median of N
 * samples via UTEST_PERF as `[PERF] sys_yield_ns=<n>`. The [PERF] line
 * is scraped by the launcher's XML/JSON emit path and by log parsers
 * for trend analysis.
 *
 * Baseline policy (today, pre tests/perf-baseline.json consumer):
 *   - The binary itself owns the threshold via UTEST_ASSERT. A future
 *     libc JSON helper will read tests/perf-baseline.json and let the
 *     launcher or the test do drift detection.
 *   - TCG (WSL2 default) is orders of magnitude slower than KVM/WHPX
 *     or bare metal. The threshold chosen here (50_000 ns for a single
 *     sys_yield round-trip) is generous enough to pass on TCG and on
 *     real hardware, so the test does not flake across runners. Real
 *     drift detection lives in the baseline JSON.
 *
 * TSC -> ns conversion:
 *   - Ring 3 can read TSC directly (Impossible OS leaves CR4.TSD=0).
 *   - User mode has only `sys_uptime()` (1-second granularity) for
 *     wall-clock time. Calibrating TSC against sys_uptime would cost
 *     up to a full second of boot time (the spin-until-sec-advances
 *     loop) -- disproportionate for a perf smoke. We instead use a
 *     fixed 2 GHz TSC assumption so ns values are APPROXIMATE across
 *     runners. The launcher's 50_000 ns ceiling is wide enough that
 *     a 2x TSC-frequency estimation error on either side still passes.
 *     When a sys_uptime_ns() (or CPUID-derived Hz) syscall lands, this
 *     test should switch to it and narrow the threshold.
 * ============================================================================ */

#include "test.h"

UTEST_DEFINE_STATE();

/* Serialise and read TSC. LFENCE + RDTSC is the standard ordering for
 * latency measurement on modern x86. Prevents out-of-order execution
 * from polluting the delta. */
static inline uint64_t read_tsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("lfence; rdtsc"
                         : "=a"(lo), "=d"(hi)
                         :
                         : "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* Assumed TSC frequency for cycles -> ns conversion. Deliberately a
 * fixed value (not a runtime calibration) so this test adds ~0 ms to
 * boot time. Real hardware runs anywhere from 1.5 GHz to 4.5 GHz;
 * 2 GHz lands in the middle so ns reports are within ~2x of truth,
 * which is well inside the 50_000 ns ceiling. See header comment. */
#define TSC_HZ_ASSUMED  2000000000ULL

int main(void)
{
    UTEST_BEGIN("test_perf_syscall");

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
     * overflow on large cycle counts. TCG can easily hit 1e5 cycles
     * for a single syscall; 1e5 * 1e9 = 1e14, fits uint64_t
     * comfortably. Approximate (TSC_HZ_ASSUMED is a fixed 2 GHz). */
    uint64_t median_ns = (median_cycles * 1000000000ULL) / TSC_HZ_ASSUMED;

    UTEST_PERF("sys_yield_ns", median_ns);

    /* Threshold: 50_000 ns. TCG is the slowest supported runner and
     * still clears this comfortably (~5k ns observed). Bare metal and
     * KVM/WHPX come in well under. Raise the number only when a real
     * slowdown needs a new baseline; do NOT widen to hide a perf
     * regression (feedback_test_fail_fix_code_not_test). */
    UTEST_ASSERT(median_ns < 50000ULL,
                 "sys_yield median < 50000 ns (TCG upper bound)");

    UTEST_END();
    return g_fail;
}
