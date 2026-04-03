/* ============================================================================
 * smbios.c -- SMBIOS System Information Parser
 *
 * Walks SMBIOS structures found via UEFI Configuration Table.
 * Extracts BIOS, System, Baseboard, Processor, and Memory info.
 * ============================================================================ */

#include "kernel/smbios.h"
#include "kernel/uefi_config.h"
#include "kernel/klog.h"
#include "registry.h"

/* ---- Internal types ---- */

/* SMBIOS 3.0 64-bit entry point */
struct smbios3_entry {
    uint8_t  anchor[5];          /* "_SM3_" */
    uint8_t  checksum;
    uint8_t  length;
    uint8_t  major_version;
    uint8_t  minor_version;
    uint8_t  docrev;
    uint8_t  revision;
    uint8_t  reserved;
    uint32_t max_struct_size;
    uint64_t struct_table_addr;
};

/* SMBIOS 2.x 32-bit entry point */
struct smbios2_entry {
    uint8_t  anchor[4];          /* "_SM_" */
    uint8_t  checksum;
    uint8_t  length;
    uint8_t  major_version;
    uint8_t  minor_version;
    uint16_t max_struct_size;
    uint8_t  revision;
    uint8_t  formatted[5];
    uint8_t  ianchor[5];         /* "_DMI_" */
    uint8_t  ichecksum;
    uint16_t struct_table_length;
    uint32_t struct_table_addr;
    uint16_t num_structures;
    uint8_t  bcd_revision;
};

/* SMBIOS structure header (common to all types) */
struct smbios_header {
    uint8_t  type;
    uint8_t  length;
    uint16_t handle;
};

/* ---- Static state ---- */

static struct smbios_system_info s_info;

/* ---- String helper ---- */

/* SMBIOS strings follow the structure data as a double-NUL-terminated list.
 * String index 1 = first string, 2 = second, etc.  Index 0 = no string. */
static const char *smbios_get_string(const struct smbios_header *hdr,
                                     uint8_t index)
{
    if (index == 0) return "";

    /* Strings start at hdr + hdr->length */
    const char *p = (const char *)hdr + hdr->length;
    uint8_t cur = 1;

    while (cur < index) {
        /* Skip to end of current string */
        while (*p != '\0') p++;
        p++;  /* skip the NUL */
        if (*p == '\0') return "";  /* hit double-NUL = end of strings */
        cur++;
    }
    return p;
}

/* Safe string copy with truncation */
static void str_copy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ---- Structure parsers ---- */

static void parse_type0(const struct smbios_header *hdr)
{
    const uint8_t *d = (const uint8_t *)hdr;
    if (hdr->length < 18) return;

    str_copy(s_info.bios_vendor,  smbios_get_string(hdr, d[4]),
             SMBIOS_STRING_MAX);
    str_copy(s_info.bios_version, smbios_get_string(hdr, d[5]),
             SMBIOS_STRING_MAX);
    str_copy(s_info.bios_date,    smbios_get_string(hdr, d[8]),
             SMBIOS_STRING_MAX);
}

static void parse_type1(const struct smbios_header *hdr)
{
    const uint8_t *d = (const uint8_t *)hdr;
    if (hdr->length < 8) return;

    str_copy(s_info.sys_manufacturer, smbios_get_string(hdr, d[4]),
             SMBIOS_STRING_MAX);
    str_copy(s_info.sys_product,      smbios_get_string(hdr, d[5]),
             SMBIOS_STRING_MAX);
    if (hdr->length >= 7)
        str_copy(s_info.sys_version,  smbios_get_string(hdr, d[6]),
                 SMBIOS_STRING_MAX);
    if (hdr->length >= 25) {
        str_copy(s_info.sys_serial,   smbios_get_string(hdr, d[7]),
                 SMBIOS_STRING_MAX);
        /* UUID at bytes 8..23 (128-bit) */
        uint32_t i;
        for (i = 0; i < 16; i++)
            s_info.sys_uuid[i] = d[8 + i];
    }
}

