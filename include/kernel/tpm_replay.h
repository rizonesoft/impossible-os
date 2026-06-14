/* ============================================================================
 * tpm_replay.h -- measured-boot PCR replay primitives
 *
 * The PCR-extend operation that the replay engine walks the event log with:
 * PCR_new = H_bank(PCR_old || measurement_digest), per TPM 2.0 PCR_Extend
 * semantics (TPM 2.0 Part 1). Pure, no global state, no TPM transaction -- so
 * it is unit-testable against fixed vectors and SMP-safe by construction.
 * Dispatches on the bank algorithm to the kernel SHA-1/256/384 transforms (and
 * monocypher SHA-512). The full event-log replay + hardware-PCR comparison
 * layers on top of this.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

typedef enum {
    TPM_REPLAY_OK      = 0,  /* extend applied, pcr updated in place */
    TPM_REPLAY_BADARG  = 1,  /* NULL / wrong length / unsupported bank alg */
} tpm_replay_status_t;

/* Extend one PCR in one hash bank: pcr := H_alg(pcr || digest), both `pcr` and
 * `digest` being exactly tpm_alg_digest_len_pub(alg) bytes. `pcr` is updated in
 * place. `alg` is a TPM_ALG_* (SHA-1/256/384/512). A fresh PCR for replay starts
 * at all-zero (the TPM reset value for these banks). */
tpm_replay_status_t tpm_pcr_extend(uint16_t alg, uint8_t *pcr, uint32_t pcr_len,
                                   const uint8_t *digest, uint32_t digest_len);
