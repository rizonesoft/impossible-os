/* ============================================================================
 * sha3.c -- FIPS 202 SHA-3 / SHAKE (Keccak-f[1600]) freestanding kernel impl
 *
 * Written from the FIPS 202 specification (Keccak is public-domain). Pure,
 * allocation-free, caller-owned context -> SMP-safe by construction. The
 * sponge state is addressed as uint64 lanes through arithmetic shifts only
 * (never byte-aliased), so absorb/squeeze are endianness-neutral.
 * ============================================================================ */

#include "kernel/crypto/sha3.h"
#include "libc/string.h"

#define KECCAK_STATE_BYTES 200u  /* 25 lanes * 8 bytes = 1600 bits */
#define KECCAK_ROUNDS      24

/* Sponge rates (bytes) = 200 - 2*(security strength in bytes). SHAKE256 shares
 * SHA3-256's 136-byte rate. These are the ONLY legal rates; sha3_ctx_valid()
 * rejects any other value so a zeroed/corrupted caller context cannot drive
 * rate-1 underflow or an over-long squeeze. */
#define SHA3_512_RATE 72u
#define SHA3_384_RATE 104u
#define SHA3_256_RATE 136u
#define SHAKE128_RATE 168u

#define ROTL64(x, n) (((x) << (n)) | ((x) >> (64 - (n))))

/* Iota round constants (FIPS 202 Table). */
static const uint64_t k_rndc[KECCAK_ROUNDS] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};

/* Rho rotation offsets and Pi lane permutation (FIPS 202). */
static const uint32_t k_rotc[KECCAK_ROUNDS] = {
    1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
    27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44,
};
static const uint32_t k_piln[KECCAK_ROUNDS] = {
    10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
    15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1,
};

_Static_assert(sizeof(((struct sha3_ctx *)0)->st) == KECCAK_STATE_BYTES,
               "Keccak state must be 1600 bits (25 lanes)");

