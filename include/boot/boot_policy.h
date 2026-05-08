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
 * tracked entry. Updates use a CRASH-TOLERANT protocol:
 *     write-new + Flush() + Close(success) + delete-old.
 * The new file is treated as durable ONLY when BOTH Flush() and Close()
 * return EFI_SUCCESS; if either fails, delete-old is SKIPPED and the old
 * file is preserved. UEFI EFI_FILE_PROTOCOL.SetInfo rename is NOT power-
 * fail-atomic on FAT32 -- LFN entries can span multiple directory entries
 * and a reset mid-rename can leave torn names, duplicates, or orphaned old
 * names. The scan path resolves duplicates conservatively (lowest
 * tries_left + highest tries_done = worst-case demotion) so a torn rename
 * never silently ignores an apparent exhaustion record. Counters are NOT
 * NVRAM and NOT in the CRC-pinned bootentries.json store.
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

    /* Local-machine UUID for the machine_id filter. Producer is the
     * SMBIOS table 1 (System Information) UUID extraction owned by the
     * loader-variables feature -- until that ships, callers leave this
     * empty and the filter treats every entry with a non-empty
     * machine_id as a mismatch. An empty machine_id field on an
     * envelope is the documented wildcard ("match any machine"). */
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

    /* Bitmask of stable kinds the caller can actually execute on the
     * current load path. Bit (1u << BOOT_ENTRY_KIND_X) set means the
     * caller has a working loader for that kind; cleared means the
     * caller would have to refuse the handoff if this kind were chosen.
     * The filter rejects entries whose kind is unsupported with
     * BOOT_REJECT_REASON_KIND_UNAVAILABLE so the ladder picks something
     * the caller can finish booting.
     *
     * Contract:
     *   - mask == 0 -> gate DISABLED (back-compat for the pure-C unit
     *     tests that pre-date this field).
     *   - mask != 0 -> closed-mask: kind >= 32 always rejects (vendor /
     *     reserved-future ranges fail closed even if the parser stops
     *     setting kind_skipped).
     *   - mask != 0, kind < 32, bit clear -> rejects with
     *     KIND_UNAVAILABLE.
     *
     * Today's bootloader (boot_policy_invoke()) sets EXACTLY ONE bit
     * based on the launch mode: BOOT_ENTRY_KIND_UKI when
     * invoked_via_uki=1, BOOT_ENTRY_KIND_SPLIT otherwise. Cross-mode
     * selection would lie to the kernel about which payload was
     * loaded (load_kernel is mode-locked at detect_uki_sections time
     * and cannot switch modes from a policy decision), so the gate
     * filters those entries out.
     *
     * Future owner sections widen the mask one kind at a time, ONLY
     * when their load path can fully execute the selected entry's
     * payload: SAFE/TEST/DIAGNOSTICS need boot_config materialization
     * from entry flags; RECOVERY needs the recovery-partition load
     * path; INSTALLER needs a distinct installer-image load path plus offline + first-install seeding;
     * CHAINLOAD/NETWORK/RESUME need the per-entry-kind handler table.
     * Each owner section's TODO has a checklist item to widen this
     * mask. */
    unsigned int supported_kinds_mask;
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
 * select). The decrement happens AFTER the ladder picks the entry and
 * BEFORE the kernel handoff so a hard reset mid-boot doesn't double-
 * decrement; the protocol is:
 *   1. Open(new_filename, CREATE) -> Flush() -> Close(). The new file is
 *      treated as durable ONLY when BOTH Flush() and Close() return
 *      EFI_SUCCESS.
 *   2. ONLY on confirmed durability: Open(old_filename) + Delete(). If
 *      either Flush or Close failed in step 1, step 2 is skipped and the
 *      old file is preserved.
 * NOT atomic rename. A reset between step 1 and step 2 leaves both files;
 * the conservative duplicate resolution (lowest tries_left + highest
 * tries_done) resolves to the worst-case state on next boot, never
 * silently ignoring an exhaustion record.
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

/* Conservative duplicate dedupe helper. Append `cand` into `out[]` if
 * its id is not already present; if it IS present (torn-rename
 * duplicate), merge into the existing slot with worst-case demotion:
 * retain min(tries_left) AND max(tries_done).
 *
 * Tri-state return:
 *   +1 = appended (new id, slot filled, *count incremented)
 *    0 = merged into existing slot (same id seen twice; *count unchanged)
 *   -1 = cap-full reject (new id, but *count == cap; *count unchanged)
 *
 * The tri-state distinction matters at production scan sites: a torn
 * rename that produces a duplicate of an existing id MUST be allowed
 * to merge even when *count == cap (the slot for that id is already
 * occupied). Only a NEW id arriving at cap-full counts as overflow.
 * The earlier 0/1 binary return conflated merged-at-cap with rejected-
 * at-cap, which forced a user-visible boot denial after a power loss
 * at exactly the boundary this dedupe is meant to recover from.
 *
 * Pure C; no UEFI types. Cross-includable from the kernel test runner
 * so the merge invariant is unit-testable -- the bootloader's
 * policy_scan_counters() is the production caller, but the actual
 * worst-case merge logic must be exercised against raw torn-duplicate
 * fixtures (foo+1-3 + foo+0-4 -> tries_left=0, tries_done=4) before
 * the ladder filter runs. */
