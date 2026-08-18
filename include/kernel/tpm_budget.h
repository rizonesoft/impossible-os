/* ============================================================================
 * tpm_budget.h -- ONE boot deadline for every verified NV read
 *
 * tpm_transport.h already bounds a SEQUENCE: work_ms for the commands plus a
 * reserved cleanup_ms so a mandatory FlushContext can always run. That bound is
 * per-operation and it is the wrong shape for the question this file answers.
 *
 * A verified read costs strictly more than the unverified read it replaces: the
 * index's identity must be established before its value is trusted, so what was
 * one NV_Read is now a ReadPublic and an NV_Read, and each authorized index
 * derives a policy contract through a sequence of its own. Every one of those
 * arms its own per-operation budget, so N records can each spend the full
 * allowance while no individual bound is ever exceeded, and the boot they add
 * up to is unbounded. The number that decides whether Phase 1 can carry
 * verified reads at all is the SUM across the boot.
 *
 * So the ledger here is aggregate and it is armed once. Each admission is
 * clamped to what the BOOT has left rather than handed the full per-call
 * budget, and the charge is the full OBSERVED elapsed span rather than the
 * grant: charging the grant would undercount exactly the operations that
 * overran, which is the population a deadline exists to bound.
 *
 * EXHAUSTION IS REPORTED, NEVER A SILENTLY SHORTER READ SET. TPM_NV_BUDGET
 * propagates out of the read; a boot that could not afford to verify every
 * record must say so rather than publish a verdict computed over the subset it
 * happened to reach.
 *
 * A CLOCKLESS BOOT IS BOUNDED BY BREADTH, NOT LEFT UNLIMITED. When no wall
 * clock is available the quota is zero and the operation count is the only
 * bound left; "unmeasurable" must not mean "unbounded".
 *
 * The ledger arithmetic is PURE (no MMIO, no clock, no lock) and fixture
 * tested. The armed boot ledger below it is the only part that reads a clock
 * or takes a lock.
 * ========================================================================= */
#ifndef KERNEL_TPM_BUDGET_H
#define KERNEL_TPM_BUDGET_H

#include "kernel/types.h"
#include "kernel/tpm_nv.h"

/* THE TWO SEQUENCE COSTS, and they differ because the sequences do.
 *
 * A POLICY sequence (deriving an index's authPolicy) opens a TPM session, so it
 * must keep the cleanup reserve available for the mandatory FlushContext that
 * releases it. A verified READ sequence uses password authorization and issues
 * only NV_ReadPublic and NV_Read: it creates nothing, tears nothing down, and
 * calls no teardown helper.
 *
 * Pricing both at work+cleanup looked conservative and was wrong at exactly one
 * boundary: with 3000..4999 ms of grant left, a read that fits its work ceiling
 * comfortably was refused for a cleanup reserve it would never spend -- and a
 * refused verified read leaves the boot's integrity verdict UNPUBLISHED, which
 * is the outcome the whole section exists to avoid. Each guard therefore prices
 * the sequence it is about to start, not a worst case borrowed from a different
 * kind of sequence.
 *
 * A sequence is INDIVISIBLE either way: once started it can spend its whole
 * price whatever the remainder, which is why the guards check before starting
 * rather than accounting afterwards. */
#define TPM_NV_POLICY_SEQ_COST_MS   (TPM_NV_OP_BUDGET_MS + TPM_NV_OP_CLEANUP_BUDGET_MS)
#define TPM_NV_READ_SEQ_COST_MS     (TPM_NV_OP_BUDGET_MS)

/* What ONE verified read may request: the record index's policy contract, the
 * counter index's policy contract, and the read itself. The admission clamps this to the boot remainder, so it is a ceiling
 * on a single read rather than an entitlement.
 *
 * HONEST SCOPE: this bounds the SEQUENCES a verified read runs. The transport
 * may additionally spend up to two bounded interface aborts recovering a
 * sequence whose budget expired (tpm_transport.h), and those are bounded there,
 * not here. The aggregate is a bound on admitted work, not a promise about
 * total wall clock under a misbehaving device. */
#define TPM_NV_VERIFIED_READ_BUDGET_MS  (2u * TPM_NV_POLICY_SEQ_COST_MS + TPM_NV_READ_SEQ_COST_MS)

