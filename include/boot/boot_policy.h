/*
 * include/boot/boot_policy.h -- Boot policy merge order (boot-entry policy feature)
 *
 * Pure-C public API for the boot-entry selection ladder. The ladder runs
 * AFTER the boot-entries parser has produced a `boot_entries_parse_result_t`,
 * and BEFORE the bootloader hands off to the chosen kernel image.
 *
 * Two layers, deterministically composed:
 *   FIRMWARE LAYER -- read-only inputs from UEFI: BootCurrent / BootNext /
 *     BootOrder. The firmware has already deleted BootNext before our
 *     bootloader runs (UEFI 2.10 spec section 3.1.5), so we observe the
 *     result via BootCurrent. These inputs live in boot_info.uefi_boot_*
 *     (populated by the existing UEFI boot-variable read) and feed the OS
 *     layer below.
 *   OS LAYER -- once a Boot#### is selected by firmware, this module picks
 *     the internal entry id from the parsed store. Win11's BCD merge and
 *     Linux's per-loader rules conflate these layers; documenting the
 *     split is where Impossible OS earns its star.
 *
 * The ladder priority (highest first):
 *   1. hotkey         -- operator pressed F8/F9 at the menu (input wiring
 *                        owned by the boot-menu feature)
 *   2. watchdog       -- BlackBox sticky rollback flag (input owned by the
 *                        watchdog/policy-audit feature)
 *   3. ab_try_state   -- A/B slot try-state selection (input owned by the
 *                        A/B + recovery integration feature)
 *   4. recovery       -- recovery request asserted (input owned by the
 *                        A/B + recovery integration feature)
 *   5. store_default  -- default = first ACTIVE non-hidden non-skipped
 *                        entry whose machine_id matches the local machine
 *                        (or empty machine_id), tiebreak by sort_key
 *                        ascending then array order. BootNext is a soft
 *                        hint here: it BIASES the ladder by promoting the
 *                        BootCurrent-resolved entry, but never overrides
 *                        watchdog / A/B / recovery.
 *   6. fallback       -- the parser's synthesized fallback envelope
 *                        (always wins when the parsed store is invalid or
 *                        empty)
 *
 * Counters (tries_left / tries_done) live as BLS-style filename state at
 * \EFI\ImpossibleOS\counters\<entry-id>+<L>-<D> -- one zero-byte file per
 * tracked entry. Updates use FAT32 atomic rename (single-cluster directory
 * entry write). Counters are NOT NVRAM and NOT in the CRC-pinned
 * bootentries.json store.
 */

#ifndef BOOT_BOOT_POLICY_H
#define BOOT_BOOT_POLICY_H

#include "boot_entries.h"
#include "boot_entries_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Selection reason axis ----------------------------------------------
 * The policy-ladder result. Distinct from `enum boot_reason_code` in
 * include/kernel/boot_info.h, which is the path-flow axis. Both are set;
 * the path-flow reason is updated only when the chosen entry implies a
 * different boot_path (recovery, diagnostics, network, resume).
 */
typedef enum {
    BOOT_SELECTION_UNSET                = 0,  /* sentinel: producer must overwrite */
    BOOT_SELECTION_STORE_DEFAULT        = 1,  /* parsed-store default chosen */
    BOOT_SELECTION_BOOTNEXT_HINT        = 2,  /* BootNext+OptionalData mapped to a store id */
    BOOT_SELECTION_HOTKEY               = 3,  /* operator F8/F9 (menu input) */
    BOOT_SELECTION_WATCHDOG_ROLLBACK    = 4,  /* BlackBox sticky watchdog rollback */
    BOOT_SELECTION_AB_TRY_STATE         = 5,  /* A/B try-state picked the entry */
    BOOT_SELECTION_RECOVERY_REQUEST     = 6,  /* recovery request asserted */
    BOOT_SELECTION_FALLBACK_NO_VIABLE   = 7,  /* parsed store had no viable active entry */
    BOOT_SELECTION_FALLBACK_STORE_INVALID = 8, /* parser rejected the store outright */
    BOOT_SELECTION_UNKNOWN_BOOTCURRENT  = 9,  /* BootCurrent did not map to any known entry */
} boot_selection_reason_t;

#define BOOT_SELECTION_REASON_MAX  BOOT_SELECTION_UNKNOWN_BOOTCURRENT

/* ---- Per-entry reject reason --------------------------------------------
 * Recorded in boot_info.rejected_entries[] for entries the policy ladder
 * filtered OUT (independent of parser-level rejects which fail the whole
 * store). Mirror in the kernel registry via the post-boot diagnostics
 * surface in the policy-audit + loader-variables features.
 */