/* Keccak-f[1600] permutation, in place on the 25-lane state. */
static void keccakf(uint64_t st[25])
{
    uint64_t bc[5], t;
    uint32_t i, j, r;

    for (r = 0; r < KECCAK_ROUNDS; r++) {
        /* Theta */
        for (i = 0; i < 5; i++) {
            bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
        }
        for (i = 0; i < 5; i++) {
            t = bc[(i + 4) % 5] ^ ROTL64(bc[(i + 1) % 5], 1);
            for (j = 0; j < 25; j += 5) {
                st[j + i] ^= t;
            }
        }
        /* Rho + Pi */
        t = st[1];
        for (i = 0; i < KECCAK_ROUNDS; i++) {
            j = k_piln[i];
            bc[0] = st[j];
            st[j] = ROTL64(t, k_rotc[i]);
            t = bc[0];
        }
        /* Chi */
        for (j = 0; j < 25; j += 5) {
            for (i = 0; i < 5; i++) {
                bc[i] = st[j + i];
            }
            for (i = 0; i < 5; i++) {
                st[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
            }
        }
        /* Iota */
        st[0] ^= k_rndc[r];
    }
}

/* XOR a single message byte into block position p (endianness-neutral). */
static inline void absorb_byte(uint64_t st[25], uint32_t p, uint8_t b)
{
    st[p >> 3] ^= (uint64_t)b << (8u * (p & 7u));
}

/* Extract output byte at block position p (endianness-neutral). */
static inline uint8_t squeeze_byte(const uint64_t st[25], uint32_t p)
{
    return (uint8_t)(st[p >> 3] >> (8u * (p & 7u)));
}

static void sha3_init_internal(struct sha3_ctx *ctx, uint32_t rate, uint8_t delim)
{
    if (ctx == NULL) {
        return;
    }
    memset(ctx->st, 0, sizeof(ctx->st));
    ctx->rate = rate;
    ctx->pos = 0;
    ctx->delim = delim;
}

/* A caller-owned context is valid only with a known rate and pos < rate; any
 * other {rate,pos} (zeroed / stale / corrupted) is rejected before the
 * rate-driven index arithmetic in update/final/pad/squeeze. */
static int sha3_ctx_valid(const struct sha3_ctx *ctx)
{
    if (ctx == NULL) {
        return 0;
    }
    switch (ctx->rate) {
    case SHA3_512_RATE:
    case SHA3_384_RATE:
    case SHA3_256_RATE:  /* == SHAKE256 rate */
    case SHAKE128_RATE:
        break;
    default:
        return 0;
    }
    return ctx->pos < ctx->rate;
}

void sha3_256_init(struct sha3_ctx *ctx) { sha3_init_internal(ctx, SHA3_256_RATE, 0x06); }
void sha3_384_init(struct sha3_ctx *ctx) { sha3_init_internal(ctx, SHA3_384_RATE, 0x06); }
void sha3_512_init(struct sha3_ctx *ctx) { sha3_init_internal(ctx, SHA3_512_RATE, 0x06); }

void sha3_update(struct sha3_ctx *ctx, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;

    /* Fail closed on bad args (matches the SHA-2 wrappers). A zero-length
     * update is a no-op separator -- return before any pointer arithmetic so
     * sha3_update(ctx, NULL, 0) cannot compute NULL+0 (UB). A malformed /
     * uninitialized context is rejected before any rate-driven indexing. */
    if (p == NULL && len != 0) {
        return;
    }
    if (len == 0) {
        return;
    }
    if (!sha3_ctx_valid(ctx)) {
        return;
    }

    for (i = 0; i < len; i++) {
        absorb_byte(ctx->st, ctx->pos, p[i]);
        ctx->pos++;
        if (ctx->pos == ctx->rate) {
            keccakf(ctx->st);
            ctx->pos = 0;
        }
    }
}

/* Apply pad10*1 + domain separation and run the final permutation. After this
 * the sponge is ready to squeeze from the start of the rate region. */
static void sha3_pad(struct sha3_ctx *ctx)
{
    /* Domain delimiter at the current position, 0x80 at the last rate byte.
     * When pos == rate - 1 both land in the same byte (delim | 0x80). */
    absorb_byte(ctx->st, ctx->pos, ctx->delim);
    absorb_byte(ctx->st, ctx->rate - 1, 0x80);
    keccakf(ctx->st);
}

/* Squeeze out_len bytes, running the permutation between rate-sized blocks. */
static void sha3_squeeze(struct sha3_ctx *ctx, uint8_t *out, uint32_t out_len)
{
    uint32_t produced = 0;
    uint32_t block_pos = 0;

    while (produced < out_len) {
        if (block_pos == ctx->rate) {
            keccakf(ctx->st);
            block_pos = 0;
        }
        out[produced++] = squeeze_byte(ctx->st, block_pos++);
    }
}

int sha3_final(struct sha3_ctx *ctx, uint8_t *out, uint32_t out_cap)
{
    uint32_t digest_len;

    /* Reject a NULL out or a malformed/uninitialized context before deriving
     * digest_len from ctx->rate (a corrupted rate would underflow / over-long
     * the squeeze). */
    if (out == NULL || !sha3_ctx_valid(ctx)) {
        return -1;
    }
    /* Fixed-output digest length for a SHA-3 variant is capacity/2 =
     * (200 - rate) / 2 bytes (32 / 48 / 64). Never write past the caller's
     * buffer even if a corrupted-but-legal rate selects a larger digest than
     * the caller allocated. */
    digest_len = (KECCAK_STATE_BYTES - ctx->rate) / 2u;
    if (out_cap < digest_len) {
        return -1;
    }

    sha3_pad(ctx);
    sha3_squeeze(ctx, out, digest_len);
    return (int)digest_len;
}

void sha3_256(const void *data, uint32_t len, uint8_t out[SHA3_256_DIGEST_LEN])
{
    struct sha3_ctx ctx;
    sha3_256_init(&ctx);
    sha3_update(&ctx, data, len);
    sha3_final(&ctx, out, SHA3_256_DIGEST_LEN);
}

void sha3_384(const void *data, uint32_t len, uint8_t out[SHA3_384_DIGEST_LEN])
{
    struct sha3_ctx ctx;
    sha3_384_init(&ctx);
    sha3_update(&ctx, data, len);
    sha3_final(&ctx, out, SHA3_384_DIGEST_LEN);
}

void sha3_512(const void *data, uint32_t len, uint8_t out[SHA3_512_DIGEST_LEN])
{
    struct sha3_ctx ctx;
    sha3_512_init(&ctx);
    sha3_update(&ctx, data, len);
    sha3_final(&ctx, out, SHA3_512_DIGEST_LEN);
}

static int shake_oneshot(uint32_t rate, const void *data, uint32_t len,
                         uint8_t *out, uint32_t out_len)
{
    struct sha3_ctx ctx;

    if (out == NULL || out_len == 0 || out_len > SHAKE_MAX_OUTPUT) {
        return -1;
    }
    if (data == NULL && len != 0) {
        return -1;
    }
    sha3_init_internal(&ctx, rate, 0x1F);
    sha3_update(&ctx, data, len);
    sha3_pad(&ctx);
    sha3_squeeze(&ctx, out, out_len);
    return (int)out_len;
}

int shake128(const void *data, uint32_t len, uint8_t *out, uint32_t out_len)
{
    return shake_oneshot(SHAKE128_RATE, data, len, out, out_len);
}

int shake256(const void *data, uint32_t len, uint8_t *out, uint32_t out_len)
{
    return shake_oneshot(SHA3_256_RATE, data, len, out, out_len);
}
