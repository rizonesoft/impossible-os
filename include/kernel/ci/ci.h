/* ci.h -- Code Integrity policy object.
 *
 * The kernel decision point for whether executable code may run. This header
 * defines the policy DATA MODEL and the read-only accessor surface; the load /
 * authenticate / seal lifecycle lives in ci_policy.c and later sections own
 * image validation (section 2), the crypto bridge (section 3), catalogs,
 * revocation, and enforcement.
 *
 * Design invariants (settled by the section-1 design pass, folded into the
 * plan before code):
 *   - `enforcement` is an ORDERED scalar (higher = more restrictive) backed by
 *     the policy_lock.c CODE_INTEGRITY ratchet. `test_signing` and
 *     `measurement` are SEPARATE permissive flags, NOT points on that axis --
 *     giving them ordinals would let a permissive flag flip masquerade as an
 *     enforcement upgrade past the ratchet.
 *   - the whole policy (scalars + trust-anchor + revoked collections) is built
 *     and authenticated as ONE candidate snapshot, then published + sealed
 *     together at POLICY_PHASE_POST_REGISTRY -- never half-sealed.
 *   - post-seal mutation is denied logically (ratchet: KernelMode write ->
 *     KeBugCheckEx). Physical RO-after-lock of the policy page is deferred:
 *     it needs SMP TLB shootdown, which is unimplemented (a separate SMP TLB-shootdown work item).
 */
#ifndef KERNEL_CI_CI_H
#define KERNEL_CI_CI_H

#include "kernel/types.h"
#include "kernel/boot_init.h"   /* bool */

/* Enforcement strength -- ORDERED (a downgrade is a lower value). */
typedef enum ci_enforcement {
    CI_ENFORCE_DISABLED   = 0,  /* no CI checks */
    CI_ENFORCE_AUDIT      = 1,  /* evaluate + log, but allow */
    CI_ENFORCE_ENFORCE    = 2,  /* deny on validation failure */
    CI_ENFORCE_SECUREBOOT = 3,  /* enforce + Secure-Boot-locked (no relax) */
    CI_ENFORCE_MAX
} ci_enforcement_t;

/* Secure Boot state as CI sees it. Relaxations (test-signing,
 * nointegritychecks) are honored ONLY when SB is authoritatively KNOWN_OFF;
 * ACTIVE and UNKNOWN both fail closed (no relax). Mirrors the fail-closed
 * gate in feature.c (secure_boot_enabled==0 AND not UNREADABLE). */
typedef enum ci_sb_state {
    CI_SB_KNOWN_OFF = 0,  /* firmware read: Secure Boot inactive */
    CI_SB_ACTIVE    = 1,  /* firmware read: Secure Boot active */
    CI_SB_UNKNOWN   = 2,  /* state unreadable (degraded trust) -> treat as active */
    CI_SB_MAX
} ci_sb_state_t;

/* Trust-anchor tiers -- role-separated, each with distinct lock semantics. */
typedef enum ci_trust_tier {
    CI_TIER_COMPILED = 0,  /* compiled-in root (immutable) */
    CI_TIER_BOOT     = 1,  /* boot-added */
    CI_TIER_FIRMWARE = 2,  /* firmware / platform-supplied */
    CI_TIER_REVOKED  = 3,  /* revoked (deny even if otherwise valid) */
    CI_TIER_MAX
} ci_trust_tier_t;

#define CI_HASH_LEN     32  /* SHA-256 digest */
#define CI_MAX_ANCHORS  16
#define CI_MAX_REVOKED  64

/* Audit-emission flags (which decisions land in the tamper-evident log). */
#define CI_AUDIT_ALLOW    (1u << 0)
#define CI_AUDIT_DENY     (1u << 1)
#define CI_AUDIT_MEASURE  (1u << 2)
#define CI_AUDIT_ALL      (CI_AUDIT_ALLOW | CI_AUDIT_DENY | CI_AUDIT_MEASURE)

/* One trust anchor: an identity key-id plus its tier. */
typedef struct ci_trust_anchor {
    uint8_t key_id[CI_HASH_LEN];  /* anchor identity (pubkey hash) */
    uint8_t tier;                 /* ci_trust_tier_t */
    uint8_t _pad[7];              /* align to 8 */
} ci_trust_anchor_t;

_Static_assert(sizeof(ci_trust_anchor_t) == 40,
    "ci_trust_anchor_t layout is serialized into the policy artifact");

/* The policy object. Built as a candidate, authenticated, then sealed once. */
typedef struct ci_policy {
    uint32_t enforcement;   /* ci_enforcement_t (ordered ratchet scalar) */
    uint32_t audit_flags;   /* CI_AUDIT_* bitmask */
    uint32_t version;       /* policy artifact version (anti-rollback floor) */
    uint32_t anchor_count;  /* valid entries in anchors[] */
    uint32_t revoked_count; /* valid entries in revoked[] */
    uint8_t  test_signing;  /* permissive flag (gated by SAFE_COMP_CI_RELAX) */
    uint8_t  measurement;   /* permissive flag: record hash -> PCR, no verdict */
    uint8_t  secure_boot;   /* boot_info.secure_boot_enabled snapshot */
    uint8_t  sealed;        /* 1 once published + sealed at POST_REGISTRY */
    ci_trust_anchor_t anchors[CI_MAX_ANCHORS];
    uint8_t  revoked[CI_MAX_REVOKED][CI_HASH_LEN];
} ci_policy_t;

_Static_assert(sizeof(((ci_policy_t *)0)->revoked) == CI_MAX_REVOKED * CI_HASH_LEN,
    "revoked list is a flat fixed array -- no pointer-backed storage in the sealed object");

/* Lifecycle: build defaults, merge config (gated), authenticate the artifact,
 * publish + seal at POLICY_PHASE_POST_REGISTRY. Called once, Phase 3. */
void ci_init(void);

/* Pure policy builder: fill `out` from compiled defaults + the ALREADY
 * safe-mode-gated effective config (caller resolves relax_allowed). Relaxations
 * apply only when `sb == CI_SB_KNOWN_OFF`; ACTIVE pins SECUREBOOT enforcement
 * with permissive flags forced off; UNKNOWN falls back to ENFORCE. Reads no
 * live boot state, so unit tests drive every branch with fixtures. */
void ci_policy_build_defaults(ci_policy_t *out, ci_sb_state_t sb,
                              uint8_t cfg_testsigning, uint8_t cfg_noci,
                              bool relax_allowed);

/* Read-only accessors -- lockless hot path (the image-load caller cannot take
 * a lock on every executable-page map). Safe only after ci_init seals. */
const ci_policy_t *ci_policy_get(void);
ci_enforcement_t   ci_get_enforcement(void);
bool ci_is_test_signing(void);
bool ci_is_measurement(void);
bool ci_policy_sealed(void);

/* True if `hash` (SHA-256) is on the revoked list -- overrides any allow. */
bool ci_hash_revoked(const uint8_t hash[CI_HASH_LEN]);

#endif /* KERNEL_CI_CI_H */
