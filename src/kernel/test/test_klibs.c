/* ============================================================================
 * test_klibs.c -- Kernel embedded libraries test suite (TODO-03)
 *
 * Section 5 coverage: vendored Monocypher primitives (known-answer vectors
 * computed with independent implementations -- Python hashlib blake2b and
 * the pyca/cryptography Ed25519/X25519, proving cross-implementation
 * interop) and the kernel CSPRNG.
 *
 * CSPRNG coverage uses the pure csprng_core_* helpers with explicit state
 * (never csprng_init -- live-boot-call test policy) plus the already-seeded
 * global output path. Later TODO-03 sections (LZ4, miniz, math) extend
 * this file per the TODO's Unit Tests section.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/types.h"
#include "kernel/csprng.h"
#include "kernel/mm/pmm.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/zw.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/service_numbers.h"
#include "libc/string.h"
#include "libs/monocypher/monocypher.h"
#include "libs/monocypher/monocypher-ed25519.h"

/* ---- Known-answer vectors (independent-implementation computed) ---- */

/* blake2b-512("abc"), matches RFC 7693 appendix A. */
static const uint8_t k_blake2b_abc[64] = {
    0xBA, 0x80, 0xA5, 0x3F, 0x98, 0x1C, 0x4D, 0x0D,
    0x6A, 0x27, 0x97, 0xB6, 0x9F, 0x12, 0xF6, 0xE9,
    0x4C, 0x21, 0x2F, 0x14, 0x68, 0x5A, 0xC4, 0xB7,
    0x4B, 0x12, 0xBB, 0x6F, 0xDB, 0xFF, 0xA2, 0xD1,
    0x7D, 0x87, 0xC5, 0x39, 0x2A, 0xAB, 0x79, 0x2D,
    0xC2, 0x52, 0xD5, 0xDE, 0x45, 0x33, 0xCC, 0x95,
    0x18, 0xD3, 0x8A, 0xA8, 0xDB, 0xF1, 0x92, 0x5A,
    0xB9, 0x23, 0x86, 0xED, 0xD4, 0x00, 0x99, 0x23,
};

/* Standard Ed25519 (SHA-512): seed -> public key -> signature of the
 * empty message, computed with pyca/cryptography. */
static const uint8_t k_ed_seed[32] = {
    0x9D, 0x61, 0xB1, 0x9D, 0xEF, 0xFD, 0x5A, 0x60,
    0xBA, 0x84, 0x4A, 0xF4, 0x92, 0xEC, 0x2C, 0xC4,
    0x44, 0x49, 0xC5, 0x69, 0x7B, 0x32, 0x69, 0x19,
    0x70, 0x3B, 0xAE, 0x03, 0x1B, 0x8B, 0x65, 0xFC,
};
static const uint8_t k_ed_pub[32] = {
    0x12, 0xF5, 0x0F, 0xC0, 0x3E, 0xA5, 0xD9, 0xF9,
    0x16, 0xE4, 0x23, 0x53, 0xFE, 0x48, 0x24, 0xF7,
    0x63, 0x5F, 0xAD, 0x6A, 0xA9, 0x85, 0xC2, 0x56,
    0xA0, 0xA1, 0xD5, 0xA2, 0x8F, 0xFF, 0x46, 0x1F,
};
static const uint8_t k_ed_sig[64] = {
    0x05, 0xB8, 0x10, 0xEE, 0x3D, 0x65, 0xA8, 0xB9,
    0x60, 0xF7, 0x8F, 0xA1, 0x2B, 0x76, 0x92, 0x48,
    0x43, 0x58, 0x3F, 0x50, 0xD7, 0x57, 0xF4, 0x17,
    0xC7, 0xCE, 0x00, 0xC8, 0xAC, 0xF7, 0x77, 0xCB,
    0x01, 0x49, 0xB7, 0x05, 0x40, 0xEB, 0xB3, 0xC2,
    0xC5, 0xF4, 0xC1, 0x77, 0xB4, 0x46, 0x81, 0x5F,
    0x93, 0xB4, 0xD3, 0x41, 0x69, 0x22, 0x96, 0xEC,
    0x09, 0x00, 0xC9, 0x07, 0xF2, 0x69, 0x07, 0x0B,
};

