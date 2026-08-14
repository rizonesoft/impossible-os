/* ============================================================================
 * config.h -- Kernel configuration & policy plane
 *
 * Section 1: Boot Argument Schema and Parser.
 *
 * The UEFI bootloader already parses \EFI\ImpossibleOS\boot.conf into the typed
 * `struct boot_config` (boot_info.h) and preserves the raw command line in
 * `boot_config.cmdline[256]`. This header adds the KERNEL-side typed schema
 * layer on top of that handoff:
 *
 *   - `boot_arg_desc_t` -- one descriptor per known key (type, range, security
 *     class, consuming phase, default, help). A static table in config.c is the
 *     single schema source of truth.
 *   - A pure parser (`boot_args_parse_cmdline`) that normalizes aliases,
 *     validates type/range/enum, preserves the original value token, and
 *     records provenance -- callable from unit tests with no live-boot
 *     dependency.
 *   - A Phase-0 entry point (`boot_args_init`) that layers the bootloader's
 *     `boot_config` fields (source = BOOTCFG) under the cmdline-parsed values
 *     (source = CMDLINE), rejects unknown `kernel.*` keys (BOOT_FATAL ->
 *     boot_halt unless `boot.allow_unknown=1`), and publishes a read-only
 *     provenance view.
 *
 * Precedence + provenance: per Section 3 the merge order is compiled default <
 * boot_config < command line < firmware, so a CMDLINE value for a key DOES win
 * over the BOOTCFG projection of that key -- but never silently: the effective
 * `source` is recorded and every cmdline override of a BOOTCFG key is logged.
 * The per-value `source` + `present` metadata is what makes that auditable.
 *
 * BOOTCFG projection is canonicalized, not original: `struct boot_config`
 * carries only normalized typed fields plus a single `config_found` bit -- it
 * has NO per-key presence bitset and NO raw boot.conf tokens. So a projected
 * BOOTCFG value records `present = 1`, `source = BOOTCFG`, and `raw` = the
 * canonical string form of the field (`"1"`, `"network"`), explicitly NOT the
 * original boot.conf spelling. The finer "was this boot.conf key explicit or a
 * compiled default" distinction needs producer-side metadata (a boot_config
 * presence bitset + raw tokens = a BOOT_INFO_VERSION bump) and belongs to the
 * Section 3 precedence merge; it is filed there, not forced into Section 1.
 *
 * Phase-0 contract: everything here is fully static / fixed-size. No kmalloc,
 * no heap dependency, bounded token and CSV counts, and hard-fail (never
 * silent-truncate) on overflow. Every storage buffer is sized to its
 * documented length cap so neither a value token nor the BOOT_FATAL-causing
 * unknown key is ever truncated. The parsed result is published from a single
 * static object (`boot_args_parsed()`), never a large implicit Phase-0 stack
 * allocation -- boot_phase0 runs before the heap is initialized.
 *
 * Section 2 (kernel_config_t immutable snapshot) consumes this layer.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_info.h"        /* struct boot_config, enum boot_reason_code */
#include "kernel/boot_init.h"        /* boot_result_t */

/* ---- Fixed-size caps (no allocation at Phase 0) -------------------------- */
/* Documented LENGTH caps -- the parser hard-fails (BOOT_ARGS_ERR_OVERFLOW) when
 * a key or value exceeds these, never truncates. Each buffer below is sized to
 * cap + 1 (NUL) so a stored token is never silently clipped. */
#define BOOT_ARG_NAME_CAP      31   /* max key-name length */
#define BOOT_ARG_RAWVAL_CAP    47   /* max single value token length */
#define BOOT_ARG_STRVAL_CAP    63   /* max STRING / CSV-joined value length */

#define BOOT_ARG_NAME_MAX     (BOOT_ARG_NAME_CAP + 1)
#define BOOT_ARG_RAWVAL_MAX   (BOOT_ARG_RAWVAL_CAP + 1)
#define BOOT_ARG_STRVAL_MAX   (BOOT_ARG_STRVAL_CAP + 1)

