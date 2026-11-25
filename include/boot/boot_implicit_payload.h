/* boot_implicit_payload.h -- capacity policy for the payloads the loader
 * synthesizes itself.
 *
 * WHAT AN IMPLICIT PAYLOAD IS. Most descriptors in
 * `boot_info.payload_descriptors[]` come from boot.conf: the user names a
 * module, an initrd or a recovery image, the loader stages the path, and
 * `load_staged_payloads` flushes them into the table. An IMPLICIT payload has
 * no boot.conf entry -- the loader manufactures it because the machine needs
 * it (the firmware entropy seed; the signed headless-enrollment
 * authorization). Nobody asked for it, so nobody notices when it is missing.
 *
 * THE PROBLEM THIS HEADER EXISTS TO REMOVE. The staging table used to reserve
 * `BOOT_PAYLOAD_MAX - 1` slots against a comment naming THREE implicit
 * payloads, so at the boundary the implicit publishers competed for a single
 * slot and the loser was decided by where its call site happened to sit in
 * `efi_main`. The publishers papered over it by hand: the authorization
 * checked `idx + 1 >= BOOT_PAYLOAD_MAX` rather than `idx >= BOOT_PAYLOAD_MAX`,
 * deliberately yielding one slot early to an entropy seed published later in
 * the boot that could not signal a reservation backwards. That coupling was
 * one-directional and hardcoded: entropy knew nothing about the authorization,
 * and a third publisher added between them would have re-opened the race
 * silently.
 *
 * WHAT REPLACES IT. The publishers are NAMED in one list, the reservation is
 * COMPUTED from that list, and the claim is arbitrated here rather than at each
 * call site. Adding a publisher is one list entry: the reservation grows, the
 * static assertion re-checks it, and no call site changes. Each publisher gets
 * a slot by construction, so the yield is deleted rather than generalized.
 *
 * THE COST, STATED PLAINLY. The reservation is WORST-CASE and static: both
 * publishers are conditional (the authorization only when a valid file is
 * present, the seed only when seeding succeeded), so a machine using neither
 * still gives up two of its thirty-two slots. It cannot be dynamic. Staging
 * happens while boot.conf is parsed, long before either condition is knowable,
 * and a reservation decided after staging could only be honoured by refusing a
 * payload already accepted. Two slots of headroom is the price of never
 * silently dropping an entropy seed.
 *
 * PURE BY DESIGN. Nothing here touches `struct boot_info`, and that is
 * deliberate: the bootloader sees the mirror declaration in
 * `src/boot/uefi/boot_info_mirror.h` while the kernel-side unit tests see
 * `include/kernel/boot_info.h`, so a shared helper that took the struct could
 * be compiled by only one of them. Taking scalars instead lets the REAL
 * arbitration logic be tested directly rather than re-implemented in a fixture
 * -- which is the whole point, since a test that only inspects an
 * already-correct descriptor array passes just as happily when the arithmetic
 * behind it is wrong.
 *
 * Type discipline follows the other `include/boot/` shared headers: plain
 * `unsigned int`, no UEFI types and no `<stdint.h>`, because the bootloader has
 * one and the kernel has the other and this file must compile under both.
 */
#ifndef BOOT_IMPLICIT_PAYLOAD_H
#define BOOT_IMPLICIT_PAYLOAD_H

/* The implicit publishers, in one place.
 *
 * X(type_value, label, degradation)
 *   type_value  -- the `enum boot_payload_type` NUMBER. Spelled as a literal
 *                  rather than the enum name because this header is included
 *                  from both sides of the ABI mirror and the two spell the
 *                  constant differently (`BOOT_PAYLOAD_RANDOM_SEED = 7` in the
 *                  kernel enum, `#define BOOT_PAYLOAD_RANDOM_SEED 7u` in the
 *                  mirror). Each includer static-asserts its own constant
 *                  against the literal, so a divergence is a build failure on
 *                  whichever side drifted rather than a silent mismatch.
 *   label       -- what a human calls this payload, for the refusal line.
 *   degradation -- what the machine LOSES when it is refused. A refusal line
 *                  that names only the payload tells an operator nothing; this
 *                  is the half that says why it matters.
 *
 * A payload type NOT listed here is not implicit: it comes from boot.conf and
 * is bounded by the staging table instead. `BOOT_PAYLOAD_TPM_EVENT_LOG` and
 * `BOOT_PAYLOAD_USB_HANDOVER` are deliberately ABSENT -- both exist in the enum
 * and both are named by the old reservation comment, but neither has a producer
 * that writes a descriptor: the TPM log is retained through the scalar
 * `tpm_event_log` fields (publishing it as a RESERVED payload would double-pin
 * the same physical range and fail overlap detection), and USB handover state
 * travels in `usb_controller`. Reserving for them would cost two more slots to
 * protect code that does not exist. Add the entry in the same commit that adds
 * the producer.
 */
