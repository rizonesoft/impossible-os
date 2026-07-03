/* ============================================================================
 * test_perf_syscall.c -- perf binary: syscall latency baseline
 *
 * Measures round-trip ring 3 -> kernel -> ring 3 for the cheapest wired
 * syscall (SYS_YIELD, handler returns 0). Reports the median of N
 * samples via UTEST_PERF as `[PERF] sys_yield_ns=<n>` (trend keys keep
 * median semantics; scraped by the launcher's XML/JSON emit path and
 * log parsers) and GATES on the min of N (load-tolerant -- see
 * CYCLE_CEILING).
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

/* Cycle ceiling for the MIN-of-N sys_yield round-trip. Cycle-based so
 * a 4 GHz TSC host and a 2 GHz TSC host are compared against the same
 * bound -- a 50_000 ns ceiling on the synthetic 2 GHz report would
 * fail spuriously on any > 2 GHz CPU.
 *
 * The gate uses MIN, not median: under TCG the guest TSC tracks HOST
 * time, so unrelated host load inflates most samples and drags the
 * median past any fixed bound. The minimum is achieved by whichever
 * sample escaped host preemption, while a real syscall-path
 * regression inflates every sample INCLUDING the min.
 *
 * Ceiling derivation (measured on the WSL2 TCG runner): idle median
 * ~160k cycles, idle min ~150k; under sustained host multi-tenancy
 * even the min-of-two-rounds reaches ~212k (host bursts outlast any
 * in-test retry, so the bound itself must carry the load headroom).
 * 400k = ~2.5x the idle floor: a syscall-path regression that doubles
 * the idle cost still trips it, host multi-tenancy does not. Raise
 * only for a real, understood regression -- never to mask one. */
#define CYCLE_CEILING  400000ULL

#define N_SAMPLES 32

/* One measurement round: N samples, sorted; returns via out-params the
 * min and median. Sorting is insertion sort (N=32; O(N^2) fine). */
static void measure_round(uint64_t *min_out, uint64_t *median_out)
{
    uint64_t samples[N_SAMPLES];
    unsigned int i, j;
    for (i = 0; i < N_SAMPLES; i++) {
        uint64_t t0 = read_tsc();
        sys_yield();
        uint64_t t1 = read_tsc();
        samples[i] = t1 - t0;
    }
    for (i = 1; i < N_SAMPLES; i++) {
        uint64_t key = samples[i];
        j = i;
        while (j > 0 && samples[j - 1] > key) {
            samples[j] = samples[j - 1];
            j--;
        }
        samples[j] = key;
    }
    *min_out = samples[0];
    *median_out = samples[N_SAMPLES / 2];
}

int main(void)
{
    UTEST_BEGIN("test_perf_syscall");

    /* Warm: exercise the syscall path once so we do not bill a cold-I$
     * miss against the measurement. */
    sys_yield();

    uint64_t min_cycles, median_cycles;
    measure_round(&min_cycles, &median_cycles);

    /* Load-tolerance retry (bounded, one extra round): if even the MIN
     * blew the ceiling, the host was likely saturated for the entire
     * first round (TCG time-shares the host with whatever else runs).
     * Re-measure once and take the better round. A real regression
     * fails both rounds identically; the retry costs ~32 extra
     * syscalls (microseconds) instead of a whole-suite rerun. */
    if (min_cycles >= CYCLE_CEILING) {
        uint64_t min2, median2;
        measure_round(&min2, &median2);
        if (min2 < min_cycles) {
            min_cycles = min2;
            median_cycles = median2;
        }
    }

    /* cycles -> ns: ns = cycles * 1e9 / hz. Order matters to avoid
     * overflow on large cycle counts. TCG can easily hit 1e5 cycles
     * for a single syscall; 1e5 * 1e9 = 1e14, fits uint64_t
     * comfortably. Approximate (TSC_HZ_ASSUMED is a fixed 2 GHz). */
    uint64_t median_ns = (median_cycles * 1000000000ULL) / TSC_HZ_ASSUMED;

    /* Existing keys keep their MEDIAN semantics (trend scrapers depend
     * on them); the min the gate uses is reported alongside. */
    UTEST_PERF("sys_yield_ns", median_ns);
    UTEST_PERF("sys_yield_cycles", median_cycles);
    UTEST_PERF("sys_yield_min_cycles", min_cycles);

    /* Assert on MIN cycles (TSC-Hz-independent, host-load-tolerant --
     * see CYCLE_CEILING comment). A synthetic-ns threshold would fail
     * on > 2 GHz CPUs because the 2 GHz assumption above inflates the
     * reported ns by `real_hz / 2e9`. */
    UTEST_ASSERT(min_cycles < CYCLE_CEILING,
                 "sys_yield min < 400k cycles (TSC-Hz-independent, load-tolerant)");

    UTEST_END();
    return g_fail;
}
