/* test_ci.c -- Code Integrity policy object.
 *
 * Exercises the pure ci_policy_build_defaults() builder (ordered enforcement
 * scalar, fail-closed Secure-Boot + safe-mode gating of the permissive flags,
 * flag/enforcement orthogonality) and the CONSERVATIVE pre-publication behavior
 * of the lockless accessors (before ci_init, s_ready is 0, so every accessor
 * fails closed). ci_init itself is never called from a test -- it reads live
 * boot state, which the test-side-effect policy forbids.
 */
#include "kernel/test/test.h"
#include "kernel/ci/ci.h"
#include "kernel/ci/ci_image.h"
#include "kernel/ci/ci_crypto.h"
#include "kernel/crypto/sha256.h"
#include "kernel/crypto/hash.h"
#include "libs/monocypher/monocypher.h"
#include "libs/monocypher/monocypher-ed25519.h"
#include "libc/string.h"   /* memset, memcmp */

/* Active Secure Boot pins the locked SECUREBOOT level AND forces every
 * permissive flag off, even when safe mode would otherwise allow a relax. */
static void test_ci_secureboot_pins_and_forces_off(void)
{
    ci_policy_t p;
    ci_policy_build_defaults(&p, CI_SB_ACTIVE, /*ts=*/1, /*noci=*/1, /*relax=*/true);
    TEST_ASSERT_EQ(p.enforcement, (uint32_t)CI_ENFORCE_SECUREBOOT,
        "active Secure Boot pins SECUREBOOT enforcement");
    TEST_ASSERT_EQ(p.test_signing, 0u,
        "test-signing forced off under active Secure Boot (no relax)");
    TEST_ASSERT_EQ(p.sb_state, (uint32_t)CI_SB_ACTIVE, "active SB recorded in sb_state");
    TEST_ASSERT_EQ(p.sealed, 0u, "builder does not seal -- ci_init seals");
}

/* Unreadable/unknown Secure Boot must fail CLOSED: relaxations are refused and
 * enforcement stays ENFORCE, exactly as for an active state. */
static void test_ci_sb_unknown_failclosed(void)
{
    ci_policy_t p;
    ci_policy_build_defaults(&p, CI_SB_UNKNOWN, /*ts=*/1, /*noci=*/1, /*relax=*/true);
    TEST_ASSERT_EQ(p.test_signing, 0u, "unknown SB refuses test-signing");
    TEST_ASSERT_EQ(p.enforcement, (uint32_t)CI_ENFORCE_ENFORCE,
        "unknown SB refuses nointegritychecks -> stays ENFORCE");
    TEST_ASSERT_EQ(p.sb_state, (uint32_t)CI_SB_UNKNOWN,
        "unknown SB persists as UNKNOWN, distinct from KNOWN_OFF (degraded trust != off)");

    /* The sealed snapshot must let a consumer tell degraded-unknown from
     * authoritatively-off: the two states persist to different sb_state values. */
    ci_policy_t off;
    ci_policy_build_defaults(&off, CI_SB_KNOWN_OFF, /*ts=*/0, /*noci=*/0, /*relax=*/false);
    TEST_ASSERT_EQ(off.sb_state, (uint32_t)CI_SB_KNOWN_OFF, "known-off persists as KNOWN_OFF");
    TEST_ASSERT_NEQ(off.sb_state, p.sb_state, "KNOWN_OFF and UNKNOWN are stored distinctly");
}

/* Relaxations are honored ONLY when safe mode allows AND SB is known-off. */
static void test_ci_relax_gating(void)
{
    ci_policy_t denied;
    ci_policy_build_defaults(&denied, CI_SB_KNOWN_OFF, /*ts=*/1, /*noci=*/1, /*relax=*/false);
    TEST_ASSERT_EQ(denied.test_signing, 0u, "safe mode denies relax -> test-signing off");
    TEST_ASSERT_EQ(denied.enforcement, (uint32_t)CI_ENFORCE_ENFORCE,
        "safe mode denies relax -> stays ENFORCE");

    ci_policy_t allowed;
    ci_policy_build_defaults(&allowed, CI_SB_KNOWN_OFF, /*ts=*/1, /*noci=*/1, /*relax=*/true);
    TEST_ASSERT_EQ(allowed.test_signing, 1u, "known-off + relax allowed -> test-signing honored");
    TEST_ASSERT_EQ(allowed.enforcement, (uint32_t)CI_ENFORCE_AUDIT,
        "authorized nointegritychecks drops to AUDIT (observe, allow)");
}

