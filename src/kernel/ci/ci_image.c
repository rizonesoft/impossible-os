/* ci_image.c -- Code Integrity image-admission decision engine.
 *
 * Consumes the sealed CI policy object (ci.h accessors) and turns an image
 * admission request into a verdict. The engine is authoritative and fail-closed;
 * it is deliberately NOT wired into the loader choke points yet (see ci_image.h
 * header docstring for why: the exec path destroys the old image before the
 * loader runs, and no embedded-signature validator exists to make an unsigned
 * image ALLOW under ENFORCE).
 *
 * Decision order (revocation overrides everything -- including a DISABLED policy
 * -- and unverified is denied unless the policy is only auditing):
 *   1. NULL out / NULL image / bad request     -> DENY  (fail closed)
 *   2. policy not sealed                        -> DENY  / NOT_READY
 *   3. digest revoked                           -> DENY  / REVOKED
 *   4. CI disabled                              -> ALLOW / DISABLED
 *   5. otherwise UNVERIFIED, mapped by mode:
 *        ENFORCE / SECUREBOOT                   -> DENY  / UNVERIFIED
 *        AUDIT                                  -> AUDIT_ALLOW / UNVERIFIED
 */
#include "kernel/ci/ci_image.h"
#include "kernel/ci/ci.h"
#include "kernel/ex.h"
#include "kernel/crypto/sha256.h"
#include "kernel/klog.h"
#include "libc/string.h"   /* memset */

/* \Callback\CiImageLoad object, or NULL until ci_image_init creates it. Written
 * once at init (single boot-path writer), read on every validation; release/
 * acquire so a consumer that registered before a producer fires is visible. */
static EX_CALLBACK_OBJECT *s_ci_image_cb;

void ci_image_init(void)
{
    /* Idempotent: ExCreateCallback opens the object if it already exists. */
    EX_CALLBACK_OBJECT *cb = ExCreateCallback("CiImageLoad", /*create=*/true,
                                              /*allow_multiple=*/true);
    __atomic_store_n(&s_ci_image_cb, cb, __ATOMIC_RELEASE);
}

/* Fire \Callback\CiImageLoad with (image, decision-copy) when the object exists.
 * No-op before ci_image_init, so the engine is safe to call unwired. The caller
 * passes a COPY of the decision -- a consumer must not be able to rewrite the
 * authoritative verdict or suppress the deny audit. */
static void ci_image_notify(ci_image_info_t *img, const ci_decision_t *dec_copy)
{
    EX_CALLBACK_OBJECT *cb = __atomic_load_n(&s_ci_image_cb, __ATOMIC_ACQUIRE);
    if (cb) {
        ExNotifyCallback(cb, img, (void *)dec_copy);
    }
}

/* Emit the tamper-evident audit line for a decision. DENY is a warning; an
 * audit-allow is informational; a clean allow under enforcement is quiet. */
static void ci_image_audit(const ci_image_info_t *img, const ci_decision_t *dec)
{
    const char *path = img->path ? img->path : "<buffer>";
    if (dec->verdict == CI_VERDICT_DENY) {
        klog(LOG_WARN, "ci", "deny image '%s' (reason=%u, mode=%u)",
             path, (uint64_t)dec->reason, (uint64_t)dec->enforcement);
    } else if (dec->verdict == CI_VERDICT_AUDIT_ALLOW) {
        klog(LOG_INFO, "ci", "audit-allow image '%s' (reason=%u, mode=%u)",
             path, (uint64_t)dec->reason, (uint64_t)dec->enforcement);
    }
}

/* Map the UNVERIFIED state (no signature/catalog proof) to a verdict by the
 * effective enforcement mode. Fail-closed default: anything not AUDIT denies. */
static void ci_map_unverified(uint32_t enforcement, ci_decision_t *out)
{
    out->reason = CI_REASON_UNVERIFIED;
    if (enforcement == CI_ENFORCE_AUDIT) {
        out->verdict = CI_VERDICT_AUDIT_ALLOW;
    } else {
        out->verdict = CI_VERDICT_DENY;   /* ENFORCE / SECUREBOOT / anything else */
    }
}

/* Single decision tail: capture the verdict BEFORE any callback runs, notify the
 * consumer with an immutable copy, audit from the authoritative record, and
 * return the saved verdict -- so a callback cannot flip the outcome. */
static ci_verdict_t ci_finish(ci_image_info_t *img, ci_decision_t *dec)
{
    ci_verdict_t  result   = dec->verdict;
    ci_decision_t snapshot = *dec;
    ci_image_notify(img, &snapshot);
    ci_image_audit(img, dec);
    return result;
}

