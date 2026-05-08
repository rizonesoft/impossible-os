/* boot_entries.h -- on-ESP boot entry store schema (TODO-07 boot entry file format).
 *
 * Owners (in TODO-07 boot-entry-store-menu-policy):
 *   - boot entry file format (this header + docs/boot/boot-entry-schema.md): envelope,
 *     kind enum, flag bits, CRC algorithm.
 *   - boot entry parser and validator (src/boot/uefi parser): consumes the layout
 *     defined here.
 *   - per-entry-kind validators + loaders: consume the kind enum and per-kind
 *     payload field tables defined in the canonical schema doc.
 *
 * TRUST MODEL (two-tier; pinned by this section):
 *   Tier 1 -- UKI + Secure Boot (BOOT_FLAG_INVOKED_VIA_UKI set):
 *       The store is ADVISORY. Bootloader uses it for menu labels, ordering,
 *       hide_when_alone, and diagnostic display only. Kind / path / cmdline come from
 *       the signed UKI image -- never from this file. Disk-side path overrides are
 *       already rejected under UKI mode by include/boot/uki_cmdline_check.h; this
 *       file does NOT relax that contract.
 *   Tier 2 -- Split-path:
 *       BOOTX64.EFI Secure Boot signature (or its absence) is the trust anchor. The
 *       store is LOAD-BEARING for kernel-path / kind selection. CRC-32 in the envelope
 *       detects CORRUPTION; it is NOT authentication. An attacker with ESP write
 *       access can substitute a valid-CRC store. Signed entry stores (Ed25519 from
 *       the desktop-shell CNG crypto subsystem) are tracked as a Branch B follow-up;
 *       until then, the split-path threat model accepts the store as configuration,
 *       not authority.
 *
 * FORWARD COMPAT (pinned by this section):
 *   - Stable kinds:           0..99    (named in boot_entry_kind_t below; do not renumber)
 *   - Vendor / experimental:  100..199 (skip-with-warn for unknown values; never reject the store)
 *   - Reserved future:        >=200    (skip-with-warn)
 *   - schema_version is a hard-reject ONLY on envelope-incompatible bumps. Adding a
 *     new stable kind, a new optional payload field, or a new flag bit does NOT bump
 *     the version.
 *
 * FALLBACK CONTRACT (pinned here, implemented by the bootloader parser):
 *   When \EFI\ImpossibleOS\bootentries.json is missing, oversize, CRC-mismatched, or
 *   has schema_version > BOOT_ENTRIES_SCHEMA_VERSION, the bootloader synthesizes ONE
 *   entry that matches whatever path it currently loads:
 *     - UKI fast path active:   kind: uki   pointing at the loaded UKI image
 *     - Split-path active:      kind: split pointing at \EFI\ImpossibleOS\kernel.exe
 *   No A/B reference -- the slot-metadata TODO is not yet shipped; once it lands, the
 *   A/B-and-recovery integration TODO will widen the fallback to honor active slot.
 *
 * Header-only (no static-inline functions yet). The bootloader parser TODO will add a
 * parser; the per-entry-kind TODO will add per-kind validators. The C header is the
 * single source of truth for kind values + flag bits + size caps;
 * tools/boot-entry-validate/validate.py re-states these constants and is kept in sync
 * by a follow-up drift check (filed as a tooling / tests follow-up).
 *
 * Reference: docs/boot/boot-entry-schema.md (canonical spec, JSON examples, CRC algorithm).
 */

#ifndef BOOT_ENTRIES_H
#define BOOT_ENTRIES_H

/* No <stdint.h> include here. The bootloader builds -ffreestanding and the kernel uses
 * include/kernel/types.h for fixed-width typedefs. This header consumes neither: the
 * struct uses plain `unsigned int` and a _Static_assert below pins the size. Pattern
 * matches include/boot/uki_cmdline_check.h (header-only, no stdlib include).
 *
 * On every Impossible OS target (x86_64 LP64 / LLP64), `unsigned int` is exactly 32
 * bits. The static assert catches any future port where that does not hold.
 */
_Static_assert(sizeof(unsigned int) == 4, "boot_entries.h assumes 32-bit unsigned int");

/* Schema version. Bump only on envelope-incompatible changes (field add / remove /
 * reorder, type change, CRC algorithm change). New stable kinds, new optional payload
 * fields, and new flag bits MUST NOT trigger a bump.
 */
#define BOOT_ENTRIES_SCHEMA_VERSION 1u

/* Envelope size constraints. The bootloader parser enforces these; this header just
 * states them so producers (host validator + offline editor tooling) match.
 */
