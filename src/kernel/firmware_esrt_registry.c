/* ============================================================================
 * firmware_esrt_registry.c -- mirror ESRT inventory into HKLM\HARDWARE\Firmware\ESRT
 *
 * Consumer of esrt_count() / esrt_get_entry() / esrt_resource_count_max() /
 * esrt_resource_version() (uefi_config.c).  Writes one subkey per ESRT
 * entry keyed by the canonical Microsoft brace-form FwClass GUID, plus
 * a sibling _Header subkey with the table-level metadata.
 *
 * Idempotent: clears the entire HARDWARE\Firmware\ESRT subtree before
 * writing.  ESRT-absent boots leave the parent key empty (no stale
 * per-resource keys carry over from a prior firmware that exposed
 * different components).
 *
 * Per-entry value names match the UEFI 2.10 section 23.6
 * EFI_SYSTEM_RESOURCE_ENTRY field names with REG_SZ for the decoded
 * status label (LastAttemptStatusName) so capsule policy UX gets
 * operator-readable strings without re-implementing the decode.
 *
 * Wired from registry.c registry_populate_defaults() after
 * firmware_platform_populate_registry().
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/uefi_config.h"
#include "kernel/klog.h"
#include "registry.h"

/* Format a boot_uefi_guid (UEFI mixed-endian over the wire) as the
 * canonical Microsoft brace form
 *   "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"
 * 38 chars + NUL = 39-byte buffer minimum.  fwupd, Windows Update,
 * and operator tools all use this form. */
#define ESRT_GUID_BUF_LEN 40

static void hex8(char *out, uint8_t v)
{
    static const char hex[] = "0123456789abcdef";
    out[0] = hex[(v >> 4) & 0xF];
    out[1] = hex[v & 0xF];
}

static void format_fwclass_guid(const struct boot_uefi_guid *g,
                                char buf[ESRT_GUID_BUF_LEN])
{
    /* Layout: { ddddddddd-dddd-dddd-dddd-dddddddddddd }
     * data1 (u32), data2 (u16), data3 (u16) are little-endian
     * scalars; data4 is 8 raw bytes.  Render as documented. */
    size_t pos = 0;
    buf[pos++] = '{';
    /* data1 LE */
    hex8(&buf[pos], (uint8_t)(g->data1 >> 24)); pos += 2;
    hex8(&buf[pos], (uint8_t)(g->data1 >> 16)); pos += 2;
    hex8(&buf[pos], (uint8_t)(g->data1 >> 8));  pos += 2;
    hex8(&buf[pos], (uint8_t)(g->data1));       pos += 2;
    buf[pos++] = '-';
    /* data2 LE */
    hex8(&buf[pos], (uint8_t)(g->data2 >> 8));  pos += 2;
    hex8(&buf[pos], (uint8_t)(g->data2));       pos += 2;
    buf[pos++] = '-';
    /* data3 LE */
    hex8(&buf[pos], (uint8_t)(g->data3 >> 8));  pos += 2;
    hex8(&buf[pos], (uint8_t)(g->data3));       pos += 2;
    buf[pos++] = '-';
    /* data4[0..1] */
    hex8(&buf[pos], g->data4[0]); pos += 2;
    hex8(&buf[pos], g->data4[1]); pos += 2;
    buf[pos++] = '-';
    /* data4[2..7] */
    for (size_t i = 2; i < 8; i++) {
        hex8(&buf[pos], g->data4[i]);
        pos += 2;
    }
    buf[pos++] = '}';
    buf[pos]   = '\0';
}

/* Build a path "HARDWARE\Firmware\ESRT\<subkey>" into out_path.  The
 * static prefix is embedded literally; subkey is the GUID brace form
 * or "_Header".  Caller-side cap = ESRT_PATH_CAP. */
#define ESRT_PATH_CAP 96

