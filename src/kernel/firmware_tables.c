/* ============================================================================
 * firmware_tables.c -- Unified firmware table catalog
 *
 * Aggregates UEFI configuration table entries, ACPI SDTs, the SMBIOS
 * structure-table region, FPDT, ESRT, and DTB into a single read-only
 * catalog queryable by GUID, name, or owner. Per-provider validation is
 * NOT duplicated here -- this code consumes the helpers in acpi.c,
 * smbios.c, and uefi_config.c that already perform checksum / length
 * checks. Defense-in-depth range/checksum re-validation against the UEFI
 * memory map is performed by firmware_table_validate_all() (defined
 * below) and runs from firmware_tables_init() before the boot summary.
 *
 * SMP: built once on the BSP during Phase 1 boot (before sti); read-only
 * thereafter, so no locking is required on the lookup paths.
 * ============================================================================ */

#include "kernel/firmware_tables.h"
#include "kernel/boot_info.h"
#include "kernel/boot_init.h"
#include "kernel/uefi_config.h"
#include "kernel/acpi.h"
#include "kernel/smbios.h"
#include "kernel/klog.h"
#include "libc/string.h"

/* Cross-source consistency: ESRT entry stride literal must equal the
 * actual struct laid out by uefi_config.c.  If struct boot_uefi_guid or
 * struct esrt_entry ever grows or shrinks, this assertion fails at
 * compile time so the validator's range check stays in sync with the
 * parser's wire layout. */
_Static_assert(sizeof(struct esrt_entry) == 40,
               "ESRT entry wire-format stride must be 40 bytes");

/* Compile-time sanity: cap must be at least the config-table cap so every
 * UEFI cfg-table slot can land before ACPI SDTs start filling the rest. */
_Static_assert(FIRMWARE_TABLE_MAX >= BOOT_CONFIG_TABLE_MAX,
               "FIRMWARE_TABLE_MAX must hold every config_table[] slot");

static struct firmware_table_entry g_entries[FIRMWARE_TABLE_MAX];
static uint32_t g_count;
static uint8_t  g_initialized;

/* ---- Tiny helpers -------------------------------------------------------- */

static int guid_eq(const struct boot_uefi_guid *a,
                   const struct boot_uefi_guid *b)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (uint32_t i = 0; i < 16; i++)
        if (pa[i] != pb[i]) return 0;
    return 1;
}

static int guid_is_zero(const struct boot_uefi_guid *g)
{
    const uint8_t *p = (const uint8_t *)g;
    for (uint32_t i = 0; i < 16; i++)
        if (p[i] != 0) return 0;
    return 1;
}