typedef enum {
    BOOT_REJECT_REASON_NONE                 = 0,  /* sentinel; never written */
    BOOT_REJECT_REASON_KIND_SKIPPED         = 1,  /* envelope.kind_skipped (vendor / reserved-future) */
    BOOT_REJECT_REASON_NOT_ACTIVE           = 2,  /* missing BOOT_ENTRY_FLAG_ACTIVE */
    BOOT_REJECT_REASON_HIDDEN               = 3,  /* BOOT_ENTRY_FLAG_HIDDEN set; menu-only filter */
    BOOT_REJECT_REASON_MACHINE_ID_MISMATCH  = 4,  /* machine_id non-empty + did not match local */
    BOOT_REJECT_REASON_TRIES_EXHAUSTED      = 5,  /* counters file showed tries_left == 0 */
    BOOT_REJECT_REASON_PATH_ESCAPE          = 6,  /* chainload+secure_boot without trusted_chainload */
    BOOT_REJECT_REASON_KIND_UNAVAILABLE     = 7,  /* per-kind handler reports the input is invalid */
} boot_reject_reason_t;

#define BOOT_REJECT_REASON_MAX  BOOT_REJECT_REASON_KIND_UNAVAILABLE

/* ---- Inputs ---------------------------------------------------------------
 * The ladder is a pure function from these inputs to a decision. Sections
 * that own a layer (menu hotkey, watchdog, A/B + recovery integration)
 * fill the matching field; otherwise leave the default (no override).
 */
typedef struct {
    /* Firmware-layer mirror of boot_info.uefi_boot_* (read-only diagnostic). */
    unsigned int boot_current;          /* BootCurrent value, 0xFFFF = absent */
    unsigned int boot_next;             /* BootNext value, ignored if !boot_next_valid */
    int          boot_next_valid;       /* 1 if BootNext was present in NVRAM */

    /* Resolved BootCurrent -> internal entry id. Empty string if BootCurrent
     * was absent OR Boot####.OptionalData lacked the IPOS\1 tag. The ladder
     * uses this to bias `store_default` toward the firmware-selected entry. */
    char         bootcurrent_entry_id[64];
    int          bootcurrent_known;     /* 1 if the resolution succeeded */

    /* Local-machine UUID for the machine_id filter. Producer is the existing
     * boot_info.smbios_system_uuid path; an empty machine_id field on an
     * envelope means "match any machine". */
    char         local_machine_id[40];  /* 36-char UUID + NUL + slack, "" if absent */

    /* Operator hotkey override (input owned by the boot-menu feature).
     * 0 = no hotkey, otherwise the 1-based menu index the operator picked. */
    unsigned int hotkey_override_index;

    /* Watchdog rollback request (input owned by the watchdog feature).
     * 1 = roll back to the previous-good entry (resolution: skip the most-
     * recently-attempted entry; choose the next viable). */
    int          watchdog_rollback;

    /* A/B try-state (input owned by the A/B + recovery integration feature).
     * `ab_slot_valid` MUST be 1 for the ladder to consider this branch --
     * BSS-zero default (valid=0) means "no A/B applies", so a freshly-
     * AllocateZeroPool'd inputs struct does NOT silently fire the A/B
     * priority with slot 0. When valid=1, ab_slot_index is the 0-based
     * slot index the prober picked. The legacy BOOT_POLICY_NO_AB_SLOT
     * sentinel below is preserved for callers that want to express
     * "definitely no A/B" via a sentinel value AND keeps backwards
     * compatibility with code that sets only ab_slot_index. */
    unsigned int ab_slot_valid;
    unsigned int ab_slot_index;

    /* Recovery request (input owned by the A/B + recovery integration
     * feature). 1 = enter recovery; loader picks the first KIND_RECOVERY
     * entry. */
    int          recovery_requested;

    /* Path-escape gate (forwarded from the parser's secure_boot_active). */
    int          secure_boot_active;

    /* UKI invocation context. When the bootloader was launched via the
     * Unified Kernel Image path (BOOT_FLAG_INVOKED_VIA_UKI), this flag
     * MUST be 1 so FALLBACK_NO_VIABLE synthesizes a kind=uki envelope
     * instead of the default split-path one. Default 0 (split). */
    int          invoked_via_uki;

    /* Counter-scan overflow signal. The bootloader-side counter directory
     * scan caps its array at BOOT_ENTRIES_MAX_ENTRIES; if the scan saw
     * more files than the cap, it sets this flag. The ladder then fails
     * closed (returns FALLBACK_NO_VIABLE) rather than risk booting an
     * entry whose tries-exhausted record was dropped beyond the cap by a
     * hostile or stale ESP. */
    int          counters_overflow;
} boot_policy_inputs_t;

#define BOOT_POLICY_NO_AB_SLOT  0xFFFFu

/* ---- Decision -----------------------------------------------------------
 * The ladder writes ONE selected envelope plus a packed-prefix array of
 * rejected entries. Parallel to the boot_info shape; bootx64.c copies
 * fields out of here into boot_info pre-handoff.
 */
