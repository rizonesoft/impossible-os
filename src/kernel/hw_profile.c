/*
 * hw_profile.c -- Boot-time hardware self-benchmark + auto-tune (TODO-09 S14)
 *
 * See include/kernel/hw_profile.h for the contract. Runs once in boot_phase3()
 * after task_init() (scheduler + INT 0x81 yield handler live) and before
 * wm_init() (no compositor competing for the CPU yet); BSP-only, no lock held.
 *
 * Design constraints:
 *  - Scratch is a small bounded buffer reused across passes, NOT a 256 MiB
 *    contiguous allocation (would be layout-fragile + blow the boot budget).
 *    Allocation failure is non-fatal: the metric is skipped and its validity
 *    bit stays clear.
 *  - The context-switch benchmark needs the scheduler armed, which only holds
 *    in Phase 3 after task_init() -- hence the placement.
 *  - SIMD throughput reuses the already-correctly-compiled simd_blend_pixels_*
 *    alpha-blend primitives (never new SIMD here -- VEX/EVEX TU split rule);
 *    alpha-blend is compute-bound so it actually separates the ISA tiers.
 *  - Auto-tune writes simd_avx2_ok/simd_avx512_ok with __ATOMIC_RELEASE because
 *    the memops dispatch path reads them from other CPUs after boot.
 */
#include "kernel/types.h"
#include "kernel/hw_profile.h"
#include "kernel/klog.h"
#include "kernel/cpuid.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/memops.h"
#include "kernel/boot_timing.h"
#include "kernel/sched/task.h"
#include "gfx_simd.h"
#include "registry.h"

/* ---- Tunables (bounded to keep the benchmark well under the 2s budget, even
 * under TCG where it re-runs every fresh-disk boot) ---- */
#define HWP_REG_PATH        "SYSTEM\\HwProfile"
#define HWP_SCRATCH_PAGES   512u            /* 2 MiB scratch buffer            */
#define HWP_SCRATCH_BYTES   (HWP_SCRATCH_PAGES * 4096u)
#define HWP_BW_PASSES       8u              /* 8 x 2 MiB = 16 MiB written      */
#define HWP_LAT_ACCESSES    262144u         /* 256K pointer-chase steps        */
#define HWP_LAT_STRIDE      64u             /* one cache line per hop          */
#define HWP_SIMD_PASSES     16u             /* copies per ISA throughput run   */
#define HWP_CTX_ITERS       1000u           /* yield iterations for ctx switch */

static hw_profile_t s_profile;              /* BSP-only; published after init  */
static int          s_profile_ready;        /* set once at end of init         */
static uint64_t     s_tsc_hz;               /* TSC frequency (Hz), set at init  */

/* ctx-switch benchmark coordination (BSP main + one worker, Phase 3 only) */
static volatile int s_ctx_stop;
static volatile int s_ctx_done;             /* worker sets before it returns   */

/* Raw TSC read for benchmark timing. rdtsc_ns() cannot be used here: it returns
 * 0 unless the mono-clock active source is TSC, which is not guaranteed at this
 * Phase-3 point. We measure in raw ticks and convert with the known TSC freq
 * (boot_timing_tsc_freq), independent of mono-clock source selection. lfence
 * serializes prior loads so the delta brackets the measured work. */
static inline uint64_t hwp_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t hwp_delta_ns(uint64_t t0, uint64_t t1)
{
    if (t1 <= t0 || s_tsc_hz == 0)
        return 0;
    uint64_t delta = t1 - t0;
    /* Outlier guard: a bounded micro-benchmark never spans > ~0.5s of TSC; a
     * larger delta means a VM pause / SMI storm -> treat as unmeasured (0) so
     * the caller leaves the validity bit clear rather than persisting garbage. */
    if (delta > s_tsc_hz / 2u)
        return 0;
    /* Divide-first to avoid overflow in delta * 1e9 on long deltas. */
    return (delta / s_tsc_hz) * 1000000000ull
         + (delta % s_tsc_hz) * 1000000000ull / s_tsc_hz;
}

/* ---- low-level CPUID (arch-specific file, raw cpuid is fine here) ---- */
static inline void hwp_cpuid(uint32_t leaf, uint32_t subleaf,
                             uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    uint32_t ra, rb, rc, rd;
    __asm__ volatile("cpuid"
                     : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd)
                     : "a"(leaf), "c"(subleaf));
    if (a) *a = ra;
    if (b) *b = rb;
    if (c) *c = rc;
    if (d) *d = rd;
}

