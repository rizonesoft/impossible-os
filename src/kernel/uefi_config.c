/* ============================================================================
 * uefi_config.c -- UEFI Configuration Table Walker
 *
 * Walks the UEFI configuration table entries preserved in boot_info to find
 * platform data (ACPI, SMBIOS, Memory Attributes, etc.) by GUID.
 * ============================================================================ */

#include "kernel/uefi_config.h"
#include "kernel/boot_info.h"
#include "kernel/firmware_tables.h"
#include "kernel/firmware_quirks.h"
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

/* EFI_CONFORMANCE_PROFILES_TABLE layout (UEFI 2.10 section 4.6.5).
 * Header is { version u16, profile_count u16 }; profiles follow as a
 * flexible array of EFI_GUID.  ECPT itself does NOT carry a total
 * length, so per-GUID reads must be range-checked against the
 * firmware memory map (firmware_table_mmap_contains) and a defensive
 * count cap (UEFI_CONFORM_PROFILE_MAX) bounds the walk. */
struct uefi_conformance_table {
    uint16_t version;
    uint16_t profile_count;
    struct boot_uefi_guid profiles[];
};

/* Per-profile-id presence flags.  ECPT is an array, not a winner --
 * a system that claims BOTH UEFI Spec AND EBBR has both bits set, and
 * the PC-contradiction detector consults the EBBR bit independently
 * of the display winner. */
static uint8_t s_profile_present[UEFI_PROFILE_ID__COUNT];

/* Internal-only display-rank table.  Higher rank wins for display
 * naming when multiple profiles match; per-policy queries
 * (allows_omit, pc_contradiction) consult presence flags directly. */
struct uefi_conformance_row {
    enum uefi_conformance_profile_id id;
    struct boot_uefi_guid guid;
    const char *name;
    uint8_t allow_omit_pc_tables;
    uint8_t display_rank;  /* internal -- not part of public ABI */
};

static const struct uefi_conformance_row s_conformance_rows[] = {
    { UEFI_PROFILE_ID_UEFI_SPEC, UEFI_PROFILE_UEFI_SPEC, "UEFI Spec", 0, 100 },
    { UEFI_PROFILE_ID_EBBR,      UEFI_PROFILE_EBBR,      "EBBR",      1, 50  },
    /* Reserved-but-not-yet-defined profiles.  Do NOT populate with
     * placeholder GUIDs -- a zero GUID matches a maliciously-zeroed
     * ECPT entry.  Add a real row only when a published spec lands:
     *   - SBBR (Server Base Boot Requirements, Arm)
     *   - ARM BBR (Base Boot Requirements, the umbrella spec)
     *   - Microsoft EBBR variants (rumored, no GUID published) */
};

#define UEFI_CONFORMANCE_ROW_COUNT \
    (sizeof(s_conformance_rows) / sizeof(s_conformance_rows[0]))

static int s_conformance_level = UEFI_CONFORM_FULL;

