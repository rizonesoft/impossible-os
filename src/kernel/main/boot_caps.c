/* ============================================================================
 * boot_caps.c -- capability negotiation validator.
 *
 * Runs in Phase 0 AFTER boot_payload_validate succeeds and BEFORE any
 * subsystem consumes a capability-gated field. Contract lives in
 * docs/boot/boot-info-fields.md under "Capability negotiation". Rejects:
 *   - caps_required bits outside BOOT_CAP_MASK_KNOWN (stale kernel on
 *     newer loader): unknown required bit means the loader asserts a
 *     feature this kernel cannot honor.
 *   - caps_required & caps_degraded != 0: "required but not provided"
 *     is a producer contradiction.
 *   - caps_present & caps_degraded != 0: same bit cannot be both
 *     populated and degraded.
 *   - any known bit left UNCLASSIFIED (neither present nor degraded):
 *     the contract requires the producer to account for every known
 *     capability by explicitly advertising it as either populated or
 *     skipped. Leaving a known bit clear in BOTH words lets a buggy
 *     or crafted loader suppress degraded reporting and push the
 *     kernel back to "infer from zeroed companion fields", which is
 *     the exact failure mode this ABI was introduced to prevent.
 *
 * Ignores unknown bits in caps_present / caps_degraded (forward
 * compatibility: older kernels boot against newer loaders that set
 * optional new bits). The classify + halt path feeds klog(LOG_ERROR)
 * with the offending bitmask delta so operators see the exact
 * mismatch, not a generic compatibility halt. Logs at LOG_ERROR --
 * the caller (boot_hw.c) converts the BOOT_FATAL return into the
 * actual halt via boot_halt(). Using LOG_FATAL here would trip the
 * klog fatal path (panic on sight) and prevent the validator from
 * being unit-testable with fixture inputs.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"

const char *boot_caps_bit_name(uint64_t cap_bit)
{
    switch (cap_bit) {
    case BOOT_CAP_PAYLOAD_DESCRIPTORS:  return "payload_descriptors";
    case BOOT_CAP_RUNTIME_SERVICES:     return "runtime_services";
    case BOOT_CAP_SECURE_BOOT_STATE:    return "secure_boot_state";
    case BOOT_CAP_TPM_EVENT_LOG:        return "tpm_event_log";
    case BOOT_CAP_USB_HANDOVER:         return "usb_handover";
    case BOOT_CAP_MEDIA_ROLE:           return "media_role";
    case BOOT_CAP_NETWORK_PROVENANCE:   return "network_provenance";
    case BOOT_CAP_RESUME_METADATA:      return "resume_metadata";
    case BOOT_CAP_ALT_PROTOCOL_ADAPTER: return "alt_protocol_adapter";
    default:                            return "reserved";
    }
}

/* Log every SET bit in `mask` as a space-separated list of names. For
 * bits outside BOOT_CAP_MASK_KNOWN emit "reserved(0xNN)" so operators
 * can correlate a forward-compat bit with its raw position without
 * needing the kernel header to spell it. Only used on the fatal
 * path; the per-bit string rendering is intentionally simple (no
 * dynamic allocation) at the cost of a few klog lines. */
static void log_bits(uint32_t severity, const char *label, uint64_t mask)
{
    uint32_t i;
    int first = 1;
    for (i = 0u; i < 64u; i++) {
        uint64_t bit = 1ull << i;
        if ((mask & bit) == 0u)
            continue;
        if (bit & BOOT_CAP_MASK_KNOWN) {
            klog(severity, "boot",
                 "  %s: %s (bit %u / 0x%llx)",
                 (uint64_t)(uintptr_t)label,
                 (uint64_t)(uintptr_t)boot_caps_bit_name(bit),
                 (uint64_t)i, (uint64_t)bit);
        } else {
            klog(severity, "boot",
                 "  %s: reserved bit %u / 0x%llx",
                 (uint64_t)(uintptr_t)label, (uint64_t)i, (uint64_t)bit);
        }
        first = 0;
    }
    if (first) {
        klog(severity, "boot",
             "  %s: (none)", (uint64_t)(uintptr_t)label);
    }
}

boot_result_t boot_caps_validate(const struct boot_info *info,
                                 enum boot_caps_error *out_error)
{
    if (out_error != (enum boot_caps_error *)0)
        *out_error = BOOT_CAPS_ERR_OK;

