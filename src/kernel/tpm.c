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
#include "kernel/boot_info.h"
#include "kernel/klog.h"

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

/* Hash algorithm IDs (TCG Algorithm Registry) */
#define TPM_ALG_SHA1    0x0004
#define TPM_ALG_SHA256  0x000B
#define TPM_ALG_SHA384  0x000C
#define TPM_ALG_SHA512  0x000D

/* Event type constants */
#define EV_NO_ACTION                0x00000003
#define EV_EFI_VARIABLE_BOOT        0x80000002
#define EV_EFI_BOOT_SERVICES_APP    0x80000003

/* ---- Module state ---- */
static int      s_available;
static int      s_version;
static uint32_t s_event_count;

boot_result_t tpm_init(void)
{
    s_available = 0;
    s_version = 0;
    s_event_count = 0;

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

    /* Parse the first entry -- must be EV_NO_ACTION with spec ID event */
    const struct tcg_pcr_event *first =
        (const struct tcg_pcr_event *)log;

    /* Containment check, overflow-safe: rearrange to subtraction so a
     * malformed event_data_size near UINT32_MAX cannot wrap the sum
     * back to a small value. Codex 2026-04-30 Round 2 finding: the
     * previous form sizeof(...) + event_data_size could wrap in 32-bit
     * and pass the > log_size check while the buffer was actually too
     * small. Order matters: check log_size >= header size FIRST so
     * the subtraction below cannot wrap. */
    if ((size_t)log_size < sizeof(struct tcg_pcr_event)) {
        klog(LOG_WARN, "TPM", "Event log smaller than header");
        return BOOT_DEGRADED;
    }
    if ((size_t)first->event_data_size
            > (size_t)log_size - sizeof(struct tcg_pcr_event)) {
        klog(LOG_WARN, "TPM", "Event log truncated");
        return BOOT_DEGRADED;
    }

    /* For TPM 2.0 crypto-agile logs, parse the spec ID event to get
     * hash algorithm sizes -- needed to walk TCG_PCR_EVENT2 entries */
    uint32_t total_digest_size = 20;  /* default: SHA-1 only */
    (void)total_digest_size;  /* used for future PCR replay verification */
    uint32_t num_algs = 0;
    const char *hash_name = "SHA-1";

    if (s_version == 2 && first->event_type == EV_NO_ACTION &&
        first->event_data_size >= sizeof(struct tcg_spec_id_event)) {

        const struct tcg_spec_id_event *spec =
            (const struct tcg_spec_id_event *)(log + sizeof(struct tcg_pcr_event));

        num_algs = spec->number_of_algorithms;

        /* Cap the iteration count at the loop's hard limit (8). */
        uint32_t algs_to_read = num_algs;
        if (algs_to_read > 8u)
            algs_to_read = 8u;

        /* Bounds gate: each algorithm entry is 4 bytes (uint16 alg_id +
         * uint16 digest_size) trailing the spec_id_event header. The
         * cap-present check at the if-line above only confirmed the
         * header fits; a malformed cap-present log can still claim
         * number_of_algorithms = N while the buffer holds only the
         * header. Codex 2026-04-30 finding: read past first event
         * payload / past log buffer. Validate the advertised table
         * before touching alg_ptr.
         *
         * Overflow-safe: algs_to_read is at most 8 (capped above), so
         * algs_to_read * 4 is at most 32 -- no multiplication wrap on
         * any uint32_t. The addition uses size_t and cannot overflow
         * because both addends are bounded by struct sizes. */
        size_t needed = sizeof(struct tcg_spec_id_event)
                      + (size_t)algs_to_read * 4u;
        if ((size_t)first->event_data_size < needed) {
            klog(LOG_WARN, "TPM",
                 "Event log spec event truncated: data_size=%u < needed=%lu (num_algs=%u); skipping algorithm parse",
                 (uint64_t)first->event_data_size,
                 (uint64_t)needed,
                 (uint64_t)num_algs);
            algs_to_read = 0u;
        }

        /* Calculate total digest size from algorithm list */
        const uint8_t *alg_ptr = (const uint8_t *)(spec + 1);
        total_digest_size = 0;
        uint32_t i;
        for (i = 0; i < algs_to_read; i++) {
            uint16_t alg_id = *(const uint16_t *)(alg_ptr + i * 4);
            uint16_t digest_sz = *(const uint16_t *)(alg_ptr + i * 4 + 2);
            total_digest_size += digest_sz;

            /* Pick the strongest algorithm name for logging */
            if (alg_id == TPM_ALG_SHA384)      hash_name = "SHA-384";
            else if (alg_id == TPM_ALG_SHA256)  hash_name = "SHA-256";
            else if (alg_id == TPM_ALG_SHA512)  hash_name = "SHA-512";
        }
    }

    /* Walk the event log and count entries.
     * First entry is TCG_PCR_EVENT format (always).
     * Remaining entries are TCG_PCR_EVENT2 format for TPM 2.0. */
    /* Safe to compute as uint32_t now that the containment checks
     * above proved event_data_size + sizeof(tcg_pcr_event) fits in
     * the (possibly larger) log_size; the sum still fits in uint32_t
     * because sizeof(tcg_pcr_event) is small and event_data_size has
     * already been bound-checked against log_size. */
    uint32_t offset = (uint32_t)sizeof(struct tcg_pcr_event)
                    + first->event_data_size;
    uint32_t event_count = 1;  /* count the first entry */

    /* Containment pattern: every check is "required <= log_size -
     * offset" form so a malformed log claiming near-4GiB sizes cannot
     * wrap a uint32_t addition past the bound. Codex 2026-04-30
     * Round 3 finding: prior `offset + N < log_size` chain wraps when
     * offset or N approaches UINT32_MAX. log_size is uint32_t per the
     * boot_info contract. */
    if (s_version == 2) {
        /* TCG_PCR_EVENT2 format:
         * uint32_t pcr_index
         * uint32_t event_type
         * TPML_DIGEST_VALUES { uint32_t count; TPMT_HA[count] }
         * uint32_t event_data_size
         * uint8_t  event_data[] */
        while (offset < log_size && (uint32_t)(log_size - offset) >= 12u) {
            /* Skip pcr_index (4) + event_type (4) */
            uint32_t digest_count = *(const uint32_t *)(log + offset + 8);
            if (digest_count > 8) break;  /* sanity check */

            /* Walk digests: count field (4) + sum of (alg_id(2) + digest).
             * After the count field, dptr is at log + offset + 12. */
            uint32_t digests_size = 4;  /* count field */
            uint32_t dpos = offset + 12u;  /* offset is bounded; +12 fits u32 */
            uint32_t d;
            int oob = 0;
            for (d = 0; d < digest_count; d++) {
                /* Need at least 2 bytes for alg_id at dpos. */
                if (dpos > log_size || (log_size - dpos) < 2u) { oob = 1; break; }
                uint16_t alg_id = *(const uint16_t *)(log + dpos);
                uint16_t dsz = 0;
                /* Look up digest size from algorithm ID */
                if (alg_id == TPM_ALG_SHA1) dsz = 20;
                else if (alg_id == TPM_ALG_SHA256) dsz = 32;
                else if (alg_id == TPM_ALG_SHA384) dsz = 48;
                else if (alg_id == TPM_ALG_SHA512) dsz = 64;
                else dsz = 32;  /* unknown -- guess SHA-256 */
                /* Need 2 + dsz bytes at dpos. */
                if ((log_size - dpos) < (uint32_t)(2u + dsz)) { oob = 1; break; }
                digests_size += 2u + dsz;
                dpos += 2u + dsz;
            }
            if (oob) goto done;

            /* event2_header = 8 (pcr+type) + digests_size. digests_size
             * is bounded by the per-iteration check above, so the sum
             * cannot wrap u32 (max digest_count=8, max dsz=64 each;
             * worst-case digests_size = 4 + 8*(2+64) = 532). */
            uint32_t event2_header = 8u + digests_size;

            /* Need event2_header + 4 bytes (ev_data_size field) at offset. */
            if (event2_header > log_size - offset
                || 4u > log_size - offset - event2_header) break;

            uint32_t ev_data_size =
                *(const uint32_t *)(log + offset + event2_header);
            /* entry_size = header + 4 (length field) + ev_data_size.
             * ev_data_size is attacker-controlled u32; check via
             * subtraction. */
            if (ev_data_size > log_size - offset - event2_header - 4u) break;
            uint32_t entry_size = event2_header + 4u + ev_data_size;

            event_count++;
            offset += entry_size;
        }
    } else {
        /* TCG 1.2 format -- all entries are TCG_PCR_EVENT (32-byte
         * fixed header + variable event_data). */
        while (offset < log_size && (log_size - offset) >= 32u) {
            uint32_t ev_data_size = *(const uint32_t *)(log + offset + 28);
            /* Need 32 + ev_data_size bytes at offset; subtract to
             * avoid wrap. */
            if (ev_data_size > log_size - offset - 32u) break;
            uint32_t entry_size = 32u + ev_data_size;
            event_count++;
            offset += entry_size;
        }
    }

done:
    s_event_count = event_count;

    klog(LOG_INFO, "TPM", "TPM %s detected, %s, %u boot events measured",
         s_version == 2 ? "2.0" : "1.2", hash_name, event_count);

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

boot_result_t tpm_integrity_init(void)
{
    uint32_t i;
    /* Zero the report */
    uint8_t *p = (uint8_t *)&s_integrity_report;
    for (i = 0; i < (uint32_t)sizeof(s_integrity_report); i++)
        p[i] = 0;

    s_integrity_report.event_count = s_event_count;
    s_integrity_report.tpm_version = (uint8_t)s_version;

    /* Check Secure Boot state (from.1) */
    /* Forward declaration not needed -- we call uefi_secureboot_enabled()
     * via its extern linkage.  But since we don't include uefi_runtime.h
     * here to avoid circular deps, we just check boot_info. */
    s_integrity_report.secure_boot = 0;  /* Updated below if SB detection ran */

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
