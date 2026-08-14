/* ============================================================================
 * test_smp_lifecycle.c -- live CPU online membership + async worker claim
 *                         (bare-metal hardening: live CPU online lifecycle)
 *
 * Two surfaces, both exercised over CALLER-SUPPLIED words so the state machine
 * is provable without a live SMP system and without mutating the running
 * kernel's own membership:
 *
 *   - the async lifecycle claim (SMP_ASYNC_* encoding, dispatch / complete /
 *     park / publish-idle transitions), and
 *   - the online-mask helpers the live count is derived from.
 *
 * smp_publish_cpu_online() / smp_retract_cpu_online() are deliberately NOT
 * called here: they take a per_cpu_data pointer but mutate the KERNEL's global
 * online mask, so a test driving them would retire a live CPU from the running
 * system. The transitions they are built from are what these tests cover; the
 * wiring itself is carried by the boot-path smoke matrix.
 *
 * The query side (smp_cpu_count / smp_cpu_present_count / smp_online_mask) is
 * read ONLY -- state the boot path already published, same discipline as
 * test_cpu_seq.c.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/smp.h"
#include "kernel/acpi.h"
#include "kernel/boot_init.h"   /* pure S27 fallback decision, no boot state */

extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* ---- Claim encoding ---- */

/* A zero word is what a per_cpu_data slot holds before anything runs. It MUST
 * read OFFLINE, or a CPU that never came online is dispatchable by default. */
static void test_claim_zero_word_is_offline(void)
{
    uint32_t claim = 0;
    uint32_t gen = 0xFFFFFFFFu;

    TEST_ASSERT_EQ(SMP_ASYNC_STATE_OF(claim), SMP_ASYNC_OFFLINE,
        "a zero claim word must decode as OFFLINE");
    TEST_ASSERT(!smp_async_claim_dispatch(&claim, &gen),
        "an OFFLINE slot must refuse dispatch");
    TEST_ASSERT_EQ(claim, 0u,
        "a refused dispatch must not mutate the claim word");
}

/* State and generation must survive a round trip through the encoding for
 * every generation the field admits, including both boundaries. */
static void test_claim_encoding_round_trips(void)
{
    static const uint32_t gens[] = { 0u, 1u, 2u, 1000u, SMP_ASYNC_GEN_MAX - 1u,
                                     SMP_ASYNC_GEN_MAX };
    static const uint32_t states[] = { SMP_ASYNC_OFFLINE, SMP_ASYNC_IDLE,
                                       SMP_ASYNC_BUSY, SMP_ASYNC_RESERVED };
    char msg[128];
    uint32_t gi, si;

    for (gi = 0; gi < sizeof(gens) / sizeof(gens[0]); gi++) {
        for (si = 0; si < sizeof(states) / sizeof(states[0]); si++) {
            uint32_t w = SMP_ASYNC_CLAIM(states[si], gens[gi]);
            snprintf(msg, sizeof(msg),
                     "claim(state=%u, gen=%u) decoded state=%u gen=%u",
                     (unsigned)states[si], (unsigned)gens[gi],
                     (unsigned)SMP_ASYNC_STATE_OF(w),
                     (unsigned)SMP_ASYNC_GEN_OF(w));
            TEST_ASSERT(SMP_ASYNC_STATE_OF(w) == states[si] &&
                        SMP_ASYNC_GEN_OF(w) == gens[gi], msg);
        }
    }
}

/* ---- Claim transitions ---- */

/* The ordinary cycle: an idle slot is claimed once, the generation advances,
 * and the worker hands it back. */
static void test_claim_dispatch_then_complete(void)
{
    uint32_t claim = SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 7);
    uint32_t gen = 0;

    TEST_ASSERT(smp_async_claim_dispatch(&claim, &gen),
        "an IDLE slot must accept dispatch");
    TEST_ASSERT_EQ(gen, 8u, "dispatch must advance the generation by one");
    TEST_ASSERT_EQ(claim, SMP_ASYNC_CLAIM(SMP_ASYNC_RESERVED, 8),
        "phase 1 must leave the slot RESERVED, not runnable");
    TEST_ASSERT(!smp_async_claim_is_busy(&claim, (uint32_t *)0),
        "a RESERVED slot must not read as carrying work");

    TEST_ASSERT(smp_async_claim_arm(&claim, gen), "phase 2 arms the slot");
    TEST_ASSERT_EQ(claim, SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 8),
        "an armed slot must read BUSY at the dispatched generation");

    TEST_ASSERT(smp_async_claim_complete(&claim, gen),
        "the dispatched generation must be able to complete");
    TEST_ASSERT_EQ(claim, SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 8),
        "completion returns the slot to IDLE at the same generation");
}

/* The core of the section: a worker the BSP timed out is still running, so its
 * slot must NOT be handed to a later group. */
