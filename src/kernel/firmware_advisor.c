/* ============================================================================
 * firmware_advisor.c -- read-only firmware-update advisor
 *
 * Loads the LVFS-style metadata cache at X:\Diag\lvfs-metadata.json (offline
 * data only -- the live HTTPS fetch + signed-XML verification is filed as a
 * separate "live LVFS metadata refresh" follow-up, gated on TCP/TLS/crypto/
 * XML infrastructure landing first), joins each cache entry with the
 * corresponding ESRT record by FwClass GUID, and mirrors the resulting
 * per-component verdict to HKLM\SOFTWARE\Impossible\FirmwareAdvisor\<GUID>.
 *
 * Cache schema (schema_version=1):
 *   {
 *     "schema_version": 1,
 *     "fetched_unix_time": <u64>,
 *     "components": [
 *       {
 *         "fw_class":               "{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}",
 *         "latest_version":         <u32>,
 *         "vendor_update_url":      "https://...",
 *         "release_notes_url":      "https://...",
 *         "cve_id":                 "<str-empty-if-none>",
 *         "vulnerability_summary":  "<str>"
 *       },
 *       ...
 *     ]
 *   }
 *
 * Idempotent: RegDeleteTree on the parent key before writing.  Safe to call
 * with ESRT empty (parent key stays empty, no per-component subkeys).  Safe
 * to call with cache absent or malformed (every component renders as
 * status=unknown; one LOG_INFO line documents which path was taken).
 *
 * NOT a write path.  No UpdateCapsule, no OsIndications, no ESP staging.
 * Linker-time refusal lives in firmware_capsule_refused.c.
 * ============================================================================ */

#include "kernel/firmware_advisor.h"
#include "kernel/uefi_config.h"
#include "kernel/json.h"
#include "kernel/klog.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/fs/vfs.h"
#include "kernel/util/json_builder.h"
#include "registry.h"

/* Match the format used by firmware_esrt_registry.c so the joined keys live
 * in the same canonical brace form Win11 / fwupd / vendor tools all use. */
#define ADV_GUID_BUF_LEN 40

#define ADV_PATH_PREFIX "SOFTWARE\\Impossible\\FirmwareAdvisor\\"
#define ADV_PATH_CAP    96

/* Cache file location.  Pinned because every consumer (this module, the
 * userland sysinfo CLI, future fetcher) must agree on one path. */
#define ADV_CACHE_PATH "X:\\Diag\\lvfs-metadata.json"
#define ADV_CACHE_MAX_BYTES (256 * 1024)  /* 256 KiB upper bound for kmalloc */

/* ---- Latched state (BSP-only init, no SMP races after init returns) ---- */

static enum firmware_advisor_cache_state s_cache_state = FW_ADVISOR_CACHE_MISSING;
static enum firmware_advisor_status   s_status[ESRT_MAX_ENTRIES];
static enum firmware_advisor_severity s_severity[ESRT_MAX_ENTRIES];
static uint32_t s_component_count;

/* ---- Tiny string helpers -- freestanding kernel, no <string.h> ---------- */

static char fa_tolower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

/* Case-insensitive string equality.  Returns 1 on match, 0 on mismatch.
 * Matches the fat32_strcasecmp convention so consumers don't have to
 * remember which sign indicates equality. */
