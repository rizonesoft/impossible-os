/* ============================================================================
 * tpm_budget.c -- the aggregate boot deadline for verified NV reads
 *
 * Two layers, deliberately separated. The ledger arithmetic is pure so the
 * whole policy (clamping, saturation, the overrun latch, the clockless breadth
 * bound) is fixture-testable without a TPM, a clock or a second CPU. The armed
 * boot ledger under it is the only code here that reads a clock or takes a
 * lock, and it holds that lock across nothing but integer arithmetic.
 * ========================================================================= */

#include "kernel/tpm_budget.h"
#include "kernel/timer.h"
#include "kernel/sched/spinlock.h"

/* ---- Pure ledger ---- */

void tpm_boot_budget_init(struct tpm_boot_budget *b, uint32_t quota_ms,
                          uint32_t max_ops)
{
    if (!b)
        return;
    b->quota_ms = quota_ms;
    b->spent_ms = 0u;
    b->max_ops  = max_ops;
    b->ops      = 0u;
    b->overrun  = 0u;
    /* Deterministic, NOT incremented from the old value: `b` may be a fresh
     * stack ledger whose previous epoch is uninitialised. The armed global gets
     * its generation from the monotonic counter in tpm_boot_budget_arm(). */
    b->epoch    = 0u;
    b->next_id  = 0u;
    {
        uint32_t i;
        for (i = 0u; i < TPM_BOOT_MAX_INFLIGHT; i++)
            b->live_ids[i] = 0u;
    }
    /* Armed LAST. Every field the admission path reads is settled before the
     * flag that makes it read them, which is the same publish-last discipline
     * the online mask uses -- and it is what makes arming safe against a
     * concurrent admission on another CPU even though the caller also holds
     * the lock. */
    b->armed    = 1u;
}

uint32_t tpm_boot_grant_work_ms(uint32_t remaining_grant_ms)
{
    return (remaining_grant_ms > TPM_NV_OP_BUDGET_MS) ? TPM_NV_OP_BUDGET_MS
                                                      : remaining_grant_ms;
}

uint32_t tpm_boot_budget_remaining_ms(const struct tpm_boot_budget *b)
{
    if (!b || b->quota_ms <= b->spent_ms)
        return 0u;
    return b->quota_ms - b->spent_ms;
}

tpm_nv_status_t tpm_boot_budget_admit(struct tpm_boot_budget *b,
                                      uint32_t request_ms,
                                      uint32_t *out_grant_ms)
{
    uint32_t remaining;

    /* Argument checks come FIRST, ahead of the unarmed short-circuit: a
     * malformed call is malformed whether or not a deadline is in force, and
     * an unarmed ledger must not launder it into a grant. */
    if (!b || !out_grant_ms || request_ms == 0u)
        return TPM_NV_BADARG;

    if (!b->armed) {
        *out_grant_ms = request_ms;
        return TPM_NV_OK;
    }

    /* An earlier operation already spent past what was left. Refusing here
     * rather than serving the (now zero) remainder is what stops one runaway
     * from being followed by a queue of doomed ones. */
    if (b->overrun)
        return TPM_NV_BUDGET;

    if (b->max_ops != 0u && b->ops >= b->max_ops)
        return TPM_NV_BUDGET;

    if (b->quota_ms != 0u) {
        remaining = tpm_boot_budget_remaining_ms(b);
        if (remaining == 0u)
            return TPM_NV_BUDGET;
        *out_grant_ms = (request_ms < remaining) ? request_ms : remaining;
    } else {
        /* No wall clock to measure against. The breadth bound above is the
         * whole enforcement, so the grant is whatever was asked for. */
        *out_grant_ms = request_ms;
    }

    if (b->ops != 0xFFFFFFFFu)
        b->ops++;
    return TPM_NV_OK;
}

