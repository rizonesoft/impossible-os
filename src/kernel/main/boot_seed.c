/* ============================================================================
 * boot_seed.c -- boot_info random-seed payload consumer (Phase 1)
 *
 * Walks every validated BOOT_PAYLOAD_RANDOM_SEED descriptor, parses the
 * in-payload header + framed transcript (entropy_seed_parse, pure), routes
 * seed-file carryover records through the NVRAM-token verifier
 * (seed_file_early_verify), records accepted sources in the entropy model,
 * and hands the combined validated transcript to the caller for the FIRST
 * CSPRNG seed (csprng_init folds it into the initial key -- never a
 * post-init reseed).
 *
 * Memory ownership: each consumed payload is wiped in place; the frames go
 * back to the PMM ONLY when the descriptor certifies exclusive page
 * ownership (4 KiB-aligned phys_start AND alignment == 4096 -- the shape
 * our bootloader's AllocatePages publication produces). A descriptor from
 * another producer that does not certify page ownership is wiped but its
 * frames stay reserved: a sub-page buffer inside a shared allocation must
 * never be freed by frame. The descriptor's FLAG_VALID is cleared in the
 * kernel copy after consumption so later scans cannot mistake the zeroed
 * range for live seed material.
 *
 * Earlier-stage concatenation (Linux EFI config-table parity): EVERY
 * RANDOM_SEED descriptor is consumed and mixed, not just the first -- a
 * chain stage that published its own seed payload contributes alongside
 * ours instead of being overwritten.
 *
 * BSP boot path only: runs once in Phase 1 before csprng_init(); no SMP
 * concerns by construction (APs are not started yet).
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/boot_init.h"
#include "kernel/csprng.h"
#include "kernel/entropy.h"
#include "kernel/seed_file.h"
#include "kernel/klog.h"
#include "kernel/mm/pmm.h"
#include "libs/monocypher/monocypher.h"

/* entropy_seed_verify_fn shape over the seed_file module. */
static int boot_seed_verify_carryover(const uint8_t *blob, uint32_t len,
                                      uint64_t *counter_out,
                                      uint8_t payload_out[32])
{
    return seed_file_early_verify(blob, len, counter_out, payload_out);
}

/* Record every source present in the accepted records, quality taken from
 * the ADVISORY header slot but re-clamped by entropy_record_source (JITTER/
 * TIME never above LOW, reserved value -> NONE). The seed-file class is
 * recorded LOW here regardless of the header: HIGH credit belongs to the
 * Phase-3 lifecycle's NVRAM commit point, not the early read. */
void boot_seed_record_sources(const struct entropy_seed_parse_result *r)
{
    uint32_t src;

    for (src = 0; src < (uint32_t)ENTROPY_SRC_COUNT; src++) {
        entropy_quality_t q;

        if ((r->records_mask & ENTROPY_SRC_BIT(src)) == 0u)
            continue;
        if (src == (uint32_t)ENTROPY_SRC_SEED_FILE)
            q = ENTROPY_Q_LOW;
        else
            q = entropy_quality_get(r->hdr_quality, (entropy_src_t)src);
        entropy_record_source((entropy_src_t)src, q);
    }
}

