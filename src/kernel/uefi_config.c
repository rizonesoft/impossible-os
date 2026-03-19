/* ============================================================================
 * uefi_config.c — UEFI Configuration Table Walker
 *
 * Walks the UEFI configuration table entries preserved in boot_info to find
 * platform data (ACPI, SMBIOS, Memory Attributes, etc.) by GUID.
 * ============================================================================ */

#include "kernel/uefi_config.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"

/* Compare two boot_uefi_guid structs byte-by-byte */
static int guid_equal(const struct boot_uefi_guid *a,
                      const struct boot_uefi_guid *b)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    uint32_t i;
    for (i = 0; i < 16; i++)
        if (pa[i] != pb[i]) return 0;
    return 1;
}

/* Name lookup for well-known GUIDs (for logging) */
static const char *guid_name(const struct boot_uefi_guid *g)
{
    struct boot_uefi_guid acpi20 = UEFI_GUID_ACPI_20;
    struct boot_uefi_guid acpi10 = UEFI_GUID_ACPI_10;
    struct boot_uefi_guid smbios3 = UEFI_GUID_SMBIOS3;
    struct boot_uefi_guid smbios = UEFI_GUID_SMBIOS;
    struct boot_uefi_guid memattr = UEFI_GUID_MEM_ATTR;
    struct boot_uefi_guid rtprops = UEFI_GUID_RT_PROPS;
    struct boot_uefi_guid conform = UEFI_GUID_CONFORMANCE;
    struct boot_uefi_guid dtb = UEFI_GUID_DTB;

    if (guid_equal(g, &acpi20))  return "ACPI2.0";
    if (guid_equal(g, &acpi10))  return "ACPI1.0";
    if (guid_equal(g, &smbios3)) return "SMBIOS3";
    if (guid_equal(g, &smbios))  return "SMBIOS";
    if (guid_equal(g, &memattr)) return "MemAttr";
    if (guid_equal(g, &rtprops)) return "RtProps";
    if (guid_equal(g, &conform)) return "Conform";
    if (guid_equal(g, &dtb))     return "DTB";
    return (void *)0;
}

uintptr_t uefi_find_config_table(const struct boot_uefi_guid *guid)
{
    uint32_t i;
    for (i = 0; i < g_boot_info.config_table_count; i++) {
        if (guid_equal(&g_boot_info.config_table[i].guid, guid))
            return (uintptr_t)g_boot_info.config_table[i].table_addr;
    }
    return 0;
}

void uefi_config_init(void)
{
    uint32_t i;
    uint32_t known_count = 0;

    /* Build a summary string of known tables found */
    /* We log each known table name; max ~8 known GUIDs, keep it concise */
    char found_names[128];
    uint32_t pos = 0;

    for (i = 0; i < g_boot_info.config_table_count; i++) {
        const char *name = guid_name(&g_boot_info.config_table[i].guid);
        if (name) {
            known_count++;
            /* Append name to found_names with space separator */
            if (pos > 0 && pos < sizeof(found_names) - 1)
                found_names[pos++] = ' ';
            uint32_t j;
            for (j = 0; name[j] && pos < sizeof(found_names) - 1; j++)
                found_names[pos++] = name[j];
        }
    }
    found_names[pos] = '\0';

    klog(LOG_INFO, "UEFI", "Config tables: %s (%u/%u known)",
         found_names, known_count, g_boot_info.config_table_count);
}

/* ============================================================================
 * Conformance Profile Detection (UEFI 2.10 §4.6)
 *
 * The EFI_CONFORMANCE_PROFILES_TABLE contains a list of profile GUIDs that
 * describe firmware capabilities.  If absent, assume full UEFI conformance
 * (pre-2.10 firmware).  If present, check for:
 * - UEFI Spec GUID → full conformance
 * - EBBR GUID → embedded/minimal (no HII, limited services)
 * ============================================================================ */

/* EFI_CONFORMANCE_PROFILES_TABLE layout (per UEFI 2.10 spec) */
struct uefi_conformance_table {
    uint16_t version;                  /* table version, must be 0x1 */
    uint16_t profile_count;            /* number of profile GUIDs */
    struct boot_uefi_guid profiles[];  /* flexible array of GUIDs */
};

static int s_conformance_level = UEFI_CONFORM_FULL;

