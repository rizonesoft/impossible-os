/* ============================================================================
 * tpm.h -- TPM Measured Boot interface
 *
 * Parses the TCG event log passed from the bootloader and exposes TPM
 * availability, version, and boot event summary to the kernel.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_init.h"

/* Initialize TPM subsystem -- parse event log from boot_info.
 * Returns BOOT_OK on success, BOOT_DEGRADED if no TPM. */
boot_result_t tpm_init(void);

/* Returns 1 if a TPM was detected during boot. */
int tpm_available(void);

/* Returns TPM version: 0 = none, 1 = 1.2, 2 = 2.0. */
int tpm_version(void);

/* Returns the number of measured boot events in the event log. */
uint32_t tpm_event_count(void);

/* ---- TCG event-log parse result (measured-boot event-log hardening) ----
 *
 * The parser walks the bootloader-copied TCG log once, preserving per-event
 * metadata (PCR index, type, primary digest, payload span) and returning a
 * structured status. Truncation/corruption is reported with the exact failing
 * byte offset; on any non-OK status the metadata array is marked invalid and
 * event_count stays 0 (a malformed prefix never looks like a shorter valid
 * log). All multi-byte log fields are read with byte-load helpers, so a
 * misaligned firmware log cannot fault on strict-alignment cores.
 *
 * SMP: written once by tpm_init() (Phase-1 BSP, single-threaded) and read-only
 * thereafter, so the static metadata array needs no lock. */
typedef enum {
    TPM_EVLOG_OK           = 0,  /* clean parse to end-of-buffer */
    TPM_EVLOG_NO_LOG       = 1,  /* no log present / degraded by loader caps */
    TPM_EVLOG_BAD_HEADER   = 2,  /* first TCG_PCR_EVENT header malformed/short */
    TPM_EVLOG_BAD_SPEC_ID  = 3,  /* crypto-agile spec-ID event malformed */
    TPM_EVLOG_TRUNCATED    = 4,  /* an entry runs past the buffer end */
    TPM_EVLOG_CAP_EXCEEDED = 5,  /* more events than TPM_EVENT_MAX */
    TPM_EVLOG_UNSUPPORTED_ALG = 6, /* digest bank uses an unknown hash alg */
} tpm_evlog_status_t;

/* Fixed cap on preserved events. A static array (no attacker-sized alloc);
 * overflow is reported as TPM_EVLOG_CAP_EXCEEDED, not silently dropped. */
#define TPM_EVENT_MAX 256u

/* Per-event metadata. Digest/payload bytes are NOT copied -- the event log
 * buffer is retained by the legacy tpm_event_log boot_reserved reservation, so
 * offsets into it stay valid for later replay and CEL-JSON export. */
struct tpm_event {
    uint32_t pcr_index;
    uint32_t event_type;
    uint32_t digest_count;       /* number of digests in this event's bank list */
    uint16_t primary_alg_id;     /* TPM_ALG_* of the strongest digest present */
    uint8_t  primary_digest_len; /* 20/32/48/64 */
    uint8_t  pad;
    uint32_t primary_digest_off; /* byte offset of the primary digest in the log */
    uint32_t payload_off;        /* byte offset of event_data in the log */
    uint32_t payload_size;       /* event_data length */
};

/* Pure, side-effect-free TCG event-log parser (no globals, no klog) so it is
 * unit-testable against fixture buffers. Fills out[0..out_max) with per-event
 * metadata; *out_count = events recorded, *out_overflow = 1 if the log held
 * more than out_max events, *out_fail_offset = byte offset of the first
 * rejection. Returns the structured status (non-OK => malformed; the caller
 * must not trust the partial prefix). Out params may be NULL. */
tpm_evlog_status_t tpm_evlog_parse(const uint8_t *log, uint32_t log_size, int version,
                                   struct tpm_event *out, uint32_t out_max,
                                   uint32_t *out_count, uint32_t *out_overflow,
                                   uint32_t *out_fail_offset);

/* Structured parse status + the exact offset of the first rejection (0 when
 * status == TPM_EVLOG_OK). */
tpm_evlog_status_t tpm_evlog_status(void);
uint32_t           tpm_evlog_fail_offset(void);

/* Preserved event metadata. tpm_event_get() returns NULL for out-of-range i
 * or when the last parse failed. tpm_event_overflow() is 1 when the log held
 * more than TPM_EVENT_MAX events. */
const struct tpm_event *tpm_event_get(uint32_t i);
int                     tpm_event_overflow(void);

