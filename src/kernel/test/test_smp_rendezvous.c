/* ============================================================================
 * test_smp_rendezvous.c -- stop-the-world CPU rendezvous protocol
 *                          (TODO-26 S26)
 *
 * The rendezvous is a barrier that stops every other online CPU. Driving the
 * LIVE one from a test would stop the test runner, so every case here runs the
 * PURE protocol helpers over a CALLER-SUPPLIED struct smp_rendezvous -- the
 * same discipline test_smp_lifecycle.c uses for the online-membership words
 * and the bringup handshake.
 *
 * That is not a weaker test than driving the real barrier. The properties that
 * can actually go wrong are protocol properties: that a stale acknowledgement
 * from a previous round cannot complete this one, that a timeout leaves nobody
 * parked, that only the owner can release, and that a delayed duplicate IPI is
 * inert. Each is provable over synthetic state and none of them needs a second
 * processor.
 *
 * The pure cases are not the whole story, and saying "the smoke matrix carries
 * the wiring" would be false: the smoke matrix only proves the handler gets
 * REGISTERED, because nothing in the tree calls the barrier yet (its consumer,
 * the S3 suspend path, is deferred). A wiring or interrupt-state defect would
 * therefore first surface as a machine hang during a sleep transition. The one
 * live case at the bottom of this file closes that gap by actually stopping
 * and restarting the world.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/smp.h"

/* A round is open exactly while generation != released_gen, so a zeroed struct
 * -- which is what the kernel's own static instance holds before anything runs
 * -- must read IDLE. If it did not, the online-publish backstop would refuse
 * every AP at boot. */
static void test_rv_zeroed_is_idle(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_round_active(&rv), 0,
                   "a zeroed rendezvous must not read as an open round");
    TEST_ASSERT_EQ(smp_rendezvous_round_active((struct smp_rendezvous *)0), 0,
                   "a NULL rendezvous is inert, not an open round");
}

static void test_rv_arm_opens_a_round(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x6u, 0u), 0,
                   "arming an idle rendezvous must succeed");
    TEST_ASSERT_EQ(smp_rendezvous_round_active(&rv), 1,
                   "arming must open a round");
    TEST_ASSERT_EQ((uint32_t)rv.generation, 1u,
                   "arming must bump the generation exactly once");
}

/* Single-flight: a second arm while a round is open would replace the target
 * set underneath the owner, so the CPUs already parked for round N would be
 * waiting on a release for a round nobody is tracking. */
static void test_rv_arm_refuses_while_open(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x2u, 0u), 0, "first arm succeeds");
    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x4u, 0u), -1,
                   "a second arm while a round is open must be refused");
    TEST_ASSERT_EQ(rv.target_mask, 0x2u,
                   "the refused arm must not have replaced the target set");
}

/* The owner spins in begin(), not in the handler, so it can never acknowledge.
 * A mask naming it would be a round that can only ever time out. */
static void test_rv_arm_refuses_owner_in_target_mask(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x5u, 0u), -1,
                   "a target mask containing the owner must be refused");
    TEST_ASSERT_EQ(smp_rendezvous_round_active(&rv), 0,
                   "a refused arm must leave the rendezvous idle");
}

static void test_rv_arm_refuses_out_of_range_owner(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x2u, SMP_RENDEZVOUS_MASK_BITS), -1,
                   "an owner slot at the mask width must be refused");
    TEST_ASSERT_EQ(smp_rendezvous_round_active(&rv), 0,
                   "a refused arm must leave the rendezvous idle");
}

/* Uniprocessor is a real case, not a skip. With no other online CPU the round
 * opens, completes immediately, and still has to be balanced by a release. */
static void test_rv_empty_target_set_completes_immediately(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x0u, 0u), 0, "arm with no targets");
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 1,
                   "an empty target set is complete with nobody to signal");
    TEST_ASSERT_EQ(smp_rendezvous_release(&rv, 0u), 0,
                   "an empty round must still be releasable by its owner");
    TEST_ASSERT_EQ(smp_rendezvous_round_active(&rv), 0,
                   "release must close the round");
}

