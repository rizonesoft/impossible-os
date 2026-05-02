/* ============================================================================
 * uefi_config.h -- UEFI Configuration Table Walker
 *
 * Searches the UEFI configuration table (copied into boot_info by the
 * bootloader) for entries identified by GUID.  Provides uefi_find_config_table()
 * and an init function that logs all discovered tables.
 * ============================================================================ */

#pragma once

#include "kernel/boot_info.h"

/* Initialize the UEFI config table walker. Logs all discovered tables. */
void uefi_config_init(void);

/* Search the config table for a specific GUID. Returns the physical address
 * of the vendor table, or 0 if not found. */
uintptr_t uefi_find_config_table(const struct boot_uefi_guid *guid);

/* ---- Conformance Profile Detection ---- */

/* Well-known conformance profile GUIDs (UEFI 2.10 section 4.6.5).
 *
 * The ECPT (EFI_CONFORMANCE_PROFILES_TABLE) is an ARRAY of profile GUIDs
 * the firmware supports, not an ordered choice -- a system can claim
 * UEFI Spec AND EBBR simultaneously.  Detection must therefore track
 * per-profile presence, not just a single "winner."
 *
 * UEFI Spec GUID per UEFI 2.10 section 4.6.5; EBBR GUID per Arm EBBR
 * 2.1 (cce33c35-... published by U-Boot mailing list, EBBR 2.0 has no
 * separate GUID -- a single EBBR class profile is intentional).
 *
 * IMPORTANT (audit fix 2026-05-02): the previous values for both
 * GUIDs (4b2a-9a5a-d00dd31a2427 / 4b2a-9088-58d50682f149) were
 * fabricated; ECPT detection therefore never matched real firmware.
 * The values below match the canonical UEFI 2.10 / Arm EBBR 2.1
 * specifications byte-for-byte. */
#define UEFI_PROFILE_UEFI_SPEC \
    ((struct boot_uefi_guid){ 0x523c91af, 0xa195, 0x4382, \
        { 0x81, 0x8d, 0x29, 0x5f, 0xe4, 0x00, 0x64, 0x65 } })
#define UEFI_PROFILE_EBBR \
    ((struct boot_uefi_guid){ 0xcce33c35, 0x74ac, 0x4087, \
        { 0xbc, 0xe7, 0x8b, 0x29, 0xb0, 0x2e, 0xeb, 0x27 } })

/* Profile-id enum used by uefi_conformance_has_profile().  Internal
 * ordering only; consumers reference symbolic names, not values. */
enum uefi_conformance_profile_id {
    UEFI_PROFILE_ID_UEFI_SPEC = 0,
    UEFI_PROFILE_ID_EBBR      = 1,
    UEFI_PROFILE_ID__COUNT
};

/* Backward-compat scalar levels (kept for callers that already query
 * uefi_conformance_level()).  New code SHOULD use the per-profile
 * presence + omit / contradiction queries below. */
#define UEFI_CONFORM_FULL     0  /* UEFI Spec profile claimed (or table absent) */
#define UEFI_CONFORM_EBBR     1  /* EBBR profile claimed and UEFI Spec was NOT */
#define UEFI_CONFORM_UNKNOWN  2  /* Table present but no known profile matched */

/* Initialize conformance profile detection.
 * Reads EFI_CONFORMANCE_PROFILES_TABLE from UEFI configuration tables;
 * each GUID access is bounded via firmware_table_mmap_contains() so a
 * malformed NumberOfProfiles cannot drive an overread, and a defensive
 * cap (UEFI_CONFORM_PROFILE_MAX) limits per-table walk cost.
 *
 * Init-order: requires uefi_config_init() (config-table lookup) AND
 * a populated g_boot_info.mmap[] (the firmware-region oracle reads
 * boot_info directly).  firmware_tables_init() is NOT a dependency:
 * the catalog calls into this header to read profile state, so the
 * Phase 1 boot path runs uefi_conformance_init() FIRST then
 * firmware_tables_init() -- inverting that order would be the bug. */
void uefi_conformance_init(void);

/* Defensive cap on profile count -- ECPT carries no total length, so
 * this bounds the walk independent of firmware-supplied count. */
#define UEFI_CONFORM_PROFILE_MAX 16

/* Backward-compat scalar level (UEFI_CONFORM_*).  Computed as: UEFI
 * Spec present -> FULL; otherwise EBBR present -> EBBR; otherwise
 * UNKNOWN (table parsed but no match) or FULL (table absent / pre-2.10
 * firmware).  Prefer the per-profile queries below for new policy. */
int uefi_conformance_level(void);

/* Per-profile presence query.  Returns 1 iff the firmware advertised
 * the matching GUID in ECPT this boot.  Drives EBBR-omit and PC-
 * contradiction policy: a system that claims BOTH UEFI Spec AND EBBR
 * still has has_profile(EBBR)==1 so the PC-contradiction warning
 * fires (UEFI 2.10 section 4.6.5: ECPT is an array, not a winner). */
int uefi_conformance_has_profile(enum uefi_conformance_profile_id id);

