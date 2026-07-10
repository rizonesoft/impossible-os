/* ci_image.h -- Code Integrity image-admission decision plane.
 *
 * The decision surface every loader (and, later, every runtime code-generation
 * path) consults before executable bytes may run. This layer owns the DECISION
 * ENGINE and the \Callback\CiImageLoad notification point; it consumes the
 * sealed CI policy object (ci.h) and does NOT itself enforce at any loader call
 * site -- wiring the choke points is deferred (tracked in the CI TODO), because:
 *   - a clean deny requires transactional / NX-until-admission remapping in the
 *     exec path (task_exec replaces the image range before the loader runs), and
 *   - a real ALLOW verdict needs embedded-signature / catalog proof, which is
 *     not built yet; until then an UNVERIFIED image is denied under ENFORCE, so
 *     wiring loaders now would refuse every unsigned image at boot.
 *
 * The engine is therefore complete and unit-tested here; the enforcement rollout
 * lands with embedded-signature validation + the policy_lock ratchet-domain
 * merge that unifies the enforcement-mode authority.
 */
#ifndef KERNEL_CI_CI_IMAGE_H
#define KERNEL_CI_CI_IMAGE_H

#include "kernel/types.h"
#include "kernel/ci/ci.h"   /* CI_HASH_LEN, enforcement accessors (pulls boot_init.h bool) */

/* Image class under admission. */
typedef enum ci_image_type {
    CI_IMAGE_USER    = 0,   /* ring-3 user executable */
    CI_IMAGE_DRIVER  = 1,   /* kernel-mode driver / module */
    CI_IMAGE_KERNEL  = 2,   /* the kernel image itself */
    CI_IMAGE_DYNAMIC = 3,   /* runtime-generated code, no backing image */
    CI_IMAGE_TYPE_MAX
} ci_image_type_t;

/* Loader that produced the request (informational + future policy input). */
typedef enum ci_loader_id {
    CI_LOADER_UNKNOWN = 0,
    CI_LOADER_ELF     = 1,
    CI_LOADER_PE      = 2,
    CI_LOADER_EIF     = 3,
    CI_LOADER_DYNAMIC = 4,
    CI_LOADER_MAX
} ci_loader_id_t;

/* Final admission verdict. */
typedef enum ci_verdict {
    CI_VERDICT_ALLOW       = 0,   /* admit */
    CI_VERDICT_AUDIT_ALLOW = 1,   /* admit, but the decision was logged (audit mode) */
    CI_VERDICT_DENY        = 2,   /* refuse -- do not map or execute */
    CI_VERDICT_MAX
} ci_verdict_t;

/* Why a decision landed where it did (drives the audit record). */
typedef enum ci_deny_reason {
    CI_REASON_OK          = 0,   /* clean allow */
    CI_REASON_DISABLED    = 1,   /* CI disabled -- no checks performed */
    CI_REASON_NOT_READY   = 2,   /* policy not sealed yet (fail closed) */
    CI_REASON_REVOKED     = 3,   /* digest on the revocation list */
    CI_REASON_UNVERIFIED  = 4,   /* no signature / catalog proof (validation pending) */
    CI_REASON_BAD_REQUEST = 5,   /* NULL/zero-size/oversized image, or missing data */
    CI_REASON_WX          = 6,   /* dynamic code: writable+executable refused */
    CI_REASON_MAX
} ci_deny_reason_t;

/* One admission request. The caller fills the identity fields it knows; the
 * validator ALWAYS recomputes `hash` over [data, data+size) -- a caller-supplied
 * digest is never trusted as the admission input. `signer` and measured-boot
 * context are reserved (zeroed) until embedded-signature parsing / the TPM PCR
 * measured-boot binding land. */
typedef struct ci_image_info {
    const char     *path;               /* canonical path, or NULL for a raw buffer */
    ci_image_type_t type;               /* user / driver / kernel / dynamic */
    ci_loader_id_t  loader;             /* originating loader */
    const uint8_t  *data;               /* image bytes */
    uint64_t        size;               /* image length in bytes */
    uint64_t        token;              /* requesting process token id (0 = system) */
    uint8_t         hash[CI_HASH_LEN];  /* OUT: digest the validator computed */
    uint8_t         signer[CI_HASH_LEN];/* reserved (embedded-signature id) -- zeroed */
    uint8_t         measured;           /* OUT: 1 if the digest was recorded for measurement */
} ci_image_info_t;

/* Decision returned by the validator. */
typedef struct ci_decision {
    ci_verdict_t     verdict;
    ci_deny_reason_t reason;
    uint32_t         enforcement;   /* ci_enforcement_t in effect at decision time */
} ci_decision_t;

/* Largest image the one-shot digest API can hash faithfully (u32 length). An
 * image beyond this is a bad request (fail closed), not a silent truncation. */
#define CI_IMAGE_MAX_BYTES  0xFFFFFFFFu

/* Create the \Callback\CiImageLoad notification object so consumers can register
 * before any producer fires. Idempotent; PASSIVE. NOT wired into boot yet --
 * lands with the ci_init policy seal (blocked on the ratchet-domain merge). */
void ci_image_init(void);

/* Validate an image for admission against the sealed CI policy. Recomputes the
 * digest over the image bytes, checks revocation (overrides any allow), then
 * maps the UNVERIFIED state to a verdict by enforcement mode: ENFORCE/SECUREBOOT
 * deny, AUDIT audit-allow, DISABLED allow. Fires \Callback\CiImageLoad with the
 * decision when the object exists. Fills *out. FAIL-CLOSED: NULL args, an
 * empty/oversized image, or an unsealed policy all deny. */
ci_verdict_t ci_validate_image(ci_image_info_t *img, ci_decision_t *out);

/* Validate a runtime code-generation / W->X request that has no backing image.
 * `writable_and_exec` (W+X) is refused outright (CI_REASON_WX). Otherwise the
 * request is UNVERIFIED and mapped to a verdict by enforcement mode, exactly as
 * ci_validate_image. `token` is reserved for the deferred token/provenance trust
 * model; there is no call site yet (wiring NtAllocateVirtualMemory /
 * NtProtectVirtualMemory is a deferred item). PASSIVE. */
ci_verdict_t ci_validate_dynamic_code(uint64_t token, bool writable_and_exec,
                                      ci_decision_t *out);

#endif /* KERNEL_CI_CI_IMAGE_H */