/* X25519 base-point multiplication, RFC 7748 section 6.1 (Alice). */
static const uint8_t k_x_priv[32] = {
    0x77, 0x07, 0x6D, 0x0A, 0x73, 0x18, 0xA5, 0x7D,
    0x3C, 0x16, 0xC1, 0x72, 0x51, 0xB2, 0x66, 0x45,
    0xDF, 0x4C, 0x2F, 0x87, 0xEB, 0xC0, 0x99, 0x2A,
    0xB1, 0x77, 0xFB, 0xA5, 0x1D, 0xB9, 0x2C, 0x2A,
};
static const uint8_t k_x_pub[32] = {
    0x85, 0x20, 0xF0, 0x09, 0x89, 0x30, 0xA7, 0x54,
    0x74, 0x8B, 0x7D, 0xDC, 0xB4, 0x3E, 0xF7, 0x5A,
    0x0D, 0xBF, 0x3A, 0x0D, 0x26, 0x38, 0x1A, 0xF4,
    0xEB, 0xA4, 0xA9, 0x8E, 0xAA, 0x9B, 0x4E, 0x6A,
};

/* ---- Monocypher primitives ---- */

static void test_blake2b_vector(void)
{
    uint8_t out[64];

    crypto_blake2b(out, sizeof(out), (const uint8_t *)"abc", 3);
    TEST_ASSERT_EQ(memcmp(out, k_blake2b_abc, sizeof(out)), 0,
                   "blake2b-512(\"abc\") matches RFC 7693 vector");
}

static void test_aead_roundtrip(void)
{
    static const uint8_t key[32]   = { 1, 2, 3 };   /* rest zero */
    static const uint8_t nonce[24] = { 9, 8, 7 };
    const char *msg = "hello kernel";
    uint8_t ct[16], pt[16], mac[16];
    uint32_t n = 12;

    crypto_aead_lock(ct, mac, key, nonce, NULL, 0,
                     (const uint8_t *)msg, n);
    TEST_ASSERT_NEQ(memcmp(ct, msg, n), 0,
                    "AEAD ciphertext differs from plaintext");

    TEST_ASSERT_EQ(crypto_aead_unlock(pt, mac, key, nonce, NULL, 0, ct, n),
                   0, "AEAD unlock authenticates");
    TEST_ASSERT_EQ(memcmp(pt, msg, n), 0, "AEAD round-trip plaintext");

    ct[3] ^= 0x01;  /* tamper one ciphertext byte */
    TEST_ASSERT_EQ(crypto_aead_unlock(pt, mac, key, nonce, NULL, 0, ct, n),
                   -1, "AEAD unlock rejects tampered ciphertext");
}

static void test_x25519_vector(void)
{
    uint8_t pub[32];

    crypto_x25519_public_key(pub, k_x_priv);
    TEST_ASSERT_EQ(memcmp(pub, k_x_pub, 32), 0,
                   "x25519 public key matches RFC 7748 vector");
}

static void test_ed25519_standard(void)
{
    uint8_t seed[32], secret[64], pub[32], sig[64];

    /* key_pair WIPES its seed argument -- pass a mutable copy. */
    memcpy(seed, k_ed_seed, sizeof(seed));
    crypto_ed25519_key_pair(secret, pub, seed);
    TEST_ASSERT_EQ(memcmp(pub, k_ed_pub, 32), 0,
                   "ed25519 public key matches independent implementation");

    crypto_ed25519_sign(sig, secret, NULL, 0);
    TEST_ASSERT_EQ(memcmp(sig, k_ed_sig, 64), 0,
                   "ed25519 empty-message signature matches");
    TEST_ASSERT_EQ(crypto_ed25519_check(sig, pub, NULL, 0), 0,
                   "ed25519 signature verifies");

    sig[0] ^= 0x01;
    TEST_ASSERT_NEQ(crypto_ed25519_check(sig, pub, NULL, 0), 0,
                    "ed25519 rejects corrupted signature");
}

