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
