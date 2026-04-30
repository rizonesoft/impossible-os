/* ============================================================================
 * firmware_platform.h -- ACPI/DTB/Hybrid arbitration policy
 *
 * Picks one of FW_PLATFORM_ACPI / FW_PLATFORM_DTB / FW_PLATFORM_HYBRID /
 * FW_PLATFORM_UNKNOWN at boot based on which firmware tables the
 * unified catalog discovered and the validator promoted to
 * FW_STATUS_VALIDATED, plus the FDT header validator's verdict on any
 * DTB blob.
 *
 * Doctrine: PC-class hardware MUST use ACPI for interrupt and timer
 * ownership when ACPI is present. The policy below honours that by
 * preferring ACPI over DTB whenever both are advertised: HYBRID is
 * recorded as a diagnostic (primary=ACPI, secondary=DTB), not as a
 * permission to mix interrupt sources. Linux kernel arbitrates the
 * same way on ARM SBSA + DTB systems.
 *
 * Registry: HKLM\SYSTEM\Boot\Firmware\* gets one REG_SZ
 * (FirmwarePlatform = "ACPI" / "DTB" / "Hybrid" / "Unknown") plus
 * REG_DWORD presence flags HasACPI / HasDTB / HasSMBIOS and the ACPI
 * version (0/1/2). Sibling key of HKLM\SYSTEM\Boot\Device\* populated
 * by the boot-device-discovery Registry hook; reserved exclusively
 * from the firmware-tables inventory key under
 * HKLM\HARDWARE\Firmware\Tables (owned by the firmware report TODO).
 *
 * SMP: BSP-only init at Phase 1 after firmware_table_validate_all.
 * Read-only thereafter -- lock-free getters.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

enum fw_platform {
    FW_PLATFORM_UNKNOWN = 0,
    FW_PLATFORM_ACPI    = 1,
    FW_PLATFORM_DTB     = 2,
    FW_PLATFORM_HYBRID  = 3,  /* ACPI + DTB; ACPI is primary (PC-class doctrine) */
};

/* Decide the platform classification, log the result, and write the
 * Boot\Firmware Registry key. Idempotent: subsequent calls are no-ops. */
void firmware_platform_init(void);

/* Read the platform classification computed by firmware_platform_init.
 * Returns FW_PLATFORM_UNKNOWN before init. */
enum fw_platform firmware_platform_get(void);

/* Short stable string for logs / Registry. Always non-NULL. */
const char *firmware_platform_name(enum fw_platform p);

/* Boolean accessors used by the Registry mirror + diagnostic callers. */
int firmware_platform_has_acpi(void);
int firmware_platform_has_dtb(void);
int firmware_platform_has_smbios(void);

/* ACPI revision (0 = none, 1 = ACPI 1.0 RSDP, 2 = ACPI 2.0+ RSDP).
 * Mirrors the value the catalog uses to label the cfg-table entry. */
uint32_t firmware_platform_acpi_version(void);
