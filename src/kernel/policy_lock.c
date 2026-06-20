/* ============================================================================
 * policy_lock.c -- Policy Lock Phases and Tamper Audit
 *
 * See include/kernel/policy_lock.h for the contract, the ratchet-encoding
 * invariant, the panic policy, and the locking discipline. SMP-safe via one
 * irqsave spinlock guarding registry mutation, the audit ring, and the
 * published monotonic scalars; the scalar reads are lockless.
 * ============================================================================ */
#include "kernel/policy_lock.h"
#include "kernel/boot_info.h"
#include "kernel/etw.h"
#include "kernel/bugcheck.h"
#include "kernel/klog.h"
#include "kernel/sched/spinlock.h"
#include "libc/string.h"

extern struct boot_info g_boot_info;

typedef struct {
    char     name[POLICY_NAME_CAP];
    uint64_t value;
    uint64_t max_value;     /* value domain: writes above this are rejected */
    uint8_t  seal_phase;    /* policy_lock_phase_t */
    uint16_t class_flags;
    uint8_t  owner;
    uint8_t  used;
} policy_entry_t;

static policy_entry_t s_policies[POLICY_MAX];
static uint32_t       s_count;
static DEFINE_SPINLOCK(s_lock);

/* Published monotonic scalars -- updated under s_lock, read lockless. */
static volatile uint8_t s_phase    = (uint8_t)POLICY_PHASE_PRE_MEMORY;
static volatile uint8_t s_lockdown = (uint8_t)KERNEL_LOCKDOWN_NONE;

/* Index of the "policy.lockdown" row once registered, else -1. Keeps the
 * lockless s_lockdown scalar in sync whenever that row's value changes. */
static int s_lockdown_idx = -1;

/* Tamper-audit ring (newest overwrites oldest on wrap). */
static policy_audit_record_t s_ring[POLICY_AUDIT_RING];
static uint32_t s_ring_head;     /* next write slot */
static uint32_t s_ring_count;    /* valid records, capped at POLICY_AUDIT_RING */
static uint64_t s_tamper_count;  /* total denied + panic attempts */
static volatile int s_tamper_sticky;

/* ---- bounded string helpers (no libc strcmp; never read past NUL) -------- */

static uint32_t name_len(const char *s)
{
    uint32_t n = 0;
    /* Bound check BEFORE the dereference so an unterminated POLICY_NAME_CAP-byte
     * buffer never reads byte [POLICY_NAME_CAP]. */
    while (n < POLICY_NAME_CAP && s[n]) n++;
    return n;
}

static int names_eq(const char *a, const char *b)
{
    uint32_t i = 0;
    for (; i < POLICY_NAME_CAP; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0)    return 1;
    }
    return 1;  /* both filled the cap without a NUL -> treat as equal */
}

/* 1 if name is "policy.<x>" with at least one char after the prefix. */
static int name_in_namespace(const char *name, uint32_t nl)
{
    static const char p[] = "policy.";   /* len 7 */
    if (nl <= 7u) return 0;
    uint32_t i = 0;
    for (; i < 7u; i++) if (name[i] != p[i]) return 0;
    return 1;
}

/* Short name (after "policy.") for log lines; falls back to full name. */
static const char *short_name(const char *name)
{
    uint32_t nl = name_len(name);
    return (nl > 7u) ? (name + 7) : name;
}

/* Caller holds s_lock. -1 if not found. */
static int find_index(const char *name)
{
    for (uint32_t i = 0; i < s_count; i++)
        if (s_policies[i].used && names_eq(s_policies[i].name, name))
            return (int)i;
    return -1;
}

/* Caller holds s_lock. Append one audit record to the ring. */
static void audit_append(const char *name, uint64_t attempted, uint8_t caller,
                         uint8_t phase, uint8_t result)
{
    policy_audit_record_t *r = &s_ring[s_ring_head];
    uint32_t i = 0;
    for (; i + 1u < POLICY_NAME_CAP && name[i]; i++) r->policy[i] = name[i];
    r->policy[i] = 0;
    r->attempted = attempted;
    r->caller = caller;
    r->phase = phase;
    r->result = result;
    r->_pad = 0;
    s_ring_head = (s_ring_head + 1u) % POLICY_AUDIT_RING;
    if (s_ring_count < POLICY_AUDIT_RING) s_ring_count++;
}

/* ---- registration ------------------------------------------------------- */