/* ===================== pure helpers (unit-tested) ===================== */

int hw_profile_is_stale(const hw_profile_t *stored,
                        const char *stored_brand, const char *live_brand)
{
    if (!stored || !(stored->flags & HW_PROFILE_VALID))
        return 1;
    if (!stored_brand || !live_brand)
        return 1;
    /* brand strings are fixed 48-char CPUID buffers; bounded compare */
    for (uint32_t i = 0; i < 49; i++) {
        char a = stored_brand[i], b = live_brand[i];
        if (a != b)
            return 1;
        if (a == '\0')
            break;
    }
    return 0;
}

void hw_profile_simd_decision(uint32_t sse2_gpix, uint32_t avx_gpix,
                             uint32_t avx512_gpix,
                             int *keep_avx2, int *keep_avx512)
{
    /* keep an ISA tier only when it beats the next-lower tier by >= the gain
     * threshold. A zero measurement means the ISA was absent/unmeasured ->
     * leave the existing flag untouched (decision "keep" so we do not disable
     * a working path on missing data). */
    if (keep_avx512) {
        if (avx512_gpix == 0 || avx_gpix == 0)
            *keep_avx512 = 1;
        else
            *keep_avx512 =
                ((uint64_t)avx512_gpix * 100u >=
                 (uint64_t)avx_gpix * (100u + HW_PROFILE_SIMD_GAIN_PCT)) ? 1 : 0;
    }
    if (keep_avx2) {
        if (avx_gpix == 0 || sse2_gpix == 0)
            *keep_avx2 = 1;
        else
            *keep_avx2 =
                ((uint64_t)avx_gpix * 100u >=
                 (uint64_t)sse2_gpix * (100u + HW_PROFILE_SIMD_GAIN_PCT)) ? 1 : 0;
    }
}

/* ===================== scratch buffer ===================== */

/* Returns identity-mapped virtual pointer (== physical) or NULL. */
static void *hwp_alloc_scratch(uint32_t pages)
{
    uintptr_t phys = pmm_alloc_contiguous(pages);
    return (phys == 0) ? NULL : (void *)phys;
}

static void hwp_free_scratch(void *p, uint32_t pages)
{
    if (!p)
        return;
    uintptr_t base = (uintptr_t)p;
    for (uint32_t i = 0; i < pages; i++)
        pmm_free_frame(base + (uintptr_t)i * 4096u);
}

/* ===================== cache sizes (CPUID, deterministic) ===================== */

static int hwp_is_amd(void)
{
    uint32_t b, c, d;
    hwp_cpuid(0, 0, NULL, &b, &c, &d);
    /* "AuthenticAMD": EBX=0x68747541, EDX=0x69746E65, ECX=0x444D4163 */
    return (b == 0x68747541u && d == 0x69746E65u && c == 0x444D4163u);
}

/* Parse one deterministic-cache subleaf (Intel leaf 4 / AMD leaf 0x8000001D
 * share the EAX/EBX/ECX layout). Returns 0 when the subleaf type is null. */
static int hwp_cache_subleaf(uint32_t leaf, uint32_t sub,
                             uint32_t *level, uint32_t *type, uint32_t *size_kb)
{
    uint32_t a, b, c, d;
    hwp_cpuid(leaf, sub, &a, &b, &c, &d);
    uint32_t ctype = a & 0x1Fu;          /* 0=null,1=data,2=inst,3=unified */
    if (ctype == 0)
        return 0;
    uint32_t lvl   = (a >> 5) & 0x7u;
    uint32_t ways  = ((b >> 22) & 0x3FFu) + 1u;
    uint32_t parts = ((b >> 12) & 0x3FFu) + 1u;
    uint32_t line  = (b & 0xFFFu) + 1u;
    uint32_t sets  = c + 1u;
    uint64_t bytes = (uint64_t)ways * parts * line * sets;
    if (level) *level = lvl;
    if (type)  *type  = ctype;
    if (size_kb) *size_kb = (uint32_t)(bytes / 1024u);
    return 1;
}

