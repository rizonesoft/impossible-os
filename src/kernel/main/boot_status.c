/* ============================================================================
 * boot_status.c -- Boot status policy + boot success ledger
 * (TODO-02 kernel configuration policy, boot-status section)
 *
 * Single boot-success authority. The acceptance LEDGER is one atomic stage; the
 * caller that first drives it to the configured accept stage wins an exactly-
 * once accepted transition (CAS latch) and performs ALL bless side effects:
 * A/B slot mark-good, per-entry MarkGood (only if the Phase-3 health gate
 * passed), and a versioned CRC32 NVRAM record for the next boot. The compositor
 * first frame and the health gate route their signals through here instead of
 * blessing the boot independently.
 *
 * SMP: the stage and the accepted latch are plain atomics; no spinlock is held
 * across the bless I/O (firmware SetVariable + disk), which runs once on the
 * first-frame thread after the frame is presented -- the same context that
 * already performed the A/B mark before this section unified the authorities.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_status.h"
#include "kernel/config.h"
#include "kernel/policy_lock.h"
#include "kernel/klog.h"
#include "kernel/boot_halt.h"
#include "kernel/uefi_runtime.h"
#include "kernel/boot_info.h"
#include "kernel/boot_health_check.h"
#include "kernel/fs/gpt.h"
#include "kernel/fs/partition.h"
#include "kernel/sched/workqueue.h"
#include "libc/string.h"

extern struct boot_info g_boot_info;

/* ---- state -------------------------------------------------------------- */

static boot_status_policy_t s_policy;
static int                  s_policy_ready;        /* published last at init */

static volatile int s_stage        = (int)BOOT_ACCEPT_PENDING;  /* atomic ledger */
static volatile int s_accept_fired;                /* exactly-once accept latch */
static volatile int s_health_passed;               /* Phase-3 health-gate verdict */

static boot_status_record_t s_last;                /* prior-boot durable record */
static int                  s_last_valid;

/* Highest acceptance stage with a live advancer today. A policy may not require
 * a stage past this or the boot could never bless (rollback storm): the resolver
 * clamps accept_stage to this ceiling and logs when it does. The compositor
 * first frame (and the nogui console-ready path) drive UI_READY; CRITICAL_READY
 * and REGISTRY_FLUSHED are reserved for future service-manager / config-flush
 * milestones and have no advancer yet. */
#define BOOT_ACCEPT_MAX_LIVE  BOOT_ACCEPT_UI_READY

/* ---- NVRAM durable record ---------------------------------------------- */

/* IPOSBootStatus GUID + UCS-2 name. NV | BS | RT = 0x7. Distinct from the
 * anti-rollback IPOSRequiredSecVersion variable. */
static struct boot_uefi_guid s_status_guid = {
    0x6f35d3a4, 0xc0e6, 0x4a82,
    { 0xb5, 0xd8, 0x7c, 0x9d, 0x2e, 0x4f, 0x8a, 0x20 }
};
static const uint16_t s_status_name[] = {
    'I','P','O','S','B','o','o','t','S','t','a','t','u','s', 0
};
/* Canonical UEFI variable attributes: NV | BS | RT. A read whose attributes
 * differ is treated as untrusted (externally-created / stale). */
#define BOOT_STATUS_VAR_ATTRS  0x7u

uint32_t boot_status_record_crc(const boot_status_record_t *r)
{
    if (!r) return 0;
    /* CRC over every field before crc32 (IEEE CRC32, shared GPT helper). */
    return gpt_crc32(r, (uint32_t)__builtin_offsetof(boot_status_record_t, crc32));
}

static int load_record(boot_status_record_t *out)
{
    if (!out) return 0;
    boot_status_record_t r;
    memset(&r, 0, sizeof(r));
    uint32_t attrs = 0;
    uint64_t sz = sizeof(r);
    uint64_t st = uefi_get_variable(&s_status_guid, s_status_name, &attrs, &sz, &r);
    if (st != 0 || sz != sizeof(r))
        return 0;                       /* absent / wrong size */
    /* Reject any variable whose attributes are not the canonical NV|BS|RT that
     * store_record() writes. name+GUID alone can match an externally-created or
     * stale variable; UEFI cannot rewrite attributes in place, so a wrong-attr
     * record must not be trusted as the prior boot status. */
    if (attrs != BOOT_STATUS_VAR_ATTRS)
        return 0;
    if (r.schema_version != BOOT_STATUS_RECORD_VERSION)
        return 0;                       /* version mismatch -- ignore */
    if (r.crc32 != boot_status_record_crc(&r))
        return 0;                       /* corrupt */
    /* CRC is integrity, not authenticity -- any prior OS / firmware / user with
     * variable-write access can recompute it. Validate every field domain before
     * exposing the record to the rollback / recovery consumers, so a tampered or
     * all-0xFF record can never be read as "accepted" or "recovery-suppressed". */
    if (r.last_stage >= (uint8_t)BOOT_ACCEPT_COUNT)
        return 0;
    if (r.recovery_suppressed > 1u)
        return 0;
    /* failure_bucket + rollback_hint enums are owned by the recovery-escalation
     * and A/B-rollback consumers and are not defined yet; until they ship (and
     * bump BOOT_STATUS_RECORD_VERSION), only 0 (none) is a known-good value. */
    if (r.failure_bucket != 0u || r.rollback_hint != 0u)
        return 0;
    *out = r;
    return 1;
}