/* The aggregate wall-clock allowance for every verified NV read in one boot.
 *
 * Sized from the read set, NOT from the per-operation bound multiplied by the
 * number of callers: the second grows every time a caller is added, which is
 * the unbounded shape this ledger exists to replace. Phase 1 issues a small
 * fixed number of verified reads (the baseline bind record and the A/B floor),
 * so the quota is two full-price reads.
 *
 * This is a CEILING on pathological hardware, not an expectation: it is three
 * per-sequence worst cases times two reads, and a healthy TPM answers each NV
 * command in single-digit milliseconds. Sizing it to the observed cost instead
 * would refuse honest slow devices, which is the failure the unpublished-
 * verdict path makes expensive. */
#define TPM_BOOT_BUDGET_MS      (2u * TPM_NV_VERIFIED_READ_BUDGET_MS)

/* The breadth bound, and the ONLY bound on a boot with no usable wall clock.
 * Comfortably above the read set so it never binds on a healthy boot; it is a
 * runaway stop, not a quota. */
#define TPM_BOOT_BUDGET_OPS     8u

/* How many reservations may be outstanding at once. Bounds the live-id table
 * that makes settlement exactly-once; an admission finding no free slot is
 * refused exactly like one finding no budget, so the table can never overflow
 * and a leaked reservation cannot be papered over by reusing a slot. */
#define TPM_BOOT_MAX_INFLIGHT   8u

/* A quota below one read's request would clamp the FIRST admission, so every
 * verified read on every boot would run on a partial grant and the ledger would
 * look like it was working while refusing honest reads. */
_Static_assert(TPM_BOOT_BUDGET_MS >= TPM_NV_VERIFIED_READ_BUDGET_MS,
               "boot quota must cover at least one full verified read");
/* And a read's request must fund each step it actually takes, or a guard would
 * refuse work the admission had already promised. */
_Static_assert(TPM_NV_VERIFIED_READ_BUDGET_MS >= TPM_NV_POLICY_SEQ_COST_MS,
               "a read's request must fund its first policy sequence");
_Static_assert(TPM_NV_POLICY_SEQ_COST_MS > TPM_NV_READ_SEQ_COST_MS,
               "a policy sequence costs more than a read: it must clean up a session");

/* The ledger. Fields are public because the fixture tests assert on them
 * directly and because there is nothing here worth hiding behind an accessor:
 * four counters and two flags, with no invariant a caller could break that the
 * pure functions below do not re-establish. */
struct tpm_boot_budget {
    uint32_t quota_ms;   /* 0 = no wall-clock quota (clockless boot) */
    uint32_t spent_ms;   /* saturating; never wraps back into credit */
    uint32_t max_ops;    /* 0 = no breadth bound */
    uint32_t ops;        /* admissions granted, saturating */
    uint32_t epoch;      /* ledger generation; see struct tpm_boot_grant */
    uint32_t next_id;    /* monotonic reservation id source (0 is never issued) */
    uint32_t live_ids[TPM_BOOT_MAX_INFLIGHT];  /* 0 = free slot */
    uint8_t  armed;      /* an UNARMED ledger governs nothing and clamps nothing */
    uint8_t  overrun;    /* latched once a charge exceeded what was left */
};

/* What an admission hands back, and the reason admission is not just a number.
 *
 * The grant is RESERVED against the ledger at admission, not merely computed
 * from it. Computing and releasing the lock let two concurrent admissions read
 * the same remainder and each be told they could spend it, so the aggregate
 * could be exceeded before either charge landed -- a quota that only bounds
 * SEQUENTIAL callers is not the SMP-safe ledger this header promises.
 *
 * `epoch` is what makes settlement safe against a ledger that was re-armed (or
 * swapped by the test seam) while an operation was in flight: a settlement
 * whose epoch no longer matches belongs to a ledger that no longer exists, and
 * charging it into the replacement would corrupt an accounting it never took
 * part in. Such a settlement is DROPPED rather than misapplied. */
struct tpm_boot_grant {
    uint32_t granted_ms; /* the reserved allowance */
    uint32_t epoch;      /* the ledger generation it was reserved against */
    uint32_t mark_ms;    /* the clock reading at reservation */
    uint32_t id;         /* unique while outstanding; consumed by settlement */
};

/* Arm a ledger. `quota_ms` 0 means no wall clock is available, which leaves
 * `max_ops` as the only bound. */
void tpm_boot_budget_init(struct tpm_boot_budget *b, uint32_t quota_ms,
                          uint32_t max_ops);

