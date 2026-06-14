/* ============================================================================
 * tpm.c -- TPM Measured Boot event log parser
 *
 * Reads the TCG event log that the bootloader retrieved from the
 * EFI_TCG2_PROTOCOL before ExitBootServices().  Parses the log to
 * count events, identify hash algorithms, and store TPM state.
 *
 * The event log uses the TCG PC Client Specific Implementation
 * Specification format:
 * - First entry: TCG_PCR_EVENT with EV_NO_ACTION containing
 *   TCG_EfiSpecIDEvent (identifies log format version + hash sizes)
 * - Remaining entries (TPM 2.0): TCG_PCR_EVENT2 with variable-length
 *   TPML_DIGEST_VALUES based on active PCR banks
 * ============================================================================ */

#include "kernel/tpm.h"
#include "kernel/tpm_transport.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "kernel/fs/vfs.h"
#include "libc/string.h"   /* snprintf */

/* ---- TCG event log structures ---- */

/* TCG_PCR_EVENT (legacy format, always first entry even in crypto-agile logs) */
struct tcg_pcr_event {
    uint32_t pcr_index;
    uint32_t event_type;
    uint8_t  digest[20];       /* SHA-1 */
    uint32_t event_data_size;
    /* uint8_t event_data[] follows */
};

/* TCG_EfiSpecIDEvent (payload of the first EV_NO_ACTION event) */
struct tcg_spec_id_event {
    uint8_t  signature[16];    /* "Spec ID Event03\0" for crypto-agile */
    uint32_t platform_class;
    uint8_t  spec_version_minor;
    uint8_t  spec_version_major;
    uint8_t  spec_errata;
    uint8_t  uintn_size;      /* 1=uint32, 2=uint64 */
    uint32_t number_of_algorithms;
    /* digest_sizes[] follows: pairs of (uint16_t alg_id, uint16_t digest_size) */
};

/* TPM_ALG_SHA1/256/384/512 now live in kernel/tpm.h (public API inputs). */

/* Event type constants */
#define EV_NO_ACTION                0x00000003
#define EV_EFI_VARIABLE_BOOT        0x80000002
#define EV_EFI_BOOT_SERVICES_APP    0x80000003

/* ---- Module state ---- */
static int      s_available;
static int      s_version;
static uint32_t s_event_count;

/* Preserved per-event measured-boot metadata. Written once by tpm_init() on the
 * Phase-1 BSP and read-only afterwards, so no lock is required. */
static struct tpm_event  s_events[TPM_EVENT_MAX];
static uint32_t          s_event_overflow;
static tpm_evlog_status_t s_evlog_status = TPM_EVLOG_NO_LOG;
static uint32_t          s_evlog_fail_offset;

/* ---- Unaligned-safe little-endian byte loads ----
 *
 * The TCG log is a flat firmware buffer with no alignment guarantee. Reading
 * multi-byte fields with pointer casts (e.g. *(const uint32_t*)p) can fault on
 * strict-alignment cores; assemble the value from bytes instead. Every caller
 * MUST first prove (via the subtraction-form bounds checks) that the bytes are
 * in range. */
static inline uint16_t tpm_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t tpm_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Digest length in bytes for a TCG algorithm id; 0 means unknown/unsupported. */
static uint16_t tpm_alg_digest_len(uint16_t alg_id)
{
    switch (alg_id) {
        case TPM_ALG_SHA1:   return 20;
        case TPM_ALG_SHA256: return 32;
        case TPM_ALG_SHA384: return 48;
        case TPM_ALG_SHA512: return 64;
        default:             return 0;
    }
}

/* Strength rank for "primary digest" selection (higher = stronger). */
static int tpm_alg_rank(uint16_t alg_id)
{
    switch (alg_id) {
        case TPM_ALG_SHA512: return 4;
        case TPM_ALG_SHA384: return 3;
        case TPM_ALG_SHA256: return 2;
        case TPM_ALG_SHA1:   return 1;
        default:             return 0;
    }
}

/* Record one parsed event into the metadata array. Returns 0 on success, 1 on
 * cap overflow (caller stops the walk and reports TPM_EVLOG_CAP_EXCEEDED). */