#define BOOT_ARG_CSV_MAX        8   /* max elements in a CSV-list value */
#define BOOT_ARGS_UNKNOWN_MAX   8   /* unknown keys recorded before the halt line */
#define BOOT_ARGS_TOKEN_MAX    48   /* max distinct keys held in a parsed result */

/* ---- Value type ---------------------------------------------------------- */

typedef enum {
    BOOT_ARG_BOOL = 0,   /* on/off/1/0/yes/no/true/false */
    BOOT_ARG_INT,        /* signed decimal, range-checked */
    BOOT_ARG_ENUM,       /* one of a fixed name set (value = index) */
    BOOT_ARG_STRING,     /* opaque short string */
    BOOT_ARG_DURATION,   /* "500ms" / "2s" / "1m" -> milliseconds */
    BOOT_ARG_BYTESIZE,   /* "64k" / "2M" / "1G" -> bytes */
    BOOT_ARG_CSV,        /* comma-separated list of short tokens */
} boot_arg_type_t;

/* Security class -- Section 9 (policy lock phases) keys lock timing and the
 * post-lock-downgrade-is-fatal rule off this; Section 3 (registry merge) keys
 * override precedence off it too. The values are load-bearing now, not
 * decorative metadata. */
typedef enum {
    BOOT_ARG_CLASS_NORMAL = 0,   /* operator-tunable, no security weight */
    BOOT_ARG_CLASS_DEBUG,        /* diagnostics; relaxed/ignored under production lock */
    BOOT_ARG_CLASS_SECURITY,     /* CI / Secure Boot / verifier -- locked early, downgrade fatal */
} boot_arg_class_t;

/* Earliest phase that consumes the key -- Section 3 precedence and Section 9
 * lock timing both read this. */
typedef enum {
    BOOT_ARG_PHASE0 = 0,
    BOOT_ARG_PHASE1,
    BOOT_ARG_PHASE2,
    BOOT_ARG_PHASE3,
    BOOT_ARG_RUNTIME,
} boot_arg_phase_t;

/* Where a resolved value came from -- the basis of Section 3 merge precedence
 * (compiled default < boot_config < command line < firmware). config_dump
 * (Section 11) prints this so an operator can see why a key has its value. */
typedef enum {
    BOOT_ARG_SRC_DEFAULT = 0,    /* key absent; descriptor default_val in effect */
    BOOT_ARG_SRC_BOOTCFG,        /* projected from a boot_config field (canonical raw) */
    BOOT_ARG_SRC_CMDLINE,        /* parsed from cfg->cmdline */
    BOOT_ARG_SRC_FIRMWARE,       /* firmware-enforced (reserved for Section 3 top precedence) */
} boot_arg_source_t;

/* ---- Descriptor ---------------------------------------------------------- */

typedef struct {
    const char        *name;            /* canonical key name */
    boot_arg_type_t    type;
    boot_arg_class_t   security_class;
    boot_arg_phase_t   phase;
    int64_t            min_val;         /* INT/DURATION/BYTESIZE inclusive lower bound */
    int64_t            max_val;         /* INT/DURATION/BYTESIZE inclusive upper bound */
    int64_t            default_val;     /* BOOL/INT/ENUM default when key absent */
    const char *const *enum_names;      /* ENUM only: NULL-terminated array, value = index */
    const char        *help;            /* one-line operator help text */
} boot_arg_desc_t;

/* The static schema table (config.c). `boot_arg_table(&count)` returns it. */
const boot_arg_desc_t *boot_arg_table(uint32_t *out_count);

/* Look up a descriptor by canonical name, NULL if not in the schema. */
const boot_arg_desc_t *boot_arg_find(const char *name);

/* ---- Parsed result + provenance ------------------------------------------ */

/* One validated key/value, normalized to its descriptor type. INT/BOOL/ENUM/
 * DURATION/BYTESIZE land in `ival`; STRING/CSV land in `sval` (CSV joined back
 * with ',' after per-element validation). `raw` preserves the original
 * unnormalized token (`"2s"` for a DURATION whose `ival` is 2000) for CMDLINE
 * values, or the canonical field string for BOOTCFG values; empty for
 * default-sourced values. */
