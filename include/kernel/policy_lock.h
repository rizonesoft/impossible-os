/* ============================================================================
 * policy_lock.h -- Policy Lock Phases and Tamper Audit
 *
 * Makes security policy immutable past a defined lock point and makes every
 * blocked mutation observable. A small fixed-capacity registry of named
 * security policies ("policy.<name>") is sealed phase-by-phase as the boot
 * advances; once sealed, a policy can only be written when its value moves
 * strictly toward MORE restriction (a ratchet).
 *
 * RATCHET ENCODING INVARIANT (single source of comparator correctness):
 *   Every ratchet/hard-class policy MUST encode a HIGHER numeric value as MORE
 *   restrictive. Then "downgrade" is exactly `new < old` for every policy and
 *   there is no per-policy comparator direction to get wrong. Debugger and
 *   boot-verifier policies are therefore stored as "lockout"/"enforced" senses
 *   (0 = permissive, 1 = restricted), never as "enabled".
 *
 * Panic policy: a post-seal downgrade of a Secure Boot or Code Integrity policy
 * is a measured contradiction of a trusted invariant ONLY when it originates
 * KernelMode (internal). A KernelMode SB/CI downgrade after seal calls
 * KeBugCheckEx. A UserMode (untrusted) attempt NEVER bugchecks -- it returns
 * STATUS_ACCESS_DENIED, sets the sticky tamper flag, and is audited. This
 * prevents an unprivileged caller from converting a denied write into a
 * bare-metal denial of service.
 *
 * Locking: kernel_policy_set() makes the entire seal/ratchet/old-value decision
 * UNDER the registry spinlock -- the same lock that advances the phase -- so a
 * phase advance on another CPU cannot race a stale pre-seal decision. The
 * lockless getters below are for status/hot read and logging/panic paths ONLY;
 * they are never consulted for authorization. Lockdown level and the current
 * lock phase are published as lockless monotonic scalars (updated under the
 * registry spinlock).
 *
 * Notification-facility fanout of tamper events is owned by the kernel
 * notification facility and not wired here yet (-> XREF: 02-kernel-core/TODO-16).
 * ============================================================================ */
#ifndef KERNEL_POLICY_LOCK_H
#define KERNEL_POLICY_LOCK_H

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"

#define POLICY_NAME_CAP    40u   /* incl. NUL; names start with "policy." */
#define POLICY_MAX         32u   /* registry capacity */
#define POLICY_AUDIT_RING  64u   /* tamper-audit ring depth */

/* Monotonic policy lock phases. Each policy seals at exactly one of these; the
 * global phase only ever advances. */
typedef enum {
    POLICY_PHASE_PRE_MEMORY         = 0,  /* earliest: before memory manager */
    POLICY_PHASE_POST_SECURITY_INIT = 1,  /* after the security reference monitor */
    POLICY_PHASE_POST_REGISTRY      = 2,  /* after registry-sourced policy merge */
    POLICY_PHASE_POST_USER_MODE     = 3,  /* after first user-mode transition */
} policy_lock_phase_t;

/* Lockdown level (Linux lockdown= parity). Higher = more restrictive. */
typedef enum {
    KERNEL_LOCKDOWN_NONE            = 0,
    KERNEL_LOCKDOWN_INTEGRITY       = 1,
    KERNEL_LOCKDOWN_CONFIDENTIALITY = 2,
} kernel_lockdown_level_t;

/* Policy class flags (bitmask). */
#define POLICY_CLASS_RATCHET        0x01u  /* post-seal: writes allowed only strictly more restrictive */
#define POLICY_CLASS_SECURE_BOOT    0x02u  /* KernelMode downgrade after seal -> KeBugCheckEx */
#define POLICY_CLASS_CODE_INTEGRITY 0x04u  /* KernelMode downgrade after seal -> KeBugCheckEx */

/* Caller provenance for a set attempt. */
typedef enum {
    POLICY_CALLER_KERNEL = 0,  /* trusted internal */
    POLICY_CALLER_USER   = 1,  /* untrusted; never bugchecks */
} policy_caller_mode_t;