static int tpm_event_record(struct tpm_event *out, uint32_t out_max, uint32_t idx,
                            uint32_t pcr_index, uint32_t event_type,
                            uint32_t digest_count, uint16_t primary_alg_id,
                            uint8_t primary_digest_len, uint32_t primary_digest_off,
                            uint32_t payload_off, uint32_t payload_size,
                            uint32_t digests_off)
{
    if (!out)            /* count-only caller: nothing to store, not an overflow */
        return 0;
    if (idx >= out_max)  /* genuine cap overflow */
        return 1;
    struct tpm_event *e = &out[idx];
    e->pcr_index          = pcr_index;
    e->event_type         = event_type;
    e->digest_count       = digest_count;
    e->primary_alg_id     = primary_alg_id;
    e->primary_digest_len = primary_digest_len;
    e->pad                = 0;
    e->primary_digest_off = primary_digest_off;
    e->payload_off        = payload_off;
    e->payload_size       = payload_size;
    e->digests_off        = digests_off;
    return 0;
}

/* Pure TCG event-log parser (no globals, no klog) so it is unit-testable
 * against fixture buffers. Walks `log[0..log_size)` of the given TCG version,
 * filling out[0..out_max) with per-event metadata. *out_count is the number of
 * events recorded, *out_overflow is 1 when the log held more than out_max
 * events, *out_fail_offset is the byte offset of the first rejection (0 on OK).
 * Returns the structured status; a non-OK status means a malformed log -- the
 * caller must NOT treat the partial prefix as authoritative. */