static void test_eddsa_blake2b_roundtrip(void)
{
    uint8_t seed[32], secret[64], pub[32], sig[64];
    const char *msg = "EIF native signing";

    memset(seed, 0x42, sizeof(seed));
    crypto_eddsa_key_pair(secret, pub, seed);
    crypto_eddsa_sign(sig, secret, (const uint8_t *)msg, 18);
    TEST_ASSERT_EQ(crypto_eddsa_check(sig, pub, (const uint8_t *)msg, 18),
                   0, "EdDSA-blake2b sign/check round-trip");
    sig[10] ^= 0x80;
    TEST_ASSERT_NEQ(crypto_eddsa_check(sig, pub, (const uint8_t *)msg, 18),
                    0, "EdDSA-blake2b rejects corrupted signature");
}

static void test_argon2id_smoke(void)
{
    /* Minimum legal config: 8 blocks = 8 KiB work area (> 4 KiB, so PMM). */
    crypto_argon2_config cfg = {
        .algorithm = CRYPTO_ARGON2_ID,
        .nb_blocks = 8,
        .nb_passes = 1,
        .nb_lanes  = 1,
    };
    crypto_argon2_inputs in = {
        .pass      = (const uint8_t *)"correct horse battery staple",
        .salt      = (const uint8_t *)"impossible-salt!",
        .pass_size = 28,
        .salt_size = 16,
    };
    uint8_t h1[32], h2[32];
    uintptr_t work = pmm_alloc_contiguous(2);  /* 8 KiB */

    TEST_ASSERT(work != 0, "argon2 work area allocated");
    if (work == 0)
        return;

    crypto_argon2(h1, sizeof(h1), (void *)work, cfg, in,
                  crypto_argon2_no_extras);
    crypto_argon2(h2, sizeof(h2), (void *)work, cfg, in,
                  crypto_argon2_no_extras);
    TEST_ASSERT_EQ(memcmp(h1, h2, 32), 0, "argon2id is deterministic");

    in.salt = (const uint8_t *)"different-salt!!";
    crypto_argon2(h2, sizeof(h2), (void *)work, cfg, in,
                  crypto_argon2_no_extras);
    TEST_ASSERT_NEQ(memcmp(h1, h2, 32), 0, "argon2id salt changes hash");

    pmm_free_frame(work);
    pmm_free_frame(work + 4096);
}

/* ---- Kernel CSPRNG (pure core; never touches csprng_init) ---- */

/* Golden vectors for the core composition, computed with TWO independent
 * implementations that agreed byte-for-byte: Python hashlib blake2b +
 * pyca/cryptography ChaCha20, and host-compiled upstream Monocypher.
 * Transcript is the 32-byte string below INCLUDING its NUL terminator. */