void tpm_boot_budget_charge(struct tpm_boot_budget *b, uint32_t elapsed_ms)
{
    uint32_t remaining;

    if (!b)
        return;

    remaining = tpm_boot_budget_remaining_ms(b);

    /* Saturate rather than wrap. A wrapped charge would REFUND the budget,
     * turning the single most expensive operation of the boot into free
     * headroom for the next one. */
    if (b->spent_ms > 0xFFFFFFFFu - elapsed_ms)
        b->spent_ms = 0xFFFFFFFFu;
    else
        b->spent_ms += elapsed_ms;

    /* Latched, not recomputed: once an operation has overrun, later charges
     * cannot un-observe it. Only meaningful against a wall-clock quota. */
    if (b->armed && b->quota_ms != 0u && elapsed_ms > remaining)
        b->overrun = 1u;
}

/* ---- The armed boot ledger ---- */

static struct tpm_boot_budget s_boot;
static spinlock_t s_boot_lock = SPINLOCK_INIT;
/* Set when an admission was refused or a charge overran. Separate from the
 * ledger's own `overrun` because a refusal on the operation COUNT is also an
 * expiry the boot must be able to report, and that leaves no mark on the
 * clock-side state. */
static uint8_t s_boot_expired;
/* Monotonic across every arm and every test install. A reservation carries the
 * generation it was taken against, so one taken against a ledger that has since
 * been replaced settles into nothing instead of into its successor. */
static uint32_t s_epoch;

#ifdef KERNEL_TESTS
static int      s_clock_override;
static uint32_t s_clock_now_ms;

void tpm_boot_budget_test_set_clock(int enable, uint32_t now_ms)
{
    uint64_t irqf;

    spin_lock_irqsave(&s_boot_lock, &irqf);
    s_clock_override = enable;
    s_clock_now_ms   = now_ms;
    spin_unlock_irqrestore(&s_boot_lock, irqf);
}
#endif

uint32_t tpm_boot_now_ms(void)
{
#ifdef KERNEL_TESTS
    if (s_clock_override)
        return s_clock_now_ms;
#endif
    return (uint32_t)(uptime_ns() / 1000000ULL);
}

uint32_t tpm_boot_elapsed_ms(uint32_t mark)
{
    /* Unsigned subtraction: a 32-bit millisecond wrap yields the true delta
     * rather than a ~49-day one. */
    return tpm_boot_now_ms() - mark;
}

void tpm_boot_budget_arm(void)
{
    uint64_t irqf;
    uint32_t quota;

    /* system_get_freq() is 0 until a timer driver is published, and uptime_ns()
     * then reads a constant 0. A wall-clock quota measured against a constant
     * clock never spends: it would look enforced and enforce nothing, which is
     * strictly worse than admitting there is no clock and falling back to the
     * breadth bound. */
    quota = (system_get_freq() != 0u) ? TPM_BOOT_BUDGET_MS : 0u;

    spin_lock_irqsave(&s_boot_lock, &irqf);
    tpm_boot_budget_init(&s_boot, quota, TPM_BOOT_BUDGET_OPS);
    s_boot.epoch = ++s_epoch;
    s_boot_expired = 0u;
    spin_unlock_irqrestore(&s_boot_lock, irqf);
}

void tpm_boot_budget_disarm(void)
{
    uint64_t irqf;

    spin_lock_irqsave(&s_boot_lock, &irqf);
    /* Only the armed flag is cleared. The spent/ops counters and the expiry
     * flag are left standing so a caller that wants to REPORT what the boot
     * cost still can -- disarming ends the deadline, it does not erase the
     * measurement. */
    s_boot.armed = 0u;
    /* Outstanding reservations are released with the deadline they belonged to.
     * Leaving them would hold in-flight slots for the rest of the machine's
     * uptime against a ledger that no longer governs anything. */
    {
        uint32_t i;
        for (i = 0u; i < TPM_BOOT_MAX_INFLIGHT; i++)
            s_boot.live_ids[i] = 0u;
    }
    spin_unlock_irqrestore(&s_boot_lock, irqf);
}