static int fa_strcaseeq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *b) {
        if (fa_tolower(*a) != fa_tolower(*b))
            return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

/* ---- GUID formatter (mirror of firmware_esrt_registry.c::format_fwclass_guid)
 * Kept as a private copy because the ESRT registry mirror's helper is static
 * to that translation unit; promoting it to a public API would touch
 * uefi_config.h and that's broader scope than the advisor needs. */

static void fa_hex8(char *out, uint8_t v)
{
    static const char hex[] = "0123456789abcdef";
    out[0] = hex[(v >> 4) & 0xF];
    out[1] = hex[v & 0xF];
}

static void fa_format_fwclass(const struct boot_uefi_guid *g,
                              char buf[ADV_GUID_BUF_LEN])
{
    size_t pos = 0;
    buf[pos++] = '{';
    fa_hex8(&buf[pos], (uint8_t)(g->data1 >> 24)); pos += 2;
    fa_hex8(&buf[pos], (uint8_t)(g->data1 >> 16)); pos += 2;
    fa_hex8(&buf[pos], (uint8_t)(g->data1 >> 8));  pos += 2;
    fa_hex8(&buf[pos], (uint8_t)(g->data1));       pos += 2;
    buf[pos++] = '-';
    fa_hex8(&buf[pos], (uint8_t)(g->data2 >> 8));  pos += 2;
    fa_hex8(&buf[pos], (uint8_t)(g->data2));       pos += 2;
    buf[pos++] = '-';
    fa_hex8(&buf[pos], (uint8_t)(g->data3 >> 8));  pos += 2;
    fa_hex8(&buf[pos], (uint8_t)(g->data3));       pos += 2;
    buf[pos++] = '-';
    fa_hex8(&buf[pos], g->data4[0]); pos += 2;
    fa_hex8(&buf[pos], g->data4[1]); pos += 2;
    buf[pos++] = '-';
    for (size_t i = 2; i < 8; i++) {
        fa_hex8(&buf[pos], g->data4[i]);
        pos += 2;
    }
    buf[pos++] = '}';
    buf[pos]   = '\0';
}

static void fa_compose_path(char out[ADV_PATH_CAP], const char *subkey)
{
    size_t i = 0;
    static const char prefix[] = ADV_PATH_PREFIX;
    for (; prefix[i] && i < ADV_PATH_CAP - 1; i++)
        out[i] = prefix[i];
    if (subkey) {
        for (size_t j = 0; subkey[j] && i < ADV_PATH_CAP - 1; j++, i++)
            out[i] = subkey[j];
    }
    out[i] = '\0';
}

/* ---- Cache loader ------------------------------------------------------- */

/* Reads X:\Diag\lvfs-metadata.json into a PMM-backed NUL-terminated buffer.
 * Returns NULL if the file is absent or larger than ADV_CACHE_MAX_BYTES.
 * Caller takes ownership and must release via fa_release_cache_text().
 *
 * Uses pmm_alloc_contiguous() instead of kmalloc(): the cache cap is
 * 256 KiB which is far above kmalloc's 4 KiB ceiling per CLAUDE.md
 * "Freestanding Kernel" rules.  pmm_alloc_contiguous() also gives us a
 * physically contiguous span that vfs_read can satisfy in one walk.
 *
 * *out_pages records the page count so the caller can release without
 * re-tracking it.  *out_state captures the load disposition. */
static char *fa_read_cache_text(enum firmware_advisor_cache_state *out_state,
                                uint32_t *out_pages)
{
    *out_state = FW_ADVISOR_CACHE_MISSING;
    *out_pages = 0;

    struct vfs_node *f = vfs_open(ADV_CACHE_PATH, VFS_O_READ);
    if (!f)
        return (char *)0;

    uint32_t size = f->size;
    if (size == 0 || size > ADV_CACHE_MAX_BYTES) {
        vfs_close(f);
        *out_state = FW_ADVISOR_CACHE_MALFORMED;
        return (char *)0;
    }

    /* +1 for NUL terminator the JSON parser expects -- round up to whole
     * pages since pmm_alloc_contiguous works in 4 KiB units. */
    uint32_t pages = (size + 1u + 4095u) / 4096u;
    uintptr_t phys = pmm_alloc_contiguous(pages);
    if (!phys) {
        vfs_close(f);
        *out_state = FW_ADVISOR_CACHE_MALFORMED;
        return (char *)0;
    }

    char *buf = (char *)phys;
    int n = vfs_read(f, 0, size, (uint8_t *)buf);
    vfs_close(f);
    if (n <= 0 || (uint32_t)n != size) {
        for (uint32_t p = 0; p < pages; p++)
            pmm_free_frame(phys + p * 4096u);
        *out_state = FW_ADVISOR_CACHE_MALFORMED;
        return (char *)0;
    }
    buf[size] = '\0';
    *out_pages = pages;
    return buf;
}

static void fa_release_cache_text(char *buf, uint32_t pages)
{
    if (!buf || pages == 0)
        return;
    uintptr_t phys = (uintptr_t)buf;
    for (uint32_t p = 0; p < pages; p++)
        pmm_free_frame(phys + p * 4096u);
}

/* ---- ESRT × cache join + registry mirror -------------------------------- */

/* Look up the cache component whose fw_class GUID matches `guid_str` (the
 * canonical brace form of an ESRT entry's FwClass).  Returns the matching
 * cJSON object (still owned by the parser tree) or NULL.
 *
 * Walks the cJSON child/next linked list once.  Using
 * `for (i; i<size; i++) json_array_get(arr, i)` would force cJSON to walk
 * the linked list from the head N times -- O(N^2) on cache-controlled
 * input, with N bounded only by ADV_CACHE_MAX_BYTES (256 KiB cache could
 * hold thousands of components).  Linear walk via child/next is O(N). */
static struct cJSON *fa_match_component(struct cJSON *components,
                                        const char *guid_str)
{
    for (struct cJSON *row = json_array_first(components);
         row != (struct cJSON *)0;
         row = json_array_next(row)) {
        const char *row_guid = json_str(json_get(row, "fw_class"));
        if (row_guid && fa_strcaseeq(row_guid, guid_str))
            return row;
    }
    return (struct cJSON *)0;
}

/* Decide the per-component verdict.  Pure function -- centralizes the logic
 * so tests can exercise every branch independently of the registry write
 * path.  When the cache row is NULL (no entry for this FwClass), severity
 * stays NONE per the contract.
 *
 * Malformed cache renders all components as unknown rather than panicking
 * or trusting stale metadata. */
static void fa_classify(const struct esrt_entry *esrt_row,
                        struct cJSON *cache_row,
                        enum firmware_advisor_status   *out_status,
                        enum firmware_advisor_severity *out_severity)
{
    if (!cache_row) {
        *out_status   = FW_ADVISOR_STATUS_UNKNOWN;
        *out_severity = FW_ADVISOR_SEVERITY_NONE;
        return;
    }

    /* Read latest_version as u32 via the json_u32 helper (json_int saturates
     * at INT_MAX, silently downgrading any version >= 0x80000000 to
     * 0x7FFFFFFF and flipping the verdict to "up_to_date" -- a security-
     * relevant misclassification because that path also masks a CVE).  An
     * entry without latest_version, with a non-numeric value, or with
     * out-of-range / fractional / negative numbers classifies as unknown. */
    int latest_valid = 0;
    uint32_t latest = json_u32(json_get(cache_row, "latest_version"), &latest_valid);
    if (!latest_valid || latest == 0) {
        *out_status   = FW_ADVISOR_STATUS_UNKNOWN;
        *out_severity = FW_ADVISOR_SEVERITY_NONE;
        return;
    }

    if (esrt_row->fw_version >= latest) {
        *out_status   = FW_ADVISOR_STATUS_UP_TO_DATE;
        *out_severity = FW_ADVISOR_SEVERITY_NONE;
        return;
    }

    *out_status = FW_ADVISOR_STATUS_UPDATE_AVAILABLE;
    const char *cve = json_str(json_get(cache_row, "cve_id"));
    *out_severity = (cve && cve[0])
                  ? FW_ADVISOR_SEVERITY_CRITICAL
                  : FW_ADVISOR_SEVERITY_RECOMMENDED;
}

/* RegSetString that warns and substitutes "" when the cache-supplied string
 * exceeds REG_MAX_VALUE_SIZE.  Without this guard the registry mirror would
 * silently lose a long vendor URL or vulnerability summary; the consumer
 * (sysinfo CLI, desktop notification UX) would then render an absent field
 * with no operator diagnostic. */
static void fa_set_string_bounded(HKEY h, const char *name, const char *value)
{
    /* Length-check evaluates `len < cap` BEFORE deref so we never read past
     * REG_MAX_VALUE_SIZE bytes -- cJSON guarantees NUL-termination but the
     * cap-first form is robust against any future caller.  +1 below for the
     * NUL byte the registry stores. */
    uint32_t len = 0;
    if (value)
        while (len < (uint32_t)REG_MAX_VALUE_SIZE && value[len]) len++;
    if (value && (len + 1u) > (uint32_t)REG_MAX_VALUE_SIZE) {
        klog(LOG_WARN, "advisor",
             "registry: %s exceeds %u-byte cap (len=%u); substituting empty value",
             name, (uint64_t)REG_MAX_VALUE_SIZE, (uint64_t)len);
        RegSetString(h, name, "");
        return;
    }
    RegSetString(h, name, value ? value : "");
}

static void fa_write_header(enum firmware_advisor_cache_state state,
                            uint64_t fetched_unix_time)
{
    char path[ADV_PATH_CAP];
    fa_compose_path(path, "_Header");
    HKEY h = (HKEY)0;
    uint32_t disp = 0;
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0, (const char *)0, 0,
                       KEY_ALL_ACCESS, (void *)0, &h, &disp)
        != ERROR_SUCCESS) {
        klog(LOG_WARN, "advisor",
             "Registry: failed to create %s", path);
        return;
    }
    RegSetDword(h,  "SchemaVersion",       1);
    RegSetQword(h,  "CacheFetchedUnixTime", fetched_unix_time);
    RegSetString(h, "CacheState",          firmware_advisor_cache_state_name(state));
    RegSetString(h, "RefusalNotice",
        "Impossible OS does not write firmware. "
        "Apply via the vendor's BIOS update tool.");
    RegCloseKey(h);
}

