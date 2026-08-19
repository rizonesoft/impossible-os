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
#include "kernel/tpm_pcr_alloc.h"   /* tpm_pcr_baseline_pcrs (canonical measured set) */
#include "kernel/tpm_baseline.h"    /* TPM_BASELINE_MAX_PCRS (report-size invariant) */
#include "kernel/tpm_replay.h"      /* TPM_REPLAY_TAMPER (the pinned verdict) */
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "kernel/fs/vfs.h"
#include "kernel/sched/spinlock.h"  /* boot-integrity report publication lock */
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
#ifdef KERNEL_TESTS
/* Test-only queryable copy of the first-rejection offset. The offset is also
 * emitted on the production serial path (see tpm_init) and returned by the pure
 * tpm_evlog_parse() out-param, so the production diagnostic is unaffected;
 * only the caller-less accessor is guarded out of release (release test-surface exclusion). */
static uint32_t          s_evlog_fail_offset;
#endif

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
                            uint32_t digests_off, uint8_t legacy)
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
    e->legacy             = legacy;
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
                         TPM_ALG_SHA1, 20u, 8u, 32u, first_dsize, 8u, 1u /*legacy*/)) {
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
                uint16_t dsz = tpm_alg_digest_len_pub(alg_id);
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
                                 offset + 12u, 0u /*EVENT2 list*/)) {
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
                                 offset + 8u, payload_off, ev_data_size, offset + 8u,
                                 1u /*legacy*/)) {
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
#ifdef KERNEL_TESTS
    s_evlog_fail_offset = 0;
#endif

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
#ifdef KERNEL_TESTS
    s_evlog_fail_offset = foff;
#endif
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

#ifdef KERNEL_TESTS
uint32_t tpm_evlog_fail_offset(void)
{
    return s_evlog_fail_offset;
}
#endif

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
    if (dir)
        vfs_close(dir);
    /* O_TRUNC: a shorter event log (fewer events than a prior boot) must not
     * leave stale previous-boot tail bytes after the freshly written JSON. */
    struct vfs_node *f = vfs_open("X:\\Diag\\tpm-events.json",
                                  VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
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
 * The report is assembled across two boot phases and published as one immutable
 * snapshot per update (see the publication block below).
 *
 *   Phase 0 (tpm_integrity_init, here): what the parsed event log and the UEFI
 *     runtime can tell us with no TPM transport -- TPM presence and version,
 *     event count, live Secure Boot state -- plus the measured-boot PCR set
 *     seeded NO_CRYPTO, because neither the transport nor the PCR cache exists
 *     yet and nothing is verifiable.
 *
 *   Phase 1 (elsewhere, published back through the setters here): the PCR
 *     replay-vs-hardware verdict (tpm_replay.c, event-log tamper detection) and
 *     the golden-baseline verdict with its per-PCR detail (tpm_baseline.c,
 *     reading the enrolled blob from a TPM NV index).
 *
 * Ranking between the two is fixed: an event-log TAMPER verdict outranks any
 * baseline result, because a log that does not replay to the hardware PCRs is a
 * definitive failure whatever the golden values say.
 * ============================================================================ */

/* ---- Boot-integrity report publication ----
 *
 * The report is published as an IMMUTABLE SNAPSHOT swapped in by release store,
 * never mutated field-by-field in place. It used to be one static struct that
 * every writer poked directly, which was safe only because all four writers
 * happened to run single-threaded on the BSP before the APs came up -- an
 * accident of init ordering rather than a stated contract. The readers wired
 * today are the attestation report builder (tpm_attest_report.c), the baseline
 * snapshot, and a Phase-1 boot log line; the UI and VPD consumers the report
 * was designed for are not wired yet, and none of them would carry that
 * single-threaded guarantee when they are.
 *
 * Two slots are enough, and the reason is the reader protocol rather than the
 * slot count: a reader holds s_publish_lock across BOTH the pointer load and
 * the copy, so no reader can still be inside a slot once it drops the lock, and
 * the writer's next publication always has the non-published slot free. A
 * lock-free reader would need more (hazard pointers or an RCU grace period),
 * because acquiring a pointer and then pinning it leaves a window in which the
 * writer swaps and reuses the buffer underneath.
 *
 * Writers serialize on the SAME lock and follow base-load -> mutate -> publish
 * inside one critical section. Safe readers alone would not be enough: the
 * setters mutate independent fields, so two writers each rebuilding off the
 * snapshot they read would let a slower baseline writer publishing VERIFIED
 * discard a replay-tamper writer's MISMATCH -- every published snapshot would
 * stay internally coherent while the report said VERIFIED on a tampered boot.
 *
 * No blocking work, TPM transaction or serial output may sit inside the
 * publication region; callers build their inputs first and publish last. */
/* THE LOCK IS THE SAFETY PROPERTY, NOT THE SLOT COUNT OR THE ATOMICS. Under the
 * shipped reader protocol -- tpm_integrity_report_copy holds s_publish_lock
 * across BOTH the pointer load and the copy -- a single buffer would be equally
 * safe, so the second slot and the ACQUIRE/RELEASE pair add no protection
 * TODAY. They are kept because they are what makes the published value an
 * immutable snapshot rather than a mutable struct, which is the property a
 * future lock-free or RCU reader would need and which is impossible to retrofit
 * onto in-place mutation. Do NOT read the release-store as licence to load the
 * pointer without the lock: two slots do not bound how long a reader may hold
 * one, so an unlocked reader can still be overtaken. Going lock-free requires
 * hazard pointers or an RCU grace period first (see tpm.h). */
static struct boot_integrity_report s_report_slots[2];
static struct boot_integrity_report *s_report_published;   /* NULL until first publish */
static uint8_t s_report_next_slot;                         /* index of the free slot */
static spinlock_t s_publish_lock = SPINLOCK_INIT;

/* The reported PCR set is the measured-boot set, so the two array bounds are
 * one invariant expressed in two headers. tpm.c is the only translation unit
 * that sees both (tpm.h deliberately does not include tpm_baseline.h). */
_Static_assert(BOOT_INTEGRITY_MAX_PCRS == TPM_BASELINE_MAX_PCRS,
               "boot_integrity_report.pcrs[] must hold the whole measured-boot "
               "PCR set that the baseline pins");

/* Snapshot the currently published report into `out`. Caller holds s_publish_lock.
 * Before the first publication there is no snapshot, so the base is all-zero --
 * which reads as BOOT_INTEGRITY_UNKNOWN, the honest "not evaluated yet" state. */
static void integrity_base_locked(struct boot_integrity_report *out)
{
    const struct boot_integrity_report *cur =
        __atomic_load_n(&s_report_published, __ATOMIC_ACQUIRE);
    if (cur)
        memcpy(out, cur, sizeof(*out));
    else
        memset(out, 0, sizeof(*out));
}

/* Publish `next` as the new snapshot. Caller holds s_publish_lock.
 * The release store is the publication point and is written LAST, so a reader
 * that observes the new pointer observes a fully written slot. */
static void integrity_publish_locked(const struct boot_integrity_report *next)
{
    struct boot_integrity_report *slot = &s_report_slots[s_report_next_slot];
    memcpy(slot, next, sizeof(*slot));
    /* NORMALIZE the reported count at the publication boundary, so no consumer
     * can be handed a pcr_count larger than pcrs[] actually holds. Every WRITE
     * path already clamps, but the count itself was passed through verbatim --
     * and a reader's natural `for (i = 0; i < r.pcr_count; i++)` idiom over its
     * own struct would then over-read its stack copy. Clamping here makes the
     * published count trustworthy for every reader instead of asking each one to
     * re-derive the bound.
     *
     * The clamp also INVALIDATES a VERIFIED verdict rather than silently making
     * an out-of-range count look well-formed. A producer that reports 255 PCRs
     * is corrupt, and the fact that its slot contents happen to survive
     * truncation is no reason to trust the claim it attached to them -- a silent
     * clamp would launder exactly that corruption into a credible integrity
     * assertion. */
    if (slot->pcr_count > (uint8_t)BOOT_INTEGRITY_MAX_PCRS) {
        slot->pcr_count = (uint8_t)BOOT_INTEGRITY_MAX_PCRS;
        if (slot->overall_status == (uint8_t)BOOT_INTEGRITY_VERIFIED)
            slot->overall_status = (uint8_t)BOOT_INTEGRITY_UNKNOWN;
    }
    s_report_next_slot = (uint8_t)(s_report_next_slot ^ 1u);
    __atomic_store_n(&s_report_published, slot, __ATOMIC_RELEASE);
}

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
        uint8_t measured[24];
        uint8_t nmeasured = tpm_pcr_baseline_pcrs(measured, sizeof measured);
        for (k = 0; k < nmeasured; k++) {
            uint32_t idx = measured[k];
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

int tpm_integrity_build_report(struct boot_integrity_report *out,
                               int tpm_present, uint8_t version,
                               uint32_t events, int sb_valid, int sb_enabled,
                               const uint8_t *set, uint8_t n)
{
    uint8_t i;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    out->event_count = events;
    out->tpm_version = version;

    /* NEVER collapse an unreadable Secure Boot state into "off": secure_boot is
     * 1 only when the state is readable AND active, and secure_boot_valid
     * records the readability, so a consumer can tell genuine "off" from
     * "unknown". */
    out->secure_boot_valid = (uint8_t)(sb_valid ? 1 : 0);
    out->secure_boot = (uint8_t)((sb_valid && sb_enabled) ? 1 : 0);

    if (!tpm_present) {
        /* No TPM: cannot verify, and no PCR is reportable at all. */
        out->overall_status = BOOT_INTEGRITY_NO_TPM;
        out->pcr_count = 0;
        return 1;
    }

    /* tpm_pcr_baseline_pcrs() returns the TOTAL matching count, NOT the number
     * it wrote -- it fills only while total < cap (tpm_pcr_alloc.c). So a
     * BASELINE policy mask that outgrew the caller's array returns n > capacity,
     * and iterating to n would read past `set` and write past `out->pcrs`.
     *
     * The test is EXACT EQUALITY, not just an upper bound, and matches
     * tpm_baseline_snapshot() (npcr != TPM_BASELINE_MAX_PCRS -> BADARG). An
     * asymmetric guard would let a mask that LOST a PCR publish a quietly
     * reduced set here while the baseline layer refused it as BADARG -- two
     * subsystems disagreeing about what the measured set is, which is exactly
     * the drift the sizing exists to prevent. Either direction fails CLOSED: no
     * PCRs reported and the verdict left UNKNOWN, so a report that cannot
     * represent the measured set never reads as verified. */
    if (!set || n != (uint8_t)BOOT_INTEGRITY_MAX_PCRS) {
        out->pcr_count = 0;
        out->overall_status = BOOT_INTEGRITY_UNKNOWN;
        return 0;
    }

    /* TPM present, but Phase 0 has neither the transport nor the PCR cache, so
     * nothing is verifiable yet. The Phase-1 baseline verify replaces every one
     * of these through tpm_integrity_publish_baseline(). */
    for (i = 0; i < n; i++) {
        out->pcrs[i].pcr_index = set[i];
        out->pcrs[i].status = BOOT_INTEGRITY_NO_CRYPTO;
    }
    out->pcr_count = n;
    out->overall_status = BOOT_INTEGRITY_NO_CRYPTO;
    return 1;
}

boot_result_t tpm_integrity_init(void)
{
    /* Built OFF-lock into a local, published in one short critical section at
     * the end. The Secure Boot reads below are UEFI runtime calls and must not
     * happen inside the publication region. */
    struct boot_integrity_report r;
    uint64_t flags;
    boot_result_t res = BOOT_OK;
    int no_tpm;
    int set_drift = -1;   /* >= 0: measured-set size that disagrees with the report */

    /* Sample the live inputs here; the construction RULES live in the pure
     * builder above so they can be tested without calling this function. */
    no_tpm = !s_available;
    {
        /* Live Secure Boot state. Declared extern here because tpm.c
         * deliberately does not include uefi_runtime.h (circular deps). The
         * builder holds the rule that an unreadable state never collapses to
         * "off". (UEFI runtime + secureboot init run before this in the boot
         * sequence.) */
        extern int uefi_secureboot_state_valid(void);
        extern int uefi_secureboot_enabled(void);
        uint8_t set[BOOT_INTEGRITY_MAX_PCRS];
        uint8_t n = 0;
        int sbv = uefi_secureboot_state_valid();

        if (!no_tpm)
            n = tpm_pcr_baseline_pcrs(set, (uint8_t)BOOT_INTEGRITY_MAX_PCRS);

        if (!tpm_integrity_build_report(&r, !no_tpm, (uint8_t)s_version,
                                        s_event_count, sbv,
                                        uefi_secureboot_enabled(), set, n))
            set_drift = n;
    }
    /* A measured-set drift degrades the SUBSYSTEM too, not just the report.
     * Returning BOOT_OK there let boot_hw.c compute worst == BOOT_OK and mark
     * SUBSYS_TPM fully ready while the report itself said UNKNOWN -- a
     * subsystem status contradicting the thing it owns. */
    if (no_tpm || set_drift >= 0)
        res = BOOT_DEGRADED;

    /* ENFORCE the "FIRST snapshot" contract rather than only documenting it.
     * This is the one writer that publishes WITHOUT base-loading, because there
     * is nothing to base-load from; that also means a second call would wipe a
     * published replay-TAMPER pin and the RNG-availability flag. The other
     * writers are idempotent by construction (they base-load); this one is not,
     * so it says so in code. */
    spin_lock_irqsave(&s_publish_lock, &flags);
    if (__atomic_load_n(&s_report_published, __ATOMIC_ACQUIRE) == (void *)0)
        integrity_publish_locked(&r);
    else
        res = BOOT_DEGRADED;
    spin_unlock_irqrestore(&s_publish_lock, flags);

    /* Logging is deliberately OUTSIDE the publication region -- serial output is
     * millisecond-scale and must never sit inside a spinlock. */
    if (no_tpm) {
        klog(LOG_INFO, "TPM", "Boot integrity: skipped (no TPM)");
    } else if (set_drift >= 0) {
        /* A build-configuration error, not a runtime condition: the PCR policy
         * table changed size and BOOT_INTEGRITY_MAX_PCRS did not follow it.
         * Loud, and the report says UNKNOWN rather than claiming a set it
         * cannot faithfully represent in either direction. */
        klog(LOG_ERROR, "TPM",
             "Boot integrity: measured-boot set is %u PCRs but the report is "
             "sized %u -- reporting UNKNOWN (resize BOOT_INTEGRITY_MAX_PCRS)",
             (uint32_t)set_drift, (uint32_t)BOOT_INTEGRITY_MAX_PCRS);
    } else {
        klog(LOG_INFO, "TPM",
             "Boot integrity: pending (crypto stack required for PCR replay)");
        klog(LOG_INFO, "TPM",
             "Boot integrity: %u events measured, %u measured-boot PCRs not yet "
             "verifiable", s_event_count, (uint32_t)r.pcr_count);
    }

    return res;
}

int tpm_integrity_baseline_verified(void)
{
    struct boot_integrity_report r;
    tpm_integrity_report_copy(&r);
    /* Tamper is checked FIRST, exactly as tpm_integrity_status_label does.
     * The two answer the same question -- one for a reader, one for a caller --
     * and they must not be able to disagree. Today they cannot in production,
     * because tpm_integrity_set_replay_verdict pins MISMATCH inside the
     * publication lock, so this is defence in depth rather than a live fix:
     * it costs one comparison and removes the possibility that some future
     * publisher constructs the VERIFIED-plus-TAMPER combination and hands a
     * programmatic caller a success it would never have read off the label.
     * A test builds exactly that report, which is how the divergence surfaced. */
    if (r.replay_verdict == (uint8_t)TPM_REPLAY_TAMPER)
        return 0;
    return r.overall_status == BOOT_INTEGRITY_VERIFIED;
}

void tpm_integrity_report_copy(struct boot_integrity_report *out)
{
    uint64_t flags;
    if (!out)
        return;
    /* The lock spans the pointer LOAD and the COPY. That span is the whole
     * point: it is what makes acquisition safe, so no reader can be paused
     * mid-copy while a writer swaps the slot out from under it. */
    spin_lock_irqsave(&s_publish_lock, &flags);
    integrity_base_locked(out);
    spin_unlock_irqrestore(&s_publish_lock, flags);
}

const char *tpm_integrity_status_label(const struct boot_integrity_report *r)
{
    if (!r)
        return "unknown";
    /* A zero-initialized or not-yet-evaluated report has overall_status ==
     * BOOT_INTEGRITY_UNKNOWN. Report it as "unknown" rather than inferring
     * no-TPM from tpm_version == 0 -- a pre-init report also has version 0, so
     * the version field cannot distinguish "not checked" from "no hardware".
     * The authoritative no-TPM signal is overall_status == BOOT_INTEGRITY_NO_TPM,
     * which tpm_integrity_init() sets only after probing for a TPM. */
    if (r->overall_status == BOOT_INTEGRITY_UNKNOWN)
        return "unknown";
    /* event-log tamper (replay != hardware) is checked BEFORE the baseline
     * status so the TAMPER->MISMATCH escalation is reported as tamper, not as a
     * generic baseline mismatch. */
    if (r->replay_verdict == (uint8_t)TPM_REPLAY_TAMPER)
        return "event-log-tamper";
    switch (r->overall_status) {
        /* "baseline-verified", never a bare "verified": the subject is the
         * enrolled baseline, and the bare word invites a reader to conclude
         * the kernel image was measured. Nothing in the baseline hashes it. */
        case BOOT_INTEGRITY_VERIFIED:    return "baseline-verified";
        case BOOT_INTEGRITY_MISMATCH:    return "baseline-mismatch";
        case BOOT_INTEGRITY_NO_TPM:      return "no-TPM";
        case BOOT_INTEGRITY_NO_BASELINE: return "no-baseline";
        case BOOT_INTEGRITY_NO_CRYPTO:   return "no-crypto";
        default:                         return "unknown";
    }
}

/* DELIBERATELY TERSE. Each of these is concatenated into the Phase-1 boot log
 * line, and serial_write holds its lock with interrupts off for the whole
 * string -- at 115200 baud that is ~87 us per character, so the first draft's
 * 96-188 byte sentences cost 8-17 ms of interrupt-off wire time on every boot
 * and could suppress a 100 Hz timer tick. Every load-bearing claim survives
 * the trim (the kernel IMAGE exclusion, the three MISMATCH kinds, "could not
 * be compared" rather than "nothing was measured"); the prose that went is
 * prose. Do not re-expand these for readability -- a longer explanation
 * belongs in a UI or a diagnostics dump, not on the boot wire. */
int tpm_integrity_render_mismatched_pcrs(const struct boot_integrity_report *r,
                                         char *out, uint32_t cap)
{
    uint32_t w = 0;
    uint8_t k, n;
    int truncated = 0;
    int all_compared = 1;

    if (!out || cap == 0u)
        return -1;
    out[0] = '\0';
    if (!r)
        return -1;

    n = r->pcr_count;
    if (n > (uint8_t)BOOT_INTEGRITY_MAX_PCRS)
        n = (uint8_t)BOOT_INTEGRITY_MAX_PCRS;

    /* "NO PCR DIFFERS" MUST BE EARNED BY EVERY SLOT, not by any slot. Two
     * weaker rules were tried and both produce an exculpatory lie:
     *
     *   count-based -- a Phase-0 report carries the FULL measured set with
     *     every slot NO_CRYPTO, so a nonzero count reported "no PCR differs"
     *     about a comparison that never ran;
     *   any-slot -- one VERIFIED slot beside eight UNKNOWN ones said the same
     *     thing about eight PCRs nobody checked.
     *
     * Only VERIFIED and MISMATCH mean a digest was actually compared.
     * NO_BASELINE does NOT: tpm_baseline_compare_pcrs writes it when the golden
     * pins nothing for that slot, and its own contract calls that boot value
     * UNVERIFIED rather than wrong. Anything else (UNKNOWN, NO_CRYPTO, NO_TPM,
     * or a value from outside the enum) is likewise not a comparison. */
    if (n == 0u)
        return -1;
    for (k = 0; k < n; k++) {
        uint8_t st = r->pcrs[k].status;
        if (st != BOOT_INTEGRITY_VERIFIED && st != BOOT_INTEGRITY_MISMATCH) {
            all_compared = 0;
            break;
        }
    }

    for (k = 0; k < n; k++) {
        uint8_t idx = r->pcrs[k].pcr_index;
        if (r->pcrs[k].status != BOOT_INTEGRITY_MISMATCH)
            continue;
        /* EXACT width for THIS entry, not a worst-case reserve. A blanket
         * "separator + two digits" bound wastes a byte on every single-digit
         * index and can drop a trailing entry that would have fitted, which
         * on this line means silently under-reporting which PCRs moved. */
        uint32_t need = (w ? 1u : 0u) + (idx >= 10u ? 2u : 1u);
        if (w + need > cap - 1u) {
            /* TRUNCATION IS ITS OWN ANSWER. Returning 1 with a short list
             * would present a partial set as the whole one, and returning 0
             * when even the FIRST index did not fit would claim nothing
             * differs. Either is a false statement on a security line. */
            truncated = 1;
            break;
        }
        if (w)
            out[w++] = ',';
        if (idx >= 10u)
            out[w++] = (char)('0' + (idx / 10u));
        out[w++] = (char)('0' + (idx % 10u));
    }
    out[w] = '\0';
    /* Truncation always means a mismatch was FOUND and did not fit, so it is
     * never the exculpatory answer even with an empty buffer. */
    if (truncated)
        return 2;
    /* Some slot was never compared, so a differing PCR could be hiding behind
     * it: report what was found, but never claim none differ. */
    if (!all_compared)
        return w ? 2 : -1;
    return w ? 1 : 0;
}

const char *tpm_integrity_status_scope(const struct boot_integrity_report *r)
{
    if (!r || r->overall_status == BOOT_INTEGRITY_UNKNOWN)
        return "no verdict has been computed yet";
    /* Ordered to match tpm_integrity_status_label so the two can never describe
     * different statuses for the same report. */
    if (r->replay_verdict == (uint8_t)TPM_REPLAY_TAMPER)
        return "event log disagrees with hardware PCRs";
    switch (r->overall_status) {
        case BOOT_INTEGRITY_VERIFIED:
            return "enrolled baseline only; the kernel IMAGE was not measured";
        case BOOT_INTEGRITY_MISMATCH:
            /* DELIBERATELY does not promise a field cause. This status is
             * published by THREE different kinds of failure: a comparison
             * that ran and disagreed, a stored baseline that failed its own
             * corruption or authenticity checks, and THIS KERNEL's own
             * read-only identity failing validation before any baseline was
             * even read. Only the first kind has a differing FIELD, so text
             * directing every reader to one would send two thirds of them
             * looking for evidence that does not exist. The third kind is the
             * one that must never be missed: enrollment refuses it today just
             * as it refuses the second, but the safety there rests entirely on
             * the kernel-identity check running, so the two must never share a
             * recovery path.
             *
             * Which kind it was is named on its own boot-log line, and every
             * MISMATCH-publishing status now has one. Distinguishing them
             * inside the report itself would need a provenance field, which
             * belongs with the report struct and its publication. */
            /* "integrity/authenticity", not authenticity alone: CORRUPT is a
             * magic/version/size/CRC failure of the stored blob, which is
             * integrity, and dropping the word made this line false for a
             * reachable MISMATCH producer. */
            return "baseline differs, or stored-baseline integrity/authenticity, "
                   "or this kernel's own identity; the log names which";
        case BOOT_INTEGRITY_NO_TPM:
            /* "could not be compared", NOT "nothing was measured": the event
             * count is filled from the pre-transport phase before the no-TPM
             * branch is taken, so a NO_TPM report can legitimately carry a
             * nonzero one and a claim that nothing was measured contradicts
             * the report a reader is holding. */
            return "no usable TPM; the baseline could not be compared";
        case BOOT_INTEGRITY_NO_BASELINE:
            return "nothing enrolled, so nothing to compare against";
        case BOOT_INTEGRITY_NO_CRYPTO:
            return "crypto or transport unavailable at that phase";
        default:
            return "no verdict has been computed yet";
    }
}

void tpm_integrity_set_rng_available(int available)
{
    struct boot_integrity_report r;
    uint64_t flags;
    spin_lock_irqsave(&s_publish_lock, &flags);
    integrity_base_locked(&r);
    r.tpm_rng_available = available ? 1u : 0u;
    integrity_publish_locked(&r);
    spin_unlock_irqrestore(&s_publish_lock, flags);
}

void tpm_integrity_set_replay_verdict(uint8_t verdict)
{
    struct boot_integrity_report r;
    uint64_t flags;
    spin_lock_irqsave(&s_publish_lock, &flags);
    integrity_base_locked(&r);
    r.replay_verdict = verdict;
    /* A replay/hardware mismatch (event-log tamper) is a definitive integrity
     * failure regardless of any golden baseline -- escalate the overall status. */
    if (verdict == (uint8_t)TPM_REPLAY_TAMPER)
        r.overall_status = BOOT_INTEGRITY_MISMATCH;
    integrity_publish_locked(&r);
    spin_unlock_irqrestore(&s_publish_lock, flags);
}

#ifdef KERNEL_TESTS
void tpm_integrity_test_republish(const struct boot_integrity_report *r)
{
    uint64_t flags;
    if (!r)
        return;
    spin_lock_irqsave(&s_publish_lock, &flags);
    integrity_publish_locked(r);
    spin_unlock_irqrestore(&s_publish_lock, flags);
}
#endif /* KERNEL_TESTS */

void tpm_integrity_publish_baseline(uint8_t status,
                                    const uint8_t *pcr_status, uint8_t n)
{
    struct boot_integrity_report r;
    uint64_t flags;
    uint8_t i;

    spin_lock_irqsave(&s_publish_lock, &flags);
    integrity_base_locked(&r);

    /* Per-PCR detail is refreshed FIRST and unconditionally, so it is applied
     * even on the TAMPER path below that pins the overall verdict. Pinning the
     * overall status is a statement about the verdict, not a reason to keep
     * reporting stale per-PCR values.
     *
     * n == 0 means this verify path never reached the comparison (no baseline,
     * a corrupt blob, a corrupt kernel descriptor). The honest per-PCR answer
     * there is NOT-EVALUATED (BOOT_INTEGRITY_UNKNOWN) rather than the stale
     * Phase-0 NO_CRYPTO, which would read as "the crypto stack is missing" long
     * after it arrived. The bound is defensive: `n` is caller-supplied. */
    if (n > (uint8_t)BOOT_INTEGRITY_MAX_PCRS)
        n = (uint8_t)BOOT_INTEGRITY_MAX_PCRS;
    for (i = 0; i < r.pcr_count && i < (uint8_t)BOOT_INTEGRITY_MAX_PCRS; i++)
        r.pcrs[i].status = (pcr_status && i < n)
                               ? pcr_status[i]
                               : (uint8_t)BOOT_INTEGRITY_UNKNOWN;

    /* The Phase-1 baseline verify (tpm_baseline_verify) publishes its golden-vs-
     * current verdict here. A replay/hardware TAMPER (replay_verdict == 1) is a
     * definitive integrity failure that pins MISMATCH -- NO later baseline status
     * (VERIFIED, NO_BASELINE, ...) may overwrite it, or an overall_status
     * consumer would lose the tamper signal. This guard is INSIDE the critical
     * section on purpose: reading replay_verdict off-lock and publishing later
     * would let a baseline writer that read a pre-tamper snapshot overwrite the
     * tamper verdict with VERIFIED, which is exactly the lost update that safe
     * readers alone do not prevent. */
    if (r.replay_verdict != (uint8_t)TPM_REPLAY_TAMPER)
        r.overall_status = status;

    /* COHERENCE: a VERIFIED verdict is a positive assertion about every PCR the
     * report carries, so it may not stand beside a slot that is not itself
     * VERIFIED. Without this, publish_baseline(VERIFIED, st, 4) would report
     * pcr_count=9 with five UNKNOWN slots and an overall VERIFIED -- the exact
     * verdict-vs-detail contradiction this publication path exists to remove,
     * reintroduced through a short detail array instead of through a torn read.
     * Downgrade to UNKNOWN rather than trusting the caller: an integrity report
     * that cannot back its own claim must not make it. Non-VERIFIED verdicts are
     * untouched -- MISMATCH or NO_BASELINE beside partial detail asserts nothing
     * that needs backing.
     *
     * The count test is EXACT EQUALITY against the measured set, not merely
     * "non-zero", and the difference is the whole guard. A loop bounded by
     * pcr_count passes VACUOUSLY on a zero-slot base, and passes WRONGLY on a
     * truncated one: a snapshot carrying 4 slots all marked VERIFIED would
     * publish VERIFIED while saying nothing about the other five measured PCRs.
     * An oversized count is equally unacceptable -- integrity_publish_locked
     * clamps it, which would otherwise launder a corrupt producer state into a
     * well-formed VERIFIED report. Only a report covering exactly the measured
     * set can carry the claim. */
    if (r.overall_status == (uint8_t)BOOT_INTEGRITY_VERIFIED) {
        if (r.pcr_count != (uint8_t)BOOT_INTEGRITY_MAX_PCRS) {
            r.overall_status = (uint8_t)BOOT_INTEGRITY_UNKNOWN;
        } else {
            for (i = 0; i < (uint8_t)BOOT_INTEGRITY_MAX_PCRS; i++)
                if (r.pcrs[i].status != (uint8_t)BOOT_INTEGRITY_VERIFIED) {
                    r.overall_status = (uint8_t)BOOT_INTEGRITY_UNKNOWN;
                    break;
                }
        }
    }

    integrity_publish_locked(&r);
    spin_unlock_irqrestore(&s_publish_lock, flags);
}