typedef struct {
    const boot_arg_desc_t *desc;        /* schema entry this value satisfies */
    int64_t                ival;        /* numeric/enum/bool/duration(ms)/bytesize */
    char                   sval[BOOT_ARG_STRVAL_MAX];
    char                   raw[BOOT_ARG_RAWVAL_MAX];   /* original/canonical value token */
    boot_arg_source_t      source;      /* who set this value (precedence basis) */
    uint8_t                present;     /* 1 = explicitly set; 0 = descriptor default */
    uint8_t                csv_count;   /* CSV: element count (0 otherwise) */
} boot_arg_value_t;

/* Parser status -- distinguishes the failure classes the Phase-0 caller routes
 * through the boot failure policy. */
typedef enum {
    BOOT_ARGS_OK = 0,            /* all tokens parsed + validated */
    BOOT_ARGS_ERR_UNKNOWN_KEY,   /* an unknown kernel.* key (no allow_unknown) */
    BOOT_ARGS_ERR_BAD_VALUE,     /* type/range/enum validation failed */
    BOOT_ARGS_ERR_OVERFLOW,      /* token / CSV / value-length cap exceeded */
} boot_args_status_t;

/* Fixed-size parsed view. No pointers into the source string; every value is
 * copied so the result outlives the cmdline buffer. Published as a single
 * static object via boot_args_parsed(); callers must not place one on the
 * Phase-0 stack (it is several KB). */
typedef struct {
    boot_arg_value_t   values[BOOT_ARGS_TOKEN_MAX];
    uint32_t           count;                  /* populated entries in values[] */
    boot_args_status_t status;
    /* First offending key (UNKNOWN_KEY / BAD_VALUE / OVERFLOW), full -- never
     * truncated, so a failed boot's diagnostic names the exact key. */
    char               err_key[BOOT_ARG_NAME_MAX];
    /* Unknown kernel.* keys seen (recorded even when allow_unknown lets boot continue). */
    char               unknown[BOOT_ARGS_UNKNOWN_MAX][BOOT_ARG_NAME_MAX];
    uint8_t            unknown_count;
    uint8_t            allow_unknown;          /* boot.allow_unknown=1 was present */
    /* Count of CMDLINE tokens that overrode a BOOTCFG-projected key (logged at
     * boot_args_init time -- the audit trail for the precedence rule). */
    uint8_t            overridden_count;
    /* First hard (BAD_VALUE/OVERFLOW) error seen, tracked separately from the
     * first-failure `status`. A hard error is NEVER downgradeable, so when
     * boot.allow_unknown=1 downgrades a leading UNKNOWN_KEY this surviving hard
     * error is promoted instead of letting the parse pass as OK. */
    boot_args_status_t hard_status;
    char               hard_key[BOOT_ARG_NAME_MAX];
} boot_args_t;

/* ---- Pure parser (unit-testable; no live boot infrastructure) ------------ */

/* Parse a command-line string into `out`. Normalizes aliases (`/debug` ->
 * `debug=1`, bare `nogui` -> `nogui=1`, etc.), validates each known key against
 * its descriptor, records the original token + source = CMDLINE, tracks unknown
 * `kernel.*` keys, and sets `out->status`. Returns the same status. Hard-fails
 * (no truncation) when a length / count cap is exceeded. Does NOT halt -- the
 * Phase-0 wrapper owns the failure-policy decision. `out` is caller-owned;
 * callers pass the static boot_args object, not a stack temporary. */
boot_args_status_t boot_args_parse_cmdline(const char *cmdline, boot_args_t *out);

/* Length-bounded variant: never reads past `max_len` bytes of `cmdline`. The
 * live Phase-0 caller passes BOOT_CONF_CMDLINE_MAX for the 256-byte handoff
 * field, which may be corrupt/unterminated. Returns BOOT_ARGS_ERR_OVERFLOW when
 * no NUL terminator is found within `max_len` (a corrupt handoff is rejected,
 * never over-read into adjacent boot_info bytes). The unbounded wrapper above
 * is a convenience for NUL-terminated literals (unit tests). */
boot_args_status_t boot_args_parse_cmdline_n(const char *cmdline, uint32_t max_len,
                                             boot_args_t *out);