/* Bounded copy into a fixed-size char field. Always NUL-terminates. */
static void copy_field(char *dst, uint32_t dst_size, const char *src)
{
    if (dst_size == 0) return;
    uint32_t i = 0;
    if (src) {
        while (i + 1 < dst_size && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
    while (++i < dst_size) dst[i - 1] = '\0';  /* zero remainder */
}

static int str_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Map a UEFI cfg-table GUID to its short name. NULL for unknown GUIDs. */
static const char *cfg_table_short_name(const struct boot_uefi_guid *g)
{
    struct boot_uefi_guid acpi20 = UEFI_GUID_ACPI_20;
    struct boot_uefi_guid acpi10 = UEFI_GUID_ACPI_10;
    struct boot_uefi_guid smbios3 = UEFI_GUID_SMBIOS3;
    struct boot_uefi_guid smbios = UEFI_GUID_SMBIOS;
    struct boot_uefi_guid memattr = UEFI_GUID_MEM_ATTR;
    struct boot_uefi_guid rtprops = UEFI_GUID_RT_PROPS;
    struct boot_uefi_guid conform = UEFI_GUID_CONFORMANCE;
    struct boot_uefi_guid dtb = UEFI_GUID_DTB;
    struct boot_uefi_guid fpdt = UEFI_GUID_FPDT;
    struct boot_uefi_guid esrt = UEFI_GUID_ESRT;

    if (guid_eq(g, &acpi20))  return "ACPI2.0";
    if (guid_eq(g, &acpi10))  return "ACPI1.0";
    if (guid_eq(g, &smbios3)) return "SMBIOS3";
    if (guid_eq(g, &smbios))  return "SMBIOS";
    if (guid_eq(g, &memattr)) return "MemAttr";
    if (guid_eq(g, &rtprops)) return "RtProps";
    if (guid_eq(g, &conform)) return "Conform";
    if (guid_eq(g, &dtb))     return "DTB";
    if (guid_eq(g, &fpdt))    return "FPDT";
    if (guid_eq(g, &esrt))    return "ESRT";
    return (const char *)0;
}

static const char *cfg_table_owner(const struct boot_uefi_guid *g)
{
    struct boot_uefi_guid acpi20 = UEFI_GUID_ACPI_20;
    struct boot_uefi_guid acpi10 = UEFI_GUID_ACPI_10;
    struct boot_uefi_guid smbios3 = UEFI_GUID_SMBIOS3;
    struct boot_uefi_guid smbios = UEFI_GUID_SMBIOS;
    struct boot_uefi_guid memattr = UEFI_GUID_MEM_ATTR;
    struct boot_uefi_guid rtprops = UEFI_GUID_RT_PROPS;
    struct boot_uefi_guid conform = UEFI_GUID_CONFORMANCE;
    struct boot_uefi_guid dtb = UEFI_GUID_DTB;
    struct boot_uefi_guid fpdt = UEFI_GUID_FPDT;
    struct boot_uefi_guid esrt = UEFI_GUID_ESRT;

    if (guid_eq(g, &acpi20) || guid_eq(g, &acpi10)) return "ACPI";
    if (guid_eq(g, &smbios3) || guid_eq(g, &smbios)) return "SMBIOS";
    if (guid_eq(g, &memattr)) return "MAT";
    if (guid_eq(g, &rtprops)) return "RtProps";
    if (guid_eq(g, &conform)) return "Conform";
    if (guid_eq(g, &dtb))     return "DTB";
    if (guid_eq(g, &fpdt))    return "FPDT";
    if (guid_eq(g, &esrt))    return "ESRT";
    return "UEFI";
}

/* ---- Catalog construction ----------------------------------------------- */

static struct firmware_table_entry *alloc_entry(void)
{
    if (g_count >= FIRMWARE_TABLE_MAX) return (struct firmware_table_entry *)0;
    struct firmware_table_entry *e = &g_entries[g_count++];
    /* Zero the fresh slot so optional fields default cleanly. */
    uint8_t *p = (uint8_t *)e;
    for (uint32_t i = 0; i < sizeof(*e); i++) p[i] = 0;
    return e;
}

/* Format a 4-byte ACPI signature (LE) into a NUL-terminated char[5]. */
static void format_acpi_sig(uint32_t sig, char out[5])
{
    out[0] = (char)(sig & 0xFF);
    out[1] = (char)((sig >> 8) & 0xFF);
    out[2] = (char)((sig >> 16) & 0xFF);
    out[3] = (char)((sig >> 24) & 0xFF);
    out[4] = '\0';
    /* Sanitize: ACPI signatures are printable ASCII per spec, but if
     * firmware misbehaves and returns control bytes we render '?' so the
     * boot log stays clean. */
    for (uint32_t i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)out[i];
        if (c < 0x20 || c > 0x7E) out[i] = '?';
    }
}

/* Returns 1 only when the per-provider helper that owns this short
 * name has actually succeeded.  ACPI uses acpi_is_ready() (set on
 * acpi_init success); SMBIOS uses smbios_get_raw_table() (returns 1
 * only after smbios_init parsed the entry point).  Other known names
 * (FPDT, MAT, RtProps, Conform, ESRT, DTB) have no boot-time oracle
 * exposed today and are treated as unvalidated until the firmware-
 * table validator (firmware_table_validate_all) inspects them. */
static int cfg_owner_validated(const char *short_name)
{
    if (!short_name) return 0;
    if (str_eq(short_name, "ACPI2.0") || str_eq(short_name, "ACPI1.0"))
        return acpi_is_ready();
    if (str_eq(short_name, "SMBIOS3") || str_eq(short_name, "SMBIOS")) {
        const uint8_t *addr = (const uint8_t *)0;
        uint32_t size = 0;
        return smbios_get_raw_table(&addr, &size);
    }
    return 0;  /* known short_name but no provider oracle */
}

static void catalog_uefi_cfg_tables(void)
{
    uint32_t n = g_boot_info.config_table_count;
    if (n > BOOT_CONFIG_TABLE_MAX) n = BOOT_CONFIG_TABLE_MAX;

    for (uint32_t i = 0; i < n; i++) {
        const struct boot_uefi_config_entry *src = &g_boot_info.config_table[i];
        struct firmware_table_entry *e = alloc_entry();
        if (!e) {
            klog(LOG_WARN, "FW", "catalog full: %u cfg-table entries dropped",
                 (uint32_t)(n - i));
            return;
        }
        e->guid = src->guid;
        e->phys_addr = src->table_addr;
        e->size = 0;  /* size not known from cfg-table alone */
        e->source = FW_SOURCE_UEFI_CFG_TABLE;

        const char *short_name = cfg_table_short_name(&src->guid);

        /* Status assignment honours the header contract: VALIDATED ONLY
         * when a per-provider helper has confirmed success.  Knowing the
         * GUID is not enough -- acpi_init may have rejected the RSDP,
         * smbios_init may have failed entry-point parsing, etc. */
        if (src->table_addr == 0) {
            e->status = FW_STATUS_DEGRADED;
            e->degraded_reason = FW_DEGRADED_NULL_POINTER;
        } else if (cfg_owner_validated(short_name)) {
            e->status = FW_STATUS_VALIDATED;
            e->degraded_reason = FW_DEGRADED_NONE;
        } else {
            /* Includes both unknown GUIDs AND known-name entries whose
             * provider has not yet been validated (FPDT/MAT/RtProps/
             * Conform/ESRT/DTB own their own checksums, but no oracle
             * is exposed at catalog time -- those land here, and
             * firmware_table_validate_all re-checks them). */
            e->status = FW_STATUS_UNKNOWN_PROFILE;
            e->degraded_reason = FW_DEGRADED_NONE;
        }

        copy_field(e->name, FIRMWARE_TABLE_NAME_MAX,
                   short_name ? short_name : "uefi-cfg");
        copy_field(e->owner, FIRMWARE_TABLE_OWNER_MAX,
                   cfg_table_owner(&src->guid));
    }
}

struct acpi_walk_ctx {
    uint32_t added;
};

static int acpi_record_into_catalog(uint32_t index, uint32_t signature,
                                     const uint8_t *addr, uint32_t size,
                                     void *raw_ctx)
{
    (void)index;
    struct acpi_walk_ctx *ctx = (struct acpi_walk_ctx *)raw_ctx;

    /* Continue iterating once the catalog is full so the caller can
     * compare emitted vs added and emit an accurate overflow warning;
     * the per-record cost beyond the cap is one bounded callback, not
     * an O(N) re-walk per ordinal. */
    if (g_count >= FIRMWARE_TABLE_MAX)
        return 1;

    struct firmware_table_entry *e = alloc_entry();
    if (!e) return 1;

    char sig_str[5];
    format_acpi_sig(signature, sig_str);

    e->signature = signature;
    e->phys_addr = (uintptr_t)addr;
    e->size = size;
    e->source = FW_SOURCE_ACPI_SDT;
    e->status = FW_STATUS_VALIDATED;
    e->degraded_reason = FW_DEGRADED_NONE;
    copy_field(e->name, FIRMWARE_TABLE_NAME_MAX, sig_str);
    copy_field(e->owner, FIRMWARE_TABLE_OWNER_MAX, "ACPI");
    ctx->added++;
    return 1;
}

static void catalog_acpi_sdts(void)
{
    /* Single validated walk: acpi_for_each_record does the XSDT/RSDT root
     * check once and validates each child once. Duplicate-signature
     * tables (multiple SSDTs are typical) arrive as distinct callbacks,
     * so each lands as its own catalog entry. */
    struct acpi_walk_ctx ctx = { 0 };
    uint32_t emitted = acpi_for_each_record(acpi_record_into_catalog, &ctx);

    if (emitted > ctx.added) {
        klog(LOG_WARN, "FW",
             "ACPI SDT overflow: %u of %u catalogued (cap=%u)",
             ctx.added, emitted, FIRMWARE_TABLE_MAX);
    }
}

static void catalog_smbios_raw(void)
{
    const uint8_t *addr = (const uint8_t *)0;
    uint32_t size = 0;
    if (!smbios_get_raw_table(&addr, &size))
        return;

    struct firmware_table_entry *e = alloc_entry();
    if (!e) return;

    e->phys_addr = (uintptr_t)addr;
    e->size = size;
    e->source = FW_SOURCE_SMBIOS_RAW;
    e->status = FW_STATUS_VALIDATED;
    e->degraded_reason = FW_DEGRADED_NONE;
    copy_field(e->name, FIRMWARE_TABLE_NAME_MAX, "SMBIOS-raw");
    copy_field(e->owner, FIRMWARE_TABLE_OWNER_MAX, "SMBIOS");
}

/* ---- Range + checksum validation --------------------------------------- */

/* SMBIOS entry-point wire-format sizes (DMTF DSP0134 6.1).  Re-declared
 * here as private structs so we do not have to expose smbios.c internals
 * via a header.  The static_asserts below pin the sizes to the spec
 * wire-format -- if the compiler ever pads these, validation breaks
 * loudly at compile time. */
struct fw_smbios3_ep_view {
    uint8_t  anchor[5];          /* "_SM3_" */
    uint8_t  checksum;
    uint8_t  length;             /* spec: 0x18 */
    uint8_t  major_version;
    uint8_t  minor_version;
    uint8_t  docrev;
    uint8_t  revision;
    uint8_t  reserved;
    uint32_t max_struct_size;
    uint64_t struct_table_addr;
};
struct fw_smbios2_ep_view {
    uint8_t  anchor[4];          /* "_SM_" */
    uint8_t  checksum;
    uint8_t  length;             /* spec: 0x1E or 0x1F */
    uint8_t  rest[25];           /* opaque tail; full validation reads ep->length bytes */
};
#define FW_SMBIOS3_EP_LEN     0x18u
#define FW_SMBIOS2_EP_LEN_MIN 0x1Eu
#define FW_SMBIOS2_EP_LEN_MAX 0x1Fu

_Static_assert(sizeof(struct fw_smbios3_ep_view) == FW_SMBIOS3_EP_LEN,
               "SMBIOS 3.x wire-format entry point must be exactly 0x18 bytes");
_Static_assert(sizeof(struct fw_smbios2_ep_view) == FW_SMBIOS2_EP_LEN_MAX,
               "SMBIOS 2.x view sized to spec maximum (0x1F)");

/* Minimal RSDP layouts mirroring acpi.h structures so we can validate
 * without pulling acpi internals.  Field offsets must match. */
struct fw_rsdp_v1_view {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
};
struct fw_rsdp_v2_view {
    struct fw_rsdp_v1_view v1;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));
