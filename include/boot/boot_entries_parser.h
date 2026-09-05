/* boot_entries_parser.h -- bootloader-side parser for the boot entry store (TODO-07).
 *
 * Parses `\EFI\ImpossibleOS\bootentries.json` per the schema in
 * docs/boot/boot-entry-schema.md. Pure C, no UEFI types and no allocations: the
 * caller hands the parser a raw byte buffer + a result struct; the parser walks
 * the bytes and populates the result. This shape lets the same code link into
 * the bootloader (called from bootx64.c after the file read) and the kernel
 * unit-test binary (called from test_boot_entry_parser.c with fixture strings).
 *
 * Owners (in TODO-07 boot-entry-store-menu-policy):
 *   - boot entry parser and validator (this header + boot_entries_parser.c):
 *     JSON walker + envelope validator + CRC verification + path-escape
 *     rejection + fallback synth.
 *   - per-entry-kind handlers consume the payload byte-range slice this parser
 *     stores in each envelope; the parser does NOT validate per-kind fields
 *     (that is the per-entry-kind TODO's hook table).
 *
 * Trust model: see docs/boot/boot-entry-schema.md "Trust Model" (two-tier). The
 * parser is the gate that turns ESP bytes into validated envelopes; under split-
 * path the bootloader signature is the authority + this CRC for corruption
 * detection; under UKI mode the store is advisory.
 *
 * Iterative + bounded: no recursion (depth-bomb safe). Token + value parsing
 * walks a fixed-size depth stack capped at BOOT_ENTRIES_MAX_PARSE_DEPTH. Hostile
 * input (deep nesting, malformed JSON) -> clean reject + fallback, never stack
 * overflow.
 */

#ifndef BOOT_ENTRIES_PARSER_H
#define BOOT_ENTRIES_PARSER_H

#include "boot_entries.h"

/* Maximum JSON nesting depth the parser will walk. The schema's load-bearing
 * shape is `{ schema_version, crc32, entries: [ { ..., flags: [...], policy_tags:
 * [...], payload: { ... } }, ... ] }` -- nesting of 5 covers it; 8 leaves room
 * for future optional sub-objects. Reject before stack damage. */
#define BOOT_ENTRIES_MAX_PARSE_DEPTH 8u

/* Reject reason buffer length. Short, fits in serial output cleanly. */
#define BOOT_ENTRIES_REJECT_MSG_LEN 96u

/* Parse rejection codes. Returned in boot_entries_parse_result.reject_code so
 * the caller can map to a serial line and a fallback selection_reason later
 * when the policy merge layer (boot policy merge order TODO) plumbs it.
 */
typedef enum {
    BOOT_ENTRIES_OK = 0,
    BOOT_ENTRIES_REJECT_FILE_TOO_LARGE,
    BOOT_ENTRIES_REJECT_JSON_PARSE,
    BOOT_ENTRIES_REJECT_DEPTH_LIMIT,
    BOOT_ENTRIES_REJECT_NOT_OBJECT,
    BOOT_ENTRIES_REJECT_MISSING_FIELD,
    BOOT_ENTRIES_REJECT_BAD_SCHEMA_VERSION,
    BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD,
    BOOT_ENTRIES_REJECT_CRC_MISMATCH,
    BOOT_ENTRIES_REJECT_NOT_ARRAY,
    BOOT_ENTRIES_REJECT_NO_ENTRIES,
    BOOT_ENTRIES_REJECT_TOO_MANY_ENTRIES,
    BOOT_ENTRIES_REJECT_DUPLICATE_ID,
    BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
    BOOT_ENTRIES_REJECT_BAD_ID,
    BOOT_ENTRIES_REJECT_BAD_TITLE,
    BOOT_ENTRIES_REJECT_BAD_FLAGS,
    BOOT_ENTRIES_REJECT_UNKNOWN_KIND_RANGE,
    BOOT_ENTRIES_REJECT_PATH_ESCAPE,
    BOOT_ENTRIES_REJECT_INTERNAL,
    /* Appended (not inserted) so every value above keeps the number a shipped
     * reject record already carries. */
    BOOT_ENTRIES_REJECT_DUPLICATE_KEY,   /* same key twice in one object */
    BOOT_ENTRIES_REJECT_ESCAPED_KEY,     /* key name spelled with a JSON escape */
} boot_entries_reject_code_t;

/* Parsed envelope for one entry. Fields are NUL-terminated where applicable.
 * The numeric kind covers the stable + vendor + reserved-future ranges per
 * boot_entries.h; per-kind payload validation is deferred to the per-entry-
 * kind TODO's handler table, which consumes payload_offset / payload_length
 * as a byte-range slice into the original raw buffer.
 */
/* Caps for the health_check_subset override -- shared with the cross-boot
 * handoff record in include/boot/boot_health_handoff.h. Geometry chosen
 * so the bootloader can copy entries by name without re-marshalling. */
#define BOOT_ENTRIES_HEALTH_SUBSET_NAME_LEN 24u
#define BOOT_ENTRIES_HEALTH_SUBSET_MAX_NAMES 8u

