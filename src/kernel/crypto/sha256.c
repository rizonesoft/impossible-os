/* ============================================================================
 * sha256.c -- FIPS 180-4 SHA-256
 *
 * Textbook implementation: big-endian message schedule, 64-round compression.
 * No allocation, no global mutable state -- the context is caller-owned, so the
 * transform is SMP-safe by construction. Correctness is pinned by the NIST
 * known-answer vectors in test_sha256.c (empty / "abc" / 448-bit / 1 000 000 *
 * 'a'); any error in the constants, schedule, padding, or length encoding fails
 * those vectors.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/crypto/sha256.h"

/* FIPS 180-4 section 4.2.2 round constants (first 32 bits of the fractional
 * parts of the cube roots of the first 64 primes). */
static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32u - n)); }

#define CH(x, y, z)   (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x)      (rotr((x), 2)  ^ rotr((x), 13) ^ rotr((x), 22))
#define BSIG1(x)      (rotr((x), 6)  ^ rotr((x), 11) ^ rotr((x), 25))
#define SSIG0(x)      (rotr((x), 7)  ^ rotr((x), 18) ^ ((x) >> 3))
#define SSIG1(x)      (rotr((x), 17) ^ rotr((x), 19) ^ ((x) >> 10))

static void sha256_compress(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t t;

    for (t = 0; t < 16u; t++)
        w[t] = ((uint32_t)block[t * 4u] << 24) | ((uint32_t)block[t * 4u + 1u] << 16) |
               ((uint32_t)block[t * 4u + 2u] << 8) | (uint32_t)block[t * 4u + 3u];
    for (t = 16u; t < 64u; t++)
        w[t] = SSIG1(w[t - 2u]) + w[t - 7u] + SSIG0(w[t - 15u]) + w[t - 16u];

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (t = 0; t < 64u; t++) {
        uint32_t t1 = h + BSIG1(e) + CH(e, f, g) + K[t] + w[t];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_init(struct sha256_ctx *ctx)
{
    if (!ctx)
        return;
    /* FIPS 180-4 section 5.3.3 initial hash value. */
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->total_len = 0;
    ctx->buf_len = 0;
}

void sha256_update(struct sha256_ctx *ctx, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!ctx || (!p && len))
        return;
    /* A zero-length update is a no-op separator -- return BEFORE any pointer
     * arithmetic so sha256_update(ctx, NULL, 0) cannot compute NULL+0 (UB). */
    if (len == 0u)
        return;
    ctx->total_len += len;

    /* Top up a partial block first. */
    if (ctx->buf_len) {
        uint32_t need = SHA256_BLOCK_LEN - ctx->buf_len;
        uint32_t take = (len < need) ? len : need;
        for (uint32_t i = 0; i < take; i++)
            ctx->buf[ctx->buf_len + i] = p[i];
        ctx->buf_len += take;
        p += take;
        len -= take;
        if (ctx->buf_len == SHA256_BLOCK_LEN) {
            sha256_compress(ctx->state, ctx->buf);
            ctx->buf_len = 0;
        }
    }
    /* Process whole blocks straight from the input. */
    while (len >= SHA256_BLOCK_LEN) {
        sha256_compress(ctx->state, p);
        p += SHA256_BLOCK_LEN;
        len -= SHA256_BLOCK_LEN;
    }
    /* Buffer the remainder. */
    for (uint32_t i = 0; i < len; i++)
        ctx->buf[ctx->buf_len + i] = p[i];
    ctx->buf_len += len;
}

void sha256_final(struct sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_LEN])
{
    uint64_t bit_len;
    uint32_t i;
    if (!ctx || !out)
        return;
    bit_len = ctx->total_len * 8u;

    /* Append 0x80, then zeros, then the 64-bit big-endian bit length so the
     * padded message length is a multiple of 64 bytes (FIPS 180-4 section 5.1.1). */
    ctx->buf[ctx->buf_len++] = 0x80u;
    if (ctx->buf_len > SHA256_BLOCK_LEN - 8u) {
        while (ctx->buf_len < SHA256_BLOCK_LEN)
            ctx->buf[ctx->buf_len++] = 0u;
        sha256_compress(ctx->state, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < SHA256_BLOCK_LEN - 8u)
        ctx->buf[ctx->buf_len++] = 0u;
    for (i = 0; i < 8u; i++)
        ctx->buf[SHA256_BLOCK_LEN - 1u - i] = (uint8_t)(bit_len >> (8u * i));
    sha256_compress(ctx->state, ctx->buf);

    for (i = 0; i < 8u; i++) {
        out[i * 4u]      = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4u + 1u] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4u + 2u] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4u + 3u] = (uint8_t)(ctx->state[i]);
    }
}

void sha256(const void *data, uint32_t len, uint8_t out[SHA256_DIGEST_LEN])
{
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}
