/* ============================================================================
 * smbios.c -- SMBIOS System Information Parser
 *
 * Walks SMBIOS structures found via UEFI Configuration Table.
 * Extracts BIOS, System, Baseboard, Processor, and Memory info.
 * ============================================================================ */

#include "kernel/smbios.h"
#include "kernel/uefi_config.h"
#include "kernel/klog.h"
#include "kernel/mm/pmm.h"
#include "kernel/boot_timing.h"
#include "registry.h"

static inline uint64_t smbios_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Maximum SMBIOS table size copied into kernel RAM at smbios_init.
 * Workstations top out around 8 KiB, servers with 50-100 DIMM slots
 * around 16-32 KiB; 64 KiB is generous and page-aligned.  Copy lifts
 * the 50-100 us per-byte cost of UEFI runtime-services memory access
 * into a one-time pass so subsequent string scans run from RAM. */
#define SMBIOS_COPY_MAX_BYTES   (16u * 4096u)

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

/* Per-array min(start)/max(end) summaries accumulated during the walk, keyed by
 * Memory Array Handle, then resolved against the finally-selected Type 16 handle
 * in smbios_finalize_mem_mapped(). Summarizing per handle (rather than buffering
 * raw ranges) makes the result independent of Type 16 / Type 19 emission order
 * AND bounded by the number of distinct arrays (typically 1-4) rather than the
 * range count -- so a flood of ranges can never evict the selected array's data.
 * Only a brand-new distinct handle is dropped when the table is full, and the
 * selected handle's summary is therefore always complete or entirely absent. */
#define SMBIOS_ARRAY_MAP_MAX 8
struct smbios_array_map { uint64_t min_start; uint64_t max_end; uint16_t handle; uint8_t used; };
static struct smbios_array_map s_array_maps[SMBIOS_ARRAY_MAP_MAX];
static uint32_t s_array_map_count;
static uint32_t s_array_map_dropped;

/* Table end pointer -- set by walk_structures, bounds all string scans.
 * Safe as file-scope static: SMBIOS init runs once, single-threaded. */
static const uint8_t *s_table_end;

/* Captured table base + length, exposed via smbios_get_raw_table() so
 * the Win32 GetSystemFirmwareTable('RSMB',...) path can return raw
 * bytes without re-parsing.  Points at the FIRMWARE-mapped table so
 * the firmware-tables inventory validator sees a real EfiRuntimeServices
 * address.  Set in walk_structures() ONLY when bounds are accepted.
 * Single-writer at boot, read-only after. */
static const uint8_t *s_table_base;
static uint32_t       s_table_size;

/* ---- String helper ---- */

/* SMBIOS spec wire-format entry-point lengths (DMTF DSP0134 3.7.0).
 * Do NOT use sizeof(struct smbios{2,3}_entry) as the upper bound --
 * the local C structs are unpacked and natural padding can stretch
 * sizeof beyond the wire length, letting a hostile firmware ep->length
 * pass a sizeof-based cap and let the checksum loop read one byte past
 * the spec record. */
#include "kernel/smbios_wire.h"

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
 * advertised max_len.  A naive smbios_get_string + str_copy pair loses the
 * s_table_end bound at the copy site -- a truncated table whose last string
 * lacks a NUL inside max_len would let str_copy walk past s_table_end into
 * unmapped memory.  This helper preserves the bound end-to-end.  Output is
 * always NUL-terminated. */
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

/* ---- Pure helpers (no global state; unit-testable) ---- */

uint64_t smbios_kb_to_bytes(uint32_t kb)
{
    /* 64-bit before the shift so values above 4 GiB do not wrap (Codex D2). */
    return ((uint64_t)kb) << 10;
}

