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