static void test_claim_busy_slot_refuses_second_dispatch(void)
{
    uint32_t claim = SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 0);
    uint32_t gen = 0, gen2 = 0xFFFFFFFFu;

    TEST_ASSERT(smp_async_claim_dispatch(&claim, &gen), "first dispatch");
    TEST_ASSERT(!smp_async_claim_dispatch(&claim, &gen2),
        "a RESERVED slot must refuse a second dispatch");
    TEST_ASSERT_EQ(claim, SMP_ASYNC_CLAIM(SMP_ASYNC_RESERVED, 1),
        "the refused dispatch must leave the reservation byte-identical");

    TEST_ASSERT(smp_async_claim_arm(&claim, gen), "arm the first dispatch");
    TEST_ASSERT(!smp_async_claim_dispatch(&claim, &gen2),
        "a BUSY slot must refuse a second dispatch");
    TEST_ASSERT_EQ(claim, SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 1),
        "the refused dispatch must leave the owning claim byte-identical");
}

/* A superseded worker must not retire a claim it no longer owns -- completion
 * names the generation it was dispatched under. */
static void test_claim_complete_is_generation_exact(void)
{
    uint32_t claim = SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 4);

    TEST_ASSERT(!smp_async_claim_complete(&claim, 3),
        "an older generation must not complete a newer claim");
    TEST_ASSERT(!smp_async_claim_complete(&claim, 5),
        "a newer generation must not complete an older claim");
    TEST_ASSERT_EQ(claim, SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 4),
        "a refused completion must leave the claim unchanged");
    TEST_ASSERT(smp_async_claim_complete(&claim, 4),
        "the owning generation completes");
}

/* Parking is terminal for the dispatch in flight: the generation is preserved
 * so the BSP can tell WHICH dispatch died, and the parked worker can no longer
 * hand the slot back. */
static void test_claim_park_preserves_generation_and_blocks_completion(void)
{
    uint32_t claim = SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 12);
    uint32_t gen = 0;

    smp_async_claim_park(&claim);
    TEST_ASSERT_EQ(claim, SMP_ASYNC_CLAIM(SMP_ASYNC_OFFLINE, 12),
        "park must go OFFLINE at the same generation");
    TEST_ASSERT(smp_async_claim_is_dead(&claim, 12),
        "the BSP must see its own dispatch generation as dead");
    TEST_ASSERT(!smp_async_claim_is_dead(&claim, 11),
        "a different generation must not read as dead");
    TEST_ASSERT(!smp_async_claim_complete(&claim, 12),
        "a parked slot must not be completable");
    TEST_ASSERT(!smp_async_claim_dispatch(&claim, &gen),
        "a parked slot must not be dispatchable");
}

/* Park is idempotent: a CPU that faults twice must not disturb the generation
 * the BSP is waiting on. */
static void test_claim_park_is_idempotent(void)
{
    uint32_t claim = SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 3);

    smp_async_claim_park(&claim);
    smp_async_claim_park(&claim);
    TEST_ASSERT_EQ(claim, SMP_ASYNC_CLAIM(SMP_ASYNC_OFFLINE, 3),
        "a repeated park must be a no-op");
}

/* A CPU coming online publishes IDLE without resetting the generation, so a
 * stale in-flight completion still cannot match. */
static void test_claim_publish_idle_preserves_generation(void)
{
    uint32_t claim = SMP_ASYNC_CLAIM(SMP_ASYNC_OFFLINE, 9);
    uint32_t gen = 0;

    smp_async_claim_publish_idle(&claim);
    TEST_ASSERT_EQ(claim, SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 9),
        "publish-idle keeps the generation");
    TEST_ASSERT(smp_async_claim_dispatch(&claim, &gen),
        "a published slot is dispatchable");
    TEST_ASSERT_EQ(gen, 10u, "the generation continues rather than restarting");
    TEST_ASSERT_EQ(claim, SMP_ASYNC_CLAIM(SMP_ASYNC_RESERVED, 10),
        "dispatch reserves; it does not arm");
}

/* Republishing an in-flight slot as IDLE would erase the owner and let a second
 * dispatch land on a CPU still running the first -- the exact failure the claim
 * exists to prevent, so publish-idle must refuse anything but OFFLINE. */
static void test_claim_publish_idle_refuses_in_flight_slots(void)
{
    uint32_t busy     = SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 6);
    uint32_t reserved = SMP_ASYNC_CLAIM(SMP_ASYNC_RESERVED, 6);
    uint32_t gen = 0;

    smp_async_claim_publish_idle(&busy);
    TEST_ASSERT_EQ(busy, SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 6),
        "publish-idle must leave a BUSY slot byte-identical");
    TEST_ASSERT(!smp_async_claim_dispatch(&busy, &gen),
        "a BUSY slot must stay undispatchable across publish-idle");

    smp_async_claim_publish_idle(&reserved);
    TEST_ASSERT_EQ(reserved, SMP_ASYNC_CLAIM(SMP_ASYNC_RESERVED, 6),
        "publish-idle must leave a RESERVED slot byte-identical");
    TEST_ASSERT(!smp_async_claim_dispatch(&reserved, &gen),
        "a RESERVED slot must stay undispatchable across publish-idle");
}