int boot_policy_counter_dedup_insert(boot_counter_t *out,
                                     unsigned int *count,
                                     unsigned int cap,
                                     const boot_counter_t *cand);

/* ---- Boot menu pure logic helpers -------------------------------------
 * Render + input-loop UEFI glue lives in bootx64.c. The two pure-C
 * helpers below are factored out so kernel tests can exercise the
 * filtering and visibility rules without UEFI / firmware mocks.
 *
 * boot_policy_menu_should_show:
 *   Returns 1 iff the policy ladder picked a "soft" entry (the user
 *   has a meaningful choice between candidates) AND >=2 viable
 *   candidates remain. Forced-selection reasons (HOTKEY / WATCHDOG /
 *   AB_TRY_STATE / RECOVERY_REQUEST / FALLBACK_*) all return 0.
 *
 * boot_policy_menu_collect:
 *   Walks parse->entries[] and produces an index list of viable
 *   candidates: not kind_skipped, not flagged HIDDEN, not in
 *   decision->rejected[]. Returns the count; sets *out_default_idx to
 *   the index in out_idx[] whose envelope id matches
 *   decision->selected_entry_id, or BOOT_POLICY_MENU_DEFAULT_NOT_FOUND
 *   when the selected entry is NOT representable within `cap` (e.g.
 *   it lives beyond the cap or has been filtered out). Callers MUST
 *   suppress the menu in that case -- otherwise the visible default
 *   highlight would diverge from the actual boot decision, and a
 *   timeout / Enter would boot a different entry than the user sees.
 *   `cap` is the visible-row limit (BOOT_MENU_MAX_VISIBLE in the
 *   bootloader; tests pass smaller values to exercise the cap).
 */
#define BOOT_POLICY_MENU_DEFAULT_NOT_FOUND  0xFFFFFFFFu
int boot_policy_menu_should_show(const boot_policy_decision_t *decision,
                                 const boot_entries_parse_result_t *parse,
                                 unsigned int viable_count);

unsigned int boot_policy_menu_collect(
    const boot_entries_parse_result_t *parse,
    const boot_policy_decision_t *decision,
    unsigned int *out_idx, unsigned int cap,
    unsigned int *out_default_idx);

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

/* ---- Selected-kind to (boot_path, boot_reason, src_flag_add) override ---
 * Pure mapping table for the post-handoff boot-decision populate block.
 * For path-changing kinds (recovery / diagnostics / network / resume)
 * returns 1 and fills `out` with raw uint values that match the kernel-
 * side enums in include/kernel/boot_info.h. The static_asserts in the
 * bootloader's populate-block call site pin those raw values to the
 * kernel enum, so a future enum reorder triggers a build error before
 * the mapping silently drifts. For non-path-changing kinds (split / uki
 * / installer / safe / test / chainload / vendor / reserved-future)
 * returns 0 and leaves the caller's boot_path / boot_reason / source
 * flags unmodified (a media-role override, if any, stands).
 *
 * Pure C, no UEFI types -- cross-includable from the kernel test runner
 * the same way boot_policy_decide() is. The raw-uint outputs decouple
 * the bootloader pure-C surface from kernel headers so the build layer
 * boundary stays clean.
 */
typedef struct {
    unsigned int boot_path;     /* enum boot_path_type raw value */
    unsigned int boot_reason;   /* enum boot_reason_code raw value */
    unsigned int src_flag_add;  /* BOOT_SOURCE_FLAG_* bits to OR in */
} boot_policy_path_override_t;

int boot_policy_kind_to_path(unsigned int kind,
                             boot_policy_path_override_t *out);

/* Raw-value constants the helper returns. Pinned to the kernel-side
 * enum values via static_assert at the call site in bootx64.c. Listed
 * here so the test runner can assert against the same constants. */
#define BOOT_POLICY_PATH_RECOVERY       3u
#define BOOT_POLICY_PATH_NETWORK        4u
#define BOOT_POLICY_PATH_RESUME         5u
#define BOOT_POLICY_PATH_DIAGNOSTIC     7u
#define BOOT_POLICY_REASON_USER_SELECTED        2u
#define BOOT_POLICY_REASON_RECOVERY_TRIGGER     9u
#define BOOT_POLICY_REASON_DIAGNOSTIC_REQUEST  11u
#define BOOT_POLICY_SRC_FLAG_RECOVERY_TRIGGERED (1u << 5)

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
