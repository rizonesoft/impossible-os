/* ============================================================================
 * mono_clock.c -- Monotonic nanosecond clock source selection
 *
 * Priority: invariant TSC > HPET > LAPIC timer.
 * Uses integer multiply-shift for ns conversion (no division in hot path).
 * ============================================================================ */

#include "kernel/time/mono_clock.h"
#include "kernel/cpuid.h"
#include "kernel/boot_timing.h"
#include "kernel/drivers/lapic.h"
#include "kernel/drivers/hpet.h"
#include "kernel/smp.h"
#include "kernel/klog.h"
#include "kernel/sched/seqlock.h"

/* ---- State --------------------------------------------------------------- */

static uint32_t s_source = MONO_SRC_NONE;
static uint64_t s_freq_hz;            /* source frequency in Hz */
static uint64_t s_ns_per_tick_num;    /* numerator for ticks -> ns */
static uint64_t s_ns_per_tick_den;    /* denominator */

/* ---- TSC read ------------------------------------------------------------ */

static inline uint64_t rdtsc_read(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- Init ---------------------------------------------------------------- */

void mono_clock_init(void)
{
    /* Seed the tick-fallback epoch with the live tick frequency: the
     * fallback reader uses ONLY snapshot state (an epoch paired with a
     * live frequency sample would race a resolution change) */
    {
        extern uint32_t system_get_freq(void);
        mono_clock_tick_rebase(system_get_freq());
    }

    /* Try 1: Invariant TSC with known frequency */
    if (cpu_has(CPU_FEATURE_TSC_INV)) {
        uint64_t freq = boot_timing_tsc_freq();
        if (freq > 0) {
            s_source = MONO_SRC_TSC;
            s_freq_hz = freq;
            /* ns = ticks * 1000000000 / freq
             * Store as num/den to avoid 64-bit division in hot path.
             * For typical freq ~4 GHz: num=1000000000, den=freq */
            s_ns_per_tick_num = 1000000000ULL;
            s_ns_per_tick_den = freq;
            klog(LOG_INFO, "time",
                 "Monotonic clock: TSC (%u MHz, invariant)",
                 (uint64_t)(freq / 1000000));
            return;
        }
    }

    /* Try 2: HPET main counter */
    if (hpet_available()) {
        uint64_t freq = hpet_frequency_hz();
        if (freq > 0) {
            s_source = MONO_SRC_HPET;
            s_freq_hz = freq;
            s_ns_per_tick_num = 1000000000ULL;
            s_ns_per_tick_den = freq;
            klog(LOG_INFO, "time",
                 "Monotonic clock: HPET (%u MHz)",
                 (uint64_t)(freq / 1000000));
            return;
        }
    }

    /* Try 3: LAPIC timer ticks */
    {
        uint32_t ticks_per_ms = lapic_timer_ticks_per_ms();
        if (ticks_per_ms > 0) {
            s_source = MONO_SRC_LAPIC;
            s_freq_hz = (uint64_t)ticks_per_ms * 1000;
            s_ns_per_tick_num = 1000000ULL;  /* ns per ms */
            s_ns_per_tick_den = (uint64_t)ticks_per_ms;
            klog(LOG_INFO, "time",
                 "Monotonic clock: LAPIC (%u ticks/ms)",
                 (uint64_t)ticks_per_ms);
            return;
        }
    }

    /* No clock source available */
    klog(LOG_WARN, "time", "Monotonic clock: no source available");
}


/* ---- CPUID 0x15 TSC frequency cross-check -------------------------------- */

static uint64_t cpuid15_tsc_freq(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x15), "c"(0));
    /* eax = denominator, ebx = numerator, ecx = crystal clock Hz (0 = unknown) */
    if (eax == 0 || ebx == 0)
        return 0;
    if (ecx == 0)
        return 0;  /* crystal freq not reported -- can't compute */
    return (uint64_t)ecx * ebx / eax;
}

void mono_clock_crosscheck_tsc(void)
{
    uint64_t cpuid_freq, boot_freq;

    if (s_source != MONO_SRC_TSC)
        return;

    cpuid_freq = cpuid15_tsc_freq();
    if (cpuid_freq == 0)
        return;

    boot_freq = s_freq_hz;

    /* Check within 1% */
    uint64_t delta = (cpuid_freq > boot_freq)
                   ? cpuid_freq - boot_freq : boot_freq - cpuid_freq;
    uint64_t threshold = boot_freq / 100;

    if (delta <= threshold) {
        klog(LOG_DEBUG, "time",
             "TSC freq cross-check: CPUID 0x15 = %u MHz, boot = %u MHz -- OK",
             (uint64_t)(cpuid_freq / 1000000),
             (uint64_t)(boot_freq / 1000000));
    } else {
        klog(LOG_WARN, "time",
             "TSC freq mismatch: CPUID 0x15 = %u MHz, boot = %u MHz (>1%%)",
             (uint64_t)(cpuid_freq / 1000000),
             (uint64_t)(boot_freq / 1000000));
    }
}

/* ---- Fast TSC read with scale -------------------------------------------- */

uint64_t rdtsc_ns(void)
{
    uint64_t ticks;
    uint64_t whole, rem;

    if (s_source != MONO_SRC_TSC || s_ns_per_tick_den == 0)
        return 0;

    ticks = rdtsc_read();
    {
        struct per_cpu_data *cpu = smp_this_cpu();
        if (cpu)
            ticks = (uint64_t)((int64_t)ticks + cpu->tsc_offset);
    }

    whole = ticks / s_ns_per_tick_den;
    rem   = ticks % s_ns_per_tick_den;
    return whole * s_ns_per_tick_num
         + (rem * s_ns_per_tick_num) / s_ns_per_tick_den;
}

