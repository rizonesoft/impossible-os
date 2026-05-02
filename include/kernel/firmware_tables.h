/* ============================================================================
 * firmware_tables.h -- Unified firmware table catalog
 *
 * Aggregates firmware-provided platform tables (UEFI configuration table
 * entries, ACPI SDTs, SMBIOS structure-table, FPDT, ESRT, DTB) behind one
 * inventory API. Per-provider validation is owned by acpi.c / smbios.c /
 * uefi_config.c; this catalog records what was discovered and where, so
 * later subsystems and diagnostics can query firmware data without each
 * one re-walking boot_info.config_table[].
 *
 * Range and checksum validation against the UEFI memory map is the
 * responsibility of firmware_table_validate_all (declared below), which
 * uses the catalog populated here as its input set and runs from
 * firmware_tables_init before the boot summary tally.
 *
 * SMP: Built once on the BSP during Phase 1 boot (before sti). Read-only
 * thereafter -- no locks required.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_info.h"

/* Source kind: where the catalog entry came from. */
#define FW_SOURCE_UEFI_CFG_TABLE  1  /* boot_info.config_table[] entry */
#define FW_SOURCE_ACPI_SDT        2  /* one ACPI SDT (validated by acpi.c) */
#define FW_SOURCE_SMBIOS_RAW      3  /* SMBIOS structure-table region */
#define FW_SOURCE_FPDT            4  /* Firmware Performance Data Table */
#define FW_SOURCE_ESRT            5  /* EFI System Resource Table */
#define FW_SOURCE_DTB             6  /* Devicetree blob (EBBR) */

/* Validation status assigned at catalog time.
 *
 *   VALIDATED       -- a per-provider helper has confirmed success on this
 *                      entry (e.g. acpi_init parsed the RSDP, smbios_init
 *                      validated the SMBIOS entry point).
 *   UNKNOWN_PROFILE -- entry is present but no provider has validated it
 *                      yet.  Includes both unknown GUIDs and known-named
 *                      entries whose providers have no boot-time oracle
 *                      (FPDT/MAT/RtProps/Conformance/ESRT/DTB).
 *                      firmware_table_validate_all re-checks these.
 *   DEGRADED        -- entry was present but failed a check.  At catalog
 *                      time only the NULL VendorTable case sets this
 *                      (FW_DEGRADED_NULL_POINTER); the validator may
 *                      downgrade other entries with additional reasons. */
#define FW_STATUS_VALIDATED       0  /* per-provider helper succeeded */
#define FW_STATUS_DEGRADED        1  /* present but a check failed (see reason) */
#define FW_STATUS_UNKNOWN_PROFILE 2  /* present but no provider validation yet */

/* Reason codes for FW_STATUS_DEGRADED.  The catalog itself emits only
 * FW_DEGRADED_NULL_POINTER for NULL VendorTable entries; remaining
 * reason codes are owned by the validator. */
#define FW_DEGRADED_NONE           0
#define FW_DEGRADED_NULL_POINTER   1
#define FW_DEGRADED_RANGE_UNMAPPED 2
#define FW_DEGRADED_CHECKSUM_FAIL  3
#define FW_DEGRADED_LENGTH_BAD     4

#define FIRMWARE_TABLE_NAME_MAX  16
#define FIRMWARE_TABLE_OWNER_MAX 16
/* Cap: 32 cfg-table slots + headroom for ACPI SDTs (typical 12-20).
 * Excess SDTs beyond the cap are dropped with a single LOG_WARN. */
#define FIRMWARE_TABLE_MAX       64

struct firmware_table_entry {
    struct boot_uefi_guid guid;       /* zeroed for ACPI SDT entries */
    uint32_t  signature;              /* nonzero only for ACPI SDTs (LE 'FACP' = 0x50434146) */
    uintptr_t phys_addr;              /* firmware-mapped physical address */
    uint32_t  size;                   /* bytes; 0 if unknown at catalog time */
    uint32_t  computed_checksum;      /* sum-of-bytes mod 0x100 over the table when validated;
                                       * 0 when the validator did not compute one (catalog-only
                                       * entries: UEFI cfg-table GUIDs that have no full-format
                                       * validator yet, DTB header / vendor blobs, etc.) */
    uint8_t   source;                 /* FW_SOURCE_* */
    uint8_t   status;                 /* FW_STATUS_* */
    uint8_t   degraded_reason;        /* FW_DEGRADED_* (populated by validator) */
    uint8_t   _pad;
    char      name[FIRMWARE_TABLE_NAME_MAX];   /* short label: "ACPI2.0", "FACP", ... */
    char      owner[FIRMWARE_TABLE_OWNER_MAX]; /* subsystem owner: "UEFI", "ACPI", ... */
};

