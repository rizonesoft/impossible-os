/* ci_policy.c -- Code Integrity policy object.
 *
 * Owns the ci_policy_t lifecycle: build compiled defaults merged with the
 * (safe-mode + Secure-Boot-gated) effective config, publish once with
 * release/acquire ordering, then serve lockless read-only accessors on the
 * image-admission hot path.
 *
 * Two security invariants:
 *   - Relaxations (test-signing, nointegritychecks) are honored ONLY when
 *     Secure Boot is AUTHORITATIVELY known-off. Active or unreadable/unknown SB
 *     forces every permissive flag off and never drops below ENFORCE -- fail
 *     closed, mirroring feature.c's security_disable_allowed().
 *   - Publication is release/acquire, not a bare struct copy: s_policy is built
 *     fully, then s_ready is release-stored; every accessor acquire-loads
 *     s_ready first and returns CONSERVATIVE (fail-closed) values before it is
 *     set. This prevents a torn read on the ~2.7 KiB record and a fail-open
 *     window before ci_init.
 */
#include "kernel/ci/ci.h"
#include "kernel/config.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "libc/string.h"   /* memset */

/* The single policy. Built by ci_init (one boot-path writer), then published. */
static ci_policy_t s_policy;

/* Publication flag. 0 until ci_init release-stores 1; accessors acquire-load
 * it before touching s_policy. This is the happens-before edge that makes the
 * s_policy build visible without tearing. */
static uint32_t s_ready;

static inline bool ci_ready(void)
{
    return __atomic_load_n(&s_ready, __ATOMIC_ACQUIRE) != 0;
}

/* Pure builder -- populate `out` with compiled defaults merged with the
 * already-gated effective config. No live boot state read here, so unit tests
 * drive every branch with fixtures. */
void ci_policy_build_defaults(ci_policy_t *out, ci_sb_state_t sb,
                              uint8_t cfg_testsigning, uint8_t cfg_noci,
                              bool relax_allowed)
{
    memset(out, 0, sizeof(*out));

    /* Relaxations are honored ONLY when safe mode allows AND Secure Boot is
     * authoritatively known-off. Active or unknown SB zeroes them (fail-closed). */
    bool relax_ok = relax_allowed && (sb == CI_SB_KNOWN_OFF);
    uint8_t eff_testsign = relax_ok ? (cfg_testsigning ? 1 : 0) : 0;
    uint8_t eff_noci     = relax_ok ? (cfg_noci ? 1 : 0) : 0;

    out->secure_boot  = (sb == CI_SB_ACTIVE) ? 1 : 0;
    out->test_signing = eff_testsign;   /* forced 0 under ACTIVE/UNKNOWN */
    out->measurement  = 0;              /* default is a verdict mode */

    /* Enforcement default: active SB pins the locked SECUREBOOT level (no
     * relax); else an authorized nointegritychecks relaxation drops to AUDIT;
     * otherwise (including UNKNOWN) ENFORCE. DISABLED is never a default. */
    if (sb == CI_SB_ACTIVE) {
        out->enforcement = CI_ENFORCE_SECUREBOOT;
    } else if (eff_noci) {
        out->enforcement = CI_ENFORCE_AUDIT;
    } else {
        out->enforcement = CI_ENFORCE_ENFORCE;
    }

    out->audit_flags   = CI_AUDIT_ALL;
    out->version       = 1;   /* compiled baseline; an external signed artifact bumps this */
    out->anchor_count  = 0;   /* compiled Ed25519 policy-root key: tracked [ ] item */
    out->revoked_count = 0;
}

/* Resolve Secure Boot into the CI tri-state, fail-closed on an unreadable
 * (degraded-trust) state -- an unknown state must not be honored as "off". */
static ci_sb_state_t ci_sb_state(void)
{
    if (g_boot_info.secure_boot_enabled) {
        return CI_SB_ACTIVE;
    }
    if (g_boot_info.degraded_trust_flags & BOOT_DEGRADED_TRUST_SECURE_BOOT_UNREADABLE) {
        return CI_SB_UNKNOWN;
    }
    return CI_SB_KNOWN_OFF;
}

void ci_init(void)
{
    const kernel_config_t *kc = kernel_config_get();
    uint8_t cfg_ts   = kc ? kc->testsigning : 0;
    uint8_t cfg_noci = kc ? kc->nointegritychecks : 0;
    bool relax_ok    = kernel_safe_mode_allows(SAFE_COMP_CI_RELAX) != 0;

    /* Build the whole record into the storage, then release-publish. The
     * release store below makes this build visible to any acquiring reader;
     * before it, ci_ready() is false and accessors fail closed. */
    ci_policy_build_defaults(&s_policy, ci_sb_state(), cfg_ts, cfg_noci, relax_ok);
    s_policy.sealed = 1;

    __atomic_store_n(&s_ready, 1u, __ATOMIC_RELEASE);

    klog(LOG_INFO, "ci", "policy sealed (enforce=%u, sb=%u, ver=%u)",
         (uint64_t)s_policy.enforcement, (uint64_t)s_policy.secure_boot,
         (uint64_t)s_policy.version);
}

/* Accessors. Before publication they return CONSERVATIVE (fail-closed) values:
 * ENFORCE (never DISABLED), not-sealed, permissive flags off. */
const ci_policy_t *ci_policy_get(void)
{
    return ci_ready() ? &s_policy : (const ci_policy_t *)0;
}

ci_enforcement_t ci_get_enforcement(void)
{
    if (!ci_ready()) {
        return CI_ENFORCE_ENFORCE;   /* fail closed before ci_init */
    }
    return (ci_enforcement_t)s_policy.enforcement;
}

bool ci_is_test_signing(void) { return ci_ready() && s_policy.test_signing != 0; }
bool ci_is_measurement(void)  { return ci_ready() && s_policy.measurement != 0; }
bool ci_policy_sealed(void)   { return ci_ready(); }

bool ci_hash_revoked(const uint8_t hash[CI_HASH_LEN])
{
    /* Fail CLOSED before publication: an unpublished policy cannot vouch for
     * any hash, so treat everything as revoked (deny) until ci_init seals.
     * (Returning false here would fail open -- admission could run revoked
     * code before the revocation list is live.) */
    if (!ci_ready()) {
        return true;
    }
    if (!hash) {
        return false;
    }
    uint32_t n = s_policy.revoked_count;
    if (n > CI_MAX_REVOKED) {
        n = CI_MAX_REVOKED;
    }
    for (uint32_t i = 0; i < n; i++) {
        int diff = 0;
        for (int b = 0; b < CI_HASH_LEN; b++) {
            diff |= s_policy.revoked[i][b] ^ hash[b];  /* full-length compare, no early out */
        }
        if (diff == 0) {
            return true;
        }
    }
    return false;
}
