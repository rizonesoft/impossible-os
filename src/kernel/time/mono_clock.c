/* ============================================================================
 * mono_clock.c -- Monotonic nanosecond clock source selection
 *
 * Priority: invariant TSC > HPET > LAPIC timer.
 * Uses integer multiply-shift for ns conversion (no division in hot path).
 * ============================================================================ */

#include "kernel/time/mono_clock.h"
#include "kernel/cpuid.h"
#include "kernel/cpu_security.h"
#include "kernel/boot_timing.h"
#include "kernel/drivers/lapic.h"
#include "kernel/drivers/hpet.h"
#include "kernel/acpi.h"
#include "kernel/smp.h"
#include "kernel/klog.h"
#include "kernel/sched/seqlock.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/kworker.h"

/* ---- State --------------------------------------------------------------- */

/* Immutable per-source scale descriptor. mono_clock_init() fills one entry for
 * EACH available source (so the watchdog has a ready demotion target) and never
 * mutates it after; the active source is selected by the atomic index
 * s_active_src. The lock-free mono_ns() reader loads s_active_src ONCE and reads
 * a coherent {source, freq, num, den} from the immutable entry -- a runtime
 * demotion just republishes the index, so source and scale can never tear
 * against each other. An immutable descriptor + atomic index is used rather than
 * a generation seqlock, which would spin the ISR-callable hot path and risk a
 * torn pointer read. Indexed by MONO_SRC_* (0..PMTMR). */
struct mono_desc {
    uint32_t source;          /* MONO_SRC_* (also the array index) */
    uint64_t freq_hz;         /* source frequency in Hz */
    uint64_t num;             /* numerator for ticks -> ns */
    uint64_t den;             /* denominator */
};
static struct mono_desc s_desc[MONO_SRC_PMTMR + 1];
static uint8_t  s_src_avail[MONO_SRC_PMTMR + 1];   /* 1 if entry initialized */
static uint32_t s_active_src = MONO_SRC_NONE;      /* atomic active index */

/* PMTMR 64-bit epoch (wrap extension); see mono_clock_pmtmr_advance() below.
 * Snapshot {epoch_ns, last_raw} is seqlock-protected (writer = timer-ISR
 * advance; readers lock-free). mask/source are set once at init before any
 * reader runs. */
static seqlock_t s_pmtmr_lock = SEQLOCK_INIT;
static uint64_t  s_pmtmr_epoch_ns;   /* banked ns up to s_pmtmr_last_raw */
static uint32_t  s_pmtmr_last_raw;   /* last banked raw PMTMR sample (masked) */
static uint32_t  s_pmtmr_mask;       /* 0xFFFFFFFF (32-bit) or 24-bit mask */
/* External serializer for ALL PMTMR epoch writers. The original advance had a
 * single writer (the BSP timer ISR); the watchdog added more (quiesce sync,
 * demotion re-anchor), and a seqlock does NOT serialize writers, so two
 * concurrent bankers would corrupt the seqlock AND let one compute a masked
 * delta against another's freshly-updated last_raw (a near-full-wrap forward
 * jump). This lock + reading the raw counter INSIDE it makes each bank atomic.
 * Taken irqsave so the BSP ISR advance and a PASSIVE sync/reanchor on another
 * CPU serialize. Order: OUTER to the s_pmtmr_lock seqlock writer. */
static spinlock_t s_pmtmr_wlock = SPINLOCK_INIT;

/* Mono-wide monotonic floor: mono_ns() never returns below any prior return,
 * across ALL sources and across a runtime demotion. Generalizes the original
 * PMTMR-only floor from the PMTMR clocksource work. ALWAYS applied (every
 * mono_ns return clamps to it) -- a gated "only after a demotion" optimization
 * was rejected because a reader that loads the gate as 0 then stalls across a
 * demotion would return an unfloored old value, observable as a backward step
 * relative to a newer reader on the new source. Correctness over the steady-state
 * CAS cost; the hot scheduler/uptime path uses the cheaper mono_ns_coarse(), and
 * mono_floor only writes when the candidate advances. Cacheline-isolated: hit by
 * every read while the ISR writes the PMTMR epoch above, so no false-sharing. */
static uint64_t  s_mono_floor __attribute__((aligned(64)));
/* HPET re-anchor: added to hpet_ns() so a demotion TO HPET continues from the
 * old source's value instead of HPET's native absolute counter. 0 until/unless
 * HPET becomes a demotion target (PMTMR/LAPIC re-anchor via their own epochs;
 * TSC is never a demotion target). Read coherently inside the HPET case, which a
 * reader only enters after acquire-loading s_active_src==HPET (published last in
 * mono_clock_demote, after this store). */