static void fa_write_component(uint32_t idx,
                               const struct esrt_entry *e,
                               const char *guid_str,
                               struct cJSON *cache_row,
                               enum firmware_advisor_status   status,
                               enum firmware_advisor_severity severity)
{
    char path[ADV_PATH_CAP];
    fa_compose_path(path, guid_str);

    HKEY h = (HKEY)0;
    uint32_t disp = 0;
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0, (const char *)0, 0,
                       KEY_ALL_ACCESS, (void *)0, &h, &disp)
        != ERROR_SUCCESS) {
        klog(LOG_WARN, "advisor", "Registry: failed to create %s", path);
        return;
    }

    RegSetDword(h,  "Current", e->fw_version);
    {
        /* Same json_u32 path as fa_classify -- using json_int here would
         * publish a truncated Latest for cache rows >= 0x80000000.
         * Out-of-range / malformed values fall through to 0. */
        int v_ok = 0;
        uint32_t latest = cache_row
            ? json_u32(json_get(cache_row, "latest_version"), &v_ok)
            : 0;
        RegSetDword(h, "Latest", v_ok ? latest : 0u);
    }
    RegSetString(h, "Status",                   firmware_advisor_status_name(status));
    RegSetString(h, "Severity",                 firmware_advisor_severity_name(severity));
    RegSetDword(h,  "LastAttemptStatus",        e->last_attempt_status);
    RegSetString(h, "LastAttemptStatusName",    esrt_decode_status(e->last_attempt_status));
    RegSetDword(h,  "RollbackFloorOk",          (uint32_t)esrt_rollback_floor_ok(idx));
    RegSetDword(h,  "LowestSupportedFwVersion", e->lowest_supported_version);

    /* Cache-derived strings.  REG_MAX_VALUE_SIZE caps individual values at
     * 512 bytes; an oversized vendor URL / vulnerability summary in the
     * operator-supplied cache would silently fail RegSetString and the
     * registry mirror would lack the field with no diagnostic.  Bounded
     * (no memory-safety risk), but a quiet drop violates the registry-
     * mirror contract -- log at LOG_WARN and write an explicit "" so the
     * value name still exists for consumers. */
    const char *vendor_url   = cache_row ? json_str(json_get(cache_row, "vendor_update_url"))   : (const char *)0;
    const char *release_url  = cache_row ? json_str(json_get(cache_row, "release_notes_url"))   : (const char *)0;
    const char *cve_id       = cache_row ? json_str(json_get(cache_row, "cve_id"))              : (const char *)0;
    const char *vuln_summary = cache_row ? json_str(json_get(cache_row, "vulnerability_summary")): (const char *)0;
    fa_set_string_bounded(h, "VendorUpdateUrl",      vendor_url   ? vendor_url   : "");
    fa_set_string_bounded(h, "ReleaseNotesUrl",      release_url  ? release_url  : "");
    fa_set_string_bounded(h, "CveId",                cve_id       ? cve_id       : "");
    fa_set_string_bounded(h, "VulnerabilitySummary", vuln_summary ? vuln_summary : "");

    RegCloseKey(h);
}

