/* test_sha384.c -- NIST FIPS 180-4 known-answer tests for kernel SHA-384.
 * Verifies the SHA-384 IV install + 48-byte truncation over the monocypher
 * SHA-512 compression. No live boot infrastructure (test policy).
 *
 * XREF: 01-boot-platform/TODO-13-tpm-measured-boot-attestation.md "PCR Replay
 * Engine" (SHA-384 bank prerequisite).
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/crypto/sha384.h"
#include "libc/string.h"

static const uint8_t KAT_EMPTY[48] = {
    0x38,0xb0,0x60,0xa7,0x51,0xac,0x96,0x38,0x4c,0xd9,0x32,0x7e,0xb1,0xb1,0xe3,0x6a,
    0x21,0xfd,0xb7,0x11,0x14,0xbe,0x07,0x43,0x4c,0x0c,0xc7,0xbf,0x63,0xf6,0xe1,0xda,
    0x27,0x4e,0xde,0xbf,0xe7,0x6f,0x65,0xfb,0xd5,0x1a,0xd2,0xf1,0x48,0x98,0xb9,0x5b,
};
static const uint8_t KAT_ABC[48] = {
    0xcb,0x00,0x75,0x3f,0x45,0xa3,0x5e,0x8b,0xb5,0xa0,0x3d,0x69,0x9a,0xc6,0x50,0x07,
    0x27,0x2c,0x32,0xab,0x0e,0xde,0xd1,0x63,0x1a,0x8b,0x60,0x5a,0x43,0xff,0x5b,0xed,
    0x80,0x86,0x07,0x2b,0xa1,0xe7,0xcc,0x23,0x58,0xba,0xec,0xa1,0x34,0xc8,0x25,0xa7,
};
/* 112-byte two-block message (FIPS 180-4 SHA-384 example 2). */
static const uint8_t KAT_896[48] = {
    0x09,0x33,0x0c,0x33,0xf7,0x11,0x47,0xe8,0x3d,0x19,0x2f,0xc7,0x82,0xcd,0x1b,0x47,
    0x53,0x11,0x1b,0x17,0x3b,0x3b,0x05,0xd2,0x2f,0xa0,0x80,0x86,0xe3,0xb0,0xf7,0x12,
    0xfc,0xc7,0xc7,0x1a,0x55,0x7e,0x2d,0xb9,0x66,0xc3,0xe9,0xfa,0x91,0x74,0x60,0x39,
};

static void test_sha384_kat_vectors(void)
{
    uint8_t out[48];

    sha384("", 0u, out);
    TEST_ASSERT(memcmp(out, KAT_EMPTY, 48) == 0, "SHA-384(\"\") matches NIST vector");

    sha384("abc", 3u, out);
    TEST_ASSERT(memcmp(out, KAT_ABC, 48) == 0, "SHA-384(\"abc\") matches NIST vector");

    sha384("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
           "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 112u, out);
    TEST_ASSERT(memcmp(out, KAT_896, 48) == 0, "SHA-384(112-byte) matches NIST vector");
}

static void test_sha384_streaming_equals_oneshot(void)
{
    static const char *msg = "The quick brown fox jumps over the lazy dog";
    struct sha384_ctx ctx;
    uint8_t a[48], b[48];
    uint32_t i, n = 43u;

    sha384(msg, n, a);
    sha384_init(&ctx);
    for (i = 0; i < n; i++) {
        sha384_update(&ctx, msg + i, 1u);
        sha384_update(&ctx, (const void *)0, 0u);  /* NULL,0 no-op separator */
    }
    sha384_final(&ctx, b);
    TEST_ASSERT(memcmp(a, b, 48) == 0, "byte-streamed SHA-384 == one-shot");
    /* SHA-384("...lazy dog") = ca737f1014a48f4c0b6dd43cb177b0afd9e5169367544c49
     * 4011e3317dbf9a509cb1e5dc1e85a941bbee3d7f2afbc9b1 */
    TEST_ASSERT_EQ(a[0], 0xcau, "fox digest byte 0 = 0xca");
    TEST_ASSERT_EQ(a[47], 0xb1u, "fox digest byte 47 = 0xb1");
}

void test_register_sha384(void)
{
    test_suite_register_cat("crypto: SHA-384 NIST KAT", test_sha384_kat_vectors, TEST_CAT_SECURITY);
    test_suite_register_cat("crypto: SHA-384 stream==oneshot", test_sha384_streaming_equals_oneshot, TEST_CAT_SECURITY);
}

#endif /* KERNEL_TESTS */