void uefi_conformance_init(void)
{
    struct boot_uefi_guid conform_guid = UEFI_GUID_CONFORMANCE;
    uintptr_t table_addr = uefi_find_config_table(&conform_guid);

    if (table_addr == 0) {
        /* Table absent — pre-UEFI 2.10 or firmware that omits it.
         * Assume full UEFI conformance. */
        s_conformance_level = UEFI_CONFORM_FULL;
        klog(LOG_INFO, "UEFI", "Conformance: Full UEFI (table absent, assumed)");
        return;
    }

    const struct uefi_conformance_table *ct =
        (const struct uefi_conformance_table *)table_addr;

    /* Search for known profile GUIDs */
    struct boot_uefi_guid uefi_spec = UEFI_PROFILE_UEFI_SPEC;
    struct boot_uefi_guid ebbr      = UEFI_PROFILE_EBBR;
    int found_uefi = 0;
    int found_ebbr = 0;

    uint16_t i;
    for (i = 0; i < ct->profile_count; i++) {
        if (guid_equal(&ct->profiles[i], &uefi_spec))
            found_uefi = 1;
        if (guid_equal(&ct->profiles[i], &ebbr))
            found_ebbr = 1;
    }

    if (found_uefi) {
        s_conformance_level = UEFI_CONFORM_FULL;
        klog(LOG_INFO, "UEFI", "Conformance: Full UEFI 2.10 (%u profiles)",
             ct->profile_count);
    } else if (found_ebbr) {
        s_conformance_level = UEFI_CONFORM_EBBR;
        klog(LOG_WARN, "UEFI", "Conformance: Reduced (EBBR) — "
             "some services may be unavailable");
    } else {
        s_conformance_level = UEFI_CONFORM_UNKNOWN;
        klog(LOG_WARN, "UEFI", "Conformance: Unknown profile (%u entries)",
             ct->profile_count);
    }
}

int uefi_conformance_level(void)
{
    return s_conformance_level;
}

/* ============================================================================
 * ESRT Firmware Inventory (UEFI 2.5+ §23.4)
 *
 * The EFI_SYSTEM_RESOURCE_TABLE lists all updateable firmware components.
 * Each entry describes a firmware resource with version, type, and last
 * update status.  This feeds into the "Firmware Health" panel and capsule
 * updates.
 * ============================================================================ */

/* EFI_SYSTEM_RESOURCE_TABLE header (at the config table address) */
struct esrt_table_header {
    uint32_t fw_resource_count;
    uint32_t fw_resource_count_max;
    uint64_t fw_resource_version;
};

static struct esrt_entry s_esrt_entries[ESRT_MAX_ENTRIES];
static uint32_t s_esrt_count;

static const char *esrt_type_name(uint32_t fw_type)
{
    switch (fw_type) {
    case ESRT_FW_TYPE_SYSTEM:       return "System";
    case ESRT_FW_TYPE_DEVICE:       return "Device";
    case ESRT_FW_TYPE_UEFI_DRIVER:  return "Driver";
    default:                        return "Unknown";
    }
}

void esrt_init(void)
{
    s_esrt_count = 0;

    struct boot_uefi_guid esrt_guid = UEFI_GUID_ESRT;
    uintptr_t table_addr = uefi_find_config_table(&esrt_guid);

    if (table_addr == 0) {
        klog(LOG_INFO, "ESRT", "Not present (no firmware inventory)");
        return;
    }

    const struct esrt_table_header *hdr =
        (const struct esrt_table_header *)table_addr;

    uint32_t count = hdr->fw_resource_count;
    if (count > ESRT_MAX_ENTRIES)
        count = ESRT_MAX_ENTRIES;

    if (count == 0) {
        klog(LOG_INFO, "ESRT", "Present but empty (0 entries)");
        return;
    }

    /* ESRT entries follow immediately after the header.
     * Each entry matches our esrt_entry struct layout exactly. */
    const struct esrt_entry *src =
        (const struct esrt_entry *)(table_addr + sizeof(struct esrt_table_header));

    uint32_t i;
    for (i = 0; i < count; i++)
        s_esrt_entries[i] = src[i];
    s_esrt_count = count;

    /* Log summary */
    klog(LOG_INFO, "ESRT", "%u firmware component(s):", count);
    for (i = 0; i < count; i++) {
        const struct esrt_entry *e = &s_esrt_entries[i];
        klog(LOG_INFO, "ESRT", "  [%u] %s v%u.%u (min v%u.%u, status=%u)",
             i, esrt_type_name(e->fw_type),
             e->fw_version >> 16, e->fw_version & 0xFFFF,
             e->lowest_supported_version >> 16,
             e->lowest_supported_version & 0xFFFF,
             e->last_attempt_status);
    }
}

uint32_t esrt_count(void)
{
    return s_esrt_count;
}

const struct esrt_entry *esrt_get_entry(uint32_t index)
{
    if (index >= s_esrt_count)
        return (const struct esrt_entry *)0;
    return &s_esrt_entries[index];
}