/* Completion requires an exact BUSY(gen) claim, not merely a matching
 * generation: an IDLE slot at the same generation is an already-completed
 * dispatch, and accepting it would be a duplicate completion. */
static void test_claim_complete_refuses_idle_at_matching_generation(void)
{
    uint32_t claim = SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 4);

    TEST_ASSERT(!smp_async_claim_complete(&claim, 4),
        "an IDLE slot must not complete again at its own generation");
    TEST_ASSERT_EQ(claim, SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 4),
        "the refused completion must leave the claim unchanged");
    TEST_ASSERT(!smp_async_claim_is_dead(&claim, 4),
        "an IDLE slot is not dead at its own generation");
    TEST_ASSERT(!smp_async_claim_is_dead(&claim, 5),
        "an IDLE slot is not dead at any generation");
}

/* Arming is exact too: only the reservation's own generation may make a slot
 * runnable, and a parked reservation can never be armed. */
static void test_claim_arm_is_generation_exact_and_refuses_parked(void)
{
    uint32_t claim = SMP_ASYNC_CLAIM(SMP_ASYNC_RESERVED, 2);
    uint32_t parked = SMP_ASYNC_CLAIM(SMP_ASYNC_RESERVED, 2);

    TEST_ASSERT(!smp_async_claim_arm(&claim, 1),
        "a foreign generation must not arm a reservation");
    TEST_ASSERT_EQ(claim, SMP_ASYNC_CLAIM(SMP_ASYNC_RESERVED, 2),
        "the refused arm must leave the reservation unchanged");
    TEST_ASSERT(!smp_async_claim_arm(&claim, 3),
        "a newer generation must not arm an older reservation");

    smp_async_claim_park(&parked);
    TEST_ASSERT(!smp_async_claim_arm(&parked, 2),
        "a slot parked between reserve and arm must not become runnable");
    TEST_ASSERT_EQ(parked, SMP_ASYNC_CLAIM(SMP_ASYNC_OFFLINE, 2),
        "the refused arm must leave the parked slot unchanged");

    TEST_ASSERT(smp_async_claim_arm(&claim, 2), "the owning generation arms");
    TEST_ASSERT(!smp_async_claim_arm(&claim, 2),
        "arming twice must fail -- the slot is no longer RESERVED");
}

/* The helpers are called from the panic path and from an ISR, so their
 * defensive NULL and optional-output branches are contract, not decoration. */
static void test_claim_helpers_tolerate_null_and_optional_output(void)
{
    uint32_t claim = SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 0);
    uint32_t gen = 0xABCDu;

    TEST_ASSERT(!smp_async_claim_dispatch((uint32_t *)0, &gen),
        "dispatch on a NULL claim must refuse");
    TEST_ASSERT_EQ(gen, 0xABCDu,
        "a refused dispatch must not write the caller's generation");
    TEST_ASSERT(!smp_async_claim_arm((uint32_t *)0, 0),
        "arm on a NULL claim must refuse");
    TEST_ASSERT(!smp_async_claim_complete((uint32_t *)0, 0),
        "complete on a NULL claim must refuse");
    TEST_ASSERT(!smp_async_claim_is_dead((const uint32_t *)0, 0),
        "is_dead on a NULL claim must answer false");
    TEST_ASSERT(!smp_async_claim_is_busy((const uint32_t *)0, &gen),
        "is_busy on a NULL claim must answer false");
    TEST_ASSERT_EQ(gen, 0xABCDu,
        "a refused is_busy must not write the caller's generation");

    /* NULL park/publish must be inert rather than faulting. */
    smp_async_claim_park((uint32_t *)0);
    smp_async_claim_publish_idle((uint32_t *)0);
    smp_mask_set((uint32_t *)0, 0);
    smp_mask_clear((uint32_t *)0, 0);

    /* out_gen is optional on both reporting helpers. */
    TEST_ASSERT(smp_async_claim_dispatch(&claim, (uint32_t *)0),
        "dispatch must succeed without an out_gen");
    TEST_ASSERT(smp_async_claim_arm(&claim, 1),
        "the generation is still 1 even though it was not reported");
    TEST_ASSERT(smp_async_claim_is_busy(&claim, (uint32_t *)0),
        "is_busy must answer without an out_gen");
}

/* The AP-side gate: only a BUSY slot carries work, and it reports the
 * generation the worker must complete under. */
static void test_claim_is_busy_reports_owning_generation(void)
{
    uint32_t idle    = SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 5);
    uint32_t offline = SMP_ASYNC_CLAIM(SMP_ASYNC_OFFLINE, 5);
    uint32_t busy    = SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 5);
    uint32_t gen = 0xFFFFFFFFu;

    TEST_ASSERT(!smp_async_claim_is_busy(&idle, &gen),
        "an IDLE slot carries no work");
    TEST_ASSERT(!smp_async_claim_is_busy(&offline, &gen),
        "an OFFLINE slot carries no work");
    TEST_ASSERT(smp_async_claim_is_busy(&busy, &gen), "a BUSY slot carries work");
    TEST_ASSERT_EQ(gen, 5u, "is_busy must report the owning generation");
}