/* The enforcement scalar is ORDERED (the ratchet depends on this). */
static void test_ci_enforcement_ordered(void)
{
    TEST_ASSERT(CI_ENFORCE_DISABLED < CI_ENFORCE_AUDIT, "DISABLED < AUDIT");
    TEST_ASSERT(CI_ENFORCE_AUDIT < CI_ENFORCE_ENFORCE, "AUDIT < ENFORCE");
    TEST_ASSERT(CI_ENFORCE_ENFORCE < CI_ENFORCE_SECUREBOOT, "ENFORCE < SECUREBOOT");
}

/* The permissive flags are ORTHOGONAL to the enforcement scalar -- flipping one
 * (when it is even allowed) must not move enforcement. */
static void test_ci_flags_orthogonal(void)
{
    ci_policy_t off, on;
    ci_policy_build_defaults(&off, CI_SB_KNOWN_OFF, /*ts=*/0, /*noci=*/0, /*relax=*/true);
    ci_policy_build_defaults(&on,  CI_SB_KNOWN_OFF, /*ts=*/1, /*noci=*/0, /*relax=*/true);
    TEST_ASSERT_NEQ(off.test_signing, on.test_signing, "test_signing flag flipped");
    TEST_ASSERT_EQ(off.enforcement, on.enforcement,
        "test_signing flip does not change the enforcement scalar");
    TEST_ASSERT_EQ(on.enforcement, (uint32_t)CI_ENFORCE_ENFORCE,
        "no nointegritychecks relaxation -> ENFORCE regardless of test-signing");
}

/* A fresh policy starts with empty collections and all decisions audited. */
static void test_ci_collections_empty(void)
{
    ci_policy_t p;
    ci_policy_build_defaults(&p, CI_SB_ACTIVE, /*ts=*/0, /*noci=*/0, /*relax=*/false);
    TEST_ASSERT_EQ(p.anchor_count, 0u, "no anchors until the policy-root key is compiled in");
    TEST_ASSERT_EQ(p.revoked_count, 0u, "no revoked hashes by default");
    TEST_ASSERT_EQ(p.audit_flags, (uint32_t)CI_AUDIT_ALL, "all decision classes audited");
    TEST_ASSERT_EQ(p.measurement, 0u, "default is a verdict mode, not measure-only");
}

/* Before ci_init publishes (s_ready == 0), every accessor fails CLOSED:
 * enforcement is ENFORCE (never DISABLED), sealed is false, flags are off, and
 * every hash (incl. NULL) is reported revoked. (ci_init is not wired into a test.) */
static void test_ci_preinit_failclosed(void)
{
    TEST_ASSERT_EQ(ci_policy_sealed(), false, "not sealed before ci_init");
    TEST_ASSERT_EQ(ci_get_enforcement(), (uint32_t)CI_ENFORCE_ENFORCE,
        "pre-init enforcement fails closed to ENFORCE, not DISABLED");
    TEST_ASSERT_EQ(ci_is_test_signing(), false, "pre-init test-signing off");
    TEST_ASSERT_NULL(ci_policy_get(), "pre-init ci_policy_get returns NULL");
    uint8_t zero[CI_HASH_LEN] = {0};
    TEST_ASSERT_EQ(ci_hash_revoked(zero), true,
        "pre-init treats every hash as revoked (fail closed, deny until sealed)");
    TEST_ASSERT_EQ(ci_hash_revoked((const uint8_t *)0), true,
        "a NULL digest always denies (no measurement -> fail closed), pre- or post-seal");
}