/* ---- JSON publish to X:\Diag\firmware-advisor.json ---------------------- *
 * Denormalized verdict the user-mode sysinfo tool reads.  Cleaner than
 * exposing user-mode registry SSDT plumbing for one consumer.  Same fail-
 * closed truncation policy as firmware_tables_json (jb_truncated -> skip
 * the disk write so consumers never see a half-written prefix).  Pinned
 * 16 KiB buffer matches the firmware-tables.json budget. */

#define FA_JSON_BUF_PAGES 4u
#define FA_JSON_BUF_SIZE  (FA_JSON_BUF_PAGES * 4096u)
#define FA_JSON_PATH      "X:\\Diag\\firmware-advisor.json"

/* Decimal-emit a 64-bit fetched_unix_time.  jb_u32_dec would truncate at
 * 2106-02-07; even though that's far away, the kernel writer commits to
 * 64-bit timestamps everywhere else (fetched_unix_time is u64 in the
 * cache schema) so the publish path stays consistent. */
static void fa_jb_u64_dec(struct json_builder *j, uint64_t v)
{
    char tmp[24];
    int n = 0;
    if (v == 0) {
        jb_putc(j, '0');
        return;
    }
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n--)
        jb_putc(j, tmp[n]);
}

/* Emit one component object into the json_builder.  cache_row may be NULL
 * for components that had no LVFS cache match (status will be unknown). */