static void test_rv_completes_only_when_every_target_acks(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x6u, 0u), 0, "arm targets 1 and 2");
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 0,
                   "no acknowledgement yet, so not complete");

    TEST_ASSERT_EQ((uint32_t)smp_rendezvous_ack(&rv, 1u), 1u,
                   "acknowledging returns the generation acknowledged");
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 0,
                   "one of two targets is not complete");

    (void)smp_rendezvous_ack(&rv, 2u);
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 1,
                   "both targets acknowledged, so complete");
}

/* An acknowledgement from a CPU OUTSIDE the target set must not move the
 * round: completion reads the targeted slots only. */
static void test_rv_untargeted_ack_does_not_complete(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x2u, 0u), 0, "arm target 1 only");
    (void)smp_rendezvous_ack(&rv, 3u);
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 0,
                   "an acknowledgement from an untargeted slot must not complete");
}

/* THE ABA CASE, and the reason acknowledgement is generation-VALUED rather
 * than a bit in a shared mask. A CPU reads generation N, stalls, the owner
 * times out N and arms N+1, and only then does the stalled CPU publish. With a
 * bitmap that stale publication satisfies round N+1 while the CPU is still
 * running. With a per-slot generation it cannot: N never equals N+1. */
static void test_rv_stale_ack_cannot_complete_the_next_round(void)
{
    struct smp_rendezvous rv = {0};
    uint64_t stale;

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x6u, 0u), 0, "round N: targets 1,2");
    stale = smp_rendezvous_ack(&rv, 1u);          /* CPU 1 acknowledges N */
    TEST_ASSERT_EQ((uint32_t)stale, 1u, "CPU 1 acknowledged generation 1");

    /* Round N times out with CPU 2 never answering. */
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 0, "round N never completed");
    TEST_ASSERT_EQ(smp_rendezvous_release(&rv, 0u), 0, "fail-closed release");

    /* Round N+1 over the same targets. CPU 1's slot still holds N. */
    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x6u, 0u), 0, "round N+1");
    TEST_ASSERT_EQ((uint32_t)rv.generation, 2u, "second arm reaches generation 2");
    TEST_ASSERT_EQ((uint32_t)rv.ack_gen[1], 1u, "CPU 1's slot still carries the stale N");
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 0,
                   "a stale acknowledgement must not complete the next round");

    /* Only a FRESH acknowledgement from both counts. */
    (void)smp_rendezvous_ack(&rv, 1u);
    (void)smp_rendezvous_ack(&rv, 2u);
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 1,
                   "fresh acknowledgements from every target complete the round");
}

/* Release is the only way a parked CPU resumes, so a CPU that did not arm the
 * round must never be able to restart the world underneath the owner. */
static void test_rv_release_refuses_non_owner(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x6u, 0u), 0, "CPU 0 arms");
    TEST_ASSERT_EQ(smp_rendezvous_release(&rv, 1u), -1,
                   "a non-owner must not be able to release the round");
    TEST_ASSERT_EQ(smp_rendezvous_round_active(&rv), 1,
                   "a refused release must leave the round open");
    TEST_ASSERT_EQ(smp_rendezvous_release(&rv, 0u), 0, "the owner can release");
}

static void test_rv_release_refuses_when_idle(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_release(&rv, 0u), -1,
                   "releasing with no open round must be refused");
}

/* A timeout releases before returning, so no CPU is left parked behind a
 * failed begin(). After it, the rendezvous must be armable again. */