/* Fetch a validated value by canonical name from a parsed result.
 * Returns NULL when the key was absent (caller falls back to desc->default_val). */
const boot_arg_value_t *boot_args_get(const boot_args_t *args, const char *name);

#ifdef KERNEL_TESTS
/* Resolved value of an extension key with no kernel_config_t field of its own.
 * Exists for the TODO-10 S27 degraded-configuration injection keys
 * (`test_abandon_ap`, `test_hold_async_cpu`, `test_park_cpu`), whose schema
 * rows and consumers both compile out at KERNEL_TESTS=off. Returns 0 (= off)
 * before the Phase-0 reconciliation has published the parsed table. */
int64_t boot_arg_resolved_ival(const char *name);
#endif

/* ---- Phase-0 entry point + provenance ------------------------------------ */

/* Validate the bootloader handoff: project the `boot_config` fields through the
 * schema as the BOOTCFG layer, parse `cfg->cmdline` for aliases + extension
 * keys as the CMDLINE layer (CMDLINE overrides BOOTCFG per Section 3 precedence,
 * each override logged), and record provenance (original cmdline, selected
 * boot-entry id, selection reason). On an unknown `kernel.*` key without
 * `boot.allow_unknown=1`, logs `[CONF] unknown boot key: <k>` and returns
 * BOOT_FATAL so the caller halts (Phase 0 has no degraded-continue path; T01
 * section 7). Otherwise logs `[CONF] parsed boot args: ...` and returns
 * BOOT_OK. Call once in boot_phase0 before any Phase-0 config consumer. */
boot_result_t boot_args_init(const struct boot_config *cfg);

/* Read-only provenance accessors, valid after a successful boot_args_init().
 * The original command line, the selected boot-entry id, and the boot-selection
 * reason are preserved for crash-dump and diagnostic output. */
const char            *boot_args_cmdline(void);
const char            *boot_args_entry_id(void);
/* The boot-ENTRY ladder reason (enum boot_selection_reason as uint32), the
 * field paired with selected_entry_id -- NOT boot_reason (the policy reason). */
uint32_t               boot_args_selection_reason(void);

/* The parsed result published by boot_args_init() (NULL before it runs). */
const boot_args_t *boot_args_parsed(void);

/* ============================================================================
 * Section 2: kernel_config_t -- immutable Phase 0 configuration snapshot.
 *
 * One read-only typed snapshot that every later phase consumes instead of
 * re-reading the mutable boot_args / boot_config / boot_info structures. Every
 * Phase-0-resolved boot argument is FLATTENED into a plain scalar/enum field
 * here (boot_args_t stays a private parser/provenance input, never the public
 * consumption contract) so consumers never re-derive defaults, enum meanings,
 * or provenance.
 *
 * Publication ordering: published once, on the BSP, AFTER boot_decision_validate
 * has accepted the boot-decision provenance (boot_reason / selected_entry_id) --
 * NOT right after boot_args_init, whose decision inputs are still unvalidated.
 * kernel_config_get() returns NULL before that point. The `ready` flag is
 * written LAST, behind an smp_mb release, so an AP/late reader never observes a
 * half-filled snapshot.
 *
 * The control-set target + LastKnownGood are NOT fields here: they are resolved
 * only after Select-value validation, which is ControlSet/LastKnownGood selection
 * work, and a zero placeholder in an immutable object would be permanently
 * false. That section owns the resolved control-set policy and how it is
 * published.
 * ============================================================================ */

#define KERNEL_CONFIG_MAGIC    0x43464731u   /* "CFG1" -- layout sanity guard */
#define KERNEL_CONFIG_VERSION  1u            /* logged/layout guard; bump on field change */