/* Result of a set attempt, recorded in the tamper-audit ring. */
typedef enum {
    POLICY_RESULT_ALLOWED       = 0,  /* applied (or no-op equal write) */
    POLICY_RESULT_DENIED_LOCKED = 1,  /* sealed, non-ratchet, value differs */
    POLICY_RESULT_DENIED_RATCHET= 2,  /* sealed ratchet, attempted downgrade */
    POLICY_RESULT_PANIC         = 3,  /* KernelMode SB/CI downgrade -> bugcheck */
} policy_result_t;

/* Register a policy. name MUST start with "policy.". Higher value = more
 * restrictive for ratchet/hard classes (see RATCHET ENCODING INVARIANT).
 * max_value bounds the policy's value domain: an initial value or any later set
 * above max_value is rejected, which also keeps narrowing publications (e.g.
 * the uint8 lockdown scalar) within range and stops an out-of-domain ratchet
 * write from poisoning the row. Returns STATUS_SUCCESS,
 * STATUS_OBJECT_NAME_COLLISION on duplicate, STATUS_INSUFFICIENT_RESOURCES when
 * full, STATUS_INVALID_PARAMETER on a bad name/namespace/phase or value >
 * max_value. */
NTSTATUS kernel_policy_register(const char *name, uint64_t value,
                                uint64_t max_value,
                                policy_lock_phase_t seal_phase,
                                uint16_t class_flags, uint8_t owner);

/* Set a policy value. Returns STATUS_SUCCESS, STATUS_ACCESS_DENIED (blocked by
 * seal/ratchet), or STATUS_INVALID_PARAMETER (unknown name or value above the
 * policy's max_value domain). A KernelMode downgrade of a
 * SECURE_BOOT/CODE_INTEGRITY policy after its seal phase calls KeBugCheckEx and
 * does NOT return. The seal decision is made under the registry lock; see the
 * Locking note above. */
NTSTATUS kernel_policy_set(const char *name, uint64_t value,
                           policy_caller_mode_t caller);

/* Read a policy value. Returns 0 and writes *out on success, -1 if unknown. */
int kernel_policy_get(const char *name, uint64_t *out_value);

/* Lockless reads of the published monotonic scalars. Status/hot/log paths
 * only -- never used for authorization in kernel_policy_set(). */
policy_lock_phase_t     kernel_policy_lock_phase_get(void);
kernel_lockdown_level_t kernel_lockdown_level_get(void);

/* Advance the global lock phase. Forward-only: a non-forward `to` is ignored
 * (logged). Updates the published scalar under the registry lock. */
void kernel_policy_lock_phase_advance(policy_lock_phase_t to);

/* Audit / dump accessors. */
uint32_t kernel_policy_count(void);          /* registered policies */
uint64_t kernel_policy_tamper_count(void);   /* total denied + panic attempts */
int      kernel_policy_tamper_sticky(void);  /* 1 if any tamper blocked since boot */

/* Tamper-audit ring record (oldest dropped on wrap). Only BLOCKED attempts
 * (denied/panic) are recorded, so a stream of allowed writes can never evict
 * tamper evidence; allowed state changes are observable via ETW, not the ring. */
typedef struct {
    char     policy[POLICY_NAME_CAP];
    uint64_t attempted;     /* attempted value */
    uint8_t  caller;        /* policy_caller_mode_t */
    uint8_t  phase;         /* policy_lock_phase_t at the attempt */
    uint8_t  result;        /* policy_result_t */
    uint8_t  _pad;
} policy_audit_record_t;

/* Copy up to max newest-first audit records into out; returns the count. */
uint32_t kernel_policy_audit_dump(policy_audit_record_t *out, uint32_t max);

/* Register the built-in core security policies and seed the lockdown level from
 * the Secure Boot state. Idempotent; call once at Phase 3. Returns 0 when every
 * core policy registered, -1 if any failed (registry full / collision) -- the
 * caller MUST NOT advance/seal the policy phases on a failure, since required
 * rows (e.g. policy.ci.mode, policy.secure_boot) could be absent and the
 * documented post-lock ACCESS_DENIED/panic behavior would silently degrade. */
int kernel_policy_register_core(void);

#endif /* KERNEL_POLICY_LOCK_H */