static void test_rv_timeout_release_leaves_nobody_parked(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x6u, 0u), 0, "arm");
    (void)smp_rendezvous_ack(&rv, 1u);            /* CPU 1 parked, CPU 2 did not */
    TEST_ASSERT_EQ(smp_rendezvous_park_step(&rv, 1u), 1,
                   "CPU 1 is parked while the round is open");

    TEST_ASSERT_EQ(smp_rendezvous_release(&rv, 0u), 0, "fail-closed release");
    TEST_ASSERT_EQ(smp_rendezvous_park_step(&rv, 1u), 0,
                   "the released CPU must resume, not stay parked");
    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x6u, 0u), 0,
                   "the rendezvous must be armable again after a timeout");
}

/* park_step is the whole parked-CPU loop body: it acknowledges and reports
 * whether to keep spinning. */
static void test_rv_park_step_acks_and_parks(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x2u, 0u), 0, "arm target 1");
    TEST_ASSERT_EQ(smp_rendezvous_park_step(&rv, 1u), 1,
                   "an open round parks the target");
    TEST_ASSERT_EQ((uint32_t)rv.ack_gen[1], 1u,
                   "parking must acknowledge the current generation");
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 1,
                   "the acknowledgement from park_step completes the round");
}

/* An edge-triggered 0xF9 can still be pending when the round has already
 * closed, and it executes after IRET. That delayed duplicate must be inert. */
static void test_rv_park_step_is_inert_when_no_round_is_open(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_park_step(&rv, 1u), 0,
                   "a duplicate IPI with no open round must not park");
    TEST_ASSERT_EQ((uint32_t)rv.ack_gen[1], 0u,
                   "an inert park_step must not write an acknowledgement");

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x2u, 0u), 0, "arm");
    TEST_ASSERT_EQ(smp_rendezvous_release(&rv, 0u), 0, "release");
    TEST_ASSERT_EQ(smp_rendezvous_park_step(&rv, 1u), 0,
                   "a duplicate arriving after release must not park");
}

/* A CPU delayed across a timeout and a re-arm re-acknowledges the NEW
 * generation on its next iteration instead of leaving the new round
 * permanently un-acknowledged. That convergence is why park_step re-reads the
 * generation every pass rather than latching it once on entry. */
static void test_rv_park_step_reacks_after_rearm(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x2u, 0u), 0, "round N");
    TEST_ASSERT_EQ(smp_rendezvous_park_step(&rv, 1u), 1, "CPU 1 parks for N");
    TEST_ASSERT_EQ(smp_rendezvous_release(&rv, 0u), 0, "N released");

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x2u, 0u), 0, "round N+1");
    TEST_ASSERT_EQ((uint32_t)rv.ack_gen[1], 1u, "the slot still carries N");
    TEST_ASSERT_EQ(smp_rendezvous_park_step(&rv, 1u), 1, "CPU 1 parks for N+1");
    TEST_ASSERT_EQ((uint32_t)rv.ack_gen[1], 2u,
                   "the parked CPU re-acknowledges the new generation");
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 1, "round N+1 completes");
}

/* Acknowledging twice at the same generation is what a duplicate IPI does. It
 * must not advance anything. */
static void test_rv_ack_is_idempotent(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x2u, 0u), 0, "arm");
    TEST_ASSERT_EQ((uint32_t)smp_rendezvous_ack(&rv, 1u), 1u, "first acknowledgement");
    TEST_ASSERT_EQ((uint32_t)smp_rendezvous_ack(&rv, 1u), 1u, "duplicate acknowledgement");
    TEST_ASSERT_EQ((uint32_t)rv.generation, 1u,
                   "acknowledging must never advance the generation");
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 1, "still exactly complete");
}

/* Out-of-range slots are refused rather than writing past ack_gen[]. */
static void test_rv_out_of_range_slot_is_refused(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x2u, 0u), 0, "arm");
    TEST_ASSERT_EQ((uint32_t)smp_rendezvous_ack(&rv, SMP_RENDEZVOUS_MASK_BITS), 0u,
                   "acknowledging past the mask width must be refused");
    TEST_ASSERT_EQ(smp_rendezvous_park_step(&rv, SMP_RENDEZVOUS_MASK_BITS), 0,
                   "parking past the mask width must be refused, not spin");
}