boot_seed_desc_class_t boot_seed_desc_classify(uint64_t caps_present,
                                               uint32_t flags,
                                               uint64_t phys_start,
                                               uint64_t length)
{
    /* CAPABILITY gate before everything, including FLAG_RESERVED. The
     * PMM reservation pass in boot_reserved.c is itself gated on
     * BOOT_CAP_PAYLOAD_DESCRIPTORS, so when that bit is clear nothing
     * ever acted on FLAG_RESERVED and the flag certifies nothing --
     * trusting it would let a producer with caps_present clear hand us
     * a range the reservation pass skipped and the allocator now owns.
     * Same order, and the same reason, as boot_headless_authz_classify. */
    /* caps_present is taken at the field's own uint64_t width, per the
     * convention boot_info.h states for the whole BOOT_CAP_ family
     * ("consumers always widen to uint64_t at use time"). A uint32_t
     * parameter would silently truncate the word and make any future
     * bit >= 32 permanently unobservable. */
    if ((caps_present & (uint64_t)BOOT_CAP_PAYLOAD_DESCRIPTORS) == 0u)
        return BOOT_SEED_DESC_NO_CAPABILITY;
    /* RESERVED gate next: a descriptor without FLAG_RESERVED was never
     * pinned by the PMM reservation pass, so by Phase 1 its frames may
     * already be allocator-owned. Touching them (even to wipe) would
     * corrupt the new owner -- retire untouched. */
    if ((flags & BOOT_PAYLOAD_FLAG_RESERVED) == 0u)
        return BOOT_SEED_DESC_NOT_RESERVED;
    /* Identity-map bound: phys_start is dereferenced through the boot
     * identity map, which covers the first 4 GiB only. An out-of-map
     * payload cannot even be wiped. */
    if (phys_start >= BOOT_INFO_EARLY_MAP_END ||
        length > BOOT_INFO_EARLY_MAP_END - phys_start)
        return BOOT_SEED_DESC_OUT_OF_MAP;
    if (!boot_seed_length_reservable(length))
        return BOOT_SEED_DESC_BAD_LENGTH;
    return BOOT_SEED_DESC_CONSUMABLE;
}

int boot_seed_length_reservable(uint64_t length)
{
    /* The SAME predicate the Phase-0 reservation pass applies before it
     * will pin a RANDOM_SEED descriptor (src/kernel/mm/boot_reserved.c).
     * Keeping it in one function is the point: if the pass and the
     * consumer ever disagreed, one of them would be touching memory the
     * other never reserved. */
    return length >= sizeof(struct entropy_seed_header) &&
           length <= BOOT_SEED_PAYLOAD_CAP;
}

uint64_t boot_seed_desc_wipe_len(boot_seed_desc_class_t cls, uint64_t length)
{
    /* CONSUMABLE has already passed the length contract, so its own
     * length is the honest bound, and it is the ONLY class the
     * reservation pass actually pinned. Everything else is retired
     * untouched: an unreserved, un-negotiated or bad-length range was
     * never pinned and may already belong to another owner by Phase 1,
     * and an out-of-map range is not addressable at all. Wiping any of
     * them would corrupt the new owner to scrub bytes we never owned. */
    switch (cls) {
    case BOOT_SEED_DESC_CONSUMABLE:
        return length;
    case BOOT_SEED_DESC_BAD_LENGTH:
    case BOOT_SEED_DESC_NOT_RESERVED:
    case BOOT_SEED_DESC_OUT_OF_MAP:
    case BOOT_SEED_DESC_NO_CAPABILITY:
    default:
        return 0ull;
    }
}

int boot_seed_desc_may_free(boot_seed_desc_class_t cls)
{
    /* CONSUMABLE only. BAD_LENGTH must never reach the frame loop: that
     * loop derives its end from the descriptor's own length, which this
     * class has just declared wrong, so an overlong descriptor would
     * return a long unwiped suffix to the allocator. Wiping a clamped
     * prefix does not license freeing an unclamped range. */
    return cls == BOOT_SEED_DESC_CONSUMABLE;
}

int boot_seed_release_payload(uint8_t *payload, uint64_t length,
                              uint64_t phys_start, uint64_t alignment,
                              uint32_t producer_id)
{
    if (!payload || length == 0)
        return 0;
    crypto_wipe(payload, (size_t)length);
    /* Frame release needs an OWNERSHIP contract, not just shape:
     * alignment is the generic natural-alignment field, and a foreign
     * producer's page-aligned descriptor can truthfully certify 4096
     * while pointing into a shared allocation. Only OUR bootloader's
     * publish contract (producer_id == BOOT_PRODUCER_UEFI plus the
     * AllocatePages-exclusive page shape) makes the frames safe to
     * return; everything else is wipe-only. */
    return (producer_id == (uint32_t)BOOT_PRODUCER_UEFI) &&
           ((phys_start & 0xFFFull) == 0u) && (alignment == 4096ull);
}