/* Red-green protection for the post-publication NULL-digest deny. Publishing a
 * fixture (s_ready=1) via the test seam reaches the code path the pre-init test
 * cannot: with the policy sealed, a NULL digest must STILL deny (the pre-fix
 * fail-open code returned false here). Also exercises the published revocation
 * scan: a listed hash denies, an unlisted hash allows. Results are captured
 * BEFORE reset so a failing assert cannot leak s_ready=1 into later suites. */
static void test_ci_published_null_and_scan(void)
{
    ci_policy_t fx;
    ci_policy_build_defaults(&fx, CI_SB_KNOWN_OFF, /*ts=*/0, /*noci=*/0, /*relax=*/false);
    fx.revoked_count = 1;
    for (int b = 0; b < CI_HASH_LEN; b++) {
        fx.revoked[0][b] = (uint8_t)(b + 1);
    }
    ci_policy_publish_for_test(&fx);

    uint8_t listed[CI_HASH_LEN];
    for (int b = 0; b < CI_HASH_LEN; b++) {
        listed[b] = (uint8_t)(b + 1);
    }
    uint8_t unlisted[CI_HASH_LEN] = {0};

    bool null_denied     = ci_hash_revoked((const uint8_t *)0);
    bool listed_revoked  = ci_hash_revoked(listed);
    bool unlisted_allowed = ci_hash_revoked(unlisted);
    bool sealed_now      = ci_policy_sealed();
    ci_policy_reset_for_test();

    TEST_ASSERT_EQ(sealed_now, true, "fixture published -> sealed");
    TEST_ASSERT_EQ(null_denied, true, "published: NULL digest still denies (fail closed)");
    TEST_ASSERT_EQ(listed_revoked, true, "published: a revoked hash is reported revoked");
    TEST_ASSERT_EQ(unlisted_allowed, false, "published: a non-revoked hash is not revoked");
    TEST_ASSERT_EQ(ci_policy_sealed(), false, "reset restores fail-closed pre-publish state");
}

/* A tiny fixed image buffer for the admission-engine tests. */
static const uint8_t k_ci_test_image[16] = {
    0x7f, 'E', 'L', 'F', 0x02, 0x01, 0x01, 0x00,
    0xde, 0xad, 0xbe, 0xef, 0x11, 0x22, 0x33, 0x44,
};

static void ci_fill_image(ci_image_info_t *img)
{
    memset(img, 0, sizeof(*img));
    img->path   = "C:\\Impossible\\test.exe";
    img->type   = CI_IMAGE_USER;
    img->loader = CI_LOADER_ELF;
    img->data   = k_ci_test_image;
    img->size   = sizeof(k_ci_test_image);
}

/* Under ENFORCE an unsigned image is UNVERIFIED and denied; under AUDIT the same
 * image is audit-allowed. This is the core fail-closed-vs-observe split. */
static void test_ci_image_enforce_vs_audit(void)
{
    ci_policy_t enf;
    ci_policy_build_defaults(&enf, CI_SB_KNOWN_OFF, /*ts=*/0, /*noci=*/0, /*relax=*/false);
    ci_policy_publish_for_test(&enf);
    ci_image_info_t img_e; ci_decision_t dec_e;
    ci_fill_image(&img_e);
    ci_verdict_t v_enf = ci_validate_image(&img_e, &dec_e);
    ci_policy_reset_for_test();

    ci_policy_t aud;
    ci_policy_build_defaults(&aud, CI_SB_KNOWN_OFF, /*ts=*/0, /*noci=*/1, /*relax=*/true);
    ci_policy_publish_for_test(&aud);
    ci_image_info_t img_a; ci_decision_t dec_a;
    ci_fill_image(&img_a);
    ci_verdict_t v_aud = ci_validate_image(&img_a, &dec_a);
    ci_policy_reset_for_test();

    TEST_ASSERT_EQ(v_enf, CI_VERDICT_DENY, "ENFORCE denies an unverified image");
    TEST_ASSERT_EQ(dec_e.reason, CI_REASON_UNVERIFIED, "ENFORCE deny reason is UNVERIFIED");
    TEST_ASSERT_EQ(v_aud, CI_VERDICT_AUDIT_ALLOW, "AUDIT audit-allows an unverified image");
    TEST_ASSERT_EQ(dec_a.reason, CI_REASON_UNVERIFIED, "AUDIT allow reason is UNVERIFIED");
}