tpm_evlog_status_t tpm_evlog_parse(const uint8_t *log, uint32_t log_size, int version,
                                   struct tpm_event *out, uint32_t out_max,
                                   uint32_t *out_count, uint32_t *out_overflow,
                                   uint32_t *out_fail_offset)
{
    uint32_t rec = 0, overflow = 0, fail_off = 0;
    tpm_evlog_status_t status = TPM_EVLOG_OK;

    if (out_count)       *out_count = 0;
    if (out_overflow)    *out_overflow = 0;
    if (out_fail_offset) *out_fail_offset = 0;

    if (!log || (size_t)log_size < sizeof(struct tcg_pcr_event))
        return TPM_EVLOG_BAD_HEADER;

    /* First entry is TCG_PCR_EVENT (always): pcr(4) type(4) digest[20]
     * data_size(4) data[]. Containment is subtraction-form (overflow-safe). */
    uint32_t first_pcr   = tpm_le32(log + 0);
    uint32_t first_type  = tpm_le32(log + 4);
    uint32_t first_dsize = tpm_le32(log + 28);
    if ((size_t)first_dsize > (size_t)log_size - sizeof(struct tcg_pcr_event)) {
        if (out_fail_offset) *out_fail_offset = 28u;
        return TPM_EVLOG_TRUNCATED;
    }
    if (tpm_event_record(out, out_max, rec, first_pcr, first_type, 1u,
                         TPM_ALG_SHA1, 20u, 8u, 32u, first_dsize, 8u)) {
        if (out_overflow)    *out_overflow = 1;
        if (out_fail_offset) *out_fail_offset = 0u;
        return TPM_EVLOG_CAP_EXCEEDED;   /* out!=NULL with out_max == 0 */
    }
    rec++;

    uint32_t offset = (uint32_t)sizeof(struct tcg_pcr_event) + first_dsize;

    if (version == 2) {
        /* TCG_PCR_EVENT2: pcr(4) type(4) TPML_DIGEST_VALUES{count(4)
         * [alg(2) digest[]]*} data_size(4) data[]. */
        while (offset < log_size) {
            /* offset < log_size with fewer than a header's worth of bytes left
             * is a dangling partial event, not clean EOF (an exact log ends at
             * offset == log_size). Treat it as truncation. */
            if ((log_size - offset) < 12u) {
                status = TPM_EVLOG_TRUNCATED; fail_off = offset; break;
            }
            uint32_t pcr    = tpm_le32(log + offset);
            uint32_t etype  = tpm_le32(log + offset + 4u);
            uint32_t dcount = tpm_le32(log + offset + 8u);
            if (dcount > 8u) { status = TPM_EVLOG_TRUNCATED; fail_off = offset + 8u; break; }

            uint32_t digests_size = 4u;
            uint32_t dpos = offset + 12u;
            uint16_t primary_alg = 0; uint8_t primary_len = 0; uint32_t primary_off = 0;
            int best = 0, oob = 0, bad_alg = 0;
            uint32_t d;
            for (d = 0; d < dcount; d++) {
                if (dpos > log_size || (log_size - dpos) < 2u) { oob = 1; break; }
                uint16_t alg_id = tpm_le16(log + dpos);
                uint16_t dsz = tpm_alg_digest_len(alg_id);
                if (dsz == 0u) { bad_alg = 1; break; }
                if ((log_size - dpos) < (uint32_t)(2u + dsz)) { oob = 1; break; }
                int r = tpm_alg_rank(alg_id);
                if (r > best) {
                    best = r; primary_alg = alg_id;
                    primary_len = (uint8_t)dsz; primary_off = dpos + 2u;
                }
                digests_size += 2u + dsz;
                dpos += 2u + dsz;
            }
            if (bad_alg) { status = TPM_EVLOG_UNSUPPORTED_ALG; fail_off = dpos; break; }
            if (oob)     { status = TPM_EVLOG_TRUNCATED; fail_off = dpos; break; }

            uint32_t event2_header = 8u + digests_size;
            if (event2_header > log_size - offset
                || 4u > log_size - offset - event2_header) {
                status = TPM_EVLOG_TRUNCATED; fail_off = offset; break;
            }
            uint32_t ev_data_size = tpm_le32(log + offset + event2_header);
            if (ev_data_size > log_size - offset - event2_header - 4u) {
                status = TPM_EVLOG_TRUNCATED; fail_off = offset + event2_header; break;
            }
            uint32_t payload_off = offset + event2_header + 4u;
            uint32_t entry_size  = event2_header + 4u + ev_data_size;

            if (tpm_event_record(out, out_max, rec, pcr, etype, dcount, primary_alg,
                                 primary_len, primary_off, payload_off, ev_data_size,
                                 offset + 12u)) {
                status = TPM_EVLOG_CAP_EXCEEDED; fail_off = offset; overflow = 1; break;
            }
            rec++;
            offset += entry_size;
        }
    } else {
        /* TCG 1.2: every entry is TCG_PCR_EVENT (32-byte header + data). */
        while (offset < log_size) {
            /* Sub-header remainder is a dangling partial event, not clean EOF. */
            if ((log_size - offset) < 32u) {
                status = TPM_EVLOG_TRUNCATED; fail_off = offset; break;
            }
            uint32_t pcr   = tpm_le32(log + offset);
            uint32_t etype = tpm_le32(log + offset + 4u);
            uint32_t ev_data_size = tpm_le32(log + offset + 28u);
            if (ev_data_size > log_size - offset - 32u) {
                status = TPM_EVLOG_TRUNCATED; fail_off = offset + 28u; break;
            }
            uint32_t payload_off = offset + 32u;
            uint32_t entry_size  = 32u + ev_data_size;
            if (tpm_event_record(out, out_max, rec, pcr, etype, 1u, TPM_ALG_SHA1, 20u,
                                 offset + 8u, payload_off, ev_data_size, offset + 8u)) {
                status = TPM_EVLOG_CAP_EXCEEDED; fail_off = offset; overflow = 1; break;
            }
            rec++;
            offset += entry_size;
        }
    }

    if (out_overflow)    *out_overflow = overflow;
    if (out_fail_offset) *out_fail_offset = fail_off;
    if (out_count)       *out_count = rec;
    return status;
}