int smbios_chassis_type_is_mobile(uint8_t code)
{
    switch (code) {
    case 0x08:  /* Portable */
    case 0x09:  /* Laptop */
    case 0x0A:  /* Notebook */
    case 0x0B:  /* Hand Held */
    case 0x0E:  /* Sub Notebook */
    case 0x1E:  /* Tablet (30) */
    case 0x1F:  /* Convertible (31) */
    case 0x20:  /* Detachable (32) */
        return 1;
    default:
        return 0;
    }
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
    /* Manufacturer (d[4]) + Product (d[5]) need length >= 6. The extended
     * string fields each require their own offset to be inside the formatted
     * area; reading them under a blanket length>=8 guard would treat the
     * first string-table byte as a field index on short/older records
     * (Codex design D4). Guard each field by its own offset. */
    if (hdr->length < 6) return;

    /* Clear all five fields first so a later (last-wins) Type 2 record that is
     * shorter cannot leave stale version/serial/asset strings from an earlier
     * record live. A malformed sub-6 record returns above without clobbering. */
    s_info.board_manufacturer[0] = '\0';
    s_info.board_product[0]      = '\0';
    s_info.board_version[0]      = '\0';
    s_info.board_serial[0]       = '\0';
    s_info.board_asset[0]        = '\0';

    smbios_copy_string(s_info.board_manufacturer, SMBIOS_STRING_MAX, hdr, d[4]);
    smbios_copy_string(s_info.board_product,      SMBIOS_STRING_MAX, hdr, d[5]);
    if (hdr->length >= 7)
        smbios_copy_string(s_info.board_version, SMBIOS_STRING_MAX, hdr, d[6]);
    if (hdr->length >= 8)
        smbios_copy_string(s_info.board_serial,  SMBIOS_STRING_MAX, hdr, d[7]);
    if (hdr->length >= 9)
        smbios_copy_string(s_info.board_asset,   SMBIOS_STRING_MAX, hdr, d[8]);
    /* Valid iff this record left ANY field non-empty so sparse firmware
     * (version/serial/asset only) still publishes; recomputed (set OR cleared)
     * each record so a later all-empty Type 2 cannot leave a stale flag. */
    s_info.board_valid =
        (s_info.board_manufacturer[0] || s_info.board_product[0] ||
         s_info.board_version[0] || s_info.board_serial[0] ||
         s_info.board_asset[0]) ? 1 : 0;
}

/* Type 3 -- System Enclosure / Chassis. Type code at offset 0x05 carries the
 * "lock present" bit in bit7; strip it for the enclosure type per DMTF
 * DSP0134 Table 17. */
static void parse_type3(const struct smbios_header *hdr)
{
    const uint8_t *d = (const uint8_t *)hdr;
    if (hdr->length < 9) return;  /* manufacturer..asset span offsets 4..8 */

    smbios_copy_string(s_info.chassis_manufacturer, SMBIOS_STRING_MAX, hdr, d[4]);
    s_info.chassis_type = (uint8_t)(d[5] & 0x7F);
    smbios_copy_string(s_info.chassis_serial, SMBIOS_STRING_MAX, hdr, d[7]);
    smbios_copy_string(s_info.chassis_asset,  SMBIOS_STRING_MAX, hdr, d[8]);
    s_info.chassis_valid = 1;
}

/* Pure: decode a Type 16 formatted record's maximum capacity (handling the
 * 0x80000000 -> 8-byte Extended Maximum Capacity sentinel) and device count.
 * All KB->byte math is 64-bit (Codex design D2). Returns 1 if long enough. */
int smbios_type16_decode(const uint8_t *rec, uint8_t length,
                         uint64_t *out_capacity_bytes,
                         uint16_t *out_device_count)
{
    if (out_capacity_bytes) *out_capacity_bytes = 0;
    if (out_device_count)   *out_device_count = 0;
    if (length < 0x0F) return 0;

    uint32_t cap_kb = (uint32_t)(rec[7] | ((uint32_t)rec[8] << 8) |
                      ((uint32_t)rec[9] << 16) | ((uint32_t)rec[0x0A] << 24));
    if (cap_kb == 0x80000000u) {
        /* Extended Maximum Capacity (8 bytes, in bytes) at offset 0x0F */
        if (length >= 0x17) {
            uint64_t ext = 0;
            uint32_t i;
            for (i = 0; i < 8; i++)
                ext |= ((uint64_t)rec[0x0F + i]) << (8 * i);
            if (out_capacity_bytes) *out_capacity_bytes = ext;
        }
    } else if (cap_kb != 0 && out_capacity_bytes) {
        *out_capacity_bytes = smbios_kb_to_bytes(cap_kb);
    }

    if (out_device_count)
        *out_device_count = (uint16_t)(rec[0x0D] | ((uint16_t)rec[0x0E] << 8));
    return 1;
}