#define BOOT_ENTRIES_MAX_ENTRIES        64u
#define BOOT_ENTRIES_MAX_TOTAL_BYTES    (16u * 1024u)
#define BOOT_ENTRIES_MAX_TITLE_LEN      63u
#define BOOT_ENTRIES_MAX_ID_LEN         47u
#define BOOT_ENTRIES_MAX_PATH_LEN       255u
#define BOOT_ENTRIES_MAX_SORT_KEY_LEN   63u
#define BOOT_ENTRIES_UUID_TEXT_LEN      36u
#define BOOT_ENTRIES_MAX_POLICY_TAGS    4u
#define BOOT_ENTRIES_MAX_POLICY_TAG_LEN 23u
#define BOOT_ENTRIES_TIMEOUT_OVERRIDE_NONE 0xFFFFFFFFu

/* CRC-32 polynomial (IEEE 802.3 / zlib / PNG / Ethernet). Reuses the kernel-side
 * gpt_crc32() at src/kernel/fs/gpt.c when consumed kernel-side; the bootloader's
 * parser ships its own copy with the same polynomial.
 */
#define BOOT_ENTRIES_CRC32_POLY 0xEDB88320u

/* Discriminator. Stable kinds 0..99 do not change across schema versions; their
 * numeric values are part of the on-disk format and must not be renumbered.
 *
 * Per-kind payload field tables live in docs/boot/boot-entry-schema.md "Per-Kind Fields".
 */
typedef enum {
    BOOT_ENTRY_KIND_SPLIT       = 0,    /* split-payload: kernel + initrd[] + cmdline + root */
    BOOT_ENTRY_KIND_UKI         = 1,    /* unified PE under \EFI\Linux or \EFI\ImpossibleOS */
    BOOT_ENTRY_KIND_CHAINLOAD   = 2,    /* non-IPOS UEFI app (LoadImage / StartImage) */
    BOOT_ENTRY_KIND_NETWORK     = 3,    /* HTTP / TFTP target with sha256 digest */
    BOOT_ENTRY_KIND_RESUME      = 4,    /* hibernation snapshot (hibernation TODO) */
    BOOT_ENTRY_KIND_RECOVERY    = 5,    /* recovery partition target (recovery TODO) */
    BOOT_ENTRY_KIND_INSTALLER   = 6,    /* installer media role */
    BOOT_ENTRY_KIND_SAFE        = 7,    /* safe-mode entry */
    BOOT_ENTRY_KIND_DIAGNOSTICS = 8,    /* verbose POST + extended boot logging */
    BOOT_ENTRY_KIND_TEST        = 9,    /* TEST_CAT_* runner */
    /* 10..99 reserved for future stable kinds. */

    BOOT_ENTRY_KIND_VENDOR_FIRST    = 100u, /* 100..199 vendor / experimental: skip-with-warn */
    BOOT_ENTRY_KIND_VENDOR_LAST     = 199u
    /* >=200 reserved future use: skip-with-warn */
} boot_entry_kind_t;

/* Envelope flag bits (per-entry). Bits 5..31 are reserved and MUST be zero in v1. */
#define BOOT_ENTRY_FLAG_ACTIVE              (1u << 0)   /* visible in menu */
#define BOOT_ENTRY_FLAG_HIDDEN              (1u << 1)   /* present but not menu-listed */
#define BOOT_ENTRY_FLAG_TRUSTED_CHAINLOAD   (1u << 2)   /* required for kind: chainload + Secure Boot */
#define BOOT_ENTRY_FLAG_HIDE_WHEN_ALONE     (1u << 3)   /* skip menu render when only this entry visible */
#define BOOT_ENTRY_FLAG_ALLOW_EDITOR        (1u << 4)   /* user may edit cmdline at menu */

#define BOOT_ENTRY_FLAG_MASK_KNOWN_V1       0x0000001Fu

/* Header carried at the top of bootentries.json. The parser and host validator both
 * compute CRC-32 over the file bytes with the crc32 field's 8 hex digits zeroed (see
 * docs/boot/boot-entry-schema.md Section 5 "CRC-32 Algorithm") and compare to the
 * stored crc32 value. Mismatch is a hard parse error; the bootloader then
 * synthesizes the fallback entry per FALLBACK CONTRACT above.
 *
 * The struct layout here is informational -- the on-disk format is JSON. The
 * bootloader's parser reads the JSON header and populates a runtime equivalent.
 */
typedef struct {
    unsigned int schema_version;    /* must equal BOOT_ENTRIES_SCHEMA_VERSION for a v1 reader */
    unsigned int crc32;             /* IEEE 802.3 over file bytes with crc32 field zeroed */
    unsigned int entry_count;       /* informational; parser still bounds-checks against MAX_ENTRIES */
    unsigned int reserved;          /* must be 0 */
} boot_entries_header_t;

#endif /* BOOT_ENTRIES_H */