boot_result_t tpm_init(void)
{
    s_available = 0;
    s_version = 0;
    s_event_count = 0;
    s_event_overflow = 0;
    s_evlog_status = TPM_EVLOG_NO_LOG;
    s_evlog_fail_offset = 0;

    if (!g_boot_info.tpm_available) {
        klog(LOG_INFO, "TPM", "Not detected");
        return BOOT_DEGRADED;
    }

    s_available = 1;
    s_version = g_boot_info.tpm_version;

    /* Authoritative gate: capability negotiation decides whether the
     * event log is available. A loader that reports
     * BOOT_CAP_TPM_EVENT_LOG as degraded (e.g. log pointer present
     * but integrity check failed) MUST leave s_event_count at 0 so
     * attestation falls back to a minimal PCR-read path. */
    if (!boot_caps_require(BOOT_CAP_TPM_EVENT_LOG)) {
        klog(LOG_WARN, "TPM",
             "Event log degraded by loader caps (caps_present=0x%lx); "
             "skipping parse, attestation falls back to PCR-read only",
             (uint64_t)(g_boot_info.caps_present & BOOT_CAP_TPM_EVENT_LOG));
        return BOOT_DEGRADED;
    }

    const uint8_t *log = (const uint8_t *)g_boot_info.tpm_event_log;
    uint32_t log_size = g_boot_info.tpm_event_log_size;

    if (!log || log_size < sizeof(struct tcg_pcr_event)) {
        klog(LOG_WARN, "TPM", "TPM %s detected but event log invalid",
             s_version == 2 ? "2.0" : "1.2");
        return BOOT_DEGRADED;
    }

    /* Parse via the pure walker (unit-testable; see tpm_evlog_parse) into the
     * static metadata array, then publish the structured result. */
    uint32_t cnt = 0, ovf = 0, foff = 0;
    tpm_evlog_status_t st = tpm_evlog_parse(log, log_size, s_version,
                                            s_events, TPM_EVENT_MAX,
                                            &cnt, &ovf, &foff);
    s_evlog_status      = st;
    s_evlog_fail_offset = foff;
    s_event_overflow    = ovf;

    if (st != TPM_EVLOG_OK) {
        /* Malformed log: do not expose a partial prefix as a valid count.
         * Callers gate on tpm_evlog_status() == TPM_EVLOG_OK. */
        s_event_count = 0;
        klog(LOG_WARN, "TPM",
             "TPM %s event log rejected (status=%d at offset %u); %u events before fault",
             s_version == 2 ? "2.0" : "1.2", (int)st, foff, cnt);
        return BOOT_DEGRADED;
    }

    s_event_count = cnt;
    klog(LOG_INFO, "TPM", "TPM %s detected, %u boot events measured",
         s_version == 2 ? "2.0" : "1.2", cnt);
    return BOOT_OK;
}

int tpm_available(void)
{
    return s_available;
}

int tpm_version(void)
{
    return s_version;
}

uint32_t tpm_event_count(void)
{
    return s_event_count;
}

tpm_evlog_status_t tpm_evlog_status(void)
{
    return s_evlog_status;
}

uint32_t tpm_evlog_fail_offset(void)
{
    return s_evlog_fail_offset;
}

int tpm_event_overflow(void)
{
    return (int)s_event_overflow;
}

const struct tpm_event *tpm_event_get(uint32_t i)
{
    /* Metadata is meaningful only after a clean parse; a rejected log leaves
     * a partial prefix that callers must not treat as authoritative. */
    if (s_evlog_status != TPM_EVLOG_OK || i >= s_event_count)
        return (const struct tpm_event *)0;
    return &s_events[i];
}

/* ---- CEL-JSON export ----
 *
 * Writes X:\Diag\tpm-events.json as a TCG Canonical Event Log (CEL) JSON
 * subset so external verifiers (systemd-pcrlock, Keylime) can consume it. The
 * parse runs in Phase 0 (no filesystem); this export runs from the post-mount
 * desktop bring-up off the preserved metadata. STREAMING: one bounded
 * vfs_write per event, so an attacker-influenced event count cannot force a
 * large in-RAM JSON buffer. Payload bodies are omitted (offset/size + digest
 * are exported); the digest hex is read from the retained log buffer. Only a
 * clean parse (TPM_EVLOG_OK) is exported -- a rejected log writes nothing. */
static char tpm_hexdig(uint8_t n)
{
    return (char)(n < 10u ? ('0' + n) : ('a' + (n - 10u)));
}

static const char *tpm_alg_name(uint16_t alg_id)
{
    switch (alg_id) {
        case TPM_ALG_SHA1:   return "sha1";
        case TPM_ALG_SHA256: return "sha256";
        case TPM_ALG_SHA384: return "sha384";
        case TPM_ALG_SHA512: return "sha512";
        default:             return "unknown";
    }
}

/* Append n bytes of s into the coalescing buffer acc[cap], flushing acc to f
 * at *woff when it would overflow. Coalesces the per-event JSON into a bounded
 * buffer so an attacker-influenced event count (up to TPM_EVENT_MAX) produces
 * ~cap/event-size writes instead of one tiny FAT32 write per event, while the
 * in-RAM buffer stays bounded (the streaming intent). Returns 0 / -1 on a
 * vfs_write failure. */