/* Pure: decode a Type 19 record's mapped address range into an inclusive byte
 * [start, end]. 4-byte fields are KB; the 0xFFFFFFFF sentinel directs readers
 * to the 8-byte Extended byte addresses at 0x0F/0x17 (SMBIOS 2.7+). Returns 1
 * on a valid range, 0 if too short or malformed (end < start). */
int smbios_type19_decode(const uint8_t *rec, uint8_t length,
                         uint64_t *out_start, uint64_t *out_end,
                         uint16_t *out_array_handle)
{
    if (length < 0x0F) return 0;

    /* Memory Array Handle at offset 0x0C..0x0D (within the 0x0F base guard). */
    if (out_array_handle)
        *out_array_handle = (uint16_t)(rec[0x0C] | ((uint16_t)rec[0x0D] << 8));

    uint32_t start_kb = (uint32_t)(rec[4] | ((uint32_t)rec[5] << 8) |
                        ((uint32_t)rec[6] << 16) | ((uint32_t)rec[7] << 24));
    uint32_t end_kb   = (uint32_t)(rec[8] | ((uint32_t)rec[9] << 8) |
                        ((uint32_t)rec[0x0A] << 16) | ((uint32_t)rec[0x0B] << 24));

    /* Each 32-bit field independently signals "see extended" via its own
     * 0xFFFFFFFF sentinel (DSP0134 7.20), so resolve start and end from their
     * own source rather than treating one sentinel as switching both. A range
     * that crosses 4 TiB legitimately has one 32-bit field and one extended. */
    int start_ext = (start_kb == 0xFFFFFFFFu);
    int end_ext   = (end_kb == 0xFFFFFFFFu);
    if ((start_ext || end_ext) && length < 0x1F)
        return 0;  /* an extended field is needed but absent */

    uint64_t start, end;
    uint32_t i;
    if (start_ext) {
        uint64_t es = 0;
        for (i = 0; i < 8; i++) es |= ((uint64_t)rec[0x0F + i]) << (8 * i);
        start = es;  /* extended starting address is a byte address */
    } else {
        start = smbios_kb_to_bytes(start_kb);
    }
    if (end_ext) {
        uint64_t ee = 0;
        for (i = 0; i < 8; i++) ee |= ((uint64_t)rec[0x17 + i]) << (8 * i);
        end = ee;  /* extended ending address is an inclusive byte address */
    } else {
        /* ending address is the last KB of the range; inclusive byte end */
        end = smbios_kb_to_bytes(end_kb) + 1023u;
    }

    if (end < start) return 0;  /* reject malformed range */
    if (out_start) *out_start = start;
    if (out_end)   *out_end   = end;
    return 1;
}

/* Type 16 -- Physical Memory Array. Prefer the System Memory array (Use==0x03)
 * over video/flash/NVRAM/cache arrays: a non-system array emitted first must
 * not become the registry's "the RAM array" and mis-filter Type 19 ranges.
 * A non-system array is recorded only as a fallback and replaced when a
 * system-memory array appears; on replace, ranges already aggregated against
 * the old (wrong) handle are dropped. */
static void parse_type16(const struct smbios_header *hdr)
{
    const uint8_t *d = (const uint8_t *)hdr;
    if (hdr->length < 0x0F) return;

    int this_is_system = (d[5] == SMBIOS_MEM_ARRAY_USE_SYSTEM);
    if (s_info.mem_array_valid) {
        int sel_is_system = (s_info.mem_array_use == SMBIOS_MEM_ARRAY_USE_SYSTEM);
        /* Keep the current selection unless it is a non-system fallback and
         * this record is the system-memory array. Type 19 ranges are buffered
         * and aggregated after the walk against the final handle, so no
         * inline range reset is needed when the selection changes. */
        if (sel_is_system || !this_is_system) return;
    }

    s_info.mem_array_location         = d[4];
    s_info.mem_array_use              = d[5];
    s_info.mem_array_error_correction = d[6];
    s_info.mem_array_handle           = hdr->handle;
    smbios_type16_decode(d, hdr->length,
                         &s_info.mem_array_max_capacity_bytes,
                         &s_info.mem_array_device_count);
    s_info.mem_array_valid = 1;
}