#define BOOT_IMPLICIT_PAYLOAD_LIST(X)                                          \
    X(7u,  "firmware entropy seed",                                            \
        "CSPRNG and stack canary stay on their degraded TSC-derived path")     \
    X(10u, "headless enrollment authorization",                                \
        "this machine cannot enroll without a console")

/* Count the list. `0 + 1 + 1` rather than a hand-maintained number, so the
 * reservation below cannot fall behind the publishers it is reserving for.
 * The per-entry macro stays defined because BOOT_IMPLICIT_PAYLOAD_COUNT expands
 * at its USE sites, not here. */
#define BOOT_IMPLICIT_PAYLOAD_COUNT_ONE(t, l, d) + 1
#define BOOT_IMPLICIT_PAYLOAD_COUNT \
    (0 BOOT_IMPLICIT_PAYLOAD_LIST(BOOT_IMPLICIT_PAYLOAD_COUNT_ONE))

/* The claim state is a bit per publisher in one `unsigned int`. */
_Static_assert(BOOT_IMPLICIT_PAYLOAD_COUNT > 0,
               "at least one implicit publisher must be listed");
_Static_assert(BOOT_IMPLICIT_PAYLOAD_COUNT <= 32,
               "claim mask is a 32-bit word; widen it before listing a 33rd publisher");

/* How many descriptor slots boot.conf staging may occupy, given the total.
 *
 * Parameterized on `max` rather than reading BOOT_PAYLOAD_MAX directly for the
 * same reason the list uses literals: each side has its own spelling of the
 * constant. The bootloader defines BOOT_PAYLOAD_STAGE_MAX from this macro, and
 * the kernel-side test evaluates it against the kernel's BOOT_PAYLOAD_MAX, so
 * both are checking one relation rather than two copies of an answer.
 */
#define BOOT_PAYLOAD_STAGE_MAX_FOR(max) ((max) - BOOT_IMPLICIT_PAYLOAD_COUNT)

/* Outcome of a claim. Every non-OK value is a REFUSAL to publish, and the
 * caller must not touch the descriptor table after one. */
enum boot_implicit_claim_result {
    BOOT_IMPLICIT_CLAIM_OK = 0,
    /* The type is not an implicit publisher -- a caller bug, not a capacity
     * condition. An explicit payload must go through the staging table so it is
     * counted against the staging bound. */
    BOOT_IMPLICIT_CLAIM_NOT_IMPLICIT = 1,
    /* This publisher already took its slot. With an exact reservation a second
     * claim would eat another publisher's guaranteed slot, so it is refused
     * rather than served. */
    BOOT_IMPLICIT_CLAIM_DUPLICATE = 2,
    /* No slot left. Unreachable while the staging bound holds, and kept anyway
     * because "unreachable" is a property of code elsewhere in the loader: this
     * function must not corrupt the table if that code changes. */
    BOOT_IMPLICIT_CLAIM_TABLE_FULL = 3,
    /* The caller passed a NULL claim-state pointer. */
    BOOT_IMPLICIT_CLAIM_NULL_STATE = 4,
};

/* Position of `payload_type` in the list, or -1 when it is not implicit.
 * The position is the claim-state bit index. */
static inline int boot_implicit_payload_index(unsigned int payload_type)
{
    int idx = 0;
#define BOOT_IMPLICIT_PAYLOAD_INDEX_CASE(t, l, d) \
    if (payload_type == (t)) { return idx; }      \
    idx++;
    BOOT_IMPLICIT_PAYLOAD_LIST(BOOT_IMPLICIT_PAYLOAD_INDEX_CASE)
#undef BOOT_IMPLICIT_PAYLOAD_INDEX_CASE
    (void)idx;
    return -1;
}

/* Human name for the refusal line. Never NULL: a refusal path must not have to
 * NULL-check the string it is about to print. */
