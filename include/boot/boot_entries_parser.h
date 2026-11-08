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
    /* Where the parse stopped (1-based, in bytes), near the offending token.
     * reject_line is 0 when the whole file is at fault (size, checksum). */
    unsigned int reject_line;
    unsigned int reject_column;

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

/* Nonzero when a store of `size` bytes is within the size cap. The cap is
 * inclusive (BOOT_ENTRIES_MAX_TOTAL_BYTES itself is accepted). The ONE
 * comparison both the loader's pre-read check in bootx64.c and
 * boot_entries_parse() use, so the kernel boundary tests cover both. */
int boot_entries_size_within_cap(unsigned long long size);

/* Short stable token for a reject code ("CRC_MISMATCH", "PATH_ESCAPE", ...).
 * Never NULL; an out-of-range code returns "UNKNOWN". Mirrors
 * boot_entry_kind_reject_name() so both reject vocabularies print the same
 * way on serial and on the rejected-store notice. */
const char *boot_entries_reject_name(int code);

/* One-line human cause for a reject code, phrased for a user standing at the
 * machine rather than a developer reading a log ("the store's checksum does
 * not match its contents"). Never NULL. Kept short so the bootloader can
 * render it on one line of the ConOut fallback. */
const char *boot_entries_reject_cause(int code);

/* The reject name plus its position, when there is one: "JSON_PARSE at line 12,
 * col 3". Always NUL-terminated; cap includes the NUL and a short buf truncates.
 * It goes on the notice's short token line, because the 800x600 banner's cause
 * line has no room for it. BOOT_ENTRIES_REJECT_LOCATOR_LEN holds every name with
 * any position the parser can produce (a store is at most 16 KiB, so 5 digits
 * each), and fits that line. A token rendered from a STORED record carries
 * untrusted u32 positions and uses BOOT_STORE_REJECT_TOKEN_LEN instead. */
#define BOOT_ENTRIES_REJECT_LOCATOR_LEN 48u
void boot_entries_reject_locator(const boot_entries_parse_result_t *r, char *buf, unsigned int cap);

/* Why the store could not be handed to the parser. Kept beside the reject
 * codes so "why is the store unusable" has one answer set spanning both the
 * load step (UEFI file I/O, bootloader-side) and the parse step.
 *
 * ABSENT is deliberately narrow: a filesystem was searched and the file is not
 * there. UNINSPECTED is the boot device with no filesystem at all (PXE / ramdisk),
 * equally silent, but it proves nothing about a store elsewhere and so must not
 * clear the BootStoreReject record. Everything else is UNREADABLE, because a store that exists and cannot be read is a failure
 * the user must see, while a missing store is normal -- the ESP staging in
 * the Makefile copies only boot.conf, so every dev and smoke image ships
 * without one. */
typedef enum {
    BOOT_STORE_LOAD_OK = 0,
    BOOT_STORE_LOAD_ABSENT,      /* no store here, and that is normal */
    BOOT_STORE_LOAD_UNREADABLE,  /* a store exists (or the volume broke) */
    BOOT_STORE_LOAD_UNINSPECTED, /* no filesystem on the boot device to search */
} boot_store_load_status_t;

/* Does this (load_status, reject_code) pair warrant a user-visible notice?
 *
 * This is the false-positive gate for the rejected-store notice, split out
 * as a pure predicate so it can be proven from the kernel tests without a
 * framebuffer or a UEFI volume. Returns 1 only when a store was actually
 * there and could not be used. A missing store returns 0 no matter what
 * reject code the caller synthesized for the policy ladder. */
int boot_entries_store_notice_warranted(int load_status, int reject_code);

/* The `BootStoreReject` UEFI variable: the one durable record of a store the
 * loader could not use. A rejection recurs on every boot until the user fixes
 * the file, so unlike a fatal it may not write on each occurrence: the record
 * is written only when it differs from the stored one and deleted once the
 * store is good again. It lives outside the BootErrorHistory ring so a corrupt
 * store cannot evict the unrelated errors that ring exists to keep, and it
 * holds no timestamp because a field that changes per boot would force the
 * write the comparison exists to avoid. */