/* Build the catalog from boot_info.config_table[] plus ACPI/SMBIOS provider
 * accessors. Idempotent: subsequent calls return without rescanning. Must be
 * called on the BSP after uefi_config_init(), acpi_init(), smbios_init(),
 * esrt_init(), mat_init(), and uefi_conformance_init(). Emits one boot log:
 *
 *   [BOOT] firmware tables: <N> cataloged, <M> validated, <K> degraded
 */
void firmware_tables_init(void);

/* Number of catalog entries populated by firmware_tables_init().
 * Returns 0 before init or when no firmware tables were discovered. */
uint32_t firmware_table_count(void);

/* Get the n-th catalog entry (read-only). Returns NULL if index is out
 * of range. Pointer is stable for the kernel's lifetime. */
const struct firmware_table_entry *firmware_table_get(uint32_t index);

/* Look up a UEFI configuration-table entry by GUID. Returns the first
 * matching FW_SOURCE_UEFI_CFG_TABLE entry, or NULL on not-found / NULL guid. */
const struct firmware_table_entry *
firmware_table_lookup_guid(const struct boot_uefi_guid *guid);

/* Look up by short name (case-sensitive exact match against entry->name).
 * Useful for tests and diagnostics (e.g. "FACP", "SMBIOS3"). */
const struct firmware_table_entry *
firmware_table_lookup_name(const char *name);

/* Collect every entry whose owner matches the given subsystem string
 * (case-sensitive exact match). Writes up to max_out pointers into out[]
 * and returns the total number of matches found (may exceed max_out --
 * caller can re-call with a larger array). out may be NULL only when
 * max_out == 0 (count-only mode). */
uint32_t firmware_table_lookup_owner(const char *owner,
                                      const struct firmware_table_entry **out,
                                      uint32_t max_out);

/* Re-validate every catalog entry against the UEFI memory map and recompute
 * per-source checksums / length invariants. Entries already FW_STATUS_DEGRADED
 * are left untouched (one-way downgrade). On failure, sets entry->status =
 * FW_STATUS_DEGRADED and entry->degraded_reason to one of:
 *
 *   FW_DEGRADED_RANGE_UNMAPPED -- phys_addr (and span if size>0) is not
 *                                 contained in a single UEFI memory-map entry
 *                                 of a firmware-bearing type (RESERVED,
 *                                 BOOT/RUNTIME/LOADER_*, ACPI_RECLAIM, ACPI_NVS).
 *                                 Reject CONVENTIONAL/UNUSABLE/MMIO/PAL.
 *   FW_DEGRADED_LENGTH_BAD     -- header length below required minimum, or
 *                                 declared length disagrees with catalog size,
 *                                 or count*entry-size overruns the table region.
 *   FW_DEGRADED_CHECKSUM_FAIL  -- 8-bit byte-sum-to-zero check failed (ACPI
 *                                 RSDP / SDT / SMBIOS entry-point / FPDT).
 *
 * Idempotent: running a second time produces the same result and never flips
 * a DEGRADED entry back to VALIDATED. Called once from firmware_tables_init()
 * before the boot summary so the cataloged/validated/degraded counts reflect
 * validator findings; can also be invoked from tests. */
void firmware_table_validate_all(void);

/* Mirror the firmware-table catalog into HKLM\HARDWARE\Firmware\Tables.
 * Writes one subkey per cataloged entry, keyed by the entry's catalog
 * name (sanitized for Registry naming rules), with values Address,
 * Size, Checksum, ValidationStatus, Source mirroring the JSON wire
 * format pinned by docs/boot/firmware-tables-schema.md.  Idempotent
 * via RegDeleteTree on entry.  ESRT subtree (HARDWARE\Firmware\ESRT)
 * is NOT touched -- it is owned by esrt_populate_registry. */
void firmware_tables_populate_registry(void);

/* Publish the firmware-table catalog to X:\Diag\firmware-tables.json
 * conforming to schema_version=1 (canonical wire format pinned in
 * docs/boot/firmware-tables-schema.md).  Manual JSON formatting,
 * 16 KiB pmm_alloc_contiguous buffer, write via VFS.  Wire from
 * boot_phase3() AFTER VFS+IXFS mount (mirrors boot_history_kernel_
 * mark_phase3 timing).  Idempotent on repeated calls within the
 * same boot via VFS file overwrite. */
