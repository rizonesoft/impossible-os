/* ============================================================================
 * tpm_sb_reconcile.c -- Secure Boot Variable Measurement Reconciliation
 *
 * Decodes EV_EFI_VARIABLE_* events from the parsed TCG event log and reconciles
 * the MEASURED Secure Boot variable payloads against the LIVE UEFI variables
 * (UEFI runtime GetVariable), plus structural impossible-combination checks.
 *
 * STRUCTURAL, UNAUTHENTICATED diagnostic ONLY. With no in-kernel SHA yet, this
 * cannot prove the logged digest equals SHA-256(payload) or that the event was
 * extended into PCR7; it byte-compares the measured payload against the live
 * variable and runs presence/impossible-combination checks. It NEVER sets
 * BOOT_INTEGRITY_VERIFIED and the report carries unauthenticated=1 until PCR
 * replay validates the log against hardware PCRs.
 *
 * SMP: tpm_secureboot_reconcile() runs once on the BSP in Phase 1 (single-
 * threaded), then the report + scratch are untouched -- read-only thereafter,
 * so the static report needs no lock (same model as the event metadata array).
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/tpm.h"
#include "kernel/uefi_runtime.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "libc/string.h"

/* Little-endian byte loads (TCG/UEFI structures are LE; read with byte loads so
 * a misaligned firmware log cannot fault on strict-alignment cores). */
static uint16_t sb_le16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint64_t sb_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

/* TCG UEFI_VARIABLE_DATA: VariableName(16) + UnicodeNameLength(8, CHAR16 units)
 * + VariableDataLength(8, bytes) + UnicodeName[UCS-2] + VariableData[]. */
#define UEFI_VAR_DATA_HEADER 32u
/* Sanity cap on a measured name length so name_bytes can never overflow. A real
 * UEFI variable name is a handful of CHAR16; 64 KiB of name is pathological. */
#define UEFI_VAR_NAME_MAX_CHARS 0x10000ull

int uefi_var_data_parse(const uint8_t *payload, uint32_t size,
                        uint8_t out_guid[16],
                        uint16_t *out_name, uint32_t name_cap, uint32_t *out_name_chars,
                        uint32_t *out_data_off, uint32_t *out_data_len)
{
    if (out_name_chars) *out_name_chars = 0;
    if (out_data_off)   *out_data_off = 0;
    if (out_data_len)   *out_data_len = 0;
    if (!payload || size < UEFI_VAR_DATA_HEADER)
        return -1;

    uint64_t name_chars = sb_le64(payload + 16);
    uint64_t data_len   = sb_le64(payload + 24);
    if (name_chars > UEFI_VAR_NAME_MAX_CHARS)
        return -1;
    /* SUBTRACTION-form bounds so an attacker-controlled 64-bit data_len cannot
     * wrap an additive `need` back under `size`. size >= header is proven above;
     * name_bytes <= 0x20000 (name_chars capped). Each subtrahend is bounded by
     * the prior check, so every difference stays non-negative. */
    uint64_t name_bytes = name_chars * 2ull;
    uint64_t avail = (uint64_t)size - UEFI_VAR_DATA_HEADER;
    if (name_bytes > avail)
        return -1;
    if (data_len > avail - name_bytes)
        return -1;
    if (data_len > 0xFFFFFFFFull)       /* must fit out_data_len (uint32_t) */
        return -1;

    if (out_guid)
        for (uint32_t i = 0; i < 16u; i++)
            out_guid[i] = payload[i];

    if (out_name_chars)
        *out_name_chars = (uint32_t)name_chars;
    if (out_name && name_cap) {
        uint32_t n = (name_chars < (uint64_t)name_cap) ? (uint32_t)name_chars : name_cap;
        for (uint32_t i = 0; i < n; i++)
            out_name[i] = sb_le16(payload + UEFI_VAR_DATA_HEADER + (uint32_t)i * 2u);
    }
    if (out_data_off)
        *out_data_off = UEFI_VAR_DATA_HEADER + (uint32_t)name_bytes;
    if (out_data_len)
        *out_data_len = (uint32_t)data_len;
    return 0;
}

uint8_t sb_reconcile_classify(int log_clean, int pcr7_events, int sb_state_valid,
                              int sb_enabled, int setup_mode, int pk_present)
{
    uint8_t f = SB_IMPOSSIBLE_NONE;
    /* Unreadable live state is its own flag; with no trustworthy state we make
     * no "enabled but ..." claims (those would be unfounded). */
    if (!sb_state_valid)
        return SB_IMPOSSIBLE_STATE_UNKNOWN;
    if (sb_enabled) {
        /* Only a CLEAN parse proves PCR7 policy events are genuinely absent. A
         * degraded/unavailable log says nothing -- never a false impossibility. */
        if (log_clean && pcr7_events == 0)
            f |= SB_IMPOSSIBLE_ENABLED_NO_PCR7;
        if (setup_mode)
            f |= SB_IMPOSSIBLE_ENABLED_SETUPMODE;
        if (!pk_present)
            f |= SB_IMPOSSIBLE_ENABLED_NO_PK;
    }
    return f;
}