/* The generation is a wrapping counter; wrapping must not manufacture the
 * OFFLINE encoding or lose the state bits. */
static void test_claim_generation_wraps_without_aliasing_offline(void)
{
    uint32_t claim = SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, SMP_ASYNC_GEN_MAX);
    uint32_t gen = 0xFFFFFFFFu;

    TEST_ASSERT(smp_async_claim_dispatch(&claim, &gen),
        "dispatch at the maximum generation");
    TEST_ASSERT_EQ(gen, 0u, "the generation wraps to zero");
    TEST_ASSERT_EQ(claim, SMP_ASYNC_CLAIM(SMP_ASYNC_RESERVED, 0),
        "a wrapped claim must still read RESERVED, never OFFLINE");
    TEST_ASSERT(!smp_async_claim_is_dead(&claim, 0),
        "a wrapped reservation must not read as dead");
    TEST_ASSERT(smp_async_claim_arm(&claim, 0),
        "a wrapped reservation still arms");
    TEST_ASSERT_EQ(claim, SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 0),
        "a wrapped armed claim must read BUSY, never OFFLINE");
}

/* ---- Online mask helpers ---- */

static void test_mask_set_clear_test_count(void)
{
    uint32_t mask = 0;

    TEST_ASSERT_EQ(smp_mask_count(mask), 0u, "an empty mask counts zero");

    smp_mask_set(&mask, 0);
    smp_mask_set(&mask, 2);
    TEST_ASSERT_EQ(smp_mask_count(mask), 2u, "two CPUs set");
    TEST_ASSERT(smp_mask_test(mask, 0) && smp_mask_test(mask, 2),
        "both set bits must test true");
    TEST_ASSERT(!smp_mask_test(mask, 1),
        "an unset bit between two set ones must test false");

    /* The sparse case the section exists for: CPU1 parks, CPU2 survives. The
     * live COUNT is 2 while the highest live SLOT is 2, so a count can never
     * be used as a slot bound. */
    smp_mask_set(&mask, 1);
    smp_mask_clear(&mask, 1);
    TEST_ASSERT_EQ(smp_mask_count(mask), 2u, "clearing returns the count");
    TEST_ASSERT(smp_mask_test(mask, 2),
        "clearing a lower CPU must not disturb a higher live one");
}

static void test_mask_rejects_out_of_range_cpu(void)
{
    uint32_t mask = 0;

    smp_mask_set(&mask, MAX_CPUS);
    smp_mask_set(&mask, 0xFFFFFFFFu);
    TEST_ASSERT_EQ(mask, 0u,
        "a CPU id past MAX_CPUS must not set a bit");
    TEST_ASSERT(!smp_mask_test(0xFFFFFFFFu, MAX_CPUS),
        "a CPU id past MAX_CPUS must never test as a member");
}

static void test_mask_count_matches_every_populated_bit(void)
{
    uint32_t mask = 0;
    uint32_t i;
    char msg[96];

    for (i = 0; i < MAX_CPUS; i++) {
        smp_mask_set(&mask, i);
        snprintf(msg, sizeof(msg), "after setting CPU%u the count is %u",
                 (unsigned)i, (unsigned)smp_mask_count(mask));
        TEST_ASSERT(smp_mask_count(mask) == i + 1u, msg);
    }
}

/* ---- Live query surface (read-only) ---- */

/* The floor that predates SMP: Phase 0 code calls smp_cpu_count() before any
 * AP exists, and it must never answer zero. */
static void test_live_count_floor_and_bsp_membership(void)
{
    char msg[96];
    uint32_t live = smp_cpu_count();

    snprintf(msg, sizeof(msg), "smp_cpu_count() = %u", (unsigned)live);
    TEST_ASSERT(live >= 1u, msg);
    TEST_ASSERT(smp_cpu_is_online(0), "the BSP must be in the live online set");
    TEST_ASSERT_EQ(smp_mask_count(smp_online_mask()), live,
        "the live count must equal the population of the live mask");
}

/* Present is the machine; live is what is running. Live can never exceed it,
 * and a parked CPU must not shrink the machine. */
static void test_present_count_bounds_live_count(void)
{
    char msg[128];
    uint32_t live = smp_cpu_count();
    uint32_t present = smp_cpu_present_count();

    snprintf(msg, sizeof(msg), "live=%u present=%u", (unsigned)live,
             (unsigned)present);
    TEST_ASSERT(present >= live, msg);
    TEST_ASSERT(present <= MAX_CPUS, msg);
}

