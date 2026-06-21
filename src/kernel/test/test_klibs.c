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
#include "kernel/entropy.h"
#include "kernel/mm/pmm.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/zw.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/service_numbers.h"
#include "libc/string.h"
#include "libc/math.h"
#include "kernel/json.h"
#include "kernel/kchecksum.h"
#include "kernel/kcodec.h"
#include "libs/lz4.h"
#include "kernel/cpuid.h"
#include "kernel/mm/heap.h"
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

/* Section 10 item 1: source-mixing determinism on a SOURCE-FRAMED transcript
 * (not just a raw string). entropy_frame_source builds a multi-source
 * length-tagged transcript; the bytes are pinned to a golden layout, and the
 * mixer keys that exact transcript to a known blake2b-256 digest computed by
 * an independent reference (Python hashlib, byte-verified against the in-tree
 * RFC 7693 vector). A regression that dropped a source tag/length or reordered
 * records would change the framed bytes and the digest. */
static const uint8_t k_framed_transcript[17] = {
    /* src=ENTROPY_SRC_FW_RNG(0), len=4 LE, payload DE AD BE EF */
    0x00, 0x04, 0x00, 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF,
    /* src=ENTROPY_SRC_TPM_RNG(2), len=3 LE, payload 11 22 33 */
    0x02, 0x03, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33,
};
static const uint8_t k_framed_seed_key[32] = {
    0xC0, 0x17, 0xF0, 0x5B, 0xC1, 0x0F, 0x66, 0x5C,
    0xF6, 0x0E, 0x49, 0x51, 0x8A, 0xE2, 0x8C, 0xBE,
    0x15, 0x01, 0x89, 0x39, 0xBE, 0x2B, 0x10, 0xA2,
    0xF4, 0x9B, 0x58, 0xFB, 0xAB, 0xD3, 0x84, 0x43,
};
/* Same two records in TPM-first order -- pins the swapped negative case so
 * order-sensitivity cannot pass on a malformed (empty/truncated) frame. */
static const uint8_t k_framed_swapped[17] = {
    /* src=ENTROPY_SRC_TPM_RNG(2), len=3 LE, payload 11 22 33 */
    0x02, 0x03, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33,
    /* src=ENTROPY_SRC_FW_RNG(0), len=4 LE, payload DE AD BE EF */
    0x00, 0x04, 0x00, 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF,
};

static void test_csprng_source_framed_vector(void)
{
    static const uint8_t fw[4]  = { 0xDE, 0xAD, 0xBE, 0xEF };
    static const uint8_t tpm[3] = { 0x11, 0x22, 0x33 };
    uint8_t framed[64];
    uint8_t swapped[64];
    csprng_core_t a, b;
    uint32_t n, n2;

    /* Build the framed transcript and pin its byte layout (src tags, LE
     * lengths, ordering, concatenation). */
    n = entropy_frame_source(framed, sizeof(framed), 0,
                             ENTROPY_SRC_FW_RNG, fw, sizeof(fw));
    n = entropy_frame_source(framed, sizeof(framed), n,
                             ENTROPY_SRC_TPM_RNG, tpm, sizeof(tpm));
    TEST_ASSERT_EQ(n, (uint32_t)sizeof(k_framed_transcript),
                   "two-source framed transcript length");
    TEST_ASSERT_EQ(memcmp(framed, k_framed_transcript, sizeof(k_framed_transcript)),
                   0, "framed transcript matches golden byte layout");

    /* Known digest: the mixer keys the framed transcript to the independently
     * computed blake2b-256 of exactly those bytes. */
    csprng_core_seed(&a, framed, n);
    TEST_ASSERT_EQ(memcmp(a.key, k_framed_seed_key, CSPRNG_KEY_SIZE), 0,
                   "framed transcript seeds the known blake2b-256 digest");

    /* Determinism: same framed input -> same key. */
    csprng_core_seed(&b, framed, n);
    TEST_ASSERT_EQ(memcmp(a.key, b.key, CSPRNG_KEY_SIZE), 0,
                   "framed source mixing is deterministic");

    /* Order sensitivity: swapping the two records yields a different digest --
     * a regression that dropped src tags/lengths would collide these. Pin the
     * swapped transcript first so the negative case cannot pass on a malformed
     * (empty / truncated) frame. */
    n2 = entropy_frame_source(swapped, sizeof(swapped), 0,
                              ENTROPY_SRC_TPM_RNG, tpm, sizeof(tpm));
    n2 = entropy_frame_source(swapped, sizeof(swapped), n2,
                              ENTROPY_SRC_FW_RNG, fw, sizeof(fw));
    TEST_ASSERT_EQ(n2, (uint32_t)sizeof(k_framed_swapped),
                   "swapped transcript framing length");
    TEST_ASSERT_EQ(memcmp(swapped, k_framed_swapped, sizeof(k_framed_swapped)), 0,
                   "swapped transcript matches golden swapped layout");
    csprng_core_seed(&b, swapped, n2);
    TEST_ASSERT_NEQ(memcmp(a.key, b.key, CSPRNG_KEY_SIZE), 0,
                    "source order changes the mixed digest");
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

    /* Two-part seed: seed2(t1, t2) must equal seed(t1 || t2) exactly
     * (csprng_init folds the boot_info seed transcript via this path). */
    {
        static const uint8_t t1[] = "first transcript part";
        static const uint8_t t2[] = "and the boot payload part";
        uint8_t cat[sizeof(t1) + sizeof(t2)];

        memcpy(cat, t1, sizeof(t1));
        memcpy(cat + sizeof(t1), t2, sizeof(t2));
        csprng_core_seed(&a, cat, sizeof(cat));
        a.ctr = 0;  /* belt: compare from a known-equal baseline */
        b.ctr = 77; /* must be RESET by seed2, not inherited */
        csprng_core_seed2(&b, t1, sizeof(t1), t2, sizeof(t2));
        TEST_ASSERT_EQ(memcmp(a.key, b.key, CSPRNG_KEY_SIZE), 0,
                       "seed2 equals seed over the concatenation");
        TEST_ASSERT_EQ(b.ctr, 0u, "seed2 resets the block counter");
        /* Full-state equivalence: the first ratchet from both states
         * must produce identical request keys (catches any divergence
         * the key compare alone would miss). */
        csprng_core_ratchet(&a, ka);
        csprng_core_ratchet(&b, kb);
        TEST_ASSERT_EQ(memcmp(ka, kb, CSPRNG_KEY_SIZE), 0,
                       "first ratchet after seed2 matches seed");

        /* Either part may be absent: (t, NULL) == seed(t). */
        csprng_core_seed(&a, t1, sizeof(t1));
        csprng_core_seed2(&b, t1, sizeof(t1), NULL, 0);
        TEST_ASSERT_EQ(memcmp(a.key, b.key, CSPRNG_KEY_SIZE), 0,
                       "seed2 with empty second part equals seed");
        csprng_core_seed2(&b, NULL, 0, t1, sizeof(t1));
        TEST_ASSERT_EQ(memcmp(a.key, b.key, CSPRNG_KEY_SIZE), 0,
                       "seed2 with empty first part equals seed");
    }
}