static void parse_type2(const struct smbios_header *hdr)
{
    const uint8_t *d = (const uint8_t *)hdr;
    if (hdr->length < 8) return;

    str_copy(s_info.board_manufacturer, smbios_get_string(hdr, d[4]),
             SMBIOS_STRING_MAX);
    str_copy(s_info.board_product,      smbios_get_string(hdr, d[5]),
             SMBIOS_STRING_MAX);
}

static void parse_type4(const struct smbios_header *hdr)
{
    const uint8_t *d = (const uint8_t *)hdr;
    if (hdr->length < 26) return;

    /* Fill next per-socket slot */
    if (s_info.cpu_count >= SMBIOS_CPU_MAX) return;
    struct smbios_cpu_info *cpu = &s_info.cpus[s_info.cpu_count];

    str_copy(cpu->socket,       smbios_get_string(hdr, d[4]),
             SMBIOS_STRING_MAX);
    cpu->family = d[6];
    str_copy(cpu->manufacturer, smbios_get_string(hdr, d[7]),
             SMBIOS_STRING_MAX);

    /* Max speed at offset 0x14 (uint16_t LE) */
    cpu->max_speed_mhz = (uint16_t)(d[0x14] | ((uint16_t)d[0x15] << 8));

    /* Core / thread counts (SMBIOS 2.5+) */
    if (hdr->length >= 0x24)
        cpu->core_count   = (uint16_t)d[0x23];
    if (hdr->length >= 0x26)
        cpu->thread_count = (uint16_t)d[0x25];

    cpu->valid = 1;
    s_info.cpu_count++;

    /* Keep legacy summary fields populated from first socket */
    if (s_info.cpu_count == 1) {
        str_copy(s_info.cpu_socket,       cpu->socket,       SMBIOS_STRING_MAX);
        str_copy(s_info.cpu_manufacturer, cpu->manufacturer, SMBIOS_STRING_MAX);
        s_info.cpu_max_speed_mhz  = cpu->max_speed_mhz;
        s_info.cpu_core_count     = cpu->core_count;
        s_info.cpu_thread_count   = cpu->thread_count;
    }
}

static void parse_type17(const struct smbios_header *hdr)
{
    const uint8_t *d = (const uint8_t *)hdr;
    if (hdr->length < 21) return;

    /* Size at offset 0x0C (uint16_t LE, in MB or KB depending on bit 15) */
    uint16_t raw_size = (uint16_t)(d[0x0C] | ((uint16_t)d[0x0D] << 8));
    if (raw_size == 0 || raw_size == 0xFFFF) return;  /* Not installed or unknown */

    uint32_t size_mb;
    if (raw_size & 0x8000) {
        /* Size in KB */
        size_mb = (uint32_t)(raw_size & 0x7FFF) / 1024;
    } else {
        size_mb = (uint32_t)raw_size;
    }

    /* SMBIOS 2.7+: if size == 0x7FFF, use extended size at offset 0x1C */
    if (raw_size == 0x7FFF && hdr->length >= 0x20) {
        uint32_t ext = (uint32_t)(d[0x1C] | ((uint32_t)d[0x1D] << 8) |
                       ((uint32_t)d[0x1E] << 16) | ((uint32_t)d[0x1F] << 24));
        size_mb = ext;  /* Extended size is always in MB */
    }

    /* Aggregate totals (legacy callers) */
    s_info.ram_total_mb += size_mb;
    s_info.ram_dimm_count++;

    if (hdr->length >= 0x17 && s_info.ram_speed_mhz == 0)
        s_info.ram_speed_mhz = (uint16_t)(d[0x15] | ((uint16_t)d[0x16] << 8));
    if (s_info.ram_type == 0)
        s_info.ram_type = d[0x12];

    /* Per-DIMM slot */
    if (s_info.dimm_count >= SMBIOS_DIMM_MAX) return;
    struct smbios_dimm_info *dimm = &s_info.dimms[s_info.dimm_count];

    dimm->size_mb   = size_mb;
    dimm->mem_type  = d[0x12];
    if (hdr->length >= 0x17)
        dimm->speed_mhz = (uint16_t)(d[0x15] | ((uint16_t)d[0x16] << 8));

    /* String fields: device locator (d[0x10]), bank locator (d[0x11]),
     * manufacturer (d[0x17]), part number (d[0x1A]) -- SMBIOS 2.3+ */
    str_copy(dimm->device_locator, smbios_get_string(hdr, d[0x10]),
             SMBIOS_STRING_MAX);
    str_copy(dimm->bank_locator,   smbios_get_string(hdr, d[0x11]),
             SMBIOS_STRING_MAX);
    if (hdr->length >= 0x1B) {
        str_copy(dimm->manufacturer, smbios_get_string(hdr, d[0x17]),
                 SMBIOS_STRING_MAX);
        str_copy(dimm->part_number,  smbios_get_string(hdr, d[0x1A]),
                 SMBIOS_STRING_MAX);
    }

    dimm->valid = 1;
    s_info.dimm_count++;
}