static const uint8_t k_core_transcript[] = "fixed seed transcript for tests";
static const uint8_t k_core_seed_key[32] = {
    0x16, 0xE5, 0x35, 0xA5, 0xDC, 0x3E, 0x3E, 0x4F,
    0x8A, 0x56, 0x82, 0xBB, 0x89, 0xEA, 0x1C, 0xC6,
    0x83, 0xDE, 0x5F, 0x9A, 0x18, 0x0F, 0xAA, 0x01,
    0x10, 0xD3, 0x16, 0xAB, 0x3F, 0xC4, 0xAB, 0x13,
};
static const uint8_t k_core_newkey[32] = {
    0x4C, 0x49, 0x15, 0x9F, 0xC6, 0x9E, 0xA5, 0x91,
    0xDB, 0x96, 0xB2, 0x6B, 0xB3, 0xDD, 0x5E, 0x0E,
    0x70, 0x7A, 0x97, 0x76, 0x2A, 0xDC, 0x0E, 0x42,
    0xAF, 0xFD, 0x23, 0xAF, 0xA1, 0xD2, 0xA8, 0x49,
};
static const uint8_t k_core_reqkey[32] = {
    0x85, 0xE9, 0x84, 0x1C, 0x3A, 0xC1, 0x56, 0x6D,
    0xFB, 0xC2, 0x3B, 0xEB, 0x5B, 0x1A, 0xBE, 0x36,
    0x4E, 0x24, 0xAA, 0x2A, 0xC4, 0x72, 0x78, 0x41,
    0x6A, 0x1D, 0x27, 0xB8, 0x09, 0x1C, 0x28, 0x53,
};
static const uint8_t k_core_stream48[48] = {
    0x6E, 0x81, 0x01, 0x84, 0x04, 0x57, 0xD0, 0x46,
    0xB5, 0x2A, 0xCE, 0x3E, 0xD6, 0x73, 0x4B, 0x93,
    0x74, 0x56, 0x73, 0x71, 0x0B, 0xFE, 0x2E, 0x3E,
    0xF0, 0x51, 0x0C, 0x37, 0xEB, 0x57, 0xD9, 0xBB,
    0x8A, 0xD6, 0x8B, 0xF9, 0xF1, 0xAA, 0x36, 0x54,
    0xE8, 0x5D, 0xBE, 0x77, 0x4C, 0x0F, 0x3D, 0x78,
};

static void test_csprng_core_vectors(void)
{
    csprng_core_t c;
    uint8_t rkey[CSPRNG_KEY_SIZE];
    uint8_t stream[48 + 2];   /* +2 canary bytes after the payload */

    csprng_core_seed(&c, k_core_transcript, sizeof(k_core_transcript));
    TEST_ASSERT_EQ(memcmp(c.key, k_core_seed_key, 32), 0,
                   "core seed key matches independent blake2b-256");

    csprng_core_ratchet(&c, rkey);
    TEST_ASSERT_EQ(memcmp(c.key, k_core_newkey, 32), 0,
                   "ratchet next-key matches independent chacha20");
    TEST_ASSERT_EQ(memcmp(rkey, k_core_reqkey, 32), 0,
                   "ratchet request-key matches independent chacha20");

    memset(stream, 0xC5, sizeof(stream));
    csprng_core_stream(rkey, stream, 48);
    TEST_ASSERT_EQ(memcmp(stream, k_core_stream48, 48), 0,
                   "48-byte stream matches independent chacha20");
    TEST_ASSERT(stream[48] == 0xC5 && stream[49] == 0xC5,
                "stream does not write past the requested length");

    /* Odd lengths: 1 and 63 are prefixes of the same keystream. */
    memset(stream, 0xC5, sizeof(stream));
    csprng_core_stream(rkey, stream, 1);
    TEST_ASSERT_EQ(stream[0], k_core_stream48[0], "1-byte stream prefix");
    TEST_ASSERT_EQ(stream[1], 0xC5, "1-byte stream canary intact");
    memset(stream, 0xC5, sizeof(stream));
    csprng_core_stream(rkey, stream, 47);
    TEST_ASSERT_EQ(memcmp(stream, k_core_stream48, 47), 0,
                   "47-byte stream is a prefix of the 48-byte stream");
    TEST_ASSERT_EQ(stream[47], 0xC5, "47-byte stream canary intact");
}

