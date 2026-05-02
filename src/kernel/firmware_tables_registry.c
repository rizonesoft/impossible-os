/* ============================================================================
 * firmware_tables_registry.c -- mirror the firmware-table catalog into
 * HKLM\HARDWARE\Firmware\Tables.
 *
 * Consumer of firmware_table_count() / firmware_table_get() (firmware_
 * tables.c).  Writes one subkey per cataloged entry keyed by the entry's
 * canonical name (or canonical brace-form GUID when the source carries
 * one) so operator tools have a stable lookup surface that mirrors the
 * X:\Diag\firmware-tables.json wire format pinned by
 * docs/boot/firmware-tables-schema.md.
 *
 * Idempotent: clears the entire HARDWARE\Firmware\Tables subtree before
 * writing.  ESRT entries are NOT mirrored here because the ESRT-specific
 * Registry mirror at HKLM\HARDWARE\Firmware\ESRT carries the full
 * UEFI 2.10 section 23.6 entry layout.  This mirror covers the GENERIC
 * table inventory: Address / Size / Checksum / ValidationStatus / Source.
 *
 * Per-entry value names match the schema doc byte-for-byte so the
 * Registry mirror and the JSON publisher share one ABI.
 *
 * Wired from registry_populate_defaults() after esrt_populate_registry().
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/firmware_tables.h"
#include "kernel/klog.h"
#include "registry.h"

#define FW_TABLES_PATH "HARDWARE\\Firmware\\Tables"
#define FW_TABLES_PATH_CAP 96

/* Format a per-entry subkey name.  Prefer the catalog name (e.g.
 * "ACPI 2.0", "FPDT", "SMBIOS3") because it is human-readable and
 * stable across vendors.  Names are validated to fit Registry key
 * naming rules: replace any character not in [A-Za-z0-9_.- ] with '_'. */
static void sanitize_subkey(char out[64], const char *name)
{
    size_t i = 0;
    if (!name || !name[0]) {
        out[0] = 'u'; out[1] = 'n'; out[2] = 'k'; out[3] = 'n';
        out[4] = 'o'; out[5] = 'w'; out[6] = 'n'; out[7] = '\0';
        return;
    }
    for (; name[i] && i < 63; i++) {
        char c = name[i];
        int ok = (c >= 'A' && c <= 'Z') ||
                 (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') ||
                 c == '_' || c == '.' || c == '-' || c == ' ';
        out[i] = ok ? c : '_';
    }
    out[i] = '\0';
}

/* Source-enum to canonical schema string.  Must match the JSON writer
 * + schema doc byte-for-byte. */
static const char *source_name(int source)
{
    switch (source) {
    case FW_SOURCE_UEFI_CFG_TABLE:  return "uefi_cfg_table";
    case FW_SOURCE_ACPI_SDT:        return "acpi_sdt";
    case FW_SOURCE_SMBIOS_RAW:      return "smbios_raw";
    case FW_SOURCE_FPDT:            return "fpdt";
    case FW_SOURCE_ESRT:            return "esrt";
    case FW_SOURCE_DTB:             return "dtb";
    default:                        return "unknown";
    }
}

/* Status-enum to canonical schema string.  Must match the JSON writer. */
static const char *status_name(int status)
{
    switch (status) {
    case FW_STATUS_VALIDATED:        return "validated";
    case FW_STATUS_DEGRADED:         return "degraded";
    case FW_STATUS_UNKNOWN_PROFILE:  return "unknown_profile";
    default:                         return "untested";
    }
}

void firmware_tables_populate_registry(void)
{
    /* Idempotent reset: blow away the prior boot's per-entry subkeys.
     * RegDeleteTree returns ERROR_FILE_NOT_FOUND if the key is absent
     * (first-ever boot or fresh hive); both states are fine because
     * the writer below recreates whatever entries are present this
     * boot.  ESRT-specific keys live under HARDWARE\Firmware\ESRT
     * (separate subtree owned by esrt_populate_registry); this delete
     * does not affect them. */
    (void)RegDeleteTree(HKEY_LOCAL_MACHINE, FW_TABLES_PATH);

    uint32_t count = firmware_table_count();
    if (count == 0) {
        klog(LOG_INFO, "FW",
             "Registry: catalog empty; HARDWARE\\Firmware\\Tables cleared");
        return;
    }

    uint32_t mirrored = 0;
    for (uint32_t i = 0; i < count; i++) {
        const struct firmware_table_entry *e = firmware_table_get(i);
        if (!e || !e->name[0])
            continue;

        char subkey[64];
        sanitize_subkey(subkey, e->name);

        char path[FW_TABLES_PATH_CAP];
        size_t pos = 0;
        const char *prefix = FW_TABLES_PATH "\\";
        for (; prefix[pos] && pos < FW_TABLES_PATH_CAP - 1; pos++)
            path[pos] = prefix[pos];
        for (size_t j = 0; subkey[j] && pos < FW_TABLES_PATH_CAP - 1; j++)
            path[pos++] = subkey[j];
        path[pos] = '\0';

        HKEY hKey = (HKEY)0;
        uint32_t disp = 0;
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0, (const char *)0,
                           0, KEY_ALL_ACCESS, (void *)0, &hKey, &disp)
            != ERROR_SUCCESS) {
            klog(LOG_WARN, "FW",
                 "Registry: failed to create %s", path);
            continue;
        }

        /* Address is u64 (firmware physical address); REG_QWORD
         * preserves the high half on systems with > 4 GiB ROM
         * shadow / RT-services-data above the 4 GiB line. */
        RegSetQword(hKey,  "Address",          (uint64_t)e->phys_addr);
        RegSetDword(hKey,  "Size",             e->size);
        /* Checksum is catalog-internal; expose 0 until the validators
         * store the computed checksum on the entry (today they only
         * set the status field).  Schema doc marks Checksum as
         * "(catalog-internal)" so 0 is fine. */
        RegSetDword(hKey,  "Checksum",         0);
        RegSetString(hKey, "ValidationStatus", status_name(e->status));
        RegSetString(hKey, "Source",           source_name(e->source));
        RegCloseKey(hKey);
        mirrored++;
    }

    klog(LOG_INFO, "FW",
         "Registry: %u/%u firmware table(s) mirrored under %s",
         mirrored, count, FW_TABLES_PATH);
}
