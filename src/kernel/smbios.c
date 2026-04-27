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

/* Table end pointer -- set by walk_structures, bounds all string scans.
 * Safe as file-scope static: SMBIOS init runs once, single-threaded. */
static const uint8_t *s_table_end;

/* ---- String helper ---- */

/* SMBIOS spec wire-format entry-point lengths (DMTF DSP0134 3.7.0).
 * Codex design review 2026-04-27 M1: must NOT use sizeof(struct
 * smbios{2,3}_entry) as the upper bound -- the local C structs are
 * unpacked and natural padding can stretch sizeof beyond the wire
 * length, letting a hostile firmware ep->length pass a sizeof-based
 * cap and let the checksum loop read one byte past the spec record. */
#define SMBIOS3_EP_LEN          0x18  /* 24 bytes; spec rev 3.x */
#define SMBIOS2_EP_LEN_MIN      0x1E  /* 30 bytes; spec rev 2.1 */
#define SMBIOS2_EP_LEN_MAX      0x1F  /* 31 bytes; spec rev 2.4+ */

/* SMBIOS strings follow the structure data as a double-NUL-terminated list.
 * String index 1 = first string, 2 = second, etc.  Index 0 = no string.
 * Scan is bounded by s_table_end to prevent overread on malformed tables. */
static const char *smbios_get_string(const struct smbios_header *hdr,
                                     uint8_t index)
{
    if (index == 0) return "";

    /* Strings start at hdr + hdr->length */
    const char *p = (const char *)hdr + hdr->length;
    const char *limit = (const char *)s_table_end;
    uint8_t cur = 1;

    while (cur < index) {
        /* Skip to end of current string */
        while (p < limit && *p != '\0') p++;
        if (p >= limit) return "";
        p++;  /* skip the NUL */
        if (p >= limit || *p == '\0') return "";  /* hit end or double-NUL */
        cur++;
    }
    return (p < limit) ? p : "";
}

/* Safe string copy with truncation. */
static void str_copy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Bounded SMBIOS string copy: like str_copy but never reads past s_table_end
 * even if the firmware-supplied table omits the terminating NUL inside the
 * advertised max_len. Codex adversarial review 2026-04-27 H2: the previous
 * smbios_get_string + str_copy pair lost the s_table_end bound at the copy
 * site -- a truncated table whose last string lacks a NUL inside max_len
 * would let str_copy walk past s_table_end into unmapped memory. This
 * helper preserves the bound end-to-end. Output is always NUL-terminated. */