/* Type 19 -- Memory Array Mapped Address. One structure per contiguous range;
 * multiple ranges can map to one array (RAM split around MMIO holes). Each
 * decoded range is buffered with its owning Memory Array Handle; the final
 * min(start)/max(end) aggregation against the selected Type 16 happens in
 * smbios_finalize_mem_mapped() after the whole table is walked, so the result
 * is independent of Type 16 / Type 19 emission order. */
static void parse_type19(const struct smbios_header *hdr)
{
    const uint8_t *d = (const uint8_t *)hdr;
    uint64_t start, end;
    uint16_t array_handle = 0;
    uint32_t i;
    if (!smbios_type19_decode(d, hdr->length, &start, &end, &array_handle))
        return;

    /* Fold the range into its array's running min/max (find-or-add by handle).
     * Bounded by distinct arrays, so the selected array's summary is never
     * evicted by ranges belonging to other arrays. */
    for (i = 0; i < s_array_map_count; i++) {
        if (s_array_maps[i].handle != array_handle) continue;
        if (start < s_array_maps[i].min_start) s_array_maps[i].min_start = start;
        if (end   > s_array_maps[i].max_end)   s_array_maps[i].max_end   = end;
        return;
    }
    if (s_array_map_count >= SMBIOS_ARRAY_MAP_MAX) { s_array_map_dropped++; return; }
    s_array_maps[s_array_map_count].handle    = array_handle;
    s_array_maps[s_array_map_count].min_start = start;
    s_array_maps[s_array_map_count].max_end   = end;
    s_array_maps[s_array_map_count].used      = 1;
    s_array_map_count++;
}

/* Resolve the selected Type 16 array's per-handle min/max summary into
 * mem_mapped_start/end. Called once after the table walk, so the selected
 * handle is final regardless of record order. If the selected handle has no
 * summary (its first range arrived after the distinct-array table filled, or
 * it had no Type 19 records), mem_mapped stays invalid rather than publishing
 * a partial map. */
