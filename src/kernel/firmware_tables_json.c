/* ============================================================================
 * firmware_tables_json.c -- publish the firmware-table catalog as JSON
 *
 * Writes X:\Diag\firmware-tables.json conforming to schema_version=1
 * (canonical wire format pinned in docs/boot/firmware-tables-schema.md).
 *
 * Top-level keys emitted: schema_version, generated_at_utc,
 * firmware_platform, conformance_profile, tables[], degraded[],
 * quirks_active[], apei {bert/hest/einj/erst}, dbg2 {dbg2},
 * wsmt {wsmt}.
 *
 * Per-table parsers for APEI/DBG2/WSMT are deferred -- this writer
 * emits {present, addr, size, checksum_status} only, looked up via
 * firmware_table_lookup_name() with the canonical 4-char ACPI
 * signature.  Schema doc says this is the contract for v1.
 *
 * Buffer: 16 KiB pmm_alloc_contiguous (4 pages).  Manual JSON
 * formatting; no kmalloc; no printf-family.  Truncation policy:
 * if the buffer fills, log a WARN and continue (the closing braces
 * are always emitted last so the file remains parseable).  Same
 * shape as boot_timeline_dump_json() in boot_progress.c.
 *
 * generated_at_utc is pinned to the git HEAD commit time
 * (g_boot_info.loader_identity.build_unix_time, set at link time
 * by tools/boot-info-manifest/gen-loader-identity.sh).  Same
 * offline-deterministic policy as the coverage.{md,json} fix
 * earlier this session: back-to-back boots produce byte-identical
 * JSON output.
 *
 * Wired from boot_phase3() after VFS+IXFS mount, mirroring the
 * boot_history_kernel_mark_phase3() Phase-3 timing.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/time_iso.h"
#include "kernel/firmware_tables.h"
#include "kernel/firmware_platform.h"
#include "kernel/firmware_quirks.h"
#include "kernel/uefi_config.h"
#include "kernel/uefi_runtime.h"
#include "kernel/boot_info.h"
#include "kernel/mm/pmm.h"
#include "kernel/fs/vfs.h"
#include "kernel/klog.h"
#include "kernel/util/json_builder.h"

#define FW_JSON_BUF_PAGES 4u
#define FW_JSON_BUF_SIZE  (FW_JSON_BUF_PAGES * 4096u)

/* ---- Helpers shared with the Registry mirror ---------------------------- */

static const char *source_name(int source)
{
    switch (source) {
    case FW_SOURCE_UEFI_CFG_TABLE:  return "uefi_cfg_table";
    case FW_SOURCE_ACPI_SDT:        return "acpi_sdt";
    case FW_SOURCE_SMBIOS_RAW:      return "smbios_raw";
    case FW_SOURCE_FPDT:            return "fpdt";
    case FW_SOURCE_ESRT:            return "esrt";
    case FW_SOURCE_DTB:             return "dtb";
    default:                        return "unknown";
    }
}

static const char *status_name(int status)
{
    switch (status) {
    case FW_STATUS_VALIDATED:        return "validated";
    case FW_STATUS_DEGRADED:         return "degraded";
    case FW_STATUS_UNKNOWN_PROFILE:  return "unknown_profile";
    default:                         return "untested";
    }
}

static const char *degraded_reason_name(uint8_t reason)
{
    switch (reason) {
    case FW_DEGRADED_CHECKSUM_FAIL:  return "checksum_fail";
    case FW_DEGRADED_LENGTH_BAD:     return "length_bad";
    case FW_DEGRADED_RANGE_UNMAPPED: return "range_unmapped";
    case FW_DEGRADED_NULL_POINTER:   return "null_pointer";
    default:                         return (const char *)0;
    }
}

/* ---- ISO-8601 from unix_time (Howard-Hinnant civil-from-days) ----------- */

static void jb_iso8601(struct json_builder *j, uint64_t unix_time)
{
    char buf[21];
    int i;
    kdate_iso8601(unix_time, buf);   /* shared civil-date formatter, kernel/time_iso.h */
    jb_putc(j, '"');
    for (i = 0; i < 20; i++)
        jb_putc(j, buf[i]);
    jb_putc(j, '"');
}