#define BOOT_STORE_REJECT_LOAD_ALLOC_FAILED 0xFFFFu /* policy never reached the store */
#define BOOT_STORE_REJECT_ERR_CODE 0x001Au /* efi.h BOOT_ERR_STORE_REJECTED; no kernel view of efi.h */

typedef struct {
    unsigned int   err_code;    /* BOOT_ERR_STORE_REJECTED */
    unsigned short load_status; /* boot_store_load_status_t, or ..._ALLOC_FAILED */
    unsigned short reject_code; /* boot_entries_reject_code_t; 0 if the parser never ran */
    unsigned int   line;        /* 1-based, 0 = no position */
    unsigned int   column;
} boot_store_reject_record_t;

_Static_assert(sizeof(boot_store_reject_record_t) == 16,
               "BootStoreReject is a persisted NVRAM layout");

typedef enum {
    BOOT_STORE_RECORD_ABSENT = 0, /* firmware reports no such variable */
    BOOT_STORE_RECORD_VALID,      /* right size, canonical attributes */
    BOOT_STORE_RECORD_MALFORMED,  /* present with another size or attributes */
    BOOT_STORE_RECORD_UNKNOWN,    /* the read itself failed */
} boot_store_record_state_t;

typedef enum {
    BOOT_STORE_RECORD_KEEP = 0,
    BOOT_STORE_RECORD_WRITE,
    BOOT_STORE_RECORD_REPLACE, /* delete first: attributes cannot be changed in place */
    BOOT_STORE_RECORD_DELETE,
} boot_store_record_action_t;

typedef enum {
    BOOT_STORE_RECORD_LEAVE = 0, /* this boot learned nothing about the store */
    BOOT_STORE_RECORD_CLEAR,     /* a filesystem was searched and nothing is wrong */
    BOOT_STORE_RECORD_RECORD,    /* a store was there and could not be used */
} boot_store_record_intent_t;

boot_store_record_intent_t boot_store_record_intent(int load_status, int reject_code);

/* What to do with the variable, given what this boot found (`wanted` NULL = the
 * store is fine) and what is stored. An unreadable variable is left alone: it
 * cannot be compared, and writing anyway would wear NVRAM on every boot. */
boot_store_record_action_t boot_store_record_decide(
    const boot_store_reject_record_t *wanted,
    boot_store_record_state_t state,
    const boot_store_reject_record_t *stored);

/* The last-known-good shadow of the store (docs/boot/boot-entry-schema.md,
 * Fallback Contract).
 *
 * The loader keeps a copy of the last store it ACCEPTED (parsed with no
 * reject) beside the primary, as bootentries.lkg.json, published through
 * bootentries.lkg.tmp. Both are the store format, CRC and all, so the parser
 * itself decides whether a copy is intact. It is "last parser-accepted", not
 * "last successfully booted": recovered entries go through the same policy
 * and image checks as any other store.
 *
 * Everything here is pure: the loader supplies file access through
 * boot_store_shadow_ops, and the kernel suite drives the same code against
 * an in-memory volume with injected faults. */
#define BOOT_STORE_PRIMARY_NAME "bootentries.json"
#define BOOT_STORE_LKG_NAME     "bootentries.lkg.json"
#define BOOT_STORE_TMP_NAME     "bootentries.lkg.tmp"

typedef enum {
    BOOT_STORE_COPY_ABSENT = 0, /* no such file */
    BOOT_STORE_COPY_INVALID,    /* present and definitely not a usable store */
    BOOT_STORE_COPY_VALID,      /* parsed with no reject */
    BOOT_STORE_COPY_UNREADABLE, /* present, but the read failed: it may still be valid */
} boot_store_copy_state_t;

typedef enum {
    BOOT_STORE_SOURCE_PRIMARY = 0,
    BOOT_STORE_SOURCE_LKG,
    BOOT_STORE_SOURCE_TMP,
    BOOT_STORE_SOURCE_FALLBACK, /* no usable copy: the synthesized entry */
} boot_store_source_t;

/* May this boot consult the shadow at all? Only when a primary store is
 * THERE and could not be used (the notice predicate). An absent primary is
 * the user resetting to the fallback, and no filesystem means nothing to
 * read; neither consults the shadow. */
int boot_store_shadow_consult(int load_status, int reject_code);

/* Which copy this boot uses. The published copy is preferred over the
 * staging one: the staging copy is only a second chance after an
 * interrupted publication. */