static void test_csprng_crypto_gate(void)
{
    /* PURE upgrade rule matrix (kernel early CSPRNG seeding section). */
    TEST_ASSERT_EQ(csprng_class_upgrade(ENTROPY_CLASS_DEGRADED,
                                        ENTROPY_Q_HIGH, CSPRNG_KEY_SIZE),
                   ENTROPY_CLASS_MINIMUM,
                   "key-size HIGH absorb lifts degraded to minimum");
    TEST_ASSERT_EQ(csprng_class_upgrade(ENTROPY_CLASS_DEGRADED,
                                        ENTROPY_Q_HIGH,
                                        CSPRNG_KEY_SIZE - 1u),
                   ENTROPY_CLASS_DEGRADED,
                   "short HIGH absorb never upgrades");
    TEST_ASSERT_EQ(csprng_class_upgrade(ENTROPY_CLASS_DEGRADED,
                                        ENTROPY_Q_LOW, 4096u),
                   ENTROPY_CLASS_DEGRADED,
                   "LOW absorb never upgrades regardless of size");
    TEST_ASSERT_EQ(csprng_class_upgrade(ENTROPY_CLASS_MINIMUM,
                                        ENTROPY_Q_HIGH, 4096u),
                   ENTROPY_CLASS_MINIMUM,
                   "absorb can never prove GOOD (needs per-source provenance)");
    TEST_ASSERT_EQ(csprng_class_upgrade(ENTROPY_CLASS_GOOD,
                                        ENTROPY_Q_LOW, 8u),
                   ENTROPY_CLASS_GOOD, "upgrade rule never downgrades");
    TEST_ASSERT_EQ(csprng_class_upgrade(ENTROPY_CLASS_DEGRADED,
                                        ENTROPY_Q_NONE, CSPRNG_KEY_SIZE),
                   ENTROPY_CLASS_DEGRADED, "Q_NONE never upgrades");
    TEST_ASSERT_EQ(csprng_class_upgrade(ENTROPY_CLASS_DEGRADED,
                                        (entropy_quality_t)3, CSPRNG_KEY_SIZE),
                   ENTROPY_CLASS_DEGRADED,
                   "reserved quality value never upgrades");

    /* Read-only oracles against the live (boot-seeded) global: the gate
     * must be exactly seeded AND policy(credited, release-mode), and
     * fill_classified must report the same credited class. */
    {
        uint8_t buf[16];
        entropy_class_t credited = csprng_credited_class();

        /* The gate is UNCONDITIONALLY release-strict: the mutable
         * boot.conf debug byte must never relax key-grade readiness. */
        TEST_ASSERT_EQ((uint64_t)csprng_crypto_ok(),
                       (uint64_t)(csprng_is_seeded() &&
                                  entropy_policy_ok(credited, 1)),
                       "crypto gate == seeded AND release policy(credited)");
        memset(buf, 0, sizeof(buf));
        TEST_ASSERT_EQ(csprng_fill_classified(buf, sizeof(buf)), credited,
                       "fill_classified reports the credited class");
        {
            uint64_t sum = 0;
            uint32_t i;
            for (i = 0; i < sizeof(buf); i++)
                sum += buf[i];
            TEST_ASSERT_NEQ(sum, 0u, "fill_classified fills the buffer");
        }

        /* Insulation: the credited class is CSPRNG-owned and must not
         * follow the loose diagnostic record. Mutate ONLY the diagnostic
         * record (snapshot/restore, the documented in-memory pattern) and
         * assert the credited class + crypto gate do not move -- a
         * recorded-but-not-absorbed source must never approve key
         * generation. */
        {
            uint32_t saved_mask = entropy_source_mask();
            uint32_t saved_q = entropy_source_quality();
            int gate_before = csprng_crypto_ok();
            uint32_t src;

            for (src = 0; src < ENTROPY_SRC_COUNT; src++)
                entropy_record_source((entropy_src_t)src, ENTROPY_Q_NONE);
            entropy_record_source(ENTROPY_SRC_FW_RNG, ENTROPY_Q_HIGH);
            entropy_record_source(ENTROPY_SRC_TPM_RNG, ENTROPY_Q_HIGH);

            TEST_ASSERT_EQ(csprng_credited_class(), credited,
                           "credited class insulated from diagnostic record");
            TEST_ASSERT_EQ((uint64_t)csprng_crypto_ok(),
                           (uint64_t)gate_before,
                           "crypto gate insulated from diagnostic record");

            for (src = 0; src < ENTROPY_SRC_COUNT; src++) {
                entropy_quality_t q = (saved_mask & ENTROPY_SRC_BIT(src))
                    ? entropy_quality_get(saved_q, (entropy_src_t)src)
                    : ENTROPY_Q_NONE;
                entropy_record_source((entropy_src_t)src, q);
            }
        }
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

/* Freestanding-libc string hardening edge cases: precision-bounded %s, %zd
 * 64-bit, INT64_MIN magnitude, strlcat on an unterminated dst, and
 * strtoul/strtol overflow clamp + invalid base. */
static void test_string_lib_edges(void)
{
    char b[40];

    /* Messages are intentionally reused (identical literals merge in .rodata)
     * to keep the test-build image under the 0x800000 user-base ceiling. */
    #define FE "fmt edge"
    #define SE "strto edge"

    snprintf(b, sizeof(b), "%zd", (long)0x100000000L);
    TEST_ASSERT(strcmp(b, "4294967296") == 0, FE);          /* %zd reads 64-bit */
    snprintf(b, sizeof(b), "%lld", (long long)(-9223372036854775807LL - 1));
    TEST_ASSERT(strcmp(b, "-9223372036854775808") == 0, FE);/* %lld INT64_MIN */
    snprintf(b, sizeof(b), "%.3s", "abcdef");
    TEST_ASSERT(strcmp(b, "abc") == 0, FE);                 /* %.3s bounds output */

    TEST_ASSERT_EQ(snprintf(b, 0, "%d", 42), 2, FE);        /* size=0 -> would-be len */
    /* Truncation in the MIDDLE of a number must not spin: PUTC must still
     * evaluate its side-effecting `tmp[--n]` argument once the buffer fills. */
    TEST_ASSERT_EQ(snprintf(b, 3, "%d", 12345), 5, FE);     /* mid-number trunc len */
    TEST_ASSERT(strcmp(b, "12") == 0, FE);                  /* mid-number trunc + NUL */
    TEST_ASSERT_EQ(snprintf(b, 4, "hello"), 5, FE);         /* trunc returns full len */
    TEST_ASSERT(strcmp(b, "hel") == 0, FE);                 /* trunc + NUL */

    char d[4]; d[0]='a'; d[1]='b'; d[2]='c'; d[3]='d';
    TEST_ASSERT_EQ(strlcat(d, "xy", 4), 6u, SE);            /* unterminated dst -> n+slen */

    TEST_ASSERT_EQ(strtoul("99999999999999999999999", (char **)0, 10),
                   0xFFFFFFFFFFFFFFFFUL, SE);               /* overflow clamps */
    char *e = (char *)0;
    TEST_ASSERT_EQ(strtoul("10", &e, 99), 0u, SE);          /* invalid base -> 0 */
    TEST_ASSERT_EQ(strtol("-99999999999999999999", (char **)0, 10),
                   (-9223372036854775807L - 1L), SE);       /* underflow -> LONG_MIN */

    /* Integer precision: min digit count, leading zeros, prec overrides `0`. */
    snprintf(b, sizeof(b), "%.5d", 42);
    TEST_ASSERT(strcmp(b, "00042") == 0, FE);               /* zero-extend to precision */
    snprintf(b, sizeof(b), "[%.0d]", 0);
    TEST_ASSERT(strcmp(b, "[]") == 0, FE);                  /* %.0d of 0 -> no digits */
    snprintf(b, sizeof(b), "[%08.3u]", 42u);
    TEST_ASSERT(strcmp(b, "[     042]") == 0, FE);          /* prec disables 0, width pads */
    snprintf(b, sizeof(b), "[%8p]", (void *)0x1234);
    TEST_ASSERT(strcmp(b, "[  0x1234]") == 0, FE);          /* %p honors width */

    /* strtoul sign + guarded 0x prefix. */
    char *p = (char *)0;
    TEST_ASSERT_EQ(strtoul("+10", &p, 10), 10u, SE);        /* consumes leading + */
    TEST_ASSERT_EQ(strtoul("-1", (char **)0, 10), 0xFFFFFFFFFFFFFFFFUL, SE); /* negate */
    p = (char *)0;
    TEST_ASSERT_EQ(strtoul("0xg", &p, 0), 0u, SE);          /* 0x w/o hex digit -> 0 */
    TEST_ASSERT(p && *p == 'x', SE);                        /* endptr after leading 0 */
    TEST_ASSERT_EQ(strtoul("-18446744073709551616", (char **)0, 10),
                   0xFFFFFFFFFFFFFFFFUL, SE);               /* neg overflow -> sentinel, not 1 */
    char *q = (char *)0;
    TEST_ASSERT_EQ(strtol("++1", &q, 10), 0L, SE);          /* doubled sign -> no conversion */
    TEST_ASSERT(q == (char *)0 || *q == '+', SE);           /* endptr at original start */

    #undef FE
    #undef SE
}

/* Reference-vector check for the freestanding math library (libc/math.h).
 * Tolerances are a few ULP scaled to magnitude, per the documented few-ULP
 * (not 1-ULP) contract. */
static int km_close(double a, double b, double tol)
{
    return kmath_fabs(a - b) <= tol;
}

static void test_math_lib(void)
{
    const double PI = 3.14159265358979311600;
    const double E  = 2.71828182845904509080;
    const double T  = 1e-9;                       /* checkpoint tolerance */

    /* sin/cos/tan */
    TEST_ASSERT(km_close(kmath_sin(0.0), 0.0, T), "sin(0)=0");
    TEST_ASSERT(km_close(kmath_sin(PI / 2), 1.0, T), "sin(pi/2)=1");
    TEST_ASSERT(km_close(kmath_sin(PI / 6), 0.5, T), "sin(pi/6)=0.5");
    TEST_ASSERT(km_close(kmath_sin(100.0 * PI), 0.0, 1e-6), "sin(100pi)~0 (range reduction)");
    TEST_ASSERT(km_close(kmath_tan(PI / 4), 1.0, T), "tan(pi/4)=1");

    /* atan / atan2 / asin */
    TEST_ASSERT(km_close(kmath_atan(1.0), PI / 4, T), "atan(1)=pi/4");
    TEST_ASSERT(km_close(kmath_atan2(1.0, 1.0), PI / 4, T), "atan2(1,1)=pi/4");
    TEST_ASSERT(km_close(kmath_atan2(1.0, -1.0), 3.0 * PI / 4, T), "atan2(1,-1)=3pi/4");
    TEST_ASSERT(km_close(kmath_atan2(-1.0, -1.0), -3.0 * PI / 4, T), "atan2(-1,-1)=-3pi/4");
    TEST_ASSERT(km_close(kmath_asin(0.5), PI / 6, T), "asin(0.5)=pi/6");
    TEST_ASSERT(km_close(kmath_asin(1.0), PI / 2, T), "asin(1)=pi/2");

    /* exp / log / log2 / log10 */
    TEST_ASSERT(km_close(kmath_exp(0.0), 1.0, T), "exp(0)=1");
    TEST_ASSERT(km_close(kmath_exp(1.0), E, T), "exp(1)=e");
    TEST_ASSERT(km_close(kmath_exp(-1.0), 1.0 / E, T), "exp(-1)=1/e");
    TEST_ASSERT(km_close(kmath_log(E), 1.0, T), "log(e)=1");
    TEST_ASSERT(km_close(kmath_log(1.0), 0.0, T), "log(1)=0");
    TEST_ASSERT(km_close(kmath_exp(kmath_log(12345.0)), 12345.0, 1e-4), "exp(log(x))=x");
    TEST_ASSERT(km_close(kmath_log2(8.0), 3.0, T), "log2(8)=3");
    TEST_ASSERT(km_close(kmath_log10(1000.0), 3.0, T), "log10(1000)=3");

    /* round / trunc / cbrt */
    TEST_ASSERT(kmath_round(2.5) == 3.0, "round(2.5)=3 (half away)");
    TEST_ASSERT(kmath_round(-2.5) == -3.0, "round(-2.5)=-3");
    TEST_ASSERT(kmath_round(0.49) == 0.0, "round(0.49)=0");
    TEST_ASSERT(kmath_trunc(2.7) == 2.0, "trunc(2.7)=2");
    TEST_ASSERT(kmath_trunc(-2.7) == -2.0, "trunc(-2.7)=-2");
    TEST_ASSERT(km_close(kmath_cbrt(27.0), 3.0, T), "cbrt(27)=3");
    TEST_ASSERT(km_close(kmath_cbrt(-8.0), -2.0, T), "cbrt(-8)=-2");

    /* negative-argument symmetry */
    TEST_ASSERT(km_close(kmath_sin(-PI / 6), -0.5, T), "sin(-pi/6)=-0.5");
    TEST_ASSERT(km_close(kmath_tan(-PI / 4), -1.0, T), "tan(-pi/4)=-1");
    TEST_ASSERT(km_close(kmath_atan(-1.0), -PI / 4, T), "atan(-1)=-pi/4");
    TEST_ASSERT(km_close(kmath_atan2(-1.0, 1.0), -PI / 4, T), "atan2(-1,1)=-pi/4");

    /* float variants (delegate to double / kmath.h inlines) */
    TEST_ASSERT(km_close((double)kmath_sinf(0.0f), 0.0, 1e-6), "sinf(0)=0");
    TEST_ASSERT(km_close((double)kmath_sqrtf(16.0f), 4.0, 1e-5), "sqrtf(16)=4");
    TEST_ASSERT(km_close((double)kmath_cosf(0.0f), 1.0, 1e-6), "cosf(0)=1");
    TEST_ASSERT(km_close((double)kmath_powf(2.0f, 10.0f), 1024.0, 1e-2), "powf(2,10)=1024");

    /* IEEE special values + domain edges */
    double qnan = km_nan_(), pinf = km_inf_(0), ninf = km_inf_(1);
    TEST_ASSERT(kmath_sin(qnan) != kmath_sin(qnan), "sin(NaN)=NaN");
    TEST_ASSERT(kmath_exp(qnan) != kmath_exp(qnan), "exp(NaN)=NaN");
    TEST_ASSERT(kmath_log(qnan) != kmath_log(qnan), "log(NaN)=NaN");
    TEST_ASSERT(kmath_sin(pinf) != kmath_sin(pinf), "sin(+inf)=NaN");
    TEST_ASSERT(kmath_tan(pinf) != kmath_tan(pinf), "tan(+inf)=NaN");
    TEST_ASSERT(km_isinf_(kmath_exp(711.0)) && kmath_exp(711.0) > 0.0, "exp(711)=+inf");
    TEST_ASSERT(kmath_exp(-746.0) == 0.0, "exp(-746)=0");
    TEST_ASSERT(km_isinf_(kmath_log(0.0)) && kmath_log(0.0) < 0.0, "log(0)=-inf");
    TEST_ASSERT(kmath_log(-1.0) != kmath_log(-1.0), "log(-1)=NaN");
    TEST_ASSERT(km_isinf_(kmath_log(pinf)) && kmath_log(pinf) > 0.0, "log(+inf)=+inf");
    TEST_ASSERT(km_close(kmath_log(0x1p-1074), -1074.0 * 0.69314718055994530942, 1e-6),
                "log(min subnormal) ~ -1074*ln2");
    TEST_ASSERT(kmath_asin(2.0) != kmath_asin(2.0), "asin(2)=NaN (domain)");
    TEST_ASSERT(kmath_asin(-2.0) != kmath_asin(-2.0), "asin(-2)=NaN (domain)");
    TEST_ASSERT(km_close(kmath_atan2(pinf, pinf), PI / 4, T), "atan2(inf,inf)=pi/4");
    TEST_ASSERT(km_close(kmath_atan2(pinf, ninf), 3.0 * PI / 4, T), "atan2(inf,-inf)=3pi/4");
    TEST_ASSERT(km_isinf_(kmath_cbrt(pinf)) && kmath_cbrt(pinf) > 0.0, "cbrt(+inf)=+inf");
    TEST_ASSERT(kmath_cbrt(0.0) == 0.0, "cbrt(0)=0");

    /* atan2 signed-zero quadrants (C99 edge semantics) */
    double nzero = -0.0;
    double r1 = kmath_atan2(nzero, pinf);
    TEST_ASSERT(r1 == 0.0 && km_signbit_(r1), "atan2(-0,+inf)=-0");
    TEST_ASSERT(km_close(kmath_atan2(nzero, ninf), -PI, T), "atan2(-0,-inf)=-pi");
    TEST_ASSERT(km_close(kmath_atan2(0.0, nzero), PI, T), "atan2(+0,-0)=+pi");
    TEST_ASSERT(km_close(kmath_atan2(nzero, nzero), -PI, T), "atan2(-0,-0)=-pi");
    double r2 = kmath_atan2(nzero, 1.0);
    TEST_ASSERT(r2 == 0.0 && km_signbit_(r2), "atan2(-0,+finite)=-0");
    double r3 = kmath_atan2(0.0, 1.0);
    TEST_ASSERT(r3 == 0.0 && !km_signbit_(r3), "atan2(+0,+finite)=+0");
    TEST_ASSERT(km_close(kmath_atan2(nzero, -1.0), -PI, T), "atan2(-0,-finite)=-pi");
    TEST_ASSERT(km_close(kmath_atan2(0.0, -1.0), PI, T), "atan2(+0,-finite)=+pi");
    double r4 = kmath_atan(nzero);
    TEST_ASSERT(r4 == 0.0 && km_signbit_(r4), "atan(-0)=-0");
    double r5 = kmath_atan2(-0x1p-1074, 0x1p1023);   /* y/x underflows to -0 */
    TEST_ASSERT(r5 == 0.0 && km_signbit_(r5), "atan2(-tiny,+huge)=-0");

    /* signed zero preserved at every entry point */
    double sn = kmath_sin(nzero), tn = kmath_tan(nzero);
    double rn = kmath_round(nzero), tr = kmath_trunc(nzero);
    TEST_ASSERT(sn == 0.0 && km_signbit_(sn), "sin(-0)=-0");
    TEST_ASSERT(tn == 0.0 && km_signbit_(tn), "tan(-0)=-0");
    TEST_ASSERT(rn == 0.0 && km_signbit_(rn), "round(-0)=-0");
    TEST_ASSERT(tr == 0.0 && km_signbit_(tr), "trunc(-0)=-0");
    /* narrowed domain: finite |x| >= KMATH_TRIG_REDUCE_MAX returns NaN (documented) */
    TEST_ASSERT(kmath_sin(KMATH_TRIG_REDUCE_MAX) != kmath_sin(KMATH_TRIG_REDUCE_MAX),
                "sin(>=reduce_max)=NaN (narrowed domain)");
    /* asin near +/-1: km_sqrt_ keeps the tiny endpoint radicand accurate */
    TEST_ASSERT(km_close(kmath_asin(1.0 - 0x1p-52), PI / 2, 1e-7), "asin(1-2^-52) ~ pi/2");
    TEST_ASSERT(km_close(kmath_asin(-(1.0 - 0x1p-52)), -PI / 2, 1e-7), "asin(-(1-2^-52)) ~ -pi/2");

    /* acosf accurate endpoints + domain; sqrtf special values */
    TEST_ASSERT(km_close((double)kmath_acosf(1.0f), 0.0, 1e-6), "acosf(1)=0");
    TEST_ASSERT(km_close((double)kmath_acosf(-1.0f), PI, 1e-6), "acosf(-1)=pi");
    TEST_ASSERT(km_close((double)kmath_acosf(0.5f), PI / 3, 1e-6), "acosf(0.5)=pi/3");
    TEST_ASSERT(kmath_acosf(2.0f) != kmath_acosf(2.0f), "acosf(2)=NaN (domain)");
    TEST_ASSERT(kmath_sqrtf(-1.0f) != kmath_sqrtf(-1.0f), "sqrtf(-1)=NaN");
    /* km_sqrt_ stays accurate on a subnormal double (scale result by 2^537) */
    TEST_ASSERT(km_close(km_sqrt_(0x1p-1074) * 0x1p537, 1.0, 1e-9), "km_sqrt_ subnormal accurate");
    TEST_ASSERT(km_isinf_((double)kmath_sqrtf((float)pinf)) && kmath_sqrtf((float)pinf) > 0.0f,
                "sqrtf(+inf)=+inf");

    /* cosf must NOT hang on inf / huge finite (guarded path, not kmath_cos) */
    TEST_ASSERT(kmath_cosf((float)pinf) != kmath_cosf((float)pinf), "cosf(+inf)=NaN (no hang)");
    TEST_ASSERT(kmath_cosf(1e30f) != kmath_cosf(1e30f), "cosf(huge)=NaN (no hang)");

    /* powf hardened special values */
    TEST_ASSERT(km_isinf_((double)kmath_powf(0.0f, -1.0f)) && kmath_powf(0.0f, -1.0f) > 0.0f,
                "powf(0,-1)=+inf");
    TEST_ASSERT(kmath_powf(-1.0f, 0.5f) != kmath_powf(-1.0f, 0.5f), "powf(-1,0.5)=NaN");
    TEST_ASSERT(km_close((double)kmath_powf(-2.0f, 3.0f), -8.0, 1e-3), "powf(-2,3)=-8");
    TEST_ASSERT(kmath_powf(3.14f, 0.0f) == 1.0f, "powf(x,0)=1");
    double pz = (double)kmath_powf(-0.0f, 3.0f);
    TEST_ASSERT(pz == 0.0 && km_signbit_(pz), "powf(-0,3)=-0");
    TEST_ASSERT(kmath_powf(1.0f, (float)qnan) == 1.0f, "powf(1,NaN)=1");
    TEST_ASSERT(km_isinf_((double)kmath_powf(2.0f, (float)pinf)) && kmath_powf(2.0f, (float)pinf) > 0.0f,
                "powf(2,+inf)=+inf (no UB)");
    TEST_ASSERT(kmath_powf(2.0f, (float)ninf) == 0.0f, "powf(2,-inf)=0");
    /* large finite integer exponent: parity must not cast a huge double to int */
    TEST_ASSERT(kmath_powf(-0.0f, 1e20f) == 0.0f && !km_signbit_((double)kmath_powf(-0.0f, 1e20f)),
                "powf(-0,1e20)=+0 (even, no UB)");
    TEST_ASSERT(km_isinf_((double)kmath_powf((float)ninf, 1e20f)) && kmath_powf((float)ninf, 1e20f) > 0.0f,
                "powf(-inf,1e20)=+inf (even, no UB)");
    TEST_ASSERT(km_isinf_((double)kmath_powf(-0.0f, -3.0f)) && kmath_powf(-0.0f, -3.0f) < 0.0f,
                "powf(-0,-3)=-inf");
    TEST_ASSERT(kmath_powf((float)pinf, -2.0f) == 0.0f, "powf(+inf,-2)=+0");
    double pno = (double)kmath_powf((float)ninf, -3.0f);
    TEST_ASSERT(pno == 0.0 && km_signbit_(pno), "powf(-inf,-3)=-0");
    TEST_ASSERT(kmath_powf(-1.0f, (float)pinf) == 1.0f, "powf(-1,+inf)=1");
    TEST_ASSERT(kmath_powf(-1.0f, (float)ninf) == 1.0f, "powf(-1,-inf)=1");

    /* atan2 infinity matrix: y-inf/x-finite + negative-y both-infinite */
    TEST_ASSERT(km_close(kmath_atan2(pinf, 1.0), PI / 2, T), "atan2(+inf,1)=pi/2");
    TEST_ASSERT(km_close(kmath_atan2(ninf, 1.0), -PI / 2, T), "atan2(-inf,1)=-pi/2");
    TEST_ASSERT(km_close(kmath_atan2(ninf, pinf), -PI / 4, T), "atan2(-inf,+inf)=-pi/4");
    TEST_ASSERT(km_close(kmath_atan2(ninf, ninf), -3.0 * PI / 4, T), "atan2(-inf,-inf)=-3pi/4");

    /* quadrant II of the shared reducer (n&3 == 2) */
    TEST_ASSERT(km_close(kmath_tan(3.0 * PI / 4), -1.0, T), "tan(3pi/4)=-1 (quadrant II)");
    TEST_ASSERT(km_close(kmath_sin(3.0 * PI / 4), 0.70710678118654752440, T), "sin(3pi/4)=sqrt2/2");

    /* large-but-in-range reduction accuracy (n exact below the 2^20 cutoff) */
    TEST_ASSERT(km_close(kmath_sin(100000.0 * PI), 0.0, 1e-6), "sin(100000pi)~0 (in-range reduction)");

    /* values with no fractional bits pass through round/trunc unchanged */
    double big = 0x1p52 + 3.0;
    TEST_ASSERT(kmath_round(big) == big, "round(2^52+3) unchanged");
    TEST_ASSERT(kmath_trunc(big) == big, "trunc(2^52+3) unchanged");
    TEST_ASSERT(kmath_round(-big) == -big, "round(-(2^52+3)) unchanged");
}

/* cJSON wrapper: parse/extract + malformed-input hardening (depth cap,
 * length-bounded parse, trailing-garbage rejection). */
static void test_json_lib(void)
{
    struct cJSON *o = json_parse("{\"os\":\"Impossible\",\"build\":1024,\"debug\":true}");
    TEST_ASSERT(o != (struct cJSON *)0, "json_parse sample doc");
    if (o) {
        const char *os = json_str(json_get(o, "os"));
        TEST_ASSERT(os && strcmp(os, "Impossible") == 0, "json_str os=Impossible");
        TEST_ASSERT_EQ(json_int(json_get(o, "build")), 1024, "json_int build=1024");
        TEST_ASSERT_EQ(json_bool(json_get(o, "debug")), 1, "json_bool debug=true");
        char *p = json_print(o);
        TEST_ASSERT(p != (char *)0, "json_print non-null");
        if (p) {
            struct cJSON *o2 = json_parse(p);
            TEST_ASSERT(o2 && json_int(json_get(o2, "build")) == 1024,
                        "json_print round-trip stable");
            json_free(o2);
            kfree(p);                /* json_print result is a kmalloc'd string */
        }
        json_free(o);
    }

    /* NULL / empty rejected */
    TEST_ASSERT(json_parse((const char *)0) == (struct cJSON *)0, "json_parse(NULL)=NULL");
    TEST_ASSERT(json_parse_len("{}", 0) == (struct cJSON *)0, "json_parse_len len=0=NULL");
    TEST_ASSERT(json_parse_len((const char *)0, 4) == (struct cJSON *)0, "json_parse_len(NULL)=NULL");

    /* length-bounded parse on a NON-NUL-terminated window: only the first 2
     * bytes ("{}") are in-bounds; the trailing "JUNK" is outside len. */
    struct cJSON *b = json_parse_len("{}JUNK", 2);
    TEST_ASSERT(b != (struct cJSON *)0, "json_parse_len bounds read to len");
    json_free(b);

    /* trailing non-whitespace after a valid value is rejected (no prefix+junk) */
    TEST_ASSERT(json_parse_len("{} xyz", 6) == (struct cJSON *)0, "json_parse_len rejects trailing junk");
    /* an embedded NUL is not an out-of-band terminator -- junk after it is still junk */
    TEST_ASSERT(json_parse_len("{}\0JUNK", 7) == (struct cJSON *)0, "json_parse_len rejects post-NUL junk");
    /* raw control bytes anywhere in the length-bounded buffer are rejected pre-parse */
    TEST_ASSERT(json_parse_len("{\0}", 3) == (struct cJSON *)0, "json_parse_len rejects structural NUL");
    TEST_ASSERT(json_parse_len("\0{}", 3) == (struct cJSON *)0, "json_parse_len rejects leading NUL");
    TEST_ASSERT(json_parse_len("[\x01]", 3) == (struct cJSON *)0, "json_parse_len rejects raw control byte");
    struct cJSON *w = json_parse("{}  \n\t");
    TEST_ASSERT(w != (struct cJSON *)0, "trailing whitespace ok");
    json_free(w);

    /* depth cap: deeper than CJSON_NESTING_LIMIT (32) rejected without
     * exhausting heap/stack. */
    char deep[96];
    int n = 0;
    for (int i = 0; i < 40; i++) deep[n++] = '[';
    for (int i = 0; i < 40; i++) deep[n++] = ']';
    deep[n] = '\0';
    TEST_ASSERT(json_parse(deep) == (struct cJSON *)0, "json_parse rejects >32-deep nesting");
}

/* Shared checksum (CRC-32 / CRC-32C, table + SSE4.2 paths) + base64/hex codec. */
static void test_checksum_codec(void)
{
    /* CRC vectors */
    TEST_ASSERT_EQ(kcrc32("123456789", 9), 0xCBF43926u, "kcrc32 IEEE vector");
    TEST_ASSERT_EQ(kcrc32c("123456789", 9), 0xE3069283u, "kcrc32c Castagnoli vector");
    TEST_ASSERT_EQ(kcrc32c_sw_test("123456789", 9), 0xE3069283u, "kcrc32c table path");
    if (cpu_has(CPU_FEATURE_SSE4_2)) {
        TEST_ASSERT_EQ(kcrc32c_hw_test("123456789", 9), 0xE3069283u, "kcrc32c SSE4.2 path");
        TEST_ASSERT_EQ(kcrc32c_hw_test("123456789", 9), kcrc32c_sw_test("123456789", 9),
                       "CRC32C hw==sw byte-identical");
    }
    /* continuation == one-shot (split-vector) */
    TEST_ASSERT_EQ(kcrc32_cont(kcrc32("123", 3), "456789", 6), 0xCBF43926u,
                   "kcrc32_cont split == one-shot");
    TEST_ASSERT_EQ(kcrc32c_cont(kcrc32c("123", 3), "456789", 6), 0xE3069283u,
                   "kcrc32c_cont split == one-shot");

    /* base64 (RFC 4648) */
    char b64[24];
    int n = base64_encode(b64, sizeof(b64), "foobar", 6);
    TEST_ASSERT(n == 8, "base64_encode foobar len");
    if (n > 0) { b64[n] = 0; TEST_ASSERT(strcmp(b64, "Zm9vYmFy") == 0, "base64_encode foobar"); }
    n = base64_encode(b64, sizeof(b64), "f", 1);
    if (n > 0) { b64[n] = 0; TEST_ASSERT(strcmp(b64, "Zg==") == 0, "base64_encode 1-byte padding"); }
    uint8_t dec[16];
    n = base64_decode(dec, sizeof(dec), "Zm9vYmFy", 8, 0);
    TEST_ASSERT(n == 6 && memcmp(dec, "foobar", 6) == 0, "base64_decode round-trip");
    n = base64_decode(dec, sizeof(dec), "Zg==", 4, 0);
    TEST_ASSERT(n == 1 && dec[0] == 'f', "base64_decode padded");
    TEST_ASSERT(base64_decode(dec, sizeof(dec), "Zg=A", 4, 0) == -1, "base64 rejects data after pad");
    TEST_ASSERT(base64_decode(dec, sizeof(dec), "Zm9", 3, 0) == -1, "base64 rejects len%%4");
    TEST_ASSERT(base64_decode(dec, sizeof(dec), "Zg=*", 4, 0) == -1, "base64 rejects invalid char");
    TEST_ASSERT(base64_decode(dec, sizeof(dec), "Zh==", 4, 0) == -1, "base64 rejects non-canonical pad (2)");
    TEST_ASSERT(base64_decode(dec, sizeof(dec), "Zm9=", 4, 0) == -1, "base64 rejects non-canonical pad (1)");
    /* MIME mode skips whitespace */
    n = base64_decode(dec, sizeof(dec), "Zm9v\nYmFy", 9, 1);
    TEST_ASSERT(n == 6 && memcmp(dec, "foobar", 6) == 0, "base64 MIME skips whitespace");

    /* hex */
    char hx[8];
    n = hex_encode(hx, sizeof(hx), "\xDE\xAD", 2);
    if (n > 0) { hx[n] = 0; TEST_ASSERT(strcmp(hx, "dead") == 0, "hex_encode lowercase"); }
    uint8_t hd[4];
    n = hex_decode(hd, sizeof(hd), "DeAd", 4);
    TEST_ASSERT(n == 2 && hd[0] == 0xDE && hd[1] == 0xAD, "hex_decode mixed case");
    TEST_ASSERT(hex_decode(hd, sizeof(hd), "abc", 3) == -1, "hex rejects odd length");
    TEST_ASSERT(hex_decode(hd, sizeof(hd), "xy", 2) == -1, "hex rejects non-hex");

    /* Overflow guards: the bound checks return -1 BEFORE dereferencing src, so a
     * bogus oversized length with a valid (un-read) pointer is safe to assert. */
    TEST_ASSERT(base64_encode(b64, sizeof(b64), "x", (size_t)0x7FFFFFFF * 3) == -1,
                "base64_encode rejects length that would wrap output");
    TEST_ASSERT(hex_encode(hx, sizeof(hx), "x", (size_t)0x7FFFFFFF) == -1,
                "hex_encode rejects length over INT_MAX/2");
    TEST_ASSERT(hex_decode(hd, sizeof(hd), "00", (size_t)0x100000000ULL) == -1,
                "hex_decode rejects out_len over INT_MAX");
}

/* ---- LZ4 block compressor (section 3) ---- */

/* Modest, compressible fixture kept small to respect the kernel test BSS
 * budget (image must stay under the 0x800000 user base). A repeating pattern
 * with embedded variation exercises both literal runs and back-references. */
static void test_lz4_block(void)
{
    static uint8_t src[2048];
    static uint8_t comp[2048 + 2048 / 255 + 16]; /* >= lz4_compress_bound(2048) */
    static uint8_t out[2048];

    for (size_t i = 0; i < sizeof(src); i++) {
        src[i] = (uint8_t)((i & 0x1F) ? (i / 17) : 0xA5);
    }

    size_t bound = lz4_compress_bound(sizeof(src));
    TEST_ASSERT(bound >= sizeof(src) && bound <= sizeof(comp),
                "lz4_compress_bound sane for 2 KiB");

    int clen = lz4_compress(src, sizeof(src), comp, sizeof(comp));
    TEST_ASSERT(clen > 0 && (size_t)clen <= bound,
                "lz4_compress within bound");
    TEST_ASSERT((size_t)clen < sizeof(src),
                "lz4_compress actually shrinks compressible input");

    int dlen = lz4_decompress(comp, (size_t)clen, out, sizeof(out));
    TEST_ASSERT_EQ(dlen, (int)sizeof(src), "lz4_decompress restores length");
    TEST_ASSERT_EQ(memcmp(out, src, sizeof(src)), 0, "lz4 round-trip identity");

    /* Insufficient dst capacity -> clean failure, not overrun. */
    uint8_t tiny[8];
    TEST_ASSERT_EQ(lz4_compress(src, sizeof(src), tiny, sizeof(tiny)),
                   LZ4_ERR_FAIL, "lz4_compress rejects undersized dst");

    /* Malformed compressed input -> negative, never a panic/overrun. The safe
     * decoder is bounds-checked against out capacity. */
    static const uint8_t junk[16] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    TEST_ASSERT(lz4_decompress(junk, sizeof(junk), out, sizeof(out)) < 0,
                "lz4_decompress rejects malformed frame");

    /* Valid block but undersized dst -> LZ4_ERR_FAIL (distinct from the
     * malformed-frame path): the safe decoder reports capacity exhaustion, the
     * wrapper maps it to a clean error, never an overrun. */
    TEST_ASSERT_EQ(lz4_decompress(comp, (size_t)clen, tiny, sizeof(tiny)),
                   LZ4_ERR_FAIL, "lz4_decompress rejects undersized dst");

    /* Argument + size-boundary guards reject BEFORE touching the buffers, so a
     * bogus oversized size_t with an un-read pointer is safe to assert. This is
     * the size_t-truncation guard: a length above LZ4_BLOCK_INPUT_MAX / INT_MAX
     * must be rejected, never narrowed into the int-based core. */
    TEST_ASSERT_EQ(lz4_compress(NULL, 16, comp, sizeof(comp)), LZ4_ERR_ARG,
                   "lz4_compress rejects NULL src");
    TEST_ASSERT_EQ(lz4_compress(src, 0, comp, sizeof(comp)), LZ4_ERR_ARG,
                   "lz4_compress rejects zero length");
    TEST_ASSERT_EQ(lz4_compress(src, (size_t)LZ4_BLOCK_INPUT_MAX + 1, comp, sizeof(comp)),
                   LZ4_ERR_RANGE, "lz4_compress rejects oversized src_size");
    TEST_ASSERT_EQ(lz4_compress(src, sizeof(src), comp, (size_t)0x100000000ULL),
                   LZ4_ERR_RANGE, "lz4_compress rejects dst_capacity over INT_MAX");
    TEST_ASSERT_EQ(lz4_decompress(NULL, (size_t)clen, out, sizeof(out)), LZ4_ERR_ARG,
                   "lz4_decompress rejects NULL src");
    TEST_ASSERT_EQ(lz4_decompress(comp, 0, out, sizeof(out)), LZ4_ERR_ARG,
                   "lz4_decompress rejects zero length");
    TEST_ASSERT_EQ(lz4_decompress(comp, (size_t)clen, NULL, sizeof(out)), LZ4_ERR_ARG,
                   "lz4_decompress rejects NULL dst");
    TEST_ASSERT_EQ(lz4_decompress(comp, (size_t)0x100000000ULL, out, sizeof(out)),
                   LZ4_ERR_RANGE, "lz4_decompress rejects src_size over INT_MAX");
    TEST_ASSERT_EQ(lz4_decompress(comp, (size_t)clen, out, (size_t)0x100000000ULL),
                   LZ4_ERR_RANGE, "lz4_decompress rejects dst_capacity over INT_MAX");
    TEST_ASSERT_EQ(lz4_compress_bound((size_t)LZ4_BLOCK_INPUT_MAX + 1), (size_t)0,
                   "lz4_compress_bound returns 0 for oversized input");
}

void test_register_klibs(void)
{
    test_suite_register_cat("klibs: string lib edges",
                            test_string_lib_edges, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: lz4 block roundtrip",
                            test_lz4_block, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: math lib",
                            test_math_lib, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: cjson wrapper",
                            test_json_lib, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: checksum + codec",
                            test_checksum_codec, TEST_CAT_EXEC);
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
    test_suite_register_cat("klibs: csprng source-framed vector",
        test_csprng_source_framed_vector, TEST_CAT_SECURITY);
    test_suite_register_cat("klibs: csprng absorb policy",
                            test_csprng_absorb_policy, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: csprng add_entropy",
                            test_csprng_add_entropy_global, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: csprng crypto gate",
        test_csprng_crypto_gate, TEST_CAT_SECURITY);
    test_suite_register_cat("klibs: csprng global",
                            test_csprng_global, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: NtGetRandom",
                            test_nt_get_random, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: NtGetRandom chunks",
                            test_nt_get_random_chunks, TEST_CAT_EXEC);
    test_suite_register_cat("klibs: NtGetRandom dispatch",
                            test_nt_get_random_dispatch, TEST_CAT_EXEC);
}