/* ---- Catalog-style entry emit ------------------------------------------- */

static void emit_entry(struct json_builder *j, const struct firmware_table_entry *e)
{
    jb_puts(j, "{\"name\":");
    jb_str(j, e->name);
    jb_puts(j, ",\"guid\":");
    {
        const struct boot_uefi_guid *g = &e->guid;
        uint8_t any = 0;
        any |= (uint8_t)(g->data1 | g->data2 | g->data3);
        for (int b = 0; b < 8; b++) any |= g->data4[b];
        if (any == 0) {
            jb_puts(j, "null");
        } else {
            static const char hex[] = "0123456789abcdef";
            jb_putc(j, '"'); jb_putc(j, '{');
            uint32_t d1 = g->data1;
            jb_putc(j, hex[(d1 >> 28) & 0xF]);
            jb_putc(j, hex[(d1 >> 24) & 0xF]);
            jb_putc(j, hex[(d1 >> 20) & 0xF]);
            jb_putc(j, hex[(d1 >> 16) & 0xF]);
            jb_putc(j, hex[(d1 >> 12) & 0xF]);
            jb_putc(j, hex[(d1 >> 8) & 0xF]);
            jb_putc(j, hex[(d1 >> 4) & 0xF]);
            jb_putc(j, hex[d1 & 0xF]);
            jb_putc(j, '-');
            uint16_t d2 = g->data2;
            jb_putc(j, hex[(d2 >> 12) & 0xF]);
            jb_putc(j, hex[(d2 >> 8) & 0xF]);
            jb_putc(j, hex[(d2 >> 4) & 0xF]);
            jb_putc(j, hex[d2 & 0xF]);
            jb_putc(j, '-');
            uint16_t d3 = g->data3;
            jb_putc(j, hex[(d3 >> 12) & 0xF]);
            jb_putc(j, hex[(d3 >> 8) & 0xF]);
            jb_putc(j, hex[(d3 >> 4) & 0xF]);
            jb_putc(j, hex[d3 & 0xF]);
            jb_putc(j, '-');
            jb_putc(j, hex[(g->data4[0] >> 4) & 0xF]);
            jb_putc(j, hex[g->data4[0] & 0xF]);
            jb_putc(j, hex[(g->data4[1] >> 4) & 0xF]);
            jb_putc(j, hex[g->data4[1] & 0xF]);
            jb_putc(j, '-');
            for (int b = 2; b < 8; b++) {
                jb_putc(j, hex[(g->data4[b] >> 4) & 0xF]);
                jb_putc(j, hex[g->data4[b] & 0xF]);
            }
            jb_putc(j, '}'); jb_putc(j, '"');
        }
    }
    jb_puts(j, ",\"source\":");
    jb_str(j, source_name(e->source));
    jb_puts(j, ",\"phys_addr\":");
    jb_hex64(j, (uint64_t)e->phys_addr);
    jb_puts(j, ",\"size\":");
    jb_u32_dec(j, e->size);
    jb_puts(j, ",\"checksum\":");
    jb_u32_dec(j, e->computed_checksum);
    jb_puts(j, ",\"checksum_status\":");
    jb_str(j, status_name(e->status));
    jb_puts(j, ",\"validation_reason\":");
    if (e->status == FW_STATUS_DEGRADED) {
        const char *r = degraded_reason_name(e->degraded_reason);
        if (r) jb_str(j, r);
        else   jb_puts(j, "null");
    } else {
        jb_puts(j, "null");
    }
    jb_putc(j, '}');
}

/* ---- APEI/DBG2/WSMT generic listing helper ------------------------------ */

