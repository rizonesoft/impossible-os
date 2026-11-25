/* boot_payload_limits.h -- the per-type length contract for typed payload
 * descriptors, in ONE place both the Phase-0 reservation pass and every
 * Phase-1 consumer read.
 *
 * WHY THIS EXISTS. `boot_reserved_populate_from_info()` pins
 * `[phys_start, +length)` for every descriptor carrying
 * `BOOT_PAYLOAD_FLAG_RESERVED` once `BOOT_CAP_PAYLOAD_DESCRIPTORS` is
 * negotiated, and `boot_payload_validate()` only checks that the TYPE is
 * known: it constrains WHERE a range sits (overlap, wrap, alignment) and
 * never how big it may be. A malformed or hostile handoff declaring a
 * gigabyte-scale `length` therefore pinned that whole span permanently.
 * Nothing unreserves it, so the boot did not degrade -- it died later in
 * `heap_init` or the first large allocation, naming the allocator rather
 * than the descriptor that starved it.
 *
 * The RANDOM_SEED type was bounded first, by the early-entropy seed
 * payload capability gate; this header generalizes that fix to the whole
 * table.
 *
 * THE INVARIANT WORTH REMEMBERING, and the part that is easy to get wrong:
 * a descriptor the reservation pass DECLINES to pin keeps its
 * `BOOT_PAYLOAD_FLAG_RESERVED` bit set, because the bit is what the
 * PRODUCER asked for and the kernel does not rewrite the handoff. So the
 * flag is a REQUEST, not proof of ownership. Every consumer that treats it
 * as proof must apply the same predicate the pass applied, or it
 * dereferences memory nothing reserved. That exact follow-on defect shipped
 * once already: `canary_seed_desc_ok()` kept its own `length >= 16` check
 * after the pass started skipping seeds, and it runs pre-IDT where a fault
 * has no handler.
 *
 * A MAX OF ZERO MEANS NOT RESERVABLE AT ALL, which is the honest answer for
 * four of the eleven types rather than a guessed number. Three have no
 * producer anywhere in the tree, and `TPM_EVENT_LOG` has one that
 * deliberately does NOT emit a descriptor: the scalar `tpm_event_log`
 * fields are its single retention path, and publishing a RESERVED payload
 * beside them would double-pin the same physical range and fail overlap
 * detection (`src/boot/uefi/bootx64.c`, and the same reasoning is already
 * recorded in `boot_implicit_payload.h`). Zero turns those comments into an
 * enforced rule. Set a real bound in the SAME commit that adds a producer;
 * deciding the number is part of shipping the producer, not a constant to
 * guess ahead of it.
 *
 * PURE BY DESIGN, and typed like the other `include/boot/` shared headers:
 * plain `unsigned long long`, no `<stdint.h>` and no UEFI types, because
 * the bootloader has one set and the kernel has the other and this file is
 * compiled by both. Type NUMBERS are spelled as literals for the same
 * reason `boot_implicit_payload.h` does: the kernel spells them as an enum
 * and the mirror as `#define`s, so each includer static-asserts its own
 * constant against the literal and a divergence becomes a build failure on
 * whichever side drifted.
 *
 * EXHAUSTIVENESS IS NOT THIS HEADER'S JOB. A list cannot notice an enum
 * value nobody added to it. The kernel-side `boot_payload_length_reservable()`
 * switches over an `enum boot_payload_type`-typed value with no `default`,
 * so a new enumerator without an arm fails the build under `-Werror,-Wswitch`
 * -- which only works because the switch expression is ENUM-typed. A switch
 * over the wire `uint32_t` would compile silently and was caught in design
 * review before it shipped.
 */
#ifndef BOOT_PAYLOAD_LIMITS_H
#define BOOT_PAYLOAD_LIMITS_H

/* The per-type contract, in one place.
 *
 * X(type_value, label, min_bytes, max_bytes, reason)
 *   type_value -- the `enum boot_payload_type` NUMBER, as a literal.
 *   label      -- what a human calls it, for the refusal line.
 *   min_bytes  -- smallest length that can be meaningful. 0 where any
 *                 nonzero length is structurally acceptable.
 *   max_bytes  -- largest length the kernel will PIN. 0 means NOT
 *                 RESERVABLE: see the header comment above.
 *   reason     -- where the number comes from. A bound whose provenance is
 *                 not written down is a bound the next person will change
 *                 by feel.
 *
 * BOOT_PAYLOAD_NONE is absent on purpose: it is the empty-slot sentinel and
 * the validator already rejects any NONE slot carrying a nonzero field.
 */
