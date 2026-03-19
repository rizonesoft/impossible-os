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