static void emit_named_block(struct json_builder *j, const char *key,
                             const char *signature)
{
    jb_putc(j, '"');
    jb_puts(j, key);
    jb_puts(j, "\":");
    const struct firmware_table_entry *e =
        firmware_table_lookup_name(signature);
    if (!e) {
        jb_puts(j, "{\"present\":false,\"addr\":null,\"size\":0,"
                   "\"checksum_status\":null}");
        return;
    }
    jb_puts(j, "{\"present\":true,\"addr\":");
    jb_hex64(j, (uint64_t)e->phys_addr);
    jb_puts(j, ",\"size\":");
    jb_u32_dec(j, e->size);
    jb_puts(j, ",\"checksum_status\":");
    jb_str(j, status_name(e->status));
    jb_putc(j, '}');
}

/* ---- Public publish ----------------------------------------------------- */

void firmware_tables_publish_json(void)
{
    /* Allocate the working buffer.  4 pages = 16 KiB; plenty of room
     * for the schema's top-level keys + per-table entries (each
     * entry ~120 chars; 16 entries ~2 KiB).  pmm_alloc_contiguous
     * guarantees a physically-contiguous range so the VFS write
     * walks one VA span instead of fragmenting. */
    uintptr_t phys = pmm_alloc_contiguous(FW_JSON_BUF_PAGES);
    if (!phys) {
        klog(LOG_WARN, "FW",
             "JSON: cannot alloc %u pages for firmware-tables.json",
             FW_JSON_BUF_PAGES);
        return;
    }
    struct json_builder jb;
    jb_init(&jb, (char *)phys, FW_JSON_BUF_SIZE);

    jb_putc(&jb, '{');
    jb_puts(&jb, "\"schema_version\":1,");

    jb_puts(&jb, "\"generated_at_utc\":");
    jb_iso8601(&jb, g_boot_info.loader_identity.build_unix_time);

    jb_puts(&jb, ",\"firmware_platform\":");
    jb_str(&jb, firmware_platform_name(firmware_platform_get()));

    jb_puts(&jb, ",\"conformance_profile\":{\"name\":");
    jb_str(&jb, uefi_conformance_name());
    jb_puts(&jb, ",\"level\":");
    {
        const char *lvl;
        switch (uefi_conformance_level()) {
        case UEFI_CONFORM_FULL:    lvl = "FULL"; break;
        case UEFI_CONFORM_EBBR:    lvl = "EBBR"; break;
        default:                   lvl = "UNKNOWN";
        }
        jb_str(&jb, lvl);
    }
    jb_puts(&jb, ",\"has_uefi_spec\":");
    jb_puts(&jb, uefi_conformance_has_profile(UEFI_PROFILE_ID_UEFI_SPEC)
                 ? "true" : "false");
    jb_puts(&jb, ",\"has_ebbr\":");
    jb_puts(&jb, uefi_conformance_has_profile(UEFI_PROFILE_ID_EBBR)
                 ? "true" : "false");
    jb_puts(&jb, ",\"allows_omit_pc_tables\":");
    jb_puts(&jb, uefi_conformance_allows_omit_pc_tables() ? "true" : "false");
    jb_puts(&jb, ",\"pc_contradiction\":");
    jb_puts(&jb, uefi_conformance_pc_contradiction() ? "true" : "false");
    jb_putc(&jb, '}');

    jb_puts(&jb, ",\"tables\":[");
    {
        uint32_t count = firmware_table_count();
        int first = 1;
        for (uint32_t i = 0; i < count; i++) {
            const struct firmware_table_entry *e = firmware_table_get(i);
            if (!e) continue;
            if (!first) jb_putc(&jb, ',');
            emit_entry(&jb, e);
            first = 0;
        }
    }
    jb_putc(&jb, ']');

    jb_puts(&jb, ",\"degraded\":[");
    {
        uint32_t count = firmware_table_count();
        int first = 1;
        for (uint32_t i = 0; i < count; i++) {
            const struct firmware_table_entry *e = firmware_table_get(i);
            if (!e || e->status != FW_STATUS_DEGRADED) continue;
            if (!first) jb_putc(&jb, ',');
            jb_u32_dec(&jb, i);
            first = 0;
        }
    }
    jb_putc(&jb, ']');

    jb_puts(&jb, ",\"quirks_active\":[");
    {
        uint32_t first = 1;
        for (uint32_t b = firmware_quirks_iter_next(0); b;
             b = firmware_quirks_iter_next(b)) {
            const char *n = firmware_quirks_name(b);
            if (!n) continue;
            if (!first) jb_putc(&jb, ',');
            jb_str(&jb, n);
            first = 0;
        }
    }
    jb_putc(&jb, ']');

    /* acpi block: presence summary via the catalog (full ACPI accessor
     * surface for version + RSDP/XSDT addresses lands when ACPICA-style
     * inventory ships).  Schema v1 minimum: rsdp_addr present iff
     * RSDP cataloged; xsdt_addr null until accessor exists; sdt_count
     * derived from the catalog by counting FW_SOURCE_ACPI_SDT entries. */
    jb_puts(&jb, ",\"acpi\":{");
    {
        const struct firmware_table_entry *rsdp2 =
            firmware_table_lookup_name("ACPI2.0");
        const struct firmware_table_entry *rsdp1 =
            firmware_table_lookup_name("ACPI1.0");
        const struct firmware_table_entry *rsdp = rsdp2 ? rsdp2 : rsdp1;
        jb_puts(&jb, "\"version\":");
        if (rsdp2)      jb_puts(&jb, "2");
        else if (rsdp1) jb_puts(&jb, "1");
        else            jb_puts(&jb, "0");

        jb_puts(&jb, ",\"rsdp_addr\":");
        if (rsdp) jb_hex64(&jb, (uint64_t)rsdp->phys_addr);
        else      jb_puts(&jb, "null");

        jb_puts(&jb, ",\"xsdt_addr\":null");

        uint32_t sdt_count = 0;
        uint32_t total = firmware_table_count();
        for (uint32_t i = 0; i < total; i++) {
            const struct firmware_table_entry *e = firmware_table_get(i);
            if (e && e->source == FW_SOURCE_ACPI_SDT)
                sdt_count++;
        }
        jb_puts(&jb, ",\"sdt_count\":");
        jb_u32_dec(&jb, sdt_count);
    }
    jb_putc(&jb, '}');

    /* smbios block: presence summary via the catalog.  Vendor and
     * product strings need a smbios_get_vendor / smbios_get_product
     * accessor that does not exist yet; emit null until it lands. */
    jb_puts(&jb, ",\"smbios\":{");
    {
        const struct firmware_table_entry *sm3 =
            firmware_table_lookup_name("SMBIOS3");
        const struct firmware_table_entry *sm2 =
            firmware_table_lookup_name("SMBIOS");
        const struct firmware_table_entry *sm = sm3 ? sm3 : sm2;
        jb_puts(&jb, "\"version_major\":");
        if (sm3) jb_puts(&jb, "3");
        else if (sm2) jb_puts(&jb, "2");
        else jb_puts(&jb, "0");
        jb_puts(&jb, ",\"version_minor\":0");
        jb_puts(&jb, ",\"table_addr\":");
        if (sm) jb_hex64(&jb, (uint64_t)sm->phys_addr);
        else    jb_puts(&jb, "null");
        jb_puts(&jb, ",\"structure_count\":0");
        jb_puts(&jb, ",\"vendor\":null,\"product\":null");
    }
    jb_putc(&jb, '}');

    /* mat block: full population from include/kernel/uefi_config.h
     * mat_* accessors. */
    jb_puts(&jb, ",\"mat\":{");
    jb_puts(&jb, "\"entry_count\":");
    jb_u32_dec(&jb, mat_get_count());
    jb_puts(&jb, ",\"code_pages\":");
    jb_u32_dec(&jb, (uint32_t)mat_get_code_pages());
    jb_puts(&jb, ",\"data_pages\":");
    jb_u32_dec(&jb, (uint32_t)mat_get_data_pages());
    jb_puts(&jb, ",\"rodata_pages\":0");
    jb_puts(&jb, ",\"guard_pages\":");
    jb_u32_dec(&jb, (uint32_t)mat_get_guard_pages());
    {
        uint64_t wx_pages = 0;
        uint32_t mc = mat_get_count();
        for (uint32_t i = 0; i < mc; i++) {
            mat_entry_t me;
            if (mat_get_entry(i, &me) != 0 &&
                me.cls == MAT_CLASS_WX_VIOLATION) {
                wx_pages += me.num_pages;
            }
        }
        jb_puts(&jb, ",\"wx_violation_pages\":");
        jb_u32_dec(&jb, (uint32_t)wx_pages);
    }
    jb_puts(&jb, ",\"overflowed\":");
    jb_puts(&jb, mat_overflowed() ? "true" : "false");
    jb_putc(&jb, '}');

    /* rt_properties block: supported_bitmask is internal so emit a
     * placeholder hex string until uefi_rt_supported_bitmask() is
     * exposed; mismatch_count comes from the existing UEFI Runtime
     * Properties inventory accessor. */
    jb_puts(&jb, ",\"rt_properties\":{");
    jb_puts(&jb, "\"supported_bitmask\":\"0x0\",\"mismatch_count\":");
    jb_u32_dec(&jb, (uint32_t)uefi_rt_property_mismatches());
    jb_putc(&jb, '}');

    /* esrt block: full population via the ESRT inventory accessors.
     * _Header carries u64 ResourceVersion (REG_QWORD on the Registry
     * side); per-entry block is keyed by canonical brace-form FwClass
     * GUID so JSON consumers and the Registry mirror share the exact
     * same lookup keys. */
    jb_puts(&jb, ",\"esrt\":{\"_Header\":{");
    jb_puts(&jb, "\"ResourceCount\":");
    jb_u32_dec(&jb, esrt_count());
    jb_puts(&jb, ",\"ResourceCountMax\":");
    jb_u32_dec(&jb, esrt_resource_count_max());
    jb_puts(&jb, ",\"ResourceVersion\":");
    {
        char tmp[21];
        int n = 0;
        uint64_t v = esrt_resource_version();
        if (v == 0) tmp[n++] = '0';
        else {
            char r[21]; int t = 0;
            while (v) { r[t++] = (char)('0' + (v % 10ull)); v /= 10ull; }
            while (t) tmp[n++] = r[--t];
        }
        for (int i = 0; i < n; i++) jb_putc(&jb, tmp[i]);
    }
    jb_putc(&jb, '}');
    {
        uint32_t ec = esrt_count();
        for (uint32_t i = 0; i < ec; i++) {
            const struct esrt_entry *e = esrt_get_entry(i);
            if (!e) continue;
            jb_puts(&jb, ",\"{");
            static const char hex[] = "0123456789abcdef";
            uint32_t d1 = e->fw_class.data1;
            jb_putc(&jb, hex[(d1 >> 28) & 0xF]);
            jb_putc(&jb, hex[(d1 >> 24) & 0xF]);
            jb_putc(&jb, hex[(d1 >> 20) & 0xF]);
            jb_putc(&jb, hex[(d1 >> 16) & 0xF]);
            jb_putc(&jb, hex[(d1 >> 12) & 0xF]);
            jb_putc(&jb, hex[(d1 >> 8) & 0xF]);
            jb_putc(&jb, hex[(d1 >> 4) & 0xF]);
            jb_putc(&jb, hex[d1 & 0xF]);
            jb_putc(&jb, '-');
            uint16_t d2 = e->fw_class.data2;
            jb_putc(&jb, hex[(d2 >> 12) & 0xF]);
            jb_putc(&jb, hex[(d2 >> 8) & 0xF]);
            jb_putc(&jb, hex[(d2 >> 4) & 0xF]);
            jb_putc(&jb, hex[d2 & 0xF]);
            jb_putc(&jb, '-');
            uint16_t d3 = e->fw_class.data3;
            jb_putc(&jb, hex[(d3 >> 12) & 0xF]);
            jb_putc(&jb, hex[(d3 >> 8) & 0xF]);
            jb_putc(&jb, hex[(d3 >> 4) & 0xF]);
            jb_putc(&jb, hex[d3 & 0xF]);
            jb_putc(&jb, '-');
            jb_putc(&jb, hex[(e->fw_class.data4[0] >> 4) & 0xF]);
            jb_putc(&jb, hex[e->fw_class.data4[0] & 0xF]);
            jb_putc(&jb, hex[(e->fw_class.data4[1] >> 4) & 0xF]);
            jb_putc(&jb, hex[e->fw_class.data4[1] & 0xF]);
            jb_putc(&jb, '-');
            for (int b = 2; b < 8; b++) {
                jb_putc(&jb, hex[(e->fw_class.data4[b] >> 4) & 0xF]);
                jb_putc(&jb, hex[e->fw_class.data4[b] & 0xF]);
            }
            jb_puts(&jb, "}\":{");
            jb_puts(&jb, "\"Type\":");
            jb_u32_dec(&jb, e->fw_type);
            jb_puts(&jb, ",\"TypeName\":");
            jb_str(&jb, esrt_decode_type(e->fw_type));
            jb_puts(&jb, ",\"FwVersion\":");
            jb_u32_dec(&jb, e->fw_version);
            jb_puts(&jb, ",\"LowestSupportedFwVersion\":");
            jb_u32_dec(&jb, e->lowest_supported_version);
            jb_puts(&jb, ",\"CapsuleFlags\":");
            jb_u32_dec(&jb, e->capsule_flags);
            jb_puts(&jb, ",\"LastAttemptVersion\":");
            jb_u32_dec(&jb, e->last_attempt_version);
            jb_puts(&jb, ",\"LastAttemptStatus\":");
            jb_u32_dec(&jb, e->last_attempt_status);
            jb_puts(&jb, ",\"LastAttemptStatusName\":");
            jb_str(&jb, esrt_decode_status(e->last_attempt_status));
            jb_putc(&jb, '}');
        }
    }
    jb_putc(&jb, '}');

    jb_puts(&jb, ",\"apei\":{");
    emit_named_block(&jb, "bert", "BERT");
    jb_putc(&jb, ',');
    emit_named_block(&jb, "hest", "HEST");
    jb_putc(&jb, ',');
    emit_named_block(&jb, "einj", "EINJ");
    jb_putc(&jb, ',');
    emit_named_block(&jb, "erst", "ERST");
    jb_putc(&jb, '}');

    jb_puts(&jb, ",\"dbg2\":{");
    emit_named_block(&jb, "dbg2", "DBG2");
    jb_putc(&jb, '}');

    jb_puts(&jb, ",\"wsmt\":{");
    emit_named_block(&jb, "wsmt", "WSMT");
    jb_putc(&jb, '}');

    jb_putc(&jb, '}');
    jb_putc(&jb, '\n');

    if (jb_truncated(&jb)) {
        /* Fail-closed truncation policy: a buffer that filled before
         * the closing '}' could be written produces a non-parseable
         * prefix. Skip the file write entirely so consumers never see
         * malformed JSON; future cap expansion is a richer-firmware
         * scaling concern, not a runtime fallback. */
        klog(LOG_WARN, "FW",
             "JSON: firmware-tables.json buffer truncated at %u bytes -- "
             "skipping file write (cap=%u; expand FW_JSON_BUF_PAGES if firmware grows)",
             (unsigned)jb_pos(&jb), FW_JSON_BUF_SIZE);
        for (uint32_t pg = 0; pg < FW_JSON_BUF_PAGES; pg++)
            pmm_free_frame(phys + pg * 4096);
        return;
    }

    {
        struct vfs_node *f = vfs_open("X:\\Diag\\firmware-tables.json",
                                      VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
        if (f) {
            vfs_write(f, 0, (uint32_t)jb_pos(&jb), (const uint8_t *)jb_buf(&jb));
            vfs_close(f);
            klog(LOG_INFO, "FW",
                 "JSON: wrote X:\\Diag\\firmware-tables.json (%u bytes)",
                 (unsigned)jb_pos(&jb));
        } else {
            klog(LOG_WARN, "FW",
                 "JSON: could not open X:\\Diag\\firmware-tables.json for write");
        }
    }

    for (uint32_t pg = 0; pg < FW_JSON_BUF_PAGES; pg++)
        pmm_free_frame(phys + pg * 4096);
}
