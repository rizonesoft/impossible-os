/* ============================================================================
 * boot_audit_codes.h -- Policy audit reason codes and NVRAM sticky record
 *
 * Per-decision boot policy audit trail. The disk-first half lives in
 * BlackBox JSONL (`X:\BlackBox\boot\history.jsonl` for per-boot history,
 * `X:\BlackBox\boot\mutations.jsonl` for entry-store mutations). The
 * NVRAM half is a single 256-byte UEFI variable named
 * `ImpossibleOS-BootSticky` and is EXCEPTIONAL-ONLY -- it carries
 * cross-boot triggers (recovery request, watchdog rollback request) and
 * the most recent boot outcome bit, NOT per-boot history. Per-boot writes
 * to NVRAM wear flash; BLS rationale at uapi-group.org rejects EFI vars
 * for high-frequency state.
 *
 * Two-phase ack: bootloader READS the sticky record and surfaces the
 * bits in boot_info (no NVRAM writes); kernel acks consumed triggers
 * (clears the bits) AFTER `boot_audit_publish()` writes the BlackBox
 * JSONL record successfully. A reset between read and ack leaves the
 * trigger pending, which is the correct sticky semantic -- a recovery
 * request must persist across crashes until the recovery boot durably
 * records its outcome.
 *
 * Pure C, freestanding, no dynamic allocation, no stdlib include.
 * Bootloader and kernel both consume this header. Pattern matches
 * include/boot/boot_entries.h: plain `unsigned int` typedefs with a
 * size assert, so the bootloader build (no `kernel/types.h`) and the
 * kernel build agree on the wire format.
 * ============================================================================ */

#ifndef IMPOSSIBLEOS_BOOT_AUDIT_CODES_H
#define IMPOSSIBLEOS_BOOT_AUDIT_CODES_H

_Static_assert(sizeof(unsigned char)  == 1, "boot_audit_codes.h assumes 8-bit char");
_Static_assert(sizeof(unsigned short) == 2, "boot_audit_codes.h assumes 16-bit short");
_Static_assert(sizeof(unsigned int)   == 4, "boot_audit_codes.h assumes 32-bit int");

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Audit event code (per-boot category for the JSONL `event` field) -- */

enum boot_audit_event_code {
    BOOT_AUDIT_EVENT_UNSET                = 0,
    BOOT_AUDIT_EVENT_NORMAL               = 1,
    BOOT_AUDIT_EVENT_BOOTNEXT_HINT        = 2,
    BOOT_AUDIT_EVENT_HOTKEY               = 3,
    BOOT_AUDIT_EVENT_WATCHDOG_ROLLBACK    = 4,
    BOOT_AUDIT_EVENT_AB_FAIL              = 5,
    BOOT_AUDIT_EVENT_RECOVERY             = 6,
    BOOT_AUDIT_EVENT_FALLBACK_DEFAULT     = 7,
    BOOT_AUDIT_EVENT_STORE_INVALID        = 8,
    BOOT_AUDIT_EVENT_UNKNOWN_BOOTCURRENT  = 9,
    BOOT_AUDIT_EVENT_MANIFEST_UNREADABLE  = 10,
    BOOT_AUDIT_EVENT_ALL_PATHS_BAD        = 11,
    BOOT_AUDIT_EVENT_FIRST_BOOT           = 12,
    BOOT_AUDIT_EVENT_AUDIT_DEGRADED       = 13,
};

#define BOOT_AUDIT_EVENT_MAX  BOOT_AUDIT_EVENT_AUDIT_DEGRADED

/* ----- NVRAM sticky variable ------------------------------------------- */

#define BOOT_STICKY_RECORD_MAGIC    0x54535342u  /* 'BSST' */
#define BOOT_STICKY_RECORD_VERSION  1u
#define BOOT_STICKY_VAR_SIZE        256u

#define BOOT_STICKY_VAR_NAME_ASCII  "ImpossibleOS-BootSticky"

struct boot_sticky_record {
    unsigned int   magic;                        /* BOOT_STICKY_RECORD_MAGIC */
    unsigned short version;                      /* BOOT_STICKY_RECORD_VERSION */
    unsigned short size;                         /* BOOT_STICKY_VAR_SIZE */

