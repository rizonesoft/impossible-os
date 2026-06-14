/* ============================================================================
 * sha384.h -- FIPS 180-4 SHA-384, freestanding kernel implementation
 *
 * SHA-384 is SHA-512 with a different initial hash value and a 48-byte
 * truncated output. Rather than duplicate the ~280-line SHA-512 transform, this
 * reuses the vetted monocypher crypto_sha512 compression: init, swap in the
 * SHA-384 IVs, absorb, then take the leading 48 bytes. No global mutable state
 * (caller-owned context) -> SMP-safe by construction. Needed for the SHA-384
 * TPM PCR bank in measured-boot replay. Validated against the NIST FIPS 180-4
 * known-answer vectors in test_sha384.c.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "libs/monocypher/monocypher-ed25519.h"  /* crypto_sha512_ctx + compression */

#define SHA384_DIGEST_LEN 48u
#define SHA384_BLOCK_LEN  128u

/* The context wraps monocypher's SHA-512 state (same compression, SHA-384 IVs
 * installed by sha384_init). */
struct sha384_ctx {
    crypto_sha512_ctx inner;
};

/* Streaming API: init -> update* -> final (writes 48 bytes to out). */
void sha384_init(struct sha384_ctx *ctx);
void sha384_update(struct sha384_ctx *ctx, const void *data, uint32_t len);
void sha384_final(struct sha384_ctx *ctx, uint8_t out[SHA384_DIGEST_LEN]);

/* One-shot convenience: SHA-384(data[0..len)) -> out[48]. */
void sha384(const void *data, uint32_t len, uint8_t out[SHA384_DIGEST_LEN]);