static int cel_append(struct vfs_node *f, char *acc, uint32_t cap, uint32_t *alen,
                      uint32_t *woff, const char *s, uint32_t n)
{
    if (*alen != 0u && *alen + n > cap) {
        if (vfs_write(f, *woff, *alen, (const uint8_t *)acc) != (int)*alen)
            return -1;
        *woff += *alen;
        *alen = 0u;
    }
    if (n > cap) {                       /* single chunk bigger than the buffer */
        if (vfs_write(f, *woff, n, (const uint8_t *)s) != (int)n)
            return -1;
        *woff += n;
        return 0;
    }
    memcpy(acc + *alen, s, n);
    *alen += n;
    return 0;
}

void tpm_evlog_export_cel(void)
{
    if (s_evlog_status != TPM_EVLOG_OK || s_event_count == 0u)
        return;
    const uint8_t *log = (const uint8_t *)g_boot_info.tpm_event_log;
    uint32_t log_size = g_boot_info.tpm_event_log_size;
    if (!log)
        return;

    /* Create via parent dir first (FAT32 dir-cache re-walk pattern). */
    struct vfs_node *dir = vfs_open("X:\\Diag\\", VFS_O_READ);
    if (dir && dir->ops && dir->ops->create)
        dir->ops->create(dir, "tpm-events.json", VFS_FILE);
    struct vfs_node *f = vfs_open("X:\\Diag\\tpm-events.json", VFS_O_WRITE);
    if (!f) {
        klog(LOG_WARN, "TPM", "CEL export: cannot open X:\\Diag\\tpm-events.json");
        return;
    }

    uint32_t woff = 0, alen = 0;
    char acc[4096];     /* coalescing buffer (bounded RAM) */
    char chunk[640];    /* per-event format temp */
    int n;

    n = snprintf(chunk, sizeof(chunk),
                 "{\"version\":1,\"format\":\"cel-json-subset\","
                 "\"eventCount\":%u,\"events\":[\n", s_event_count);
    if (n <= 0 || (uint32_t)n >= sizeof(chunk)
        || cel_append(f, acc, sizeof(acc), &alen, &woff, chunk, (uint32_t)n)) {
        vfs_close(f);
        return;
    }

    uint32_t i;
    for (i = 0; i < s_event_count; i++) {
        const struct tpm_event *e = &s_events[i];

        /* Hex-encode the primary digest from the retained log. The parser
         * proved primary_digest_off + primary_digest_len <= log_size. Clamp
         * defensively so a corrupt metadata entry cannot overread. */
        char hex[129];
        uint32_t dl = e->primary_digest_len;
        if (dl > 64u) dl = 64u;
        if (e->primary_digest_off > log_size
            || dl > log_size - e->primary_digest_off)
            dl = 0u;
        uint32_t j;
        for (j = 0; j < dl; j++) {
            uint8_t b = log[e->primary_digest_off + j];
            hex[j * 2u]      = tpm_hexdig((uint8_t)(b >> 4));
            hex[j * 2u + 1u] = tpm_hexdig((uint8_t)(b & 0x0Fu));
        }
        hex[dl * 2u] = '\0';

        n = snprintf(chunk, sizeof(chunk),
                     "%s{\"pcrIndex\":%u,\"eventType\":%u,\"digestCount\":%u,"
                     "\"hashAlg\":\"%s\",\"digest\":\"%s\","
                     "\"eventPayloadOffset\":%u,\"eventSize\":%u}",
                     (i ? ",\n" : ""),
                     e->pcr_index, e->event_type, e->digest_count,
                     tpm_alg_name(e->primary_alg_id), hex,
                     e->payload_off, e->payload_size);
        if (n <= 0 || (uint32_t)n >= sizeof(chunk)
            || cel_append(f, acc, sizeof(acc), &alen, &woff, chunk, (uint32_t)n)) {
            klog(LOG_WARN, "TPM", "CEL export: write failed at event %u", i);
            vfs_close(f);
            return;
        }
    }

    n = snprintf(chunk, sizeof(chunk), "\n]}\n");
    if (n > 0 && (uint32_t)n < sizeof(chunk))
        cel_append(f, acc, sizeof(acc), &alen, &woff, chunk, (uint32_t)n);
    if (alen != 0u)     /* final flush of the coalescing buffer */
        vfs_write(f, woff, alen, (const uint8_t *)acc);
    vfs_close(f);
    klog(LOG_INFO, "TPM",
         "CEL event log exported to X:\\Diag\\tpm-events.json (%u events)",
         s_event_count);
}

