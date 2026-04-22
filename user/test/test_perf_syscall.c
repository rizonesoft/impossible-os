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
 *     up to a full second of boot time -- disproportionate for a
 *     perf smoke. We use a fixed 2 GHz TSC assumption so the [PERF]
 *     ns value is APPROXIMATE (within ~2x of truth across runners).
 *     The actual PASS/FAIL assertion is cycle-based and TSC-Hz
 *     independent -- see `CYCLE_CEILING` below. When a sys_uptime_ns()
 *     syscall lands, this test can switch to it and tighten the
 *     report to true ns. Do NOT tighten the cycle ceiling to hide
 *     host-speed variance; raise it only for a real regression.
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

/* Assumed TSC frequency for the [PERF] ns report only. Real hardware
 * runs anywhere from 1.5 GHz to 4.5 GHz; 2 GHz lands in the middle so
 * the emitted ns value is within ~2x of wall time. The PASS/FAIL
 * assertion below is cycle-based and does not depend on this. */
#define TSC_HZ_ASSUMED  2000000000ULL

/* Cycle ceiling for the median-of-N sys_yield round-trip. Picked to
 * accommodate the slowest supported runner (TCG on a modest host):
 * ~1-2e5 cycles is the observed range; 200_000 gives headroom so the
 * test does not flake from scheduler jitter. Cycle-based so a 4 GHz
 * TSC host and a 2 GHz TSC host are compared against the same bound
 * -- a 50_000 ns ceiling on the synthetic 2 GHz report would fail
 * spuriously on any > 2 GHz CPU. Raise only for a real regression. */
#define CYCLE_CEILING  200000ULL

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
    UTEST_PERF("sys_yield_cycles", median_cycles);

    /* Assert on cycles (TSC-Hz-independent). A synthetic-ns threshold
     * would fail on > 2 GHz CPUs because the 2 GHz assumption above
     * inflates the reported ns by `real_hz / 2e9`. Cycle-based bound
     * keeps the gate honest across every supported host. */
    UTEST_ASSERT(median_cycles < CYCLE_CEILING,
                 "sys_yield median < 200k cycles (TSC-Hz-independent)");

    UTEST_END();
    return g_fail;
}