/* Revocation overrides the (would-be) verdict, and the validator recomputes the
 * digest over the actual bytes -- a caller-supplied hash is never trusted. */
static void test_ci_image_revoked_and_digest(void)
{
    uint8_t expect[CI_HASH_LEN];
    sha256(k_ci_test_image, sizeof(k_ci_test_image), expect);

    ci_policy_t p;
    ci_policy_build_defaults(&p, CI_SB_KNOWN_OFF, /*ts=*/0, /*noci=*/0, /*relax=*/false);
    p.revoked_count = 1;
    for (int b = 0; b < CI_HASH_LEN; b++) p.revoked[0][b] = expect[b];
    ci_policy_publish_for_test(&p);

    ci_image_info_t img; ci_decision_t dec;
    ci_fill_image(&img);
    memset(img.hash, 0xFF, sizeof(img.hash));     /* garbage caller hash, must be ignored */
    memset(img.signer, 0xAA, sizeof(img.signer)); /* garbage OUT fields, must be cleared */
    img.measured = 1;
    ci_verdict_t v = ci_validate_image(&img, &dec);

    int digest_ok = 1, signer_cleared = 1;
    for (int b = 0; b < CI_HASH_LEN; b++) {
        if (img.hash[b] != expect[b]) digest_ok = 0;
        if (img.signer[b] != 0) signer_cleared = 0;
    }
    ci_policy_reset_for_test();

    TEST_ASSERT_EQ(v, CI_VERDICT_DENY, "a revoked digest is denied under ENFORCE");
    TEST_ASSERT_EQ(dec.reason, CI_REASON_REVOKED, "deny reason is REVOKED (overrides unverified)");
    TEST_ASSERT_EQ(digest_ok, 1, "validator recomputes the digest, ignoring the caller hash");
    TEST_ASSERT_EQ(signer_cleared, 1, "validator clears a caller-prefilled signer (zeroed-output)");
    TEST_ASSERT_EQ(img.measured, 0u, "measured stays 0 -- no attestation claimed until PCR binding");
}

/* Bad requests and an unsealed policy fail closed; an explicitly DISABLED policy
 * allows without checks. */
static void test_ci_image_failclosed_and_disabled(void)
{
    ci_decision_t dec;

    /* NULL image and empty buffer deny (pre-seal, enforcement reads ENFORCE). */
    TEST_ASSERT_EQ(ci_validate_image((ci_image_info_t *)0, &dec), CI_VERDICT_DENY,
        "NULL image denies");
    ci_image_info_t empty; ci_fill_image(&empty); empty.size = 0;
    TEST_ASSERT_EQ(ci_validate_image(&empty, &dec), CI_VERDICT_DENY, "empty image denies");
    TEST_ASSERT_EQ(dec.reason, CI_REASON_BAD_REQUEST, "empty image reason is BAD_REQUEST");

    /* Valid image but no sealed policy -> NOT_READY deny. */
    ci_policy_reset_for_test();
    ci_image_info_t img; ci_fill_image(&img);
    TEST_ASSERT_EQ(ci_validate_image(&img, &dec), CI_VERDICT_DENY, "unsealed policy denies");
    TEST_ASSERT_EQ(dec.reason, CI_REASON_NOT_READY, "unsealed reason is NOT_READY");

    /* A sealed policy that disabled CI allows. */
    ci_policy_t off;
    ci_policy_build_defaults(&off, CI_SB_KNOWN_OFF, /*ts=*/0, /*noci=*/0, /*relax=*/false);
    off.enforcement = (uint32_t)CI_ENFORCE_DISABLED;
    ci_policy_publish_for_test(&off);
    ci_image_info_t img2; ci_decision_t dec2; ci_fill_image(&img2);
    ci_verdict_t v = ci_validate_image(&img2, &dec2);
    ci_policy_reset_for_test();
    TEST_ASSERT_EQ(v, CI_VERDICT_ALLOW, "DISABLED CI allows without checks");
    TEST_ASSERT_EQ(dec2.reason, CI_REASON_DISABLED, "allow reason is DISABLED");

    /* Revocation overrides even a DISABLED policy: a revoked digest is denied
     * with CI otherwise off (the kill-switch is not bypassed by DISABLED). */
    uint8_t digest[CI_HASH_LEN];
    sha256(k_ci_test_image, sizeof(k_ci_test_image), digest);
    ci_policy_t offrev;
    ci_policy_build_defaults(&offrev, CI_SB_KNOWN_OFF, /*ts=*/0, /*noci=*/0, /*relax=*/false);
    offrev.enforcement  = (uint32_t)CI_ENFORCE_DISABLED;
    offrev.revoked_count = 1;
    for (int b = 0; b < CI_HASH_LEN; b++) offrev.revoked[0][b] = digest[b];
    ci_policy_publish_for_test(&offrev);
    ci_image_info_t img3; ci_decision_t dec3; ci_fill_image(&img3);
    ci_verdict_t vr = ci_validate_image(&img3, &dec3);
    ci_policy_reset_for_test();
    TEST_ASSERT_EQ(vr, CI_VERDICT_DENY, "revoked image denied even under DISABLED CI");
    TEST_ASSERT_EQ(dec3.reason, CI_REASON_REVOKED, "DISABLED-revoked reason is REVOKED");

    /* NULL out fails closed (cannot communicate a decision -> deny). */
    TEST_ASSERT_EQ(ci_validate_image(&img3, (ci_decision_t *)0), CI_VERDICT_DENY,
        "NULL decision out denies");
}