static void hwp_measure_cache(hw_profile_t *p)
{
    uint32_t leaf;
    uint32_t maxext_a;
    if (hwp_is_amd()) {
        hwp_cpuid(0x80000000u, 0, &maxext_a, NULL, NULL, NULL);
        if (maxext_a < 0x8000001Du)
            return;                       /* no deterministic-cache leaf */
        leaf = 0x8000001Du;
    } else {
        leaf = 4u;
    }
    int found = 0;
    for (uint32_t sub = 0; sub < 16u; sub++) {
        uint32_t level, type, size_kb;
        if (!hwp_cache_subleaf(leaf, sub, &level, &type, &size_kb))
            break;
        /* L1 data (or unified), L2, L3 */
        if (level == 1 && (type == 1 || type == 3) && p->l1_size_kb == 0)
            p->l1_size_kb = size_kb;
        else if (level == 2 && p->l2_size_kb == 0)
            p->l2_size_kb = size_kb;
        else if (level == 3 && p->l3_size_kb == 0)
            p->l3_size_kb = size_kb;
        found = 1;
    }
    if (found && (p->l1_size_kb || p->l2_size_kb || p->l3_size_kb))
        p->flags |= HW_PROFILE_CACHE_OK;
}

/* ===================== memory bandwidth ===================== */

static void hwp_memset_best(void *dst, int val, size_t n)
{
    if (__atomic_load_n(&simd_avx512_ok, __ATOMIC_ACQUIRE))
        memset_avx512(dst, val, n);
    else if (__atomic_load_n(&simd_avx2_ok, __ATOMIC_ACQUIRE))
        memset_avx(dst, val, n);
    else
        memset_sse2(dst, val, n);
}

static void hwp_measure_bandwidth(hw_profile_t *p, void *scratch)
{
    uint64_t t0 = hwp_rdtsc();
    for (uint32_t pass = 0; pass < HWP_BW_PASSES; pass++)
        hwp_memset_best(scratch, (int)(0xAB + pass), HWP_SCRATCH_BYTES);
    uint64_t ns = hwp_delta_ns(t0, hwp_rdtsc());
    if (ns == 0)
        return;
    uint64_t bytes = (uint64_t)HWP_SCRATCH_BYTES * HWP_BW_PASSES;
    /* MB/s = bytes / (ns/1e9) / 1e6 = bytes * 1000 / ns  */
    uint64_t mb_s = (bytes * 1000u) / ns;
    p->mem_bandwidth_mb_s = (mb_s > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)mb_s;
    p->flags |= HW_PROFILE_BW_OK;
}

/* ===================== memory latency (pointer chase) ===================== */

static void hwp_measure_latency(hw_profile_t *p, void *scratch)
{
    /* Build a single cycle through the buffer at cache-line stride using a
     * coprime step so every slot is visited before wrapping. Each slot stores
     * the word index of the next slot. */
    uint32_t slots = HWP_SCRATCH_BYTES / HWP_LAT_STRIDE;
    if (slots < 8)
        return;
    uint32_t words_per_slot = HWP_LAT_STRIDE / 4u;
    /* 1031 is prime; if it divides slots, fall back to a step of 1 */
    uint32_t step = (slots % 1031u != 0) ? 1031u : 1u;
    volatile uint32_t *base = (volatile uint32_t *)scratch;
    uint32_t idx = 0;
    for (uint32_t i = 0; i < slots; i++) {
        uint32_t next = (idx + step) % slots;
        base[(size_t)idx * words_per_slot] = next * words_per_slot;
        idx = next;
    }
    uint32_t cur = 0;
    uint64_t t0 = hwp_rdtsc();
    for (uint32_t i = 0; i < HWP_LAT_ACCESSES; i++)
        cur = base[cur];
    uint64_t ns = hwp_delta_ns(t0, hwp_rdtsc());
    /* keep the compiler from eliding the chase */
    __asm__ volatile("" :: "r"(cur));
    if (ns == 0)
        return;
    p->mem_latency_ns = (uint32_t)(ns / HWP_LAT_ACCESSES);
    p->flags |= HW_PROFILE_LAT_OK;
}

/* ===================== SIMD throughput (per ISA) ===================== */

/* Alpha-blend is compute-bound (unpack/multiply/pack per pixel), so it actually
 * separates the ISA tiers -- unlike a memory-bound copy, where SSE2/AVX2/AVX-512
 * all saturate the same bus and look identical. Returns Mpix/s, 0 if
 * unmeasurable. */