static void store_record(const boot_status_record_t *r)
{
    if (!r) return;
    uint64_t st = uefi_set_variable(&s_status_guid, s_status_name,
                                    BOOT_STATUS_VAR_ATTRS, sizeof(*r), r);
    if (st == 0)
        return;                         /* wrote fine */

    /* The write failed. The ONLY case worth a destructive repair is a
     * pre-existing variable whose attributes are non-canonical: UEFI forbids
     * changing a variable's attributes in place, so the write can never succeed
     * until that variable is deleted. Probe the existing attributes first --
     * GetVariable populates `cur_attrs` iff the variable exists (it stays 0 when
     * absent or when the firmware did not report attributes, which keeps the
     * repair from firing). Repair ONLY a wrong-attr variable; for any other
     * failure (transient / resource) preserve the prior record rather than
     * deleting a possibly-valid one. */
    boot_status_record_t probe;
    uint32_t cur_attrs = 0;
    uint64_t cur_sz = sizeof(probe);
    (void)uefi_get_variable(&s_status_guid, s_status_name, &cur_attrs,
                            &cur_sz, &probe);
    if (cur_attrs != 0u && cur_attrs != BOOT_STATUS_VAR_ATTRS) {
        /* Delete using the variable's CURRENT attributes (required to remove it),
         * then recreate canonical. */
        uint64_t dst = uefi_set_variable(&s_status_guid, s_status_name,
                                         cur_attrs, 0, (const void *)0);
        st = uefi_set_variable(&s_status_guid, s_status_name,
                               BOOT_STATUS_VAR_ATTRS, sizeof(*r), r);
        klog(LOG_WARN, "CONF",
             "[CONF] boot-status wrong-attr repair (del=0x%lx set=0x%lx)",
             (uint64_t)dst, (uint64_t)st);
        return;
    }
    klog(LOG_WARN, "CONF",
         "[CONF] boot-status NVRAM write failed (0x%lx); prior record kept",
         (uint64_t)st);
}

/* ---- pure helpers ------------------------------------------------------- */

static const char *const k_stage_names[] = {
    "pending", "ui-ready", "critical-ready", "registry-flushed", "accepted",
};
_Static_assert(sizeof(k_stage_names) / sizeof(k_stage_names[0]) == BOOT_ACCEPT_COUNT,
    "stage-name table must match boot_accept_stage_t");

const char *boot_status_stage_name(boot_accept_stage_t stage)
{
    if ((unsigned)stage >= (unsigned)BOOT_ACCEPT_COUNT)
        return "?";
    return k_stage_names[stage];
}

void boot_status_policy_resolve(uint8_t bsp_arg, uint8_t recovery_arg,
                                uint8_t source, boot_status_policy_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    /* Failed-boot display: clamp an out-of-domain enum to the safe default
     * (show failures) rather than trusting an untrusted boot arg. */
    out->failure_display = (bsp_arg == (uint8_t)BOOT_STATUS_IGNORE_ALL_FAILURES)
                               ? (uint8_t)BOOT_STATUS_IGNORE_ALL_FAILURES
                               : (uint8_t)BOOT_STATUS_DISPLAY_ALL_FAILURES;
    out->recovery_enabled = recovery_arg ? 1u : 0u;

    /* Acceptance stage: default is UI_READY (bless at the first composited
     * frame). Clamped to the highest live-advancer stage so a policy can never
     * pin acceptance to an unreachable stage and stall every boot's bless. */
    out->accept_stage = (uint8_t)BOOT_ACCEPT_UI_READY;
    out->failed_threshold = 3u;     /* recovery-escalation default (T30 consumes) */
    out->provenance =
        (source == (uint8_t)BOOT_ARG_SRC_CMDLINE) ? (uint8_t)BOOT_STATUS_PROV_CMDLINE :
        (source == (uint8_t)BOOT_ARG_SRC_BOOTCFG) ? (uint8_t)BOOT_STATUS_PROV_BOOTCFG :
                                                    (uint8_t)BOOT_STATUS_PROV_DEFAULT;
}

