/* sha256_boot.h -- FIPS 180-4 SHA-256 for the UEFI bootloader.
 *
 * WHY A SECOND COPY OF SHA-256 EXISTS. The kernel ships this algorithm at
 * src/kernel/crypto/sha256.c and that file is freestanding enough to compile
 * into the loader -- but the loader's translation-unit set is fixed by
 * src/boot/uefi/Makefile, and adding a .c file there is a change to build
 * machinery. This header is therefore header-only and static, so BOOTX64.EFI
 * gains a runtime hash without gaining a translation unit.
 *
 * THE TWO COPIES ARE HELD TOGETHER BY A DIFFERENTIAL TEST, not by review.
 * test_uefi_boot.c hashes the same inputs through this header and through the
 * kernel's sha256() and asserts the digests are identical, so a divergence in
 * either implementation fails a normal test run. The NIST known-answer vectors
 * are asserted there too, because two implementations agreeing on a wrong answer
 * is the failure a differential test alone cannot catch.
 *
 * WHY THE SELF-TEST IS NOT OPTIONAL. Inside firmware there is no test harness,
 * and a hash that is subtly wrong produces a digest that looks exactly as
 * authoritative as a correct one. sha256b_selftest() runs two FIPS 180-4 vectors
 * at measurement time and the caller reports its digest ABSENT when it fails, so
 * a broken build reports nothing rather than something false.
 *
 * Type discipline: plain C types rather than UEFI UINT8/UINT32 or kernel
 * uint8_t/uint32_t, following include/boot/uki_cmdline_check.h -- the bootloader
 * uses UEFI types and the kernel uses C99 stdint, and this file must compile
 * under both. The width asserts below are what makes that safe.
 */

#ifndef SHA256_BOOT_H
#define SHA256_BOOT_H

_Static_assert(sizeof(unsigned int) == 4, "sha256_boot.h needs a 32-bit unsigned int");
_Static_assert(sizeof(unsigned long long) == 8,
               "sha256_boot.h needs a 64-bit unsigned long long");

#define SHA256B_DIGEST_LEN 32u
#define SHA256B_BLOCK_LEN  64u

/* Every entry point is static-in-a-header, so a translation unit that uses only
 * part of the API would trip -Wunused-function under -Werror. */
#define SHA256B_FN static __attribute__((unused))

struct sha256b_ctx {
    unsigned int       state[8];
    unsigned long long total_len;             /* message bytes absorbed so far */
    unsigned char      buf[SHA256B_BLOCK_LEN]; /* partial block */
    unsigned int       buf_len;                /* always < SHA256B_BLOCK_LEN */
};

/* FIPS 180-4 section 4.2.2 round constants (first 32 bits of the fractional
 * parts of the cube roots of the first 64 primes). */
static const unsigned int sha256b_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

SHA256B_FN unsigned int sha256b_rotr(unsigned int x, unsigned int n)
{
    return (unsigned int)((x >> n) | (x << (32u - n)));
}