/* Dynamic-code admission: W+X is refused outright; a non-W+X runtime request is
 * UNVERIFIED (denied under ENFORCE, audit-allowed under AUDIT). */
static void test_ci_dynamic_code(void)
{
    ci_policy_t enf;
    ci_policy_build_defaults(&enf, CI_SB_KNOWN_OFF, /*ts=*/0, /*noci=*/0, /*relax=*/false);
    ci_policy_publish_for_test(&enf);
    ci_decision_t wx, rx;
    ci_verdict_t v_wx = ci_validate_dynamic_code(0, /*writable_and_exec=*/true, &wx);
    ci_verdict_t v_rx = ci_validate_dynamic_code(0, /*writable_and_exec=*/false, &rx);
    ci_policy_reset_for_test();

    ci_policy_t aud;
    ci_policy_build_defaults(&aud, CI_SB_KNOWN_OFF, /*ts=*/0, /*noci=*/1, /*relax=*/true);
    ci_policy_publish_for_test(&aud);
    ci_decision_t ra;
    ci_verdict_t v_ra = ci_validate_dynamic_code(0, /*writable_and_exec=*/false, &ra);
    ci_policy_reset_for_test();

    /* W+X is refused UNCONDITIONALLY -- even a DISABLED policy must not admit a
     * writable+executable mapping (W^X invariant, not a CI-mode toggle). */
    ci_policy_t off;
    ci_policy_build_defaults(&off, CI_SB_KNOWN_OFF, /*ts=*/0, /*noci=*/0, /*relax=*/false);
    off.enforcement = (uint32_t)CI_ENFORCE_DISABLED;
    ci_policy_publish_for_test(&off);
    ci_decision_t owx, orx;
    ci_verdict_t v_owx = ci_validate_dynamic_code(0, /*writable_and_exec=*/true, &owx);
    ci_verdict_t v_orx = ci_validate_dynamic_code(0, /*writable_and_exec=*/false, &orx);
    ci_policy_reset_for_test();

    TEST_ASSERT_EQ(v_wx, CI_VERDICT_DENY, "W+X dynamic code refused under ENFORCE");
    TEST_ASSERT_EQ(wx.reason, CI_REASON_WX, "W+X deny reason is WX");
    TEST_ASSERT_EQ(v_rx, CI_VERDICT_DENY, "non-W+X dynamic code denied under ENFORCE (unverified)");
    TEST_ASSERT_EQ(v_ra, CI_VERDICT_AUDIT_ALLOW, "non-W+X dynamic code audit-allowed under AUDIT");
    TEST_ASSERT_EQ(v_owx, CI_VERDICT_DENY, "W+X refused even under DISABLED CI (W^X invariant)");
    TEST_ASSERT_EQ(owx.reason, CI_REASON_WX, "DISABLED W+X deny reason is WX");
    TEST_ASSERT_EQ(v_orx, CI_VERDICT_ALLOW, "non-W+X runtime code allowed under DISABLED CI");
}