/* ---- Tracked-variable table -------------------------------------------------
 * use_secdb=1 -> EFI_IMAGE_SECURITY_DATABASE_GUID (db/dbx); else
 * EFI_GLOBAL_VARIABLE_GUID (PK/KEK/SecureBoot/SetupMode). UCS-2 names are NUL-
 * terminated for uefi_get_variable() and length-matched for event decoding. */
struct sb_var_def {
    uint16_t name[12];
    uint8_t  name_chars;
    uint8_t  use_secdb;
};
static const struct sb_var_def s_sb_vars[SB_VAR_COUNT] = {
    [SB_VAR_PK]         = { { 'P','K',0 },                                   2, 0 },
    [SB_VAR_KEK]        = { { 'K','E','K',0 },                               3, 0 },
    [SB_VAR_DB]         = { { 'd','b',0 },                                   2, 1 },
    [SB_VAR_DBX]        = { { 'd','b','x',0 },                               3, 1 },
    [SB_VAR_SECUREBOOT] = { { 'S','e','c','u','r','e','B','o','o','t',0 },  10, 0 },
    [SB_VAR_SETUPMODE]  = { { 'S','e','t','u','p','M','o','d','e',0 },       9, 0 },
};

/* Phase-1, single-threaded scratch for one live-variable read (reused per var).
 * 16 KiB bounds db/dbx on real platforms; an over-cap live read is reported as
 * SB_LIVE_PATHOLOGY (too large to reconcile this pass) rather than a false EQUAL. */
#define SB_LIVE_SCRATCH 16384u
static uint8_t s_live_scratch[SB_LIVE_SCRATCH];

static struct sb_reconcile_report s_sb_report;

/* Match a decoded event's (guid, UCS-2 name) to a tracked variable; -1 if none. */
static int sb_match_var(const uint8_t guid[16], const uint16_t *name, uint32_t name_chars)
{
    struct boot_uefi_guid global = EFI_GLOBAL_VARIABLE_GUID;
    struct boot_uefi_guid secdb  = EFI_IMAGE_SECURITY_DATABASE_GUID;
    for (uint32_t v = 0; v < SB_VAR_COUNT; v++) {
        const struct sb_var_def *d = &s_sb_vars[v];
        if (name_chars != d->name_chars)
            continue;
        const struct boot_uefi_guid *want = d->use_secdb ? &secdb : &global;
        if (memcmp(guid, want, 16) != 0)
            continue;
        uint32_t i;
        for (i = 0; i < name_chars; i++)
            if (name[i] != d->name[i])
                break;
        if (i == name_chars)
            return (int)v;
    }
    return -1;
}

/* Read a live UEFI variable into s_live_scratch; sets *out_len + returns the
 * sb_live_status_t. PATHOLOGY = present but larger than the scratch (cannot be
 * fully reconciled now); never reported as a match. */
static uint8_t sb_read_live(uint32_t v, uint32_t *out_len)
{
    struct boot_uefi_guid global = EFI_GLOBAL_VARIABLE_GUID;
    struct boot_uefi_guid secdb  = EFI_IMAGE_SECURITY_DATABASE_GUID;
    const struct sb_var_def *d = &s_sb_vars[v];
    const struct boot_uefi_guid *guid = d->use_secdb ? &secdb : &global;
    uint64_t dsz = SB_LIVE_SCRATCH;
    uint32_t attrs = 0;
    *out_len = 0;
    uint64_t st = uefi_get_variable(guid, d->name, &attrs, &dsz, s_live_scratch);
    if (st == UEFI_SUCCESS) {
        *out_len = (dsz > SB_LIVE_SCRATCH) ? SB_LIVE_SCRATCH : (uint32_t)dsz;
        return SB_LIVE_OK;
    }
    if (st == UEFI_NOT_FOUND)
        return SB_LIVE_NOT_FOUND;
    if (st == UEFI_BUFFER_TOO_SMALL)
        return SB_LIVE_PATHOLOGY;
    return SB_LIVE_ERROR;
}