/* Ask for `request_ms` of work allowance.
 *
 * TPM_NV_OK with `*out_grant_ms` clamped to the boot remainder, or
 * TPM_NV_BUDGET when the quota is spent, the operation count is reached, or an
 * earlier charge already overran. TPM_NV_BADARG for a NULL ledger, a zero
 * request, or a NULL grant pointer.
 *
 * An UNARMED ledger admits and does not clamp: a caller outside the boot path
 * must never be refused by a deadline nobody set up for it. Pure. */
tpm_nv_status_t tpm_boot_budget_admit(struct tpm_boot_budget *b,
                                      uint32_t request_ms,
                                      uint32_t *out_grant_ms);

/* Charge the OBSERVED elapsed span, not the grant. Latches `overrun` when the
 * charge exceeded what was left, so the next admission refuses rather than
 * letting a single runaway operation be followed by more. Saturating: a charge
 * can never wrap `spent_ms` and refund the budget. Pure. */
void tpm_boot_budget_charge(struct tpm_boot_budget *b, uint32_t elapsed_ms);

/* The work allowance one SEQUENCE may take out of a remaining aggregate grant.
 *
 * The aggregate bounds the whole verified read; it must never WIDEN a single
 * sequence past the per-operation ceiling the transport was designed around.
 * Pure and exported so the relation is assertable: inlined at the call site it
 * was invisible to every test, and deleting it would have let the final read
 * inherit the full three-sequence grant while a fast fixture stayed green. */
uint32_t tpm_boot_grant_work_ms(uint32_t remaining_grant_ms);

/* Wall-clock milliseconds left against the quota, saturating at 0. Returns 0
 * for a clockless ledger (`quota_ms == 0`), where there is no remainder to
 * report and `max_ops` is the live bound, so check `quota_ms` before reading a
 * 0 here as exhaustion. Pure. */
uint32_t tpm_boot_budget_remaining_ms(const struct tpm_boot_budget *b);

/* ---- The armed boot ledger ----
 *
 * One ledger for the whole boot, armed before the first verified read and
 * shared by every caller after it. SMP-safe: a short-hold IRQ-safe spinlock
 * guards the counters, and no TPM I/O ever happens under it. */

/* Arm the boot ledger. It RESETS the boot's allowance, so it belongs at
 * exactly one place in Phase 1, before the first verified read.
 *
 * Arms CLOCKLESS (quota 0, breadth bound only) when no timer frequency is
 * published yet, because a quota measured against a clock that always reads
 * zero would never spend and would look enforced while enforcing nothing. */
void tpm_boot_budget_arm(void);

/* Disarm the boot ledger. MANDATORY, and it pairs with the arm on every path
 * out of the armed region.
 *
 * The ledger bounds ONE BOOT's verified reads, and nothing else ends a boot.
 * Left armed, its quota and operation count keep applying to every verified
 * read the machine ever performs: a runtime mark-good, a later attestation, or
 * a test suite would each be refused with TPM_NV_BUDGET once Phase 1's
 * allowance was spent, and the refusal would look like a TPM fault forever
 * after. Disarming restores the unarmed contract -- admit, do not clamp --
 * which is the correct posture for a caller no boot deadline governs. */
void tpm_boot_budget_disarm(void);

/* Admit against the armed boot ledger, RESERVING the grant.
 *
 * Same status contract as tpm_boot_budget_admit(), but the reservation is taken
 * under the same lock that computed it, so two concurrent admissions can never
 * be handed the same remainder. Every successful admission MUST be settled with
 * tpm_boot_budget_settle() on every path out, including error paths -- an
 * unsettled reservation is permanently spent budget.
 *
 * Before the ledger is armed, and after it is disarmed, this admits unclamped,
 * which is what makes a verified read outside the boot path work normally. */
tpm_nv_status_t tpm_boot_budget_admit_one(uint32_t request_ms,
                                          struct tpm_boot_grant *out_grant);

/* Settle a reservation against the OBSERVED elapsed span.
 *
 * Charges the excess when the operation overran its grant and returns the
 * unused remainder when it did not, so the ledger reflects what was actually
 * spent rather than what was optimistically reserved. A settlement whose epoch
 * no longer matches the live ledger is DROPPED (see struct tpm_boot_grant).
 *
 * Latches expiry when the settlement leaves nothing to admit, so the boot can
 * report that it ran out rather than that it was clean.
 *
 * EXACTLY ONCE, enforced at the ledger rather than trusted of the caller. The
 * reservation id is consumed here, so a second settlement of the same grant --
 * or of a COPY of it, which a plain struct makes trivial -- is dropped. Without
 * that, replaying an under-budget settlement refunds the same milliseconds
 * again and reopens budget that other live reservations are still holding.
 * `g` is cleared on a successful settlement so the caller's own token cannot be
 * reused either. */