int boot_status_ledger_step(boot_accept_stage_t cur, boot_accept_stage_t req,
                            boot_accept_stage_t accept_at,
                            boot_accept_stage_t *out_new)
{
    boot_accept_stage_t nw = (req > cur) ? req : cur;   /* monotonic max */
    if (out_new) *out_new = nw;
    /* The transition first reaches accept iff it crosses the threshold now. */
    return (cur < accept_at && nw >= accept_at) ? 1 : 0;
}

/* ---- accept transition + bless ----------------------------------------- */

static void do_accept_bless(void)
{
    /* A/B dual-slot mark-good: resets the active slot's tries + sets
     * successful. No-op on non-A/B disks; refused on slot mismatch. */
    (void)ab_boot_mark_slot_successful((int)g_boot_info.active_slot);

    /* Per-entry MarkGood (boot-entry tries retire). ONLY when the health gate
     * passed -- the gate no longer marks good on its own; it routes its verdict
     * through the ledger so there is one bless authority, not two. */
    if (__atomic_load_n(&s_health_passed, __ATOMIC_ACQUIRE))
        (void)mark_entry_successful(g_boot_info.selected_entry_id);

    /* Durable record for the next boot's rollback / recovery-escalation
     * consumers: an accepted boot, no failure bucket, recovery-suppression per
     * the resolved policy. */
    boot_status_record_t r;
    memset(&r, 0, sizeof(r));
    r.schema_version      = BOOT_STATUS_RECORD_VERSION;
    r.last_stage          = (uint8_t)BOOT_ACCEPT_ACCEPTED;
    r.failure_bucket      = 0;
    r.recovery_suppressed = s_policy.recovery_enabled ? 0u : 1u;
    r.rollback_hint       = 0;
    r.crc32               = boot_status_record_crc(&r);
    store_record(&r);

    klog(LOG_INFO, "CONF", "[CONF] boot accepted (stage=%s)",
         boot_status_stage_name(BOOT_ACCEPT_ACCEPTED));
}

/* sys_wq worker: runs the bless I/O off the caller's thread (typically the
 * compositor first-frame thread). */
static void bless_worker(void *arg)
{
    (void)arg;
    do_accept_bless();
}

