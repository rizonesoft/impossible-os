/* ci_crypto.c -- Code Integrity crypto provider bridge.
 *
 * Fail-closed, allocation-free selection over the kernel's existing primitives:
 * crypto_hash (SHA-2/3), Monocypher BLAKE2b, and Monocypher PureEd25519. See
 * ci_crypto.h for the security posture (every error path denies).
 */
#include "kernel/ci/ci_crypto.h"
#include "kernel/crypto/hash.h"
#include "libs/monocypher/monocypher.h"
#include "libs/monocypher/monocypher-ed25519.h"

uint32_t ci_crypto_digest_len(ci_digest_alg_t alg)
{
    switch (alg) {
    case CI_DIGEST_SHA256:      return 32;
    case CI_DIGEST_SHA384:      return 48;
    case CI_DIGEST_BLAKE2B_256: return 32;
    default:                    return 0;
    }
}

int ci_crypto_digest(ci_digest_alg_t alg, const void *data, uint32_t len,
                     uint8_t *out, uint32_t out_cap)
{
    uint32_t dlen = ci_crypto_digest_len(alg);
    if (dlen == 0 || !out || out_cap < dlen) {
        return -1;   /* unknown alg / no output / output too small */
    }
    /* Fail closed on a NULL buffer with a non-zero length: BLAKE2b would read
     * the message pointer (crypto_hash rejects this, Monocypher does not).
     * NULL data with len == 0 is a valid empty input. */
    if (!data && len != 0) {
        return -1;
    }

    switch (alg) {
    case CI_DIGEST_SHA256:
        return crypto_hash(CRYPTO_HASH_SHA256, data, len, out, out_cap);
    case CI_DIGEST_SHA384:
        return crypto_hash(CRYPTO_HASH_SHA384, data, len, out, out_cap);
    case CI_DIGEST_BLAKE2B_256:
        crypto_blake2b(out, 32, (const uint8_t *)data, len);  /* BLAKE2b-256 */
        return 32;
    default:
        return -1;   /* unreachable: dlen == 0 already handled */
    }
}

bool ci_crypto_verify(ci_sig_alg_t alg, const uint8_t *msg, uint32_t msg_len,
                      const uint8_t *sig, uint32_t sig_len,
                      const uint8_t *pubkey, uint32_t pubkey_len)
{
    /* Fail closed on any missing input. A NULL message is allowed ONLY for an
     * empty message (msg_len == 0). */
    if (!sig || !pubkey || (!msg && msg_len != 0)) {
        return false;
    }

    switch (alg) {
    case CI_SIG_ED25519:
        if (sig_len != CI_ED25519_SIG_LEN || pubkey_len != CI_ED25519_PUBKEY_LEN) {
            return false;
        }
        /* PureEd25519: crypto_ed25519_check returns 0 on a valid signature. Any
         * non-zero result (and every other path) denies. */
        return crypto_ed25519_check(sig, pubkey, msg, msg_len) == 0;
    case CI_SIG_RSA_PKCS1:
    default:
        return false;   /* RSA/X.509 reserved until the CNG provider matures */
    }
}