/* Every slot the online mask can name must be a usable rendezvous slot, or a
 * high-numbered CPU could be live and never targetable. */
static void test_rv_mask_width_covers_every_cpu_slot(void)
{
    TEST_ASSERT(SMP_RENDEZVOUS_MASK_BITS == SMP_ONLINE_MASK_BITS,
                "the rendezvous mask must be exactly the online-mask word");
    TEST_ASSERT(MAX_CPUS <= SMP_RENDEZVOUS_MASK_BITS,
                "every logical CPU slot must fit in the rendezvous mask");
}

/* TC4: every pure helper must answer safely for a NULL rendezvous rather than
 * dereference it. park_step is the one that matters most -- a NULL that
 * "keeps parking" would spin a CPU forever inside an IPI handler. */
static void test_rv_null_is_inert_everywhere(void)
{
    struct smp_rendezvous *nil = (struct smp_rendezvous *)0;

    TEST_ASSERT_EQ(smp_rendezvous_arm(nil, 0x2u, 0u), -1,
                   "arm(NULL) must be refused");
    TEST_ASSERT_EQ((uint32_t)smp_rendezvous_ack(nil, 1u), 0u,
                   "ack(NULL) must report generation 0");
    TEST_ASSERT_EQ(smp_rendezvous_complete(nil), 0,
                   "complete(NULL) must never claim completion");
    TEST_ASSERT_EQ(smp_rendezvous_release(nil, 0u), -1,
                   "release(NULL) must be refused");
    TEST_ASSERT_EQ(smp_rendezvous_park_step(nil, 1u), 0,
                   "park_step(NULL) must resume, never spin forever");
}

/* An out-of-range owner must be refused WITHOUT closing an open round -- a
 * bogus slot number must not become a way to release the world. */
static void test_rv_release_out_of_range_owner_leaves_round_open(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x2u, 0u), 0, "arm");
    TEST_ASSERT_EQ(smp_rendezvous_release(&rv, SMP_RENDEZVOUS_MASK_BITS), -1,
                   "an out-of-range owner slot must be refused");
    TEST_ASSERT_EQ(smp_rendezvous_round_active(&rv), 1,
                   "the round must still be open after a refused release");
    TEST_ASSERT_EQ(rv.owner_slot_plus1, 1u,
                   "the real owner must be unchanged");
}

/* TC4: both ends of the target mask must round-trip. Slot 0 is only ever a
 * target when a non-BSP owns the round; the top slot is the one an off-by-one
 * in the completion scan would drop. */
static void test_rv_target_mask_endpoints_round_trip(void)
{
    struct smp_rendezvous rv = {0};
    uint32_t top = SMP_RENDEZVOUS_MASK_BITS - 1u;

    /* Bottom slot, owned by slot 1 so slot 0 is a legal target. */
    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x1u, 1u), 0, "arm targeting slot 0");
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 0, "slot 0 has not acknowledged");
    (void)smp_rendezvous_ack(&rv, 0u);
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 1, "slot 0 completes the round");
    TEST_ASSERT_EQ(smp_rendezvous_release(&rv, 1u), 0, "owner releases");

    /* Top slot -- the one a completion scan bounded one short would miss. */
    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 1u << top, 0u), 0,
                   "arm targeting the top slot");
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 0,
                   "the top slot has not acknowledged");
    (void)smp_rendezvous_ack(&rv, top);
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 1,
                   "the top slot completes the round");
    TEST_ASSERT_EQ(smp_rendezvous_release(&rv, 0u), 0, "owner releases");
}

/* TC2: the ABA ordering the header actually claims to defeat, which the
 * earlier stale-ack test did NOT exercise. The dangerous sequence is a CPU
 * that reads generation N, is delayed past the owner's release AND re-arm, and
 * only THEN publishes N. The publication is a plain store into caller-owned
 * state here precisely because that is what a delayed CPU does. */