    if (info == (const struct boot_info *)0) {
        if (out_error != (enum boot_caps_error *)0)
            *out_error = BOOT_CAPS_ERR_NULL_INFO;
        return BOOT_FATAL;
    }

    uint64_t required = info->caps_required;
    uint64_t present  = info->caps_present;
    uint64_t degraded = info->caps_degraded;

    /* Rule 1: unknown required bits. This is the "stale kernel on
     * newer loader" gate. */
    uint64_t unknown_required = required & ~BOOT_CAP_MASK_KNOWN;
    if (unknown_required != 0u) {
        klog(LOG_ERROR, "boot",
             "boot_caps: unknown required bits 0x%llx (kernel known mask 0x%llx)",
             (uint64_t)unknown_required, (uint64_t)BOOT_CAP_MASK_KNOWN);
        log_bits(LOG_ERROR, "required_unknown", unknown_required);
        if (out_error != (enum boot_caps_error *)0)
            *out_error = BOOT_CAPS_ERR_UNKNOWN_REQUIRED;
        return BOOT_FATAL;
    }

    /* Rule 2: required & degraded contradiction. */
    uint64_t required_and_degraded = required & degraded;
    if (required_and_degraded != 0u) {
        klog(LOG_ERROR, "boot",
             "boot_caps: required bits also marked degraded 0x%llx "
             "(loader bug: asserted required but could not provide)",
             (uint64_t)required_and_degraded);
        log_bits(LOG_ERROR, "required_and_degraded", required_and_degraded);
        if (out_error != (enum boot_caps_error *)0)
            *out_error = BOOT_CAPS_ERR_REQUIRED_DEGRADED;
        return BOOT_FATAL;
    }

    /* Rule 3: present & degraded contradiction. */
    uint64_t present_and_degraded = present & degraded;
    if (present_and_degraded != 0u) {
        klog(LOG_ERROR, "boot",
             "boot_caps: bits set in BOTH caps_present and caps_degraded 0x%llx "
             "(contradictory: populated or skipped, not both)",
             (uint64_t)present_and_degraded);
        log_bits(LOG_ERROR, "present_and_degraded", present_and_degraded);
        if (out_error != (enum boot_caps_error *)0)
            *out_error = BOOT_CAPS_ERR_PRESENT_DEGRADED;
        return BOOT_FATAL;
    }

    /* Rule 4: every known bit must be classified (present OR degraded).
     * Leaving a known bit clear in both words is NOT forward-compat; it
     * is a producer under-specification that lets the kernel fall back
     * to inferring from zeroed companion fields. Unknown bits are NOT
     * checked (forward compat: the producer may know about capability
     * bits this kernel does not, and the "classify all known" rule is
     * evaluated against THIS kernel's BOOT_CAP_MASK_KNOWN). */
    uint64_t classified       = (present | degraded) & BOOT_CAP_MASK_KNOWN;
    uint64_t unclassified_known = BOOT_CAP_MASK_KNOWN & ~classified;
    if (unclassified_known != 0u) {
        klog(LOG_ERROR, "boot",
             "boot_caps: known capability bits 0x%llx are in NEITHER caps_present "
             "nor caps_degraded (producer must classify every known feature)",
             (uint64_t)unclassified_known);
        log_bits(LOG_ERROR, "unclassified_known", unclassified_known);
        if (out_error != (enum boot_caps_error *)0)
            *out_error = BOOT_CAPS_ERR_UNCLASSIFIED_KNOWN;
        return BOOT_FATAL;
    }

    /* Success path: log summary at LOG_INFO for observability.
     * Unknown-optional bits in caps_present / caps_degraded are
     * tolerated (forward compat) and shown so operators know the
     * loader used reserved slots. */
    if (degraded != 0u) {
        klog(LOG_INFO, "boot",
             "boot_caps: %u degraded capabilities, adapter=%s",
             (uint64_t)__builtin_popcountll(degraded),
             (uint64_t)(uintptr_t)((present & BOOT_CAP_ALT_PROTOCOL_ADAPTER)
                                   ? "alternate" : "native-UEFI"));
        log_bits(LOG_INFO, "degraded", degraded);
    }

    return BOOT_OK;
}