/* ---- Reads --------------------------------------------------------------- */

/* Pure tick-counter scaling for the LAPIC/PIT fallback source (no
 * hardware access; unit-testable). freq 0 = no tick source yet. */
uint64_t mono_lapic_ticks_to_ns(uint64_t ticks, uint32_t freq_hz)
{
    if (freq_hz == 0)
        return 0;
    /* Split division: ticks * 1e9 overflows u64 after ~106 days at a
     * 2000 Hz tick; (ticks % freq) < freq keeps the partial in range */
    return (ticks / freq_hz) * 1000000000ULL
         + ((ticks % freq_hz) * 1000000000ULL) / freq_hz;
}

/* ---- Tick-frequency epoch (resolution-change monotonicity) --------------
 * A lifetime tick counter must NEVER be rescaled by the live frequency:
 * ticks accumulated at 100 Hz converted at 1000 Hz would rewind the
 * clock 10x. On every frequency change the accumulated time is banked
 * into an ns epoch and only ticks SINCE the change use the new rate.
 * Readers run lock-free (mono_ns is ISR-callable); the writer is the
 * rare KeSetTimerResolution path. */
static seqlock_t s_tick_epoch_lock = SEQLOCK_INIT;
static uint64_t  s_tick_epoch_ns;
static uint64_t  s_tick_epoch_ticks;
static uint32_t  s_tick_epoch_freq;   /* rate for ticks SINCE the epoch;
                                       * part of the snapshot so a reader
                                       * never pairs an old epoch with a
                                       * new live frequency */

void mono_clock_tick_rebase(uint32_t new_freq_hz)
{
    extern uint64_t system_get_ticks(void);
    uint64_t t;

    seqlock_write_lock(&s_tick_epoch_lock);
    /* Sample INSIDE the critical section: a pre-lock sample from a
     * concurrent rebase could be older than the published epoch and
     * underflow the delta. Clamp as a second layer. */
    t = system_get_ticks();
    if (t < s_tick_epoch_ticks)
        t = s_tick_epoch_ticks;
    /* Bank at the OLD stored rate -- the ticks being banked were
     * accumulated under it, never under the incoming rate */
    s_tick_epoch_ns   += mono_lapic_ticks_to_ns(t - s_tick_epoch_ticks,
                                                s_tick_epoch_freq);
    s_tick_epoch_ticks = t;
    s_tick_epoch_freq  = new_freq_hz;
    seqlock_write_unlock(&s_tick_epoch_lock);
}

uint64_t mono_ns(void)
{
    uint64_t ticks;

    switch (s_source) {
    case MONO_SRC_TSC:
        ticks = rdtsc_read();
        /* Apply per-CPU TSC offset for SMP coherence */
        {
            struct per_cpu_data *cpu = smp_this_cpu();
            if (cpu)
                ticks = (uint64_t)((int64_t)ticks + cpu->tsc_offset);
        }
        /* Use 128-bit multiply to avoid overflow:
         * result = (ticks * num) / den
         * For ~4 GHz TSC and 64-bit ticks, ticks * 1e9 overflows at ~18 seconds.
         * Split: ns = (ticks / den) * num + ((ticks % den) * num) / den */
        {
            uint64_t whole = ticks / s_ns_per_tick_den;
            uint64_t rem   = ticks % s_ns_per_tick_den;
            return whole * s_ns_per_tick_num
                 + (rem * s_ns_per_tick_num) / s_ns_per_tick_den;
        }

    case MONO_SRC_HPET:
        return hpet_ns();

    case MONO_SRC_LAPIC:
        /* LAPIC tick count is not directly readable as a monotonic counter.
         * Use system_get_ticks() (PIT/LAPIC interrupt counter): banked
         * epoch ns + ticks-since-epoch at the LIVE frequency, so a
         * runtime resolution change can never rewind the clock. */
        {
            extern uint64_t system_get_ticks(void);
            uint64_t seq, base_ns, base_ticks, t;
            uint32_t base_freq;
            do {
                seq        = seqlock_read_begin(&s_tick_epoch_lock);
                base_ns    = s_tick_epoch_ns;
                base_ticks = s_tick_epoch_ticks;
                base_freq  = s_tick_epoch_freq;
                /* Sample INSIDE the read attempt: a rebase between the
                 * snapshot and the tick sample must force a retry, or a
                 * post-rebase tick pairs with the stale rate */
                t = system_get_ticks();
            } while (seqlock_read_retry(&s_tick_epoch_lock, seq));
            if (t < base_ticks)
                t = base_ticks;
            return base_ns + mono_lapic_ticks_to_ns(t - base_ticks,
                                                    base_freq);
        }

    default:
        return 0;
    }
}

uint64_t mono_filetime_units(void)
{
    /* FILETIME units = 100 ns intervals = mono_ns() / 100 */
    return mono_ns() / 100;
}

/* ---- Info ---------------------------------------------------------------- */

const char *mono_clock_source_name(void)
{
    switch (s_source) {
    case MONO_SRC_TSC:   return "TSC";
    case MONO_SRC_HPET:  return "HPET";
    case MONO_SRC_LAPIC: return "LAPIC";
    default:             return "none";
    }
}

uint32_t mono_clock_source_id(void)
{
    return s_source;
}