/* The CI crypto bridge: digest dispatch matches the underlying primitives,
 * fails closed on bad args, and NULL-data-with-length is refused (no fault). */
static void test_ci_crypto_digest(void)
{
    static const uint8_t msg[8] = { 'c', 'i', '-', 't', 'e', 's', 't', '!' };
    uint8_t out[48];

    uint8_t sha_ref[32];
    sha256(msg, sizeof(msg), sha_ref);
    int n = ci_crypto_digest(CI_DIGEST_SHA256, msg, sizeof(msg), out, sizeof(out));
    TEST_ASSERT_EQ(n, 32, "SHA-256 digest length is 32");
    TEST_ASSERT_EQ(memcmp(out, sha_ref, 32), 0, "SHA-256 bridge matches sha256()");

    uint8_t blake_ref[32];
    crypto_blake2b(blake_ref, 32, msg, sizeof(msg));
    n = ci_crypto_digest(CI_DIGEST_BLAKE2B_256, msg, sizeof(msg), out, sizeof(out));
    TEST_ASSERT_EQ(n, 32, "BLAKE2b-256 digest length is 32");
    TEST_ASSERT_EQ(memcmp(out, blake_ref, 32), 0, "BLAKE2b bridge matches crypto_blake2b()");

    TEST_ASSERT_EQ(ci_crypto_digest(CI_DIGEST_MAX, msg, sizeof(msg), out, sizeof(out)), -1,
        "unknown digest alg -> -1");
    TEST_ASSERT_EQ(ci_crypto_digest(CI_DIGEST_SHA256, msg, sizeof(msg), out, 16), -1,
        "out_cap too small -> -1");
    TEST_ASSERT_EQ(ci_crypto_digest(CI_DIGEST_BLAKE2B_256, (const void *)0, 8, out, sizeof(out)), -1,
        "NULL data with len>0 fails closed (no BLAKE2b NULL deref)");
    TEST_ASSERT_EQ(ci_crypto_digest(CI_DIGEST_SHA256, (const void *)0, 0, out, sizeof(out)), 32,
        "NULL data with len 0 is a valid empty input");

    /* SHA-384 is a distinct alg constant + length (48) -- exercise it too. */
    uint8_t sha384_ref[48];
    crypto_hash(CRYPTO_HASH_SHA384, msg, sizeof(msg), sha384_ref, sizeof(sha384_ref));
    n = ci_crypto_digest(CI_DIGEST_SHA384, msg, sizeof(msg), out, sizeof(out));
    TEST_ASSERT_EQ(n, 48, "SHA-384 digest length is 48");
    TEST_ASSERT_EQ(memcmp(out, sha384_ref, 48), 0, "SHA-384 bridge matches crypto_hash()");

    /* NULL output denies (cannot write a digest). */
    TEST_ASSERT_EQ(ci_crypto_digest(CI_DIGEST_SHA256, msg, sizeof(msg), (uint8_t *)0, 32), -1,
        "NULL out buffer -> -1");
}

/* Ed25519 signature verification is the security-critical path: valid only when
 * the signature actually verifies; every tamper/wrong-key/unsupported/NULL case
 * denies (fail closed). */