static void test_csprng_absorb_policy(void)
{
    csprng_core_t c;
    uint8_t digest[CSPRNG_KEY_SIZE];
    uint8_t key_low[CSPRNG_KEY_SIZE];
    int seeded;

    memset(&c, 0, sizeof(c));
    memset(digest, 0x11, sizeof(digest));

    /* Unseeded + LOW: material absorbed, but does NOT mark seeded. */
    seeded = csprng_core_absorb_digest(&c, 0, digest, ENTROPY_Q_LOW);
    TEST_ASSERT_EQ(seeded, 0, "LOW input does not mark unseeded core seeded");
    memcpy(key_low, c.key, sizeof(key_low));

    /* Unseeded + HIGH: marks seeded. */
    memset(&c, 0, sizeof(c));
    seeded = csprng_core_absorb_digest(&c, 0, digest, ENTROPY_Q_HIGH);
    TEST_ASSERT_EQ(seeded, 1, "HIGH input marks unseeded core seeded");
    TEST_ASSERT_EQ(memcmp(c.key, key_low, sizeof(key_low)), 0,
                   "seed key depends on material, not on the quality class");

    /* Seeded: absorb is a reseed, stays seeded, diverges the key. */
    seeded = csprng_core_absorb_digest(&c, 1, digest, ENTROPY_Q_LOW);
    TEST_ASSERT_EQ(seeded, 1, "seeded core stays seeded on LOW reseed");
    TEST_ASSERT_NEQ(memcmp(c.key, key_low, sizeof(key_low)), 0,
                    "reseed diverges the key");
}

static void test_csprng_add_entropy_global(void)
{
    uint8_t material[16];
    uint8_t out[16];

    /* No-op cases must not change seeded state (and must not crash). */
    csprng_add_entropy(NULL, 16, ENTROPY_Q_HIGH);
    csprng_add_entropy(material, 0, ENTROPY_Q_HIGH);
    TEST_ASSERT_EQ(csprng_is_seeded(), 1,
                   "NULL/zero add_entropy leaves global seeded state intact");

    /* Real mix keeps the generator seeded and producing output. */
    memset(material, 0x3C, sizeof(material));
    csprng_add_entropy(material, sizeof(material), ENTROPY_Q_HIGH);
    TEST_ASSERT_EQ(csprng_is_seeded(), 1,
                   "add_entropy(HIGH) preserves seeded state");
    memset(out, 0, sizeof(out));
    csprng_fill(out, sizeof(out));
    {
        uint8_t zeros[16];

        memset(zeros, 0, sizeof(zeros));
        TEST_ASSERT_NEQ(memcmp(out, zeros, sizeof(out)), 0,
                        "fill still produces output after add_entropy");
    }
}

static void test_csprng_core(void)
{
    static const uint8_t transcript[] = "fixed seed transcript for tests";
    csprng_core_t a, b;
    uint8_t ka[CSPRNG_KEY_SIZE], kb[CSPRNG_KEY_SIZE];
    uint8_t sa[48], sb[48];
    uint8_t key_before[CSPRNG_KEY_SIZE];

    /* Determinism: identical transcripts produce identical streams. */
    csprng_core_seed(&a, transcript, sizeof(transcript));
    csprng_core_seed(&b, transcript, sizeof(transcript));
    TEST_ASSERT_EQ(memcmp(a.key, b.key, CSPRNG_KEY_SIZE), 0,
                   "core seed is deterministic");

    memcpy(key_before, a.key, CSPRNG_KEY_SIZE);
    csprng_core_ratchet(&a, ka);
    csprng_core_ratchet(&b, kb);
    TEST_ASSERT_EQ(memcmp(ka, kb, CSPRNG_KEY_SIZE), 0,
                   "ratchet request keys match for equal states");

    /* Forward secrecy: the ratchet must replace the core key, and the
     * request key must differ from both old and new core keys. */
    TEST_ASSERT_NEQ(memcmp(a.key, key_before, CSPRNG_KEY_SIZE), 0,
                    "ratchet erases the previous core key");
    TEST_ASSERT_NEQ(memcmp(ka, a.key, CSPRNG_KEY_SIZE), 0,
                    "request key differs from new core key");

    csprng_core_stream(ka, sa, sizeof(sa));
    csprng_core_stream(kb, sb, sizeof(sb));
    TEST_ASSERT_EQ(memcmp(sa, sb, sizeof(sa)), 0,
                   "stream is a pure function of the request key");

    /* Reseed: mixing a digest changes subsequent output. */
    {
        uint8_t digest[CSPRNG_KEY_SIZE];

        memset(digest, 0x5A, sizeof(digest));
        csprng_core_reseed(&a, digest);
        csprng_core_ratchet(&a, ka);
        csprng_core_stream(ka, sa, sizeof(sa));
        TEST_ASSERT_NEQ(memcmp(sa, sb, sizeof(sa)), 0,
                        "reseed diverges the keystream");
    }
}