typedef struct {
    /* Header -- T31 bulletproofing sanity (magic/version/size verified at use). */
    uint32_t magic;                 /* KERNEL_CONFIG_MAGIC */
    uint16_t version;               /* KERNEL_CONFIG_VERSION */
    uint16_t size;                  /* sizeof(kernel_config_t) */

    /* Boot mode + safe-mode profile. */
    uint8_t  boot_mode;             /* 0=normal, 1=safe, 2=recovery (boot_config.boot_mode) */
    uint8_t  safe_mode;             /* safe_mode_t (0=off..3=dsrepair), boot-policy floored */
    uint8_t  safe_mode_reason;      /* safe_mode_reason_t: why safe mode is in effect */

    /* Init flags. */
    uint8_t  async_init;            /* parallel subsystem init on APs */
    uint8_t  deferred_init;         /* defer non-critical inits */

    /* Debug / logging transports. */
    uint8_t  debug_enabled;         /* debug=1 */
    uint8_t  serial_debug;          /* COM1 serial output */
    uint8_t  test_mode;             /* run unit tests then halt */

    /* Code-integrity / verifier policy. */
    uint8_t  testsigning;           /* allow test-signed kernel code */
    uint8_t  nointegritychecks;     /* CI enforcement disabled */
    uint8_t  noacpi;                /* skip ACPI bring-up */
    uint8_t  nogui;                 /* graphical shell disabled */

    /* Graphics mode (from the validated boot_info framebuffer). */
    uint8_t  fb_valid;              /* 1 = a usable framebuffer was handed off */
    uint8_t  pixel_format;          /* GOP_PIXEL_* */
    uint32_t fb_width;
    uint32_t fb_height;

    /* Boot-decision provenance (valid only post boot_decision_validate). The
     * boot-entry ladder pairs selected_entry_id with selection_reason: a later
     * phase needs BOTH to explain why an entry was chosen and to detect the
     * empty-id FALLBACK_STORE_INVALID sentinel without re-reading boot_info. */
    uint32_t boot_reason;           /* enum boot_reason_code (policy reason) */
    uint32_t selection_reason;      /* enum boot_selection_reason (ladder reason) */
    char     entry_id[64];          /* selected boot-entry id ("" = fallback sentinel) */

    /* Verifier flags (CSV) + a provenance summary. */
    char     verifier[BOOT_ARG_STRVAL_MAX];   /* verifier= CSV, empty if unset */
    uint8_t  cmdline_override_count;           /* CMDLINE keys that beat a BOOTCFG key */
    uint8_t  _pad[3];
} kernel_config_t;

/* Exact ABI size for version 1. Any field add/remove/reorder/repack changes
 * this -- bump KERNEL_CONFIG_VERSION and KERNEL_CONFIG_SIZE_V1 together (a <=N
 * guard would let layout drift silently while the accessor contract changed). */
#define KERNEL_CONFIG_SIZE_V1 172u
_Static_assert(sizeof(kernel_config_t) == KERNEL_CONFIG_SIZE_V1,
    "kernel_config_t layout changed: bump KERNEL_CONFIG_VERSION + KERNEL_CONFIG_SIZE_V1");
/* Load-bearing field offsets -- catch a silent reorder/repack of the contract. */
_Static_assert(__builtin_offsetof(kernel_config_t, magic) == 0,
    "magic must lead the snapshot for the layout-sanity guard");
_Static_assert(__builtin_offsetof(kernel_config_t, boot_reason) == 32,
    "boot_reason offset pinned for the v1 ABI");
_Static_assert(__builtin_offsetof(kernel_config_t, selection_reason) == 36,
    "selection_reason offset pinned for the v1 ABI");
_Static_assert(__builtin_offsetof(kernel_config_t, entry_id) == 40,
    "entry_id offset pinned for the v1 ABI");

/* Build + publish the immutable snapshot from the validated handoff. Call once
 * on the BSP after boot_decision_validate succeeds. Writes the `ready` flag last
 * behind a release barrier; logs `[CONF] snapshot ready: version=1 phase=0`. */
void kernel_config_publish(const struct boot_config *cfg);

/* The published snapshot, or NULL before kernel_config_publish() runs. */
const kernel_config_t *kernel_config_get(void);

/* Operator/support diagnostic: klog the full config plane -- the immutable
 * snapshot, the resolved safe mode, the policy-lock state (lockdown level, lock
 * phase, registered/tamper counts), the boot-status policy + acceptance stage,
 * and the runtime tunable registry. Secret-class values (TUNABLE_PRIVILEGED
 * tunables) are redacted. NOT a panic-path function: it formats + writes serial
 * via klog and serializes concurrent callers with an atomic busy flag. Safe to
 * call any time after Phase 3 (reads only lockless snapshots / self-locked
 * getters). */
