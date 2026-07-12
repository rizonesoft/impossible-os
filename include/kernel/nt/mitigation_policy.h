/* mitigation_policy.h -- NT ProcessMitigationPolicy ABI (Win32
 * SetProcessMitigationPolicy parity) and the internal per-task mitigation
 * flags.
 *
 * The ring-3 NtSet/NtQueryInformationProcess(ProcessMitigationPolicy) handlers
 * are currently DEFERRED (both return STATUS_NOT_SUPPORTED, copying nothing):
 * reading/writing a range-only-probed ring-3 buffer via the non-fault-
 * recoverable copy_from_user/copy_to_user is an unprivileged kernel-crash /
 * corruption path (the systemic usercopy gap). These ABI definitions serve the
 * LIVE kernel-internal path -- the MIT_NO_CHILD_PROCESS enforcement set via
 * task_mitigation_apply -- and the future ring-3 API once fault-recoverable
 * usercopy exists. Design rule for when the ring-3 API re-enters: only a bit
 * with live enforcement is accepted; reporting a dormant, unenforced
 * mitigation as active would be false security.
 */
#ifndef KERNEL_NT_MITIGATION_POLICY_H
#define KERNEL_NT_MITIGATION_POLICY_H

#include "kernel/types.h"

/* PROCESS_MITIGATION_POLICY selector values (Win32 enum). Named so the ABI is
 * complete for the deferred ring-3 handlers and the future implementation;
 * ProcessChildProcessPolicy is the one the enforcement path backs today. */
#define ProcessDEPPolicy                      0
#define ProcessASLRPolicy                     1
#define ProcessDynamicCodePolicy              2
#define ProcessStrictHandleCheckPolicy        3
#define ProcessSystemCallDisablePolicy        4
#define ProcessMitigationOptionsMask          5
#define ProcessExtensionPointDisablePolicy    6
#define ProcessControlFlowGuardPolicy         7
#define ProcessSignaturePolicy                8
#define ProcessFontDisablePolicy              9
#define ProcessImageLoadPolicy                10
#define ProcessSystemCallFilterPolicyClass    11
#define ProcessPayloadRestrictionPolicy       12
#define ProcessChildProcessPolicy             13
#define ProcessSideChannelIsolationPolicy     14
#define ProcessUserShadowStackPolicy          15

/* PROCESS_MITIGATION_CHILD_PROCESS_POLICY.Flags bits (Win32). */
#define PROC_MIT_CHILD_NO_CHILD_CREATION      (1u << 0)  /* NoChildProcessCreation */
#define PROC_MIT_CHILD_AUDIT_NO_CHILD         (1u << 1)  /* AuditNoChildProcessCreation */
#define PROC_MIT_CHILD_ALLOW_SECURE_CREATION  (1u << 2)  /* AllowSecureProcessCreation */

/* NT buffer for NtSet/NtQueryInformationProcess(ProcessMitigationPolicy):
 * PROCESS_MITIGATION_POLICY_INFORMATION -- the policy selector followed by the
 * policy-specific data union (collapsed here to one 32-bit Flags word, which
 * is exactly the size of every PROCESS_MITIGATION_*_POLICY struct). A caller
 * MUST pass sizeof(PROCESS_MITIGATION_POLICY_INFORMATION) exactly: a single
 * OR-mask-of-everything buffer cannot express a per-policy request. */
typedef struct _PROCESS_MITIGATION_POLICY_INFORMATION {
    uint32_t Policy;   /* PROCESS_MITIGATION_POLICY selector */
    uint32_t Flags;    /* policy-specific flags (union collapsed to one u32) */
} PROCESS_MITIGATION_POLICY_INFORMATION;

_Static_assert(sizeof(PROCESS_MITIGATION_POLICY_INFORMATION) == 8,
               "PROCESS_MITIGATION_POLICY_INFORMATION must be 8 bytes (selector + u32)");

/* Internal per-task mitigation_flags bits (struct task.mitigation_flags).
 * Only NO_CHILD_PROCESS ships now; the bit position is stable so a future flag
 * lands at its own bit without renumbering. Set monotonically via
 * __atomic_fetch_or (never cleared). */
#define MIT_NO_CHILD_PROCESS   (1ull << 3)  /* block NtCreateProcess / task_fork */

#endif /* KERNEL_NT_MITIGATION_POLICY_H */