/* Export the parsed event log as a TCG CEL-JSON subset to
 * X:\Diag\tpm-events.json. The parse runs in Phase 0 (no filesystem); call
 * this from post-mount bring-up. No-op unless the last parse was clean. */
void tpm_evlog_export_cel(void);

/* ---- Boot Integrity Verification API ----
 *
 * Verifies that measured boot values (PCR digests from the TCG event log)
 * match expected golden values.  This is the foundation for:
 *   - Trusted Boot: detect firmware/bootloader/kernel tampering
 *   - BitLocker-style FDE: seal encryption keys to PCR state
 *   - Remote attestation: prove boot integrity to remote parties
 *
 * STATUS: Stub implementation.  Full verification requires:
 *   1. SHA-256 crypto primitives (to compute expected hashes)
 *   2. TPM PCR read API (to get actual PCR register values)
 *   3. First-boot enrollment (store baseline golden PCR values)
 *   4. Secure storage for golden values (encrypted NVRAM or TPM NV index)
 * ---- */

/* Standard PCR indices for the boot chain */
#define TPM_PCR_FIRMWARE        0   /* Platform firmware code and data */
#define TPM_PCR_FIRMWARE_CONFIG 1   /* Host platform configuration (BIOS settings) */
#define TPM_PCR_OPTION_ROMS     2   /* Option ROM code */
#define TPM_PCR_OPTION_ROM_CFG  3   /* Option ROM configuration and data */
#define TPM_PCR_MBR             4   /* IPL code (bootloader / UEFI boot app) */
#define TPM_PCR_MBR_CONFIG      5   /* IPL configuration and data */
#define TPM_PCR_STATE_TRANS     6   /* State transition and wake events */
#define TPM_PCR_SECUREBOOT      7   /* Secure Boot policy (db/dbx/KEK/PK) */

/* Boot integrity verification status */
#define BOOT_INTEGRITY_UNKNOWN       0  /* Not yet checked */
#define BOOT_INTEGRITY_VERIFIED      1  /* All PCRs match golden values */
#define BOOT_INTEGRITY_MISMATCH      2  /* One or more PCRs differ */
#define BOOT_INTEGRITY_NO_TPM        3  /* No TPM -- cannot verify */
#define BOOT_INTEGRITY_NO_BASELINE   4  /* No golden values enrolled */
#define BOOT_INTEGRITY_NO_CRYPTO     5  /* Crypto stack not available */

/* Per-PCR verification result */
struct pcr_check {
    uint8_t  pcr_index;     /* PCR register number */
    uint8_t  status;        /* BOOT_INTEGRITY_* constant */
    uint8_t  pad[2];
};

/* Full boot integrity report -- feeds into the "Boot Integrity" UI panel */
struct boot_integrity_report {
    uint8_t  overall_status;           /* BOOT_INTEGRITY_* */
    uint8_t  pcr_count;                /* number of PCRs checked */
    uint8_t  pad[2];
    struct pcr_check pcrs[8];          /* PCR[0], PCR[1], ..., PCR[7] */
    uint32_t event_count;              /* total measured events */
    uint8_t  tpm_version;              /* 0=none, 1=1.2, 2=2.0 */
    uint8_t  secure_boot;              /* 1 if Secure Boot was active */
    uint8_t  tpm_rng_available;        /* 1 once TPM2_GetRandom contributed entropy */
    uint8_t  pad2;
};

/* Initialize boot integrity verification.
 * Analyzes the parsed event log, attempts PCR golden value comparison.
 * Must be called after tpm_init().
 *
 * When fully implemented, this will:
 *   - Read golden PCR values from secure storage (enrolled at first boot)
 *   - Replay the event log to compute expected PCR values (requires SHA-256)
 *   - Compare computed values against TPM PCR registers (requires TPM read API)
 *   - Set per-PCR status in the boot integrity report */
boot_result_t tpm_integrity_init(void);

/* Returns 1 if boot integrity is verified (all PCRs match golden values).
 * Returns 0 if not verified, no TPM, or no baseline enrolled. */
int tpm_integrity_verified(void);

/* Returns the full boot integrity report for the System Settings UI.
 * Valid after tpm_integrity_init(). */
const struct boot_integrity_report *tpm_integrity_report(void);

/* Record TPM RNG availability in the report. Called by the entropy
 * TPM collector AFTER tpm_integrity_init() (the transport and RNG
 * collection run in Phase 1; the report is built in Phase 0). */
void tpm_integrity_set_rng_available(int available);