static void test_csprng_global(void)
{
    uint8_t b1[32], b2[32];
    uint64_t v[64];
    uint32_t i, j, dup = 0;

    TEST_ASSERT_EQ(csprng_is_seeded(), 1, "global CSPRNG seeded at boot");

    memset(b1, 0, sizeof(b1));
    memset(b2, 0, sizeof(b2));
    csprng_fill(b1, sizeof(b1));
    csprng_fill(b2, sizeof(b2));
    TEST_ASSERT_NEQ(memcmp(b1, b2, sizeof(b1)), 0,
                    "two csprng_fill outputs differ");

    for (i = 0; i < 64; i++)
        v[i] = csprng_u64();
    for (i = 0; i < 64; i++)
        for (j = i + 1; j < 64; j++)
            if (v[i] == v[j])
                dup++;
    TEST_ASSERT_EQ(dup, 0, "csprng_u64 produces no duplicates in 64 draws");
}

static void test_nt_get_random(void)
{
    extern NTSTATUS NtGetRandom(void *user_buf, uint64_t len, uint64_t flags);
    uint8_t buf[64];
    uint32_t saved_mode = ssdt_previous_mode();

    /* Parameter validation. */
    TEST_ASSERT_EQ((uint32_t)NtGetRandom(buf, 0, 0),
                   (uint32_t)STATUS_INVALID_PARAMETER,
                   "NtGetRandom rejects len=0");
    TEST_ASSERT_EQ((uint32_t)NtGetRandom(buf, sizeof(buf), 1),
                   (uint32_t)STATUS_INVALID_PARAMETER,
                   "NtGetRandom rejects nonzero flags");
    TEST_ASSERT_EQ((uint32_t)NtGetRandom(buf, CSPRNG_GETRANDOM_MAX + 1, 0),
                   (uint32_t)STATUS_INVALID_PARAMETER,
                   "NtGetRandom rejects oversized request");

    /* Kernel-mode positive path (probe skipped for kernel callers). */
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQ((uint32_t)NtGetRandom(buf, sizeof(buf), 0),
                   (uint32_t)STATUS_SUCCESS,
                   "NtGetRandom fills kernel buffer in kernel mode");
    {
        uint8_t zeros[64];

        memset(zeros, 0, sizeof(zeros));
        TEST_ASSERT_NEQ(memcmp(buf, zeros, sizeof(buf)), 0,
                        "NtGetRandom output is non-zero");
    }

    /* User-mode probe rejection: a buffer at the user probe boundary must
     * fail ProbeForWrite (the range [boundary, boundary+64) is not user
     * memory) and NtGetRandom returns before touching it. NOTE: a kernel
     * stack address canNOT serve here -- kernel stacks are identity-mapped
     * BELOW MM_USER_PROBE_ADDRESS, so the range check accepts them.
     * Save/restore the previous-mode slot (plain setter, no live boot
     * infrastructure). */
    ssdt_set_previous_mode(SSDT_USER_MODE);
    TEST_ASSERT_EQ((uint32_t)NtGetRandom((void *)MM_USER_PROBE_ADDRESS,
                                         sizeof(buf), 0),
                   (uint32_t)STATUS_ACCESS_VIOLATION,
                   "NtGetRandom probes user buffers in user mode");

    /* User-mode POSITIVE path: exercises probe-accept + copy_to_user end to
     * end. The buffer sits below MM_USER_PROBE_ADDRESS, which is all the
     * current NT user-range contract checks (per-process User-PTE
     * validation is the systemic SMAP/KPTI work, not this handler -- see
     * the NtGetRandom header XREF). This asserts the success path runs, NOT
     * that kernel memory is a legitimate user buffer. */
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQ((uint32_t)NtGetRandom(buf, sizeof(buf), 0),
                   (uint32_t)STATUS_SUCCESS,
                   "NtGetRandom probe-accept + copy_to_user success path");
    ssdt_set_previous_mode(saved_mode);
}

