/* ============================================================================
 * tpm_replay.c -- measured-boot PCR replay primitives
 *
 * tpm_pcr_extend reproduces a TPM PCR_Extend: PCR := H_bank(PCR || digest).
 * Concatenates the current PCR value and the incoming measurement digest (each
 * one bank-digest long) and hashes with the bank's algorithm. Pure + no global
 * state; the event-log walk + hardware comparison build on this.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/tpm_replay.h"
#include "kernel/tpm.h"                 /* TPM_ALG_*, tpm_alg_digest_len_pub */
#include "kernel/crypto/sha1.h"
#include "kernel/crypto/sha256.h"
#include "kernel/crypto/sha384.h"
#include "libs/monocypher/monocypher-ed25519.h"  /* crypto_sha512 for the SHA-512 bank */
#include "libc/string.h"

/* Largest bank digest is SHA-512 (64 bytes); the concat input is 2x that. */
#define TPM_REPLAY_MAX_DIGEST 64u

tpm_replay_status_t tpm_pcr_extend(uint16_t alg, uint8_t *pcr, uint32_t pcr_len,
                                   const uint8_t *digest, uint32_t digest_len)
{
    uint16_t dl = tpm_alg_digest_len_pub(alg);
    uint8_t input[TPM_REPLAY_MAX_DIGEST * 2u];

    if (!pcr || !digest || dl == 0u ||
        pcr_len != (uint32_t)dl || digest_len != (uint32_t)dl)
        return TPM_REPLAY_BADARG;

    /* input = PCR_old || incoming_digest. */
    memcpy(input, pcr, dl);
    memcpy(input + dl, digest, dl);

    switch (alg) {
        case TPM_ALG_SHA1:   sha1(input,   (uint32_t)dl * 2u, pcr); break;
        case TPM_ALG_SHA256: sha256(input, (uint32_t)dl * 2u, pcr); break;
        case TPM_ALG_SHA384: sha384(input, (uint32_t)dl * 2u, pcr); break;
        case TPM_ALG_SHA512:
            crypto_sha512(pcr, input, (size_t)dl * 2u);
            break;
        default:
            /* Unreachable: tpm_alg_digest_len_pub already rejected unknown algs
             * above (dl == 0). Kept so the switch is total. */
            return TPM_REPLAY_BADARG;
    }

    /* Wipe the concat buffer -- it held PCR + measurement material. */
    for (uint32_t i = 0; i < sizeof(input); i++)
        input[i] = 0u;
    return TPM_REPLAY_OK;
}