static void smbios_finalize_mem_mapped(void)
{
    uint32_t i;
    if (!s_info.mem_array_valid) return;
    for (i = 0; i < s_array_map_count; i++) {
        if (s_array_maps[i].handle != s_info.mem_array_handle) continue;
        s_info.mem_mapped_start = s_array_maps[i].min_start;
        s_info.mem_mapped_end   = s_array_maps[i].max_end;
        s_info.mem_mapped_valid = 1;
        break;
    }
    if (s_array_map_dropped)
        klog(LOG_WARN, "SMBIOS",
             "Type 19: %u distinct array(s) beyond cap %u not summarized",
             (uint64_t)s_array_map_dropped, (uint64_t)SMBIOS_ARRAY_MAP_MAX);
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
 * Caller must gate s_info.valid on this return so a rejected table
 * cannot pose as successfully parsed. */
/* Copy bytes from firmware-mapped src to RAM dst, byte-at-a-time, up to
 * the lower of cap or where we observe the SMBIOS end-of-table sentinel
 * (type=127 followed by double-NUL).  This is the ONLY function that
 * reads from EfiRuntimeServicesData memory; one pass amortizes the
 * 50-100 us per-byte cost.  Returns the number of bytes actually copied
 * (always <= cap), or 0 on a degenerate input.  When walking detects
 * a terminator we still copy the type=127 header + its 2-byte
 * post-string NUL so the kernel's parser can confirm termination. */
static uint32_t smbios_copy_firmware_table(uint8_t *dst, uint32_t cap,
                                           const uint8_t *src,
                                           uint32_t src_max)
{
    if (!dst || !src || cap == 0 || src_max == 0)
        return 0;
    uint32_t limit = cap < src_max ? cap : src_max;
    /* Single pass: copy every byte to RAM. We do NOT short-circuit on
     * type=127 here -- the parser walk re-uses the same wire-format
     * scan on the RAM copy and stops naturally at the terminator.
     * Stopping early would make any followon byte read from firmware
     * memory, defeating the optimization. */
    for (uint32_t i = 0; i < limit; i++)
        dst[i] = src[i];
    return limit;
}

/* Per-type cumulative profile.  TYPE_BUCKET_COUNT covers SMBIOS standard
 * types 0-127; sparse table acceptable -- only types we parse get
 * non-zero entries, but the indexing is direct (constant-time). */
#define TYPE_BUCKET_COUNT  128u

struct smbios_type_profile {
    uint64_t total_ticks;
    uint32_t count;
};

static void smbios_dump_top3(const struct smbios_type_profile *prof,
                              uint64_t total_ticks)
{
    uint32_t top_idx[3] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    uint64_t top_ticks[3] = { 0, 0, 0 };
    for (uint32_t t = 0; t < TYPE_BUCKET_COUNT; t++) {
        uint64_t v = prof[t].total_ticks;
        if (v == 0) continue;
        if (v > top_ticks[0]) {
            top_ticks[2] = top_ticks[1]; top_idx[2] = top_idx[1];
            top_ticks[1] = top_ticks[0]; top_idx[1] = top_idx[0];
            top_ticks[0] = v;             top_idx[0] = t;
        } else if (v > top_ticks[1]) {
            top_ticks[2] = top_ticks[1]; top_idx[2] = top_idx[1];
            top_ticks[1] = v;             top_idx[1] = t;
        } else if (v > top_ticks[2]) {
            top_ticks[2] = v;             top_idx[2] = t;
        }
    }
    klog(LOG_INFO, "SMBIOS",
         "profile: total %u ms across all parsed structures",
         (uint64_t)boot_timing_tsc_delta_ms(total_ticks));
    for (int i = 0; i < 3; i++) {
        if (top_idx[i] == 0xFFFFFFFFu) break;
        klog(LOG_INFO, "SMBIOS",
             "profile: top%d type=%u count=%u total=%u ms",
             (uint64_t)(i + 1),
             (uint64_t)top_idx[i],
             (uint64_t)prof[top_idx[i]].count,
             (uint64_t)boot_timing_tsc_delta_ms(top_ticks[i]));
    }
}

/* Zero all parsed accumulators, preserving only the caller-set version. Called
 * at the start of every walk AND on every failure return, so a rejected or
 * truncated table never leaves partial fields readable in s_info (some
 * consumers read smbios_get_info() fields without first checking .valid). */
static void smbios_reset_info_keep_version(void)
{
    uint8_t saved_major = s_info.smbios_major;
    uint8_t saved_minor = s_info.smbios_minor;
    uint8_t *sp = (uint8_t *)&s_info;
    uint32_t i;
    for (i = 0; i < sizeof(s_info); i++) sp[i] = 0;
    s_info.smbios_major = saved_major;
    s_info.smbios_minor = saved_minor;
}

static int walk_structures(uintptr_t table_addr, uint32_t max_len)
{
    /* Reset parsed accumulators BEFORE any return so every exit path (bounds
     * reject, overflow, no-terminator) leaves s_info clean -- a failed 3.x
     * attempt cannot leak partial data into the 2.x fallback walk, and a
     * re-walk starts fresh. The caller writes the version into s_info just
     * before this call, so preserve it across the reset. */
    smbios_reset_info_keep_version();

    /* Pointer arithmetic on firmware-supplied table_addr + max_len can
     * wrap UINTPTR_MAX.  Reject (rather than silently cap) so a malformed
     * entry never marks s_info.valid = 1 with a partial parse. */
    if (max_len == 0 || table_addr == 0)
        return 0;
    uintptr_t end_addr;
    if (__builtin_add_overflow(table_addr, (uintptr_t)max_len, &end_addr))
        return 0;

    /* COPY the firmware table into kernel RAM ONCE.  Subsequent reads
     * (string-table scans, parse_type* field reads) hit DRAM at L1/L2
     * latency instead of trapping through hypervisor on every byte. */
    uint32_t copy_cap = max_len < SMBIOS_COPY_MAX_BYTES
                      ? max_len : SMBIOS_COPY_MAX_BYTES;
    uint32_t copy_pages = (copy_cap + 4095u) / 4096u;
    uintptr_t copy_phys = pmm_alloc_contiguous(copy_pages);
    /* s_table_base points at FIRMWARE memory so the firmware-tables
     * inventory validator sees a real EfiRuntimeServicesData address.
     * Parsing uses the RAM copy via s_table_end / parse_base below.
     * s_table_size = bytes actually validated (set after the walk
     * confirms the type=127 terminator). */
    s_table_base = (const uint8_t *)table_addr;
    s_table_size = 0;

    const uint8_t *parse_base;
    uint32_t parsed_bytes_cap;
    if (!copy_phys) {
        klog(LOG_WARN, "SMBIOS",
             "PMM alloc %u pages failed; parsing direct from firmware",
             (uint64_t)copy_pages);
        parse_base  = (const uint8_t *)table_addr;
        s_table_end = (const uint8_t *)end_addr;
        parsed_bytes_cap = max_len;
    } else {
        uint8_t *ram = (uint8_t *)copy_phys;
        uint32_t copied = smbios_copy_firmware_table(
            ram, copy_pages * 4096u,
            (const uint8_t *)table_addr, copy_cap);
        parse_base   = ram;
        s_table_end  = ram + copied;
        parsed_bytes_cap = copied;
    }

    const uint8_t *p = parse_base;
    const uint8_t *end = s_table_end;
    int saw_terminator = 0;

    /* Per-structure TSC profiling: tick deltas across each parse_type*
     * call so a future regression can be attributed to a specific type. */
    struct smbios_type_profile prof[TYPE_BUCKET_COUNT];
    for (uint32_t t = 0; t < TYPE_BUCKET_COUNT; t++) {
        prof[t].total_ticks = 0;
        prof[t].count = 0;
    }
    uint64_t total_ticks = 0;
    uint32_t parsed_count = 0;

    /* Reset the per-array Type 19 summary table for this walk (idempotent). */
    s_array_map_count = 0;
    s_array_map_dropped = 0;

    while (p + 4 <= end) {
        const struct smbios_header *hdr = (const struct smbios_header *)p;

        /* Validate header length BEFORE special-casing type=127 so a
         * truncated terminator (header alone, no trailing double-NUL)
         * cannot signal "saw terminator" and bypass the truncation
         * refusal below. */
        if (hdr->length < 4) break;
        if (hdr->length > (uint32_t)(end - p)) break;
        if (hdr->type == 127) {
            /* Spec: type 127 terminator must be followed by the
             * post-structure double-NUL.  Compute remaining bytes via
             * subtraction (post <= end is guaranteed by the
             * hdr->length bound above); never form post+2 which would
             * be UB when post == end - 1. */
            const uint8_t *post = p + hdr->length;
            if ((uint32_t)(end - post) < 2) break;   /* truncated post region */
            if (post[0] != 0 || post[1] != 0) break; /* missing double-NUL */
            saw_terminator = 1;
            break;
        }

        uint64_t t0 = smbios_rdtsc();
        switch (hdr->type) {
        case 0:  parse_type0(hdr);  break;
        case 1:  parse_type1(hdr);  break;
        case 2:  parse_type2(hdr);  break;
        case 3:  parse_type3(hdr);  break;
        case 4:  parse_type4(hdr);  break;
        case 16: parse_type16(hdr); break;
        case 17: parse_type17(hdr); break;
        case 19: parse_type19(hdr); break;
        default: break;
        }
        uint64_t dt = smbios_rdtsc() - t0;
        if (hdr->type < TYPE_BUCKET_COUNT) {
            prof[hdr->type].total_ticks += dt;
            prof[hdr->type].count++;
        }
        total_ticks += dt;
        parsed_count++;

        /* Advance past structure data */
        p += hdr->length;

        /* Skip the double-NUL-terminated string section */
        while (p + 1 < end && !(p[0] == '\0' && p[1] == '\0'))
            p++;
        p += 2;  /* skip the double NUL */
    }

    if (!saw_terminator) {
        klog(LOG_WARN, "SMBIOS",
             "no type=127 terminator within %u-byte parse window "
             "(firmware advertised %u bytes); rejecting partial parse",
             (uint64_t)parsed_bytes_cap, (uint64_t)max_len);
        s_table_base = (const uint8_t *)0;
        s_table_size = 0;
        s_table_end  = (const uint8_t *)0;
        /* Wipe any fields the partial parse populated so a consumer that
         * reads s_info without checking .valid sees no rejected firmware. */
        smbios_reset_info_keep_version();
        return 0;
    }

    /* Cap s_table_size at bytes actually parsed so smbios_get_raw_table
     * exports only the validated range -- never the firmware-advertised
     * length when the cap truncated us. */
    uint32_t consumed = (uint32_t)(p - parse_base);
    if (consumed > parsed_bytes_cap) consumed = parsed_bytes_cap;
    s_table_size = consumed < max_len ? consumed : max_len;

    /* Aggregate buffered Type 19 ranges against the final selected Type 16
     * handle now that the whole table is walked (order-independent). */
    smbios_finalize_mem_mapped();

    klog(LOG_INFO, "SMBIOS",
         "parsed %u structures from %u-byte table (RAM copy)",
         (uint64_t)parsed_count, (uint64_t)s_table_size);
    smbios_dump_top3(prof, total_ticks);
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
        /* Validate entry-point length BEFORE checksum: firmware-supplied
         * ep->length must be the spec wire-format size. ep->length=0
         * would make checksum sum 0 bytes and return "valid"; oversized
         * would make checksum read past the entry struct. Spec: SMBIOS
         * 3.x entry-point length is 0x18. */
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
            /* Validate entry-point length BEFORE checksum.  Spec:
             * SMBIOS 2.1 length 0x1E, SMBIOS 2.4+ length 0x1F.  Reject
             * anything outside [0x1E, 0x1F]. */
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
    uint32_t i, all_zero = 1, all_ff = 1;
    if (!s_info.valid) return 0;
    /* Match the bootloader's sentinel rule (see boot_smbios_format_uuid
     * in include/boot/boot_smbios_parse.h): both all-zero and all-FF
     * are the firmware "not specified" sentinels. Reporting all-FF as
     * a valid UUID would let userland mint machine_id-pinned entries
     * the bootloader's local_machine_id filter will never match. */
    for (i = 0; i < 16; i++) {
        uuid[i] = s_info.sys_uuid[i];
        if (s_info.sys_uuid[i] != 0x00) all_zero = 0;
        if (s_info.sys_uuid[i] != 0xFF) all_ff = 0;
    }
    if (all_zero || all_ff) return 0;
    return 1;
}

int smbios_get_chassis_type(void)
{
    if (!s_info.valid || !s_info.chassis_valid) return -1;
    return (int)s_info.chassis_type;
}

int smbios_chassis_is_laptop(void)
{
    if (!s_info.valid || !s_info.chassis_valid) return 0;
    return smbios_chassis_type_is_mobile(s_info.chassis_type);
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

/* Human-readable name for a SMBIOS Type 16 error-correction code. */
static const char *ecc_name(uint8_t code)
{
    switch (code) {
    case SMBIOS_ECC_OTHER:      return "Other";
    case SMBIOS_ECC_UNKNOWN:    return "Unknown";
    case SMBIOS_ECC_NONE:       return "None";
    case SMBIOS_ECC_PARITY:     return "Parity";
    case SMBIOS_ECC_SINGLE_BIT: return "Single-bit ECC";
    case SMBIOS_ECC_MULTI_BIT:  return "Multi-bit ECC";
    case SMBIOS_ECC_CRC:        return "CRC";
    default:                    return "Unknown";
    }
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

    /* ── HKLM\HARDWARE\Baseboard (Type 2) ────────────────────────────── */
    if (s_info.board_valid) {
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\Baseboard", 0,
                           (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                           &hKey, &disp) == ERROR_SUCCESS) {
            RegSetString(hKey, "Manufacturer", s_info.board_manufacturer);
            RegSetString(hKey, "Product",      s_info.board_product);
            RegSetString(hKey, "Version",      s_info.board_version);
            RegSetString(hKey, "Serial",       s_info.board_serial);
            RegSetString(hKey, "AssetTag",     s_info.board_asset);
            RegCloseKey(hKey);
        }
    }

    /* ── HKLM\HARDWARE\Chassis (Type 3) ──────────────────────────────── */
    if (s_info.chassis_valid) {
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\Chassis", 0,
                           (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                           &hKey, &disp) == ERROR_SUCCESS) {
            RegSetString(hKey, "Manufacturer", s_info.chassis_manufacturer);
            RegSetDword(hKey,  "Type",         (uint32_t)s_info.chassis_type);
            RegSetDword(hKey,  "IsLaptop",
                        (uint32_t)smbios_chassis_type_is_mobile(s_info.chassis_type));
            RegSetString(hKey, "Serial",       s_info.chassis_serial);
            RegSetString(hKey, "AssetTag",     s_info.chassis_asset);
            RegCloseKey(hKey);
        }
    }

    /* ── HKLM\HARDWARE\MemoryArray (Type 16 + Type 19) ───────────────── */
    if (s_info.mem_array_valid || s_info.mem_mapped_valid) {
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\MemoryArray", 0,
                           (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                           &hKey, &disp) == ERROR_SUCCESS) {
            if (s_info.mem_array_valid) {
                RegSetDword(hKey, "MaxCapacityMB",
                    (uint32_t)(s_info.mem_array_max_capacity_bytes / (1024ULL * 1024ULL)));
                RegSetQword(hKey, "MaxCapacityBytes",
                            s_info.mem_array_max_capacity_bytes);
                RegSetDword(hKey, "DeviceCount",
                            (uint32_t)s_info.mem_array_device_count);
                RegSetDword(hKey, "Location", (uint32_t)s_info.mem_array_location);
                RegSetDword(hKey, "Use",      (uint32_t)s_info.mem_array_use);
                RegSetDword(hKey, "ErrorCorrectionCode",
                            (uint32_t)s_info.mem_array_error_correction);
                RegSetString(hKey, "ErrorCorrection",
                             ecc_name(s_info.mem_array_error_correction));
            }
            if (s_info.mem_mapped_valid) {
                RegSetQword(hKey, "StartAddr", s_info.mem_mapped_start);
                RegSetQword(hKey, "EndAddr",   s_info.mem_mapped_end);
            }
            RegCloseKey(hKey);
        }
    }

    klog(LOG_INFO, "SMBIOS", "Registry populated: BIOS, System, %u CPU(s), %u DIMM(s)%s%s%s",
         (uint32_t)s_info.cpu_count, (uint32_t)s_info.dimm_count,
         s_info.board_valid ? ", Baseboard" : "",
         s_info.chassis_valid ? ", Chassis" : "",
         s_info.mem_array_valid ? ", MemoryArray" : "");
}