/* Every bit the live mask carries must name a slot that EXISTS. Slots are
 * allocated densely from 0 as bringup discovers CPUs, so no bit at or above the
 * present count can be legitimate -- and checking only the bits past MAX_CPUS
 * would let a nonexistent logical slot report itself online unchallenged. */
static void test_online_mask_bits_are_within_present_slots(void)
{
    uint32_t mask = smp_online_mask();
    uint32_t present = smp_cpu_present_count();
    uint32_t i;
    char msg[112];

    for (i = present; i < MAX_CPUS; i++) {
        snprintf(msg, sizeof(msg),
                 "slot %u is online but only %u slot(s) were discovered",
                 (unsigned)i, (unsigned)present);
        TEST_ASSERT(!smp_mask_test(mask, i), msg);
        TEST_ASSERT(!smp_cpu_is_online(i), msg);
    }
    for (i = MAX_CPUS; i < SMP_ONLINE_MASK_BITS; i++) {
        snprintf(msg, sizeof(msg), "online mask carries bit %u past MAX_CPUS",
                 (unsigned)i);
        TEST_ASSERT(!(mask & (1u << i)), msg);
    }
}

/* ---- Bringup arbitration (TODO-10 S27) ----
 *
 * The property under test is TERMINALITY: when the BSP's bringup loop moves
 * past a slot, that slot's verdict can no longer change, so no AP can join the
 * live set afterwards. That is unreachable from a boot -- it needs an AP
 * stalled at an exact instruction, which no emulator this repo runs can be
 * asked to produce -- but every interleaving of the four transitions is
 * reachable over caller-supplied words. */

static void test_bringup_ap_ready_from_starting(void)
{
    uint32_t state = AP_BRINGUP_STARTING;

    TEST_ASSERT(smp_ap_bringup_ready(&state),
        "an AP must be able to announce READY from STARTING");
    TEST_ASSERT_EQ(state, AP_BRINGUP_READY,
        "a won readiness CAS must leave the slot READY");
    TEST_ASSERT(!smp_ap_bringup_ready(&state),
        "announcing READY twice must fail -- the AP is already committed");
    TEST_ASSERT_EQ(state, AP_BRINGUP_READY,
        "a refused readiness CAS must not mutate the word");
}

/* The race the protocol exists to arbitrate, in the order where the BSP wins:
 * a slot it abandoned must refuse a late AP, which then parks dark. */
static void test_bringup_abandoned_slot_refuses_late_ap(void)
{
    uint32_t state = AP_BRINGUP_STARTING;

    TEST_ASSERT(!smp_bsp_bringup_arbitrate(&state),
        "arbitrating a slot that never reached READY must abandon it");
    TEST_ASSERT_EQ(state, AP_BRINGUP_ABANDONED,
        "an abandoned slot must read ABANDONED");
    TEST_ASSERT(!smp_ap_bringup_ready(&state),
        "an AP arriving after the verdict must lose and park dark");
    TEST_ASSERT_EQ(state, AP_BRINGUP_ABANDONED,
        "a late AP must not resurrect an abandoned slot");
}

/* And the order where the AP wins: the BSP must then accept it rather than
 * abandon it, because the AP is already parked awaiting a verdict. */
static void test_bringup_ready_slot_is_accepted_not_abandoned(void)
{
    uint32_t state = AP_BRINGUP_STARTING;

    TEST_ASSERT(smp_ap_bringup_ready(&state), "AP announces READY first");
    TEST_ASSERT(smp_bsp_bringup_arbitrate(&state),
        "arbitrating a READY slot must accept it");
    TEST_ASSERT_EQ(state, AP_BRINGUP_READY,
        "arbitration must NOT publish -- membership lands before ONLINE");

    smp_ap_bringup_accept(&state);
    TEST_ASSERT_EQ(state, AP_BRINGUP_ONLINE,
        "accept must release the AP by storing ONLINE");
}

/* Arbitration is idempotent on a slot that already has a verdict: re-running
 * the loop must not re-publish an accepted AP nor revive an abandoned one. */
static void test_bringup_arbitration_is_idempotent(void)
{
    uint32_t online = AP_BRINGUP_ONLINE;
    uint32_t abandoned = AP_BRINGUP_ABANDONED;

    TEST_ASSERT(smp_bsp_bringup_arbitrate(&online),
        "an already-ONLINE slot must still report accepted");
    TEST_ASSERT_EQ(online, AP_BRINGUP_ONLINE,
        "re-arbitrating an ONLINE slot must not change it");
    TEST_ASSERT(!smp_bsp_bringup_arbitrate(&abandoned),
        "an already-ABANDONED slot must still report rejected");
    TEST_ASSERT_EQ(abandoned, AP_BRINGUP_ABANDONED,
        "re-arbitrating an ABANDONED slot must not change it");
}