tpm_nv_status_t tpm_boot_budget_admit_one(uint32_t request_ms,
                                          struct tpm_boot_grant *out_grant)
{
    tpm_nv_status_t st;
    uint32_t granted = 0u;
    uint64_t irqf;

    if (!out_grant)
        return TPM_NV_BADARG;
    /* EVERY field, on every path out. The unarmed and clockless branches reserve
     * nothing and relied on `id` "staying 0", but the caller declares its grant
     * uninitialised and settles on every successful admission -- so settlement
     * read an indeterminate id on runtime reads and on clockless boots. */
    out_grant->granted_ms = 0u;
    out_grant->epoch      = 0u;
    out_grant->id         = 0u;
    /* Read the clock BEFORE the lock: it is the one non-integer operation here,
     * and taking it inside would hold the lock across it for no benefit. */
    out_grant->mark_ms    = tpm_boot_now_ms();

    spin_lock_irqsave(&s_boot_lock, &irqf);
    st = tpm_boot_budget_admit(&s_boot, request_ms, &granted);
    if (st == TPM_NV_OK && s_boot.armed && s_boot.quota_ms != 0u) {
        uint32_t slot = TPM_BOOT_MAX_INFLIGHT, i;

        for (i = 0u; i < TPM_BOOT_MAX_INFLIGHT; i++)
            if (s_boot.live_ids[i] == 0u) { slot = i; break; }
        if (slot == TPM_BOOT_MAX_INFLIGHT) {
            /* Every in-flight slot is held. Refusing is the only honest answer:
             * admitting without a slot would hand out a reservation that
             * settlement could never find and therefore never return. */
            st = TPM_NV_BUDGET;
            s_boot_expired = 1u;
        } else {
            /* RESERVE IT, under the same lock that computed it. Without this
             * the grant is only a suggestion: a second CPU admitting
             * concurrently reads the same remainder and is promised the same
             * milliseconds. */
            tpm_boot_budget_charge(&s_boot, granted);
            s_boot.next_id++;
            if (s_boot.next_id == 0u)
                s_boot.next_id = 1u;      /* 0 means "free slot", never an id */
            s_boot.live_ids[slot] = s_boot.next_id;
            out_grant->granted_ms = granted;
            out_grant->epoch      = s_boot.epoch;
            out_grant->id         = s_boot.next_id;
        }
    } else if (st == TPM_NV_OK) {
        /* Unarmed or clockless: nothing is reserved, so there is nothing to
         * settle. id stays 0, which settlement drops. */
        out_grant->granted_ms = granted;
        out_grant->epoch      = s_boot.epoch;
    }
    if (st == TPM_NV_BUDGET) {
        s_boot_expired = 1u;
    }
    spin_unlock_irqrestore(&s_boot_lock, irqf);
    return st;
}

