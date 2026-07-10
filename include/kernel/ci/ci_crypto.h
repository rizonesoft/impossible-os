/* ci_crypto.h -- Code Integrity crypto provider bridge.
 *
 * A thin, fail-closed, allocation-free wrapper the CI plane uses to hash images
 * and verify signatures without binding to a specific primitive. It selects the
 * kernel's existing transforms (crypto_hash for SHA-2/3, Monocypher for BLAKE2b
 * and Ed25519) behind one CI-facing surface so embedded-signature validation can
 * request "the digest" / "verify this signature" by algorithm id.
 *
 * Security posture: every unsupported algorithm, malformed length, or bad
 * pointer denies (digest -> -1, verify -> false). Ed25519 is the native signing
 * algorithm; RSA/X.509 is reserved until the CNG provider matures.
 */
#ifndef KERNEL_CI_CI_CRYPTO_H
#define KERNEL_CI_CI_CRYPTO_H

#include "kernel/types.h"
#include "kernel/boot_init.h"   /* bool */

/* Digest algorithms the CI plane understands. */
typedef enum ci_digest_alg {
    CI_DIGEST_SHA256      = 0,
    CI_DIGEST_SHA384      = 1,
    CI_DIGEST_BLAKE2B_256 = 2,
    CI_DIGEST_MAX
} ci_digest_alg_t;

/* Signature algorithms. Ed25519 is native; RSA is a reserved placeholder. */
typedef enum ci_sig_alg {
    CI_SIG_ED25519   = 0,   /* native PureEd25519 */
    CI_SIG_RSA_PKCS1 = 1,   /* reserved -- not supported until CNG matures */
    CI_SIG_MAX
} ci_sig_alg_t;

#define CI_ED25519_SIG_LEN     64
#define CI_ED25519_PUBKEY_LEN  32

/* Digest length in bytes for alg, or 0 if unknown. */
uint32_t ci_crypto_digest_len(ci_digest_alg_t alg);

/* Compute the digest of [data, data+len) under alg into out (out_cap bytes).
 * Returns the digest length written (> 0) on success, or -1 on: unknown alg,
 * NULL out, out_cap too small for the digest, or NULL data with len != 0 (a
 * malformed request must fail closed, not fault the kernel). NULL data with
 * len == 0 is a valid empty input. Allocation-free. */
int ci_crypto_digest(ci_digest_alg_t alg, const void *data, uint32_t len,
                     uint8_t *out, uint32_t out_cap);

/* Verify sig over [msg, msg+msg_len) with pubkey under alg. Returns true ONLY
 * on a cryptographically valid signature; false on an invalid signature, an
 * unsupported algorithm, wrong sig/pubkey length, NULL sig/pubkey, or NULL msg
 * with msg_len != 0 (fail closed). Ed25519 is PureEd25519: the signer MUST sign
 * the RAW message bytes (no external pre-hash, not Ed25519ph) -- the primitive
 * does its own SHA-512 internally. Allocation-free. */
bool ci_crypto_verify(ci_sig_alg_t alg, const uint8_t *msg, uint32_t msg_len,
                      const uint8_t *sig, uint32_t sig_len,
                      const uint8_t *pubkey, uint32_t pubkey_len);

#endif /* KERNEL_CI_CI_CRYPTO_H */
