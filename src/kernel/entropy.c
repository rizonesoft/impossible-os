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