void uefi_conformance_init(void)
{
    /* Reset presence flags before re-running (idempotent). */
    for (uint32_t i = 0; i < UEFI_PROFILE_ID__COUNT; i++)
        s_profile_present[i] = 0;

    struct boot_uefi_guid conform_guid = UEFI_GUID_CONFORMANCE;
    uintptr_t table_addr = uefi_find_config_table(&conform_guid);

    if (table_addr == 0) {
        /* Table absent -- pre-UEFI 2.10 or firmware that omits it.
         * Assume full UEFI conformance for backward compatibility. */
        s_conformance_level = UEFI_CONFORM_FULL;
        klog(LOG_INFO, "UEFI", "Conformance: Full UEFI (table absent, assumed)");
        return;
    }

    /* Bound the header read.  ECPT header is 4 bytes; check the
     * firmware memory map oracle before dereferencing.  Failure here
     * means firmware lied about the config-table address; treat as
     * UNKNOWN rather than overreading. */
    if (!firmware_table_mmap_contains(table_addr,
                                      sizeof(struct uefi_conformance_table))) {
        klog(LOG_WARN, "UEFI",
             "Conformance: ECPT header outside firmware mmap; rejected");
        s_conformance_level = UEFI_CONFORM_UNKNOWN;
        return;
    }

    const struct uefi_conformance_table *ct =
        (const struct uefi_conformance_table *)table_addr;

    /* UEFI 2.10 section 4.6.5 mandates version == 1.  Any other value
     * is a parser-future-extension hazard; refuse to interpret. */
    if (ct->version != 1) {
        klog(LOG_WARN, "UEFI",
             "Conformance: ECPT version=%u not supported (expected 1)",
             (unsigned)ct->version);
        s_conformance_level = UEFI_CONFORM_UNKNOWN;
        return;
    }

    /* Defensive count cap.  ECPT has no total-length field, so a
     * runaway profile_count could otherwise drive an overread; clamp
     * at UEFI_CONFORM_PROFILE_MAX (16) which is well above any
     * plausible firmware-published list. */
    uint16_t count = ct->profile_count;
    if (count > UEFI_CONFORM_PROFILE_MAX) {
        klog(LOG_WARN, "UEFI",
             "Conformance: ECPT profile_count=%u exceeds cap %u; truncating",
             (unsigned)count, UEFI_CONFORM_PROFILE_MAX);
        count = UEFI_CONFORM_PROFILE_MAX;
    }

    /* Walk profiles, bounding each GUID access against the firmware
     * mmap.  A claim of N profiles whose Nth GUID falls outside any
     * cataloged firmware region rejects that GUID rather than
     * overreading kernel or firmware memory. */
    int rejected_oor = 0;
    int matched_unknown = 0;
    for (uint16_t i = 0; i < count; i++) {
        uintptr_t guid_addr = (uintptr_t)&ct->profiles[i];
        if (!firmware_table_mmap_contains(guid_addr,
                                          sizeof(struct boot_uefi_guid))) {
            rejected_oor++;
            continue;
        }

        int matched = 0;
        for (uint32_t r = 0; r < UEFI_CONFORMANCE_ROW_COUNT; r++) {
            if (guid_equal(&ct->profiles[i], &s_conformance_rows[r].guid)) {
                s_profile_present[s_conformance_rows[r].id] = 1;
                matched = 1;
                break;
            }
        }

        if (!matched) {
            matched_unknown++;
            /* Surface the FULL canonical GUID so an operator hitting
             * a future profile can copy it from serial and add a row
             * to s_conformance_rows[] without round-tripping bytes. */
            const struct boot_uefi_guid *g = &ct->profiles[i];
            klog(LOG_WARN, "UEFI",
                 "Conformance: unknown profile GUID=%08x-%04x-%04x-%02x%02x-"
                 "%02x%02x%02x%02x%02x%02x",
                 (unsigned)g->data1,
                 (unsigned)g->data2,
                 (unsigned)g->data3,
                 (unsigned)g->data4[0], (unsigned)g->data4[1],
                 (unsigned)g->data4[2], (unsigned)g->data4[3],
                 (unsigned)g->data4[4], (unsigned)g->data4[5],
                 (unsigned)g->data4[6], (unsigned)g->data4[7]);
        }
    }

    if (rejected_oor > 0) {
        klog(LOG_WARN, "UEFI",
             "Conformance: %d profile slot(s) outside firmware mmap; rejected",
             rejected_oor);
    }

    /* Backward-compat scalar level: UEFI Spec wins; otherwise EBBR;
     * otherwise UNKNOWN.  Per-profile policy queries below consult
     * s_profile_present[] directly so EBBR coexisting with UEFI Spec
     * still triggers the PC-contradiction detector. */
    if (s_profile_present[UEFI_PROFILE_ID_UEFI_SPEC]) {
        s_conformance_level = UEFI_CONFORM_FULL;
    } else if (s_profile_present[UEFI_PROFILE_ID_EBBR]) {
        s_conformance_level = UEFI_CONFORM_EBBR;
    } else {
        s_conformance_level = UEFI_CONFORM_UNKNOWN;
    }

    klog(LOG_INFO, "UEFI",
         "Conformance: %s (%u profiles, %d unknown)",
         uefi_conformance_name(),
         (unsigned)count, matched_unknown);

    /* PC-contradiction warning: x86_64 host claiming EBBR cannot
     * actually be EBBR-class because PIC / i8042 / RTC port 0x70 are
     * present by the architecture's definition.  Fires independently
     * of the display winner so an EBBR claim alongside UEFI Spec
     * still surfaces. */
    if (uefi_conformance_pc_contradiction()) {
        klog(LOG_WARN, "UEFI",
             "Conformance: EBBR claim with PC-only architecture; "
             "treating as HYBRID (caller policy unchanged)");
    }
}