static void test_ci_crypto_verify(void)
{
    /* Mutable seed: crypto_ed25519_key_pair() wipes it in place (a const seed in
     * .rodata would #PF on the wipe). */
    uint8_t seed[32] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
        17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    };
    static const uint8_t msg[11] = { 'i', 'm', 'a', 'g', 'e', '-', 'b', 'y', 't', 'e', 's' };
    uint8_t secret[64], pub[32], sig[64];
    crypto_ed25519_key_pair(secret, pub, seed);
    crypto_ed25519_sign(sig, secret, msg, sizeof(msg));

    TEST_ASSERT_EQ(ci_crypto_verify(CI_SIG_ED25519, msg, sizeof(msg), sig, 64, pub, 32), true,
        "a valid Ed25519 signature verifies");

    uint8_t bad_sig[64];
    memcpy(bad_sig, sig, 64);
    bad_sig[0] ^= 0x01;
    TEST_ASSERT_EQ(ci_crypto_verify(CI_SIG_ED25519, msg, sizeof(msg), bad_sig, 64, pub, 32), false,
        "a tampered signature is rejected");

    uint8_t wrong_pub[32];
    memcpy(wrong_pub, pub, 32);
    wrong_pub[0] ^= 0x01;
    TEST_ASSERT_EQ(ci_crypto_verify(CI_SIG_ED25519, msg, sizeof(msg), sig, 64, wrong_pub, 32), false,
        "verification under the wrong key fails");

    TEST_ASSERT_EQ(ci_crypto_verify(CI_SIG_RSA_PKCS1, msg, sizeof(msg), sig, 64, pub, 32), false,
        "unsupported RSA algorithm denies (reserved)");
    TEST_ASSERT_EQ(ci_crypto_verify(CI_SIG_ED25519, msg, sizeof(msg), (const uint8_t *)0, 64, pub, 32), false,
        "NULL signature denies");
    TEST_ASSERT_EQ(ci_crypto_verify(CI_SIG_ED25519, msg, sizeof(msg), sig, 63, pub, 32), false,
        "wrong signature length denies");
    TEST_ASSERT_EQ(ci_crypto_verify(CI_SIG_ED25519, msg, sizeof(msg), sig, 64, pub, 31), false,
        "wrong public-key length denies");
    TEST_ASSERT_EQ(ci_crypto_verify(CI_SIG_ED25519, msg, sizeof(msg), sig, 64, (const uint8_t *)0, 32), false,
        "NULL public key denies");
    TEST_ASSERT_EQ(ci_crypto_verify(CI_SIG_ED25519, (const uint8_t *)0, sizeof(msg), sig, 64, pub, 32), false,
        "NULL message with a nonzero length denies");

    /* An empty message (NULL msg, len 0) is a VALID input, NOT a deny: a
     * signature over the empty message verifies. This pins the single NULL case
     * that the contract intentionally allows. */
    uint8_t sig_empty[64];
    crypto_ed25519_sign(sig_empty, secret, (const uint8_t *)0, 0);
    TEST_ASSERT_EQ(ci_crypto_verify(CI_SIG_ED25519, (const uint8_t *)0, 0, sig_empty, 64, pub, 32), true,
        "empty message (NULL, len 0) with a valid signature verifies");
}

void test_register_ci(void)
{
    test_suite_register_cat("CI: Secure Boot pins + forces flags off",
        test_ci_secureboot_pins_and_forces_off, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: unknown Secure Boot fails closed",
        test_ci_sb_unknown_failclosed, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: relax safe-mode gating",
        test_ci_relax_gating, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: enforcement scalar ordered",
        test_ci_enforcement_ordered, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: permissive flags orthogonal",
        test_ci_flags_orthogonal, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: collections empty by default",
        test_ci_collections_empty, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: pre-init accessors fail closed",
        test_ci_preinit_failclosed, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: published NULL denies + revocation scan",
        test_ci_published_null_and_scan, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: image ENFORCE denies / AUDIT audit-allows",
        test_ci_image_enforce_vs_audit, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: image revoked overrides + digest recomputed",
        test_ci_image_revoked_and_digest, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: image bad-request / not-ready / disabled",
        test_ci_image_failclosed_and_disabled, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: dynamic code W+X refused + mode mapping",
        test_ci_dynamic_code, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: crypto digest bridge (SHA/BLAKE2b + fail-closed)",
        test_ci_crypto_digest, TEST_CAT_SECURITY);
    test_suite_register_cat("CI: crypto Ed25519 verify (valid + tamper/wrong-key/unsupported)",
        test_ci_crypto_verify, TEST_CAT_SECURITY);
}