    unsigned char  recovery_trigger;             /* 1 if recovery boot requested across reset */
    unsigned char  watchdog_rollback_request;    /* 1 if previous boot timed out without mark-good */
    unsigned char  last_outcome;                 /* 1 if previous boot reached mark-good */
    unsigned char  audit_degraded_last_boot;     /* 1 if previous boot's NVRAM/BlackBox path was degraded */

    unsigned short last_event_code;              /* enum boot_audit_event_code from previous boot */
    unsigned short _reserved_a;                  /* 0 */

    unsigned int   last_boot_seq;                /* monotonic boot counter (matches JSONL boot_seq) */
    unsigned int   consumed_trigger_seq;         /* boot_seq when trigger was last consumed (ack) */

    unsigned char  _reserved_b[228];             /* zero-fill; future fields shrink this */

    unsigned int   crc32;                        /* CRC-32/IEEE-802.3 over bytes [0..251] */
};

_Static_assert(sizeof(struct boot_sticky_record) == 256,
    "boot_sticky_record must be exactly 256 bytes -- BOOT_STICKY_VAR_SIZE contract");

_Static_assert(__builtin_offsetof(struct boot_sticky_record, magic) == 0, "");
_Static_assert(__builtin_offsetof(struct boot_sticky_record, version) == 4, "");
_Static_assert(__builtin_offsetof(struct boot_sticky_record, size) == 6, "");
_Static_assert(__builtin_offsetof(struct boot_sticky_record, recovery_trigger) == 8, "");
_Static_assert(__builtin_offsetof(struct boot_sticky_record, watchdog_rollback_request) == 9, "");
_Static_assert(__builtin_offsetof(struct boot_sticky_record, last_outcome) == 10, "");
_Static_assert(__builtin_offsetof(struct boot_sticky_record, audit_degraded_last_boot) == 11, "");
_Static_assert(__builtin_offsetof(struct boot_sticky_record, last_event_code) == 12, "");
_Static_assert(__builtin_offsetof(struct boot_sticky_record, last_boot_seq) == 16, "");
_Static_assert(__builtin_offsetof(struct boot_sticky_record, consumed_trigger_seq) == 20, "");
_Static_assert(__builtin_offsetof(struct boot_sticky_record, crc32) == 252, "");

/* CRC-32/IEEE-802.3 over the first 252 bytes of `r` (everything except
 * the trailing CRC field). Same polynomial used by the boot_entries
 * envelope CRC so producer and consumer agree byte-for-byte. */
