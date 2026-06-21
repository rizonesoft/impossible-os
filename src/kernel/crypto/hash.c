/* ============================================================================
 * hash.c -- Kernel hash dispatch (SHA-2 / SHA-3 by algorithm id)
 *
 * Thin, bounds-checked selector over the per-algorithm transforms. No state,
 * no allocation; safe to call concurrently from any CPU.
 * ============================================================================ */

#include "kernel/crypto/hash.h"
#include "kernel/crypto/sha256.h"
#include "kernel/crypto/sha384.h"
#include "kernel/crypto/sha3.h"

uint32_t crypto_hash_digest_len(crypto_hash_alg_t alg)
{
    switch (alg) {
    case CRYPTO_HASH_SHA256:   return SHA256_DIGEST_LEN;
    case CRYPTO_HASH_SHA384:   return SHA384_DIGEST_LEN;
    case CRYPTO_HASH_SHA3_256: return SHA3_256_DIGEST_LEN;
    case CRYPTO_HASH_SHA3_384: return SHA3_384_DIGEST_LEN;
    case CRYPTO_HASH_SHA3_512: return SHA3_512_DIGEST_LEN;
    default:                   return 0;
    }
}

int crypto_hash(crypto_hash_alg_t alg, const void *data, uint32_t len,
                uint8_t *out, uint32_t out_cap)
{
    uint32_t digest_len = crypto_hash_digest_len(alg);

    if (digest_len == 0 || out == NULL || out_cap < digest_len) {
        return -1;
    }
    /* Reject a NULL message with a non-zero length before it reaches a
     * transform's update loop and dereferences it. (data == NULL with len == 0
     * is the valid empty-message case and is allowed.) */
    if (data == NULL && len != 0) {
        return -1;
    }

    switch (alg) {
    case CRYPTO_HASH_SHA256:   sha256(data, len, out); break;
    case CRYPTO_HASH_SHA384:   sha384(data, len, out); break;
    case CRYPTO_HASH_SHA3_256: sha3_256(data, len, out); break;
    case CRYPTO_HASH_SHA3_384: sha3_384(data, len, out); break;
    case CRYPTO_HASH_SHA3_512: sha3_512(data, len, out); break;
    default:                   return -1;
    }
    return (int)digest_len;
}