static void test_bringup_terminality_predicate(void)
{
    uint32_t starting  = AP_BRINGUP_STARTING;
    uint32_t ready     = AP_BRINGUP_READY;
    uint32_t online    = AP_BRINGUP_ONLINE;
    uint32_t abandoned = AP_BRINGUP_ABANDONED;

    TEST_ASSERT(!smp_ap_bringup_is_terminal(&starting),
        "STARTING is not a verdict");
    TEST_ASSERT(!smp_ap_bringup_is_terminal(&ready),
        "READY is not a verdict -- the BSP may still abandon it");
    TEST_ASSERT(smp_ap_bringup_is_terminal(&online), "ONLINE is terminal");
    TEST_ASSERT(smp_ap_bringup_is_terminal(&abandoned),
        "ABANDONED is terminal");
}

static void test_bringup_helpers_tolerate_null(void)
{
    TEST_ASSERT(!smp_ap_bringup_ready((uint32_t *)0),
        "a NULL handshake word must refuse readiness rather than fault");
    TEST_ASSERT(!smp_bsp_bringup_arbitrate((uint32_t *)0),
        "a NULL handshake word must arbitrate as rejected");
    TEST_ASSERT(!smp_ap_bringup_is_terminal((const uint32_t *)0),
        "a NULL handshake word is not terminal");
    smp_ap_bringup_accept((uint32_t *)0);   /* must not fault */
}

/* ---- Async fallback ownership decision (TODO-10 S27) ---- */

/* A step that already succeeded must NOT be re-run: none of the storage
 * initializers is idempotent, so re-running a success resets a working
 * controller and reallocates its queues. */
static void test_fallback_keeps_completed_success(void)
{
    uint32_t idle = SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 7);

    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_COMPLETED, BOOT_OK, idle, 7),
        (uint32_t)BOOT_ASYNC_FALLBACK_KEEP,
        "a completed BOOT_OK step must be kept, not re-run");
    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_COMPLETED, BOOT_DEGRADED, idle, 7),
        (uint32_t)BOOT_ASYNC_FALLBACK_KEEP,
        "a completed BOOT_DEGRADED step must be kept -- it ran to completion");
}

/* A step that failed through its own control flow holds no lock and has no CPU
 * inside it, so retrying it is exactly what the fallback is for. */
static void test_fallback_runs_completed_failure(void)
{
    uint32_t idle = SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 3);

    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_COMPLETED, BOOT_FATAL, idle, 3),
        (uint32_t)BOOT_ASYNC_FALLBACK_RUN,
        "a completed BOOT_FATAL step must be re-run on the BSP");
    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_COMPLETED, BOOT_DEFERRED, idle, 3),
        (uint32_t)BOOT_ASYNC_FALLBACK_RUN,
        "a completed BOOT_DEFERRED step must be re-run on the BSP");
    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_UNDISPATCHED, BOOT_OK, 0, 0),
        (uint32_t)BOOT_ASYNC_FALLBACK_RUN,
        "a step that never ran must be run");
}

/* THE defect this section exists to close: a worker that overran the deadline
 * and still owns its slot is inside the initializer right now. */
static void test_fallback_skips_step_whose_worker_still_owns_it(void)
{
    uint32_t busy     = SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 5);
    uint32_t reserved = SMP_ASYNC_CLAIM(SMP_ASYNC_RESERVED, 5);

    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_RUNNING, BOOT_FATAL, busy, 5),
        (uint32_t)BOOT_ASYNC_FALLBACK_SKIP,
        "a BUSY slot at the dispatch generation means the worker is still inside");
    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_RUNNING, BOOT_FATAL, reserved, 5),
        (uint32_t)BOOT_ASYNC_FALLBACK_SKIP,
        "a RESERVED slot at the dispatch generation is equally un-re-enterable");
}

/* A worker that finished LATE released its slot, so it is credited with what it
 * actually published -- not re-entered on the strength of the barrier's
 * sentinel.
 *
 * THE RESULT ARGUMENT IS THE LIVE ONE, and that is the whole point of these
 * cases. A timed-out step is RECORDED as (RUNNING, BOOT_FATAL), where the
 * BOOT_FATAL is a sentinel the barrier invented rather than anything the worker
 * said. Feeding that sentinel back in returns RUN and re-runs a driver that
 * merely finished late; boot_async_step_fallback() is what substitutes the
 * worker's published result once the claim shows the step released. So the
 * tuples below pair a RELEASED claim with each result a worker can really
 * publish -- which is exactly the caller-reachable set. */