static void fa_emit_component(struct json_builder *jb, uint32_t i,
                              const struct esrt_entry *e,
                              struct cJSON *cache_row)
{
    char guid[ADV_GUID_BUF_LEN];
    fa_format_fwclass(&e->fw_class, guid);

    jb_putc(jb, '{');
    jb_puts(jb, "\"fw_class\":");           jb_str(jb, guid);
    jb_puts(jb, ",\"fw_type_name\":");      jb_str(jb, esrt_decode_type(e->fw_type));
    jb_puts(jb, ",\"current\":");           jb_u32_dec(jb, e->fw_version);
    jb_puts(jb, ",\"latest\":");
    {
        int v_ok = 0;
        uint32_t latest = cache_row
            ? json_u32(json_get(cache_row, "latest_version"), &v_ok)
            : 0;
        jb_u32_dec(jb, v_ok ? latest : 0u);
    }
    jb_puts(jb, ",\"status\":");            jb_str(jb, firmware_advisor_status_name(s_status[i]));
    jb_puts(jb, ",\"severity\":");          jb_str(jb, firmware_advisor_severity_name(s_severity[i]));

    const char *vendor_url   = cache_row ? json_str(json_get(cache_row, "vendor_update_url"))    : (const char *)0;
    const char *release_url  = cache_row ? json_str(json_get(cache_row, "release_notes_url"))    : (const char *)0;
    const char *cve_id       = cache_row ? json_str(json_get(cache_row, "cve_id"))               : (const char *)0;
    const char *vuln_summary = cache_row ? json_str(json_get(cache_row, "vulnerability_summary")) : (const char *)0;
    jb_puts(jb, ",\"vendor_update_url\":");      jb_str(jb, vendor_url   ? vendor_url   : "");
    jb_puts(jb, ",\"release_notes_url\":");      jb_str(jb, release_url  ? release_url  : "");
    jb_puts(jb, ",\"cve_id\":");                 jb_str(jb, cve_id       ? cve_id       : "");
    jb_puts(jb, ",\"vulnerability_summary\":");  jb_str(jb, vuln_summary ? vuln_summary : "");