static void test_rv_late_stale_ack_after_rearm_is_ignored(void)
{
    struct smp_rendezvous rv = {0};
    uint64_t captured;

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x6u, 0u), 0, "round N: targets 1,2");
    captured = rv.generation;                  /* CPU 1 reads N, then stalls */
    TEST_ASSERT_EQ((uint32_t)captured, 1u, "captured generation is N == 1");

    TEST_ASSERT_EQ(smp_rendezvous_release(&rv, 0u), 0, "owner times out N");
    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x6u, 0u), 0, "owner arms N+1");

    /* Only NOW does the stalled CPU publish the generation it read. */
    rv.ack_gen[1] = captured;

    (void)smp_rendezvous_ack(&rv, 2u);         /* the other target is current */
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 0,
                   "a late stale publication must not complete the new round");

    (void)smp_rendezvous_ack(&rv, 1u);
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 1,
                   "only a current-generation publication completes it");
}

/* TC3: park_step reads `generation` and `released_gen` separately, so the
 * owner can release N and arm N+1 between those loads. Acting on that split
 * snapshot would resume a CPU that round N+1 is targeting. The helper re-reads
 * the generation before resuming, so the interleaving is simulated here by
 * performing the release AND the re-arm before calling it: a caller that
 * latched N would resume, and one that re-reads must keep parking. */
static void test_rv_park_step_does_not_resume_on_split_snapshot(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x2u, 0u), 0, "round N");
    TEST_ASSERT_EQ(smp_rendezvous_release(&rv, 0u), 0, "owner releases N");
    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x2u, 0u), 0, "owner arms N+1");

    TEST_ASSERT_EQ(smp_rendezvous_park_step(&rv, 1u), 1,
                   "a round is open at the CURRENT generation, so keep parking");
    TEST_ASSERT_EQ((uint32_t)rv.ack_gen[1], 2u,
                   "the parked CPU must acknowledge N+1, not the released N");
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 1, "round N+1 completes");
}

/* The generation is 64-bit specifically so it cannot wrap back onto a stale
 * acknowledgement. A 32-bit counter arming from UINT32_MAX would produce 0,
 * which already equals an untouched ack_gen -- completing a round before any
 * target had parked. Driving the counter to the 32-bit boundary proves the
 * width is real and not just a declaration. */
static void test_rv_generation_does_not_wrap_at_32_bits(void)
{
    struct smp_rendezvous rv = {0};

    rv.generation   = 0xFFFFFFFFu;
    rv.released_gen = 0xFFFFFFFFu;             /* idle at the 32-bit ceiling */
    TEST_ASSERT_EQ(smp_rendezvous_round_active(&rv), 0, "idle at the ceiling");

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x2u, 0u), 0, "arm past the ceiling");
    TEST_ASSERT(rv.generation == 0x100000000ull,
                "the generation must advance past 32 bits, never wrap to 0");
    TEST_ASSERT_EQ((uint32_t)rv.ack_gen[1], 0u,
                   "the target has never acknowledged anything");
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 0,
                   "an untouched zero slot must NOT satisfy the new generation");

    (void)smp_rendezvous_ack(&rv, 1u);
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 1,
                   "only a real acknowledgement completes it");
}

/* The live owner arms with NO targets and names them afterwards, so that
 * admission is already closed when it takes its snapshot. These cover the
 * setter that ordering depends on. */
static void test_rv_set_targets_after_arming(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x0u, 0u), 0, "arm with no targets");
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 1,
                   "an empty round is complete before targets are named");
    TEST_ASSERT_EQ(smp_rendezvous_set_targets(&rv, 0x6u, 0u), 0,
                   "the owner may name targets on an open round");
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 0,
                   "naming targets makes the round incomplete again");
    (void)smp_rendezvous_ack(&rv, 1u);
    (void)smp_rendezvous_ack(&rv, 2u);
    TEST_ASSERT_EQ(smp_rendezvous_complete(&rv), 1, "both targets acknowledge");
}

