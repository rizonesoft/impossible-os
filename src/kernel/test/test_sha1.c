/* test_sha1.c -- NIST FIPS 180-4 known-answer tests for the kernel SHA-1.
 * Pins the constants, round functions, padding, and length encoding. No live
 * boot infrastructure (test policy).
 *
 * XREF: 01-boot-platform/TODO-13-tpm-measured-boot-attestation.md "PCR Replay
 * Engine" (SHA-1 legacy-bank prerequisite).
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/crypto/sha1.h"
#include "libc/string.h"

static const uint8_t KAT_EMPTY[20] = {
    0xda,0x39,0xa3,0xee,0x5e,0x6b,0x4b,0x0d,0x32,0x55,0xbf,0xef,0x95,0x60,0x18,0x90,0xaf,0xd8,0x07,0x09,
};
static const uint8_t KAT_ABC[20] = {
    0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d,
};
/* 56-byte message -- two-block padding path. */
static const uint8_t KAT_448[20] = {
    0x84,0x98,0x3e,0x44,0x1c,0x3b,0xd2,0x6e,0xba,0xae,0x4a,0xa1,0xf9,0x51,0x29,0xe5,0xe5,0x46,0x70,0xf1,
};
/* 1 000 000 * 'a'. */
static const uint8_t KAT_MILLION_A[20] = {
    0x34,0xaa,0x97,0x3c,0xd4,0xc4,0xda,0xa4,0xf6,0x1e,0xeb,0x2b,0xdb,0xad,0x27,0x31,0x65,0x34,0x01,0x6f,
};

static void test_sha1_kat_vectors(void)
{
    uint8_t out[20];

    sha1("", 0u, out);
    TEST_ASSERT(memcmp(out, KAT_EMPTY, 20) == 0, "SHA-1(\"\") matches NIST vector");

    sha1("abc", 3u, out);
    TEST_ASSERT(memcmp(out, KAT_ABC, 20) == 0, "SHA-1(\"abc\") matches NIST vector");

    sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56u, out);
    TEST_ASSERT(memcmp(out, KAT_448, 20) == 0, "SHA-1(56-byte) matches NIST vector");
}

static void test_sha1_million_a_streaming(void)
{
    struct sha1_ctx ctx;
    uint8_t chunk[200];
    uint8_t out[20];
    uint32_t i;

    for (i = 0; i < sizeof(chunk); i++)
        chunk[i] = 'a';
    sha1_init(&ctx);
    for (i = 0; i < 5000u; i++) {
        sha1_update(&ctx, chunk, sizeof(chunk));
        sha1_update(&ctx, (const void *)0, 0u);  /* NULL,0 no-op separator must be safe */
    }
    sha1_final(&ctx, out);
    TEST_ASSERT(memcmp(out, KAT_MILLION_A, 20) == 0,
                "SHA-1(1e6 * 'a') matches NIST vector (streaming w/ NULL,0 separators)");
}

static void test_sha1_streaming_equals_oneshot(void)
{
    static const char *msg = "The quick brown fox jumps over the lazy dog";
    struct sha1_ctx ctx;
    uint8_t a[20], b[20];
    uint32_t i, n = 43u;

    sha1(msg, n, a);
    sha1_init(&ctx);
    for (i = 0; i < n; i++)
        sha1_update(&ctx, msg + i, 1u);
    sha1_final(&ctx, b);
    TEST_ASSERT(memcmp(a, b, 20) == 0, "byte-streamed SHA-1 == one-shot");
    /* SHA-1("...lazy dog") = 2fd4e1c67a2d28fced849ee1bb76e7391b93eb12 */
    TEST_ASSERT_EQ(a[0], 0x2fu, "fox digest byte 0 = 0x2f");
    TEST_ASSERT_EQ(a[19], 0x12u, "fox digest byte 19 = 0x12");
}

void test_register_sha1(void)
{
    test_suite_register_cat("crypto: SHA-1 NIST KAT", test_sha1_kat_vectors, TEST_CAT_SECURITY);
    test_suite_register_cat("crypto: SHA-1 1e6*a streaming", test_sha1_million_a_streaming, TEST_CAT_SECURITY);
    test_suite_register_cat("crypto: SHA-1 stream==oneshot", test_sha1_streaming_equals_oneshot, TEST_CAT_SECURITY);
}

#endif /* KERNEL_TESTS */