    jb_puts(jb, ",\"last_attempt_status\":");        jb_u32_dec(jb, e->last_attempt_status);
    jb_puts(jb, ",\"last_attempt_status_name\":");   jb_str(jb, esrt_decode_status(e->last_attempt_status));
    jb_puts(jb, ",\"rollback_floor_ok\":");          jb_u32_dec(jb, (uint32_t)esrt_rollback_floor_ok(i));
    jb_puts(jb, ",\"lowest_supported_fw_version\":"); jb_u32_dec(jb, e->lowest_supported_version);
    jb_putc(jb, '}');
}

/* Build + write the user-mode-readable advisor JSON.  Uses the in-flight
 * components array so cache-derived strings (vendor URL, CVE, etc.) are
 * available without re-parsing the cache file. */
static void fa_publish_json(struct cJSON *components, uint64_t fetched_unix_time)
{
    uintptr_t phys = pmm_alloc_contiguous(FA_JSON_BUF_PAGES);
    if (!phys) {
        klog(LOG_WARN, "advisor",
             "JSON: cannot alloc %u pages for firmware-advisor.json",
             (uint64_t)FA_JSON_BUF_PAGES);
        return;
    }
    struct json_builder jb;
    jb_init(&jb, (char *)phys, FA_JSON_BUF_SIZE);

    jb_putc(&jb, '{');
    jb_puts(&jb, "\"schema_version\":1");
    jb_puts(&jb, ",\"cache_state\":");
    jb_str(&jb, firmware_advisor_cache_state_name(s_cache_state));
    jb_puts(&jb, ",\"cache_fetched_unix_time\":");
    fa_jb_u64_dec(&jb, fetched_unix_time);

    jb_puts(&jb, ",\"refusal_notice\":");
    jb_str(&jb, "Apply via the vendor's BIOS update tool; "
                "Impossible OS does not write firmware.");

    jb_puts(&jb, ",\"components\":[");
    {
        int first = 1;
        for (uint32_t i = 0; i < s_component_count; i++) {
            const struct esrt_entry *e = esrt_get_entry(i);
            if (!e) continue;
            if (!first) jb_putc(&jb, ',');
            first = 0;

            char guid[ADV_GUID_BUF_LEN];
            fa_format_fwclass(&e->fw_class, guid);

            struct cJSON *row = components ? fa_match_component(components, guid)
                                           : (struct cJSON *)0;
            fa_emit_component(&jb, i, e, row);
        }
    }
    jb_putc(&jb, ']');
    jb_putc(&jb, '}');

    if (jb_truncated(&jb)) {
        klog(LOG_WARN, "advisor",
             "JSON: firmware-advisor.json truncated; skipping disk write");
        for (uint32_t p = 0; p < FA_JSON_BUF_PAGES; p++)
            pmm_free_frame(phys + p * 4096u);
        return;
    }

    struct vfs_node *f = vfs_open(FA_JSON_PATH,
                                  VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (!f) {
        klog(LOG_WARN, "advisor",
             "JSON: could not open %s for write", FA_JSON_PATH);
    } else {
        int wrote = vfs_write(f, 0, (uint32_t)jb_pos(&jb), (uint8_t *)jb_buf(&jb));
        vfs_close(f);
        if (wrote == (int)jb_pos(&jb))
            klog(LOG_INFO, "advisor",
                 "JSON: wrote %s (%u bytes)", FA_JSON_PATH, (uint64_t)jb_pos(&jb));
        else
            klog(LOG_WARN, "advisor",
                 "JSON: short write to %s (wrote=%d, expected=%u)",
                 FA_JSON_PATH, (uint64_t)wrote, (uint64_t)jb_pos(&jb));
    }

    for (uint32_t p = 0; p < FA_JSON_BUF_PAGES; p++)
        pmm_free_frame(phys + p * 4096u);
}

/* ---- Public entry points ------------------------------------------------ */

void firmware_advisor_init(void)
{
    /* Reset latched state -- safe to re-run if a future caller wants to
     * refresh after a network fetch lands the live LVFS metadata. */
    s_cache_state     = FW_ADVISOR_CACHE_MISSING;
    s_component_count = 0;
    for (uint32_t i = 0; i < ESRT_MAX_ENTRIES; i++) {
        s_status[i]   = FW_ADVISOR_STATUS_UNKNOWN;
        s_severity[i] = FW_ADVISOR_SEVERITY_NONE;
    }

    /* Idempotent reset: drop the entire FirmwareAdvisor subtree before
     * rebuilding it.  ERROR_FILE_NOT_FOUND is fine -- first-ever boot or a
     * previously empty tree both end up at the same state. */
    (void)RegDeleteTree(HKEY_LOCAL_MACHINE,
                        "SOFTWARE\\Impossible\\FirmwareAdvisor");

    /* Load + parse the cache.  The triage matrix:
     *   file absent      -> CACHE_MISSING,   all components UNKNOWN
     *   file present, parse fails       -> CACHE_MALFORMED, all UNKNOWN
     *   file present, schema_version!=1 -> CACHE_MALFORMED, all UNKNOWN
     *   file present, valid             -> CACHE_LOADED,    classify each
     */
    uint32_t text_pages = 0;
    char *text = fa_read_cache_text(&s_cache_state, &text_pages);
    struct cJSON *root = (struct cJSON *)0;
    struct cJSON *components = (struct cJSON *)0;
    uint64_t fetched_unix_time = 0;

    if (text) {
        root = json_parse(text);
        if (!root) {
            s_cache_state = FW_ADVISOR_CACHE_MALFORMED;
        } else {
            int schema = json_int(json_get(root, "schema_version"));
            components = json_get(root, "components");
            /* Reject malformed shape: missing components OR present-but-not-
             * an-array.  json_get(root,"components") == NULL means the key
             * is absent; json_is_array() == 0 means the key is present but
             * the value is the wrong shape (object / number / string).
             * Without the explicit type check the cache reports LOADED while
             * every component classifies as unknown -- a false positive that
             * would suppress a real CVE advisory. */
            if (schema != 1 || !components || !json_is_array(components)) {
                s_cache_state = FW_ADVISOR_CACHE_MALFORMED;
                json_free(root);
                root = (struct cJSON *)0;
                components = (struct cJSON *)0;
            } else {
                s_cache_state = FW_ADVISOR_CACHE_LOADED;
                /* fetched_unix_time is informational; route through json_u64
                 * for u64 safety -- json_int saturates at INT_MAX so a unix
                 * timestamp past 2038 would corrupt before reaching the
                 * registry.  Out-of-range / negative / non-integral fall
                 * through to 0. */
                int fts_valid = 0;
                fetched_unix_time = json_u64(json_get(root, "fetched_unix_time"),
                                             &fts_valid);
                if (!fts_valid)
                    fetched_unix_time = 0;
            }
        }
    }

    /* Always emit one LOG_INFO summarizing how the cache loaded so operators
     * can grep serial logs without needing to read the registry. */
    klog(LOG_INFO, "advisor",
         "cache: %s%s",
         firmware_advisor_cache_state_name(s_cache_state),
         (s_cache_state == FW_ADVISOR_CACHE_LOADED) ? " -- classifying" : "");

    /* Always write the header so consumers can read the cache state via
     * registry even when no per-component subkeys exist. */
    fa_write_header(s_cache_state, fetched_unix_time);

    uint32_t esrt_n = esrt_count();
    if (esrt_n == 0) {
        klog(LOG_INFO, "advisor",
             "ESRT empty; SOFTWARE\\Impossible\\FirmwareAdvisor populated with header only");
        /* Publish empty-components JSON so sysinfo.exe can render the
         * "no firmware components reported by ESRT" message without
         * special-casing the missing-file path. */
        fa_publish_json(components, fetched_unix_time);
        if (root) json_free(root);
        if (text) fa_release_cache_text(text, text_pages);
        return;
    }

    if (esrt_n > ESRT_MAX_ENTRIES)
        esrt_n = ESRT_MAX_ENTRIES;

    uint32_t up_to_date = 0;
    uint32_t update_available = 0;
    uint32_t unknown = 0;

    for (uint32_t i = 0; i < esrt_n; i++) {
        const struct esrt_entry *e = esrt_get_entry(i);
        if (!e) continue;

        char guid_str[ADV_GUID_BUF_LEN];
        fa_format_fwclass(&e->fw_class, guid_str);

        struct cJSON *cache_row = components
                                ? fa_match_component(components, guid_str)
                                : (struct cJSON *)0;

        enum firmware_advisor_status   st;
        enum firmware_advisor_severity sv;
        fa_classify(e, cache_row, &st, &sv);

        s_status[i]   = st;
        s_severity[i] = sv;

        switch (st) {
        case FW_ADVISOR_STATUS_UP_TO_DATE:       up_to_date++; break;
        case FW_ADVISOR_STATUS_UPDATE_AVAILABLE: update_available++; break;
        default:                                  unknown++; break;
        }

        fa_write_component(i, e, guid_str, cache_row, st, sv);
    }
    s_component_count = esrt_n;

    klog(LOG_INFO, "advisor",
         "%u component(s) classified: %u up-to-date, %u update_available, %u unknown",
         esrt_n, up_to_date, update_available, unknown);

    /* Publish the user-mode readable verdict so sysinfo.exe doesn't need
     * registry SSDT plumbing.  Done while `components` is still live so
     * cache-derived strings (vendor URL, CVE, etc.) survive into the JSON. */
    fa_publish_json(components, fetched_unix_time);

    if (root) json_free(root);
    if (text) fa_release_cache_text(text, text_pages);
}

enum firmware_advisor_cache_state firmware_advisor_cache_state(void)
{
    return s_cache_state;
}

uint32_t firmware_advisor_component_count(void)
{
    return s_component_count;
}

uint32_t firmware_advisor_count_with_status(enum firmware_advisor_status s)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s_component_count; i++) {
        if (s_status[i] == s)
            n++;
    }
    return n;
}