int uefi_conformance_level(void)
{
    return s_conformance_level;
}

int uefi_conformance_has_profile(enum uefi_conformance_profile_id id)
{
    if ((unsigned)id >= UEFI_PROFILE_ID__COUNT)
        return 0;
    return s_profile_present[id] ? 1 : 0;
}

const char *uefi_conformance_name(void)
{
    /* Both profiles claimed: explicit "+" rendering so operators
     * notice the simultaneous claim.  Single-profile fast path
     * picks the highest display_rank. */
    int has_uefi = s_profile_present[UEFI_PROFILE_ID_UEFI_SPEC];
    int has_ebbr = s_profile_present[UEFI_PROFILE_ID_EBBR];
    if (has_uefi && has_ebbr)
        return "UEFI Spec + EBBR";
    if (has_uefi)
        return "UEFI Spec";
    if (has_ebbr)
        return "EBBR";

    /* Table absent and FULL set by uefi_conformance_init's early
     * return -- distinguish that from "table parsed, no match." */
    if (s_conformance_level == UEFI_CONFORM_FULL)
        return "Full UEFI (assumed)";
    return "unknown";
}

int uefi_conformance_allows_omit_pc_tables(void)
{
    /* EBBR-class profile present -> firmware may omit FPDT/MAT/
     * RTProps.  Default-deny (require by default) when no profile
     * matched: silent omission is the worse failure mode. */
    return s_profile_present[UEFI_PROFILE_ID_EBBR] ? 1 : 0;
}

