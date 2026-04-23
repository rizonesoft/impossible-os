/* ============================================================================
 * boot_decision.c -- boot-path provenance and decision record validator.
 *
 * Runs in Phase 0 AFTER boot_caps_validate succeeds and BEFORE any
 * consumer (Registry populator, BlackBox transcription, recovery
 * orchestrator, attestation narrative builder) reads the decision
 * fields. Contract lives in docs/boot/boot-info-fields.md under
 * "Boot-path provenance". Rejects:
 *   - NULL info pointer (defense in depth; Phase 0 already validated
 *     the handoff pointer, but a unit test can pass NULL deliberately).
 *   - boot_path outside enum boot_path_type (stale producer emitted a
 *     value this kernel does not understand).
 *   - boot_reason outside enum boot_reason_code (same).
 *   - boot_source_flags with bits outside BOOT_SOURCE_FLAG_MASK_KNOWN
 *     (producer set a reason-code bit this kernel cannot interpret).
 *   - boot_fallback_depth greater than BOOT_FALLBACK_DEPTH_MAX
 *     (producer ran away in a fallback loop; likely storage detection
 *     bug).
 *   - reason/path combinations that cannot both be true (R5): a
 *     "resume validated" reason with a non-RESUME path, a
 *     "recovery triggered" reason with a non-RECOVERY path, etc.
 *   - fallback_depth>0 with a reason that does not describe a fallback
 *     (R6): only FALLBACK / ROLLBACK / RESUME_INVALIDATED /
 *     MANIFEST_FAILURE / MEASURED_BOOT_FAIL explain a depth>0 walk.
 *   - trigger-reasons without the matching flag bit (R7): a producer
 *     reporting reason=NETWORK_INSECURE without the
 *     NETWORK_INSECURE flag has an internal contradiction the kernel
 *     refuses to let consumers trust.
 *
 * Logs at LOG_ERROR; caller (boot_hw.c) converts the BOOT_FATAL return
 * into the actual halt via boot_halt(). Using LOG_FATAL here would
 * trip the klog fatal path (panic on sight) and prevent the validator
 * from being unit-testable with fixture inputs -- same reason as
 * boot_caps.c.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"

const char *boot_path_name(uint32_t path)
{
    switch (path) {
    case BOOT_PATH_NORMAL:       return "normal";
    case BOOT_PATH_INSTALLER:    return "installer";
    case BOOT_PATH_RECOVERY:     return "recovery";
    case BOOT_PATH_NETWORK:      return "network";
    case BOOT_PATH_RESUME:       return "resume";
    case BOOT_PATH_FAST_STARTUP: return "fast_startup";
    case BOOT_PATH_DIAGNOSTIC:   return "diagnostic";
    default:                     return "invalid";
    }
}

const char *boot_reason_name(uint32_t reason)
{
    switch (reason) {
    case BOOT_REASON_NORMAL:              return "normal";
    case BOOT_REASON_USER_SELECTED:       return "user_selected";
    case BOOT_REASON_ROLLBACK:            return "rollback";
    case BOOT_REASON_RESUME_VALIDATED:    return "resume_validated";
    case BOOT_REASON_RESUME_INVALIDATED:  return "resume_invalidated";
    case BOOT_REASON_NETWORK_INSECURE:    return "network_insecure";
    case BOOT_REASON_MANIFEST_FAILURE:    return "manifest_failure";
    case BOOT_REASON_MEASURED_BOOT_FAIL:  return "measured_boot_fail";
    case BOOT_REASON_RECOVERY_TRIGGER:    return "recovery_trigger";
    case BOOT_REASON_FAST_STARTUP_HIT:    return "fast_startup_hit";
    case BOOT_REASON_DIAGNOSTIC_REQUEST:  return "diagnostic_request";
    case BOOT_REASON_FALLBACK:            return "fallback";
    default:                              return "invalid";
    }
}

boot_result_t boot_decision_validate(const struct boot_info *info,
                                     enum boot_decision_error *out_error)
{
    if (out_error != (enum boot_decision_error *)0)
        *out_error = BOOT_DECISION_ERR_OK;

    if (info == (const struct boot_info *)0) {
        if (out_error != (enum boot_decision_error *)0)
            *out_error = BOOT_DECISION_ERR_NULL_INFO;
        return BOOT_FATAL;
    }

    uint32_t path     = info->boot_path;
    uint32_t reason   = info->boot_reason;
    uint32_t flags    = info->boot_source_flags;
    uint32_t depth    = info->boot_fallback_depth;

    /* Rule 1: boot_path must be in enum range. */
    if (path > (uint32_t)BOOT_PATH_TYPE_MAX) {
        klog(LOG_ERROR, "boot",
             "boot_decision: boot_path %u out of range (max %u); stale loader emitted unknown flow",
             (uint64_t)path, (uint64_t)BOOT_PATH_TYPE_MAX);
        if (out_error != (enum boot_decision_error *)0)
            *out_error = BOOT_DECISION_ERR_BAD_PATH;
        return BOOT_FATAL;
    }

    /* Rule 2: boot_reason must be in enum range. */
    if (reason > (uint32_t)BOOT_REASON_CODE_MAX) {
        klog(LOG_ERROR, "boot",
             "boot_decision: boot_reason %u out of range (max %u); stale loader emitted unknown code",
             (uint64_t)reason, (uint64_t)BOOT_REASON_CODE_MAX);
        if (out_error != (enum boot_decision_error *)0)
            *out_error = BOOT_DECISION_ERR_BAD_REASON;
        return BOOT_FATAL;
    }

    /* Rule 3: boot_source_flags outside MASK_KNOWN is a producer bug.
     * Unlike capability negotiation, we do NOT tolerate unknown bits
     * here -- every decision input maps to a documented policy, and a
     * producer asserting a brand-new flag would need a matching
     * kernel-side interpreter. Widening the mask requires a version
     * bump. */
    uint32_t unknown_flags = flags & ~BOOT_SOURCE_FLAG_MASK_KNOWN;
    if (unknown_flags != 0u) {
        klog(LOG_ERROR, "boot",
             "boot_decision: boot_source_flags 0x%x has unknown bits 0x%x (known mask 0x%x)",
             (uint64_t)flags, (uint64_t)unknown_flags,
             (uint64_t)BOOT_SOURCE_FLAG_MASK_KNOWN);
        if (out_error != (enum boot_decision_error *)0)
            *out_error = BOOT_DECISION_ERR_UNKNOWN_FLAG;
        return BOOT_FATAL;
    }

    /* Rule 4: boot_fallback_depth cap. Runaway fallback depths suggest
     * a storage-detection loop the loader should have broken; the
     * kernel refuses to interpret the decision until the producer
     * explains why it traversed that many fallbacks. */
    if (depth > (uint32_t)BOOT_FALLBACK_DEPTH_MAX) {
        klog(LOG_ERROR, "boot",
             "boot_decision: boot_fallback_depth %u exceeds cap %u (probable fallback loop)",
             (uint64_t)depth, (uint64_t)BOOT_FALLBACK_DEPTH_MAX);
        if (out_error != (enum boot_decision_error *)0)
            *out_error = BOOT_DECISION_ERR_FALLBACK_OOR;
        return BOOT_FATAL;
    }

    /* Rule 5: reason -> path compatibility. Certain reasons only make
     * sense with their matching path (RESUME_VALIDATED implies the
     * resume image was accepted and we're running RESUME; NETWORK_
     * INSECURE means the boot was a network boot with an insecure
     * channel); certain reasons are "we fell BACK from X" and
     * therefore forbid the original X as the result path
     * (RESUME_INVALIDATED forbids path=RESUME; MANIFEST_FAILURE
     * forbids path=INSTALLER since the failed manifest was the one
     * trying to install). The rest (NORMAL, USER_SELECTED, ROLLBACK,
     * FALLBACK, MEASURED_BOOT_FAIL) are path-agnostic. */
    {
        uint32_t forbidden_paths = 0u;
        uint32_t required_path   = 0xFFFFFFFFu;  /* 0xFFFFFFFF means "any" */
        switch (reason) {
        case BOOT_REASON_RESUME_VALIDATED:    required_path = BOOT_PATH_RESUME;       break;
        case BOOT_REASON_NETWORK_INSECURE:    required_path = BOOT_PATH_NETWORK;      break;
        case BOOT_REASON_RECOVERY_TRIGGER:    required_path = BOOT_PATH_RECOVERY;     break;
        case BOOT_REASON_FAST_STARTUP_HIT:    required_path = BOOT_PATH_FAST_STARTUP; break;
        case BOOT_REASON_DIAGNOSTIC_REQUEST:  required_path = BOOT_PATH_DIAGNOSTIC;   break;
        case BOOT_REASON_RESUME_INVALIDATED:  forbidden_paths = (1u << BOOT_PATH_RESUME);    break;
        case BOOT_REASON_MANIFEST_FAILURE:    forbidden_paths = (1u << BOOT_PATH_INSTALLER); break;
        default:                              /* path-agnostic */                            break;
        }
        if (required_path != 0xFFFFFFFFu && path != required_path) {
            klog(LOG_ERROR, "boot",
                 "boot_decision: reason=%s requires path=%s but got path=%s",
                 (uint64_t)(uintptr_t)boot_reason_name(reason),
                 (uint64_t)(uintptr_t)boot_path_name(required_path),
                 (uint64_t)(uintptr_t)boot_path_name(path));
            if (out_error != (enum boot_decision_error *)0)
                *out_error = BOOT_DECISION_ERR_REASON_PATH;
            return BOOT_FATAL;
        }
        if ((forbidden_paths & (1u << path)) != 0u) {
            klog(LOG_ERROR, "boot",
                 "boot_decision: reason=%s forbids path=%s (fell-back reason cannot result in the originating path)",
                 (uint64_t)(uintptr_t)boot_reason_name(reason),
                 (uint64_t)(uintptr_t)boot_path_name(path));
            if (out_error != (enum boot_decision_error *)0)
                *out_error = BOOT_DECISION_ERR_REASON_PATH;
            return BOOT_FATAL;
        }
    }

    /* Rule 6: fallback_depth > 0 requires a fallback-class reason.
     * Primary-path reasons (NORMAL, USER_SELECTED, RESUME_VALIDATED,
     * NETWORK_INSECURE, RECOVERY_TRIGGER, FAST_STARTUP_HIT,
     * DIAGNOSTIC_REQUEST) imply the loader's first choice took;
     * depth>0 would contradict that. */
    if (depth > 0u) {
        int fallback_class =
            (reason == BOOT_REASON_FALLBACK)            ||
            (reason == BOOT_REASON_ROLLBACK)            ||
            (reason == BOOT_REASON_RESUME_INVALIDATED)  ||
            (reason == BOOT_REASON_MANIFEST_FAILURE)    ||
            (reason == BOOT_REASON_MEASURED_BOOT_FAIL);
        if (!fallback_class) {
            klog(LOG_ERROR, "boot",
                 "boot_decision: fallback_depth=%u requires fallback-class reason but got %s",
                 (uint64_t)depth,
                 (uint64_t)(uintptr_t)boot_reason_name(reason));
            if (out_error != (enum boot_decision_error *)0)
                *out_error = BOOT_DECISION_ERR_FALLBACK_REASON;
            return BOOT_FATAL;
        }
    }

    /* Rule 7: trigger-reasons require the matching flag bit. If the
     * loader reports reason=NETWORK_INSECURE, consumers expect the
     * BOOT_SOURCE_FLAG_NETWORK_INSECURE bit to also be set (the flag
     * is the audit record of what input drove the decision; the
     * reason is the policy outcome). A reason without its flag
     * indicates producer inconsistency the validator refuses to paper
     * over. */
    {
        uint32_t required_flag = 0u;
        switch (reason) {
        case BOOT_REASON_RESUME_INVALIDATED:  required_flag = BOOT_SOURCE_FLAG_RESUME_INVALIDATED;     break;
        case BOOT_REASON_NETWORK_INSECURE:    required_flag = BOOT_SOURCE_FLAG_NETWORK_INSECURE;       break;
        case BOOT_REASON_MANIFEST_FAILURE:    required_flag = BOOT_SOURCE_FLAG_MANIFEST_FAILED;        break;
        case BOOT_REASON_MEASURED_BOOT_FAIL:  required_flag = BOOT_SOURCE_FLAG_MEASURED_BOOT_FAILED;   break;
        case BOOT_REASON_RECOVERY_TRIGGER:    required_flag = BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED;     break;
        case BOOT_REASON_ROLLBACK:            required_flag = BOOT_SOURCE_FLAG_ROLLBACK_TRIGGERED;     break;
        default:                              /* no flag required */                                   break;
        }
        if (required_flag != 0u && (flags & required_flag) == 0u) {
            klog(LOG_ERROR, "boot",
                 "boot_decision: reason=%s requires matching flag 0x%x but flags=0x%x",
                 (uint64_t)(uintptr_t)boot_reason_name(reason),
                 (uint64_t)required_flag, (uint64_t)flags);
            if (out_error != (enum boot_decision_error *)0)
                *out_error = BOOT_DECISION_ERR_REASON_FLAG;
            return BOOT_FATAL;
        }
    }

    /* Success: log the decision at INFO so the boot log carries a
     * one-line trace of "what path, what reason, how many fallbacks". */
    klog(LOG_INFO, "boot",
         "boot_decision: path=%s reason=%s flags=0x%x fallback=%u",
         (uint64_t)(uintptr_t)boot_path_name(path),
         (uint64_t)(uintptr_t)boot_reason_name(reason),
         (uint64_t)flags, (uint64_t)depth);

    return BOOT_OK;
}
