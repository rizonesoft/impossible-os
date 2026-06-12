/* ============================================================================
 * csprng.h -- Kernel CSPRNG (ChaCha20 fast-key-erasure, Blake2b conditioner)
 *
 * Cryptographically secure random number generator backed by the vendored
 * Monocypher primitives (src/libs/monocypher/). Seeded at boot Phase 1 from
 * a source-tagged, length-framed entropy transcript (the conditioner
 * contract in kernel/entropy.h): RDRAND, TSC/address samples, the ACPI PM
 * timer, and the kernel-side staged boot transcript (interrupt jitter, TPM
 * RNG) -- all hashed with Blake2b, never raw XOR.
 *
 * Output uses fast-key-erasure: every fill ratchets the global ChaCha20 key
 * under a short spinlock hold (one bounded 64-byte keystream operation),
 * then streams the caller's bytes OUTSIDE the lock from a single-use
 * request key. Backtracking resistance per fill; the lock is never held
 * during bulk generation or user-memory access.
 *
 * The pure csprng_core_* helpers take explicit state so unit tests never
 * touch the global instance or live boot infrastructure (test policy).
 *
 * Reseed boundary: the early-entropy first-seed handoff (boot_info seed
 * payload, 01-boot-platform early-entropy TODO) and runtime sources
 * (virtio-rng, external injection) feed csprng_add_entropy(); this module
 * owns mixing only.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/entropy.h"

#define CSPRNG_KEY_SIZE       32u
#define CSPRNG_RATCHET_BYTES  64u   /* 32 next-key + 32 request-key */

/* Largest single NtGetRandom request; larger needs loop in the caller. */
#define CSPRNG_GETRANDOM_MAX  (1u << 20)

/* ---- Pure core (explicit state; unit-testable without the global) ---- */

typedef struct csprng_core {
    uint8_t  key[CSPRNG_KEY_SIZE];
    uint64_t ctr;          /* ChaCha20 block counter for the current key */
} csprng_core_t;

/* Derive the initial key from a framed entropy transcript: key =
 * Blake2b-256(transcript). Deterministic -- same transcript, same state. */
void csprng_core_seed(csprng_core_t *c, const uint8_t *transcript,
                      uint32_t len);

/* Two-part seed: key = Blake2b-256(t1 || t2) via incremental hashing --
 * exactly equivalent to csprng_core_seed over the concatenation, without
 * needing one contiguous buffer. Either part may be (NULL, 0). Used by
 * csprng_init to fold the boot_info seed payload transcript into the
 * FIRST key (first-seed-or-nothing; never a post-init reseed). */
void csprng_core_seed2(csprng_core_t *c, const uint8_t *t1, uint32_t l1,
                       const uint8_t *t2, uint32_t l2);

/* Ratchet: generate 64 keystream bytes from the current key; the first 32
 * replace the core key (forward secrecy), the last 32 are returned as a
 * single-use request key. Bounded work -- safe under a spinlock. */
void csprng_core_ratchet(csprng_core_t *c, uint8_t out_key[CSPRNG_KEY_SIZE]);

/* Stream 'len' bytes of ChaCha20 keystream from a single-use request key,
 * starting at block counter 'block'. Pure function of (key, block, len);
 * no shared state -- call OUTSIDE any lock. Multi-chunk fills from ONE
 * request key advance 'block' by ceil(chunk/64) per chunk so the keystream
 * is continuous (NtGetRandom uses this to take the global lock once). */
void csprng_core_stream_at(const uint8_t key[CSPRNG_KEY_SIZE],
                           uint64_t block, void *out, size_t len);

/* Stream 'len' bytes from block 0 -- csprng_core_stream_at(key, 0, ...). */
void csprng_core_stream(const uint8_t key[CSPRNG_KEY_SIZE], void *out,
                        size_t len);

/* Mix a 32-byte digest into the core key: key = Blake2b-256(key || digest).
 * Bounded work -- safe under a spinlock. */
void csprng_core_reseed(csprng_core_t *c,
                        const uint8_t digest[CSPRNG_KEY_SIZE]);

/* Seeded-state transition for entropy absorption (the policy seam behind
 * csprng_add_entropy, pure for unit tests): seeded -> reseed and stay
 * seeded; unseeded -> the digest becomes the seed and only HIGH quality
 * flips the seeded flag. Returns the new seeded flag. Bounded work. */