static void smbios_copy_string(char *dst, uint32_t max,
                               const struct smbios_header *hdr,
                               uint8_t index)
{
    const char *src = smbios_get_string(hdr, index);
    const char *limit = (const char *)s_table_end;
    uint32_t i = 0;
    if (src && src < limit) {
        while (i < max - 1 && (src + i) < limit && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

/* ---- Structure parsers ---- */

static void parse_type0(const struct smbios_header *hdr)
{
    const uint8_t *d = (const uint8_t *)hdr;
    if (hdr->length < 18) return;

    smbios_copy_string(s_info.bios_vendor,  SMBIOS_STRING_MAX, hdr, d[4]);
    smbios_copy_string(s_info.bios_version, SMBIOS_STRING_MAX, hdr, d[5]);
    smbios_copy_string(s_info.bios_date,    SMBIOS_STRING_MAX, hdr, d[8]);
}

static void parse_type1(const struct smbios_header *hdr)
{
    const uint8_t *d = (const uint8_t *)hdr;
    if (hdr->length < 8) return;

    smbios_copy_string(s_info.sys_manufacturer, SMBIOS_STRING_MAX, hdr, d[4]);
    smbios_copy_string(s_info.sys_product,      SMBIOS_STRING_MAX, hdr, d[5]);
    if (hdr->length >= 7)
        smbios_copy_string(s_info.sys_version,  SMBIOS_STRING_MAX, hdr, d[6]);
    if (hdr->length >= 25) {
        smbios_copy_string(s_info.sys_serial,   SMBIOS_STRING_MAX, hdr, d[7]);
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

    smbios_copy_string(s_info.board_manufacturer, SMBIOS_STRING_MAX, hdr, d[4]);
    smbios_copy_string(s_info.board_product,      SMBIOS_STRING_MAX, hdr, d[5]);
}

static void parse_type4(const struct smbios_header *hdr)
{
    const uint8_t *d = (const uint8_t *)hdr;
    if (hdr->length < 26) return;

    /* Fill next per-socket slot */
    if (s_info.cpu_count >= SMBIOS_CPU_MAX) return;
    struct smbios_cpu_info *cpu = &s_info.cpus[s_info.cpu_count];

    smbios_copy_string(cpu->socket,       SMBIOS_STRING_MAX, hdr, d[4]);
    cpu->family = d[6];
    smbios_copy_string(cpu->manufacturer, SMBIOS_STRING_MAX, hdr, d[7]);

    /* Max speed at offset 0x14 (uint16_t LE) */
    cpu->max_speed_mhz = (uint16_t)(d[0x14] | ((uint16_t)d[0x15] << 8));

    /* Core / thread counts -- prefer 2-byte fields (SMBIOS 3.0+)
     * Core Count 2 at offset 0x2A (2 bytes), Thread Count 2 at 0x2E (2 bytes)
     * over 1-byte fields (SMBIOS 2.5+ at 0x23/0x25) for >255 core CPUs */
    if (hdr->length >= 0x30) {
        uint16_t cc2 = (uint16_t)(d[0x2A] | ((uint16_t)d[0x2B] << 8));
        uint16_t tc2 = (uint16_t)(d[0x2E] | ((uint16_t)d[0x2F] << 8));
        /* Use 2-byte fields if present and non-zero */
        cpu->core_count   = (cc2 > 0) ? cc2 : (uint16_t)d[0x23];
        cpu->thread_count = (tc2 > 0) ? tc2 : (uint16_t)d[0x25];
    } else {
        if (hdr->length >= 0x24)
            cpu->core_count   = (uint16_t)d[0x23];
        if (hdr->length >= 0x26)
            cpu->thread_count = (uint16_t)d[0x25];
    }

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
    if (raw_size == 0x7FFF) {
        /* SMBIOS 2.7+ sentinel: use extended size at offset 0x1C */
        if (hdr->length < 0x20) return;  /* Extended field not present -- skip */
        size_mb = (uint32_t)(d[0x1C] | ((uint32_t)d[0x1D] << 8) |
                  ((uint32_t)d[0x1E] << 16) | ((uint32_t)d[0x1F] << 24));
        if (size_mb == 0) return;  /* Invalid extended size */
    } else if (raw_size & 0x8000) {
        /* Size in KB */
        size_mb = (uint32_t)(raw_size & 0x7FFF) / 1024;
    } else {
        size_mb = (uint32_t)raw_size;
    }

    /* Aggregate totals (legacy callers) */
    s_info.ram_total_mb += size_mb;
    s_info.ram_dimm_count++;

    /* Aggregate speed: prefer configured (0x20) over max (0x15) */
    if (s_info.ram_speed_mhz == 0) {
        if (hdr->length >= 0x22) {
            uint16_t cfg = (uint16_t)(d[0x20] | ((uint16_t)d[0x21] << 8));
            if (cfg > 0) s_info.ram_speed_mhz = cfg;
        }
        if (s_info.ram_speed_mhz == 0 && hdr->length >= 0x17)
            s_info.ram_speed_mhz = (uint16_t)(d[0x15] | ((uint16_t)d[0x16] << 8));
    }
    if (s_info.ram_type == 0)
        s_info.ram_type = d[0x12];

    /* Per-DIMM slot */
    if (s_info.dimm_count >= SMBIOS_DIMM_MAX) return;
    struct smbios_dimm_info *dimm = &s_info.dimms[s_info.dimm_count];

    dimm->size_mb   = size_mb;
    dimm->mem_type  = d[0x12];
    /* Prefer configured speed (actual clock, offset 0x20, SMBIOS 2.7+)
     * over max speed (rated, offset 0x15) */
    if (hdr->length >= 0x22) {
        uint16_t cfg = (uint16_t)(d[0x20] | ((uint16_t)d[0x21] << 8));
        dimm->speed_mhz = (cfg > 0) ? cfg :
            (uint16_t)(d[0x15] | ((uint16_t)d[0x16] << 8));
    } else if (hdr->length >= 0x17) {
        dimm->speed_mhz = (uint16_t)(d[0x15] | ((uint16_t)d[0x16] << 8));
    }

    /* String fields: device locator (d[0x10]), bank locator (d[0x11]),
     * manufacturer (d[0x17]), part number (d[0x1A]) -- SMBIOS 2.3+ */
    smbios_copy_string(dimm->device_locator, SMBIOS_STRING_MAX, hdr, d[0x10]);
    smbios_copy_string(dimm->bank_locator,   SMBIOS_STRING_MAX, hdr, d[0x11]);
    if (hdr->length >= 0x1B) {
        smbios_copy_string(dimm->manufacturer, SMBIOS_STRING_MAX, hdr, d[0x17]);
        smbios_copy_string(dimm->part_number,  SMBIOS_STRING_MAX, hdr, d[0x1A]);
    }

    dimm->valid = 1;
    s_info.dimm_count++;
}

/* ---- Table walker ---- */

/* Returns 1 on a successful walk, 0 if the firmware-supplied bounds were
 * rejected (max_len==0, table_addr==0, or table_addr+max_len overflow).
 * Codex re-adversarial review 2026-04-27 M2: caller must gate
 * s_info.valid on this return so a rejected table cannot pose as
 * successfully parsed. */
static int walk_structures(uintptr_t table_addr, uint32_t max_len)
{
    /* Codex adversarial review 2026-04-27 M3: pointer arithmetic on
     * firmware-supplied table_addr + max_len can wrap UINTPTR_MAX. Reject
     * (rather than silently cap) so a malformed entry never marks
     * s_info.valid = 1 with a partial parse. */
    if (max_len == 0 || table_addr == 0)
        return 0;
    uintptr_t end_addr;
    if (__builtin_add_overflow(table_addr, (uintptr_t)max_len, &end_addr))
        return 0;
    const uint8_t *p = (const uint8_t *)table_addr;
    const uint8_t *end = (const uint8_t *)end_addr;

    /* Set file-scope end pointer for bounded string extraction */
    s_table_end = end;

    while (p + 4 <= end) {
        const struct smbios_header *hdr = (const struct smbios_header *)p;

        if (hdr->type == 127) break;  /* End of table */
        if (hdr->length < 4) break;   /* Corrupt */
        if (hdr->length > (uint32_t)(end - p)) break;  /* Truncated */

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
    return 1;
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

/* Validate entry point checksum (SMBIOS spec §6.1: sum of all bytes == 0) */
static int smbios_checksum_valid(const uint8_t *data, uint32_t len)
{
    uint8_t sum = 0;
    uint32_t i;
    for (i = 0; i < len; i++)
        sum += data[i];
    return (sum == 0);
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

        /* Validate anchor string "_SM3_" */
        if (ep->anchor[0] != '_' || ep->anchor[1] != 'S' ||
            ep->anchor[2] != 'M' || ep->anchor[3] != '3' ||
            ep->anchor[4] != '_') {
            klog(LOG_WARN, "SMBIOS", "3.x anchor mismatch -- skipping");
            ep_addr = 0;  /* fall through to 2.x */
        }
        /* Validate entry-point length BEFORE checksum (Codex review
         * 2026-04-27 H1): firmware-supplied ep->length must be the spec
         * wire-format size. ep->length=0 would make checksum sum 0 bytes
         * and return "valid"; oversized would make checksum read past the
         * entry struct. Spec: SMBIOS 3.x entry-point length is 0x18. */
        else if (ep->length != SMBIOS3_EP_LEN) {
            klog(LOG_WARN, "SMBIOS",
                 "3.x entry length %u not %u -- rejecting",
                 (uint32_t)ep->length, (uint32_t)SMBIOS3_EP_LEN);
            ep_addr = 0;
        }
        /* Validate entry point checksum (now bounded by validated length) */
        else if (!smbios_checksum_valid((const uint8_t *)ep, ep->length)) {
            klog(LOG_WARN, "SMBIOS", "3.x entry point checksum invalid");
            ep_addr = 0;
        }
    }

    if (ep_addr != 0) {
        const struct smbios3_entry *ep =
            (const struct smbios3_entry *)ep_addr;

        s_info.smbios_major = ep->major_version;
        s_info.smbios_minor = ep->minor_version;

        if (walk_structures((uintptr_t)ep->struct_table_addr,
                            ep->max_struct_size)) {
            s_info.valid = 1;
        } else {
            klog(LOG_WARN, "SMBIOS",
                 "3.x table bounds rejected -- not parsed");
        }
    } else {
        /* Fallback: SMBIOS 2.x */
        struct boot_uefi_guid guid2 = UEFI_GUID_SMBIOS;
        ep_addr = uefi_find_config_table(&guid2);

        if (ep_addr != 0) {
            const struct smbios2_entry *ep =
                (const struct smbios2_entry *)ep_addr;

            /* Validate anchor string "_SM_" */
            if (ep->anchor[0] != '_' || ep->anchor[1] != 'S' ||
                ep->anchor[2] != 'M' || ep->anchor[3] != '_') {
                klog(LOG_WARN, "SMBIOS", "2.x anchor mismatch -- skipping");
                ep_addr = 0;
            }
            /* Validate entry-point length BEFORE checksum (Codex review
             * 2026-04-27 H1). Spec: SMBIOS 2.1 length 0x1E, SMBIOS 2.4+
             * length 0x1F. Reject anything outside [0x1E, 0x1F]. */
            else if (ep->length < SMBIOS2_EP_LEN_MIN ||
                     ep->length > SMBIOS2_EP_LEN_MAX) {
                klog(LOG_WARN, "SMBIOS",
                     "2.x entry length %u not in [%u,%u] -- rejecting",
                     (uint32_t)ep->length,
                     (uint32_t)SMBIOS2_EP_LEN_MIN,
                     (uint32_t)SMBIOS2_EP_LEN_MAX);
                ep_addr = 0;
            }
            /* Validate entry point checksum (now bounded by validated length) */
            else if (!smbios_checksum_valid((const uint8_t *)ep, ep->length)) {
                klog(LOG_WARN, "SMBIOS", "2.x entry point checksum invalid");
                ep_addr = 0;
            }
        }

        if (ep_addr != 0) {
            const struct smbios2_entry *ep =
                (const struct smbios2_entry *)ep_addr;

            s_info.smbios_major = ep->major_version;
            s_info.smbios_minor = ep->minor_version;

            if (walk_structures((uintptr_t)ep->struct_table_addr,
                                (uint32_t)ep->struct_table_length)) {
                s_info.valid = 1;
            } else {
                klog(LOG_WARN, "SMBIOS",
                     "2.x table bounds rejected -- not parsed");
            }
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