void tpm_boot_budget_settle(struct tpm_boot_grant *g)
{
    uint32_t elapsed, i, slot = TPM_BOOT_MAX_INFLIGHT;
    uint64_t irqf;

    if (!g || g->granted_ms == 0u || g->id == 0u)
        return;
    elapsed = tpm_boot_elapsed_ms(g->mark_ms);

    spin_lock_irqsave(&s_boot_lock, &irqf);
    /* A settlement for a ledger that no longer exists belongs to nobody. The
     * epoch moved because the boot re-armed, or a fixture swapped the ledger
     * out from under an operation; charging into the replacement would corrupt
     * an accounting this operation never took part in. */
    if (g->epoch != s_boot.epoch) {
        spin_unlock_irqrestore(&s_boot_lock, irqf);
        return;
    }
    if (!s_boot.armed || s_boot.quota_ms == 0u) {
        spin_unlock_irqrestore(&s_boot_lock, irqf);
        return;
    }
    /* CONSUME the reservation. A grant is a plain struct, so a copy of it is
     * indistinguishable from the original; the id table is what makes a replay
     * -- copied or not -- find nothing to settle. */
    for (i = 0u; i < TPM_BOOT_MAX_INFLIGHT; i++)
        if (s_boot.live_ids[i] == g->id) { slot = i; break; }
    if (slot == TPM_BOOT_MAX_INFLIGHT) {
        spin_unlock_irqrestore(&s_boot_lock, irqf);
        return;
    }
    s_boot.live_ids[slot] = 0u;

    if (elapsed > g->granted_ms) {
        /* Overran the reservation: charge only the EXCESS, since the grant
         * itself was charged at admission. */
        tpm_boot_budget_charge(&s_boot, elapsed - g->granted_ms);
    } else {
        /* Came in under: return the unused remainder. Bounded by our own
         * reservation, so this can never credit the ledger with time it never
         * had -- the refund is at most what admission took. */
        uint32_t refund = g->granted_ms - elapsed;
        s_boot.spent_ms = (s_boot.spent_ms > refund) ? (s_boot.spent_ms - refund) : 0u;
    }

    if (s_boot.overrun || tpm_boot_budget_remaining_ms(&s_boot) == 0u)
        s_boot_expired = 1u;
    spin_unlock_irqrestore(&s_boot_lock, irqf);

    /* The caller's own token is spent too, so the ordinary double-settle a
     * refactor introduces is caught at the call site rather than at the table. */
    g->granted_ms = 0u;
    g->id         = 0u;
}

void tpm_boot_budget_note_expiry(void)
{
    uint64_t irqf;

    spin_lock_irqsave(&s_boot_lock, &irqf);
    /* Only an ARMED ledger can expire. A caller outside the boot path runs on
     * an unarmed ledger that clamps nothing, so a slow operation there is slow
     * -- it has not exceeded a deadline, because no deadline was set for it,
     * and latching one would make a later report claim the boot ran out when
     * it never did. */
    if (s_boot.armed)
        s_boot_expired = 1u;
    spin_unlock_irqrestore(&s_boot_lock, irqf);
}

int tpm_boot_budget_expired(void)
{
    int expired;
    uint64_t irqf;

    spin_lock_irqsave(&s_boot_lock, &irqf);
    expired = (s_boot_expired != 0u);
    spin_unlock_irqrestore(&s_boot_lock, irqf);
    return expired;
}

#ifdef KERNEL_TESTS

struct tpm_boot_budget_test_state
tpm_boot_budget_test_install(uint32_t quota_ms, uint32_t max_ops)
{
    struct tpm_boot_budget_test_state prev;
    uint64_t irqf;
    uint32_t i;

    spin_lock_irqsave(&s_boot_lock, &irqf);
    prev.ledger  = s_boot;
    prev.expired = s_boot_expired;
    prev.ok      = 1u;
    for (i = 0u; i < TPM_BOOT_MAX_INFLIGHT; i++)
        if (s_boot.live_ids[i] != 0u) { prev.ok = 0u; break; }
    if (prev.ok) {
        tpm_boot_budget_init(&s_boot, quota_ms, max_ops);
        s_boot.epoch = ++s_epoch;
        s_boot_expired = 0u;
    }
    spin_unlock_irqrestore(&s_boot_lock, irqf);
    return prev;
}

void tpm_boot_budget_test_restore(struct tpm_boot_budget_test_state st)
{
    uint64_t irqf;

    /* A refused install changed nothing, so restoring its snapshot would
     * overwrite whatever the live ledger has done since. */
    if (!st.ok)
        return;

    spin_lock_irqsave(&s_boot_lock, &irqf);
    s_boot         = st.ledger;
    s_boot_expired = st.expired;
    /* The CAPTURED epoch is restored, not a fresh one. A reservation that was
     * outstanding BEFORE the fixture installed itself still carries that epoch,
     * and minting a new one here made it permanently unsettleable: its slot
     * could never be cleared and its reserved milliseconds never returned.
     * Fixture-era grants are still rejected, because they carry the intervening
     * epoch that s_epoch has already moved past. */
    spin_unlock_irqrestore(&s_boot_lock, irqf);
}

#endif /* KERNEL_TESTS */
