/* ============================================================================
 * sha3.h -- FIPS 202 SHA-3 / SHAKE (Keccak-f[1600]), freestanding kernel impl
 *
 * Pure, allocation-free, caller-owned-context hash. No global mutable state, so
 * it is SMP-safe by construction (each caller owns its struct sha3_ctx) and
 * usable from any context. Validated against the NIST FIPS 202 known-answer
 * vectors (including the pad10*1 block-boundary cases) in test_klibs.c.
 *
 * Endianness-neutral: the sponge state is addressed as uint64 lanes via pure
 * arithmetic shifts (never aliased as a byte array), so absorb/squeeze are
 * correct on both little- and big-endian targets (ARM64 port safety).
 *
 * Complements the SHA-2 family (sha256.c / sha384.c) and Monocypher Blake2b:
 * SHA-3 is the FIPS 202 baseline Win11 24H2 CNG (BCRYPT_SHA3_*) and Linux
 * sha3_generic expose; SHAKE128/256 are the XOFs post-quantum signatures
 * (ML-DSA / SLH-DSA) and KMAC consume.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

#define SHA3_256_DIGEST_LEN 32u
#define SHA3_384_DIGEST_LEN 48u
#define SHA3_512_DIGEST_LEN 64u

/* The largest digest any fixed SHA-3 variant produces; sizes sha3_final's out
 * buffer for callers that do not know the variant statically. */
#define SHA3_MAX_DIGEST_LEN 64u

/* Conservative per-call SHAKE squeeze cap (one page). A request larger than
 * this is rejected (-1); a future consumer that genuinely needs a longer XOF
 * stream gets an explicit streaming/squeeze API at that point. */
#define SHAKE_MAX_OUTPUT 4096u

/* Keccak sponge context. `rate` (bytes) + `delim` select the variant; the
 * caller never sets these directly -- the per-variant init wrappers do.
 * `digest_len` is a redundant copy of the fixed-output length, set at init and
 * cross-checked against the rate-derived length in sha3_final, so a single
 * corrupted field (e.g. rate flipped to another legal rate) is caught instead
 * of silently producing a digest for state absorbed under the wrong rate. */
struct sha3_ctx {
    uint64_t st[25];      /* 1600-bit Keccak state, 25 lanes */
    uint32_t rate;        /* sponge rate in bytes (block size) */
    uint32_t pos;         /* bytes absorbed into the current block (< rate) */
    uint32_t digest_len;  /* fixed-output digest length (redundant w/ rate) */
    uint8_t  delim;       /* domain-separation byte (0x06 SHA-3, 0x1F SHAKE) */
};

/* Streaming fixed-output API: <variant>_init -> sha3_update* -> sha3_final.
 * sha3_final writes the variant's digest length ((200 - rate) / 2 bytes) into
 * out, but NEVER more than out_cap bytes: it returns -1 (no write) if out_cap
 * is smaller than the variant's digest, so a corrupted context rate cannot
 * select a digest larger than the caller's buffer. Returns the digest length
 * written (> 0) on success, or -1 on invalid ctx / NULL out / out_cap too
 * small. Pass SHA3_*_DIGEST_LEN (or sizeof your buffer) as out_cap. */
void sha3_256_init(struct sha3_ctx *ctx);
void sha3_384_init(struct sha3_ctx *ctx);
void sha3_512_init(struct sha3_ctx *ctx);
void sha3_update(struct sha3_ctx *ctx, const void *data, uint32_t len);
int  sha3_final(struct sha3_ctx *ctx, uint8_t *out, uint32_t out_cap);

/* One-shot fixed-output convenience. These are void, so on bad arguments
 * (NULL out, or NULL data with non-zero len) they leave out untouched rather
 * than write the empty-message digest for input that was never read. Use
 * crypto_hash() (kernel/crypto/hash.h) for an explicit -1 on bad arguments. */
void sha3_256(const void *data, uint32_t len, uint8_t out[SHA3_256_DIGEST_LEN]);
void sha3_384(const void *data, uint32_t len, uint8_t out[SHA3_384_DIGEST_LEN]);
void sha3_512(const void *data, uint32_t len, uint8_t out[SHA3_512_DIGEST_LEN]);

/* One-shot SHAKE XOF: squeeze out_len bytes into out. Returns out_len on
 * success, or -1 if out/out_len is invalid or out_len > SHAKE_MAX_OUTPUT. */
int shake128(const void *data, uint32_t len, uint8_t *out, uint32_t out_len);
int shake256(const void *data, uint32_t len, uint8_t *out, uint32_t out_len);
