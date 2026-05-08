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
/* boot_policy.h shares v19 ABI constants (BOOT_SELECTION_REASON_MAX,
 * BOOT_REJECT_REASON_MAX, sentinel enum values). It is freestanding
 * (no UEFI types) so kernel-side inclusion is safe; this avoids
 * hardcoded magic numbers in the v19 validator and gives compile-time
 * drift detection if the bootloader-side enum changes. */
#include "boot/boot_policy.h"

const char *boot_path_name(uint32_t path)
{
    switch (path) {
    case BOOT_PATH_UNSET:        return "unset";
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
    case BOOT_REASON_UNSET:               return "unset";
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
    case BOOT_REASON_MEDIA_ROLE_MARKER:   return "media_role_marker";
    default:                              return "invalid";
    }
}

const char *boot_media_role_name(uint32_t role)
{
    /* Lower-case canonical names matching the on-disk /IPOS/role.txt
     * source-of-truth content (boot-media role-detection feature).
     * Used by HKLM\SYSTEM\Boot\Device\MediaRole and serial output. */
    switch (role) {
    case BOOT_MEDIA_ROLE_UNSET:         return "unset";
    case BOOT_MEDIA_ROLE_NORMAL:        return "normal";
    case BOOT_MEDIA_ROLE_INSTALLER:     return "installer";
    case BOOT_MEDIA_ROLE_LIVE:          return "live";
    case BOOT_MEDIA_ROLE_RECOVERY:      return "recovery";
    case BOOT_MEDIA_ROLE_MANUFACTURING: return "manufacturing";
    case BOOT_MEDIA_ROLE_DIAGNOSTICS:   return "diagnostics";
    default:                            return "invalid";
    }
}

