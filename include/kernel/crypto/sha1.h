/* ============================================================================
 * sha1.h -- FIPS 180-4 SHA-1, freestanding kernel implementation
 *
 * Pure, allocation-free, caller-owned-context hash (SMP-safe by construction,
 * like sha256.h). Needed for the legacy SHA-1 TPM PCR bank in measured-boot
 * replay; SHA-1 is cryptographically weak and used ONLY to reproduce a TPM's
 * SHA-1 bank for attestation/replay, never for new security decisions.
 * Validated against the NIST FIPS 180-4 known-answer vectors in test_sha1.c.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

#define SHA1_DIGEST_LEN 20u
#define SHA1_BLOCK_LEN  64u

struct sha1_ctx {
    uint32_t state[5];     /* running hash state H0..H4 */
    uint64_t total_len;    /* total message bytes absorbed */
    uint8_t  buf[SHA1_BLOCK_LEN];  /* partial-block buffer */
    uint32_t buf_len;      /* bytes currently buffered (< SHA1_BLOCK_LEN) */
};

/* Streaming API: init -> update* -> final (writes 20 bytes to out). */
void sha1_init(struct sha1_ctx *ctx);
void sha1_update(struct sha1_ctx *ctx, const void *data, uint32_t len);
void sha1_final(struct sha1_ctx *ctx, uint8_t out[SHA1_DIGEST_LEN]);

/* One-shot convenience: SHA-1(data[0..len)) -> out[20]. */
void sha1(const void *data, uint32_t len, uint8_t out[SHA1_DIGEST_LEN]);
