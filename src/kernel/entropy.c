/* ============================================================================
 * entropy.c -- Early entropy source model (inventory + quality + policy)
 *
 * Pure model functions plus the kernel-global source record. Collection is
 * owned by the bootloader and driver sections of the early-entropy TODO;
 * this file owns classification, policy, transcript framing, and the boot
 * diagnostics line. Arch-neutral: no CPUID/MSR access here -- collectors
 * decide quality, the model clamps and classifies.
 * ============================================================================ */

#include "kernel/entropy.h"
#include "kernel/klog.h"
#include "kernel/sched/spinlock.h"
#include "kernel/tpm.h"
#include "kernel/tpm_transport.h"

/* Global source record. Written by collectors on the BSP boot path
 * (single-threaded by construction today); atomics keep readback safe
 * if a future collector (virtio-rng runtime reseed) records from another
 * context. No lock: each field is independently atomic and readers
 * tolerate seeing mask without the matching quality for one update. */
static uint32_t s_entropy_mask;
static uint32_t s_entropy_quality;

entropy_class_t entropy_classify(uint32_t mask, uint32_t quality)
{
    uint32_t high = 0;
    uint32_t src;

    for (src = 0; src < ENTROPY_SRC_COUNT; src++) {
        if (!(mask & ENTROPY_SRC_BIT(src)))
            continue;
        /* Timing sources are personalization only -- enforce the cap HERE
         * too, not just in record_source: descriptor-carried quality bits
         * are advisory and may claim HIGH for deterministic VM timing. */
        if (src == ENTROPY_SRC_JITTER || src == ENTROPY_SRC_TIME)
            continue;
        if (entropy_quality_get(quality, (entropy_src_t)src) == ENTROPY_Q_HIGH)
            high++;
    }

    if (high >= 2)
        return ENTROPY_CLASS_GOOD;
    if (high == 1)
        return ENTROPY_CLASS_MINIMUM;
    return ENTROPY_CLASS_DEGRADED;
}

int entropy_policy_ok(entropy_class_t cls, int release_mode)
{
    if (release_mode)
        return cls >= ENTROPY_CLASS_MINIMUM;
    return 1;
}

uint32_t entropy_frame_source(uint8_t *buf, uint32_t cap, uint32_t pos,
                              entropy_src_t src, const uint8_t *data,
                              uint32_t len)
{
    uint32_t need;
    uint32_t i;

    if (!buf || !data || len == 0)
        return 0;
    if ((uint32_t)src >= ENTROPY_SRC_COUNT)
        return 0;

    if (len > 0xFFFFFFFFu - 5u)
        return 0;   /* need computation would wrap */
    need = 1u + 4u + len;
    if (pos > cap || need > cap - pos)
        return 0;   /* would not fit -- hard failure, never truncate */

    buf[pos++] = (uint8_t)src;
    buf[pos++] = (uint8_t)(len & 0xFF);
    buf[pos++] = (uint8_t)((len >> 8) & 0xFF);
    buf[pos++] = (uint8_t)((len >> 16) & 0xFF);
    buf[pos++] = (uint8_t)((len >> 24) & 0xFF);
    for (i = 0; i < len; i++)
        buf[pos++] = data[i];

    return pos;
}