/* ---- Table walker ---- */

static void walk_structures(uintptr_t table_addr, uint32_t max_len)
{
    const uint8_t *p = (const uint8_t *)table_addr;
    const uint8_t *end = p + max_len;

    while (p + 4 <= end) {
        const struct smbios_header *hdr = (const struct smbios_header *)p;

        if (hdr->type == 127) break;  /* End of table */
        if (hdr->length < 4) break;   /* Corrupt */

        /* Parse known types */
        switch (hdr->type) {
        case 0:  parse_type0(hdr);  break;
        case 1:  parse_type1(hdr);  break;
        case 2:  parse_type2(hdr);  break;
        case 4:  parse_type4(hdr);  break;
        case 17: parse_type17(hdr); break;
        default: break;
        }

        /* Advance past structure data */
        p += hdr->length;

        /* Skip the double-NUL-terminated string section */
        while (p + 1 < end && !(p[0] == '\0' && p[1] == '\0'))
            p++;
        p += 2;  /* skip the double NUL */
    }
}

/* ---- Public API ---- */

static const char *mem_type_name(uint8_t type)
{
    switch (type) {
    case SMBIOS_MEM_DDR3:  return "DDR3";
    case SMBIOS_MEM_DDR4:  return "DDR4";
    case SMBIOS_MEM_LPDDR4: return "LPDDR4";
    case SMBIOS_MEM_DDR5:  return "DDR5";
    case SMBIOS_MEM_LPDDR5: return "LPDDR5";
    default: return "Unknown";
    }
}