int boot_status_accept_advance(boot_accept_stage_t stage)
{
    if (!s_policy_ready)
        return 0;                       /* ledger inactive before init */
    if ((unsigned)stage >= (unsigned)BOOT_ACCEPT_COUNT)
        return 0;

    /* Monotonic max-advance via CAS (the stage never moves backward). */
    int want = (int)stage;
    int old  = __atomic_load_n(&s_stage, __ATOMIC_ACQUIRE);
    while (want > old) {
        if (__atomic_compare_exchange_n(&s_stage, &old, want, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;                      /* published; old now == want */
        /* CAS failed: `old` reloaded with the current value -- retry. */
    }

    int cur = __atomic_load_n(&s_stage, __ATOMIC_ACQUIRE);
    if (cur < (int)s_policy.accept_stage)
        return 0;                       /* accept stage not reached yet */

    /* Reached the accept stage: fire the bless EXACTLY ONCE across all CPUs. */
    int expected = 0;
    if (!__atomic_compare_exchange_n(&s_accept_fired, &expected, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;                       /* another caller already won */

    /* Publish the terminal stage first, then defer the bless I/O (A/B disk RMW +
     * firmware SetVariable, 10-100ms on real hardware) to sys_wq so it never
     * stalls the caller -- typically the compositor first-frame thread. Fall
     * back to a synchronous bless only if the workqueue is not up yet, so the
     * boot is still durably blessed. Mirrors boot_rollback_request_raise(). */
    __atomic_store_n(&s_stage, (int)BOOT_ACCEPT_ACCEPTED, __ATOMIC_RELEASE);
    if (sys_wq == (workqueue_t *)0 ||
        !workqueue_enqueue(sys_wq, bless_worker, (void *)0))
        do_accept_bless();
    return 1;
}

void boot_status_note_health_pass(void)
{
    __atomic_store_n(&s_health_passed, 1, __ATOMIC_RELEASE);
}

int boot_status_health_passed(void)
{
    return __atomic_load_n(&s_health_passed, __ATOMIC_ACQUIRE);
}

boot_accept_stage_t boot_status_stage(void)
{
    return (boot_accept_stage_t)__atomic_load_n(&s_stage, __ATOMIC_ACQUIRE);
}

int boot_status_accepted(void)
{
    return __atomic_load_n(&s_stage, __ATOMIC_ACQUIRE) >= (int)BOOT_ACCEPT_ACCEPTED;
}

const boot_status_policy_t *boot_status_policy_get(void)
{
    return s_policy_ready ? &s_policy : (const boot_status_policy_t *)0;
}

int boot_status_last_record(boot_status_record_t *out)
{
    if (!out) return 0;
    if (!s_last_valid) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    *out = s_last;
    return 1;
}

/* ---- init --------------------------------------------------------------- */

void boot_status_init(void)
{
    static int s_done;
    if (s_done) return;

    /* Resolve the effective policy from the validated boot args. Provenance
     * tracks the bootstatuspolicy arg's source (the primary policy knob);
     * BOOTCFG vs CMDLINE vs the compiled default are all distinguishable. */
    uint8_t bsp = (uint8_t)BOOT_STATUS_DISPLAY_ALL_FAILURES;
    uint8_t rec = 1u;
    uint8_t src = (uint8_t)BOOT_ARG_SRC_DEFAULT;
    const boot_args_t *a = boot_args_parsed();
    if (a) {
        const boot_arg_value_t *vp = boot_args_get(a, "bootstatuspolicy");
        if (vp) {
            bsp = (uint8_t)vp->ival;
            src = (uint8_t)vp->source;
        }
        const boot_arg_value_t *vr = boot_args_get(a, "recoveryenabled");
        if (vr)
            rec = vr->ival ? 1u : 0u;
    }
    boot_status_policy_resolve(bsp, rec, src, &s_policy);

    /* Defensive clamp: never require an acceptance stage with no live advancer
     * (a higher bar would leave every boot pending -> rollback storm). */
    if (s_policy.accept_stage > (uint8_t)BOOT_ACCEPT_MAX_LIVE) {
        klog(LOG_WARN, "CONF",
             "[CONF] boot-status accept stage %u has no advancer -- clamping to %s",
             (uint32_t)s_policy.accept_stage,
             boot_status_stage_name(BOOT_ACCEPT_MAX_LIVE));
        s_policy.accept_stage = (uint8_t)BOOT_ACCEPT_MAX_LIVE;
    }

    /* Lock the boot-status policy as separate restrictive-sense ratchet rows,
     * sealed after the registry merge (stricter-only at runtime). Higher numeric
     * = more restrictive for each: recovery LOCKOUT (1 = recovery disabled) and
     * the acceptance-stage bar (higher = more must succeed before a bless). */
    NTSTATUS r1 = kernel_policy_register("policy.recovery_lockout",
                                 s_policy.recovery_enabled ? 0u : 1u, 1u,
                                 POLICY_PHASE_POST_REGISTRY,
                                 POLICY_CLASS_RATCHET, 0);
    /* max_value is clamped to the highest stage with a LIVE advancer so a ratchet
     * write can never pin acceptance to an unreachable stage (which would stall
     * every boot's bless). Raise the cap in lockstep when a new milestone wires
     * its advancer. */
    NTSTATUS r2 = kernel_policy_register("policy.boot_accept_stage",
                                 (uint64_t)s_policy.accept_stage,
                                 (uint64_t)BOOT_ACCEPT_MAX_LIVE,
                                 POLICY_PHASE_POST_REGISTRY,
                                 POLICY_CLASS_RATCHET, 0);
    /* A missing boot-status policy row means the stricter-only / tamper-audited
     * lock contract is silently absent past the seal -- treat it as fatal before
     * the phase advances, exactly like the core security policy registration. */
    if (r1 != STATUS_SUCCESS || r2 != STATUS_SUCCESS)
        boot_halt("boot-status policy row registration failed -- refusing to seal an incomplete boot-status policy");

    /* Load the prior-boot durable record for rollback / recovery consumers. */
    s_last_valid = load_record(&s_last);

    s_done = 1;
    __atomic_store_n(&s_policy_ready, 1, __ATOMIC_RELEASE);   /* publish last */

    klog(LOG_INFO, "CONF",
         "[CONF] boot-status policy: accept=%s display=%s recovery=%s%s",
         boot_status_stage_name((boot_accept_stage_t)s_policy.accept_stage),
         s_policy.failure_display == (uint8_t)BOOT_STATUS_IGNORE_ALL_FAILURES
             ? "ignore" : "show",
         s_policy.recovery_enabled ? "on" : "off",
         s_last_valid ? " (prior record loaded)" : "");
}