static void test_fallback_credits_late_worker_by_its_claim(void)
{
    uint32_t retired   = SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 5);
    uint32_t next_gen  = SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 6);
    uint32_t offline   = SMP_ASYNC_CLAIM(SMP_ASYNC_OFFLINE, 5);

    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_RUNNING, BOOT_OK, retired, 5),
        (uint32_t)BOOT_ASYNC_FALLBACK_KEEP,
        "a late worker that published BOOT_OK must be kept, never re-run");
    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_RUNNING, BOOT_DEGRADED, retired, 5),
        (uint32_t)BOOT_ASYNC_FALLBACK_KEEP,
        "a late worker that published BOOT_DEGRADED ran to completion -- keep it");
    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_RUNNING, BOOT_FATAL, retired, 5),
        (uint32_t)BOOT_ASYNC_FALLBACK_RUN,
        "a late worker that published BOOT_FATAL may be retried");
    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_RUNNING, BOOT_DEFERRED, retired, 5),
        (uint32_t)BOOT_ASYNC_FALLBACK_RUN,
        "a late worker that published BOOT_DEFERRED may be retried");
    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_RUNNING, BOOT_OK, next_gen, 5),
        (uint32_t)BOOT_ASYNC_FALLBACK_KEEP,
        "a slot BUSY at a LATER generation no longer owns our step");
    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_RUNNING, BOOT_OK, offline, 5),
        (uint32_t)BOOT_ASYNC_FALLBACK_KEEP,
        "a parked slot is not executing our step -- the PURE decision trusts the "
        "result it is handed; boot_async_step_fallback() is what refuses to hand "
        "it an unattributable one (see the attributable-release tests below)");
}

/* WHICH release was it? A slot reaches the same OFFLINE(gen) whether the worker
 * finished and its CPU parked afterwards, or the worker faulted INSIDE the step
 * and panic.c parked it. Only the first published a result. The witness is the
 * separator, and getting this wrong is a driver re-entered mid-reset or a driver
 * that never initialized recorded as complete. */
static void test_release_attribution_requires_the_witness(void)
{
    uint32_t idle_5    = SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 5);
    uint32_t offline_5 = SMP_ASYNC_CLAIM(SMP_ASYNC_OFFLINE, 5);
    uint32_t witness_5 = SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 5);

    TEST_ASSERT(boot_async_release_attributable(idle_5, witness_5, 5),
        "the worker's own retirement (IDLE at our gen, witness published) IS "
        "attributable -- its async_result is what it stored");
    TEST_ASSERT(boot_async_release_attributable(offline_5, witness_5, 5),
        "completed, then the CPU parked afterwards: the witness survives the "
        "park, so the result is still the worker's own");
    TEST_ASSERT(!boot_async_release_attributable(offline_5, 0u, 5),
        "parked by panic.c with NO witness -- the worker faulted inside the "
        "step and never published; its async_result must not be believed");
    TEST_ASSERT(!boot_async_release_attributable(
                    SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 6), witness_5, 6),
        "a witness from generation 5 does not attribute generation 6");
    TEST_ASSERT(!boot_async_release_attributable(
                    SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 6),
                    SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 6), 5),
        "a slot re-dispatched at a later generation carries THAT dispatch's "
        "result, never ours");
    TEST_ASSERT(!boot_async_release_attributable(offline_5,
                    SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 5), 5),
        "the witness is the exact IDLE(gen) word -- a BUSY-shaped value is not "
        "a retirement and must not be accepted as one");

    /* The predicate is self-contained: it does not assume the caller already
     * proved release. The worker publishes its witness BEFORE the retiring CAS,
     * so BUSY(gen) paired with witness IDLE(gen) is a REAL window in which the
     * worker is still executing while looking attributable. */
    TEST_ASSERT(!boot_async_release_attributable(
                    SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 5), witness_5, 5),
        "witness published but the claim still BUSY -- the worker has not "
        "retired yet and its result is not final");
    TEST_ASSERT(!boot_async_release_attributable(
                    SMP_ASYNC_CLAIM(SMP_ASYNC_RESERVED, 5), witness_5, 5),
        "a RESERVED claim is mid-publication, never an attributable release");
}

/* The generation is masked on this path too, so an over-wide dispatch_gen
 * normalises rather than failing the witness compare and poisoning a healthy
 * step. */
static void test_release_attribution_masks_generation(void)
{
    uint32_t gen  = SMP_ASYNC_GEN_MAX;
    uint32_t idle = SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, gen);

    TEST_ASSERT(boot_async_release_attributable(idle,
                    SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, gen), 0xFFFFFFFFu),
        "an over-wide generation must mask down and still match its witness");
}

/* The release predicate the decision and every live caller share. One
 * definition of "still inside the step", pinned here so a copy cannot drift. */
static void test_fallback_release_predicate(void)
{
    TEST_ASSERT(!boot_async_claim_released(
                    SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 5), 5),
        "BUSY at the dispatch generation is NOT released");
    TEST_ASSERT(!boot_async_claim_released(
                    SMP_ASYNC_CLAIM(SMP_ASYNC_RESERVED, 5), 5),
        "RESERVED at the dispatch generation is NOT released");
    TEST_ASSERT(boot_async_claim_released(
                    SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 5), 5),
        "IDLE at the dispatch generation IS released");
    TEST_ASSERT(boot_async_claim_released(
                    SMP_ASYNC_CLAIM(SMP_ASYNC_OFFLINE, 5), 5),
        "a parked slot is released -- nobody is executing the step");
    TEST_ASSERT(boot_async_claim_released(
                    SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, 6), 5),
        "BUSY at a LATER generation belongs to a different dispatch");
    TEST_ASSERT(boot_async_claim_released(0u, 5),
        "a zero word is OFFLINE at generation 0 -- not our running step");
}