void entropy_record_source(entropy_src_t src, entropy_quality_t q)
{
    uint32_t packed;

    if ((uint32_t)src >= ENTROPY_SRC_COUNT)
        return;

    /* Reserved quality value (3) fails conservative: an out-of-vocabulary
     * collector value must never grant full credit. */
    if (q > ENTROPY_Q_HIGH)
        q = ENTROPY_Q_NONE;
    /* Conservative clamp: timing-derived sources are personalization
     * only, never credited HIGH (deterministic VM timing, coarse clocks). */
    if ((src == ENTROPY_SRC_JITTER || src == ENTROPY_SRC_TIME) &&
        q > ENTROPY_Q_LOW)
        q = ENTROPY_Q_LOW;

    /* CAS loop: two collectors recording DIFFERENT sources concurrently
     * must not drop each other's 2-bit slot (load/modify/store would). */
    packed = __atomic_load_n(&s_entropy_quality, __ATOMIC_RELAXED);
    for (;;) {
        uint32_t updated = entropy_quality_set(packed, src, q);
        if (__atomic_compare_exchange_n(&s_entropy_quality, &packed, updated,
                                        0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            break;
        /* packed reloaded by the failed CAS; retry with the fresh value */
    }

    /* Q_NONE retracts the source (clears its mask bit) so a collector can
     * withdraw a contribution it later rejects. */
    if (q != ENTROPY_Q_NONE)
        __atomic_fetch_or(&s_entropy_mask, ENTROPY_SRC_BIT(src),
                          __ATOMIC_RELAXED);
    else
        __atomic_fetch_and(&s_entropy_mask, ~ENTROPY_SRC_BIT(src),
                           __ATOMIC_RELAXED);
}

uint32_t entropy_source_mask(void)
{
    return __atomic_load_n(&s_entropy_mask, __ATOMIC_RELAXED);
}

uint32_t entropy_source_quality(void)
{
    return __atomic_load_n(&s_entropy_quality, __ATOMIC_RELAXED);
}

/* ---- Staged transcript (kernel-side collectors -> CSPRNG seeding) ---- */
static uint8_t  s_stage_buf[ENTROPY_STAGE_CAP];
static uint32_t s_stage_pos;       /* guarded by s_stage_lock */
static uint32_t s_stage_overflow;  /* sticky; guarded by s_stage_lock */
static spinlock_t s_stage_lock = SPINLOCK_INIT;

int entropy_stage_source(entropy_src_t src, const uint8_t *data,
                         uint32_t len, entropy_quality_t q)
{
    uint64_t irqf;
    uint32_t np;

    spin_lock_irqsave(&s_stage_lock, &irqf);
    np = entropy_frame_source(s_stage_buf, ENTROPY_STAGE_CAP, s_stage_pos,
                              src, data, len);
    if (np == 0) {
        s_stage_overflow = 1;
        spin_unlock_irqrestore(&s_stage_lock, irqf);
        klog(LOG_WARN, "entropy",
             "staged transcript refused src %u record (%u bytes)",
             (uint64_t)src, (uint64_t)len);
        return 0;
    }
    s_stage_pos = np;
    /* Credit under the lock: a drain that observes these bytes must
     * also observe the source's mask/quality accounting. */
    entropy_record_source(src, q);
    spin_unlock_irqrestore(&s_stage_lock, irqf);

    return 1;
}

uint32_t entropy_staged_drain(uint8_t *out, uint32_t cap)
{
    uint64_t irqf;
    uint32_t i, n;

    if (!out)
        return 0;

    spin_lock_irqsave(&s_stage_lock, &irqf);
    n = s_stage_pos;
    if (n > cap) {
        /* All-or-nothing: a partial drain would split a record across
         * two consumers. Leave the transcript intact for a retry with
         * a big enough buffer. */
        spin_unlock_irqrestore(&s_stage_lock, irqf);
        return 0;
    }
    for (i = 0; i < n; i++) {
        out[i] = s_stage_buf[i];
        s_stage_buf[i] = 0;
    }
    s_stage_pos = 0;
    spin_unlock_irqrestore(&s_stage_lock, irqf);
    return n;
}

void entropy_staged_consume_zero(void)
{
    uint64_t irqf;
    uint32_t i;
    spin_lock_irqsave(&s_stage_lock, &irqf);
    for (i = 0; i < ENTROPY_STAGE_CAP; i++)
        s_stage_buf[i] = 0;
    s_stage_pos = 0;
    spin_unlock_irqrestore(&s_stage_lock, irqf);
}

int entropy_staged_overflowed(void)
{
    return __atomic_load_n(&s_stage_overflow, __ATOMIC_RELAXED) != 0;
}

/* ---- TPM RNG collector (boot Phase 1, after tpm_transport_init) ---- */

/* HIGH-credit byte floor: the quality model treats one HIGH hardware
 * source as policy-satisfying (MINIMUM), so a TPM contributing less
 * than a full digest of output must not earn that credit. */
#define ENTROPY_TPM_BYTES        64u
#define ENTROPY_TPM_FLOOR        32u
#define ENTROPY_TPM_BUDGET_MS    2000u

void entropy_collect_tpm(void)
{
    uint8_t buf[ENTROPY_TPM_BYTES];
    uint32_t i, n;
    int got;
    int stuck;

    if (!tpm_transport_available())
        return;  /* absent/wedged TPM already logged by the transport */

    got = tpm2_get_random_bounded(buf, ENTROPY_TPM_BYTES,
                                  ENTROPY_TPM_BUDGET_MS);
    if (got < (int)ENTROPY_TPM_FLOOR) {
        klog(LOG_WARN, "entropy",
             "tpm: GetRandom yielded %d bytes (floor %u); not credited",
             (int64_t)got, (uint64_t)ENTROPY_TPM_FLOOR);
        goto wipe;
    }
    n = (uint32_t)got;

    /* Stuck-output heuristics (mirrors the bootloader CPU collector):
     * all-identical bytes (covers all-zero and 0xFF fill), and for a
     * multi-chunk sample, identical halves (a TPM replaying the same
     * digest every call). */
    stuck = 1;
    for (i = 1; i < n; i++) {
        if (buf[i] != buf[0]) {
            stuck = 0;
            break;
        }
    }
    if (!stuck && (n & 1u) == 0u && n >= 2u * ENTROPY_TPM_FLOOR) {
        uint32_t half = n / 2u;
        stuck = 1;
        for (i = 0; i < half; i++) {
            if (buf[i] != buf[half + i]) {
                stuck = 0;
                break;
            }
        }
    }
    if (stuck) {
        klog(LOG_WARN, "entropy",
             "tpm: GetRandom output failed stuck-RNG heuristic; rejected");
        goto wipe;
    }

    if (entropy_stage_source(ENTROPY_SRC_TPM_RNG, buf, n,
                             ENTROPY_Q_HIGH)) {
        klog(LOG_INFO, "entropy", "RNG: TPM2_GetRandom %u bytes",
             (uint64_t)n);
        tpm_integrity_set_rng_available(1);
    }

wipe:
    for (i = 0; i < (uint32_t)sizeof(buf); i++)
        buf[i] = 0;
}

/* Short quality token for the diagnostics line. */
static const char *entropy_q_str(entropy_quality_t q)
{
    switch (q) {
    case ENTROPY_Q_LOW:  return "low";
    case ENTROPY_Q_HIGH: return "ok";
    default:             return "none";
    }
}

void entropy_report(void)
{
    uint32_t mask = entropy_source_mask();
    uint32_t quality = entropy_source_quality();
    entropy_class_t cls = entropy_classify(mask, quality);
    static const char *cls_str[] = { "degraded", "minimum", "good" };

    klog(cls == ENTROPY_CLASS_DEGRADED ? LOG_WARN : LOG_INFO, "entropy",
         "fw=%s cpu=%s tpm=%s oem0=%s seed=%s hwrng=%s jitter=%s time=%s (class=%s)",
         entropy_q_str(entropy_quality_get(quality, ENTROPY_SRC_FW_RNG)),
         entropy_q_str(entropy_quality_get(quality, ENTROPY_SRC_CPU_RNG)),
         entropy_q_str(entropy_quality_get(quality, ENTROPY_SRC_TPM_RNG)),
         entropy_q_str(entropy_quality_get(quality, ENTROPY_SRC_ACPI_OEM0)),
         entropy_q_str(entropy_quality_get(quality, ENTROPY_SRC_SEED_FILE)),
         entropy_q_str(entropy_quality_get(quality, ENTROPY_SRC_GUEST_HWRNG)),
         entropy_q_str(entropy_quality_get(quality, ENTROPY_SRC_JITTER)),
         entropy_q_str(entropy_quality_get(quality, ENTROPY_SRC_TIME)),
         cls_str[cls]);
}