boot_store_source_t boot_store_source_choose(int load_status, int reject_code,
                                             boot_store_copy_state_t lkg,
                                             boot_store_copy_state_t tmp);

typedef enum {
    BOOT_STORE_SHADOW_KEEP = 0,  /* the published copy already matches */
    BOOT_STORE_SHADOW_DROP_TMP,  /* matches, and a leftover staging copy can go */
    BOOT_STORE_SHADOW_PROMOTE,   /* copy the staging bytes to the published name */
    BOOT_STORE_SHADOW_REFRESH,   /* write the primary's bytes as the staging copy */
    BOOT_STORE_SHADOW_HOLD,      /* a copy could not be read: touch nothing this boot */
} boot_store_shadow_action_t;

/* What to do with the shadow files after the primary parsed with no reject.
 * `*_matches` is 1 when that copy's bytes equal the primary's. Two rules keep
 * a valid copy alive through any sequence of interrupted steps: a file is
 * only rewritten while the OTHER file holds a valid copy (or neither does),
 * and nothing is renamed, because a FAT rename is not power-fail atomic. A
 * copy that could not be READ might be the only valid one, so it is never
 * rewritten unless the other copy is valid; otherwise the plan is HOLD until a
 * later boot can read it. */
boot_store_shadow_action_t boot_store_shadow_plan(boot_store_copy_state_t lkg, int lkg_matches,
                                                  boot_store_copy_state_t tmp, int tmp_matches);

#define BOOT_STORE_FILE_LKG 0
#define BOOT_STORE_FILE_TMP 1

/* File access for the two shadow files (`which` is BOOT_STORE_FILE_*).
 * read:    0 absent; 1 read whole into buf and len (give it back with release);
 *          2 present but definitely not a store (empty, over the size cap);
 *          -1 present but the read failed (it may still be valid).
 * write:   0 when the file now holds exactly `buf`, durably (written, flushed,
 *          closed); any other value is a failure and the file's contents are
 *          unknown.
 * remove:  0 when the file is gone (also when it was never there).
 * accept:  1 when `buf` parses with no reject. */
struct boot_store_shadow_ops {
    void *ctx;
    int (*read)(void *ctx, int which, const unsigned char **buf, unsigned int *len);
    void (*release)(void *ctx, int which, const unsigned char *buf);
    int (*write)(void *ctx, int which, const unsigned char *buf, unsigned int len);
    int (*remove)(void *ctx, int which);
    int (*accept)(void *ctx, const unsigned char *buf, unsigned int len);
};

typedef enum {
    BOOT_STORE_SYNC_CURRENT = 0, /* nothing to do */
    BOOT_STORE_SYNC_SAVED,       /* the published copy now matches the primary */
    BOOT_STORE_SYNC_FAILED,      /* a write or remove failed; stopped there */
    BOOT_STORE_SYNC_UNSETTLED,   /* the step budget ran out */
    BOOT_STORE_SYNC_HELD,        /* a copy could not be read; nothing was changed */
} boot_store_sync_result_t;

/* Longest uninterrupted path: promote an older staging copy, refresh,
 * promote, drop the staging copy, then the final check. */
#define BOOT_STORE_SYNC_MAX_STEPS 6u

/* Bring the shadow up to date with an accepted primary. Re-plans after every
 * step and stops at the first failure, so an interrupted or failed run leaves
 * a state the next boot resumes from. */
boot_store_sync_result_t boot_store_shadow_sync(const struct boot_store_shadow_ops *ops,
                                                const unsigned char *primary,
                                                unsigned int primary_len);

/* For a present-but-unusable primary: find the copy to boot. On LKG or TMP,
 * buf and len hold the accepted copy (the caller owns it; release it with the
 * same ops) and `accept` was the LAST call made, so a caller whose accept
 * parses into scratch finds that copy's parse there. */
boot_store_source_t boot_store_shadow_pick(const struct boot_store_shadow_ops *ops,
                                           int load_status, int reject_code,
                                           const unsigned char **buf, unsigned int *len,
                                           boot_store_copy_state_t *lkg_state,
                                           boot_store_copy_state_t *tmp_state);

#endif /* BOOT_ENTRIES_PARSER_H */