typedef struct {
    char id[64];                 /* up to BOOT_ENTRIES_MAX_ID_LEN (47) + NUL + slack */
    char title[80];              /* up to BOOT_ENTRIES_MAX_TITLE_LEN (63) + NUL + slack */
    char sort_key[64];           /* BLS-style sort string; up to BOOT_ENTRIES_MAX_SORT_KEY_LEN + NUL */
    char machine_id[40];         /* RFC 4122 textual UUID (36 chars) + NUL + slack */
    char policy_tags[BOOT_ENTRIES_MAX_POLICY_TAGS][BOOT_ENTRIES_MAX_POLICY_TAG_LEN + 1u];
    unsigned int policy_tag_count;    /* 0..BOOT_ENTRIES_MAX_POLICY_TAGS retained */
    unsigned int policy_tag_overflow; /* 1 if JSON had more tags than the cap */
    /* Optional per-entry health-check subset. When
     * non-empty, restricts the kernel's health-gate run to the named
     * checks (intersection with registry). Absent / empty -> kernel runs
     * the full default check set. Names are matched by exact string
     * comparison; unknown names are ignored with a warning so adding a
     * new check name later does not retroactively invalidate stores. */
    char health_check_subset[BOOT_ENTRIES_HEALTH_SUBSET_MAX_NAMES]
                             [BOOT_ENTRIES_HEALTH_SUBSET_NAME_LEN];
    unsigned int health_check_subset_count;
    unsigned int kind;           /* boot_entry_kind_t numeric value */
    unsigned int flags;          /* OR of BOOT_ENTRY_FLAG_* */
    unsigned int timeout_override;    /* 0..600 from JSON, BOOT_ENTRIES_TIMEOUT_OVERRIDE_NONE if absent */
    unsigned int payload_offset; /* byte offset of the payload object's `{` in raw input */
    unsigned int payload_length; /* byte length of payload object including braces */
    int payload_present;         /* 1 if payload_offset/length valid; 0 if entry has no payload object */
    int kind_skipped;            /* 1 if kind was vendor/reserved-future and skipped-with-warn */
} boot_entry_envelope_t;

typedef struct {
    /* Outcome. */
    boot_entries_reject_code_t reject_code;
    char reject_msg[BOOT_ENTRIES_REJECT_MSG_LEN];

    /* Parsed envelopes. Valid only when reject_code == BOOT_ENTRIES_OK.
     * entry_count may be less than the number of objects in the input array
     * if some kinds were skipped-with-warn (vendor/reserved-future range);
     * the skipped count is recorded separately. */
    boot_entry_envelope_t entries[BOOT_ENTRIES_MAX_ENTRIES];
    unsigned int entry_count;
    unsigned int skipped_count;

    /* CRC computed from the raw input (with crc32 field zeroed). Filled even
     * on REJECT_CRC_MISMATCH so the caller can log both expected and actual.
     */
    unsigned int computed_crc;
    unsigned int header_crc;
} boot_entries_parse_result_t;

/* Logging callback: bootloader passes serial_early_print; kernel-side tests pass
 * a printf-via-klog or NULL to silence. NULL is acceptable; the parser is well-
 * behaved without a logger.
 */
typedef void (*boot_entries_log_fn)(const char *);

/* Parse + validate a boot entry store. Caller-provided `out` is fully populated;
 * on rejection, `out->entries` is left in an indeterminate state (entry_count is
 * meaningful on REJECT_OK only).
 *
 * `secure_boot_active` is a forward-looking parameter retained for future
 * grammar gates (e.g. additional payload-field shape requirements that depend
 * on Secure Boot state); it is currently a no-op at the parser layer.
 *
 * IMPORTANT: Path-escape (chainload + Secure Boot + missing trusted_chainload
 * flag) is NOT enforced by the parser. It is the boot-policy filter's
 * responsibility (see boot_policy_decide() with BOOT_REJECT_REASON_PATH_ESCAPE
 * in include/boot/boot_policy.h). The parser passes untrusted chainload
 * entries through so other viable entries are not killed by one bad entry --
 * per-entry demote rather than whole-store reject. Callers MUST run every
 * parsed entry through boot_policy_decide() before treating it as bootable;
 * BOOT_ENTRIES_OK is "store grammar accepted", not "every entry safe to
 * launch".
 *
 * Returns 0 on success (== BOOT_ENTRIES_OK), nonzero on rejection.
 */
int boot_entries_parse(
    const unsigned char *raw,
    unsigned int raw_len,
    int secure_boot_active,
    boot_entries_log_fn log,
    boot_entries_parse_result_t *out
);

/* Synthesize the in-firmware fallback entry per the file format fallback contract
 * (boot entry file format TODO):
 *   - uki_mode != 0:   kind=uki, id="fallback", title="Impossible OS (fallback)"
 *   - uki_mode == 0:   kind=split, id="fallback", title="Impossible OS (fallback)"
 * Caller fills payload_offset/length to 0; payload_present=0 (the bootloader
 * synthesizes the payload at handoff time, not from this struct). Used when
 * boot_entries_parse() rejects.
 */
void boot_entries_synthesize_fallback(
    int uki_mode,
    boot_entry_envelope_t *out
);

/* Boot Loader Specification display-order comparator: returns nonzero iff
 * envelope `a` sorts before `b`. Keys in priority order: sort_key, then
 * machine_id, then id (stable tiebreak). This is the subset of the BLS
 * display order the parse result can express; "bad-counted last" + "version"
 * sub-keys are tracked BLI-parity follow-ups in the boot entry store TODO. */
int boot_entry_bls_less(const boot_entry_envelope_t *a,
                        const boot_entry_envelope_t *b);

/* Stable in-place BLS sort of the index array idx[0..n-1] into
 * parse->entries[]. Insertion sort (n is bounded by BOOT_ENTRIES_MAX_ENTRIES
 * / BOOT_MENU_MAX_VISIBLE). Shared by the on-screen menu candidate list and
 * the LoaderEntries variable so bootctl and the menu agree on order. */
void boot_entries_bls_sort(const boot_entries_parse_result_t *parse,
                           unsigned int *idx, unsigned int n);

#endif /* BOOT_ENTRIES_PARSER_H */
