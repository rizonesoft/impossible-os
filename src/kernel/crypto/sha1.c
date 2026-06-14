/* ============================================================================
 * sha1.c -- FIPS 180-4 SHA-1
 *
 * Textbook implementation mirroring sha256.c: big-endian message schedule,
 * 80-round compression, identical padding (0x80, zeros, 64-bit big-endian bit
 * length). No allocation, no global mutable state -- caller-owned context, so
 * SMP-safe by construction. Correctness pinned by the NIST known-answer vectors
 * in test_sha1.c. SHA-1 is weak; this exists only to reproduce a TPM's SHA-1
 * PCR bank for measured-boot replay, never for new security decisions.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/crypto/sha1.h"

static inline uint32_t rotl(uint32_t x, uint32_t n) { return (x << n) | (x >> (32u - n)); }

static void sha1_compress(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e, t;

    for (t = 0; t < 16u; t++)
        w[t] = ((uint32_t)block[t * 4u] << 24) | ((uint32_t)block[t * 4u + 1u] << 16) |
               ((uint32_t)block[t * 4u + 2u] << 8) | (uint32_t)block[t * 4u + 3u];
    for (t = 16u; t < 80u; t++)
        w[t] = rotl(w[t - 3u] ^ w[t - 8u] ^ w[t - 14u] ^ w[t - 16u], 1u);

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

    for (t = 0; t < 80u; t++) {
        uint32_t f, k;
        if (t < 20u)      { f = (b & c) | (~b & d);            k = 0x5a827999u; }
        else if (t < 40u) { f = b ^ c ^ d;                     k = 0x6ed9eba1u; }
        else if (t < 60u) { f = (b & c) | (b & d) | (c & d);   k = 0x8f1bbcdcu; }
        else              { f = b ^ c ^ d;                     k = 0xca62c1d6u; }
        uint32_t tmp = rotl(a, 5u) + f + e + k + w[t];
        e = d; d = c; c = rotl(b, 30u); b = a; a = tmp;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

void sha1_init(struct sha1_ctx *ctx)
{
    if (!ctx)
        return;
    ctx->state[0] = 0x67452301u; ctx->state[1] = 0xefcdab89u;
    ctx->state[2] = 0x98badcfeu; ctx->state[3] = 0x10325476u;
    ctx->state[4] = 0xc3d2e1f0u;
    ctx->total_len = 0;
    ctx->buf_len = 0;
}

void sha1_update(struct sha1_ctx *ctx, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    if (!ctx || (!p && len))
        return;
    /* Zero-length update is a safe no-op separator -- return BEFORE any pointer
     * arithmetic so sha1_update(ctx, NULL, 0) cannot compute NULL+0 (UB). */
    if (len == 0u)
        return;
    ctx->total_len += len;

    if (ctx->buf_len) {
        uint32_t need = SHA1_BLOCK_LEN - ctx->buf_len;
        uint32_t take = (len < need) ? len : need;
        for (uint32_t i = 0; i < take; i++)
            ctx->buf[ctx->buf_len + i] = p[i];
        ctx->buf_len += take;
        p += take;
        len -= take;
        if (ctx->buf_len == SHA1_BLOCK_LEN) {
            sha1_compress(ctx->state, ctx->buf);
            ctx->buf_len = 0;
        }
    }
    while (len >= SHA1_BLOCK_LEN) {
        sha1_compress(ctx->state, p);
        p += SHA1_BLOCK_LEN;
        len -= SHA1_BLOCK_LEN;
    }
    for (uint32_t i = 0; i < len; i++)
        ctx->buf[ctx->buf_len + i] = p[i];
    ctx->buf_len += len;
}

void sha1_final(struct sha1_ctx *ctx, uint8_t out[SHA1_DIGEST_LEN])
{
    uint64_t bit_len;
    uint32_t i;
    if (!ctx || !out)
        return;
    bit_len = ctx->total_len * 8u;

    ctx->buf[ctx->buf_len++] = 0x80u;
    if (ctx->buf_len > SHA1_BLOCK_LEN - 8u) {
        while (ctx->buf_len < SHA1_BLOCK_LEN)
            ctx->buf[ctx->buf_len++] = 0u;
        sha1_compress(ctx->state, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < SHA1_BLOCK_LEN - 8u)
        ctx->buf[ctx->buf_len++] = 0u;
    for (i = 0; i < 8u; i++)
        ctx->buf[SHA1_BLOCK_LEN - 1u - i] = (uint8_t)(bit_len >> (8u * i));
    sha1_compress(ctx->state, ctx->buf);

    for (i = 0; i < 5u; i++) {
        out[i * 4u]      = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4u + 1u] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4u + 2u] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4u + 3u] = (uint8_t)(ctx->state[i]);
    }
}

void sha1(const void *data, uint32_t len, uint8_t out[SHA1_DIGEST_LEN])
{
    struct sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, out);
}