const char *boot_degraded_trust_bit_name(uint32_t bit)
{
    /* Lower-case ASCII names matching HKLM\SYSTEM\Boot\Trust\* value
     * names and serial output. Returns "unknown" for any value
     * outside BOOT_DEGRADED_TRUST_MASK_KNOWN; the validator and
     * registry populator iterate the closed mask, so callers should
     * never hit "unknown" in production. */
    switch (bit) {
    case BOOT_DEGRADED_TRUST_SECURE_BOOT_OFF:        return "secure-boot-off";
    case BOOT_DEGRADED_TRUST_SECURE_BOOT_UNREADABLE: return "secure-boot-unreadable";
    case BOOT_DEGRADED_TRUST_SETUP_MODE:             return "setup-mode";
    case BOOT_DEGRADED_TRUST_SBAT_ABSENT:            return "sbat-absent";
    case BOOT_DEGRADED_TRUST_DBX_ABSENT:             return "dbx-absent";
    default:                                         return "unknown";
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

    /* Rule 1: boot_path must be set (>= NORMAL) AND in enum range.
     * UNSET (value 0) is the BSS-zero sentinel: a producer that never
     * populated the field passes BSS through to the validator, which
     * MUST reject the phantom record instead of certifying it as a
     * normal cold boot. */
    if (path == (uint32_t)BOOT_PATH_UNSET || path > (uint32_t)BOOT_PATH_TYPE_MAX) {
        klog(LOG_ERROR, "boot",
             (path == (uint32_t)BOOT_PATH_UNSET)
                 ? "boot_decision: boot_path is UNSET (producer left BSS zero; populate before handoff)"
                 : "boot_decision: boot_path %u out of range (max %u); stale loader emitted unknown flow",
             (uint64_t)path, (uint64_t)BOOT_PATH_TYPE_MAX);
        if (out_error != (enum boot_decision_error *)0)
            *out_error = BOOT_DECISION_ERR_BAD_PATH;
        return BOOT_FATAL;
    }

    /* Rule 2: boot_reason must be set (>= NORMAL) AND in enum range. */
    if (reason == (uint32_t)BOOT_REASON_UNSET || reason > (uint32_t)BOOT_REASON_CODE_MAX) {
        klog(LOG_ERROR, "boot",
             (reason == (uint32_t)BOOT_REASON_UNSET)
                 ? "boot_decision: boot_reason is UNSET (producer left BSS zero; populate before handoff)"
                 : "boot_decision: boot_reason %u out of range (max %u); stale loader emitted unknown code",
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

    /* Rule 3.5: boot_media_role + mismatch range check. Always-on, runs
     * before the per-reason policy table so a stale producer cannot ship
     * boot_media_role > BOOT_MEDIA_ROLE_MAX or a non-boolean mismatch
     * value through to HKLM\SYSTEM\Boot\Device. The MEDIA_ROLE_MARKER
     * Rule 8 below adds the path/role consistency check on top of this
     * baseline range gate; non-marker boots still need the range gate
     * because boot_device_populate_registry() reads the field
     * unconditionally. */
    {
        uint32_t role = info->boot_media_role;
        uint8_t  mm   = info->boot_media_role_mismatch;
        if (role == (uint32_t)BOOT_MEDIA_ROLE_UNSET ||
            role > (uint32_t)BOOT_MEDIA_ROLE_MAX) {
            /* UNSET is the documented producer-must-overwrite sentinel:
             * a producer that left BSS zero (alternate firmware, partial
             * v17 wiring) must NOT pass through to consumers as if it
             * had certified the role. Same doctrine as Rules 1/2 for
             * boot_path/boot_reason. Out-of-range catches stale
             * producers writing values past MAX. */
            klog(LOG_ERROR, "boot",
                 (role == (uint32_t)BOOT_MEDIA_ROLE_UNSET)
                     ? "boot_decision: boot_media_role is UNSET (producer left BSS zero; populate before handoff)"
                     : "boot_decision: boot_media_role %u out of range (max %u); stale producer",
                 (uint64_t)role, (uint64_t)BOOT_MEDIA_ROLE_MAX);
            if (out_error != (enum boot_decision_error *)0)
                *out_error = BOOT_DECISION_ERR_BAD_REASON;
            return BOOT_FATAL;
        }
        if (mm > 1u) {
            klog(LOG_ERROR, "boot",
                 "boot_decision: boot_media_role_mismatch %u not boolean (0/1)",
                 (uint64_t)mm);
            if (out_error != (enum boot_decision_error *)0)
                *out_error = BOOT_DECISION_ERR_BAD_REASON;
            return BOOT_FATAL;
        }
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

    /* Rules 5-7: per-reason policy table. Each row pins (a) which
     * path is required for this reason (0 = any), (b) which paths are
     * forbidden as a bitmask, (c) whether the reason is in the
     * fallback class, and (d) which trigger flag the reason demands.
     *
     * Codex 2026-04-30 findings closed by going table-driven:
     *  - Rule 5/7 switch defaults silently let a future enum value
     *    bypass classification (M1 adversarial). The table is indexed
     *    by enum value with a _Static_assert pinning length to
     *    BOOT_REASON_CODE_MAX + 1, so a new reason added to the enum
     *    forces a new row or fails compilation.
     *  - Rule 7 was unidirectional (reason -> flag enforced;
     *    flag -> reason not). With the per-flag trigger set computed
     *    from the policy table, we now also check the inverse: any
     *    trigger flag set in source_flags MUST match the reason. */
    /* `classified` = 1 marks a row as deliberately filled. The length
     * _Static_assert below catches enum count changes, but length alone
     * does NOT detect a missing designated initializer for a value
     * inserted before MAX (the gap default-initializes to {0}, which
     * silently matches "path-agnostic, no fallback class, no trigger
     * flag"). The classified-flag is checked at validator entry: if any
     * reason in [NORMAL..MAX] has classified == 0, the validator halts.
     * That makes a forgotten row a runtime error on the first boot, not
     * a silent classification drift. */
    struct boot_reason_policy {
        uint8_t  classified;          /* 1 in every valid row; 0 = forgotten */
        uint32_t required_path;       /* 0 = any */
        uint32_t forbidden_paths_bm;  /* bitmask over enum boot_path_type */
        uint8_t  is_fallback_class;
        uint32_t trigger_flag;        /* 0 = no required flag */
    };
    static const struct boot_reason_policy reason_policy[BOOT_REASON_CODE_MAX + 1u] = {
        [BOOT_REASON_UNSET]               = { 0u, 0u, 0u, 0u, 0u },  /* sentinel; Rule 2 rejects */
        [BOOT_REASON_NORMAL]              = { 1u, 0u, 0u, 0u, 0u },
        [BOOT_REASON_USER_SELECTED]       = { 1u, 0u, 0u, 0u, 0u },
        [BOOT_REASON_ROLLBACK]            = { 1u, 0u, 0u, 1u, BOOT_SOURCE_FLAG_ROLLBACK_TRIGGERED },
        [BOOT_REASON_RESUME_VALIDATED]    = { 1u, BOOT_PATH_RESUME, 0u, 0u, 0u },
        [BOOT_REASON_RESUME_INVALIDATED]  = { 1u, 0u, (1u << BOOT_PATH_RESUME),
                                              1u, BOOT_SOURCE_FLAG_RESUME_INVALIDATED },
        [BOOT_REASON_NETWORK_INSECURE]    = { 1u, BOOT_PATH_NETWORK, 0u, 0u, BOOT_SOURCE_FLAG_NETWORK_INSECURE },
        [BOOT_REASON_MANIFEST_FAILURE]    = { 1u, 0u, (1u << BOOT_PATH_INSTALLER),
                                              1u, BOOT_SOURCE_FLAG_MANIFEST_FAILED },
        [BOOT_REASON_MEASURED_BOOT_FAIL]  = { 1u, 0u, 0u, 1u, BOOT_SOURCE_FLAG_MEASURED_BOOT_FAILED },
        [BOOT_REASON_RECOVERY_TRIGGER]    = { 1u, BOOT_PATH_RECOVERY, 0u, 0u, BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED },
        [BOOT_REASON_FAST_STARTUP_HIT]    = { 1u, BOOT_PATH_FAST_STARTUP, 0u, 0u, 0u },
        [BOOT_REASON_DIAGNOSTIC_REQUEST]  = { 1u, BOOT_PATH_DIAGNOSTIC, 0u, 0u, 0u },
        [BOOT_REASON_FALLBACK]            = { 1u, 0u, 0u, 1u, 0u },
        /* Media-role marker (boot-media role-detection feature, v17):
         * the bootloader read /IPOS/role.txt from ESP and BlackBox and
         * the marker selected the boot path. INSTALLER / RECOVERY /
         * DIAGNOSTIC are the only paths the bootloader will write
         * alongside this reason; required_path stays 0 (any) because
         * the per-path coupling logic in bootx64.c already pairs the
         * reason with one of those three paths, and asserting one
         * required_path here would block the others. forbidden mask
         * blocks paths the marker should never select (NORMAL,
         * NETWORK, RESUME, FAST_STARTUP). No fallback class, no
         * trigger flag (the marker itself is the trigger). */
        [BOOT_REASON_MEDIA_ROLE_MARKER]   = { 1u, 0u,
                                              ((1u << BOOT_PATH_NORMAL) |
                                               (1u << BOOT_PATH_NETWORK) |
                                               (1u << BOOT_PATH_RESUME) |
                                               (1u << BOOT_PATH_FAST_STARTUP)),
                                              0u, 0u },
    };
    _Static_assert(sizeof(reason_policy) / sizeof(reason_policy[0])
                   == (uint32_t)BOOT_REASON_CODE_MAX + 1u,
                   "reason_policy size must equal BOOT_REASON_CODE_MAX + 1");

    /* Exhaustiveness: every reason in [NORMAL..MAX] MUST have a row
     * with classified == 1. A new BOOT_REASON_X inserted before MAX
     * without a designated initializer hits this gate on first boot. */
    {
        uint32_t r;
        for (r = (uint32_t)BOOT_REASON_NORMAL;
             r <= (uint32_t)BOOT_REASON_CODE_MAX; r++) {
            if (reason_policy[r].classified == 0u) {
                klog(LOG_ERROR, "boot",
                     "boot_decision: reason_policy[%u] is unclassified -- "
                     "missing designated initializer (kernel bug, not a producer error)",
                     (uint64_t)r);
                if (out_error != (enum boot_decision_error *)0)
                    *out_error = BOOT_DECISION_ERR_BAD_REASON;
                return BOOT_FATAL;
            }
        }
    }

    const struct boot_reason_policy *pol = &reason_policy[reason];

    /* Rule 5a: required-path enforcement. */
    if (pol->required_path != 0u && path != pol->required_path) {
        klog(LOG_ERROR, "boot",
             "boot_decision: reason=%s requires path=%s but got path=%s",
             (uint64_t)(uintptr_t)boot_reason_name(reason),
             (uint64_t)(uintptr_t)boot_path_name(pol->required_path),
             (uint64_t)(uintptr_t)boot_path_name(path));
        if (out_error != (enum boot_decision_error *)0)
            *out_error = BOOT_DECISION_ERR_REASON_PATH;
        return BOOT_FATAL;
    }

    /* Rule 5b: forbidden-path enforcement. */
    if ((pol->forbidden_paths_bm & (1u << path)) != 0u) {
        klog(LOG_ERROR, "boot",
             "boot_decision: reason=%s forbids path=%s (fell-back reason cannot result in the originating path)",
             (uint64_t)(uintptr_t)boot_reason_name(reason),
             (uint64_t)(uintptr_t)boot_path_name(path));
        if (out_error != (enum boot_decision_error *)0)
            *out_error = BOOT_DECISION_ERR_REASON_PATH;
        return BOOT_FATAL;
    }

    /* Rule 6: fallback_depth > 0 requires a fallback-class reason. */
    if (depth > 0u && !pol->is_fallback_class) {
        klog(LOG_ERROR, "boot",
             "boot_decision: fallback_depth=%u requires fallback-class reason but got %s",
             (uint64_t)depth,
             (uint64_t)(uintptr_t)boot_reason_name(reason));
        if (out_error != (enum boot_decision_error *)0)
            *out_error = BOOT_DECISION_ERR_FALLBACK_REASON;
        return BOOT_FATAL;
    }

    /* Rule 7a (reason -> flag): every trigger reason demands its flag. */
    if (pol->trigger_flag != 0u && (flags & pol->trigger_flag) == 0u) {
        klog(LOG_ERROR, "boot",
             "boot_decision: reason=%s requires matching flag 0x%x but flags=0x%x",
             (uint64_t)(uintptr_t)boot_reason_name(reason),
             (uint64_t)pol->trigger_flag, (uint64_t)flags);
        if (out_error != (enum boot_decision_error *)0)
            *out_error = BOOT_DECISION_ERR_REASON_FLAG;
        return BOOT_FATAL;
    }

    /* Rule 7b (flag -> reason): every trigger flag set MUST match the
     * current reason. Computed from the policy table so a new reason
     * with a trigger flag automatically participates in this check. */
    {
        uint32_t trigger_mask = 0u;
        uint32_t i;
        for (i = 0u; i <= (uint32_t)BOOT_REASON_CODE_MAX; i++) {
            trigger_mask |= reason_policy[i].trigger_flag;
        }
        uint32_t set_triggers = flags & trigger_mask;
        if (set_triggers != 0u && set_triggers != pol->trigger_flag) {
            klog(LOG_ERROR, "boot",
                 "boot_decision: trigger flags 0x%x set but reason=%s only justifies 0x%x (provenance contradiction)",
                 (uint64_t)set_triggers,
                 (uint64_t)(uintptr_t)boot_reason_name(reason),
                 (uint64_t)pol->trigger_flag);
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

    /* Rule 8: MEDIA_ROLE_MARKER reason must be backed by a coherent
     * boot_media_role + non-mismatch flag. Without this gate, a stale or
     * corrupt producer can pass validation with reason=MEDIA_ROLE_MARKER
     * + path=INSTALLER while boot_media_role says NORMAL/UNSET/RECOVERY,
     * defeating the validator's purpose for media-role-aware consumers
     * (Registry, recovery flow, attestation). The path<->role pairs are
     * the only ones the bootloader writes alongside this reason:
     *   INSTALLER path  <-> INSTALLER role
     *   RECOVERY  path  <-> RECOVERY  role
     *   DIAGNOSTIC path <-> DIAGNOSTICS role
     * mismatch=1 means the bootloader observed disagreement between ESP
     * and BlackBox markers and fell back to NORMAL -- in that case the
     * bootloader will NOT have written reason=MEDIA_ROLE_MARKER, so
     * encountering reason=MARKER + mismatch=1 is contradictory state. */
    if (reason == (uint32_t)BOOT_REASON_MEDIA_ROLE_MARKER) {
        uint32_t role = info->boot_media_role;
        uint8_t mm   = info->boot_media_role_mismatch;
        if (role > (uint32_t)BOOT_MEDIA_ROLE_MAX || mm != 0u) {
            klog(LOG_ERROR, "boot",
                 "boot_decision: reason=media_role_marker but role=%u mismatch=%u (out of range or mismatched)",
                 (uint64_t)role, (uint64_t)mm);
            if (out_error != (enum boot_decision_error *)0)
                *out_error = BOOT_DECISION_ERR_REASON_PATH;
            return BOOT_FATAL;
        }
        uint32_t expected_path = 0u;
        switch (role) {
            case BOOT_MEDIA_ROLE_INSTALLER:   expected_path = (uint32_t)BOOT_PATH_INSTALLER;  break;
            case BOOT_MEDIA_ROLE_RECOVERY:    expected_path = (uint32_t)BOOT_PATH_RECOVERY;   break;
            case BOOT_MEDIA_ROLE_DIAGNOSTICS: expected_path = (uint32_t)BOOT_PATH_DIAGNOSTIC; break;
            default: break;  /* normal/live/manufacturing/UNSET: must NOT pair */
        }
        if (expected_path == 0u || path != expected_path) {
            klog(LOG_ERROR, "boot",
                 "boot_decision: reason=media_role_marker requires role/path pair "
                 "(installer/installer, recovery/recovery, diagnostics/diagnostic); got role=%u path=%u",
                 (uint64_t)role, (uint64_t)path);
            if (out_error != (enum boot_decision_error *)0)
                *out_error = BOOT_DECISION_ERR_REASON_PATH;
            return BOOT_FATAL;
        }
    }

    /* Trust-landscape closed-mask gate (v18). Per-bit advisory klog
     * fires from uefi_secureboot_init() at producer time (the field
     * is populated AFTER this validator runs, so a per-bit loop here
     * would be dead-code in current init order; the closed-mask
     * FATAL-on-unknown-bit gate IS still load-bearing for any future
     * producer that fills the field earlier in boot). The closed-
     * mask invariant matches BOOT_DEGRADED_TRUST_MASK_KNOWN: any bit
     * outside it is a producer bug. */
    {
        uint32_t tflags = info->degraded_trust_flags;
        if ((tflags & ~(uint32_t)BOOT_DEGRADED_TRUST_MASK_KNOWN) != 0u) {
            klog(LOG_ERROR, "boot",
                 "boot_decision: degraded_trust_flags 0x%x has bits outside MASK_KNOWN 0x%x",
                 (uint64_t)tflags,
                 (uint64_t)(uint32_t)BOOT_DEGRADED_TRUST_MASK_KNOWN);
            if (out_error != (enum boot_decision_error *)0)
                *out_error = BOOT_DECISION_ERR_UNKNOWN_FLAG;
            return BOOT_FATAL;
        }
    }

    /* Rule 9: v19 selection_reason range + UNSET-fatal. boot_policy_invoke()
     * (the live producer in bootx64.c) writes a non-UNSET selection_reason
     * on every code path -- including the AllocatePool-failure fallback path
     * which writes FALLBACK_STORE_INVALID directly. UNSET reaching the
     * validator means the producer was skipped or regressed. */
    {
        uint32_t sreason = info->selection_reason;
        if (sreason == (uint32_t)BOOT_SELECTION_UNSET ||
            sreason > (uint32_t)BOOT_SELECTION_REASON_MAX) {
            klog(LOG_ERROR, "boot",
                 (sreason == (uint32_t)BOOT_SELECTION_UNSET)
                     ? "boot_decision: selection_reason is UNSET (boot_policy_invoke skipped or producer regression)"
                     : "boot_decision: selection_reason %u out of range (max %u); stale loader",
                 (uint64_t)sreason, (uint64_t)BOOT_SELECTION_REASON_MAX);
            if (out_error != (enum boot_decision_error *)0)
                *out_error = BOOT_DECISION_ERR_BAD_SELECTION_REASON;
            return BOOT_FATAL;
        }
    }

    /* Rule 10: selected_entry_id NUL-terminated within 64 bytes. Empty
     * (id[0]==0) is the documented sentinel for FALLBACK_STORE_INVALID
     * per the v19 ABI; allowed only when reason == FALLBACK_STORE_INVALID.
     * For every other reason, id MUST be non-empty (the policy ladder
     * picked a concrete entry from the parsed store). */
    {
        const char *sid = info->selected_entry_id;
        unsigned int j;
        int has_nul = 0;
        for (j = 0; j < sizeof(info->selected_entry_id); j++) {
            if (sid[j] == 0) { has_nul = 1; break; }
        }
        if (!has_nul) {
            klog(LOG_ERROR, "boot",
                 "boot_decision: selected_entry_id not NUL-terminated"
                 " within 64 bytes (producer wrote raw bytes past the"
                 " cap)");
            if (out_error != (enum boot_decision_error *)0)
                *out_error = BOOT_DECISION_ERR_BAD_SELECTED_ID;
            return BOOT_FATAL;
        }
        /* Empty-id sentinel: only valid for STORE_INVALID. */
        uint32_t sreason = info->selection_reason;
        if (sid[0] == 0 && sreason != (uint32_t)BOOT_SELECTION_FALLBACK_STORE_INVALID) {
            klog(LOG_ERROR, "boot",
                 "boot_decision: selected_entry_id empty but "
                 "selection_reason=%u (empty allowed only for "
                 "FALLBACK_STORE_INVALID=%u)",
                 (uint64_t)sreason,
                 (uint64_t)BOOT_SELECTION_FALLBACK_STORE_INVALID);
            if (out_error != (enum boot_decision_error *)0)
                *out_error = BOOT_DECISION_ERR_BAD_SELECTED_ID;
            return BOOT_FATAL;
        }
    }

    /* Rule 11: rejected_entries integrity. count <= MAX_ENTRIES; overflow
     * boolean; for each populated entry, id NUL-terminated and reason in
     * (NONE, REJECT_REASON_MAX]. NONE is documented as "sentinel; never
     * written" so any rejected entry asserting NONE is a producer bug. */
    {
        uint32_t rcount = info->rejected_entry_count;
        uint32_t rover  = info->rejected_entry_overflow;
        if (rcount > (uint32_t)BOOT_ENTRIES_MAX_ENTRIES) {
            klog(LOG_ERROR, "boot",
                 "boot_decision: rejected_entry_count %u exceeds MAX_ENTRIES %u",
                 (uint64_t)rcount, (uint64_t)BOOT_ENTRIES_MAX_ENTRIES);
            if (out_error != (enum boot_decision_error *)0)
                *out_error = BOOT_DECISION_ERR_BAD_REJECTED_ENTRY;
            return BOOT_FATAL;
        }
        if (rover > 1u) {
            klog(LOG_ERROR, "boot",
                 "boot_decision: rejected_entry_overflow %u not boolean (0/1)",
                 (uint64_t)rover);
            if (out_error != (enum boot_decision_error *)0)
                *out_error = BOOT_DECISION_ERR_BAD_REJECTED_ENTRY;
            return BOOT_FATAL;
        }
        unsigned int ri;
        for (ri = 0; ri < rcount; ri++) {
            const char *rid = info->rejected_entries[ri].id;
            unsigned int rreason = info->rejected_entries[ri].reason;
            unsigned int j;
            int rhas_nul = 0;
            for (j = 0; j < sizeof(info->rejected_entries[ri].id); j++) {
                if (rid[j] == 0) { rhas_nul = 1; break; }
            }
            if (!rhas_nul) {
                klog(LOG_ERROR, "boot",
                     "boot_decision: rejected_entries[%u].id not NUL-terminated",
                     (uint64_t)ri);
                if (out_error != (enum boot_decision_error *)0)
                    *out_error = BOOT_DECISION_ERR_BAD_REJECTED_ENTRY;
                return BOOT_FATAL;
            }
            /* A populated rejected entry MUST name a real id. id[0]=0
             * (empty string is NUL-terminated) would slip past the
             * NUL-term check above but carries no diagnostic value --
             * the policy-audit consumer cannot identify the entry. */
            if (rid[0] == 0) {
                klog(LOG_ERROR, "boot",
                     "boot_decision: rejected_entries[%u].id is empty (count>0 requires non-empty id)",
                     (uint64_t)ri);
                if (out_error != (enum boot_decision_error *)0)
                    *out_error = BOOT_DECISION_ERR_BAD_REJECTED_ENTRY;
                return BOOT_FATAL;
            }
            /* NONE (0) is the documented "never written" sentinel; reject. */
            if (rreason < (unsigned int)BOOT_REJECT_REASON_KIND_SKIPPED ||
                rreason > (unsigned int)BOOT_REJECT_REASON_MAX) {
                klog(LOG_ERROR, "boot",
                     "boot_decision: rejected_entries[%u].reason %u "
                     "out of range [%u..%u] (NONE is sentinel; never "
                     "written)",
                     (uint64_t)ri, (uint64_t)rreason,
                     (uint64_t)BOOT_REJECT_REASON_KIND_SKIPPED,
                     (uint64_t)BOOT_REJECT_REASON_MAX);
                if (out_error != (enum boot_decision_error *)0)
                    *out_error = BOOT_DECISION_ERR_BAD_REJECTED_ENTRY;
                return BOOT_FATAL;
            }
        }
    }

    return BOOT_OK;
}