_Static_assert(sizeof(struct fw_rsdp_v1_view) == 20,
               "RSDP v1 wire-format must be 20 bytes");
_Static_assert(sizeof(struct fw_rsdp_v2_view) == 36,
               "RSDP v2 wire-format must be 36 bytes");

/* SDT header view: every ACPI SDT (and FPDT) starts with this 36-byte
 * header.  signature/length/checksum are the only fields we touch. */
struct fw_sdt_header_view {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};
_Static_assert(sizeof(struct fw_sdt_header_view) == 36,
               "ACPI SDT header wire-format must be 36 bytes");

/* ESRT header at table base; entries follow immediately. */
struct fw_esrt_header_view {
    uint32_t fw_resource_count;
    uint32_t fw_resource_count_max;
    uint64_t fw_resource_version;
};
#define FW_ESRT_ENTRY_SIZE ((uint32_t)sizeof(struct esrt_entry))
_Static_assert(sizeof(struct fw_esrt_header_view) == 16,
               "ESRT header wire-format must be 16 bytes");

/* Format-specific maxima for firmware-declared lengths.  Used to cap
 * fw_sum8_is_zero loops before scanning firmware-supplied bytes: a
 * malicious or corrupt firmware declaring multi-MB lengths could
 * otherwise force a pre-sti byte sum over an unbounded span and stall
 * boot.  The caps are several orders of magnitude above any sane
 * production size for each table:
 *
 *   RSDP -- ACPI specification 5.2.5.3 fixes RSDP at 20 (v1) or 36
 *           (v2) bytes; the cap is tightened to a single page for
 *           defense-in-depth.
 *   FPDT -- ACPI specification 5.2.23 puts FPDT at <= ~16 KiB in
 *           practice (header + a handful of pointer records); cap at
 *           one page.
 *   SDT  -- ACPI SDTs are bounded by acpi_for_each_record at parse
 *           time; we still cap at 1 MiB as a defensive ceiling for any
 *           pathological DSDT / SSDT. */