/* ============================================================================
 * Boot Integrity Verification
 *
 * Stub implementation -- provides the framework and data structures for
 * boot chain verification.  Currently reports status as BOOT_INTEGRITY_NO_CRYPTO
 * because we lack the crypto primitives to replay PCR calculations.
 *
 * Full implementation roadmap:
 *
 *   Phase 1: PCR Event Log Summary (THIS -- done)
 *     - Build a boot_integrity_report from parsed event log data
 *     - Report TPM availability, version, event count, Secure Boot state
 *     - Mark all PCRs as NO_CRYPTO since we can't verify them yet
 *
 *   Phase 2: PCR Replay (requires SHA-256)
 *     - Replay the event log: for each event, hash the event data and
 *       extend the result into a running PCR accumulator
 *       (PCR_new = SHA-256(PCR_old || digest))
 *     - Compare replayed values against TPM PCR registers
 *     - This detects event log tampering (log says X, TPM says Y)
 *
 *   Phase 3: Golden Value Enrollment (requires secure storage)
 *     - First boot: save computed PCR values as "golden baseline"
 *     - Store in TPM NV index (tamper-resistant) or encrypted UEFI variable
 *     - Subsequent boots: compare current PCRs against stored golden values
 *     - Mismatch = firmware/bootloader/kernel was modified
 *
 *   Phase 4: FDE Key Sealing (requires TPM2_Seal/Unseal)
 *     - Seal disk encryption keys to PCR[0,4,7] state
 *     - TPM only releases keys if PCRs match sealed state
 *     - Foundation for BitLocker-style automatic unlock
 * ============================================================================ */

static struct boot_integrity_report s_integrity_report;

/* ---- PCR Read API (measured-boot PCR access) ---- */

/* Cache 2nd-dimension index for a supported hash bank; -1 if unsupported. */
static int tpm_bank_index(uint16_t alg)
{
    switch (alg) {
        case TPM_ALG_SHA1:   return 0;
        case TPM_ALG_SHA256: return 1;
        case TPM_ALG_SHA384: return 2;
        case TPM_ALG_SHA512: return 3;
        default:             return -1;
    }
}

tpm_pcr_status_t tpm2_pcr_read(uint16_t alg, uint32_t pcr_index,
                               uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!out || pcr_index >= 24u || tpm_bank_index(alg) < 0)
        return TPM_PCR_BADARG;
    /* An undersized output buffer is a CALLER bug, not a TPM fault. Reject it
     * up front with TPM_PCR_BADARG (before issuing a transaction) so both this
     * uncached path and the cached tpm_pcr_get() report the SAME status for the
     * same mistake -- never collapsing it into TPM_PCR_TRANSPORT. */
    if (out_cap < tpm_alg_digest_len_pub(alg))
        return TPM_PCR_BADARG;

    uint8_t cmd[20];
    uint32_t cmd_len = tpm2_build_pcr_read(cmd, sizeof(cmd), alg, pcr_index);
    if (cmd_len == 0u)
        return TPM_PCR_BADARG;

    /* A single-PCR TPM2_PCR_Read response is < 100 bytes (header + counter +
     * one selection + one digest); 128 is ample and avoids a large stack
     * buffer. tpm2_submit() rejects an over-cap response as malformed. */
    uint8_t rsp[128];
    int rlen = tpm2_submit(cmd, cmd_len, rsp, sizeof(rsp));
    if (rlen < 0) {
        /* A transient "another transaction in flight" is NOT a TPM fault: a
         * contended consumer (esp. an uncached tpm_pcr_get() fallback racing
         * another reader) must be able to distinguish it from a wedged/absent
         * TPM and retry, instead of misreading healthy contention as failure. */
        return (rlen == TPM_T_ERR_BUSY) ? TPM_PCR_BUSY : TPM_PCR_TRANSPORT;
    }

    int dlen = tpm2_parse_pcr_read(rsp, (uint32_t)rlen, alg, pcr_index,
                                   out, out_cap);
    if (dlen < 0)
        return TPM_PCR_TRANSPORT;
    if (dlen == 0)
        return TPM_PCR_INACTIVE;
    if (out_len) *out_len = (uint32_t)dlen;
    return TPM_PCR_OK;
}