/* Display name of the matched profile, e.g. "UEFI Spec", "EBBR",
 * "UEFI Spec + EBBR" (both claimed).  Returns "unknown" when ECPT is
 * present but no row matched, or "Full UEFI (assumed)" when ECPT is
 * absent.  Display-only -- do not parse for policy. */
const char *uefi_conformance_name(void);

/* Required-table policy: 1 iff the matched profile permits firmware
 * to omit PC-class tables (FPDT, MAT, RTProps).  EBBR-class profile
 * present -> 1; UEFI Spec only -> 0; absent / unknown profile -> 0
 * (require by default; firmware that elides without telling us is
 * the worse failure mode). */
int uefi_conformance_allows_omit_pc_tables(void);

/* PC / EBBR contradiction detector: 1 iff the firmware claims an
 * EBBR-class profile AND we are running on x86_64 (which requires
 * legacy PC-only assumptions: PIC space, i8042 ports, RTC port 0x70).
 * On non-x86 architectures this always returns 0 -- the contradiction
 * is x86-specific by name (no PC-only hardware to contradict). */
int uefi_conformance_pc_contradiction(void);

/* ---- ESRT Firmware Inventory (UEFI 2.5+ §23.4) ---- */

#define UEFI_GUID_ESRT \
    ((struct boot_uefi_guid){ 0xb122a263, 0x3661, 0x4f68, \
        { 0x99, 0x29, 0x78, 0xf8, 0xb0, 0xd6, 0x21, 0x80 } })

/* Firmware type constants */
#define ESRT_FW_TYPE_UNKNOWN        0
#define ESRT_FW_TYPE_SYSTEM         1  /* System firmware (BIOS/UEFI) */
#define ESRT_FW_TYPE_DEVICE         2  /* Device firmware (EC, TB, etc.) */
#define ESRT_FW_TYPE_UEFI_DRIVER    3  /* UEFI driver */

/* Last attempt status codes (UEFI 2.10 Table 23-3).  Codes 0x06/0x07
 * were renamed in UEFI 2.7 (AC_NOT_CONNECTED -> PWR_EVT_AC,
 * INSUFFICIENT_BATTERY -> PWR_EVT_BATT) and 0x08
 * UNSATISFIED_DEPENDENCIES added in 2.7+; the canonical UEFI 2.10
 * mnemonics are pinned here so registry, JSON, BlackBox, and
 * operator UX consumers all see one stable contract. */
#define ESRT_STATUS_SUCCESS                       0x00000000
#define ESRT_STATUS_ERROR_UNSUCCESSFUL            0x00000001
#define ESRT_STATUS_ERROR_INSUFFICIENT_RESOURCES  0x00000002
#define ESRT_STATUS_ERROR_INCORRECT_VERSION       0x00000003
#define ESRT_STATUS_ERROR_INVALID_FORMAT          0x00000004
#define ESRT_STATUS_ERROR_AUTH_ERROR              0x00000005
#define ESRT_STATUS_ERROR_PWR_EVT_AC              0x00000006
#define ESRT_STATUS_ERROR_PWR_EVT_BATT            0x00000007
#define ESRT_STATUS_ERROR_UNSATISFIED_DEPENDENCIES 0x00000008

/* Capsule flag bits (UEFI 2.10 section 8.5.3).  ESRT entry carries
 * `capsule_flags` describing how the firmware update capsule must
 * behave.  Only the bit consumed by esrt_capsule_persists_across_reset
 * is named here; capsule-policy owner adds others as needed. */
#define EFI_CAPSULE_PERSIST_ACROSS_RESET    0x00010000

/* EFI_SYSTEM_RESOURCE_ENTRY (one per firmware component) */
struct esrt_entry {
    struct boot_uefi_guid fw_class;    /* identifies the firmware component */
    uint32_t fw_type;                  /* ESRT_FW_TYPE_* */
    uint32_t fw_version;               /* current version */
    uint32_t lowest_supported_version; /* rollback protection floor */
    uint32_t capsule_flags;            /* update delivery method */
    uint32_t last_attempt_version;     /* version of last update attempt */
    uint32_t last_attempt_status;      /* ESRT_STATUS_* */
};

#define ESRT_MAX_ENTRIES 16

/* Initialize ESRT -- parse EFI_SYSTEM_RESOURCE_TABLE from config table.
 * Must be called after uefi_config_init(). */
void esrt_init(void);

/* Returns number of firmware resource entries. */
uint32_t esrt_count(void);

/* Returns pointer to n-th ESRT entry, or NULL if out of range. */
const struct esrt_entry *esrt_get_entry(uint32_t index);

/* Header metadata accessors (UEFI 2.10 section 23.6 EFI_SYSTEM_RESOURCE_TABLE).
 * fw_resource_count and fw_resource_count_max bound the table; readers
 * iterate in [0, esrt_count()) via esrt_get_entry().  fw_resource_version
 * is firmware-defined (currently always 1 in practice). */
uint32_t esrt_resource_count_max(void);
uint64_t esrt_resource_version(void);