static int64_t   s_hpet_offset_ns;
/* Serializes concurrent demotions AND makes the anchor-sample -> re-anchor ->
 * publish transition nonpreemptible (taken irqsave): otherwise a PASSIVE demoter
 * preempted between re-anchor and publish would let the target advance, so the
 * first post-publish reader leaps forward by the delay. */
static spinlock_t s_demote_lock = SPINLOCK_INIT;

static uint64_t mono_floor(uint64_t cand)
{
    uint64_t cur = __atomic_load_n(&s_mono_floor, __ATOMIC_ACQUIRE);
    while (cand > cur) {
        if (__atomic_compare_exchange_n(&s_mono_floor, &cur, cand, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return cand;
        /* CAS failure reloaded cur; loop re-checks cand > cur */
    }
    return cur;   /* cand <= floor: clamp up so time never moves backward */
}

/* ---- TSC read ------------------------------------------------------------ */

/* Ordered TSC read: the value must not be sampled before prior instructions
 * have executed, or a tight measurement / monotonic read can read "earlier"
 * than its surrounding code (out-of-order CPUs). RDTSCP waits for all prior
 * instructions to retire (and yields TSC_AUX in ECX, which we discard); on
 * CPUs without it, LFENCE serializes before RDTSC. Mirrors Linux
 * rdtsc_ordered(); Win11 QPC abstracts the same. Used by every TSC read here. */
static inline uint64_t rdtsc_ordered(void)
{
    uint32_t lo, hi;
    /* Gate RDTSCP on the all-online feature INTERSECTION, never cpu_has() (which
     * is BSP-global): RDTSCP #UDs on a CPU that lacks it, and APs are an
     * optional-feature-probed set, so a BSP-has/AP-lacks skew would fault this
     * ISR-callable hot path on the skewed AP. Same gate dpc_watchdog_init uses. */
    if (cpu_feature_global_has(CPU_FEATURE_RDTSCP)) {
        uint32_t aux;
        __asm__ volatile ("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    } else {
        __asm__ volatile ("lfence" ::: "memory");
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    }
    return ((uint64_t)hi << 32) | lo;
}

/* The raw counter, published. See mono_clock.h for why this is exposed at all
 * and why it is NOT a substitute for mono_ns(): it is the one time source in
 * this kernel that keeps advancing when the tick does, so a bounded wait can
 * hold a watchdog against it. Routed through rdtsc_ordered() rather than a
 * second inline rdtsc so there is still exactly one TSC read shape in the
 * tree -- an unordered read here would let a watchdog sample land before the
 * loop iteration it is supposed to be timing. */
uint64_t mono_tsc_raw(void)
{
    return rdtsc_ordered();
}


/* ---- Init ---------------------------------------------------------------- */

/* Record an immutable scale descriptor for one available source. */
static void mono_desc_set(uint32_t src, uint64_t freq_hz,
                          uint64_t num, uint64_t den)
{
    s_desc[src].source  = src;
    s_desc[src].freq_hz = freq_hz;
    s_desc[src].num     = num;
    s_desc[src].den     = den;
    s_src_avail[src]    = 1;
}

/* Conservative boot qualification: reject a candidate whose frequency is
 * implausible (zero / absurdly out of range) so a mis-measured TSC or HPET can
 * never become the active clocksource. This is the boot-time sanity gate that
 * replaces a platform-string guess; the live drift watchdog (Phase 3) is the
 * ongoing qualification. Returns 1 if src is available AND plausible. */
static int mono_source_qualify(uint32_t src)
{
    uint64_t f;
    if (src > MONO_SRC_PMTMR || !s_src_avail[src])
        return 0;
    f = s_desc[src].freq_hz;
    switch (src) {
    case MONO_SRC_TSC:   return f >= MONO_TSC_HZ_MIN && f <= MONO_TSC_HZ_MAX;
    case MONO_SRC_HPET:  return f >=   1000000ULL && f <=   1000000000ULL;
    case MONO_SRC_PMTMR: return f == PMTMR_FREQ_HZ;
    case MONO_SRC_LAPIC: return f > 0;
    default:             return 0;
    }
}

/* The measured rate behind mono_tsc_raw(), or 0 when there is none.
 *
 * Gated on QUALIFICATION rather than on availability alone: an unqualified
 * frequency is one this file already refused to build a clocksource on, so
 * handing it out as a conversion rate would launder a value rejected here into
 * a bound somewhere else. Gated on the descriptor rather than on the ACTIVE
 * source, because the TSC's rate does not stop being the TSC's rate when a
 * drift demotion moves timekeeping to HPET. */
uint64_t mono_tsc_hz(void)
{
    /* ACTIVE, not merely qualified, and that is the whole guard rather than
     * belt-and-braces. Boot qualification range-checks the descriptor once and
     * never revisits it, so a TSC the drift watchdog LATER demoted for running
     * off its measured rate still passes -- and handing that rate out is worse
     * than handing out nothing, because a caller converting raw ticks with a
     * rate the kernel has already stopped believing gets a confidently wrong
     * duration. A demotion moves the active source away from TSC and never back
     * to it, so this one test covers "never qualified" and "no longer trusted"
     * alike. Same gate rdtsc_ns() uses, for the same reason. */
    if (__atomic_load_n(&s_active_src, __ATOMIC_ACQUIRE) != MONO_SRC_TSC)
        return 0;
    if (!mono_source_qualify(MONO_SRC_TSC))
        return 0;
    return s_desc[MONO_SRC_TSC].freq_hz;
}

void mono_clock_init(void)
{
    static const uint32_t k_priority[] = {
        MONO_SRC_TSC, MONO_SRC_HPET, MONO_SRC_PMTMR, MONO_SRC_LAPIC
    };
    uint32_t chosen = MONO_SRC_NONE;
    uint32_t i;

    /* Seed the tick-fallback epoch with the live tick frequency: the
     * fallback reader uses ONLY snapshot state (an epoch paired with a
     * live frequency sample would race a resolution change) */
    {
        extern uint32_t system_get_freq(void);
        mono_clock_tick_rebase(system_get_freq());
    }

    /* Probe EVERY source and record its immutable descriptor, so the watchdog
     * has a ready-qualified demotion target -- the active source is selected by
     * priority + qualification afterward, not by first-hit-and-return. */

    /* Invariant TSC with known frequency. num/den = 1e9/freq (no hot-path div). */
    if (cpu_has(CPU_FEATURE_TSC_INV)) {
        uint64_t freq = boot_timing_tsc_freq();
        if (freq > 0)
            mono_desc_set(MONO_SRC_TSC, freq, 1000000000ULL, freq);
    }

    /* HPET main counter. */
    if (hpet_available()) {
        uint64_t freq = hpet_frequency_hz();
        if (freq > 0)
            mono_desc_set(MONO_SRC_HPET, freq, 1000000000ULL, freq);
    }

    /* ACPI PM timer (PMTMR) -- a standalone always-running 3.579545 MHz counter.
     * Seed the wrap-extension epoch from the time already elapsed on the tick
     * fallback so uptime_ns()/mono_ns() are CONTINUOUS across the source switch
     * (a zero-based epoch would make uptime jump backward). Publication order:
     * fully initialize the epoch snapshot under the seqlock + a release fence
     * BEFORE s_active_src is published below, so a concurrent ISR/AP reader
     * never observes a BSS-zero epoch. */
    if (acpi_get_pmtimer_port() != 0) {
        extern uint64_t uptime_ns(void);
        uint64_t base_ns = uptime_ns();
        uint32_t mask = acpi_pmtimer_is_32bit() ? 0xFFFFFFFFu : PMTMR_24BIT_MASK;
        seqlock_write_lock(&s_pmtmr_lock);
        s_pmtmr_mask     = mask;
        s_pmtmr_epoch_ns = base_ns;
        s_pmtmr_last_raw = acpi_pmtimer_read_value() & mask;
        seqlock_write_unlock(&s_pmtmr_lock);
        __atomic_store_n(&s_mono_floor, base_ns, __ATOMIC_RELAXED);
        mono_desc_set(MONO_SRC_PMTMR, PMTMR_FREQ_HZ, 1000000000ULL, PMTMR_FREQ_HZ);
    }

    /* LAPIC timer ticks (per-CPU interrupt counter, last resort). */
    {
        uint32_t ticks_per_ms = lapic_timer_ticks_per_ms();
        if (ticks_per_ms > 0)
            mono_desc_set(MONO_SRC_LAPIC, (uint64_t)ticks_per_ms * 1000,
                          1000000ULL, (uint64_t)ticks_per_ms);
    }

    /* Select the highest-priority source that qualifies. */
    for (i = 0; i < sizeof(k_priority) / sizeof(k_priority[0]); i++) {
        if (mono_source_qualify(k_priority[i])) {
            chosen = k_priority[i];
            break;
        }
    }

    if (chosen == MONO_SRC_NONE) {
        klog(LOG_WARN, "time", "Monotonic clock: no source available");
        return;
    }

    /* A release fence pairs the fully-seeded descriptors + PMTMR epoch with the
     * acquire-load of s_active_src in mono_ns(): a reader that sees the chosen
     * source also sees its descriptor and epoch. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&s_active_src, chosen, __ATOMIC_RELEASE);

    switch (chosen) {
    case MONO_SRC_TSC:
        klog(LOG_INFO, "time", "Monotonic clock: TSC (%u MHz, invariant)",
             (uint64_t)(s_desc[MONO_SRC_TSC].freq_hz / 1000000));
        break;
    case MONO_SRC_HPET:
        klog(LOG_INFO, "time", "Monotonic clock: HPET (%u MHz)",
             (uint64_t)(s_desc[MONO_SRC_HPET].freq_hz / 1000000));
        break;
    case MONO_SRC_PMTMR:
        klog(LOG_INFO, "time", "Monotonic clock: PMTMR (3.579545 MHz, %s)",
             s_pmtmr_mask == 0xFFFFFFFFu ? "32-bit" : "24-bit");
        break;
    case MONO_SRC_LAPIC:
        klog(LOG_INFO, "time", "Monotonic clock: LAPIC (%u ticks/ms)",
             (uint64_t)s_desc[MONO_SRC_LAPIC].den);
        break;
    default:
        break;
    }
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

    if (__atomic_load_n(&s_active_src, __ATOMIC_ACQUIRE) != MONO_SRC_TSC)
        return;

    cpuid_freq = cpuid15_tsc_freq();
    if (cpuid_freq == 0)
        return;

    boot_freq = s_desc[MONO_SRC_TSC].freq_hz;

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

    /* Only valid while TSC is the active source. A demotion moves s_active_src
     * away from TSC (never to it), so when active==TSC no offset/floor applies
     * (the demotion machinery is dormant); use TSC's immutable scale directly. */
    if (__atomic_load_n(&s_active_src, __ATOMIC_ACQUIRE) != MONO_SRC_TSC ||
        s_desc[MONO_SRC_TSC].den == 0)
        return 0;

    ticks = rdtsc_ordered();
    {
        struct per_cpu_data *cpu = smp_this_cpu();
        if (cpu)
            ticks = (uint64_t)((int64_t)ticks + cpu->tsc_offset);
    }

    whole = ticks / s_desc[MONO_SRC_TSC].den;
    rem   = ticks % s_desc[MONO_SRC_TSC].den;
    return whole * s_desc[MONO_SRC_TSC].num
         + (rem * s_desc[MONO_SRC_TSC].num) / s_desc[MONO_SRC_TSC].den;
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

/* ---- PMTMR 64-bit epoch (wrap extension) --------------------------------
 * The ACPI PM timer is a 24/32-bit free-running counter; the 24-bit form
 * wraps every ~4.69 s. mono_clock_pmtmr_advance() (called from the timer ISR
 * far faster than half the wrap) banks elapsed ns into a 64-bit epoch so a
 * lock-free reader only ever extends a short masked delta since the last
 * advance. Snapshot {epoch_ns, last_raw} (declared in the State section) is
 * seqlock-protected so a reader never pairs a stale epoch with a fresh raw
 * sample. */
uint64_t mono_pmtmr_delta_ns(uint32_t last_raw, uint32_t now_raw, uint32_t mask)
{
    /* Masked subtraction extends across at most one wrap. delta < 2^32, so
     * delta * 1e9 < 4.3e18 fits in u64 (max ~1.8e19) -- no split needed. */
    uint32_t delta = (now_raw - last_raw) & mask;
    return (uint64_t)delta * 1000000000ULL / PMTMR_FREQ_HZ;
}

/* Advance every Nth tick, not every tick: the epoch only has to be refreshed
 * faster than half the 24-bit wrap (~2.34 s). Even at the 64 Hz default tick
 * (15.625 ms) this is 125 ms per refresh -- an ~18x margin -- while doing ZERO
 * PM timer port I/O on the other 7 ticks (port reads are slow ACPI-register
 * I/O; keeping them off every tick avoids loading the timer ISR). Called only
 * from the BSP timer ISR, so the plain counter is single-threaded. */
#define PMTMR_ADVANCE_TICKS 8

/* Bank the masked delta since the last advance into the 64-bit epoch. Caller has
 * confirmed PMTMR is active. The raw counter is read INSIDE the writer lock so
 * `now` and `s_pmtmr_last_raw` are sampled/updated as one serialized unit -- a
 * read before the lock could pair a stale `now` with another banker's newer
 * last_raw and fabricate a near-full-wrap forward jump. s_pmtmr_wlock serializes
 * the (now multiple) writers; taken irqsave so the BSP ISR advance and a PASSIVE
 * sync/reanchor on another CPU exclude each other. */
static void mono_pmtmr_bank(void)
{
    uint64_t irqf;
    uint32_t now;
    spin_lock_irqsave(&s_pmtmr_wlock, &irqf);
    now = acpi_pmtimer_read_value() & s_pmtmr_mask;
    seqlock_write_lock(&s_pmtmr_lock);
    s_pmtmr_epoch_ns += mono_pmtmr_delta_ns(s_pmtmr_last_raw, now, s_pmtmr_mask);
    s_pmtmr_last_raw  = now;
    seqlock_write_unlock(&s_pmtmr_lock);
    spin_unlock_irqrestore(&s_pmtmr_wlock, irqf);
}

void mono_clock_pmtmr_advance(void)
{
    static uint32_t s_skip;

    if (__atomic_load_n(&s_active_src, __ATOMIC_ACQUIRE) != MONO_SRC_PMTMR)
        return;
    if (++s_skip < PMTMR_ADVANCE_TICKS)
        return;
    s_skip = 0;
    mono_pmtmr_bank();
}

void mono_clock_pmtmr_sync(void)
{
    /* Unconditional bank (no skip counter) for the tick-quiesce boundary: the
     * timer ISR (which drives mono_clock_pmtmr_advance) is masked across a
     * quiesce, so bank right before masking and right after unmasking. A
     * sub-wrap (<~4.7 s 24-bit) quiesce window is then captured in one masked
     * delta and loses no wraps; the mono-wide floor blocks any backward step for
     * a pathological longer quiesce. No-op unless PMTMR is the active source. */
    if (__atomic_load_n(&s_active_src, __ATOMIC_ACQUIRE) != MONO_SRC_PMTMR)
        return;
    mono_pmtmr_bank();
}

/* Raw per-source monotonic ns: the source's own counter converted to ns, with
 * NO demotion offset and NO mono-wide floor applied (mono_ns() adds those). The
 * caller passes the source id it acquire-loaded so a concurrent demotion cannot
 * change the source mid-read. Also used by mono_clock_demote() to read the new
 * target's raw value before computing its continuation offset. */
static uint64_t mono_raw_ns(uint32_t s)
{
    switch (s) {
    case MONO_SRC_TSC: {
        uint64_t ticks = rdtsc_ordered();
        struct per_cpu_data *cpu = smp_this_cpu();
        if (cpu)
            ticks = (uint64_t)((int64_t)ticks + cpu->tsc_offset);
        /* Split multiply to avoid u64 overflow: ticks*1e9 overflows at ~18 s. */
        {
            uint64_t den = s_desc[MONO_SRC_TSC].den;
            uint64_t num = s_desc[MONO_SRC_TSC].num;
            uint64_t whole, rem;
            if (den == 0)
                return 0;
            whole = ticks / den;
            rem   = ticks % den;
            return whole * num + (rem * num) / den;
        }
    }

    case MONO_SRC_HPET:
        /* hpet_ns() is HPET's native absolute counter; the offset (0 unless HPET
         * is a demotion target) re-bases it onto the old source's value. */
        return hpet_ns() +
               (uint64_t)__atomic_load_n(&s_hpet_offset_ns, __ATOMIC_ACQUIRE);

    case MONO_SRC_PMTMR:
        /* Precise PMTMR read: banked epoch ns + the masked delta since the last
         * ISR advance, glitch-filtered (median-of-3) so a single bad sample
         * cannot fabricate a forward jump. The raw counter is sampled INSIDE the
         * read attempt so an advance between the snapshot and the sample forces a
         * retry. (The monotonic floor is applied by the caller mono_ns().) */
        {
            uint64_t seq, base_ns;
            uint32_t base_raw, now;
            do {
                seq      = seqlock_read_begin(&s_pmtmr_lock);
                base_ns  = s_pmtmr_epoch_ns;
                base_raw = s_pmtmr_last_raw;
                now      = acpi_pmtimer_read_value() & s_pmtmr_mask;
            } while (seqlock_read_retry(&s_pmtmr_lock, seq));
            return base_ns + mono_pmtmr_delta_ns(base_raw, now, s_pmtmr_mask);
        }

    case MONO_SRC_LAPIC:
        /* LAPIC tick count is not directly readable as a monotonic counter.
         * Use system_get_ticks() (PIT/LAPIC interrupt counter): banked epoch ns
         * + ticks-since-epoch at the LIVE frequency, so a runtime resolution
         * change can never rewind the clock. */
        {
            extern uint64_t system_get_ticks(void);
            uint64_t seq, base_ns, base_ticks, t;
            uint32_t base_freq;
            do {
                seq        = seqlock_read_begin(&s_tick_epoch_lock);
                base_ns    = s_tick_epoch_ns;
                base_ticks = s_tick_epoch_ticks;
                base_freq  = s_tick_epoch_freq;
                t = system_get_ticks();
            } while (seqlock_read_retry(&s_tick_epoch_lock, seq));
            if (t < base_ticks)
                t = base_ticks;
            return base_ns + mono_lapic_ticks_to_ns(t - base_ticks, base_freq);
        }

    default:
        return 0;
    }
}

uint64_t mono_ns(void)
{
    /* Acquire-load the active source ONCE. Pairs with the RELEASE publish in
     * mono_clock_init / mono_clock_demote, so a reader that sees a source also
     * sees its fully-seeded descriptor, epoch, and HPET re-anchor offset. */
    uint32_t s = __atomic_load_n(&s_active_src, __ATOMIC_ACQUIRE);
    uint64_t cand = mono_raw_ns(s);

    /* ALWAYS floor: guarantees global monotonicity across every source and every
     * runtime demotion, including for a reader sampled on the old source just
     * before a demotion (a gated floor would let such an in-flight reader return
     * an unfloored old value, observable as backward vs a newer reader). */
    return mono_floor(cand);
}

uint64_t mono_ns_coarse(void)
{
    /* Cheap monotonic read for the scheduler / uptime hot path. For PMTMR the
     * precise mono_ns() costs 3 port reads (a glitch-filtered median); return
     * the cached epoch ns instead -- accurate to the ISR advance interval,
     * monotonic (the epoch only banks positive masked deltas), and free of port
     * I/O. Other sources are already cheap, so fall through to mono_ns(). */
    if (__atomic_load_n(&s_active_src, __ATOMIC_ACQUIRE) == MONO_SRC_PMTMR) {
        uint64_t seq, ns;
        do {
            seq = seqlock_read_begin(&s_pmtmr_lock);
            ns  = s_pmtmr_epoch_ns;
        } while (seqlock_read_retry(&s_pmtmr_lock, seq));
        /* PMTMR re-anchors via its own epoch on demotion (no separate offset),
         * so the cached epoch already reflects any switch. Share the SAME
         * monotonic floor as precise mono_ns(): a precise read may have raised
         * the floor past the cached epoch, so clamping here keeps coarse
         * non-decreasing relative to a prior precise read when the two APIs are
         * mixed (uptime_ns / wall_clock do mix them). */
        return mono_floor(ns);
    }
    return mono_ns();
}

uint64_t mono_filetime_units(void)
{
    /* FILETIME units = 100 ns intervals = mono_ns() / 100 */
    return mono_ns() / 100;
}

/* ---- Info ---------------------------------------------------------------- */

static const char *mono_src_name(uint32_t s)
{
    switch (s) {
    case MONO_SRC_TSC:   return "TSC";
    case MONO_SRC_HPET:  return "HPET";
    case MONO_SRC_PMTMR: return "PMTMR";
    case MONO_SRC_LAPIC: return "LAPIC";
    default:             return "none";
    }
}

const char *mono_clock_source_name(void)
{
    return mono_src_name(__atomic_load_n(&s_active_src, __ATOMIC_ACQUIRE));
}

uint32_t mono_clock_source_id(void)
{
    return __atomic_load_n(&s_active_src, __ATOMIC_ACQUIRE);
}

/* ---- Clocksource quality watchdog (drift demotion) ----------------------- */

uint32_t mono_drift_ppm(uint64_t ref_ns, uint64_t src_ns)
{
    uint64_t diff, ppm;
    if (ref_ns == 0)
        return 0;
    diff = (src_ns > ref_ns) ? (src_ns - ref_ns) : (ref_ns - src_ns);
    /* diff >= ref means >= 100 % drift: saturate. */
    if (diff >= ref_ns)
        return MONO_DRIFT_PPM_MAX;
    /* Overflow-safe without a 128-bit divide (no compiler-rt __udivti3 in the
     * freestanding kernel): scale diff and ref down by the SAME power of two
     * (ratio preserved) until diff * 1e6 fits in u64, so even a large window
     * (a delayed watchdog, or any future caller, with ref_ns above ~5.1 h)
     * cannot overflow. diff < ref throughout, so ref never shifts to 0 first. */
    while (diff > 0xFFFFFFFFFFFFFFFFULL / 1000000ULL) {
        diff   >>= 1;
        ref_ns >>= 1;
    }
    if (ref_ns == 0)
        return MONO_DRIFT_PPM_MAX;
    ppm = diff * 1000000ULL / ref_ns;
    return (ppm > MONO_DRIFT_PPM_MAX) ? MONO_DRIFT_PPM_MAX : (uint32_t)ppm;
}

/* Re-anchor a demotion target so its next mono_raw_ns() read == anchor_ns,
 * instead of the source's native absolute value. PASSIVE context (the watchdog).
 * Each source uses its own native mechanism so the value is read coherently by
 * mono_raw_ns without a separate global offset: HPET an additive offset, PMTMR
 * its seqlock epoch, LAPIC its seqlock tick epoch. TSC is never a target. */
static void mono_source_reanchor(uint32_t to_src, uint64_t anchor_ns)
{
    switch (to_src) {
    case MONO_SRC_HPET:
        __atomic_store_n(&s_hpet_offset_ns,
                         (int64_t)anchor_ns - (int64_t)hpet_ns(),
                         __ATOMIC_RELEASE);
        break;
    case MONO_SRC_PMTMR: {
        /* Same writer serialization as mono_pmtmr_bank: take s_pmtmr_wlock so
         * this re-anchor cannot race the ISR advance / a concurrent sync. */
        uint64_t irqf;
        uint32_t raw;
        spin_lock_irqsave(&s_pmtmr_wlock, &irqf);
        raw = acpi_pmtimer_read_value() & s_pmtmr_mask;
        seqlock_write_lock(&s_pmtmr_lock);
        s_pmtmr_last_raw = raw;
        s_pmtmr_epoch_ns = anchor_ns;
        seqlock_write_unlock(&s_pmtmr_lock);
        spin_unlock_irqrestore(&s_pmtmr_wlock, irqf);
        break;
    }
    case MONO_SRC_LAPIC: {
        extern uint64_t system_get_ticks(void);
        seqlock_write_lock(&s_tick_epoch_lock);
        s_tick_epoch_ticks = system_get_ticks();
        s_tick_epoch_ns    = anchor_ns;
        /* keep s_tick_epoch_freq -- only the epoch base/ticks re-anchor */
        seqlock_write_unlock(&s_tick_epoch_lock);
        break;
    }
    default:
        break;   /* TSC is never a demotion target */
    }
}

void mono_clock_demote(uint32_t to_src, uint32_t drift_ppm)
{
    uint32_t cur = __atomic_load_n(&s_active_src, __ATOMIC_ACQUIRE);
    uint64_t anchor, irqf;

    /* Only demote to a QUALIFIED target (not merely present): the boot sanity
     * gate refused an implausible-frequency source, and the watchdog must not
     * route around that by switching the active clock to it. */
    if (!mono_source_qualify(to_src) || to_src == cur)
        return;

    /* The anchor sample -> re-anchor -> publish must be one bounded,
     * nonpreemptible transition: s_demote_lock taken irqsave both serializes
     * concurrent demotions and disables interrupts/preemption on this CPU so the
     * target cannot advance between the re-anchor and the publish (which would
     * make the first post-publish reader leap forward by the delay). The window
     * is two short hardware reads -- acceptable for a rare demotion. */
    spin_lock_irqsave(&s_demote_lock, &irqf);

    /* Re-check under the lock: a racing demoter may have already switched, and
     * re-confirm qualification holds for the target. */
    cur = __atomic_load_n(&s_active_src, __ATOMIC_ACQUIRE);
    if (to_src == cur || !mono_source_qualify(to_src)) {
        spin_unlock_irqrestore(&s_demote_lock, irqf);
        return;
    }

    /* Sample the current value (mono_ns always floors, so anchor >= the floor),
     * re-anchor the target to it, and raise the floor to the anchor so any
     * reader after the publish is clamped up to it -- the new source continues
     * from `anchor`, not its native absolute value. */
    anchor = mono_ns();
    mono_source_reanchor(to_src, anchor);
    (void)mono_floor(anchor);

    /* Publish the new source LAST (RELEASE): a reader that acquire-loads it also
     * sees the re-anchor written above. */
    __atomic_store_n(&s_active_src, to_src, __ATOMIC_RELEASE);

    spin_unlock_irqrestore(&s_demote_lock, irqf);

    /* klog OUTSIDE the lock (serial I/O must not run under an irqsave spinlock). */
    klog(LOG_WARN, "time", "clocksource: demoting %s -> %s (%u ppm)",
         mono_src_name(cur), mono_src_name(to_src), (uint64_t)drift_ppm);
}

/* ---- Watchdog kworker job ------------------------------------------------- *
 * Runs at PASSIVE on the kworker pool (~0.5 s). Monitors ONLY an active TSC --
 * the one source that drifts under SMM/C-state/thermal effects -- against an
 * HPET reference, and demotes past MONO_DRIFT_UNSTABLE_PPM for two consecutive
 * windows (one outlier from a delayed sample never demotes).
 *
 * Reference is HPET ONLY (deliberate scope). HPET is a native, continuous,
 * non-wrapping, TSC-independent counter, so the drift ratio is correct over ANY
 * window regardless of kworker scheduling delay, timer-resolution changes, or
 * timer-mask quiesce. A PMTMR reference was rejected: its 24-bit counter wraps
 * (~4.69 s) and bounding that wrap needs a clock that is simultaneously
 * TSC-independent, rate-epoch-correct, AND continuous across a timer-mask -- no
 * such clock exists here, so every window oracle had a false-demotion gap.
 * PMTMR-reference monitoring is a tracked follow-up; a no-HPET system simply
 * runs no automatic drift watchdog (matching Linux: no watchdog clocksource ->
 * no watchdog), while boot qualification + the demotion machinery stay intact. */
static uint64_t s_wd_last_act;     /* previous active-TSC ns sample */
static uint64_t s_wd_last_ref;     /* previous HPET reference ns sample */
static int      s_wd_primed;       /* 1 once baseline samples are captured */
static uint32_t s_wd_strikes;      /* consecutive over-threshold windows */

/* One bracketed sample: ref_before -> active read -> ref_after, so the active
 * value is pinned to a known HPET span. Returns 0 (discard) if a delay
 * (IRQ/SMI/preemption) crept between the reads (span > MONO_WATCHDOG_MAX_SKEW_NS),
 * which would otherwise fabricate drift; else writes *act_out + *ref_out
 * (ref_after, within the bounded span of the active read). */
static int mono_watchdog_sample(uint64_t *act_out, uint64_t *ref_out)
{
    uint64_t ref_before = hpet_ns();
    uint64_t act        = mono_raw_ns(MONO_SRC_TSC);
    uint64_t ref_after  = hpet_ns();
    uint64_t span = (ref_after > ref_before) ? (ref_after - ref_before) : 0;
    if (span > MONO_WATCHDOG_MAX_SKEW_NS)
        return 0;
    *act_out = act;
    *ref_out = ref_after;
    return 1;
}

static void mono_watchdog_tick(void *ctx)
{
    uint32_t ppm;
    uint64_t act, ref, act_d, ref_d;
    (void)ctx;

    if (__atomic_load_n(&s_active_src, __ATOMIC_ACQUIRE) != MONO_SRC_TSC) {
        s_wd_primed = 0;   /* nothing to monitor (already demoted, etc.) */
        return;
    }

    /* Every sample brackets the active read between two HPET reads and discards a
     * window whose bracket span shows a delay slipped in -- so a contaminated
     * sample cannot fabricate drift. On discard, re-baseline (no strike). */
    if (!mono_watchdog_sample(&act, &ref)) {
        s_wd_strikes = 0;
        if (s_wd_primed) { s_wd_last_act = 0; s_wd_last_ref = 0; }
        s_wd_primed = 0;   /* force a fresh clean prime next window */
        return;
    }

    /* Prime: capture baselines on the first clean window, no comparison. */
    if (!s_wd_primed) {
        s_wd_last_act = act;
        s_wd_last_ref = ref;
        s_wd_primed   = 1;
        s_wd_strikes  = 0;
        return;
    }

    /* Both endpoints are bracket-bounded (each within MONO_WATCHDOG_MAX_SKEW_NS),
     * so the per-window asymmetry stays well under the drift threshold. HPET
     * never wraps, so an arbitrarily long window is still measured correctly.
     * Clamp a (monotonic) non-advance to avoid underflow. */
    act_d = (act > s_wd_last_act) ? (act - s_wd_last_act) : 0;
    ref_d = (ref > s_wd_last_ref) ? (ref - s_wd_last_ref) : 0;
    s_wd_last_act = act;
    s_wd_last_ref = ref;

    if (ref_d == 0)
        return;   /* no reference progress this window -- skip */

    ppm = mono_drift_ppm(ref_d, act_d);
    if (ppm > MONO_DRIFT_UNSTABLE_PPM) {
        /* Two consecutive over-threshold windows before demoting: one outlier
         * from a delayed sample or an SMM excursion never demotes on its own. */
        if (++s_wd_strikes >= 2) {
            mono_clock_demote(MONO_SRC_HPET, ppm);
            s_wd_strikes = 0;
        }
    } else {
        s_wd_strikes = 0;
    }
}

void mono_clock_watchdog_init(void)
{
    static int s_registered;

    if (s_registered)
        return;

    /* Only an active TSC is worth monitoring, and only against a QUALIFIED HPET
     * reference (continuous + wrap-free + TSC-independent). No HPET -> no
     * automatic watchdog (boot qualification + demotion machinery stay intact). */
    if (__atomic_load_n(&s_active_src, __ATOMIC_ACQUIRE) != MONO_SRC_TSC)
        return;
    if (!mono_source_qualify(MONO_SRC_HPET))
        return;

    if (kworker_register(mono_watchdog_tick, (void *)0, 500) >= 0) {
        s_registered = 1;
        klog(LOG_INFO, "time",
             "clocksource watchdog: monitoring TSC against HPET reference");
    }
}