#define FW_RSDP_LENGTH_MAX     0x1000u   /* one page */
#define FW_FPDT_LENGTH_MAX     0x1000u   /* one page */
#define FW_SDT_LENGTH_MAX      0x100000u /* 1 MiB */

/* 8-bit sum-to-zero, matching ACPI / SMBIOS spec checksums. */
static int fw_sum8_is_zero(const uint8_t *bytes, uint32_t len)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum = (uint8_t)(sum + bytes[i]);
    return sum == 0;
}

#ifdef KERNEL_TESTS
/* Test-only escape hatch: when nonzero, fw_mmap_contains returns 1
 * unconditionally so tests can exercise checksum / length logic against
 * buffers in kernel BSS without tripping the range check. The test
 * helper sets and clears this around individual validator calls. */
static int g_fw_test_bypass_range;
#endif

/* True iff [addr, addr+len) sits entirely within a single mmap descriptor
 * whose memory type can legally host firmware-published data.
 *
 *   ACCEPTED: RESERVED, LOADER_CODE/DATA, BOOT_SERVICES_CODE/DATA,
 *             RUNTIME_CODE/DATA, ACPI_RECLAIM, ACPI_NVS, PERSISTENT.
 *   REJECTED: CONVENTIONAL (free RAM -- firmware data here is corrupt),
 *             UNUSABLE, MMIO, MMIO_PORT, PAL_CODE.
 *
 * len==0 is rejected (callers should use 1 for "single byte" probes); a
 * zero-length region cannot be meaningfully bounded. */