/* Naming targets is an OWNER operation on an OPEN round. A non-owner or a
 * closed round must be refused: redirecting a barrier somebody else is
 * spinning on would strand the CPUs already parked for the old set. */
static void test_rv_set_targets_refuses_non_owner_and_closed_round(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_set_targets(&rv, 0x2u, 0u), -1,
                   "naming targets with no open round must be refused");

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x2u, 0u), 0, "CPU 0 arms");
    TEST_ASSERT_EQ(smp_rendezvous_set_targets(&rv, 0x4u, 1u), -1,
                   "a non-owner must not be able to redirect the round");
    TEST_ASSERT_EQ(smp_rendezvous_set_targets(&rv, 0x5u, 0u), -1,
                   "a target mask naming the owner must be refused");
    TEST_ASSERT_EQ(smp_rendezvous_set_targets(&rv, 0x2u, SMP_RENDEZVOUS_MASK_BITS), -1,
                   "an out-of-range owner slot must be refused");
    TEST_ASSERT_EQ(rv.target_mask, 0x2u,
                   "every refused call must leave the target set unchanged");
}

/* park_step must acknowledge a generation CHANGE and then stop writing: a
 * store on every spin iteration would have every parked CPU hammering a shared
 * cache line for the whole barrier. Proven by clearing the slot underneath a
 * second call and observing that it is NOT rewritten at the same generation. */
static void test_rv_park_step_does_not_rewrite_the_same_generation(void)
{
    struct smp_rendezvous rv = {0};

    TEST_ASSERT_EQ(smp_rendezvous_arm(&rv, 0x2u, 0u), 0, "arm");
    TEST_ASSERT_EQ(smp_rendezvous_park_step(&rv, 1u), 1, "first pass parks");
    TEST_ASSERT_EQ((uint32_t)rv.ack_gen[1], 1u, "and acknowledges generation 1");

    rv.ack_gen[1] = 0xD00Du;                   /* a value the helper must not restore */
    TEST_ASSERT_EQ(smp_rendezvous_park_step(&rv, 1u), 1, "second pass keeps parking");
    TEST_ASSERT_EQ((uint32_t)rv.ack_gen[1], 1u,
                   "a differing slot IS refreshed, so the store is not dead");

    TEST_ASSERT_EQ(smp_rendezvous_park_step(&rv, 1u), 1, "third pass keeps parking");
    TEST_ASSERT_EQ((uint32_t)rv.ack_gen[1], 1u,
                   "and leaves the already-current acknowledgement alone");
}

/* The ONE case that drives the LIVE kernel barrier, and the only thing that can
 * prove the wiring: vector delivery, real AP acknowledgement, the owner's
 * interrupt masking, release, and AP resumption. Everything above proves the
 * PROTOCOL over synthetic state and would pass with the IPI never sent.
 *
 * TEST-SIDE-EFFECT-ALLOWED: this deliberately stops the world for the duration
 * of a few instructions. It is bounded and self-healing -- begin() fails closed
 * after SMP_RENDEZVOUS_TIMEOUT_MS having released anyone who parked, and end()
 * on the same CPU immediately after a successful begin() cannot fail (it is the
 * owner and the round is open), so no path leaves an AP parked.
 *
 * NOTHING inside the window may log, allocate, or take a lock -- that is the
 * DEADLOCK CONTRACT in smp.h, and a TEST_ASSERT inside the window would violate
 * it by reaching klog. So the window CAPTURES into locals and every assertion
 * runs after the world is running again. */
/* RFLAGS.IF. The tree has no define for it and this test's entire subject is
 * that bit, so naming it here beats three raw shifts. */