int uefi_conformance_pc_contradiction(void)
{
    /* PC-only-hardware contradiction is x86-specific.  ARM/RISC-V
     * builds have no PIC / i8042 / RTC port 0x70 to contradict, so
     * an EBBR claim there is consistent.  Compile-gated; runtime
     * value = (x86_64 build) AND (EBBR profile present). */
#ifdef __x86_64__
    return s_profile_present[UEFI_PROFILE_ID_EBBR] ? 1 : 0;
#else
    return 0;
#endif
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
static struct esrt_table_header s_esrt_header;  /* count_max + version mirror */

const char *esrt_decode_type(uint32_t fw_type)
{
    switch (fw_type) {
    case ESRT_FW_TYPE_SYSTEM:       return "System";
    case ESRT_FW_TYPE_DEVICE:       return "Device";
    case ESRT_FW_TYPE_UEFI_DRIVER:  return "Driver";
    default:                        return "Unknown";
    }
}

const char *esrt_decode_status(uint32_t status)
{
    /* UEFI 2.10 Table 23-3 mnemonics, unprefixed.  Codes 0x06/0x07
     * carry the UEFI 2.7+ rename (PWR_EVT_AC / PWR_EVT_BATT) and
     * 0x08 was added in UEFI 2.7.  "Reserved" applies to any value
     * outside 0x00..0x08 so operator UX is honest about future
     * spec extensions. */
    switch (status) {
    case ESRT_STATUS_SUCCESS:                          return "SUCCESS";
    case ESRT_STATUS_ERROR_UNSUCCESSFUL:               return "ERROR_UNSUCCESSFUL";
    case ESRT_STATUS_ERROR_INSUFFICIENT_RESOURCES:     return "ERROR_INSUFFICIENT_RESOURCES";
    case ESRT_STATUS_ERROR_INCORRECT_VERSION:          return "ERROR_INCORRECT_VERSION";
    case ESRT_STATUS_ERROR_INVALID_FORMAT:             return "ERROR_INVALID_FORMAT";
    case ESRT_STATUS_ERROR_AUTH_ERROR:                 return "ERROR_AUTH_ERROR";
    case ESRT_STATUS_ERROR_PWR_EVT_AC:                 return "ERROR_PWR_EVT_AC";
    case ESRT_STATUS_ERROR_PWR_EVT_BATT:               return "ERROR_PWR_EVT_BATT";
    case ESRT_STATUS_ERROR_UNSATISFIED_DEPENDENCIES:   return "ERROR_UNSATISFIED_DEPENDENCIES";
    default:                                           return "Reserved";
    }
}

void esrt_init(void)
{
    s_esrt_count = 0;
    s_esrt_header.fw_resource_count     = 0;
    s_esrt_header.fw_resource_count_max = 0;
    s_esrt_header.fw_resource_version   = 0;

    struct boot_uefi_guid esrt_guid = UEFI_GUID_ESRT;
    uintptr_t table_addr = uefi_find_config_table(&esrt_guid);

    if (table_addr == 0) {
        klog(LOG_INFO, "ESRT", "Not present (no firmware inventory)");
        return;
    }

    /* Validate the header extent against the firmware UEFI memory map
     * BEFORE the first dereference. uefi_find_config_table() returns a
     * firmware-supplied pointer; reading fw_resource_count out of the
     * header before this check is the same trust boundary the MAT and
     * conformance paths close with firmware_table_mmap_contains(). */
    if (!firmware_table_mmap_contains(table_addr,
            sizeof(struct esrt_table_header))) {
        klog(LOG_WARN, "ESRT",
             "header at 0x%lx not in firmware mmap -- table rejected",
             (uint64_t)table_addr);
        return;
    }

    const struct esrt_table_header *hdr =
        (const struct esrt_table_header *)table_addr;

    /* Mirror header metadata so esrt_resource_count_max() / _version()
     * accessors and the Registry _Header subkey can read it without
     * re-touching firmware memory after Phase 1 init. */
    s_esrt_header = *hdr;

    uint32_t count = hdr->fw_resource_count;
    if (count > ESRT_MAX_ENTRIES)
        count = ESRT_MAX_ENTRIES;

    if (count == 0) {
        klog(LOG_INFO, "ESRT", "Present but empty (0 entries)");
        return;
    }

    /* Validate the full table extent (header + count entries) against the
     * firmware memory map before dereferencing any entry. count is clamped
     * to ESRT_MAX_ENTRIES above so the multiply cannot wrap. */
    {
        uint64_t total = (uint64_t)sizeof(struct esrt_table_header)
                       + (uint64_t)count * (uint64_t)sizeof(struct esrt_entry);
        if (!firmware_table_mmap_contains(table_addr, total)) {
            klog(LOG_WARN, "ESRT",
                 "extent [0x%lx +%lu] not in firmware mmap -- table rejected",
                 (uint64_t)table_addr, (uint64_t)total);
            return;
        }
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
        klog(LOG_INFO, "ESRT", "  [%u] %s v%u.%u (min v%u.%u, status=%s)",
             i, esrt_decode_type(e->fw_type),
             e->fw_version >> 16, e->fw_version & 0xFFFF,
             e->lowest_supported_version >> 16,
             e->lowest_supported_version & 0xFFFF,
             esrt_decode_status(e->last_attempt_status));
    }
}

uint32_t esrt_resource_count_max(void)
{
    return s_esrt_header.fw_resource_count_max;
}

uint64_t esrt_resource_version(void)
{
    return s_esrt_header.fw_resource_version;
}

int esrt_rollback_floor_ok(uint32_t idx)
{
    if (idx >= s_esrt_count)
        return 0;
    const struct esrt_entry *e = &s_esrt_entries[idx];
    return e->fw_version >= e->lowest_supported_version;
}

int esrt_capsule_persists_across_reset(uint32_t idx)
{
    if (idx >= s_esrt_count)
        return 0;
    const struct esrt_entry *e = &s_esrt_entries[idx];
    return (e->capsule_flags & EFI_CAPSULE_PERSIST_ACROSS_RESET) != 0;
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

/* ============================================================================
 * Memory Attributes Table (UEFI 2.6+ §4.6.4)
 *
 * Declares fine-grained memory permissions (RO, XP, RP) for runtime regions.
 * Enforces W^X: no region may be simultaneously writable AND executable.
 * ============================================================================ */

/* MAT memory descriptor -- matches EFI_MEMORY_DESCRIPTOR layout */
struct mat_descriptor {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
};

static int s_mat_present;
static int s_wxn_ok;  /* 1 = all regions pass W^X check */

/* MAT region inventory cache (consumer API for TODO-27 W^X enforcement). */
static mat_entry_t s_mat_entries[MAT_MAX_ENTRIES];
static uint32_t    s_mat_count;
static int         s_mat_overflowed;
static uint64_t    s_mat_code_pages;
static uint64_t    s_mat_data_pages;
static uint64_t    s_mat_guard_pages;

/* Per-entry log cap: don't spam serial with hundreds of lines on
 * vendor firmware that exposes a large MAT. The aggregate summary
 * still reflects every entry; only individual lines are clipped. */
#define MAT_PER_ENTRY_LOG_CAP 32
#define MAT_VIOLATION_LOG_CAP 8u

/* Sane upper bound on per-descriptor stride. EFI_MEMORY_DESCRIPTOR is
 * currently 48 bytes (UEFI 2.10); 256 leaves headroom for vendor
 * extensions while rejecting 0xFFFFFFFF and other obviously-corrupt
 * stride values. */
#define MAT_MAX_DESC_SIZE    256

static mat_class_t mat_classify(uint64_t attr, int *is_violation_out)
{
    int is_ro = (attr & EFI_MEMORY_RO) != 0;
    int is_xp = (attr & EFI_MEMORY_XP) != 0;
    int is_rp = (attr & EFI_MEMORY_RP) != 0;

    if (is_violation_out) *is_violation_out = 0;

    if (is_rp) return MAT_CLASS_GUARD;
    if (is_ro && !is_xp) return MAT_CLASS_CODE;
    if (!is_ro && is_xp) return MAT_CLASS_DATA;
    if (is_ro && is_xp) return MAT_CLASS_RODATA;
    /* Writable AND executable: W^X violation. */
    if (is_violation_out) *is_violation_out = 1;
    return MAT_CLASS_WX_VIOLATION;
}

static const char *mat_attr_label(uint64_t attr)
{
    int is_ro = (attr & EFI_MEMORY_RO) != 0;
    int is_xp = (attr & EFI_MEMORY_XP) != 0;
    int is_rp = (attr & EFI_MEMORY_RP) != 0;

    if (is_rp) return "RP";
    if (is_ro && !is_xp) return "RX";
    if (!is_ro && is_xp) return "RW";
    if (is_ro && is_xp) return "RO";
    return "WX!";  /* Writable AND executable: W^X violation. */
}

void mat_init(void)
{
    s_mat_present = 0;
    s_wxn_ok = 0;
    s_mat_count = 0;
    s_mat_overflowed = 0;
    s_mat_code_pages = 0;
    s_mat_data_pages = 0;
    s_mat_guard_pages = 0;

    struct boot_uefi_guid mat_guid = UEFI_GUID_MEM_ATTR;
    uintptr_t table_addr = uefi_find_config_table(&mat_guid);

    if (table_addr == 0) {
        klog(LOG_INFO, "UEFI", "MAT: Not present "
             "(runtime memory W^X not declared)");
        return;
    }

    /* Validate the header extent against the firmware UEFI memory map
     * BEFORE the first dereference. The config-table pointer is
     * firmware-supplied untrusted input; reading version/count/desc_sz
     * before this check is exactly the trust-boundary the catalog's
     * fw_mmap_contains primitive exists to close. */
    if (!firmware_table_mmap_contains((uintptr_t)table_addr,
            sizeof(struct efi_memory_attributes_table))) {
        klog(LOG_WARN, "UEFI",
             "MAT: header at 0x%lx not in firmware mmap -- table rejected",
             (uint64_t)table_addr);
        return;
    }

    const struct efi_memory_attributes_table *mat =
        (const struct efi_memory_attributes_table *)table_addr;

    if (mat->version < 1) {
        klog(LOG_WARN, "UEFI", "MAT: Unknown version %u", mat->version);
        return;
    }

    uint32_t count = mat->number_of_entries;
    uint32_t desc_sz = mat->descriptor_size;

    /* Reject malformed descriptor stride: anything smaller than the
     * spec-mandated record (0, 1, ...) means firmware allocated less
     * memory per entry than we read, so silently rounding up would
     * overread adjacent firmware memory and feed bogus inventory to
     * TODO-27 W^X enforcement. Stride must also be 8-byte-aligned
     * because every field in the descriptor is 8-byte. */
    if (desc_sz < sizeof(struct mat_descriptor) || (desc_sz & 0x7) != 0) {
        klog(LOG_WARN, "UEFI",
             "MAT: descriptor_size=%u below required %u or unaligned -- "
             "table rejected", desc_sz,
             (uint32_t)sizeof(struct mat_descriptor));
        return;
    }

    /* Fail closed on malformed firmware metadata.  Either bound also
     * prevents the count*desc_sz multiply below from approaching uint64
     * overflow on hostile input. */
    if (desc_sz > MAT_MAX_DESC_SIZE) {
        klog(LOG_WARN, "UEFI",
             "MAT: descriptor_size=%u exceeds sanity cap %u -- table rejected",
             desc_sz, (uint32_t)MAT_MAX_DESC_SIZE);
        s_mat_present = 0;
        return;
    }
    /* Cache cap is also the parser cap: a count exceeding the cache
     * cap means TODO-27 W^X enforcement (which iterates the cache)
     * would miss every entry beyond MAT_MAX_ENTRIES. Publishing a
     * truncated inventory is worse than failing closed -- enforcement
     * decisions on unseen regions are silent vulnerabilities. Reject
     * the whole table; if real hardware ever exceeds the cap, the
     * fix is to bump MAT_MAX_ENTRIES, not to ship partial data. */
    /* A zero-entry MAT must NOT be accepted as W^X verified: the empty
     * walk would leave wxn_violations==0 and set s_wxn_ok=1, telling
     * TODO-27 W^X enforcement that empty inventory is fine. Treat
     * count==0 the same as "MAT absent" so consumers cannot mistake
     * a degenerate table for verified runtime memory. */
    if (count == 0) {
        klog(LOG_WARN, "UEFI",
             "MAT: number_of_entries=0 -- table rejected (no W^X coverage)");
        return;
    }

    if (count > MAT_MAX_ENTRIES) {
        klog(LOG_WARN, "UEFI",
             "MAT: number_of_entries=%u exceeds cache cap %u -- "
             "table rejected (would publish a truncated inventory)",
             count, (uint32_t)MAT_MAX_ENTRIES);
        s_mat_present = 0;
        /* Set the public overflow signal so consumers can distinguish
         * "no MAT available" from "MAT was too large to cache safely":
         * mat_get_count() == 0 + mat_overflowed() == 1 means firmware
         * advertised more descriptors than the cache cap. */
        s_mat_overflowed = 1;
        return;
    }

    /* Validate the full table extent (header + count*desc_sz) against
     * the firmware-bearing UEFI memory map BEFORE dereferencing any
     * descriptor. The firmware-table catalog already validates
     * pointers it tracks; the Memory Attributes Table does not yet
     * have a per-provider catalog entry, so do the equivalent
     * containment check inline here. count + desc_sz are bounded so
     * the multiply cannot wrap. */
    {
        uint64_t hdr_size  = (uint64_t)sizeof(struct efi_memory_attributes_table);
        uint64_t body_size = (uint64_t)count * (uint64_t)desc_sz;
        uint64_t total     = hdr_size + body_size;
        if (!firmware_table_mmap_contains((uintptr_t)table_addr, total)) {
            klog(LOG_WARN, "UEFI",
                 "MAT: extent [0x%lx +%lu] not in firmware mmap -- "
                 "table rejected", (uint64_t)table_addr, (uint64_t)total);
            s_mat_present = 0;
            return;
        }
    }

    /* All metadata validated -- now safe to dereference descriptors. */
    s_mat_present = 1;

    /* Walk descriptors, verify W^X, classify regions. Cache up to
     * MAT_MAX_ENTRIES for the consumer iteration API; aggregate
     * counts include every descriptor regardless of cache cap. */
    uint32_t code_regions = 0;
    uint32_t data_regions = 0;
    uint32_t rodata_regions = 0;
    uint32_t guard_regions = 0;
    uint32_t wxn_violations = 0;

    const uint8_t *base = (const uint8_t *)(table_addr +
        sizeof(struct efi_memory_attributes_table));

    /* Page-count cap: number_of_pages * 4096 must fit in uint64.
     * UEFI uses 4 KiB pages so a single descriptor cannot legally
     * exceed (UINT64_MAX / 4096) pages without wrapping the byte
     * size that downstream consumers (TODO-27 W^X enforcement)
     * compute from it. */
    const uint64_t MAT_PAGE_SIZE = 4096ULL;
    const uint64_t MAT_MAX_PAGES_PER_DESC = ~(uint64_t)0 / MAT_PAGE_SIZE;

    uint32_t i;
    uint32_t logged = 0;
    int range_rejection = 0;
    for (i = 0; i < count; i++) {
        /* Pointer-arithmetic overflow guard: with the count + desc_sz
         * caps above this can only fire on a future widening, but the
         * extra check is free and keeps the parser fail-closed. */
        uint64_t offset = (uint64_t)i * desc_sz;
        if (offset / desc_sz != i) {
            klog(LOG_WARN, "UEFI",
                 "MAT: offset overflow at i=%u desc_sz=%u -- truncating walk",
                 i, desc_sz);
            count = i;
            break;
        }
        const struct mat_descriptor *d =
            (const struct mat_descriptor *)(base + offset);

        uint64_t attr = d->attribute;
        uint64_t pages = d->number_of_pages;
        uint64_t pstart = d->physical_start;

        /* Per-descriptor range validation.  Reject entries where
         * pages*4096 wraps uint64 or where pstart + bytes wraps. A
         * consumer using these to compute an end address would either
         * underprotect a wrapped range or panic on a bogus address. */
        if (pages > MAT_MAX_PAGES_PER_DESC) {
            klog(LOG_WARN, "UEFI",
                 "MAT[%u]: number_of_pages=%lu wraps byte size -- table rejected",
                 i, (uint64_t)pages);
            range_rejection = 1;
            break;
        }
        uint64_t bytes = pages * MAT_PAGE_SIZE;
        if (pstart + bytes < pstart) {
            klog(LOG_WARN, "UEFI",
                 "MAT[%u]: phys=0x%lx + size=0x%lx wraps -- table rejected",
                 i, (uint64_t)pstart, (uint64_t)bytes);
            range_rejection = 1;
            break;
        }

        int violation = 0;
        mat_class_t cls = mat_classify(attr, &violation);

        /* Aggregate-counter saturation guard. Each descriptor's pages
         * are already bounded above; the sum across N <= 128
         * descriptors cannot legitimately exceed UINT64_MAX, but a
         * checked add costs nothing and keeps diagnostics honest. */
#define MAT_AGG_ADD(slot, p) do { \
    if ((slot) + (p) < (slot)) { \
        klog(LOG_WARN, "UEFI", \
             "MAT[%u]: aggregate page counter overflow -- table rejected", i); \
        range_rejection = 1; \
        goto mat_walk_done; \
    } \
    (slot) += (p); \
} while (0)

        switch (cls) {
        case MAT_CLASS_GUARD:
            guard_regions++;
            MAT_AGG_ADD(s_mat_guard_pages, pages);
            break;
        case MAT_CLASS_CODE:
            code_regions++;
            MAT_AGG_ADD(s_mat_code_pages, pages);
            break;
        case MAT_CLASS_DATA:
            data_regions++;
            MAT_AGG_ADD(s_mat_data_pages, pages);
            break;
        case MAT_CLASS_RODATA:
            rodata_regions++;
            MAT_AGG_ADD(s_mat_data_pages, pages);
            break;
        case MAT_CLASS_WX_VIOLATION:
            wxn_violations++;
            break;
        }
#undef MAT_AGG_ADD
        (void)violation;
        (void)pstart;
        (void)bytes;

        if (s_mat_count < MAT_MAX_ENTRIES) {
            s_mat_entries[s_mat_count].phys_addr = d->physical_start;
            s_mat_entries[s_mat_count].num_pages = d->number_of_pages;
            s_mat_entries[s_mat_count].attribute = attr;
            s_mat_entries[s_mat_count].cls       = cls;
            s_mat_count++;
        } else {
            s_mat_overflowed = 1;
        }

        if (logged < MAT_PER_ENTRY_LOG_CAP) {
            klog(LOG_INFO, "UEFI",
                 "MAT[%u]: phys=0x%lx pages=%lu attr=%s",
                 i, (uint64_t)d->physical_start,
                 (uint64_t)d->number_of_pages, mat_attr_label(attr));
            logged++;
        }
    }

mat_walk_done:
    if (range_rejection) {
        /* Reset every cache + aggregate so consumers see "MAT absent"
         * rather than a partially-filled inventory derived from a
         * descriptor stream we stopped trusting mid-walk. */
        s_mat_present = 0;
        s_mat_count = 0;
        s_mat_overflowed = 0;
        s_mat_code_pages = 0;
        s_mat_data_pages = 0;
        s_mat_guard_pages = 0;
        return;
    }

    if (count > logged) {
        klog(LOG_INFO, "UEFI", "MAT: ... +%u more entries (per-entry log capped)",
             count - logged);
    }

    s_wxn_ok = (wxn_violations == 0) ? 1 : 0;

    klog(LOG_INFO, "UEFI",
         "MAT: %u descriptors -- %u code (%u KB), %u data (%u KB), "
         "%u rodata, %u guard",
         count, code_regions, (uint32_t)(s_mat_code_pages * 4),
         data_regions, (uint32_t)(s_mat_data_pages * 4),
         rodata_regions, guard_regions);

    if (s_mat_overflowed) {
        klog(LOG_WARN, "UEFI",
             "MAT: %u entries exceeded cache cap of %u -- "
             "TODO-27 enforcement will only see first %u",
             count, (uint32_t)MAT_MAX_ENTRIES, (uint32_t)MAT_MAX_ENTRIES);
    }

    if (s_wxn_ok) {
        klog(LOG_INFO, "UEFI", "MAT: W^X verified -- "
             "no writable+executable regions");
    } else {
        /* Quirk-aware severity: known-bad firmware demotes to INFO.
         * Bare-metal Coreboot/Tianocore builds with bogus MAT layouts
         * are listed in firmware_quirks_table.inc. */
        int known_bad = firmware_quirks_is_active(FW_QUIRK_BOGUS_MAT);
        const char *prefix = known_bad ? "[known-bad firmware] " : "";

        if (known_bad)
            klog(LOG_INFO, "UEFI",
                 "%sMAT: W^X count -- %u regions writable+executable",
                 prefix, wxn_violations);
        else
            klog(LOG_WARN, "UEFI",
                 "MAT: W^X VIOLATION -- %u regions are writable+executable",
                 wxn_violations);

        /* Per-violation root-cause attribution: emit phys/pages/attr +
         * RO/XP/RP decode for each violating descriptor, capped at
         * MAT_VIOLATION_LOG_CAP to bound serial floods on broken
         * firmware. boot-health.json mat_wx_violations[] still emits
         * every entry; this WARN block is for serial-log triage. */
        uint32_t logged_v = 0;
        for (uint32_t i = 0; i < s_mat_count
                              && logged_v < MAT_VIOLATION_LOG_CAP; i++) {
            if (s_mat_entries[i].cls != MAT_CLASS_WX_VIOLATION)
                continue;
            uint64_t a = s_mat_entries[i].attribute;
            int ro = (a & EFI_MEMORY_RO) != 0;
            int xp = (a & EFI_MEMORY_XP) != 0;
            int rp = (a & EFI_MEMORY_RP) != 0;
            klog(known_bad ? LOG_INFO : LOG_WARN, "UEFI",
                 "%sMAT[%u] WX VIOLATION at phys=0x%lx pages=%lu "
                 "attr=0x%lx (RO=%d XP=%d RP=%d)",
                 prefix, i, (uint64_t)s_mat_entries[i].phys_addr,
                 (uint64_t)s_mat_entries[i].num_pages,
                 (uint64_t)a, (uint64_t)ro, (uint64_t)xp, (uint64_t)rp);
            logged_v++;
        }
        if (wxn_violations > logged_v) {
            klog(known_bad ? LOG_INFO : LOG_WARN, "UEFI",
                 "%sMAT: ... +%u more violations (per-violation log capped)",
                 prefix, (uint64_t)(wxn_violations - logged_v));
        }
    }
}

int mat_wxn_enforced(void)
{
    return s_mat_present && s_wxn_ok;
}

uint32_t mat_get_count(void)
{
    return s_mat_count;
}

int mat_get_entry(uint32_t idx, mat_entry_t *out)
{
    if (!out || idx >= s_mat_count) return 0;
    *out = s_mat_entries[idx];
    return 1;
}

mat_class_t mat_classify_attr(uint64_t attr)
{
    return mat_classify(attr, 0);
}

uint64_t mat_get_code_pages(void)  { return s_mat_code_pages; }
uint64_t mat_get_data_pages(void)  { return s_mat_data_pages; }
uint64_t mat_get_guard_pages(void) { return s_mat_guard_pages; }
int      mat_overflowed(void)      { return s_mat_overflowed; }