/* Absence of a running CPU is not restart safety: a worker cut down mid-step
 * left the controller half-programmed and any lock it held abandoned. */
static void test_fallback_skips_poisoned_step(void)
{
    uint32_t offline = SMP_ASYNC_CLAIM(SMP_ASYNC_OFFLINE, 9);
    uint32_t idle    = SMP_ASYNC_CLAIM(SMP_ASYNC_IDLE, 9);

    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_POISONED, BOOT_FATAL, offline, 9),
        (uint32_t)BOOT_ASYNC_FALLBACK_SKIP,
        "a step whose worker faulted mid-way must be degraded, not re-entered");
    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_POISONED, BOOT_OK, idle, 9),
        (uint32_t)BOOT_ASYNC_FALLBACK_SKIP,
        "POISONED is decided by HOW the step ended, never by a stale result");
}

/* The generation argument is masked before comparison, exactly as the claim
 * helpers mask theirs -- an out-of-range generation must normalise rather than
 * silently miss the ownership test and license a re-entry. */
static void test_fallback_generation_is_masked(void)
{
    uint32_t busy = SMP_ASYNC_CLAIM(SMP_ASYNC_BUSY, SMP_ASYNC_GEN_MAX);

    TEST_ASSERT_EQ((uint32_t)boot_async_fallback_decision(
                       BOOT_ASYNC_STEP_RUNNING, BOOT_FATAL, busy, 0xFFFFFFFFu),
        (uint32_t)BOOT_ASYNC_FALLBACK_SKIP,
        "an over-wide generation must mask down and still detect ownership");
}

/* ---- Registration ---- */

void test_register_smp_lifecycle(void)
{
    test_suite_register_cat("smp_lifecycle: zero claim word is OFFLINE",
        test_claim_zero_word_is_offline, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: claim encoding round-trips",
        test_claim_encoding_round_trips, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: dispatch then complete",
        test_claim_dispatch_then_complete, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: BUSY slot refuses second dispatch",
        test_claim_busy_slot_refuses_second_dispatch, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: completion is generation-exact",
        test_claim_complete_is_generation_exact, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: park preserves gen, blocks reuse",
        test_claim_park_preserves_generation_and_blocks_completion,
        TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: park is idempotent",
        test_claim_park_is_idempotent, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: publish-idle preserves generation",
        test_claim_publish_idle_preserves_generation, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: publish-idle refuses in-flight slots",
        test_claim_publish_idle_refuses_in_flight_slots, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: no duplicate completion when IDLE",
        test_claim_complete_refuses_idle_at_matching_generation, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: arm is exact and refuses parked",
        test_claim_arm_is_generation_exact_and_refuses_parked, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: NULL and optional-output branches",
        test_claim_helpers_tolerate_null_and_optional_output, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: is_busy reports owning generation",
        test_claim_is_busy_reports_owning_generation, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: generation wrap avoids OFFLINE",
        test_claim_generation_wraps_without_aliasing_offline, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: mask set/clear/test/count",
        test_mask_set_clear_test_count, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: mask rejects out-of-range CPU",
        test_mask_rejects_out_of_range_cpu, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: mask count tracks every bit",
        test_mask_count_matches_every_populated_bit, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: live count floor + BSP membership",
        test_live_count_floor_and_bsp_membership, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: present count bounds live count",
        test_present_count_bounds_live_count, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: online mask bits within MAX_CPUS",
        test_online_mask_bits_are_within_present_slots, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: AP announces READY from STARTING",
        test_bringup_ap_ready_from_starting, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: abandoned slot refuses a late AP",
        test_bringup_abandoned_slot_refuses_late_ap, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: READY slot is accepted, not abandoned",
        test_bringup_ready_slot_is_accepted_not_abandoned, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: arbitration is idempotent",
        test_bringup_arbitration_is_idempotent, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: only ONLINE/ABANDONED are terminal",
        test_bringup_terminality_predicate, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: bringup helpers tolerate NULL",
        test_bringup_helpers_tolerate_null, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: fallback keeps a completed success",
        test_fallback_keeps_completed_success, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: fallback re-runs a completed failure",
        test_fallback_runs_completed_failure, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: fallback skips a still-owned step",
        test_fallback_skips_step_whose_worker_still_owns_it, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: fallback credits a late worker",
        test_fallback_credits_late_worker_by_its_claim, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: fallback skips a poisoned step",
        test_fallback_skips_poisoned_step, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: fallback masks the generation",
        test_fallback_generation_is_masked, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: claim-released predicate",
        test_fallback_release_predicate, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: release attribution needs a witness",
        test_release_attribution_requires_the_witness, TEST_CAT_BOOT);
    test_suite_register_cat("smp_lifecycle: release attribution masks the gen",
        test_release_attribution_masks_generation, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
