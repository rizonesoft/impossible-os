/* ============================================================================
 * sha256.h -- FIPS 180-4 SHA-256, freestanding kernel implementation
 *
 * Pure, allocation-free, caller-owned-context hash. No global mutable state, so
 * it is SMP-safe by construction (each caller owns its struct sha256_ctx) and
 * usable from any context. Validated against the NIST FIPS 180-4 known-answer
 * vectors in test_sha256.c.
 *
 * Needed by: measured-boot PCR replay (the SHA-256 PCR bank), boot-baseline
 * image hashes, and the CNG `cng_sha256` wrapper. This is the raw transform; the
 * Win32 CNG surface layers on top.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

#define SHA256_DIGEST_LEN 32u
#define SHA256_BLOCK_LEN  64u

struct sha256_ctx {
    uint32_t state[8];     /* running hash state H0..H7 */
    uint64_t total_len;    /* total message bytes absorbed */
    uint8_t  buf[SHA256_BLOCK_LEN];  /* partial-block buffer */
    uint32_t buf_len;      /* bytes currently buffered (< SHA256_BLOCK_LEN) */
};

/* Streaming API: init -> update* -> final (writes 32 bytes to out). */
void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, uint32_t len);
void sha256_final(struct sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_LEN]);

/* One-shot convenience: SHA-256(data[0..len)) -> out[32]. */
void sha256(const void *data, uint32_t len, uint8_t out[SHA256_DIGEST_LEN]);