/* Decoder helpers -- static const char * tables; safe to print directly.
 * Status names are the unprefixed UEFI 2.10 Table 23-3 mnemonics
 * ("SUCCESS", "ERROR_UNSUCCESSFUL", ...).  Type names are the
 * UEFI 2.10 section 23.6 categories ("System", "Device", "Driver",
 * "Unknown"). */
const char *esrt_decode_status(uint32_t status);
const char *esrt_decode_type(uint32_t fw_type);

/* Capsule-policy primitives consumed by the capsule-update owner
 * (TODO-27 advanced UEFI work) -- intentionally narrow.  These are
 * NOT authoritative eligibility predicates; the capsule-policy owner
 * composes them with FwType, LastAttemptStatus, and auth checks.
 *
 * esrt_rollback_floor_ok(idx): 1 iff FwVersion >= LowestSupportedFwVersion
 * (the rollback gate from UEFI 2.10 section 23.6 LSV semantics).
 *
 * esrt_capsule_persists_across_reset(idx): 1 iff CapsuleFlags &
 * EFI_CAPSULE_PERSIST_ACROSS_RESET (0x00010000 per UEFI 2.10
 * section 8.5.3).  Says capsule survives the next reset; says
 * nothing about whether the resource itself is updatable.
 *
 * Both helpers return 0 on out-of-range idx. */
int esrt_rollback_floor_ok(uint32_t idx);
int esrt_capsule_persists_across_reset(uint32_t idx);

/* Mirror ESRT into HKLM\HARDWARE\Firmware\ESRT.  Idempotent: clears the
 * subtree on entry, then writes one subkey per entry keyed by the
 * canonical Microsoft brace-form FwClass GUID, plus a sibling _Header
 * subkey carrying ResourceCount / ResourceCountMax / ResourceVersion.
 * ESRT-absent boots leave the parent key empty (no stale per-resource
 * keys carry over). */
void esrt_populate_registry(void);

/* ---- Memory Attributes Table (UEFI 2.6+ §4.6.4) ---- */

/* EFI memory attribute flags (from UEFI spec) */
#define EFI_MEMORY_RO   0x0000000000020000ULL  /* Read-only (no write) */
#define EFI_MEMORY_XP   0x0000000000004000ULL  /* Non-executable (NX bit) */
#define EFI_MEMORY_RP   0x0000000000002000ULL  /* Not present (guard page) */

/* EFI_MEMORY_ATTRIBUTES_TABLE header */
struct efi_memory_attributes_table {
    uint32_t version;
    uint32_t number_of_entries;
    uint32_t descriptor_size;
    uint32_t reserved;
    /* followed by number_of_entries × EFI_MEMORY_DESCRIPTOR */
};

/* Initialize Memory Attributes Table -- parse and log W^X status.
 * Must be called after uefi_config_init(). */
void mat_init(void);

/* Returns 1 if MAT was present and all regions pass W^X check. */
int mat_wxn_enforced(void);

/* ---- MAT region inventory (consumer API for TODO-27 W^X enforcement) ---- */

/* Region classification derived from RP/RO/XP attribute bits. */
typedef enum {
    MAT_CLASS_GUARD = 0,    /* RP set: page is not present */
    MAT_CLASS_CODE,         /* RO + executable (no XP): runtime code */
    MAT_CLASS_DATA,         /* RW + XP: runtime data */
    MAT_CLASS_RODATA,       /* RO + XP: read-only constants */
    MAT_CLASS_WX_VIOLATION  /* writable AND executable: W^X violation */
} mat_class_t;

typedef struct {
    uint64_t    phys_addr;     /* physical_start from descriptor */
    uint64_t    num_pages;     /* descriptor pages (4 KiB each) */
    uint64_t    attribute;     /* raw EFI_MEMORY_* attribute bits */
    mat_class_t cls;           /* derived classification */
} mat_entry_t;

/* Cap on cached MAT entries; bare-metal laptops typically expose <50,
 * OVMF reports ~10-20. 128 leaves headroom without inflating .bss. */
#define MAT_MAX_ENTRIES 128

/* Returns count of cached MAT entries (clamped to MAT_MAX_ENTRIES).
 * 0 if MAT was absent or unparseable. */
uint32_t mat_get_count(void);

/* Copy entry `idx` into `*out`. Returns 1 on success, 0 if idx out of range
 * or `out` is NULL. Read-only after mat_init(); SMP-safe lock-free. */
int mat_get_entry(uint32_t idx, mat_entry_t *out);

/* Aggregate page counts (4 KiB each) by classification. */
uint64_t mat_get_code_pages(void);
uint64_t mat_get_data_pages(void);
uint64_t mat_get_guard_pages(void);

/* Returns 1 if firmware reported more descriptors than MAT_MAX_ENTRIES
 * (the entire table was rejected to avoid publishing a truncated
 * inventory). Combined with mat_get_count()==0 this lets consumers
 * distinguish "no MAT available" from "MAT was too large to cache
 * safely" -- both leave the inventory empty but only the latter
 * indicates firmware data the kernel deliberately refused. */
int mat_overflowed(void);

/* Pure classification helper for synthetic testing: takes an
 * EFI_MEMORY_* attribute bitmask and returns the derived class. */
mat_class_t mat_classify_attr(uint64_t attr);