ci_verdict_t ci_validate_image(ci_image_info_t *img, ci_decision_t *out)
{
    if (!out) {
        return CI_VERDICT_DENY;   /* fail closed: cannot communicate a decision */
    }
    out->verdict     = CI_VERDICT_DENY;   /* fail-closed defaults */
    out->reason      = CI_REASON_BAD_REQUEST;
    out->enforcement = (uint32_t)CI_ENFORCE_ENFORCE;

    if (!img) {
        return CI_VERDICT_DENY;   /* no image -- nothing to notify/audit */
    }

    uint32_t enforcement = (uint32_t)ci_get_enforcement();
    out->enforcement = enforcement;

    /* 1. Bad request: no bytes, or too large for the one-shot digest to hash
     *    faithfully (never truncate the input to a hash). */
    if (!img->data || img->size == 0 || img->size > CI_IMAGE_MAX_BYTES) {
        out->verdict = CI_VERDICT_DENY;
        out->reason  = CI_REASON_BAD_REQUEST;
        return ci_finish(img, out);
    }

    /* 2. Fail closed before the policy is sealed. */
    if (!ci_policy_sealed()) {
        out->verdict = CI_VERDICT_DENY;
        out->reason  = CI_REASON_NOT_READY;
        return ci_finish(img, out);
    }

    /* 3. ALWAYS recompute the digest over the actual staged bytes -- a
     *    caller-supplied hash is never trusted as the admission input. */
    sha256(img->data, (uint32_t)img->size, img->hash);

    /* 4. Revocation overrides EVERY allow, including a DISABLED policy: a
     *    known-bad image is refused even when CI is otherwise off. */
    if (ci_hash_revoked(img->hash)) {
        out->verdict = CI_VERDICT_DENY;
        out->reason  = CI_REASON_REVOKED;
        return ci_finish(img, out);
    }

    /* 5. CI disabled: no signature/catalog checks (revocation enforced above). */
    if (enforcement == (uint32_t)CI_ENFORCE_DISABLED) {
        out->verdict = CI_VERDICT_ALLOW;
        out->reason  = CI_REASON_DISABLED;
        return ci_finish(img, out);
    }

    /* 6. Measurement mode records the digest for the TPM PCR aggregate. The
     *    actual PCR extend is owned by the measured-boot binding; here we mark. */
    if (ci_is_measurement()) {
        img->measured = 1;
    }

    /* 7. No signature/catalog proof exists yet -> UNVERIFIED, mapped by mode. */
    ci_map_unverified(enforcement, out);
    return ci_finish(img, out);
}

ci_verdict_t ci_validate_dynamic_code(uint64_t token, bool writable_and_exec,
                                      ci_decision_t *out)
{
    if (!out) {
        return CI_VERDICT_DENY;
    }
    (void)token;   /* reserved for the deferred token/provenance trust model */

    out->verdict     = CI_VERDICT_DENY;
    out->reason      = CI_REASON_WX;
    out->enforcement = (uint32_t)ci_get_enforcement();

    /* W+X is refused UNCONDITIONALLY, in every mode: no page is ever both
     * writable and executable (a W^X invariant, not a CI-mode toggle). */
    if (writable_and_exec) {
        out->verdict = CI_VERDICT_DENY;
        out->reason  = CI_REASON_WX;
        klog(LOG_WARN, "ci", "deny dynamic code: W+X refused (mode=%u)",
             (uint64_t)out->enforcement);
        return CI_VERDICT_DENY;
    }

    uint32_t enforcement = out->enforcement;

    if (enforcement == (uint32_t)CI_ENFORCE_DISABLED) {
        out->verdict = CI_VERDICT_ALLOW;
        out->reason  = CI_REASON_DISABLED;
        return CI_VERDICT_ALLOW;
    }

    if (!ci_policy_sealed()) {
        out->verdict = CI_VERDICT_DENY;
        out->reason  = CI_REASON_NOT_READY;
        return CI_VERDICT_DENY;
    }

    /* Runtime-generated code has no image to verify -> UNVERIFIED by mode. */
    ci_map_unverified(enforcement, out);
    if (out->verdict == CI_VERDICT_AUDIT_ALLOW) {
        klog(LOG_INFO, "ci", "audit-allow dynamic code (mode=%u)",
             (uint64_t)enforcement);
    } else {
        klog(LOG_WARN, "ci", "deny dynamic code: unverified (mode=%u)",
             (uint64_t)enforcement);
    }
    return out->verdict;
}