void config_dump(void);

/* ============================================================================
 * Section 5: Safe Mode policy object.
 *
 * A first-class safe-mode level + reason, resolved from the immutable Phase-0
 * snapshot. Boot-policy safe mode (boot_config.boot_mode safe/recovery) is a
 * MONOTONIC FLOOR over the safemode boot arg -- a lower-precedence cmdline
 * `safemode=off` can never downgrade a safe boot the bootloader/recovery entry
 * requested. Because the level lives in the immutable snapshot, no (lower-
 * precedence) registry value can disable it either.
 *
 * The selected-control-set surface (control-set selection feature) and the
 * recovery-trigger surface (boot-status acceptance ledger feature) are NOT
 * exposed here -- those are deferred to their owning sections' Phase-2
 * effective-policy / acceptance-ledger objects.
 * ============================================================================ */

typedef enum {
    SAFE_MODE_OFF      = 0,
    SAFE_MODE_MINIMAL  = 1,   /* core drivers/services only */
    SAFE_MODE_NETWORK  = 2,   /* minimal + networking */
    SAFE_MODE_DSREPAIR = 3,   /* directory-services-repair equivalent */
    SAFE_MODE_COUNT,          /* sentinel: name-table length guard, not a level */
} safe_mode_t;

typedef enum {
    SAFE_REASON_NONE        = 0,   /* not in safe mode */
    SAFE_REASON_OPERATOR    = 1,   /* operator set safemode= on the command line */
    SAFE_REASON_BOOT_POLICY = 2,   /* bootloader/boot-entry requested safe boot */
    SAFE_REASON_RECOVERY    = 3,   /* repeated-failure / recovery boot path */
    SAFE_REASON_COUNT,             /* sentinel: name-table length guard, not a reason */
} safe_mode_reason_t;

/* Components gated by safe mode. kernel_safe_mode_allows() answers whether each
 * is permitted at the current level. Consumed by the kernel module loader, the
 * desktop service manager, and the code-integrity-relaxation gate. */
typedef enum {
    SAFE_COMP_NETWORK,            /* network stack + NIC drivers */
    SAFE_COMP_GUI,                /* graphical shell / compositor */
    SAFE_COMP_THIRD_PARTY_MODULE, /* non-core loadable modules */
    SAFE_COMP_NONESSENTIAL_SVC,   /* non-essential auto-start services */
    SAFE_COMP_CI_RELAX,           /* code-integrity relaxation (test-signing etc.) */
} safe_mode_component_t;

/* Effective safe-mode level + reason, valid after kernel_config_publish().
 * Return SAFE_MODE_OFF / SAFE_REASON_NONE before publish. */
safe_mode_t        kernel_safe_mode(void);
safe_mode_reason_t kernel_safe_mode_reason(void);

/* 1 = `component` is permitted at the current safe-mode level, 0 = gated off.
 * The single gating contract every safe-mode consumer reads (no scattered
 * boot_mode checks). CI relaxations are NEVER auto-allowed under boot-policy or
 * recovery safe mode. */
int kernel_safe_mode_allows(safe_mode_component_t component);

/* Pure safe-mode floor resolver (unit-testable; no live state). `boot_mode` is
 * the boot_config value (0=normal,1=safe,2=recovery); `sm_arg` is the resolved
 * safemode boot-arg level (0..3); `operator_set` = the level came explicitly
 * from the command line. Writes the floored level + reason: boot_mode safe(1)/
 * recovery(2) floors the level to at least SAFE_MODE_MINIMAL so a lower-
 * precedence cmdline cannot downgrade a boot-policy safe boot. */
void safe_mode_resolve(uint8_t boot_mode, uint8_t sm_arg, int operator_set,
                       uint8_t *out_level, uint8_t *out_reason);

/* Pure gating decision (unit-testable): is `component` permitted at safe-mode
 * `level`? kernel_safe_mode_allows() is this evaluated at the live level. */
int safe_mode_component_allowed(safe_mode_t level, safe_mode_component_t component);