static uint32_t hwp_simd_blend_gpix(
        void (*blend)(uint32_t *, const uint32_t *, uint32_t),
        uint32_t *dst, const uint32_t *src, uint32_t count)
{
    uint64_t t0 = hwp_rdtsc();
    for (uint32_t pass = 0; pass < HWP_SIMD_PASSES; pass++)
        blend(dst, src, count);
    uint64_t ns = hwp_delta_ns(t0, hwp_rdtsc());
    if (ns == 0)
        return 0;
    uint64_t pixels = (uint64_t)count * HWP_SIMD_PASSES;
    /* Mpix/s = pixels / (ns/1e9) / 1e6 = pixels * 1000 / ns */
    uint64_t gpix = (pixels * 1000u) / ns;
    return (gpix > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)gpix;
}

static void hwp_measure_simd(hw_profile_t *p, void *scratch)
{
    /* scratch is split: lower half = dst (blended in place), upper half = src */
    uint32_t *dst = (uint32_t *)scratch;
    const uint32_t *src =
        (const uint32_t *)((const uint8_t *)scratch + HWP_SCRATCH_BYTES / 2u);
    uint32_t count = (HWP_SCRATCH_BYTES / 2u) / 4u;   /* pixels per half */
    if (cpu_has(CPU_FEATURE_SSE2))
        p->simd_sse2_gpix_s = hwp_simd_blend_gpix(simd_blend_pixels_sse2,
                                                  dst, src, count);
    /* Gate AVX2 on the runtime dispatch flag, NOT raw CPUID: simd_avx2_ok is
     * set only after cpu_configure_xcr0() enabled XCR0.YMM, and the VEX-encoded
     * simd_blend_pixels_avx2 would #UD if YMM is not enabled. This also means
     * we only ever benchmark (and tune) a tier the dispatch path actually uses. */
    if (__atomic_load_n(&simd_avx2_ok, __ATOMIC_ACQUIRE))
        p->simd_avx_gpix_s = hwp_simd_blend_gpix(simd_blend_pixels_avx2,
                                                 dst, src, count);
    /* No AVX-512 alpha-blend primitive exists yet, so simd_avx512_gpix_s stays
     * 0 and hw_profile_simd_decision leaves AVX-512 alone -- the AVX-512
     * throttle auto-tune is deferred until an AVX-512 blend lands (TODO-09 S14
     * scope gap / S3 AVX-512). */
    if (p->simd_sse2_gpix_s || p->simd_avx_gpix_s)
        p->flags |= HW_PROFILE_SIMD_OK;
}

/* ===================== context switch ===================== */

static void hwp_ctx_worker(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&s_ctx_stop, __ATOMIC_ACQUIRE))
        yield();
    __atomic_store_n(&s_ctx_done, 1, __ATOMIC_RELEASE);
    /* falls off the end -> thread exits */
}

static void hwp_measure_ctx_switch(hw_profile_t *p)
{
    __atomic_store_n(&s_ctx_stop, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_ctx_done, 0, __ATOMIC_RELEASE);
    int tid = kthread_create(hwp_ctx_worker, NULL, 8192);
    if (tid < 0)
        return;                          /* no worker -> skip metric */
    uint64_t t0 = hwp_rdtsc();
    for (uint32_t i = 0; i < HWP_CTX_ITERS; i++)
        yield();
    uint64_t ns = hwp_delta_ns(t0, hwp_rdtsc());
    __atomic_store_n(&s_ctx_stop, 1, __ATOMIC_RELEASE);
    /* Bounded wait for the worker to exit -- NEVER an unbounded thread_join()
     * (that would hang the boot if the worker were somehow never scheduled).
     * The flat-cyclic scheduler picks the worker within a few yields once stop
     * is set; the 4096 cap is the safety bound. */
    int done = 0;
    for (uint32_t i = 0; i < 4096u; i++) {
        if (__atomic_load_n(&s_ctx_done, __ATOMIC_ACQUIRE)) {
            done = 1;
            break;
        }
        yield();
    }
    if (!done) {
        /* leave the worker to exit on its own (stop is set); skip the metric
         * rather than block the boot on a join. One unreaped thread slot is a
         * far smaller cost than an infinite boot hang. */
        klog(LOG_WARN, "hwprofile", "ctx-switch worker did not exit; metric skipped");
        return;
    }
    thread_join((uint32_t)tid);          /* worker already exited -> returns now */
    if (ns == 0)
        return;
    /* ns per yield round (includes the switch out + back) */
    p->ctx_switch_ns = (uint32_t)(ns / HWP_CTX_ITERS);
    p->flags |= HW_PROFILE_CTX_OK;
}

