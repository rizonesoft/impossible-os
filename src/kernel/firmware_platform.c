/* ============================================================================
 * firmware_platform.c -- ACPI/DTB/Hybrid arbitration policy
 *
 * Picks the platform classification at boot based on the validated
 * firmware-table catalog (firmware_table_lookup_name + entry status)
 * and the FDT header validator's verdict, then mirrors the result to
 * HKLM\SYSTEM\Boot\Firmware so user-mode tools can query "are we on
 * an ACPI box, a DTB box, or both?" without re-walking config tables.
 *
 * Init contract: firmware_table_validate_all() must have run; on PC
 * boot that happens at the end of firmware_tables_init(). Calling
 * order is firmware_platform_init() AFTER the validator, BEFORE
 * registry_populate_defaults() reads the platform.
 * ============================================================================ */

#include "kernel/firmware_platform.h"
#include "kernel/firmware_tables.h"
#include "kernel/dtb.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "registry.h"

static enum fw_platform s_platform = FW_PLATFORM_UNKNOWN;
static uint8_t s_initialised;
static uint8_t s_has_acpi;
static uint8_t s_has_dtb;
static uint8_t s_has_smbios;
static uint32_t s_acpi_version;

/* Look up `name` in the catalog and report whether it ended up
 * VALIDATED. UNKNOWN_PROFILE and DEGRADED both fail the gate so the
 * platform decision only honours data we have actually checked. */
static int catalog_validated(const char *name)
{
    const struct firmware_table_entry *e = firmware_table_lookup_name(name);
    if (!e) return 0;
    return e->status == FW_STATUS_VALIDATED;
}

void firmware_platform_init(void)
{
    if (s_initialised) return;
    s_initialised = 1;

    /* ACPI: prefer 2.0 then 1.0; record the highest validated version. */
    if (catalog_validated("ACPI2.0")) {
        s_has_acpi = 1;
        s_acpi_version = 2;
    } else if (catalog_validated("ACPI1.0")) {
        s_has_acpi = 1;
        s_acpi_version = 1;
    }

    /* SMBIOS: presence is informational; never decides ACPI vs DTB. */
    if (catalog_validated("SMBIOS3") || catalog_validated("SMBIOS"))
        s_has_smbios = 1;

    /* DTB: catalog presence is necessary but not sufficient. The FDT
     * header validator must accept the blob AND the discovery
     * inventory must show at least one /memory and one /cpu node before
     * HasDTB flips on. A structurally-valid empty DTB (root node only
     * + FDT_END) is not a real DTB platform -- a DTB-only system that
     * cannot enumerate memory or CPUs is not bootable, so an empty
     * blob staying classified as Unknown is the correct outcome. */
    const struct firmware_table_entry *dtb_entry =
        firmware_table_lookup_name("DTB");
    if (dtb_entry && dtb_entry->status != FW_STATUS_DEGRADED &&
        dtb_entry->phys_addr != 0) {
        dtb_init(dtb_entry->phys_addr);
        if (dtb_is_valid() &&
            dtb_memory_count() > 0 && dtb_cpu_count() > 0) {
            s_has_dtb = 1;
            /* Reconcile the catalog with our acceptance: the
             * unified validator left DTB at UNKNOWN_PROFILE because
             * it has no per-format oracle. Now that dtb_init has
             * accepted the FDT header + structure walk + inventory
             * gate, promote the catalog entry so consumers reading
             * firmware_table_lookup_name("DTB")->status agree with
             * HKLM\SYSTEM\Boot\Firmware\HasDTB=1. */
            firmware_table_promote_to_validated("DTB");
        }
    }

    /* Arbitration: ACPI wins when both are present. PC-class doctrine
     * pins interrupt/timer ownership to ACPI on x86; HYBRID is a
     * diagnostic for ARM SBSA boxes that ship both, and even there the
     * primary remains ACPI. */
    if (s_has_acpi && s_has_dtb)      s_platform = FW_PLATFORM_HYBRID;
    else if (s_has_acpi)              s_platform = FW_PLATFORM_ACPI;
    else if (s_has_dtb)               s_platform = FW_PLATFORM_DTB;
    else                              s_platform = FW_PLATFORM_UNKNOWN;

    klog(LOG_INFO, "BOOT",
         "firmware platform: %s (acpi=%u dtb=%u smbios=%u acpi_ver=%u)",
         firmware_platform_name(s_platform),
         (uint64_t)s_has_acpi, (uint64_t)s_has_dtb,
         (uint64_t)s_has_smbios, (uint64_t)s_acpi_version);
}

const char *firmware_platform_name(enum fw_platform p)
{
    switch (p) {
    case FW_PLATFORM_ACPI:    return "ACPI";
    case FW_PLATFORM_DTB:     return "DTB";
    case FW_PLATFORM_HYBRID:  return "Hybrid";
    default:                  return "Unknown";
    }
}

enum fw_platform firmware_platform_get(void) { return s_platform; }
int      firmware_platform_has_acpi(void)    { return (int)s_has_acpi; }
int      firmware_platform_has_dtb(void)     { return (int)s_has_dtb; }
int      firmware_platform_has_smbios(void)  { return (int)s_has_smbios; }
uint32_t firmware_platform_acpi_version(void){ return s_acpi_version; }

/* ----: Registry mirror ---------------------------------------------------
 * Called from registry_populate_defaults() after registry_init(). Writes
 * a single REG_SZ FirmwarePlatform key plus presence DWORDs under
 * HKLM\SYSTEM\Boot\Firmware so sysinfo/diagnostic tools have a stable
 * read-only platform fingerprint. */
void firmware_platform_populate_registry(void)
{
    HKEY hKey = (HKEY)0;
    uint32_t disp = 0;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Boot\\Firmware", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) != ERROR_SUCCESS) {
        klog(LOG_WARN, "boot",
             "Failed to create HKLM\\SYSTEM\\Boot\\Firmware key");
        return;
    }

    RegSetString(hKey, "FirmwarePlatform",
                 firmware_platform_name(s_platform));
    RegSetDword(hKey, "HasACPI",     (uint32_t)s_has_acpi);
    RegSetDword(hKey, "HasDTB",      (uint32_t)s_has_dtb);
    RegSetDword(hKey, "HasSMBIOS",   (uint32_t)s_has_smbios);
    RegSetDword(hKey, "AcpiVersion", s_acpi_version);
    if (s_has_dtb)
        RegSetDword(hKey, "DtbTotalSize", dtb_total_size());

    RegCloseKey(hKey);
}