/* Per-payload parse scratch, sized to the descriptor length contract so
 * a contract-valid payload can NEVER hit NO_FIT (re-framed output is
 * always <= input length). Static, not stack: 16 KiB would smash the
 * Phase 1 stack. BSP-only by construction (Phase 1 runs before APs);
 * wiped after every use. */
static uint8_t s_payload_tx[BOOT_SEED_PAYLOAD_CAP];

uint32_t boot_seed_consume(uint8_t *out, uint32_t cap)
{
    crypto_blake2b_ctx ctx;
    uint32_t idx = 0;
    uint64_t total = 0;
    uint32_t consumed = 0;
    uint32_t rejected = 0;
    int caps_refused = 0;

    POST16(POST16_BOOT_SEED);

    if (!out || cap < BOOT_SEED_DIGEST_LEN) {
        POST16(POST16_BOOT_SEED_OK);
        return 0;
    }

    /* Digest-chain the payload transcripts instead of concatenating
     * them into one buffer: 32 descriptors at the 16 KiB cap is half a
     * megabyte, far past any sane static buffer, and dropping a payload
     * for buffer space would silently break the every-descriptor-mixed
     * contract. Blake2b-256 over the concatenated transcripts preserves
     * the entropy up to the key size the CSPRNG derives anyway. */
    crypto_blake2b_init(&ctx, BOOT_SEED_DIGEST_LEN);

    /* Always look up occurrence 0: retiring a descriptor clears its
     * FLAG_VALID and boot_payload_find() skips retired entries, so the
     * next live descriptor pops into slot 0. Bounded by the table size
     * as a belt against a find/retire semantics regression. */
    for (idx = 0; idx < (uint32_t)BOOT_PAYLOAD_MAX; idx++) {
        const struct boot_payload_desc *d =
            boot_payload_find(&g_boot_info,
                              (uint32_t)BOOT_PAYLOAD_RANDOM_SEED, 0);
        struct entropy_seed_parse_result res;
        entropy_seed_status_t st = ENTROPY_SEED_BAD_ARGS;
        int abandon_walk = 0;
        uint8_t *payload = (uint8_t *)0;
        uint32_t out_len = 0;
        int owns_pages = 0;
        int parsed = 0;
        boot_seed_desc_class_t cls;

        if (!d)
            break;

        /* boot_payload_validate proved the range is disjoint from every
         * retained region and does not wrap; the classifier layers the
         * seed contract on top: PMM reservation (an unreserved range may
         * already be allocator-owned -- untouchable), the boot identity
         * map bound (an out-of-map range cannot even be wiped), and the
         * type-specific length contract. */
        cls = boot_seed_desc_classify(g_boot_info.caps_present, d->flags,
                                      d->phys_start, d->length);

        /* EXHAUSTIVE switch with NO `default`, deliberately. Under the
         * kernel's -Wall -Wextra -Werror set (Makefile:21) -Wswitch turns
         * a missing enumerator into a BUILD FAILURE, so the capability
         * refusal cannot be deleted or a new class silently added and
         * still compile. That matters more than a runtime test here: the
         * previous if/else-if chain ended in a permissive `else` that ran
         * the parse path, so removing the capability arm would have made
         * an un-negotiated payload parse, wipe and free -- while every
         * classifier assertion stayed green. A test cannot catch that,
         * because a test may not drive this function at all: it calls
         * pmm_free_frame() on live frames and mutates g_boot_info
         * (docs/infrastructure/test-policy.md). */
        switch (cls) {
        case BOOT_SEED_DESC_NO_CAPABILITY:
            /* Global refusal, so abandon the WALK rather than this entry.
             * Nothing is read, wiped, freed, or retired: without the
             * negotiated capability we have no basis to believe any of
             * these ranges is ours, and clearing FLAG_VALID would be
             * mutating a handoff we just declined to trust. Abandoning is
             * also required for termination -- the loop re-queries
             * occurrence 0 every pass and only retiring advances it, so
             * continuing would spin on this same descriptor. */
            klog(LOG_WARN, "entropy",
                 "seed payloads refused: BOOT_CAP_PAYLOAD_DESCRIPTORS "
                 "not negotiated (caps=0x%lx); every seed descriptor left "
                 "untouched of %u total payload descriptor(s), boot "
                 "continues without seed entropy",
                 (uint64_t)g_boot_info.caps_present,
                 (uint64_t)g_boot_info.payload_count);
            caps_refused = 1;
            abandon_walk = 1;
            break;
        case BOOT_SEED_DESC_NOT_RESERVED:
            klog(LOG_WARN, "entropy",
                 "seed payload #%u rejected: not PMM-reserved "
                 "(retired untouched)", (uint64_t)idx);
            st = ENTROPY_SEED_BAD_ARGS;
            break;
        case BOOT_SEED_DESC_OUT_OF_MAP:
            klog(LOG_WARN, "entropy",
                 "seed payload #%u rejected: 0x%lx for %lu bytes is "
                 "outside the boot identity map (retired untouched)",
                 (uint64_t)idx, (uint64_t)d->phys_start,
                 (uint64_t)d->length);
            st = ENTROPY_SEED_BAD_ARGS;
            break;
        case BOOT_SEED_DESC_BAD_LENGTH:
            klog(LOG_WARN, "entropy",
                 "seed payload #%u rejected: length %lu outside contract "
                 "(never PMM-pinned, so retired untouched)",
                 (uint64_t)idx, (uint64_t)d->length);
            st = ENTROPY_SEED_BAD_LENGTH;
            break;
        case BOOT_SEED_DESC_CONSUMABLE:
            /* Identity-mapped low memory -- the same access contract
             * every other payload consumer (boot.conf modules,
             * warm-update state) relies on. */
            payload = (uint8_t *)(uintptr_t)d->phys_start;
            parsed = 1;
            st = entropy_seed_parse(payload, d->length,
                                    (d->flags &
                                     BOOT_PAYLOAD_FLAG_CHECKSUMMED) ? 1 : 0,
                                    d->checksum,
                                    boot_seed_verify_carryover,
                                    s_payload_tx,
                                    (uint32_t)sizeof(s_payload_tx),
                                    &out_len, &res);
            break;
        }

        if (abandon_walk)
            break;

        /* Single retire path for EVERY discovered descriptor: wipe +
         * ownership decision (accepted, parse-rejected, and bad-length
         * payloads alike -- rejected bytes are still one-time seed
         * material from someone's RNG and must not linger), then clear
         * FLAG_VALID in the kernel copy so boot_payload_find() never
         * rediscovers the zeroed/recycled range. The bootloader's
         * original at 0x10000 is untouched. */
        if (payload) {
            /* Both bounds come from the pure disposition helpers, never
             * from d->length directly: BAD_LENGTH wipes a clamped
             * prefix and is refused the frame loop entirely, because
             * that loop's end address would otherwise be derived from
             * the very field the classifier rejected. */
            owns_pages = boot_seed_release_payload(
                             payload,
                             boot_seed_desc_wipe_len(cls, d->length),
                             d->phys_start, d->alignment, d->producer_id)
                         && boot_seed_desc_may_free(cls);
            if (owns_pages) {
                uint64_t page;
                uint64_t end = (d->phys_start + d->length + 0xFFFull) &
                               ~0xFFFull;
                for (page = d->phys_start; page < end; page += 4096ull)
                    pmm_free_frame((uintptr_t)page);
            }
        }
        {
            struct boot_payload_desc *mut =
                (struct boot_payload_desc *)(uintptr_t)d;
            mut->flags &= ~(uint32_t)BOOT_PAYLOAD_FLAG_VALID;
        }

        if (st != ENTROPY_SEED_OK) {
            if (parsed) {
                crypto_wipe(s_payload_tx, sizeof(s_payload_tx));
                klog(LOG_WARN, "entropy",
                     "seed payload #%u rejected: parse status=%u "
                     "(wiped, %s)",
                     (uint64_t)idx, (uint64_t)st,
                     owns_pages ? "pages freed" : "pages kept reserved");
            }
            rejected++;
            continue;
        }

        boot_seed_record_sources(&res);
        /* Mark carryover consumed ONLY now -- the whole payload was
         * accepted into the transcript. A verified src-4 record inside
         * a payload that failed later (BAD_RECORD / NO_FIT) never
         * reached the CSPRNG; marking it would make seed_file_phase3()
         * skip its only absorption. */
        if (res.seed_file_ok > 0u)
            seed_file_early_mark_consumed(res.seed_file_counter);
        if (out_len) {
            crypto_blake2b_update(&ctx, s_payload_tx, out_len);
            crypto_wipe(s_payload_tx, sizeof(s_payload_tx));
        }
        total += out_len;
        consumed++;

        klog(LOG_INFO, "entropy",
             "seed payload %u bytes (mask=0x%x, quality=0x%x): "
             "%u records, carryover %u ok / %u rejected",
             (uint64_t)d->length, (uint64_t)res.hdr_mask,
             (uint64_t)res.hdr_quality, (uint64_t)res.record_count,
             (uint64_t)res.seed_file_ok, (uint64_t)res.seed_file_rejected);
    }

    /* Only claim "none published" when the lookup genuinely found
     * nothing. After a capability refusal descriptors WERE published and
     * were deliberately left alone, so saying otherwise contradicts the
     * refusal line emitted moments earlier -- on exactly the degraded
     * boot that line exists to explain. */
    if (consumed == 0 && rejected == 0 && !caps_refused)
        klog(LOG_INFO, "entropy",
             "seed payload: none published -- first seed uses local + "
             "staged sources only");

    POST16(POST16_BOOT_SEED_OK);

    /* No accepted transcript bytes: nothing to hand to the first seed.
     * (An accepted header-only payload contributes no bytes either.) */
    if (total == 0) {
        crypto_wipe(&ctx, sizeof(ctx));
        return 0;
    }
    crypto_blake2b_final(&ctx, out);
    klog(LOG_INFO, "entropy",
         "seed payload digest: %lu transcript bytes over %u payload(s)",
         (uint64_t)total, (uint64_t)consumed);
    return BOOT_SEED_DIGEST_LEN;
}