NTSTATUS kernel_policy_register(const char *name, uint64_t value,
                                uint64_t max_value,
                                policy_lock_phase_t seal_phase,
                                uint16_t class_flags, uint8_t owner)
{
    if (!name) return STATUS_INVALID_PARAMETER;

    /* Snapshot the caller's name ONCE into a local buffer in a SINGLE bounded
     * pass -- each source byte is read exactly once, and nl is derived from the
     * copied NUL position (not from a prior measuring pass). All validation, the
     * lockdown guard, the duplicate lookup, the stored copy, and the
     * lockdown-publish decision then use only the snapshot, so a mutable caller
     * buffer that changes mid-call is structurally harmless (a register-time
     * TOCTOU that could otherwise store a malformed name or publish an
     * out-of-domain lockdown value through the uint8 scalar). */
    char nm[POLICY_NAME_CAP];
    uint32_t nl = 0;
    while (nl < POLICY_NAME_CAP) {
        char ch = name[nl];
        nm[nl] = ch;
        if (ch == 0) break;
        nl++;
    }
    if (nl >= POLICY_NAME_CAP)   /* no NUL within the cap: too long/unterminated */
        return STATUS_INVALID_PARAMETER;

    if (!name_in_namespace(nm, nl))
        return STATUS_INVALID_PARAMETER;
    if ((unsigned)seal_phase > (unsigned)POLICY_PHASE_POST_USER_MODE)
        return STATUS_INVALID_PARAMETER;
    if (value > max_value)
        return STATUS_INVALID_PARAMETER;

    /* The published lockdown scalar is a narrowing uint8 cast, so policy.lockdown
     * MUST be domain-bound independent of the caller's max_value -- otherwise a
     * pre-core registration with value=max_value=256 would store 256 and publish
     * (uint8)256 == NONE. Decided on the snapshot, before the lock. */
    int is_lockdown = names_eq(nm, "policy.lockdown");
    if (is_lockdown &&
        (value > (uint64_t)KERNEL_LOCKDOWN_CONFIDENTIALITY ||
         max_value > (uint64_t)KERNEL_LOCKDOWN_CONFIDENTIALITY))
        return STATUS_INVALID_PARAMETER;

    uint64_t flags;
    spin_lock_irqsave(&s_lock, &flags);

    if (find_index(nm) >= 0) {
        spin_unlock_irqrestore(&s_lock, flags);
        return STATUS_OBJECT_NAME_COLLISION;
    }
    if (s_count >= POLICY_MAX) {
        spin_unlock_irqrestore(&s_lock, flags);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    policy_entry_t *p = &s_policies[s_count];
    for (uint32_t c = 0; c < nl; c++) p->name[c] = nm[c];
    p->name[nl] = 0;
    p->value = value;
    p->max_value = max_value;
    p->seal_phase = (uint8_t)seal_phase;
    p->class_flags = class_flags;
    p->owner = owner;
    p->used = 1;

    int idx = (int)s_count;
    s_count++;

    /* Publish the lockless lockdown scalar from the validated snapshot decision;
     * value is already domain-bound above, so the uint8 cast cannot narrow. */
    if (is_lockdown) {
        s_lockdown_idx = idx;
        s_lockdown = (uint8_t)value;
    }

    spin_unlock_irqrestore(&s_lock, flags);
    return STATUS_SUCCESS;
}

/* ---- set / get ---------------------------------------------------------- */

NTSTATUS kernel_policy_set(const char *name, uint64_t value,
                           policy_caller_mode_t caller)
{
    if (!name) return STATUS_INVALID_PARAMETER;

    uint64_t flags;
    spin_lock_irqsave(&s_lock, &flags);

    int idx = find_index(name);
    if (idx < 0) {
        spin_unlock_irqrestore(&s_lock, flags);
        return STATUS_INVALID_PARAMETER;
    }

    policy_entry_t *p = &s_policies[idx];

    /* Domain check FIRST: an out-of-domain value must never enter the row (it
     * would poison the ratchet -- a stored 256 publishes as 0 through the uint8
     * lockdown scalar and then makes legitimate values look like downgrades). */
    if (value > p->max_value) {
        spin_unlock_irqrestore(&s_lock, flags);
        return STATUS_INVALID_PARAMETER;
    }

    uint64_t old = p->value;
    uint16_t cls = p->class_flags;
    uint8_t  ph  = s_phase;                 /* authoritative: read under lock */
    int sealed   = (ph >= p->seal_phase);
    int hard     = (cls & (POLICY_CLASS_SECURE_BOOT | POLICY_CLASS_CODE_INTEGRITY)) != 0;

    uint8_t result;
    int applied = 0, need_panic = 0;

    if (!sealed) {
        p->value = value; applied = 1; result = (uint8_t)POLICY_RESULT_ALLOWED;
    } else if (value == old) {
        result = (uint8_t)POLICY_RESULT_ALLOWED;            /* idempotent no-op */
    } else if (value > old) {
        /* strictly more restrictive: only ratchet/hard classes may advance */
        if ((cls & POLICY_CLASS_RATCHET) || hard) {
            p->value = value; applied = 1; result = (uint8_t)POLICY_RESULT_ALLOWED;
        } else {
            result = (uint8_t)POLICY_RESULT_DENIED_LOCKED;
        }
    } else {
        /* value < old: a downgrade */
        if (hard) {
            if (caller == POLICY_CALLER_KERNEL) {
                result = (uint8_t)POLICY_RESULT_PANIC; need_panic = 1;
            } else {
                result = (uint8_t)POLICY_RESULT_DENIED_RATCHET;
            }
        } else if (cls & POLICY_CLASS_RATCHET) {
            result = (uint8_t)POLICY_RESULT_DENIED_RATCHET;
        } else {
            result = (uint8_t)POLICY_RESULT_DENIED_LOCKED;
        }
    }

    if (applied && idx == s_lockdown_idx)
        s_lockdown = (uint8_t)value;

    audit_append(p->name, value, (uint8_t)caller, ph, result);

    int blocked = (result != (uint8_t)POLICY_RESULT_ALLOWED);
    if (blocked) {
        s_tamper_count++;
        s_tamper_sticky = 1;
    }

    /* Snapshot for the outside-lock emit + panic. */
    char snap[POLICY_NAME_CAP];
    uint32_t i = 0;
    for (; i + 1u < POLICY_NAME_CAP && p->name[i]; i++) snap[i] = p->name[i];
    snap[i] = 0;

    spin_unlock_irqrestore(&s_lock, flags);

    if (blocked) {
        etw_policy_tamper_payload_t pl;
        memset(&pl, 0, sizeof(pl));
        uint32_t j = 0;
        for (; j + 1u < sizeof(pl.policy) && snap[j]; j++) pl.policy[j] = snap[j];
        pl.attempted = value;
        pl.caller = (uint8_t)caller;
        pl.phase = ph;
        pl.result = result;
        etw_emit_kernel_event(ETW_EVT_POLICY_TAMPER, 2 /* warning */,
                              &pl, (uint32_t)sizeof(pl));
        klog(LOG_WARN, "CONF", "[CONF] tamper blocked: %s", short_name(snap));
    }

    if (need_panic) {
        klog(LOG_FATAL, "CONF",
             "[CONF] policy downgrade after lock: %s old=%lu new=%lu",
             short_name(snap), (uint64_t)old, (uint64_t)value);
        KeBugCheckEx(BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE,
                     (uint64_t)ph, old, value, (uint64_t)cls);
    }

    return blocked ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
}

int kernel_policy_get(const char *name, uint64_t *out_value)
{
    if (!name || !out_value) return -1;
    uint64_t flags;
    spin_lock_irqsave(&s_lock, &flags);
    int idx = find_index(name);
    if (idx >= 0) *out_value = s_policies[idx].value;
    spin_unlock_irqrestore(&s_lock, flags);
    return (idx >= 0) ? 0 : -1;
}

/* ---- lockless published scalars ----------------------------------------- */

policy_lock_phase_t kernel_policy_lock_phase_get(void)
{
    return (policy_lock_phase_t)s_phase;
}

kernel_lockdown_level_t kernel_lockdown_level_get(void)
{
    return (kernel_lockdown_level_t)s_lockdown;
}

void kernel_policy_lock_phase_advance(policy_lock_phase_t to)
{
    /* Reject an out-of-range phase before the forward-only comparison so a bad
     * caller cannot publish an undefined phase that seals every policy. */
    if ((unsigned)to > (unsigned)POLICY_PHASE_POST_USER_MODE)
        return;
    uint64_t flags;
    spin_lock_irqsave(&s_lock, &flags);
    if ((uint8_t)to > s_phase) {
        s_phase = (uint8_t)to;
        spin_unlock_irqrestore(&s_lock, flags);
        klog(LOG_INFO, "CONF", "[CONF] policy lock phase -> %u", (uint32_t)to);
        return;
    }
    spin_unlock_irqrestore(&s_lock, flags);
}

/* ---- audit / dump ------------------------------------------------------- */

uint32_t kernel_policy_count(void)
{
    uint64_t flags;
    spin_lock_irqsave(&s_lock, &flags);
    uint32_t n = s_count;
    spin_unlock_irqrestore(&s_lock, flags);
    return n;
}

uint64_t kernel_policy_tamper_count(void)
{
    uint64_t flags;
    spin_lock_irqsave(&s_lock, &flags);
    uint64_t n = s_tamper_count;
    spin_unlock_irqrestore(&s_lock, flags);
    return n;
}

int kernel_policy_tamper_sticky(void)
{
    return s_tamper_sticky;
}

uint32_t kernel_policy_audit_dump(policy_audit_record_t *out, uint32_t max)
{
    if (!out || max == 0) return 0;
    uint64_t flags;
    spin_lock_irqsave(&s_lock, &flags);
    uint32_t avail = s_ring_count;
    uint32_t n = (avail < max) ? avail : max;
    /* Newest first: walk back from the most recent write. */
    for (uint32_t k = 0; k < n; k++) {
        uint32_t slot = (s_ring_head + POLICY_AUDIT_RING - 1u - k) % POLICY_AUDIT_RING;
        out[k] = s_ring[slot];
    }
    spin_unlock_irqrestore(&s_lock, flags);
    return n;
}

/* ---- core registration -------------------------------------------------- */

void kernel_policy_register_core(void)
{
    static int s_done;
    if (s_done) return;
    s_done = 1;

    /* Secure Boot active maps to a default lockdown level: integrity. When
     * Secure Boot is off/unknown the default is none (Linux lockdown= parity:
     * lockdown is opt-in, raised here from the firmware trust state). */
    uint8_t sb_on = g_boot_info.secure_boot_enabled ? 1u : 0u;
    uint64_t default_lockdown = sb_on ? (uint64_t)KERNEL_LOCKDOWN_INTEGRITY
                                      : (uint64_t)KERNEL_LOCKDOWN_NONE;

    /* Lockdown level: ratchet, sealed after the security reference monitor.
     * Domain { none=0, integrity=1, confidentiality=2 }. */
    kernel_policy_register("policy.lockdown", default_lockdown,
                           (uint64_t)KERNEL_LOCKDOWN_CONFIDENTIALITY,
                           POLICY_PHASE_POST_SECURITY_INIT,
                           POLICY_CLASS_RATCHET, 0);

    /* Secure Boot state: hard class; downgrade after registry merge panics for
     * KernelMode callers. Higher = more restrictive (off=0 < on=1). */
    kernel_policy_register("policy.secure_boot", sb_on, 1,
                           POLICY_PHASE_POST_REGISTRY,
                           POLICY_CLASS_SECURE_BOOT | POLICY_CLASS_RATCHET, 0);

    /* Code Integrity mode: hard class (off=0 < audit=1 < enforce=2). Defaults
     * to enforce when Secure Boot is on, audit otherwise. */
    kernel_policy_register("policy.ci.mode", sb_on ? 2u : 1u, 2,
                           POLICY_PHASE_POST_REGISTRY,
                           POLICY_CLASS_CODE_INTEGRITY | POLICY_CLASS_RATCHET, 0);

    /* Memory-protection policies: ratchet, sealed at the earliest safe phase
     * (post-security-init). enabled=1 is more restrictive than disabled=0. */
    kernel_policy_register("policy.kaslr", 1, 1,
                           POLICY_PHASE_POST_SECURITY_INIT, POLICY_CLASS_RATCHET, 0);
    kernel_policy_register("policy.smep_smap", 1, 1,
                           POLICY_PHASE_POST_SECURITY_INIT, POLICY_CLASS_RATCHET, 0);
    kernel_policy_register("policy.kpti", 1, 1,
                           POLICY_PHASE_POST_SECURITY_INIT, POLICY_CLASS_RATCHET, 0);

    /* Debugger lockout: stored as lockout sense (0 = debugger allowed,
     * 1 = debugger blocked). Sealed after user mode is reachable. */
    kernel_policy_register("policy.debug.lockout", sb_on, 1,
                           POLICY_PHASE_POST_USER_MODE, POLICY_CLASS_RATCHET, 0);

    /* Boot-verifier enforcement: enforced=1 is more restrictive. */
    kernel_policy_register("policy.boot_verifier", sb_on, 1,
                           POLICY_PHASE_POST_REGISTRY, POLICY_CLASS_RATCHET, 0);

    klog(LOG_INFO, "CONF", "[CONF] policy registry: %u core policies, lockdown=%u",
         kernel_policy_count(), (uint32_t)s_lockdown);
}