void firmware_tables_publish_json(void);

/* True iff `[addr, addr+len)` is contained in a single UEFI memory-map
 * descriptor of a firmware-bearing type (RESERVED / RUNTIME_* /
 * ACPI_RECLAIM / ACPI_NVS / PERSISTENT).
 *
 * Post-PMM-reclaim contract: REJECTS UEFI_MMAP_BOOT_SERVICES_CODE/DATA
 * AND UEFI_MMAP_LOADER_CODE/DATA along with CONVENTIONAL / UNUSABLE /
 * MMIO / MMIO_PORT / PAL_CODE.  PMM reclaims BootServices and Loader
 * memory in Phase 0 before the firmware-table catalog walk in Phase 1,
 * so any firmware structure pointed into either type may have been
 * overwritten by the kernel allocator by the time a Phase-1+ consumer
 * reads it.
 *
 * Phase-0 callers that run BEFORE `pmm_init()` reclaims BootServices
 * (e.g. `uefi_runtime_init` reading `EFI_RT_PROPERTIES_TABLE` from
 * firmware-published BootServicesData) MUST use
 * `firmware_table_mmap_contains_pre_reclaim()` instead.
 *
 * Also rejects len==0 / addr+len wraparound.  Exposed so consumers
 * parsing additional firmware structures (DTB header walk, future
 * ESRT entry array walk) can re-use the same firmware-region oracle
 * without re-implementing the type allowlist. */
int firmware_table_mmap_contains(uintptr_t addr, uint64_t len);

/* Phase-0 pre-PMM-reclaim variant: also accepts UEFI_MMAP_BOOT_SERVICES_
 * CODE/DATA AND UEFI_MMAP_LOADER_CODE/DATA as firmware-owned. Use when
 * the caller runs before `pmm_init()` recovers those pages to the
 * free-page pool (e.g. `uefi_runtime_init` -> `read_rt_properties`
 * reading the EFI_RT_PROPERTIES_TABLE which firmware may publish in
 * BootServicesData or LoaderData). Phase-1+ callers (firmware-table
 * catalog walk, validators, MAT inventory, ECPT detection) MUST use
 * the standard `firmware_table_mmap_contains()` to reject those types. */
int firmware_table_mmap_contains_pre_reclaim(uintptr_t addr, uint64_t len);

/* Promote a single named catalog entry from FW_STATUS_UNKNOWN_PROFILE
 * to FW_STATUS_VALIDATED after a deferred provider oracle has finished
 * its full-format check. Used by `firmware_platform_init` once
 * `dtb_init` has accepted the FDT header + structure walk; without
 * this call the catalog and `HKLM\SYSTEM\Boot\Firmware` would
 * disagree on whether the DTB is trusted.
 *
 * One-way: never demotes a DEGRADED entry, never touches a
 * VALIDATED entry. Returns 1 iff a transition occurred. */
int firmware_table_promote_to_validated(const char *name);

#ifdef KERNEL_TESTS
/* Test-only single-entry validator used by `test_firmware_tables.c` to
 * exercise checksum / length / range-unmapped paths against synthetic
 * `firmware_table_entry` structures pointing at test-owned buffers.
 *
 * `bypass_range_check` skips the UEFI memory-map containment check so
 * tests can validate checksum / length logic against buffers in kernel
 * BSS (which sits in EfiConventionalMemory after ExitBootServices and
 * would otherwise trip RANGE_UNMAPPED). Set to 0 to exercise the range
 * check itself against a real mmap address.
 *
 * Production callers MUST use `firmware_table_validate_all`. */
void firmware_table_validate_one_for_test(struct firmware_table_entry *entry,
                                          int bypass_range_check);

/* Test-only toggle for the firmware-region oracle used by
 * `firmware_table_mmap_contains` / private `fw_mmap_contains`. When
 * set, the oracle returns 1 unconditionally so tests can validate
 * DTB / firmware-table consumers against fixture buffers in kernel
 * BSS. Toggle ONLY around the call under test; reset before yielding
 * to other test code. Single-threaded test runner; no concurrent
 * exposure. */
void firmware_table_set_mmap_bypass_for_test(int bypass);
#endif