/* ===================== registry persistence ===================== */

static int hwp_load_stored(hw_profile_t *out, char *brand_out, uint32_t brand_cap)
{
    HKEY hk;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, HWP_REG_PATH, 0, KEY_READ, &hk)
        != ERROR_SUCCESS)
        return 0;
    /* Require EVERY field to read back, not just Flags -- a partial persist or
     * an older hive schema must re-benchmark rather than reuse a profile whose
     * validity bits are set while a backing value is missing (a missing SIMD
     * value reads as 0 -> "keep tier" -> silently re-enables a tier the prior
     * boot disabled). Any failed read invalidates the whole stored profile. */
    uint32_t flags = 0;
    int ok = (RegGetDword(hk, "Flags", &flags) == ERROR_SUCCESS) &&
             (RegGetString(hk, "CpuBrand", brand_out, brand_cap) == ERROR_SUCCESS) &&
             (RegGetDword(hk, "MemBandwidthMbS", &out->mem_bandwidth_mb_s) == ERROR_SUCCESS) &&
             (RegGetDword(hk, "MemLatencyNs", &out->mem_latency_ns) == ERROR_SUCCESS) &&
             (RegGetDword(hk, "L1SizeKb", &out->l1_size_kb) == ERROR_SUCCESS) &&
             (RegGetDword(hk, "L2SizeKb", &out->l2_size_kb) == ERROR_SUCCESS) &&
             (RegGetDword(hk, "L3SizeKb", &out->l3_size_kb) == ERROR_SUCCESS) &&
             (RegGetDword(hk, "SimdSse2MpixS", &out->simd_sse2_gpix_s) == ERROR_SUCCESS) &&
             (RegGetDword(hk, "SimdAvxMpixS", &out->simd_avx_gpix_s) == ERROR_SUCCESS) &&
             (RegGetDword(hk, "SimdAvx512MpixS", &out->simd_avx512_gpix_s) == ERROR_SUCCESS) &&
             (RegGetDword(hk, "CtxSwitchNs", &out->ctx_switch_ns) == ERROR_SUCCESS) &&
             (RegGetDword(hk, "TscMhz", &out->tsc_mhz) == ERROR_SUCCESS);
    if (ok)
        out->flags = flags;
    RegCloseKey(hk);
    return ok;
}

static void hwp_persist(const hw_profile_t *p)
{
    HKEY hk;
    uint32_t disp = 0;
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, HWP_REG_PATH, 0, NULL, 0,
                       KEY_ALL_ACCESS, NULL, &hk, &disp) != ERROR_SUCCESS) {
        klog(LOG_WARN, "hwprofile", "registry persist failed (create key)");
        return;
    }
    RegSetString(hk, "CpuBrand", cpuid_get()->brand);
    RegSetDword(hk, "MemBandwidthMbS", p->mem_bandwidth_mb_s);
    RegSetDword(hk, "MemLatencyNs", p->mem_latency_ns);
    RegSetDword(hk, "L1SizeKb", p->l1_size_kb);
    RegSetDword(hk, "L2SizeKb", p->l2_size_kb);
    RegSetDword(hk, "L3SizeKb", p->l3_size_kb);
    RegSetDword(hk, "SimdSse2MpixS", p->simd_sse2_gpix_s);
    RegSetDword(hk, "SimdAvxMpixS", p->simd_avx_gpix_s);
    RegSetDword(hk, "SimdAvx512MpixS", p->simd_avx512_gpix_s);
    RegSetDword(hk, "CtxSwitchNs", p->ctx_switch_ns);
    RegSetDword(hk, "TscMhz", p->tsc_mhz);
    RegSetDword(hk, "Flags", p->flags);
    RegCloseKey(hk);
}

/* ===================== auto-tune ===================== */