#define BOOT_PAYLOAD_LIMIT_LIST(X)                                             \
    X(1u,  "kernel module",             1ull,  268435456ull,                   \
        "matches BOOT_PAYLOAD_FILE_MAX, the loader's own 256 MiB cap on "      \
        "every boot.conf-staged file; a tighter kernel bound would refuse a "  \
        "payload the loader already allocated and published")                  \
    X(2u,  "initrd / initramfs",        1ull,  268435456ull,                   \
        "same loader cap; INITRD is staged through the identical path")        \
    X(3u,  "recovery image",            1ull,  268435456ull,                   \
        "same loader cap; RECOVERY_IMAGE is staged through the identical "     \
        "path")                                                                \
    X(4u,  "hibernation metadata",      0ull,  0ull,                           \
        "NOT RESERVABLE: no producer emits this descriptor anywhere in the "   \
        "tree. The owning TODO sets a bound in the commit that adds one")      \
    X(5u,  "TPM event log",             0ull,  0ull,                           \
        "NOT RESERVABLE by design, not by omission: the scalar "               \
        "tpm_event_log fields are the single retention path and a RESERVED "   \
        "descriptor beside them would double-pin one range and fail overlap "  \
        "detection")                                                           \
    X(6u,  "network boot config",       0ull,  0ull,                           \
        "NOT RESERVABLE: no producer emits this descriptor anywhere in the "   \
        "tree. The owning TODO sets a bound in the commit that adds one")      \
    X(7u,  "firmware entropy seed",     32ull, 16384ull,                       \
        "the min is sizeof(struct entropy_seed_header) and the max is "        \
        "BOOT_SEED_PAYLOAD_CAP; both are static-asserted against their real "  \
        "definitions on the kernel side")                                      \
    X(8u,  "USB handover state",        0ull,  0ull,                           \
        "NOT RESERVABLE: handover state travels in usb_controller, not in a "  \
        "descriptor. The owning TODO sets a bound in the commit that adds "    \
        "a producer")                                                          \
    X(9u,  "warm-update preserved state", 4096ull, 67108864ull,                \
        "page-multiple by its own consumer contract, so the min is one "       \
        "page; 64 MiB is preserved SUBSYSTEM STATE, which is metadata "        \
        "rather than bulk data, and the outgoing kernel that publishes it "    \
        "has no legitimate reason to exceed it")                               \
    X(10u, "headless enrollment authorization", 152ull, 152ull,                \
        "EXACT length: the blob is a fixed TPM_HEADLESS_BLOB_LEN structure, "  \
        "static-asserted against it on the kernel side, so any other length "  \
        "is a malformed producer rather than a smaller token")

/* Aggregate ceiling on everything the reservation pass may pin from typed
 * payload descriptors, across the whole table.
 *
 * PER-DESCRIPTOR BOUNDS ALONE DO NOT CLOSE THE STARVATION PATH, which is
 * the finding that produced this constant. `BOOT_PAYLOAD_MAX` is 32 and
 * nothing forbids repeated types, so 32 individually-legal 256 MiB
 * descriptors would pin 8 GiB while passing every per-type rule. Bounding
 * the sum is a separate defense from bounding each term, and a section that
 * only did the latter would not have fixed the failure it named.
 *
 * The arithmetic behind 768 MiB: the largest handoff anyone can justify is
 * a maxed initrd plus a maxed recovery image (512 MiB together, and both
 * are singletons) with 256 MiB of headroom left for modules. It is a
 * static number rather than a fraction of RAM because this budget is
 * consumed in Phase 0, before the allocator that would report RAM exists.
 */
#define BOOT_PAYLOAD_RESERVE_TOTAL_MAX  805306368ull   /* 768 MiB */

/* Types that may legitimately appear MORE THAN ONCE in the table.
 *
 * TWO types can, and the second was nearly got wrong. MODULE is the obvious
 * case: a boot loading three drivers stages three MODULE descriptors and
 * that is normal. RANDOM_SEED is the non-obvious case -- it is
 * multi-descriptor BY DOCUMENTED CONTRACT. `bootx64.c` leaves an
 * already-published seed from a prior chain stage untouched and APPENDS its
 * own ("Linux EFI config-table parity"), and `boot_seed.c` digest-chains
 * every occurrence rather than concatenating them, naming an
 * "every-descriptor-mixed contract" sized for 32 descriptors. Making it a
 * singleton would have pinned only the first and silently dropped the
 * loader's fresh firmware and CPU entropy, degrading the CSPRNG for a
 * reason no log would explain.
 *
 * The rest are singletons by meaning -- one initrd, one recovery image, one
 * authorization token, one warm-update region -- so a second occurrence is a
 * malformed or hostile handoff, and pinning it multiplies the memory a
 * single type may claim. Returns 1 when repeats are allowed.
 *
 * This is deliberately NOT a column in the list above: it is a cardinality
 * rule, not a length one, and folding two different defenses into one
 * table is how a later reader comes to believe that satisfying one
 * satisfies the other.
 */
#define BOOT_PAYLOAD_TYPE_IS_REPEATABLE(type_value)  \
    ((type_value) == 1u || (type_value) == 7u)

#endif /* BOOT_PAYLOAD_LIMITS_H */
