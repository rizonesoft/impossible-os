/* ============================================================================
 * firmware_quirks.h -- Firmware quirk database
 *
 * Detects known-broken firmware behaviors by matching SMBIOS vendor/product/
 * BIOS-version strings against a static descriptor table. Each quirk is a
 * single bit in the active mask; consumers gate workarounds on the bit.
 *
 * Quirks shipped in the firmware quirk database:
 *   FW_QUIRK_BROKEN_FPDT       -- FPDT timestamps are unreliable / all-zero
 *   FW_QUIRK_BAD_MADT_CHECKSUM -- MADT byte-sum != 0 (firmware bug)
 *   FW_QUIRK_GOP_PITCH_LIES    -- GOP PixelsPerScanLine != actual stride
 *   FW_QUIRK_BOGUS_MAT         -- Memory Attributes Table is malformed
 *   FW_QUIRK_USB_HANDOFF_BLACKLIST -- skip BIOS->OS USB handover
 *   FW_QUIRK_EC_ECDT_PORTS_SWAPPED -- ECDT names EC_CONTROL/EC_DATA swapped
 *
 * SMP: Built once on the BSP during Phase 1 (after smbios_init, before
 * firmware_tables_init). Read-only thereafter -- no locks required.
 *
 * Override: boot_config.firmware_quirk_disable bitmask suppresses individual
 * quirks at boot.conf load time (`firmware_quirk_disable=broken_fpdt`).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Quirk bit positions -- defined via shared X-macro so the bootloader
 * override parser (src/boot/uefi/bootx64.c) and the kernel descriptor
 * table (src/kernel/firmware_quirks.c) cannot drift on names or bit
 * values. Adding a new quirk: append a single line to
 * firmware_quirks_table.inc; FW_QUIRK_COUNT auto-updates. */
#define FW_QUIRK_DEF(id, bit, name) FW_QUIRK_##id = bit,
enum {
#include "kernel/firmware_quirks_table.inc"
};
#undef FW_QUIRK_DEF

/* Total quirks defined; computed via X-macro expansion. */
#define FW_QUIRK_DEF(id, bit, name) +1
enum { FW_QUIRK_COUNT = (0
#include "kernel/firmware_quirks_table.inc"
) };
#undef FW_QUIRK_DEF

/* Idempotent. Walks the static quirk descriptor table, evaluates each
 * predicate against `smbios_get_info()`, and records the active bitmask
 * (with override applied from boot_config.firmware_quirk_disable).
 * Emits one LOG_INFO line:
 *   [BOOT] firmware quirks: <N> active[: name1,name2,...]
 * Safe to call before SMBIOS is parsed (predicates short-circuit on
 * NULL info / valid==0); active mask stays 0 in that case. */
void firmware_quirks_init(void);

/* Active quirk bitmask after init. 0 before firmware_quirks_init() runs
 * AND on firmware that triggers no quirks. Bits use FW_QUIRK_* values. */
uint32_t firmware_quirks_active_mask(void);

/* True iff `bit` (single FW_QUIRK_* value) is set in the active mask. */
int firmware_quirks_is_active(uint32_t bit);

/* Number of quirks set in the active mask. */
uint32_t firmware_quirks_active_count(void);

/* Canonical quirk name for `bit` (single FW_QUIRK_* value). NULL on
 * out-of-range / non-power-of-two input. Stable for the kernel's
 * lifetime; used by JSON writer + Registry mirror + BlackBox. */
const char *firmware_quirks_name(uint32_t bit);

/* Iterate active quirks: returns the next active bit AFTER `prev_bit`
 * (pass 0 to start), or 0 when no more bits. Use:
 *   for (uint32_t b = firmware_quirks_iter_next(0);
 *        b; b = firmware_quirks_iter_next(b)) { ... }
 */
uint32_t firmware_quirks_iter_next(uint32_t prev_bit);

/* Parse a comma-separated quirk-name list ("broken_fpdt,bogus_mat") into
 * a bitmask. Unknown names are silently ignored. NULL or empty input
 * returns 0. Used by both bootloader (boot.conf parser) and unit
 * tests. Pure function -- no kernel state read. */
uint32_t firmware_quirks_parse_disable(const char *list);