void smbios_init(void)
{
    uint32_t i;
    /* Zero the info struct */
    uint8_t *p = (uint8_t *)&s_info;
    for (i = 0; i < sizeof(s_info); i++)
        p[i] = 0;

    /* Try SMBIOS 3.x first */
    struct boot_uefi_guid guid3 = UEFI_GUID_SMBIOS3;
    uintptr_t ep_addr = uefi_find_config_table(&guid3);

    if (ep_addr != 0) {
        const struct smbios3_entry *ep =
            (const struct smbios3_entry *)ep_addr;

        s_info.smbios_major = ep->major_version;
        s_info.smbios_minor = ep->minor_version;

        walk_structures((uintptr_t)ep->struct_table_addr,
                        ep->max_struct_size);
        s_info.valid = 1;
    } else {
        /* Fallback: SMBIOS 2.x */
        struct boot_uefi_guid guid2 = UEFI_GUID_SMBIOS;
        ep_addr = uefi_find_config_table(&guid2);

        if (ep_addr != 0) {
            const struct smbios2_entry *ep =
                (const struct smbios2_entry *)ep_addr;

            s_info.smbios_major = ep->major_version;
            s_info.smbios_minor = ep->minor_version;

            walk_structures((uintptr_t)ep->struct_table_addr,
                            (uint32_t)ep->struct_table_length);
            s_info.valid = 1;
        }
    }

    if (!s_info.valid) {
        klog(LOG_WARN, "SMBIOS", "Not found in config table");
        return;
    }

    klog(LOG_INFO, "SMBIOS", "v%u.%u -- %s %s",
         (uint32_t)s_info.smbios_major, (uint32_t)s_info.smbios_minor,
         s_info.sys_manufacturer[0] ? s_info.sys_manufacturer : "Unknown",
         s_info.sys_product[0] ? s_info.sys_product : "Unknown");

    klog(LOG_INFO, "SMBIOS", "BIOS: %s %s (%s)",
         s_info.bios_vendor[0] ? s_info.bios_vendor : "Unknown",
         s_info.bios_version[0] ? s_info.bios_version : "Unknown",
         s_info.bios_date[0] ? s_info.bios_date : "Unknown");

    if (s_info.cpu_max_speed_mhz > 0) {
        klog(LOG_INFO, "SMBIOS", "CPU: %s, %u cores / %u threads, %u MHz",
             s_info.cpu_manufacturer[0] ?
                 s_info.cpu_manufacturer : "Unknown",
             (uint32_t)s_info.cpu_core_count,
             (uint32_t)s_info.cpu_thread_count,
             (uint32_t)s_info.cpu_max_speed_mhz);
    }

    if (s_info.ram_total_mb > 0) {
        if (s_info.ram_total_mb >= 1024) {
            klog(LOG_INFO, "SMBIOS", "RAM: %u GB %s @ %u MHz (%u DIMMs)",
                 s_info.ram_total_mb / 1024,
                 mem_type_name(s_info.ram_type),
                 (uint32_t)s_info.ram_speed_mhz,
                 (uint32_t)s_info.ram_dimm_count);
        } else {
            klog(LOG_INFO, "SMBIOS", "RAM: %u MB %s @ %u MHz (%u DIMMs)",
                 s_info.ram_total_mb,
                 mem_type_name(s_info.ram_type),
                 (uint32_t)s_info.ram_speed_mhz,
                 (uint32_t)s_info.ram_dimm_count);
        }
    }

    if (s_info.board_manufacturer[0]) {
        klog(LOG_INFO, "SMBIOS", "Board: %s %s",
             s_info.board_manufacturer, s_info.board_product);
    }
}

const struct smbios_system_info *smbios_get_info(void)
{
    return &s_info;
}

int smbios_get_system_uuid(uint8_t uuid[16])
{
    uint32_t i, all_zero = 1;
    if (!s_info.valid) return 0;
    for (i = 0; i < 16; i++) {
        uuid[i] = s_info.sys_uuid[i];
        if (s_info.sys_uuid[i] != 0) all_zero = 0;
    }
    return !all_zero;
}

/* Format uuid[16] as "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" into buf (37 bytes). */
static void uuid_to_string(const uint8_t uuid[16], char *buf)
{
    static const char hex[] = "0123456789ABCDEF";
    const uint8_t order[] = {
        3,2,1,0, 5,4, 7,6, 8,9, 10,11,12,13,14,15
    };
    uint32_t i, pos = 0;
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            buf[pos++] = '-';
        uint8_t b = uuid[order[i]];
        buf[pos++] = hex[b >> 4];
        buf[pos++] = hex[b & 0x0F];
    }
    buf[pos] = '\0';
}

/* Format a uint32_t as a decimal string into buf. */
static void u32_to_str(uint32_t v, char *buf, uint32_t bufsize)
{
    char tmp[12];
    uint32_t pos = 10;
    tmp[11] = '\0';
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (v > 0 && pos > 0) { tmp[pos--] = (char)('0' + v % 10); v /= 10; }
    uint32_t len = 11 - pos - 1;
    if (len >= bufsize) len = bufsize - 1;
    uint32_t i;
    for (i = 0; i < len; i++) buf[i] = tmp[pos + 1 + i];
    buf[len] = '\0';
}