enum firmware_advisor_status firmware_advisor_status_by_index(uint32_t idx)
{
    if (idx >= s_component_count)
        return FW_ADVISOR_STATUS_UNKNOWN;
    return s_status[idx];
}

enum firmware_advisor_severity firmware_advisor_severity_by_index(uint32_t idx)
{
    if (idx >= s_component_count)
        return FW_ADVISOR_SEVERITY_NONE;
    return s_severity[idx];
}

const char *firmware_advisor_status_name(enum firmware_advisor_status s)
{
    switch (s) {
    case FW_ADVISOR_STATUS_UP_TO_DATE:       return "up_to_date";
    case FW_ADVISOR_STATUS_UPDATE_AVAILABLE: return "update_available";
    default:                                  return "unknown";
    }
}

const char *firmware_advisor_severity_name(enum firmware_advisor_severity s)
{
    switch (s) {
    case FW_ADVISOR_SEVERITY_CRITICAL:    return "critical";
    case FW_ADVISOR_SEVERITY_RECOMMENDED: return "recommended";
    default:                               return "";
    }
}

const char *firmware_advisor_cache_state_name(enum firmware_advisor_cache_state s)
{
    switch (s) {
    case FW_ADVISOR_CACHE_LOADED:    return "loaded";
    case FW_ADVISOR_CACHE_MALFORMED: return "malformed";
    default:                          return "missing";
    }
}