static inline const char *boot_implicit_payload_label(unsigned int payload_type)
{
#define BOOT_IMPLICIT_PAYLOAD_LABEL_CASE(t, l, d) \
    if (payload_type == (t)) { return (l); }
    BOOT_IMPLICIT_PAYLOAD_LIST(BOOT_IMPLICIT_PAYLOAD_LABEL_CASE)
#undef BOOT_IMPLICIT_PAYLOAD_LABEL_CASE
    return "unknown implicit payload";
}

/* What the machine loses when this payload is refused. Never NULL, same
 * reason. */
static inline const char *boot_implicit_payload_degradation(unsigned int payload_type)
{
#define BOOT_IMPLICIT_PAYLOAD_DEGRADE_CASE(t, l, d) \
    if (payload_type == (t)) { return (d); }
    BOOT_IMPLICIT_PAYLOAD_LIST(BOOT_IMPLICIT_PAYLOAD_DEGRADE_CASE)
#undef BOOT_IMPLICIT_PAYLOAD_DEGRADE_CASE
    return "unknown degradation";
}

/* Arbitrate one implicit publisher's claim on a descriptor slot.
 *
 *   payload_type  -- the `enum boot_payload_type` value being published
 *   payload_count -- the table's CURRENT occupancy (`boot_info.payload_count`)
 *   payload_max   -- the table's capacity (`BOOT_PAYLOAD_MAX`)
 *   claimed_mask  -- in/out; one bit per publisher, zero-initialized once per
 *                    boot. Mutated ONLY on success, so a refusal leaves the
 *                    caller free to retry after fixing whatever it passed.
 *
 * On BOOT_IMPLICIT_CLAIM_OK the caller owns slot `payload_count` and must then
 * write the descriptor and advance both `payload_count` and
 * `payload_total_bytes`. On any other result the caller must change nothing.
 *
 * CALL THIS LAST. The claim is the commit point, so every fallible step --
 * allocation, file read, length check -- belongs BEFORE it, with the descriptor
 * fully built. Claiming first and failing afterwards would leave an empty slot
 * inside the packed prefix, and the kernel validator rejects that outright: the
 * machine would not boot at all, which is strictly worse than the dropped
 * payload this arbitration exists to prevent.
 */
static inline enum boot_implicit_claim_result
boot_implicit_payload_claim(unsigned int payload_type,
                            unsigned int payload_count,
                            unsigned int payload_max,
                            unsigned int *claimed_mask)
{
    int bit;

    if (claimed_mask == 0) {
        return BOOT_IMPLICIT_CLAIM_NULL_STATE;
    }
    bit = boot_implicit_payload_index(payload_type);
    if (bit < 0) {
        return BOOT_IMPLICIT_CLAIM_NOT_IMPLICIT;
    }
    if ((*claimed_mask & (1u << (unsigned int)bit)) != 0u) {
        return BOOT_IMPLICIT_CLAIM_DUPLICATE;
    }
    if (payload_count >= payload_max) {
        return BOOT_IMPLICIT_CLAIM_TABLE_FULL;
    }
    *claimed_mask |= (1u << (unsigned int)bit);
    return BOOT_IMPLICIT_CLAIM_OK;
}

/* Commit a claimed slot: write the descriptor and advance BOTH counters.
 *
 * A macro rather than a function because `struct boot_info` and
 * `struct boot_payload_desc` are declared in two different headers -- the
 * mirror for the loader, the kernel header for everything else -- so a typed
 * function could only be compiled by one side. The macro is type-agnostic and
 * is therefore ONE definition that both sides genuinely execute, which is the
 * point: the four lines below are exactly where a wrong index or a forgotten
 * counter would produce an invalid packed prefix, and a kernel-side test that
 * could not reach them would let that regression ship.
 *
 * Call ONLY after boot_implicit_payload_claim returned OK, with a fully built
 * descriptor. Evaluates `bi` once and `desc` twice; both are plain pointers at
 * every call site.
 */
#define BOOT_IMPLICIT_PAYLOAD_COMMIT(bi, desc)                                 \
    do {                                                                       \
        unsigned int _bipc_idx = (unsigned int)(bi)->payload_count;            \
        (bi)->payload_descriptors[_bipc_idx] = *(desc);                        \
        (bi)->payload_count = _bipc_idx + 1u;                                  \
        (bi)->payload_total_bytes += (desc)->length;                           \
    } while (0)

#endif /* BOOT_IMPLICIT_PAYLOAD_H */
