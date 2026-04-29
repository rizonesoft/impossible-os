/* ============================================================================
 * firmware_tables.c -- Unified firmware table catalog
 *
 * Aggregates UEFI configuration table entries, ACPI SDTs, the SMBIOS
 * structure-table region, FPDT, ESRT, and DTB into a single read-only
 * catalog queryable by GUID, name, or owner. Per-provider validation is
 * NOT duplicated here -- this code consumes the helpers in acpi.c,
 * smbios.c, and uefi_config.c that already perform checksum / length
 * checks. Range/checksum validation against the UEFI memory map is
 * deferred to the validator that lands in a later TODO-04 section.
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

        /* Status assignment honours the header contract:
         *   VALIDATED       -> per-provider helper succeeded (known GUID)
         *   UNKNOWN_PROFILE -> nonzero pointer, GUID not recognised here
         *   DEGRADED        -> NULL VendorTable pointer (firmware bug)
         * Range checks against the UEFI memory map remain owned by the
         * later TODO-04 validator section. */
        if (src->table_addr == 0) {
            e->status = FW_STATUS_DEGRADED;
            e->degraded_reason = FW_DEGRADED_NULL_POINTER;
        } else if (short_name) {
            e->status = FW_STATUS_VALIDATED;
            e->degraded_reason = FW_DEGRADED_NONE;
        } else {
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

/* ---- Public init -------------------------------------------------------- */

void firmware_tables_init(void)
{
    if (g_initialized) return;

    POST16(POST16_FW_TABLES);

    catalog_uefi_cfg_tables();
    catalog_acpi_sdts();
    catalog_smbios_raw();

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
