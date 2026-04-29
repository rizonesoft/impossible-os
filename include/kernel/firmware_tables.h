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
 * responsibility of the firmware-table validator (firmware_table_validate_all,
 * landing in TODO-04 physical-range-and-checksum-validation), which uses the
 * catalog populated here as its input set.
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
 * The firmware-table validator (later TODO-04 section) may downgrade entries
 * to FW_STATUS_DEGRADED with a reason code; the catalog itself only records
 * what the per-provider helper already accepted. */
#define FW_STATUS_VALIDATED       0  /* per-provider helper succeeded */
#define FW_STATUS_DEGRADED        1  /* present but a check failed (see reason) */
#define FW_STATUS_UNKNOWN_PROFILE 2  /* present but profile/format not recognised */

/* Reason codes for FW_STATUS_DEGRADED (populated by the validator -- the
 * catalog leaves this at FW_DEGRADED_NONE). */
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