/* ---- PCR cache (eager Phase-1 populate, lock-free read) ----
 *
 * Populated once on the BSP by tpm_pcr_cache_init() (Phase 1, single-threaded,
 * before APs/policy consumers run), then read-only -- so tpm_pcr_get() needs no
 * lock (same model as s_events). Eager (not lazy) avoids holding a lock across
 * the slow tpm2_submit() and avoids a scheduler dependency in early Phase 1.
 * Only the measured-boot PCRs are pre-cached; tpm_pcr_get() falls back to an
 * uncached tpm2_pcr_read() for any other (index, alg) so it stays correct for
 * every valid PCR (the fallback never writes the cache -> read-only invariant
 * preserved).
 *
 * Cost bound: the populate batch is up to ~|active banks| * |measured PCRs|
 * transactions, but a wedged/absent TPM trips the sticky s_failed flag on the
 * FIRST failed read (tpm2_submit() short-circuits every later call to
 * TPM_T_ERR_FAILED), so a dead TPM costs one timeout, not the whole batch. The
 * PCR-0 bank-activity probe result is reused (PCR 0 is not re-read). */
#define TPM_PCR_BANKS 4u
static const uint8_t s_pcr_measured[] = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 11u };
static const uint16_t s_pcr_bank_alg[TPM_PCR_BANKS] = {
    TPM_ALG_SHA1, TPM_ALG_SHA256, TPM_ALG_SHA384, TPM_ALG_SHA512
};
struct pcr_cache_entry {
    uint8_t  digest[64];
    uint8_t  len;
    uint8_t  status;   /* tpm_pcr_status_t */
    uint8_t  valid;    /* 0 = not populated */
};
static struct pcr_cache_entry s_pcr_cache[24][TPM_PCR_BANKS];

void tpm_pcr_cache_init(void)
{
    uint32_t b, k;
    for (b = 0; b < TPM_PCR_BANKS; b++) {
        uint16_t alg = s_pcr_bank_alg[b];
        /* Probe PCR 0 to learn whether this hash bank is active, and KEEP the
         * result as PCR 0's cache entry (no second read of PCR 0 below). */
        struct pcr_cache_entry *e0 = &s_pcr_cache[0][b];
        uint32_t p0len = 0;
        tpm_pcr_status_t probe = tpm2_pcr_read(alg, 0u, e0->digest,
                                               sizeof(e0->digest), &p0len);
        if (probe == TPM_PCR_TRANSPORT)
            return;             /* no TPM / transport down: leave cache empty */
        if (probe != TPM_PCR_OK)
            continue;           /* bank inactive (or bad-arg): skip */
        e0->status = (uint8_t)probe;
        e0->len    = (uint8_t)p0len;
        e0->valid  = 1u;
        for (k = 0; k < sizeof(s_pcr_measured); k++) {
            uint32_t idx = s_pcr_measured[k];
            struct pcr_cache_entry *e;
            uint32_t dl = 0;
            tpm_pcr_status_t st;
            if (idx == 0u)
                continue;       /* already populated from the activity probe */
            e  = &s_pcr_cache[idx][b];
            st = tpm2_pcr_read(alg, idx, e->digest, sizeof(e->digest), &dl);
            e->status = (uint8_t)st;
            e->len    = (st == TPM_PCR_OK) ? (uint8_t)dl : 0u;
            e->valid  = 1u;
        }
    }
    klog(LOG_INFO, "TPM", "PCR cache populated (measured-boot PCRs, active banks)");
}

tpm_pcr_status_t tpm_pcr_get(uint32_t pcr_index, uint16_t alg,
                             uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
    int bank = tpm_bank_index(alg);
    if (out_len) *out_len = 0;
    if (!out || pcr_index >= 24u || bank < 0)
        return TPM_PCR_BADARG;

    struct pcr_cache_entry *e = &s_pcr_cache[pcr_index][bank];
    if (!e->valid)
        return tpm2_pcr_read(alg, pcr_index, out, out_cap, out_len);

    tpm_pcr_status_t st = (tpm_pcr_status_t)e->status;
    if (st == TPM_PCR_OK) {
        uint32_t i;
        if ((uint32_t)e->len > out_cap)
            return TPM_PCR_BADARG;
        for (i = 0; i < e->len; i++)
            out[i] = e->digest[i];
        if (out_len) *out_len = e->len;
    }
    return st;
}

