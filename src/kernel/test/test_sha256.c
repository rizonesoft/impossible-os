/* test_sha256.c -- NIST FIPS 180-4 known-answer tests for the kernel SHA-256.
 * Pins the constants, message schedule, padding, and length encoding. No live
 * boot infrastructure (test policy).
 *
 * XREF: 01-boot-platform/TODO-13-tpm-measured-boot-attestation.md "PCR Replay
 * Engine" (SHA-256 prerequisite).
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/crypto/sha256.h"
#include "libc/string.h"

static const uint8_t KAT_EMPTY[32] = {
    0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
    0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55,
};
static const uint8_t KAT_ABC[32] = {
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
};
/* 56-byte message -- exercises the two-block padding path (56 + 0x80 > 56). */
static const uint8_t KAT_448[32] = {
    0x24,0x8d,0x6a,0x61,0xd2,0x06,0x38,0xb8,0xe5,0xc0,0x26,0x93,0x0c,0x3e,0x60,0x39,
    0xa3,0x3c,0xe4,0x59,0x64,0xff,0x21,0x67,0xf6,0xec,0xed,0xd4,0x19,0xdb,0x06,0xc1,
};
/* 1 000 000 * 'a' -- exercises many full blocks + streaming. */
static const uint8_t KAT_MILLION_A[32] = {
    0xcd,0xc7,0x6e,0x5c,0x99,0x14,0xfb,0x92,0x81,0xa1,0xc7,0xe2,0x84,0xd7,0x3e,0x67,
    0xf1,0x80,0x9a,0x48,0xa4,0x97,0x20,0x0e,0x04,0x6d,0x39,0xcc,0xc7,0x11,0x2c,0xd0,
};

static void test_sha256_kat_vectors(void)
{
    uint8_t out[32];

    sha256("", 0u, out);
    TEST_ASSERT(memcmp(out, KAT_EMPTY, 32) == 0, "SHA-256(\"\") matches NIST vector");

    sha256("abc", 3u, out);
    TEST_ASSERT(memcmp(out, KAT_ABC, 32) == 0, "SHA-256(\"abc\") matches NIST vector");

    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56u, out);
    TEST_ASSERT(memcmp(out, KAT_448, 32) == 0, "SHA-256(56-byte) matches NIST vector");
}

static void test_sha256_million_a_streaming(void)
{
    struct sha256_ctx ctx;
    uint8_t chunk[200];
    uint8_t out[32];
    uint32_t i;

    for (i = 0; i < sizeof(chunk); i++)
        chunk[i] = 'a';
    /* 1 000 000 = 5000 * 200, fed in uneven-ish streaming chunks to cross block
     * boundaries inside sha256_update. */
    sha256_init(&ctx);
    for (i = 0; i < 5000u; i++)
        sha256_update(&ctx, chunk, sizeof(chunk));
    sha256_final(&ctx, out);
    TEST_ASSERT(memcmp(out, KAT_MILLION_A, 32) == 0,
                "SHA-256(1e6 * 'a') matches NIST vector (streaming)");
}

static void test_sha256_streaming_equals_oneshot(void)
{
    /* Byte-at-a-time streaming must equal the one-shot for the same input,
     * proving the partial-block buffering is correct across every boundary. */
    static const char *msg = "The quick brown fox jumps over the lazy dog";
    struct sha256_ctx ctx;
    uint8_t a[32], b[32];
    uint32_t i, n = 43u; /* strlen */

    sha256(msg, n, a);
    sha256_init(&ctx);
    for (i = 0; i < n; i++) {
        sha256_update(&ctx, msg + i, 1u);
        /* (NULL, 0) no-op separator after a partial block must be safe (no NULL
         * pointer arithmetic) and must not perturb the digest. */
        sha256_update(&ctx, (const void *)0, 0u);
    }
    sha256_final(&ctx, b);
    TEST_ASSERT(memcmp(a, b, 32) == 0, "byte-streamed SHA-256 (with NULL,0 separators) == one-shot");
    /* And a known value for that classic input. */
    TEST_ASSERT_EQ(a[0], 0xd7u, "fox digest byte 0 = 0xd7");
    TEST_ASSERT_EQ(a[31], 0x92u, "fox digest byte 31 = 0x92");
}

void test_register_sha256(void)
{
    test_suite_register_cat("crypto: SHA-256 NIST KAT", test_sha256_kat_vectors, TEST_CAT_SECURITY);
    test_suite_register_cat("crypto: SHA-256 1e6*a streaming", test_sha256_million_a_streaming, TEST_CAT_SECURITY);
    test_suite_register_cat("crypto: SHA-256 stream==oneshot", test_sha256_streaming_equals_oneshot, TEST_CAT_SECURITY);
}

#endif /* KERNEL_TESTS */