SHA256B_FN void sha256b_compress(unsigned int state[8], const unsigned char blk[64])
{
    unsigned int w[64];
    unsigned int a, b, c, d, e, f, g, h;
    unsigned int i;

    for (i = 0; i < 16u; i++) {
        w[i] = ((unsigned int)blk[i * 4u + 0u] << 24)
             | ((unsigned int)blk[i * 4u + 1u] << 16)
             | ((unsigned int)blk[i * 4u + 2u] << 8)
             | ((unsigned int)blk[i * 4u + 3u]);
    }
    for (i = 16u; i < 64u; i++) {
        unsigned int s0 = sha256b_rotr(w[i - 15u], 7u) ^ sha256b_rotr(w[i - 15u], 18u)
                        ^ (w[i - 15u] >> 3);
        unsigned int s1 = sha256b_rotr(w[i - 2u], 17u) ^ sha256b_rotr(w[i - 2u], 19u)
                        ^ (w[i - 2u] >> 10);
        w[i] = (unsigned int)(w[i - 16u] + s0 + w[i - 7u] + s1);
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64u; i++) {
        unsigned int s1 = sha256b_rotr(e, 6u) ^ sha256b_rotr(e, 11u) ^ sha256b_rotr(e, 25u);
        unsigned int ch = (unsigned int)((e & f) ^ (~e & g));
        unsigned int t1 = (unsigned int)(h + s1 + ch + sha256b_k[i] + w[i]);
        unsigned int s0 = sha256b_rotr(a, 2u) ^ sha256b_rotr(a, 13u) ^ sha256b_rotr(a, 22u);
        unsigned int mj = (unsigned int)((a & b) ^ (a & c) ^ (b & c));
        unsigned int t2 = (unsigned int)(s0 + mj);

        h = g; g = f; f = e; e = (unsigned int)(d + t1);
        d = c; c = b; b = a; a = (unsigned int)(t1 + t2);
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

SHA256B_FN void sha256b_init(struct sha256b_ctx *ctx)
{
    unsigned int i;

    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->total_len = 0u;
    ctx->buf_len = 0u;
    for (i = 0; i < SHA256B_BLOCK_LEN; i++)
        ctx->buf[i] = 0u;
}

/* len is 64-bit on purpose: the caller streams a whole file in chunks, and a
 * 32-bit length would silently truncate on a large image. */
SHA256B_FN void sha256b_update(struct sha256b_ctx *ctx, const void *data,
                               unsigned long long len)
{
    const unsigned char *p = (const unsigned char *)data;

    ctx->total_len += len;

    if (ctx->buf_len != 0u) {
        unsigned long long need = (unsigned long long)(SHA256B_BLOCK_LEN - ctx->buf_len);
        unsigned long long take = (len < need) ? len : need;
        unsigned long long i;

        for (i = 0; i < take; i++)
            ctx->buf[ctx->buf_len + (unsigned int)i] = p[i];
        ctx->buf_len += (unsigned int)take;
        p += take;
        len -= take;

        if (ctx->buf_len == SHA256B_BLOCK_LEN) {
            sha256b_compress(ctx->state, ctx->buf);
            ctx->buf_len = 0u;
        }
    }

    while (len >= (unsigned long long)SHA256B_BLOCK_LEN) {
        sha256b_compress(ctx->state, p);
        p += SHA256B_BLOCK_LEN;
        len -= SHA256B_BLOCK_LEN;
    }

    while (len > 0u) {
        ctx->buf[ctx->buf_len++] = *p++;
        len--;
    }
}

SHA256B_FN void sha256b_final(struct sha256b_ctx *ctx,
                              unsigned char out[SHA256B_DIGEST_LEN])
{
    unsigned long long bits = ctx->total_len * 8ull;
    unsigned int i;

    ctx->buf[ctx->buf_len++] = 0x80u;
    /* When the length field no longer fits behind the 0x80 marker the padding
     * spills into a SECOND block. That is the branch a single-vector self-test
     * walks straight past, which is why sha256b_selftest runs two. */
    if (ctx->buf_len > SHA256B_BLOCK_LEN - 8u) {
        while (ctx->buf_len < SHA256B_BLOCK_LEN)
            ctx->buf[ctx->buf_len++] = 0u;
        sha256b_compress(ctx->state, ctx->buf);
        ctx->buf_len = 0u;
    }
    while (ctx->buf_len < SHA256B_BLOCK_LEN - 8u)
        ctx->buf[ctx->buf_len++] = 0u;

    for (i = 0; i < 8u; i++)
        ctx->buf[SHA256B_BLOCK_LEN - 1u - i] = (unsigned char)(bits >> (8u * i));
    sha256b_compress(ctx->state, ctx->buf);

    for (i = 0; i < 8u; i++) {
        out[i * 4u + 0u] = (unsigned char)(ctx->state[i] >> 24);
        out[i * 4u + 1u] = (unsigned char)(ctx->state[i] >> 16);
        out[i * 4u + 2u] = (unsigned char)(ctx->state[i] >> 8);
        out[i * 4u + 3u] = (unsigned char)(ctx->state[i]);
    }
}

SHA256B_FN void sha256b(const void *data, unsigned long long len,
                        unsigned char out[SHA256B_DIGEST_LEN])
{
    struct sha256b_ctx ctx;

    sha256b_init(&ctx);
    sha256b_update(&ctx, data, len);
    sha256b_final(&ctx, out);
}

/* FIPS 180-4 known-answer vectors, run in the firmware environment before any
 * digest is published. TWO vectors rather than one: "abc" exercises a single
 * padded block, and the 56-byte vector exercises the two-block padding path
 * where the length field does not fit behind the marker. Returns 1 on success. */
SHA256B_FN int sha256b_selftest(void)
{
    static const unsigned char abc_expect[SHA256B_DIGEST_LEN] = {
        0xbau, 0x78u, 0x16u, 0xbfu, 0x8fu, 0x01u, 0xcfu, 0xeau,
        0x41u, 0x41u, 0x40u, 0xdeu, 0x5du, 0xaeu, 0x22u, 0x23u,
        0xb0u, 0x03u, 0x61u, 0xa3u, 0x96u, 0x17u, 0x7au, 0x9cu,
        0xb4u, 0x10u, 0xffu, 0x61u, 0xf2u, 0x00u, 0x15u, 0xadu,
    };
    /* SHA-256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") */
    static const unsigned char two_block_expect[SHA256B_DIGEST_LEN] = {
        0x24u, 0x8du, 0x6au, 0x61u, 0xd2u, 0x06u, 0x38u, 0xb8u,
        0xe5u, 0xc0u, 0x26u, 0x93u, 0x0cu, 0x3eu, 0x60u, 0x39u,
        0xa3u, 0x3cu, 0xe4u, 0x59u, 0x64u, 0xffu, 0x21u, 0x67u,
        0xf6u, 0xecu, 0xedu, 0xd4u, 0x19u, 0xdbu, 0x06u, 0xc1u,
    };
    static const char two_block_msg[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    unsigned char got[SHA256B_DIGEST_LEN];
    unsigned int i;

    sha256b("abc", 3ull, got);
    for (i = 0; i < SHA256B_DIGEST_LEN; i++) {
        if (got[i] != abc_expect[i])
            return 0;
    }

    sha256b(two_block_msg, (unsigned long long)(sizeof(two_block_msg) - 1u), got);
    for (i = 0; i < SHA256B_DIGEST_LEN; i++) {
        if (got[i] != two_block_expect[i])
            return 0;
    }
    return 1;
}

#endif /* SHA256_BOOT_H */
