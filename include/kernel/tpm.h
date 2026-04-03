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

/* ---- Boot Integrity Verification API (§9.2) ----
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
    uint8_t  pad2[2];
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