#define TEST_RFLAGS_IF  (1u << 9)

/* Bound for the post-release resume wait. Generous: the targets are resuming
 * concurrently and only have to fall out of a spin loop, so exhausting this is
 * a real failure rather than a slow machine. */
#define TEST_RV_RESUME_SPINS  100000000u

static void test_rv_live_begin_end_round_trip(void)
{
    uint32_t resumed_before[SMP_RENDEZVOUS_MASK_BITS];
    uint64_t flags_before, flags_inside = 0, flags_after;
    uint32_t mask_before, mask_after, expected_targets, self, slot;
    int rc_begin, rc_end = 0, active_inside = 0;

    self             = smp_cpu_id();
    for (slot = 0; slot < SMP_RENDEZVOUS_MASK_BITS; slot++)
        resumed_before[slot] = smp_rendezvous_resume_count(slot);
    mask_before      = smp_online_mask();
    expected_targets = mask_before & ~(1u << self);

    __asm__ volatile ("pushfq\n\t popq %0" : "=r"(flags_before));  /* ARCH: x86-64 */

    rc_begin = smp_rendezvous_begin(SMP_RENDEZVOUS_TIMEOUT_MS);
    if (rc_begin == 0) {
        /* ---- world stopped: capture only, no logging, no locks ---- */
        __asm__ volatile ("pushfq\n\t popq %0" : "=r"(flags_inside));  /* ARCH: x86-64 */
        active_inside = smp_rendezvous_in_progress();
        rc_end = smp_rendezvous_end();
        /* ---- world running again ---- */
    }

    __asm__ volatile ("pushfq\n\t popq %0" : "=r"(flags_after));  /* ARCH: x86-64 */
    mask_after = smp_online_mask();

    /* The barrier is armed on BOTH the single-CPU and the SMP boot path, so a
     * refusal is a real wiring failure on either, not a configuration to
     * tolerate. Widening this to accept -1 would make the test verify nothing. */
    TEST_ASSERT_EQ(rc_begin, 0,
                   "the live rendezvous must stop the world from the BSP");
    TEST_ASSERT_EQ(active_inside, 1,
                   "the round must read as open while the world is stopped");
    TEST_ASSERT_EQ(rc_end, 0, "the owner must be able to release the world");
    TEST_ASSERT_EQ(smp_rendezvous_in_progress(), 0,
                   "the round must be closed after release");

    /* The owner runs the window with interrupts MASKED (a preemptible owner
     * could be switched out with APs parked and never release them), and end()
     * must hand back exactly the state the caller had. */
    TEST_ASSERT_EQ((uint32_t)(flags_inside & TEST_RFLAGS_IF), 0u,
                   "interrupts must be masked while the world is stopped");
    TEST_ASSERT_EQ((uint32_t)(flags_after & TEST_RFLAGS_IF),
                   (uint32_t)(flags_before & TEST_RFLAGS_IF),
                   "end() must restore the caller's interrupt state exactly");

    /* Membership is a necessary condition, not the resumption proof. The
     * barrier never touches the online mask, so this catches a CPU the round
     * RETRACTED but says nothing about one still spinning in its park loop. */
    TEST_ASSERT_EQ(mask_after, mask_before,
                   "every CPU online before the round must be online after it");

    /* THE ACTUAL RESUMPTION PROOF. Each target bumps its resume counter after
     * its park loop exits, so an advance is positive evidence that the CPU got
     * out. Two earlier attempts at this assertion were both vacuous:
     * `smp_cpu_count() >= 1` only said the BSP was still counted, and mask
     * equality holds even when a target is permanently parked, because the
     * barrier does not change membership. Bounded wait, because the targets
     * resume concurrently with this code -- a failure here means a CPU never
     * left the barrier, which is the worst outcome the primitive has. */
    for (slot = 0; slot < SMP_RENDEZVOUS_MASK_BITS; slot++) {
        uint32_t spins = 0;
        if (!(expected_targets & (1u << slot)))
            continue;
        while (smp_rendezvous_resume_count(slot) == resumed_before[slot] &&
               spins < TEST_RV_RESUME_SPINS) {
            spins++;
            __asm__ volatile("pause");  /* ARCH: x86-64 */
        }
        TEST_ASSERT(smp_rendezvous_resume_count(slot) != resumed_before[slot],
                    "every targeted CPU must leave the barrier after release");
    }

    /* WITHOUT THIS THE TEST PASSES VACUOUSLY. With one online CPU the target
     * set is empty, begin() returns 0 without sending a single IPI, and every
     * assertion above still holds -- so on the 1-CPU boot-matrix legs this
     * proves nothing about vector delivery or AP acknowledgement, and nothing
     * in the output said so. Now the two runs are distinguishable: a machine
     * with another online CPU MUST have had targets, and a uniprocessor run
     * announces that it did not. */
    if (expected_targets == 0u) {
        TEST_SKIP("uniprocessor: no targets, so IPI delivery is not exercised");
        return;
    }
    TEST_ASSERT(smp_cpu_count() > 1u,
                "a non-empty target set implies more than one online CPU");
}