void tpm_secureboot_reconcile(void)
{
    memset(&s_sb_report, 0, sizeof(s_sb_report));
    s_sb_report.version         = (uint16_t)SB_RECONCILE_VERSION;
    s_sb_report.size            = (uint16_t)sizeof(s_sb_report);
    s_sb_report.unauthenticated = 1u;
    s_sb_report.evlog_status    = (uint8_t)tpm_evlog_status();
    for (uint32_t v = 0; v < SB_VAR_COUNT; v++)
        s_sb_report.vars[v].var_id = (uint8_t)v;

    /* Live Secure Boot state -- honest tri-state, never collapse unknown to off. */
    int sv         = uefi_secureboot_state_valid();
    int enabled    = sv && uefi_secureboot_enabled();
    int setup_mode = uefi_secureboot_setup_mode() ? 1 : 0;
    int pk_present = uefi_secureboot_pk_present() ? 1 : 0;
    s_sb_report.sb_state_valid = (uint8_t)(sv ? 1 : 0);
    s_sb_report.sb_enabled     = (uint8_t)(enabled ? 1 : 0);
    s_sb_report.setup_mode     = (uint8_t)setup_mode;

    if (!tpm_available()) {
        /* No measured events to reconcile; still report the structural state. */
        s_sb_report.ran = 0u;
        s_sb_report.impossible_flags =
            sb_reconcile_classify(0, 0, sv, enabled, setup_mode, pk_present);
        klog(LOG_INFO, "TPM", "SB reconcile: no TPM -- live SB state only (unauthenticated)");
        return;
    }

    int log_clean = (tpm_evlog_status() == TPM_EVLOG_OK);
    const uint8_t *log = (const uint8_t *)g_boot_info.tpm_event_log;
    uint32_t log_size  = g_boot_info.tpm_event_log_size;

    /* Per-variable measured DRIVER_CONFIG payload span (the full variable). Only
     * DRIVER_CONFIG carries the whole variable; AUTHORITY carries a single
     * authorizing entry, so it counts toward PCR7 evidence but is not byte-
     * compared against the full live variable. */
    uint32_t meas_off[SB_VAR_COUNT];
    uint32_t meas_len[SB_VAR_COUNT];
    /* Presence is tracked separately from length: a valid VariableDataLength==0
     * measurement (e.g. an empty dbx) is genuinely "measured" and must compare
     * EQUAL to an empty live variable, not fall through to NOMEASURE. */
    uint8_t meas_present[SB_VAR_COUNT];
    for (uint32_t v = 0; v < SB_VAR_COUNT; v++) { meas_off[v] = 0; meas_len[v] = 0; meas_present[v] = 0; }

    uint32_t pcr7 = 0;
    if (log_clean && log) {
        uint32_t n = tpm_event_count();
        for (uint32_t i = 0; i < n; i++) {
            const struct tpm_event *e = tpm_event_get(i);
            if (!e || e->pcr_index != 7u)
                continue;
            if (e->event_type != EV_EFI_VARIABLE_DRIVER_CONFIG &&
                e->event_type != EV_EFI_VARIABLE_AUTHORITY)
                continue;
            pcr7++;
            /* Payload bytes live in the retained event-log buffer. The parser
             * proved payload_off + payload_size <= log_size; re-check defensively. */
            if (e->payload_off > log_size || e->payload_size > log_size - e->payload_off)
                continue;
            if (e->event_type != EV_EFI_VARIABLE_DRIVER_CONFIG)
                continue;
            uint8_t guid[16];
            uint16_t name[16];
            uint32_t name_chars = 0, data_off = 0, data_len = 0;
            if (uefi_var_data_parse(log + e->payload_off, e->payload_size,
                                    guid, name, 16u, &name_chars,
                                    &data_off, &data_len) != 0)
                continue;
            int v = sb_match_var(guid, name, name_chars);
            if (v < 0)
                continue;
            s_sb_report.vars[v].measured = 1u;
            /* First DRIVER_CONFIG measurement for this variable wins; record the
             * span even when data_len == 0 so an empty measurement is comparable. */
            if (!meas_present[v]) {
                meas_present[v] = 1u;
                meas_off[v] = e->payload_off + data_off;
                meas_len[v] = data_len;
            }
        }
    }
    s_sb_report.pcr7_event_count = (uint8_t)(pcr7 > 255u ? 255u : pcr7);

    /* Reconcile each variable: read live, then classify the match. */
    for (uint32_t v = 0; v < SB_VAR_COUNT; v++) {
        uint32_t live_len = 0;
        uint8_t live = sb_read_live(v, &live_len);
        s_sb_report.vars[v].live_status = live;

        uint8_t m = SB_MATCH_NA;
        int measured = meas_present[v] && log != 0;
        if (measured && live == SB_LIVE_OK) {
            /* memcmp(.,.,0)==0 makes a 0==0 length pair compare EQUAL. */
            if (meas_len[v] == live_len &&
                memcmp(log + meas_off[v], s_live_scratch, live_len) == 0)
                m = SB_MATCH_EQUAL;
            else
                m = SB_MATCH_DIFFER;
        } else if (measured) {
            m = SB_MATCH_NOLIVE;       /* measured but live unreadable/absent/too-big */
        } else if (live == SB_LIVE_OK && log_clean && log) {
            /* "No measurement" is only inferable from a CLEAN, present log. On a
             * degraded/unavailable log absence cannot be claimed -> stays NA. */
            m = SB_MATCH_NOMEASURE;
        }
        s_sb_report.vars[v].match = m;
    }

    s_sb_report.impossible_flags =
        sb_reconcile_classify(log_clean, (int)pcr7, sv, enabled, setup_mode, pk_present);
    s_sb_report.ran = 1u;

    klog(LOG_INFO, "TPM",
         "SB reconcile (unauthenticated): %u PCR7 events, SB=%s, impossible=0x%x",
         (uint64_t)pcr7,
         sv ? (enabled ? "on" : "off") : "unknown",
         (uint64_t)s_sb_report.impossible_flags);
}

const struct sb_reconcile_report *tpm_sb_reconcile_report(void)
{
    return &s_sb_report;
}