void tpm_boot_budget_settle(struct tpm_boot_grant *g);

/* Nonzero once a boot-ledger admission has been REFUSED, or a charge overran.
 *
 * The boot caller reports expiry with this rather than inferring it from a
 * status: TPM_NV_BUDGET maps to TPM_BASELINE_NO_TPM alongside TRANSPORT and
 * BUSY (tpm_baseline.c nv_to_baseline), deliberately, so that a TPM which
 * merely answered too slowly leaves the verdict unpublished instead of
 * reporting a false tamper. That collapse is right for the VERDICT and wrong
 * for DIAGNOSIS, so the distinguishing fact is published here instead. */
int tpm_boot_budget_expired(void);

/* Latch expiry from a caller that ran out of GRANT rather than out of quota.
 *
 * A verified read whose prerequisite work consumes its whole grant is refused
 * by the caller's own deadline check, not by an admission, so nothing in this
 * module would otherwise observe the refusal -- and the boot would report a
 * clean-but-unmeasured verdict with no way to say why. Every caller that turns
 * a grant check into TPM_NV_BUDGET calls this. */
void tpm_boot_budget_note_expiry(void);

/* The monotonic millisecond clock the ledger measures against.
 * and boot-wide: derived from uptime_ns(), never from the transport's per-CPU
 * RDTSC, which is not comparable across a migration. */
uint32_t tpm_boot_now_ms(void);

/* Milliseconds elapsed since `mark`. Unsigned arithmetic makes a 32-bit
 * millisecond wrap (~49.7 days) yield the correct delta rather than a huge
 * one. Reads the clock; does not touch the ledger. */
uint32_t tpm_boot_elapsed_ms(uint32_t mark);



/* KERNEL_TESTS-gated block (release-flavor test-surface exclusion): the seam
 * below is called exclusively from src/kernel/test/test_tpm_*.c, which is
 * pruned entirely at KERNEL_TESTS=off; guarding the declaration keeps a release
 * build from even seeing the prototype. */
#ifdef KERNEL_TESTS

/* Drive the module's clock from a fixture. Without this the wall-clock half of
 * the ledger is untestable: a fake TPM answers in microseconds, so every
 * fixture read charges 0 ms and deleting the charge entirely would leave every
 * suite green. `enable` 0 returns the module to uptime_ns(). */
void tpm_boot_budget_test_set_clock(int enable, uint32_t now_ms);

/* Full snapshot of the armed boot ledger plus its expiry flag, so a test can
 * put back EVERY field it perturbed. A test that armed the live ledger and
 * walked away would leave the running boot's allowance reset, which is exactly
 * the mis-routing the transport seam's full-snapshot restore exists to
 * prevent. */
struct tpm_boot_budget_test_state {
    struct tpm_boot_budget ledger;
    uint8_t expired;
    uint8_t ok;          /* 0 = install refused (ledger not quiescent) */
};

/* Arm the boot ledger with a chosen quota and return the prior state. Lets a
 * fixture drive a verified read against a deliberately small aggregate without
 * waiting out TPM_BOOT_BUDGET_MS of real time.
 *
 * REQUIRES A QUIESCENT LEDGER, and says so loudly rather than coping. If a
 * reservation is outstanding when the fixture installs itself, that operation
 * settles against an epoch the fixture has moved past, so its settlement is
 * dropped and its token consumed -- and restore then copies the captured
 * reservation back into a ledger whose owner has already finished with it,
 * stranding both the slot and its milliseconds until the next arm. Coping with
 * that interleaving would mean suspending and merging settlements, which is
 * machinery in a test seam; refusing is honest and the caller is a fixture that
 * can simply not overlap a live read. `ok` is 0 when the install was refused,
 * in which case nothing was changed and restore must not be called. */
struct tpm_boot_budget_test_state
tpm_boot_budget_test_install(uint32_t quota_ms, uint32_t max_ops);

/* Restore a snapshot captured by tpm_boot_budget_test_install(). */
void tpm_boot_budget_test_restore(struct tpm_boot_budget_test_state st);

#endif /* KERNEL_TESTS */

#endif /* KERNEL_TPM_BUDGET_H */
