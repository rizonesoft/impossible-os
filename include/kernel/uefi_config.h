/* ============================================================================
 * uefi_config.h — UEFI Configuration Table Walker
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

/* Well-known conformance profile GUIDs (UEFI 2.10 §4.6) */
#define UEFI_PROFILE_UEFI_SPEC \
    ((struct boot_uefi_guid){ 0x523c91af, 0xa195, 0x4b2a, \
        { 0x9a, 0x5a, 0xd0, 0x0d, 0xd3, 0x1a, 0x24, 0x27 } })
#define UEFI_PROFILE_EBBR \
    ((struct boot_uefi_guid){ 0xcce33c35, 0x74ac, 0x4b2a, \
        { 0x90, 0x88, 0x58, 0xd5, 0x06, 0x82, 0xf1, 0x49 } })

/* Conformance levels */
#define UEFI_CONFORM_FULL     0  /* Full UEFI 2.10 (or table absent) */
#define UEFI_CONFORM_EBBR     1  /* Embedded Base Boot Requirements (minimal) */
#define UEFI_CONFORM_UNKNOWN  2  /* Conformance table present but no known profile */

/* Initialize conformance profile detection.
 * Reads EFI_CONFORMANCE_PROFILES_TABLE from config table.
 * Must be called after uefi_config_init(). */
void uefi_conformance_init(void);

/* Returns UEFI_CONFORM_FULL, UEFI_CONFORM_EBBR, or UEFI_CONFORM_UNKNOWN. */
int uefi_conformance_level(void);

/* ---- ESRT Firmware Inventory (UEFI 2.5+ §23.4) ---- */

#define UEFI_GUID_ESRT \
    ((struct boot_uefi_guid){ 0xb122a263, 0x3661, 0x4f68, \
        { 0x99, 0x29, 0x78, 0xf8, 0xb0, 0xd6, 0x21, 0x80 } })

/* Firmware type constants */
#define ESRT_FW_TYPE_UNKNOWN        0
#define ESRT_FW_TYPE_SYSTEM         1  /* System firmware (BIOS/UEFI) */
#define ESRT_FW_TYPE_DEVICE         2  /* Device firmware (EC, TB, etc.) */
#define ESRT_FW_TYPE_UEFI_DRIVER    3  /* UEFI driver */

/* Last attempt status codes */
#define ESRT_STATUS_SUCCESS                 0x00000000
#define ESRT_STATUS_ERROR_UNSUCCESSFUL      0x00000001
#define ESRT_STATUS_ERROR_INSUFFICIENT_RESOURCES 0x00000002
#define ESRT_STATUS_ERROR_INCORRECT_VERSION  0x00000003
#define ESRT_STATUS_ERROR_INVALID_FORMAT    0x00000004
#define ESRT_STATUS_ERROR_AUTH_ERROR        0x00000005
#define ESRT_STATUS_ERROR_AC_NOT_CONNECTED  0x00000006
#define ESRT_STATUS_ERROR_INSUFFICIENT_BATTERY 0x00000007

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

/* Initialize ESRT — parse EFI_SYSTEM_RESOURCE_TABLE from config table.
 * Must be called after uefi_config_init(). */
void esrt_init(void);

/* Returns number of firmware resource entries. */
uint32_t esrt_count(void);

/* Returns pointer to n-th ESRT entry, or NULL if out of range. */
const struct esrt_entry *esrt_get_entry(uint32_t index);

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

/* Initialize Memory Attributes Table — parse and log W^X status.
 * Must be called after uefi_config_init(). */
void mat_init(void);

/* Returns 1 if MAT was present and all regions pass W^X check. */
int mat_wxn_enforced(void);