void test_register_smp_rendezvous(void)
{
    test_suite_register_cat("smp_rendezvous: zeroed state is idle",
        test_rv_zeroed_is_idle, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: arm opens a round",
        test_rv_arm_opens_a_round, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: arm refuses while open",
        test_rv_arm_refuses_while_open, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: arm refuses owner in target mask",
        test_rv_arm_refuses_owner_in_target_mask, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: arm refuses out-of-range owner",
        test_rv_arm_refuses_out_of_range_owner, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: empty target set completes",
        test_rv_empty_target_set_completes_immediately, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: completes only when all ack",
        test_rv_completes_only_when_every_target_acks, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: untargeted ack does not complete",
        test_rv_untargeted_ack_does_not_complete, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: stale ack cannot complete next round",
        test_rv_stale_ack_cannot_complete_the_next_round, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: release refuses non-owner",
        test_rv_release_refuses_non_owner, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: release refuses when idle",
        test_rv_release_refuses_when_idle, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: timeout leaves nobody parked",
        test_rv_timeout_release_leaves_nobody_parked, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: park_step acks and parks",
        test_rv_park_step_acks_and_parks, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: park_step inert with no round",
        test_rv_park_step_is_inert_when_no_round_is_open, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: park_step re-acks after re-arm",
        test_rv_park_step_reacks_after_rearm, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: ack is idempotent",
        test_rv_ack_is_idempotent, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: out-of-range slot refused",
        test_rv_out_of_range_slot_is_refused, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: mask width covers every CPU slot",
        test_rv_mask_width_covers_every_cpu_slot, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: NULL is inert everywhere",
        test_rv_null_is_inert_everywhere, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: out-of-range owner cannot release",
        test_rv_release_out_of_range_owner_leaves_round_open, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: target mask endpoints round-trip",
        test_rv_target_mask_endpoints_round_trip, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: late stale ack after re-arm ignored",
        test_rv_late_stale_ack_after_rearm_is_ignored, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: park_step resists split snapshot",
        test_rv_park_step_does_not_resume_on_split_snapshot, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: generation does not wrap at 32 bits",
        test_rv_generation_does_not_wrap_at_32_bits, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: set_targets after arming",
        test_rv_set_targets_after_arming, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: set_targets refuses non-owner and closed round",
        test_rv_set_targets_refuses_non_owner_and_closed_round, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: park_step does not rewrite same generation",
        test_rv_park_step_does_not_rewrite_the_same_generation, TEST_CAT_X86);
    test_suite_register_cat("smp_rendezvous: LIVE begin/end stops and restarts the world",
        test_rv_live_begin_end_round_trip, TEST_CAT_X86);
}

#endif /* KERNEL_TESTS */