static int fw_mmap_contains(uintptr_t addr, uint64_t len)
{
#ifdef KERNEL_TESTS
    if (g_fw_test_bypass_range) return 1;
#endif
    if (len == 0) return 0;
    /* End-exclusive overflow guard: if addr + len wraps, reject. */
    if (addr + len < addr) return 0;
    uint64_t end = (uint64_t)addr + len;

    uint32_t n = g_boot_info.mmap_count;
    for (uint32_t i = 0; i < n; i++) {
        const struct boot_mmap_entry *m = &g_boot_info.mmap[i];
        if (m->length == 0) continue;
        uint64_t m_end = m->base_addr + m->length;
        if (m_end < m->base_addr) continue;  /* malformed entry: skip */
        if ((uint64_t)addr < m->base_addr || end > m_end) continue;

        uint32_t t = m->uefi_memory_type;
        switch (t) {
        case UEFI_MMAP_RESERVED:
        case UEFI_MMAP_LOADER_CODE:
        case UEFI_MMAP_LOADER_DATA:
        case UEFI_MMAP_BOOT_SERVICES_CODE:
        case UEFI_MMAP_BOOT_SERVICES_DATA:
        case UEFI_MMAP_RUNTIME_CODE:
        case UEFI_MMAP_RUNTIME_DATA:
        case UEFI_MMAP_ACPI_RECLAIM:
        case UEFI_MMAP_ACPI_NVS:
        case UEFI_MMAP_PERSISTENT:
            return 1;
        default:
            return 0;  /* CONVENTIONAL / UNUSABLE / MMIO / MMIO_PORT / PAL */
        }
    }
    return 0;
}

static void fw_degrade(struct firmware_table_entry *e, uint8_t reason,
                       const char *why)
{
    e->status = FW_STATUS_DEGRADED;
    e->degraded_reason = reason;
    klog(LOG_WARN, "FW", "%s [%s @0x%lx]: %s",
         e->name[0] ? e->name : "(unnamed)",
         e->owner[0] ? e->owner : "?",
         (unsigned long)e->phys_addr, why);
}

/* ---- Per-source validators --------------------------------------------- */

static void fw_validate_acpi_sdt(struct firmware_table_entry *e)
{
    if (e->size < sizeof(struct fw_sdt_header_view)) {
        fw_degrade(e, FW_DEGRADED_LENGTH_BAD, "SDT size below header");
        return;
    }
    if (e->size > FW_SDT_LENGTH_MAX) {
        fw_degrade(e, FW_DEGRADED_LENGTH_BAD,
                   "SDT size exceeds defensive cap");
        return;
    }
    if (!fw_mmap_contains(e->phys_addr, e->size)) {
        fw_degrade(e, FW_DEGRADED_RANGE_UNMAPPED, "SDT span outside firmware mmap");
        return;
    }
    const struct fw_sdt_header_view *h =
        (const struct fw_sdt_header_view *)e->phys_addr;
    if (h->length != e->size) {
        fw_degrade(e, FW_DEGRADED_LENGTH_BAD,
                   "SDT header.length disagrees with catalog size");
        return;
    }
    if (!fw_sum8_is_zero((const uint8_t *)h, h->length)) {
        fw_degrade(e, FW_DEGRADED_CHECKSUM_FAIL, "SDT checksum nonzero");
        return;
    }
}

static void fw_validate_acpi_rsdp(struct firmware_table_entry *e)
{
    /* v1 footprint always present; v2 footprint conditional on revision. */
    if (!fw_mmap_contains(e->phys_addr, sizeof(struct fw_rsdp_v1_view))) {
        fw_degrade(e, FW_DEGRADED_RANGE_UNMAPPED, "RSDP v1 span outside firmware mmap");
        return;
    }
    const struct fw_rsdp_v1_view *v1 =
        (const struct fw_rsdp_v1_view *)e->phys_addr;
    if (!fw_sum8_is_zero((const uint8_t *)v1, sizeof(*v1))) {
        fw_degrade(e, FW_DEGRADED_CHECKSUM_FAIL, "RSDP v1 checksum nonzero");
        return;
    }
    if (v1->revision >= 2) {
        if (!fw_mmap_contains(e->phys_addr, sizeof(struct fw_rsdp_v2_view))) {
            fw_degrade(e, FW_DEGRADED_RANGE_UNMAPPED,
                       "RSDP v2 span outside firmware mmap");
            return;
        }
        const struct fw_rsdp_v2_view *v2 =
            (const struct fw_rsdp_v2_view *)e->phys_addr;
        /* ACPI specification 5.2.5.3: extended checksum covers exactly
         * v2->length bytes (must be at least sizeof(v2)).  Reject
         * undersized declarations -- otherwise a 0-length RSDP would
         * trivially pass with sum=0 over zero bytes. */
        if (v2->length < sizeof(*v2)) {
            fw_degrade(e, FW_DEGRADED_LENGTH_BAD, "RSDP v2 length below 36");
            return;
        }
        if (v2->length > FW_RSDP_LENGTH_MAX) {
            fw_degrade(e, FW_DEGRADED_LENGTH_BAD,
                       "RSDP v2 length exceeds defensive cap");
            return;
        }
        if (!fw_mmap_contains(e->phys_addr, v2->length)) {
            fw_degrade(e, FW_DEGRADED_RANGE_UNMAPPED,
                       "RSDP v2 declared length outside firmware mmap");
            return;
        }
        if (!fw_sum8_is_zero((const uint8_t *)v2, v2->length)) {
            fw_degrade(e, FW_DEGRADED_CHECKSUM_FAIL,
                       "RSDP v2 extended checksum nonzero");
            return;
        }
    }
}