static void esrt_path(char out_path[ESRT_PATH_CAP], const char *subkey)
{
    static const char prefix[] = "HARDWARE\\Firmware\\ESRT\\";
    size_t i = 0;
    for (; prefix[i] && i < ESRT_PATH_CAP - 1; i++)
        out_path[i] = prefix[i];
    for (size_t j = 0; subkey[j] && i < ESRT_PATH_CAP - 1; j++, i++)
        out_path[i] = subkey[j];
    out_path[i] = '\0';
}

void esrt_populate_registry(void)
{
    /* Idempotent reset: blow away the prior boot's ESRT subtree.
     * RegDeleteTree returns ERROR_FILE_NOT_FOUND if the key is
     * absent (first-ever boot or fresh hive); both states are fine
     * because the writer below recreates whatever entries are
     * actually present this boot.  ESRT-absent firmware exits early
     * after the delete so the parent key stays empty. */
    (void)RegDeleteTree(HKEY_LOCAL_MACHINE, "HARDWARE\\Firmware\\ESRT");

    uint32_t count = esrt_count();
    if (count == 0) {
        klog(LOG_INFO, "ESRT",
             "Registry: ESRT absent or empty; HARDWARE\\Firmware\\ESRT cleared");
        return;
    }

    /* Header subkey: ResourceCount / ResourceCountMax / ResourceVersion. */
    {
        char path[ESRT_PATH_CAP];
        esrt_path(path, "_Header");
        HKEY hHdr = (HKEY)0;
        uint32_t disp = 0;
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0, (const char *)0,
                           0, KEY_ALL_ACCESS, (void *)0, &hHdr, &disp)
            == ERROR_SUCCESS) {
            RegSetDword(hHdr, "ResourceCount",     count);
            RegSetDword(hHdr, "ResourceCountMax",  esrt_resource_count_max());
            /* ResourceVersion is u64 per UEFI 2.10 section 23.6 EFI_
             * SYSTEM_RESOURCE_TABLE.fw_resource_version.  Store as
             * REG_QWORD so a firmware that ever populates a non-zero
             * high half survives the mirror untruncated. */
            RegSetQword(hHdr, "ResourceVersion", esrt_resource_version());
            RegCloseKey(hHdr);
        } else {
            klog(LOG_WARN, "ESRT",
                 "Registry: failed to create %s", path);
        }
    }

    /* Per-entry subkey: keyed by canonical brace-form FwClass GUID. */
    for (uint32_t i = 0; i < count; i++) {
        const struct esrt_entry *e = esrt_get_entry(i);
        if (!e)
            continue;

        char guid_buf[ESRT_GUID_BUF_LEN];
        format_fwclass_guid(&e->fw_class, guid_buf);

        char path[ESRT_PATH_CAP];
        esrt_path(path, guid_buf);

        HKEY hEnt = (HKEY)0;
        uint32_t disp = 0;
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0, (const char *)0,
                           0, KEY_ALL_ACCESS, (void *)0, &hEnt, &disp)
            != ERROR_SUCCESS) {
            klog(LOG_WARN, "ESRT",
                 "Registry: failed to create %s", path);
            continue;
        }

        RegSetDword(hEnt,  "Type",                       e->fw_type);
        RegSetString(hEnt, "TypeName",                   esrt_decode_type(e->fw_type));
        RegSetDword(hEnt,  "FwVersion",                  e->fw_version);
        RegSetDword(hEnt,  "LowestSupportedFwVersion",   e->lowest_supported_version);
        RegSetDword(hEnt,  "CapsuleFlags",               e->capsule_flags);
        RegSetDword(hEnt,  "LastAttemptVersion",         e->last_attempt_version);
        RegSetDword(hEnt,  "LastAttemptStatus",          e->last_attempt_status);
        RegSetString(hEnt, "LastAttemptStatusName",      esrt_decode_status(e->last_attempt_status));
        RegCloseKey(hEnt);
    }

    klog(LOG_INFO, "ESRT",
         "Registry: %u ESRT component(s) mirrored under HARDWARE\\Firmware\\ESRT",
         count);
}