static void hwp_autotune_simd(const hw_profile_t *p)
{
    if (!(p->flags & HW_PROFILE_SIMD_OK))
        return;
    int keep_avx2 = 1, keep_avx512 = 1;
    hw_profile_simd_decision(p->simd_sse2_gpix_s, p->simd_avx_gpix_s,
                             p->simd_avx512_gpix_s, &keep_avx2, &keep_avx512);
    /* Only ever DISABLE a tier from the benchmark (never re-enable one the
     * feature probe left off). Release stores: memops dispatch reads these
     * from other CPUs after boot. */
    if (!keep_avx512 && __atomic_load_n(&simd_avx512_ok, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&simd_avx512_ok, 0, __ATOMIC_RELEASE);
        klog(LOG_INFO, "hwprofile",
             "AVX-512 throttle: <%u%% gain over AVX2, dispatch -> AVX2",
             (uint64_t)HW_PROFILE_SIMD_GAIN_PCT);
    }
    if (!keep_avx2 && __atomic_load_n(&simd_avx2_ok, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&simd_avx2_ok, 0, __ATOMIC_RELEASE);
        klog(LOG_INFO, "hwprofile",
             "AVX2 low gain: <%u%% over SSE2, dispatch -> SSE2",
             (uint64_t)HW_PROFILE_SIMD_GAIN_PCT);
    }
}

/* ===================== orchestrator ===================== */

void hw_profile_init(void)
{
    /* 1. Try the persisted profile; reuse it if the hardware is unchanged. */
    hw_profile_t stored;
    char stored_brand[49];
    for (uint32_t i = 0; i < sizeof(stored); i++) ((uint8_t *)&stored)[i] = 0;
    for (uint32_t i = 0; i < sizeof(stored_brand); i++) stored_brand[i] = 0;

    if (hwp_load_stored(&stored, stored_brand, sizeof(stored_brand)) &&
        !hw_profile_is_stale(&stored, stored_brand, cpuid_get()->brand)) {
        s_profile = stored;
        s_profile_ready = 1;
        klog(LOG_INFO, "hwprofile", "reusing stored profile (brand unchanged)");
        hwp_autotune_simd(&s_profile);
        return;
    }

    /* 2. Fresh benchmark. */
    hw_profile_t *p = &s_profile;
    for (uint32_t i = 0; i < sizeof(*p); i++) ((uint8_t *)p)[i] = 0;

    s_tsc_hz = boot_timing_tsc_freq();
    p->tsc_mhz = (uint32_t)(s_tsc_hz / 1000000u);

    hwp_measure_cache(p);                 /* CPUID, no scratch needed */

    void *scratch = hwp_alloc_scratch(HWP_SCRATCH_PAGES);
    if (scratch) {
        hwp_measure_bandwidth(p, scratch);
        hwp_measure_simd(p, scratch);     /* uses both halves of scratch */
        hwp_measure_latency(p, scratch);  /* rewrites scratch as a chase ring */
        hwp_free_scratch(scratch, HWP_SCRATCH_PAGES);
    } else {
        klog(LOG_WARN, "hwprofile",
             "scratch alloc failed (%u pages); bandwidth/latency/SIMD skipped",
             (uint64_t)HWP_SCRATCH_PAGES);
    }

    hwp_measure_ctx_switch(p);

    p->flags |= HW_PROFILE_VALID;
    s_profile_ready = 1;

    klog(LOG_INFO, "hwprofile",
         "bw=%u MB/s lat=%u ns L1=%uK L2=%uK L3=%uK simd(sse2/avx/512)=%u/%u/%u ctx=%u ns tsc=%u MHz",
         (uint64_t)p->mem_bandwidth_mb_s, (uint64_t)p->mem_latency_ns,
         (uint64_t)p->l1_size_kb, (uint64_t)p->l2_size_kb, (uint64_t)p->l3_size_kb,
         (uint64_t)p->simd_sse2_gpix_s, (uint64_t)p->simd_avx_gpix_s,
         (uint64_t)p->simd_avx512_gpix_s,
         (uint64_t)p->ctx_switch_ns, (uint64_t)p->tsc_mhz);

    hwp_persist(p);
    hwp_autotune_simd(p);
}

const hw_profile_t *hw_profile_get(void)
{
    return s_profile_ready ? &s_profile : NULL;
}
