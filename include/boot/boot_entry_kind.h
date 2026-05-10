/* boot_entry_kind.h -- per-kind payload validators + decoded payload struct.
 *
 * The boot-entry envelope only carries the kind discriminator. This header
 * owns the per-kind payload validation contract: given an envelope and the
 * byte range of its payload object inside the raw bootentries.json,
 * validate the per-kind shape (kernel path required, ASCII cmdline, no
 * nested objects, etc.) and decode the fields into a typed struct that
 * the loader path consumes.
 *
 * Pure C, freestanding, no UEFI types, no kernel/types.h. Same plain-type
 * pattern as include/boot/boot_entries.h so this header is linkable from
 * the bootloader (-ffreestanding x86_64-elf) and the kernel unit-test
 * binary alike.
 *
 * Consumer: the bootloader's policy invocation runs validation AFTER the
 * menu and boot_config materialization (so the FINAL selected envelope
 * is what gets validated -- not a pre-menu candidate that a hotkey may
 * have replaced) and BEFORE counter decrement. The decoded SPLIT payload
 * is then handed to the kernel loader as an authoritative kernel path:
 * when the policy ladder picked a SPLIT entry with a payload kernel,
 * that exact path is the SOLE candidate -- no fallback to ambient
 * defaults (otherwise the ladder reports `selected=X` while a different
 * kernel actually loaded).
 */

#ifndef BOOT_ENTRY_KIND_H
#define BOOT_ENTRY_KIND_H

#include "boot_entries.h"

_Static_assert(sizeof(unsigned int) == 4, "boot_entry_kind.h assumes 32-bit unsigned int");

/* Validation reject codes. Returned when a per-kind validator rejects
 * the entry. The ladder treats a non-zero value as KIND_UNAVAILABLE
 * and demotes the entry. */
typedef enum {
    BOOT_ENTRY_KIND_OK                  = 0,
    BOOT_ENTRY_KIND_REJ_PAYLOAD_MISSING = 1, /* required payload object absent */
    BOOT_ENTRY_KIND_REJ_PAYLOAD_FORBIDDEN = 2, /* payload object present but kind disallows it */
    BOOT_ENTRY_KIND_REJ_FIELD_MISSING   = 3, /* required field (kernel for split) missing */
    BOOT_ENTRY_KIND_REJ_FIELD_TYPE      = 4, /* field present but wrong JSON shape */
    BOOT_ENTRY_KIND_REJ_FIELD_VALUE     = 5, /* field value violates per-kind grammar (path / ASCII / digest) */
    BOOT_ENTRY_KIND_REJ_NOT_SUPPORTED   = 6, /* validator stub: kind known but not yet wired */
    BOOT_ENTRY_KIND_REJ_INTERNAL        = 7, /* parser walker exhausted bounds -- corruption */
} boot_entry_kind_reject_t;

/* Decoded SPLIT payload. Sizes match the file format caps from
 * boot_entries.h so the parser-validated envelope never overflows. */
#define BOOT_ENTRY_DECODED_SPLIT_INITRD_MAX 8u

typedef struct {
    char kernel[BOOT_ENTRIES_MAX_PATH_LEN + 1u];
    char cmdline[BOOT_ENTRIES_MAX_PATH_LEN + 1u]; /* same cap; ASCII only */
    char root[BOOT_ENTRIES_MAX_PATH_LEN + 1u];    /* slot id or partition GUID text */
    char initrd[BOOT_ENTRY_DECODED_SPLIT_INITRD_MAX][BOOT_ENTRIES_MAX_PATH_LEN + 1u];
    unsigned int initrd_count;
    unsigned int has_kernel;  /* 1 if kernel field present + non-empty */
    unsigned int has_cmdline; /* 1 if cmdline field present (may be empty string) */
    unsigned int has_root;    /* 1 if root field present */
} boot_entry_decoded_split_t;

/* Decoded UKI payload. The runtime load path is BOOTX64.UKI.efi's own
 * embedded .linux / .cmdline / .initrd PE sections, not disk-side
 * fields, so payload-absence is accepted by the validator. When a
 * payload object IS present the validator enforces schema 4.2:
 * `uki_path` required (ASCII + \EFI\Linux\ or \EFI\ImpossibleOS\
 * prefix + no traversal), optional `profile` integer 0..15, all other
 * keys rejected as smuggled overrides on a Secure-Boot-signed UKI.
 * The decoded struct intentionally carries no fields because the
 * advisory uki_path is not consumed by the loader; the contract is
 * enforcement-only. */
typedef struct {
    unsigned int _placeholder; /* reserved; uki_path is validated, not decoded */
} boot_entry_decoded_uki_t;

/* Decoded payload union, dispatched on envelope.kind. Future per-kind
 * payloads (chainload path, network url+digest, resume digest) go here
 * when their owner sections wire them. */
typedef struct {
    unsigned int kind;       /* boot_entry_kind_t */
    unsigned int valid;      /* 1 if validate succeeded; 0 otherwise */
    union {
        boot_entry_decoded_split_t split;
        boot_entry_decoded_uki_t   uki;
    } u;
} boot_entry_decoded_t;

/* Validate one entry's payload bytes per the envelope's kind. Returns
 * BOOT_ENTRY_KIND_OK on accept, non-zero reject code otherwise. The
 * decoded fields are written into `out` only on success; on rejection
 * `out->valid` is 0 and the union content is undefined.
 *
 * `payload_bytes` may be NULL when the envelope's payload_present == 0
 * (entry omitted the payload object entirely). Validators that require
 * the payload object reject with PAYLOAD_MISSING in that case;
 * validators that forbid it (UKI) accept the absence.
 *
 * `flags` carries the envelope BOOT_ENTRY_FLAG_* mask for validators
 * that branch on flags (chainload + Secure Boot path-escape gate).
 *
 * `secure_boot_active` is 1 when firmware reports Secure Boot enabled.
 * Used only by the chainload validator's path-escape gate today.
 */
int boot_entry_kind_validate(unsigned int kind,
                             unsigned int flags,
                             int secure_boot_active,
                             const unsigned char *payload_bytes,
                             unsigned int payload_len,
                             boot_entry_decoded_t *out);

/* Symbolic name for a reject code; used by serial logging. Returns
 * "OK" / "REJ_PAYLOAD_MISSING" / etc.; "UNKNOWN" for out-of-range. */
const char *boot_entry_kind_reject_name(int code);

#endif /* BOOT_ENTRY_KIND_H */