static void fw_validate_smbios_ep(struct firmware_table_entry *e, int is_v3)
{
    /* Read just the length byte first to bound the full checksum span. */
    if (!fw_mmap_contains(e->phys_addr, is_v3 ? FW_SMBIOS3_EP_LEN
                                              : FW_SMBIOS2_EP_LEN_MIN)) {
        fw_degrade(e, FW_DEGRADED_RANGE_UNMAPPED,
                   "SMBIOS entry-point header outside firmware mmap");
        return;
    }
    if (is_v3) {
        const struct fw_smbios3_ep_view *ep =
            (const struct fw_smbios3_ep_view *)e->phys_addr;
        if (ep->anchor[0] != '_' || ep->anchor[1] != 'S' ||
            ep->anchor[2] != 'M' || ep->anchor[3] != '3' ||
            ep->anchor[4] != '_') {
            fw_degrade(e, FW_DEGRADED_LENGTH_BAD,
                       "SMBIOS3 anchor mismatch");
            return;
        }
        if (ep->length != FW_SMBIOS3_EP_LEN) {
            fw_degrade(e, FW_DEGRADED_LENGTH_BAD,
                       "SMBIOS3 length not 0x18");
            return;
        }
        if (!fw_sum8_is_zero((const uint8_t *)ep, ep->length)) {
            fw_degrade(e, FW_DEGRADED_CHECKSUM_FAIL,
                       "SMBIOS3 checksum nonzero");
            return;
        }
    } else {
        const struct fw_smbios2_ep_view *ep =
            (const struct fw_smbios2_ep_view *)e->phys_addr;
        if (ep->anchor[0] != '_' || ep->anchor[1] != 'S' ||
            ep->anchor[2] != 'M' || ep->anchor[3] != '_') {
            fw_degrade(e, FW_DEGRADED_LENGTH_BAD,
                       "SMBIOS2 anchor mismatch");
            return;
        }
        if (ep->length < FW_SMBIOS2_EP_LEN_MIN ||
            ep->length > FW_SMBIOS2_EP_LEN_MAX) {
            fw_degrade(e, FW_DEGRADED_LENGTH_BAD,
                       "SMBIOS2 length not in [0x1E, 0x1F]");
            return;
        }
        if (!fw_mmap_contains(e->phys_addr, ep->length)) {
            fw_degrade(e, FW_DEGRADED_RANGE_UNMAPPED,
                       "SMBIOS2 declared length outside firmware mmap");
            return;
        }
        if (!fw_sum8_is_zero((const uint8_t *)ep, ep->length)) {
            fw_degrade(e, FW_DEGRADED_CHECKSUM_FAIL,
                       "SMBIOS2 checksum nonzero");
            return;
        }
    }
}

static void fw_validate_fpdt(struct firmware_table_entry *e)
{
    /* FPDT is an ACPI SDT advertised via UEFI cfg-table on some firmware.
     * size is unknown at catalog time (cfg-table source), so read header
     * length first, then bound the checksum. */
    if (!fw_mmap_contains(e->phys_addr, sizeof(struct fw_sdt_header_view))) {
        fw_degrade(e, FW_DEGRADED_RANGE_UNMAPPED, "FPDT header outside firmware mmap");
        return;
    }
    const struct fw_sdt_header_view *h =
        (const struct fw_sdt_header_view *)e->phys_addr;
    if (h->length < sizeof(*h)) {
        fw_degrade(e, FW_DEGRADED_LENGTH_BAD, "FPDT length below header");
        return;
    }
    if (h->length > FW_FPDT_LENGTH_MAX) {
        fw_degrade(e, FW_DEGRADED_LENGTH_BAD,
                   "FPDT length exceeds defensive cap");
        return;
    }
    if (!fw_mmap_contains(e->phys_addr, h->length)) {
        fw_degrade(e, FW_DEGRADED_RANGE_UNMAPPED,
                   "FPDT declared length outside firmware mmap");
        return;
    }
    if (!fw_sum8_is_zero((const uint8_t *)h, h->length)) {
        fw_degrade(e, FW_DEGRADED_CHECKSUM_FAIL, "FPDT checksum nonzero");
        return;
    }
}