void smbios_populate_registry(void)
{
    HKEY hKey;
    uint32_t disp;
    char buf[32];
    uint32_t i;

    if (!s_info.valid) return;

    /* ── HKLM\HARDWARE\BIOS ──────────────────────────────────────────── */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\BIOS", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        RegSetString(hKey, "BIOSVendor",      s_info.bios_vendor);
        RegSetString(hKey, "BIOSVersion",     s_info.bios_version);
        RegSetString(hKey, "BIOSReleaseDate", s_info.bios_date);
        RegCloseKey(hKey);
    }

    /* ── HKLM\HARDWARE\System ────────────────────────────────────────── */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\System", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) == ERROR_SUCCESS) {
        RegSetString(hKey, "SystemManufacturer", s_info.sys_manufacturer);
        RegSetString(hKey, "SystemProductName",  s_info.sys_product);
        RegSetString(hKey, "SystemVersion",      s_info.sys_version);
        RegSetString(hKey, "SystemSerial",       s_info.sys_serial);
        /* UUID as canonical string */
        char uuid_str[40];
        uuid_to_string(s_info.sys_uuid, uuid_str);
        RegSetString(hKey, "SystemUUID", uuid_str);
        RegCloseKey(hKey);
    }

    /* ── HKLM\HARDWARE\CPU\{idx} -- per socket ────────────────────────── */
    for (i = 0; i < (uint32_t)s_info.cpu_count; i++) {
        const struct smbios_cpu_info *cpu = &s_info.cpus[i];
        if (!cpu->valid) continue;

        char path[48];
        path[0] = '\0';
        /* Build "HARDWARE\CPU\N" */
        const char *prefix = "HARDWARE\\CPU\\";
        uint32_t p = 0;
        while (prefix[p]) { path[p] = prefix[p]; p++; }
        u32_to_str(i, path + p, (uint32_t)(sizeof(path) - p));

        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0,
                           (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                           &hKey, &disp) == ERROR_SUCCESS) {
            RegSetString(hKey, "Socket",       cpu->socket);
            RegSetString(hKey, "Manufacturer", cpu->manufacturer);
            RegSetDword(hKey,  "MaxSpeedMHz",  (uint32_t)cpu->max_speed_mhz);
            RegSetDword(hKey,  "CoreCount",    (uint32_t)cpu->core_count);
            RegSetDword(hKey,  "ThreadCount",  (uint32_t)cpu->thread_count);
            RegSetDword(hKey,  "Family",       (uint32_t)cpu->family);
            RegCloseKey(hKey);
        }
    }

    /* ── HKLM\HARDWARE\Memory\{idx} -- per DIMM ──────────────────────── */
    for (i = 0; i < (uint32_t)s_info.dimm_count; i++) {
        const struct smbios_dimm_info *dimm = &s_info.dimms[i];
        if (!dimm->valid) continue;

        char path[52];
        const char *prefix2 = "HARDWARE\\Memory\\";
        uint32_t p = 0;
        while (prefix2[p]) { path[p] = prefix2[p]; p++; }
        u32_to_str(i, path + p, (uint32_t)(sizeof(path) - p));

        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0,
                           (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                           &hKey, &disp) == ERROR_SUCCESS) {
            RegSetDword(hKey,  "SizeMB",        dimm->size_mb);
            RegSetDword(hKey,  "SpeedMHz",      (uint32_t)dimm->speed_mhz);
            u32_to_str((uint32_t)dimm->mem_type, buf, sizeof(buf));
            RegSetString(hKey, "MemTypeCode",   buf);
            RegSetString(hKey, "MemType",       mem_type_name(dimm->mem_type));
            RegSetString(hKey, "Manufacturer",  dimm->manufacturer);
            RegSetString(hKey, "PartNumber",    dimm->part_number);
            RegSetString(hKey, "BankLocator",   dimm->bank_locator);
            RegSetString(hKey, "DeviceLocator", dimm->device_locator);
            RegCloseKey(hKey);
        }
    }

    klog(LOG_INFO, "SMBIOS", "Registry populated: BIOS, System, %u CPU(s), %u DIMM(s)",
         (uint32_t)s_info.cpu_count, (uint32_t)s_info.dimm_count);
}