void early_entropy_init(void)
{
    /* The SINGLE named early-entropy init point, BEFORE every randomness
     * consumer (AT_RANDOM in task_exec, AP canaries, KUSD cookie, GUID
     * generation, future KASLR): consume the boot_info seed payloads,
     * fold the digest into the FIRST CSPRNG key, report the credited
     * class + crypto-gate verdict. BSP boot path only (Phase 1). */
    uint8_t digest[BOOT_SEED_DIGEST_LEN];
    uint32_t len;
    entropy_class_t cls;

    len = boot_seed_consume(digest, (uint32_t)sizeof(digest));
    csprng_init(len ? digest : (const uint8_t *)0, len);
    crypto_wipe(digest, sizeof(digest));

    cls = csprng_credited_class();
    klog(LOG_INFO, "entropy",
         "early entropy init complete: credited class=%s, "
         "release crypto %s",
         entropy_class_str(cls),
         csprng_crypto_ok() ? "permitted" : "REFUSED until reseed");

    /* Visible degraded indication on a still-live surface: the splash
     * diag line persists from here until the desktop takes over (VPD
     * tier 1 is already stopped by this point, and the Phase-3 surfaces
     * -- registry mirror + entropy.json -- carry the durable record).
     * The credited class is FINAL here: the Phase-3 seed-file carryover
     * absorbs at Q_LOW and can never upgrade it. */
    if (cls == ENTROPY_CLASS_DEGRADED) {
        extern void boot_splash_diag(const char *msg);
        boot_splash_diag("WARNING: degraded randomness -- no hardware "
                         "entropy source credited");
    }
}