typedef struct {
    char            selected_entry_id[64];
    boot_selection_reason_t reason;

    /* The chosen envelope, deep-copied from the parser result so the caller
     * does not need to keep a pointer into the parsed entries array. Valid
     * only when reason != BOOT_SELECTION_UNSET AND
     * reason != BOOT_SELECTION_FALLBACK_STORE_INVALID -- in those cases the
     * caller falls back to boot_entries_synthesize_fallback().*/
    boot_entry_envelope_t selected;

    struct {
        char id[64];
        unsigned int reason;            /* boot_reject_reason_t */
    } rejected[BOOT_ENTRIES_MAX_ENTRIES];
    unsigned int rejected_count;
    unsigned int rejected_overflow;     /* 1 if more than MAX_ENTRIES rejects observed */
} boot_policy_decision_t;

/* ---- Counters (BLS-style filename state) --------------------------------
 * Parsed from a directory listing; the bootloader-side glue scans
 * \EFI\ImpossibleOS\counters\, hands each filename here, and the result
 * cooperates with the ladder's tries_exhausted gate.
 *
 * Filename grammar:  <entry-id>+<L>-<D>
 *   entry-id : kebab-case 1..47 chars (boot-entries id grammar)
 *   L        : tries_left as decimal 0..9
 *   D        : tries_done as decimal 0..99
 *
 * `tries_left=0` means the entry has been demoted (still visible, no auto-
 * select). The decrement-rename happens AFTER the ladder picks the entry
 * and BEFORE the kernel handoff so a hard reset mid-boot doesn't double-
 * decrement; the rename is FAT32 single-cluster atomic so the worst
 * post-crash state is the old filename remaining (counters un-decremented,
 * which the next boot will detect as a stale try).
 */
typedef struct {
    char         id[64];
    unsigned int tries_left;
    unsigned int tries_done;
} boot_counter_t;

/* Parse one filename of the form `<entry-id>+<L>-<D>` (NO directory prefix,
 * NO suffix, length terminated). Returns 1 on success and fills `out`; 0 if
 * the filename is not well-formed. Does NOT validate against the parsed
 * store (different entry ids may be tracked from a previous store version).
 */
int  boot_counter_parse_filename(const char *name, unsigned int name_len,
                                 boot_counter_t *out);

/* Format a counter into a filename in `out` (NUL-terminated). Returns the
 * written length not counting the NUL. `out_cap` must be at least
 * BOOT_COUNTER_FILENAME_MAX bytes. Aborts via return 0 if out_cap is too
 * small or values exceed the cap.
 */
unsigned int boot_counter_format_filename(const boot_counter_t *c,
                                          char *out, unsigned int out_cap);

#define BOOT_COUNTER_FILENAME_MAX  64u   /* 47 + 1 + 1 + 1 + 2 + NUL + slack */
#define BOOT_COUNTER_TRIES_LEFT_MAX  9u
#define BOOT_COUNTER_TRIES_DONE_MAX  99u

/* ---- Ladder entrypoint --------------------------------------------------
 * Pure function. Inputs are read-only; out is fully populated. Always
 * returns one of BOOT_SELECTION_* in `out->reason`; never returns UNSET on
 * a successful call.
 *
 * `parse` may have reject_code != BOOT_ENTRIES_OK -- that's the
 * STORE_INVALID branch: the ladder writes reason=FALLBACK_STORE_INVALID
 * and the caller should synthesize the fallback envelope via
 * boot_entries_synthesize_fallback().
 *
 * `counters` is an array of length `counter_count` (may be 0). The ladder
 * gates `store_default` and the BootNext-hint promotion through
 * tries_exhausted; counters absent for a given entry id => tries_left
 * treated as max (no gate).
 */
void boot_policy_decide(const boot_policy_inputs_t *inputs,
                        const boot_entries_parse_result_t *parse,
                        const boot_counter_t *counters,
                        unsigned int counter_count,
                        boot_policy_decision_t *out);

/* ---- Result-size discipline (pre-EBS allocation) -----------------------
 * boot_policy_decision_t carries an inline 64-entry rejected[] array plus
 * a deep-copied selected envelope, so it is several KiB. The bootloader
 * MUST allocate it (and the upstream boot_entries_parse_result_t) via
 * gBS->AllocatePool, NOT as an automatic local on the pre-EBS stack.
 * The static_asserts below pin the upper bound so a future envelope
 * widening cannot silently push a stack-allocating caller into stack
 * exhaustion.
 */
_Static_assert(sizeof(boot_policy_decision_t) <= 8u * 1024u,
               "boot_policy_decision_t exceeds 8 KiB; callers must use AllocatePool");
_Static_assert(sizeof(boot_policy_inputs_t) <= 256u,
               "boot_policy_inputs_t grew unexpectedly; review BSS-zero defaults");

#ifdef __cplusplus
}
#endif

#endif /* BOOT_BOOT_POLICY_H */
