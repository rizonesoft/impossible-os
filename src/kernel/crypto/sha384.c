/* ============================================================================
 * sha384.c -- FIPS 180-4 SHA-384 over the monocypher SHA-512 compression
 *
 * SHA-384 differs from SHA-512 only in the initial hash value and the 48-byte
 * truncation. monocypher's crypto_sha512_init sets the SHA-512 IVs and zeroes
 * the counters; we install the SHA-384 IVs over ctx->hash[] afterward and reuse
 * the identical (vetted) update/final compression, then keep the leading 48
 * bytes. Correctness pinned by the NIST known-answer vectors in test_sha384.c.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/crypto/sha384.h"

/* This wrapper installs the SHA-384 IVs over monocypher's SHA-512 state, which
 * relies on crypto_sha512_ctx.hash being the leading 8-word SHA-512 state.
 * monocypher documents its context layout as non-contractual, so pin the exact
 * assumption at compile time: a future monocypher refresh that moves or resizes
 * `hash` must FAIL THIS BUILD loudly, never silently produce wrong PCR-replay
 * digests (the KATs in test_sha384.c are the second line of defense). */
_Static_assert(__builtin_offsetof(crypto_sha512_ctx, hash) == 0,
    "sha384: crypto_sha512_ctx.hash must be the leading SHA-512 state words");
_Static_assert(sizeof(((crypto_sha512_ctx *)0)->hash) == 64u,
    "sha384: crypto_sha512_ctx.hash must be 8 x uint64 (the SHA-512/384 state)");

/* FIPS 180-4 section 5.3.4 SHA-384 initial hash value. */
static const uint64_t SHA384_IV[8] = {
    0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
    0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull,
};

void sha384_init(struct sha384_ctx *ctx)
{
    if (!ctx)
        return;
    crypto_sha512_init(&ctx->inner);     /* sets SHA-512 IVs + zeroes counters */
    for (uint32_t i = 0; i < 8u; i++)    /* override with the SHA-384 IVs */
        ctx->inner.hash[i] = SHA384_IV[i];
}

void sha384_update(struct sha384_ctx *ctx, const void *data, uint32_t len)
{
    if (!ctx || (!data && len) || len == 0u)
        return;
    crypto_sha512_update(&ctx->inner, (const uint8_t *)data, (size_t)len);
}

void sha384_final(struct sha384_ctx *ctx, uint8_t out[SHA384_DIGEST_LEN])
{
    uint8_t full[64];
    if (!ctx || !out)
        return;
    crypto_sha512_final(&ctx->inner, full);  /* SHA-512-format 64-byte digest */
    for (uint32_t i = 0; i < SHA384_DIGEST_LEN; i++)
        out[i] = full[i];                    /* SHA-384 = leading 48 bytes */
    /* Wipe the discarded tail + working buffer (it held digest material). */
    for (uint32_t i = 0; i < 64u; i++)
        full[i] = 0u;
}

void sha384(const void *data, uint32_t len, uint8_t out[SHA384_DIGEST_LEN])
{
    struct sha384_ctx ctx;
    sha384_init(&ctx);
    sha384_update(&ctx, data, len);
    sha384_final(&ctx, out);
}