static inline unsigned int
boot_sticky_compute_crc(const struct boot_sticky_record *r)
{
    static const unsigned int poly = 0xEDB88320u;
    const unsigned char *p = (const unsigned char *)r;
    unsigned int crc = 0xFFFFFFFFu;
    for (unsigned int i = 0; i < (unsigned int)(sizeof(*r) - sizeof(unsigned int)); i++) {
        crc ^= (unsigned int)p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (poly & -(int)(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

static inline int
boot_sticky_record_is_valid(const struct boot_sticky_record *r)
{
    if (r->magic != BOOT_STICKY_RECORD_MAGIC) return 0;
    if (r->version != BOOT_STICKY_RECORD_VERSION) return 0;
    if (r->size != BOOT_STICKY_VAR_SIZE) return 0;
    if (r->crc32 != boot_sticky_compute_crc(r)) return 0;
    return 1;
}

/* Map an enum boot_selection_reason value to a boot_audit_event_code.
 * Audit codes are a SUPERSET of selection reasons -- callers must
 * special-case codes that have no selection_reason equivalent
 * (MANIFEST_UNREADABLE, ALL_PATHS_BAD, FIRST_BOOT, AUDIT_DEGRADED). */
static inline unsigned short
boot_audit_event_from_selection_reason(unsigned int selection_reason)
{
    switch (selection_reason) {
        case 1:  return BOOT_AUDIT_EVENT_NORMAL;             /* STORE_DEFAULT */
        case 2:  return BOOT_AUDIT_EVENT_BOOTNEXT_HINT;
        case 3:  return BOOT_AUDIT_EVENT_HOTKEY;
        case 4:  return BOOT_AUDIT_EVENT_WATCHDOG_ROLLBACK;
        case 5:  return BOOT_AUDIT_EVENT_AB_FAIL;
        case 6:  return BOOT_AUDIT_EVENT_RECOVERY;
        case 7:  return BOOT_AUDIT_EVENT_FALLBACK_DEFAULT;   /* FALLBACK_NO_VIABLE */
        case 8:  return BOOT_AUDIT_EVENT_STORE_INVALID;
        case 9:  return BOOT_AUDIT_EVENT_UNKNOWN_BOOTCURRENT;
        default: return BOOT_AUDIT_EVENT_UNSET;
    }
}

static inline const char *
boot_audit_event_name(unsigned short code)
{
    switch (code) {
        case BOOT_AUDIT_EVENT_UNSET:                return "UNSET";
        case BOOT_AUDIT_EVENT_NORMAL:               return "NORMAL";
        case BOOT_AUDIT_EVENT_BOOTNEXT_HINT:        return "BOOTNEXT_HINT";
        case BOOT_AUDIT_EVENT_HOTKEY:               return "HOTKEY";
        case BOOT_AUDIT_EVENT_WATCHDOG_ROLLBACK:    return "WATCHDOG_ROLLBACK";
        case BOOT_AUDIT_EVENT_AB_FAIL:              return "AB_FAIL";
        case BOOT_AUDIT_EVENT_RECOVERY:             return "RECOVERY";
        case BOOT_AUDIT_EVENT_FALLBACK_DEFAULT:     return "FALLBACK_DEFAULT";
        case BOOT_AUDIT_EVENT_STORE_INVALID:        return "STORE_INVALID";
        case BOOT_AUDIT_EVENT_UNKNOWN_BOOTCURRENT:  return "UNKNOWN_BOOTCURRENT";
        case BOOT_AUDIT_EVENT_MANIFEST_UNREADABLE:  return "MANIFEST_UNREADABLE";
        case BOOT_AUDIT_EVENT_ALL_PATHS_BAD:        return "ALL_PATHS_BAD";
        case BOOT_AUDIT_EVENT_FIRST_BOOT:           return "FIRST_BOOT";
        case BOOT_AUDIT_EVENT_AUDIT_DEGRADED:       return "AUDIT_DEGRADED";
        default:                                    return "UNKNOWN";
    }
}

#define BOOT_AUDIT_JSONL_SCHEMA_VERSION  1u

/* The BlackBox partition is mounted as drive X:\, so `X:\Boot\` is the
 * runtime path even though the partition's volume label is BLACKBOX.
 * `Boot` (capital B) matches the pre-staged skeleton from the disk
 * image build (see scripts/release/build-image.sh and the system disk
 * Makefile rule, both of which `mmd ::Boot` on the FAT32 partition).
 * The audit publisher does not currently create subdirs, so the path
 * MUST land under a directory the image build pre-creates. */
#define BOOT_AUDIT_HISTORY_PATH         "X:\\Boot\\history.jsonl"
#define BOOT_AUDIT_MUTATIONS_PATH       "X:\\Boot\\mutations.jsonl"
#define BOOT_AUDIT_HISTORY_ROTATED      "X:\\Boot\\history.1.jsonl"
#define BOOT_AUDIT_MUTATIONS_ROTATED    "X:\\Boot\\mutations.1.jsonl"
#define BOOT_AUDIT_ROTATE_THRESHOLD     (4u * 1024u * 1024u)  /* 4 MiB */

/* Per-boot monotonic counter persisted on the BlackBox partition (NOT
 * NVRAM -- writing this on every successful boot must not wear flash).
 * The audit publisher reads this file at compose time, increments the
 * value, writes the new value BEFORE composing the JSONL line, and
 * then writes the JSONL with that boot_seq. Pre-write order ensures
 * a JSONL append failure still advances the seq next boot (skipped
 * seq is tolerable; duplicate seq violates the schema's monotonic
 * contract). */
#define BOOT_AUDIT_SEQUENCE_PATH        "X:\\Boot\\sequence"
#define BOOT_AUDIT_SEQUENCE_TEMP_PATH   "X:\\Boot\\sequence.new"
#define BOOT_AUDIT_SEQUENCE_LEN_MAX     16u   /* room for "4294967295\n" */

#ifdef __cplusplus
}
#endif

#endif /* IMPOSSIBLEOS_BOOT_AUDIT_CODES_H */