static void fw_validate_esrt(struct firmware_table_entry *e)
{
    if (!fw_mmap_contains(e->phys_addr, sizeof(struct fw_esrt_header_view))) {
        fw_degrade(e, FW_DEGRADED_RANGE_UNMAPPED, "ESRT header outside firmware mmap");
        return;
    }
    const struct fw_esrt_header_view *h =
        (const struct fw_esrt_header_view *)e->phys_addr;
    if (h->fw_resource_count > h->fw_resource_count_max) {
        fw_degrade(e, FW_DEGRADED_LENGTH_BAD,
                   "ESRT count exceeds count_max");
        return;
    }
    /* Header + count*entry must fit; guard the multiplication overflow. */
    uint64_t entries_bytes = (uint64_t)h->fw_resource_count * FW_ESRT_ENTRY_SIZE;
    if (entries_bytes > 0xFFFFFFFFull) {
        fw_degrade(e, FW_DEGRADED_LENGTH_BAD, "ESRT entries overflow u32");
        return;
    }
    uint64_t total = (uint64_t)sizeof(*h) + entries_bytes;
    if (!fw_mmap_contains(e->phys_addr, total)) {
        fw_degrade(e, FW_DEGRADED_RANGE_UNMAPPED,
                   "ESRT entries span outside firmware mmap");
        return;
    }
}

static void fw_validate_smbios_raw(struct firmware_table_entry *e)
{
    /* Catalog already records [phys_addr, size) for the structure-table
     * region.  Re-check it lies entirely in firmware-bearing memory. */
    if (e->size == 0) {
        fw_degrade(e, FW_DEGRADED_LENGTH_BAD, "SMBIOS raw size is zero");
        return;
    }
    if (!fw_mmap_contains(e->phys_addr, e->size)) {
        fw_degrade(e, FW_DEGRADED_RANGE_UNMAPPED,
                   "SMBIOS raw span outside firmware mmap");
        return;
    }
}

/* Dispatch one entry. Status transitions are:
 *   DEGRADED          -- one-way; never re-promoted.
 *   VALIDATED         -- left at VALIDATED on full pass; degraded on failure.
 *   UNKNOWN_PROFILE   -- promoted to VALIDATED when a full format-specific
 *                        validator passes (RSDP / SMBIOS EP / FPDT / ESRT);
 *                        left UNKNOWN when only a base-byte range probe ran
 *                        (unknown GUIDs, DTB until full DTB parsing lands). */
static void fw_validate_entry(struct firmware_table_entry *e)
{
    /* Skip entries that catalog or a prior validator pass already
     * downgraded -- preserve the lower-trust status. */
    if (e->status == FW_STATUS_DEGRADED) return;

    if (e->phys_addr == 0) {
        fw_degrade(e, FW_DEGRADED_NULL_POINTER, "phys_addr is NULL");
        return;
    }

    /* Track whether this entry went through a FULL format-specific
     * validator (so passing it earns FW_STATUS_VALIDATED) or only a
     * minimal base-byte range probe (no promotion -- the caller still
     * does not know the table is well-formed). */
    int full_format_check = 0;

    switch (e->source) {
    case FW_SOURCE_UEFI_CFG_TABLE:
        if (str_eq(e->name, "ACPI2.0") || str_eq(e->name, "ACPI1.0")) {
            fw_validate_acpi_rsdp(e);
            full_format_check = 1;
        } else if (str_eq(e->name, "SMBIOS3")) {
            fw_validate_smbios_ep(e, 1);
            full_format_check = 1;
        } else if (str_eq(e->name, "SMBIOS")) {
            fw_validate_smbios_ep(e, 0);
            full_format_check = 1;
        } else if (str_eq(e->name, "FPDT")) {
            fw_validate_fpdt(e);
            full_format_check = 1;
        } else if (str_eq(e->name, "ESRT")) {
            fw_validate_esrt(e);
            full_format_check = 1;
        } else {
            /* Unknown / no-oracle GUIDs (MAT, RtProps, Conform, DTB,
             * unrecognised vendor GUIDs): range-check the single base
             * byte so we at least catch wildly bad pointers. No format
             * decode happened, so do NOT promote to VALIDATED. */
            if (!fw_mmap_contains(e->phys_addr, 1)) {
                fw_degrade(e, FW_DEGRADED_RANGE_UNMAPPED,
                           "cfg-table base outside firmware mmap");
            }
        }
        break;
    case FW_SOURCE_ACPI_SDT:
        fw_validate_acpi_sdt(e);
        full_format_check = 1;
        break;
    case FW_SOURCE_SMBIOS_RAW:
        fw_validate_smbios_raw(e);
        full_format_check = 1;
        break;
    case FW_SOURCE_FPDT:
        fw_validate_fpdt(e);
        full_format_check = 1;
        break;
    case FW_SOURCE_ESRT:
        fw_validate_esrt(e);
        full_format_check = 1;
        break;
    case FW_SOURCE_DTB:
        /* DTB has its own header magic + size-of-totalsize field; full
         * validation lands with the DTB arbitration work. Range-check
         * the base byte so a wildly bad pointer still degrades here, but
         * do not promote to VALIDATED until full DTB parsing exists. */
        if (!fw_mmap_contains(e->phys_addr, 1)) {
            fw_degrade(e, FW_DEGRADED_RANGE_UNMAPPED,
                       "DTB base outside firmware mmap");
        }
        break;
    default:
        /* Catalog source is policed elsewhere; leave entry untouched. */
        break;
    }

    /* Promote UNKNOWN_PROFILE -> VALIDATED on a clean full-format pass
     * so downstream consumers (ESRT mirror, Registry/JSON publication,
     * sysinfo.exe firmware view) can distinguish "fully checked" from
     * "only cataloged". DEGRADED is preserved by the early-return above
     * already; entries that started as VALIDATED stay VALIDATED. */
    if (full_format_check && e->status == FW_STATUS_UNKNOWN_PROFILE)
        e->status = FW_STATUS_VALIDATED;
}