/* The public accessor returns the firmware-mapped pointer + length
 * captured at smbios_init(); callers MUST memcpy into their own buffer
 * before exposing to user-mode.  The pointer never crosses the syscall
 * boundary.  Returns 0 (not 1) when SMBIOS was unparsed or the bounds
 * are zero so the Win32 GetSystemFirmwareTable path can return
 * STATUS_NOT_FOUND cleanly.
 *
 * Cap the raw-table size at 16 MiB to mirror the 16 MiB ACPI SDT cap.
 * SMBIOS 3.x allows a 32-bit size field; firmware reporting a huge value
 * would let NtQuerySystemInformation(RSMB) compute a wrap-prone
 * hdr_size + raw_size and hand a near-zero capacity check to a
 * multi-gigabyte memcpy.  This cap rejects malformed firmware before it
 * can drive downstream wraparound.  Real SMBIOS tables are well under
 * 64 KiB. */
#define SMBIOS_RAW_TABLE_MAX (16U * 1024U * 1024U)

int smbios_get_raw_table(const uint8_t **out_addr, uint32_t *out_size)
{
    if (!out_addr || !out_size) return 0;
    if (!s_info.valid || s_table_base == (const uint8_t *)0 ||
        s_table_size == 0 || s_table_size > SMBIOS_RAW_TABLE_MAX) {
        *out_addr = (const uint8_t *)0;
        *out_size = 0;
        return 0;
    }
    *out_addr = s_table_base;
    *out_size = s_table_size;
    return 1;
}
