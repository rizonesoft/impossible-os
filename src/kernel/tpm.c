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

    uint32_t first_entry_size =
        (uint32_t)sizeof(struct tcg_pcr_event) + first->event_data_size;

    if (first_entry_size > log_size) {
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

        /* Calculate total digest size from algorithm list */
        const uint8_t *alg_ptr = (const uint8_t *)(spec + 1);
        total_digest_size = 0;
        uint32_t i;
        for (i = 0; i < num_algs && i < 8; i++) {
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
    uint32_t offset = first_entry_size;
    uint32_t event_count = 1;  /* count the first entry */

    if (s_version == 2) {
        /* TCG_PCR_EVENT2 format:
         * uint32_t pcr_index
         * uint32_t event_type
         * TPML_DIGEST_VALUES { uint32_t count; TPMT_HA[count] }
         * uint32_t event_data_size
         * uint8_t  event_data[] */
        while (offset + 12 < log_size) {
            /* Skip pcr_index (4) + event_type (4) */
            uint32_t digest_count = *(const uint32_t *)(log + offset + 8);
            if (digest_count > 8) break;  /* sanity check */

            /* Skip past digests: count field (4) + sum of (alg_id(2) + digest) */
            uint32_t digests_size = 4;  /* count field */
            const uint8_t *dptr = log + offset + 12;
            uint32_t d;
            for (d = 0; d < digest_count; d++) {
                if ((uint32_t)(dptr - log) + 2 >= log_size) goto done;
                uint16_t alg_id = *(const uint16_t *)dptr;
                uint16_t dsz = 0;
                /* Look up digest size from algorithm ID */
                if (alg_id == TPM_ALG_SHA1) dsz = 20;
                else if (alg_id == TPM_ALG_SHA256) dsz = 32;
                else if (alg_id == TPM_ALG_SHA384) dsz = 48;
                else if (alg_id == TPM_ALG_SHA512) dsz = 64;
                else dsz = 32;  /* unknown -- guess SHA-256 */
                digests_size += 2 + dsz;
                dptr += 2 + dsz;
            }

            uint32_t event2_header = 8 + digests_size;
            if (offset + event2_header + 4 > log_size) break;

            uint32_t ev_data_size =
                *(const uint32_t *)(log + offset + event2_header);
            uint32_t entry_size = event2_header + 4 + ev_data_size;

            if (offset + entry_size > log_size) break;

            event_count++;
            offset += entry_size;
        }
    } else {
        /* TCG 1.2 format -- all entries are TCG_PCR_EVENT */
        while (offset + 32 < log_size) {
            uint32_t ev_data_size = *(const uint32_t *)(log + offset + 28);
            uint32_t entry_size = 32 + ev_data_size;
            if (offset + entry_size > log_size) break;
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
 * Boot Integrity Verification (§9.2)
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

    /* Check Secure Boot state (from §5.1) */
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