void firmware_table_validate_all(void)
{
    for (uint32_t i = 0; i < g_count; i++)
        fw_validate_entry(&g_entries[i]);
}

#ifdef KERNEL_TESTS
void firmware_table_validate_one_for_test(struct firmware_table_entry *entry,
                                          int bypass_range_check)
{
    if (!entry) return;
    int prev = g_fw_test_bypass_range;
    g_fw_test_bypass_range = bypass_range_check ? 1 : 0;
    fw_validate_entry(entry);
    g_fw_test_bypass_range = prev;
}
#endif

/* ---- Public init -------------------------------------------------------- */

void firmware_tables_init(void)
{
    if (g_initialized) return;

    POST16(POST16_FW_TABLES);

    catalog_uefi_cfg_tables();
    catalog_acpi_sdts();
    catalog_smbios_raw();

    /* Re-check every cataloged entry against the UEFI memory map and
     * recompute per-source checksums before we publish the boot summary,
     * so the validated/degraded counts reflect the final ground truth. */
    firmware_table_validate_all();

    g_initialized = 1;

    /* Tally for the boot summary. */
    uint32_t validated = 0;
    uint32_t degraded = 0;
    for (uint32_t i = 0; i < g_count; i++) {
        if (g_entries[i].status == FW_STATUS_VALIDATED) validated++;
        else if (g_entries[i].status == FW_STATUS_DEGRADED) degraded++;
    }

    klog(LOG_INFO, "BOOT",
         "firmware tables: %u cataloged, %u validated, %u degraded",
         g_count, validated, degraded);

    POST16(POST16_FW_TABLES_OK);
}

/* ---- Public lookups ----------------------------------------------------- */

uint32_t firmware_table_count(void)
{
    return g_count;
}

const struct firmware_table_entry *firmware_table_get(uint32_t index)
{
    if (index >= g_count) return (const struct firmware_table_entry *)0;
    return &g_entries[index];
}

const struct firmware_table_entry *
firmware_table_lookup_guid(const struct boot_uefi_guid *guid)
{
    if (!guid || guid_is_zero(guid))
        return (const struct firmware_table_entry *)0;
    for (uint32_t i = 0; i < g_count; i++) {
        if (g_entries[i].source != FW_SOURCE_UEFI_CFG_TABLE) continue;
        if (guid_eq(&g_entries[i].guid, guid))
            return &g_entries[i];
    }
    return (const struct firmware_table_entry *)0;
}

const struct firmware_table_entry *
firmware_table_lookup_name(const char *name)
{
    if (!name) return (const struct firmware_table_entry *)0;
    for (uint32_t i = 0; i < g_count; i++) {
        if (str_eq(g_entries[i].name, name))
            return &g_entries[i];
    }
    return (const struct firmware_table_entry *)0;
}

uint32_t firmware_table_lookup_owner(const char *owner,
                                     const struct firmware_table_entry **out,
                                     uint32_t max_out)
{
    if (!owner) return 0;
    if (max_out > 0 && !out) return 0;  /* misuse: array required */
    uint32_t total = 0;
    for (uint32_t i = 0; i < g_count; i++) {
        if (!str_eq(g_entries[i].owner, owner)) continue;
        if (total < max_out)
            out[total] = &g_entries[i];
        total++;
    }
    return total;
}