/* Chunked-copy boundaries: lengths around the 256-byte bounce buffer
 * (1, 255, 256, 257, 513) with canaries on both sides of the payload. */
static void test_nt_get_random_chunks(void)
{
    extern NTSTATUS NtGetRandom(void *user_buf, uint64_t len, uint64_t flags);
    static const uint64_t lens[] = { 1, 255, 256, 257, 513 };
    /* 8 canary bytes | payload (max 513) | 8 canary bytes */
    static uint8_t arena[8 + 513 + 8];
    uint32_t i, k;

    for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        uint64_t n = lens[i];
        int front_ok = 1, back_ok = 1, all_zero = 1;

        memset(arena, 0xA7, sizeof(arena));
        memset(arena + 8, 0, (size_t)n);
        TEST_ASSERT_EQ((uint32_t)NtGetRandom(arena + 8, n, 0),
                       (uint32_t)STATUS_SUCCESS,
                       "NtGetRandom chunked length succeeds");
        for (k = 0; k < 8; k++) {
            if (arena[k] != 0xA7) front_ok = 0;
            if (arena[8 + n + k] != 0xA7) back_ok = 0;
        }
        for (k = 0; k < n; k++)
            if (arena[8 + k] != 0) { all_zero = 0; break; }
        TEST_ASSERT(front_ok, "no underwrite before the buffer");
        TEST_ASSERT(back_ok, "no overwrite past the requested length");
        TEST_ASSERT(!all_zero, "payload bytes were written");
    }
}

/* Real syscall wiring: service number 0x03D8 must reach the handler via
 * ssdt_dispatch (proves csprng_register_ssdt ran during boot). */
static void test_nt_get_random_dispatch(void)
{
    uint8_t buf[32];
    uint8_t zeros[32];

    memset(buf, 0, sizeof(buf));
    memset(zeros, 0, sizeof(zeros));
    TEST_ASSERT_EQ((uint32_t)ssdt_dispatch(SSDT_NtGetRandom,
                                           (uint64_t)(uintptr_t)buf,
                                           sizeof(buf), 0, 0, 0, 0),
                   (uint32_t)STATUS_SUCCESS,
                   "ssdt_dispatch(SSDT_NtGetRandom) reaches the handler");
    TEST_ASSERT_NEQ(memcmp(buf, zeros, sizeof(buf)), 0,
                    "dispatched NtGetRandom wrote random bytes");
    TEST_ASSERT_EQ((uint32_t)ssdt_dispatch(SSDT_NtGetRandom,
                                           (uint64_t)(uintptr_t)buf,
                                           0, 0, 0, 0, 0),
                   (uint32_t)STATUS_INVALID_PARAMETER,
                   "dispatched NtGetRandom validates parameters");
}

void test_register_klibs(void)
{
    test_suite_register_cat("klibs: blake2b vector",
                            test_blake2b_vector, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: chacha20-poly1305 AEAD",
                            test_aead_roundtrip, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: x25519 vector",
                            test_x25519_vector, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: ed25519 standard",
                            test_ed25519_standard, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: eddsa-blake2b roundtrip",
                            test_eddsa_blake2b_roundtrip, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: argon2id smoke",
                            test_argon2id_smoke, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: csprng core",
                            test_csprng_core, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: csprng core vectors",
                            test_csprng_core_vectors, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: csprng absorb policy",
                            test_csprng_absorb_policy, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: csprng add_entropy",
                            test_csprng_add_entropy_global, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: csprng global",
                            test_csprng_global, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: NtGetRandom",
                            test_nt_get_random, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: NtGetRandom chunks",
                            test_nt_get_random_chunks, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: NtGetRandom dispatch",
                            test_nt_get_random_dispatch, TEST_CAT_EXEC);
}