int csprng_core_absorb_digest(csprng_core_t *c, int seeded,
                              const uint8_t digest[CSPRNG_KEY_SIZE],
                              entropy_quality_t quality);

/* ---- Global kernel CSPRNG ---- */

/* Seed the global instance. Called once from boot Phase 1 after the
 * interrupt-jitter and TPM RNG collectors have staged their transcript
 * records AND after boot_seed_consume() has validated the boot_info seed
 * payload -- boot_transcript/boot_len carry its Blake2b-256 digest over
 * every accepted payload transcript ((NULL, 0) when no payload was
 * usable) and are hashed into the FIRST key together with the local +
 * staged sources. Logs the entropy
 * classification; degraded boots WARN loudly but continue (release-mode
 * gating is owned by the boot entropy policy sections). */
void csprng_init(const uint8_t *boot_transcript, uint32_t boot_len);

/* Fill 'buf' with 'len' cryptographically random bytes. SMP-safe: the
 * global key ratchet runs under one irqsave spinlock with a bounded
 * (64-byte) hold; the keystream for the request is generated OUTSIDE the
 * lock. THREAD CONTEXT for arbitrary 'len' -- the keystream generation
 * runs in the caller's context, so a multi-KB fill from an ISR would run
 * with interrupts disabled for the whole stream. ISR callers must keep
 * 'len' small (<= a few hundred bytes). If invoked before csprng_init()
 * (defensive), performs an emergency local-source seed and logs LOG_ERROR
 * after releasing the lock. */
void csprng_fill(void *buf, size_t len);

/* Fill + return the CREDITED entropy class so degraded output is
 * explicit (fill itself never blocks or fails post-init -- Win11
 * BCryptGenRandom parity). Callers that need key-grade bytes refuse on
 * ENTROPY_CLASS_DEGRADED (or gate on csprng_crypto_ok). */
entropy_class_t csprng_fill_classified(void *buf, uint32_t len);

/* CSPRNG-owned credited entropy class: snapshotted from the diagnostic
 * record at the first-seed install (where it is bound to the absorbed
 * transcript) and upgraded only when csprng_add_entropy actually
 * absorbs hardware-quality material (csprng_class_upgrade rule). The
 * loose diagnostic record in entropy.c is for reporting; THIS is for
 * policy. */
entropy_class_t csprng_credited_class(void);

/* PURE upgrade rule (unit-testable): one absorbed HIGH contribution of
 * key size or more lifts DEGRADED to MINIMUM; nothing reaches GOOD
 * without per-source provenance (runtime reseed roadmap); never
 * downgrades. */
entropy_class_t csprng_class_upgrade(entropy_class_t cur,
                                     entropy_quality_t quality,
                                     uint32_t len);

/* Cryptographic key-generation gate: 1 when the CSPRNG is seeded AND
 * the credited class is at least MINIMUM -- UNCONDITIONALLY. The
 * mutable boot.conf debug byte never relaxes key-grade readiness;
 * debug-mode latitude applies to boot progression (entropy_policy_ok
 * callers), not here. Key-generation consumers (key mint, future
 * TLS/session keys) check this BEFORE drawing; bulk consumers like
 * NtGetRandom never gate on it. */
int csprng_crypto_ok(void);

/* Convenience: one random uint64_t. */
uint64_t csprng_u64(void);

/* Mix caller-provided entropy into the global key. 'quality' uses the
 * entropy model classes (kernel/entropy.h); LOW/NONE input still mixes
 * (Blake2b conditioning -- extra input can never hurt) but is not
 * credited. The input is hashed OUTSIDE the lock; only the bounded
 * 32-byte digest mix runs locked. */
void csprng_add_entropy(const void *buf, uint32_t len,
                        entropy_quality_t quality);

/* 1 once csprng_init() (or an emergency seed) has run. */
int csprng_is_seeded(void);

/* Register the NtGetRandom SSDT handler (service number SSDT_NtGetRandom).
 * Called from the boot SSDT registration block after ssdt_init().
 * Returns 0 on success, non-zero on registration failure. */
int csprng_register_ssdt(void);
