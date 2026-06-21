/* ============================================================================
 * hash.h -- Kernel hash dispatch (select SHA-2 or SHA-3 by algorithm id)
 *
 * One bounds-checked entry point so a consumer can pick a fixed-output hash by
 * id instead of binding to a specific transform. Thin selector over the
 * existing sha256.c / sha384.c (SHA-2) and sha3.c (SHA-3) implementations; all
 * are pure, allocation-free, caller-owned-context, SMP-safe by construction.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

typedef enum {
    CRYPTO_HASH_SHA256 = 0,
    CRYPTO_HASH_SHA384,
    CRYPTO_HASH_SHA3_256,
    CRYPTO_HASH_SHA3_384,
    CRYPTO_HASH_SHA3_512,
} crypto_hash_alg_t;

/* Digest length (bytes) for alg, or 0 if alg is unknown. */
uint32_t crypto_hash_digest_len(crypto_hash_alg_t alg);

/* Hash data[0, len) with alg into out (capacity out_cap). Returns the digest
 * length written (> 0) on success, or -1 on an unknown alg or out_cap too
 * small for the digest. */
int crypto_hash(crypto_hash_alg_t alg, const void *data, uint32_t len,
                uint8_t *out, uint32_t out_cap);