boot_result_t tpm_integrity_init(void)
{
    uint32_t i;
    /* Zero the report */
    uint8_t *p = (uint8_t *)&s_integrity_report;
    for (i = 0; i < (uint32_t)sizeof(s_integrity_report); i++)
        p[i] = 0;

    s_integrity_report.event_count = s_event_count;
    s_integrity_report.tpm_version = (uint8_t)s_version;

    /* Live Secure Boot state. Declared extern here because tpm.c deliberately
     * does not include uefi_runtime.h (circular deps). NEVER collapse an
     * unreadable SB state into "off": secure_boot is 1 only when the state is
     * readable AND active; secure_boot_valid records readability so a consumer
     * can tell genuine "off" from "unknown". (UEFI runtime + secureboot init
     * run before this in the boot sequence.) */
    {
        extern int uefi_secureboot_state_valid(void);
        extern int uefi_secureboot_enabled(void);
        int sbv = uefi_secureboot_state_valid();
        s_integrity_report.secure_boot_valid = (uint8_t)(sbv ? 1 : 0);
        s_integrity_report.secure_boot =
            (uint8_t)((sbv && uefi_secureboot_enabled()) ? 1 : 0);
    }

    /* ---- No TPM: cannot verify ---- */
    if (!s_available) {
        s_integrity_report.overall_status = BOOT_INTEGRITY_NO_TPM;
        s_integrity_report.pcr_count = 0;
        klog(LOG_INFO, "TPM", "Boot integrity: skipped (no TPM)");
        return BOOT_DEGRADED;
    }

    /* ---- TPM present but no crypto stack for PCR replay ----
     *
     * TODO: When SHA-256 is available, implement:
     *   1. Walk the parsed event log entries
     *   2. For each entry, compute: PCR[i] = SHA-256(PCR[i] || event_digest)
     *   3. After replaying all events, compare computed PCR values
     *      against actual TPM PCR registers (via TPM2_PCR_Read command)
     *   4. If all match: BOOT_INTEGRITY_VERIFIED
     *   5. If any differ: BOOT_INTEGRITY_MISMATCH + flag which PCR
     *
     * For now, populate the report with what we know and mark as NO_CRYPTO. */

    s_integrity_report.pcr_count = 8;  /* PCR[0] through PCR[7] */
    for (i = 0; i < 8; i++) {
        s_integrity_report.pcrs[i].pcr_index = (uint8_t)i;
        s_integrity_report.pcrs[i].status = BOOT_INTEGRITY_NO_CRYPTO;
        s_integrity_report.pcrs[i].pad[0] = 0;
        s_integrity_report.pcrs[i].pad[1] = 0;
    }

    /* TODO Phase 3: Read golden PCR values from secure storage.
     * If no golden values are enrolled, set:
     *   s_integrity_report.overall_status = BOOT_INTEGRITY_NO_BASELINE;
     * If golden values exist and PCR replay matches, set:
     *   s_integrity_report.overall_status = BOOT_INTEGRITY_VERIFIED;
     * If golden values exist but mismatch, set:
     *   s_integrity_report.overall_status = BOOT_INTEGRITY_MISMATCH;
     *   s_integrity_report.pcrs[i].status = BOOT_INTEGRITY_MISMATCH; */

    s_integrity_report.overall_status = BOOT_INTEGRITY_NO_CRYPTO;

    klog(LOG_INFO, "TPM",
         "Boot integrity: pending (crypto stack required for PCR replay)");
    klog(LOG_INFO, "TPM",
         "Boot integrity: %u events measured, PCR[0-7] not yet verifiable",
         s_event_count);

    return BOOT_OK;
}

int tpm_integrity_verified(void)
{
    return s_integrity_report.overall_status == BOOT_INTEGRITY_VERIFIED;
}

const struct boot_integrity_report *tpm_integrity_report(void)
{
    return &s_integrity_report;
}

void tpm_integrity_set_rng_available(int available)
{
    s_integrity_report.tpm_rng_available = available ? 1u : 0u;
}
