/* ============================================================================
 * boot_status.h -- Boot status policy + boot success ledger
 * (TODO-02 kernel configuration policy, boot-status section)
 *
 * Turns boot success into an explicit, policy-controlled state machine so the
 * "bless this boot" decision (A/B mark-good, rollback-counter reset, per-entry
 * MarkGood, LastKnownGood update) no longer hinges on one ad-hoc first-frame
 * heuristic scattered across the compositor and the health gate.
 *
 * Two objects:
 *   1. boot_status_policy_t -- a Phase-3 EFFECTIVE-POLICY object (NOT the
 *      immutable Phase-0 kernel_config_t snapshot): the acceptance stage a boot
 *      must reach before it is blessed, the failed-boot display policy, the
 *      recovery-enabled flag, and the failed-boot escalation threshold. Parsed
 *      from the already-validated bootstatuspolicy / recoveryenabled boot args
 *      with provenance.
 *   2. The per-boot acceptance LEDGER -- a monotonic, forward-only stage that
 *      every readiness milestone advances. When the ledger first reaches the
 *      configured acceptance stage, exactly ONE caller wins the accepted
 *      transition and fires all bless side effects. This is the single success
 *      authority that replaces the compositor first-frame mark-good and the
 *      health-gate per-entry mark-good (both now route through here).
 *
 * Durability: a small versioned record is persisted to NVRAM at the accepted
 * transition so the next boot (and the A/B rollback + recovery-escalation
 * consumers) can read the last boot's outcome, recovery-suppression reason, and
 * rollback hint. The failure-bucket CLASSIFICATION and escalation policy are
 * owned by the kernel recovery-escalation TODO; the LastKnownGood control-set
 * copy is owned by the kernel ControlSet/LastKnownGood selection feature.
 *
 * Design invariants: durable persistence is part of this feature (not deferred);
 * the accepted transition is exactly-once and owns ALL bless side effects, so the
 * compositor and the health gate route through this ledger rather than blessing
 * the boot independently; policy fields lock as separate restrictive-sense
 * policy_lock rows, never one packed ratchet value.
 * ============================================================================ */

#ifndef KERNEL_BOOT_STATUS_H
#define KERNEL_BOOT_STATUS_H

#include "kernel/types.h"

/* Monotonic, forward-only acceptance stages. Higher = closer to accepted; the
 * ledger never moves backward (a late lower-stage advance is a no-op). The
 * "console-or-desktop ready" milestone is one stage (UI_READY): on a GUI boot
 * the compositor's first composited frame raises it; on a nogui boot the
 * console-ready milestone raises it. */
typedef enum {
    BOOT_ACCEPT_PENDING          = 0,  /* boot in progress, not yet blessable */
    BOOT_ACCEPT_UI_READY         = 1,  /* console-or-desktop reached first output */
    BOOT_ACCEPT_CRITICAL_READY   = 2,  /* critical services up (health gate PASS) */
    BOOT_ACCEPT_REGISTRY_FLUSHED = 3,  /* registry/config durably persisted */
    BOOT_ACCEPT_ACCEPTED         = 4,  /* boot fully accepted (terminal) */
    BOOT_ACCEPT_COUNT,                 /* sentinel: stage-name table length guard */
} boot_accept_stage_t;

/* Failed-boot display policy (mirrors the bootstatuspolicy boot-arg enum). */
typedef enum {
    BOOT_STATUS_DISPLAY_ALL_FAILURES = 0,  /* show the recovery UI on failure */
    BOOT_STATUS_IGNORE_ALL_FAILURES  = 1,  /* boot through failures silently */
    BOOT_STATUS_DISPLAY_COUNT,             /* sentinel: name-table length guard */
} boot_failure_display_t;

/* Where the policy values came from (provenance for diagnostics). */
typedef enum {
    BOOT_STATUS_PROV_DEFAULT = 0,  /* compiled-in default */
    BOOT_STATUS_PROV_BOOTCFG = 1,  /* boot.conf / BOOTCFG layer */
    BOOT_STATUS_PROV_CMDLINE = 2,  /* explicit command line */
    BOOT_STATUS_PROV_COUNT,        /* sentinel */
} boot_status_prov_t;

/* Phase-3 effective boot-status policy. Resolved once by boot_status_init(). */
typedef struct {
    uint8_t  accept_stage;     /* boot_accept_stage_t a boot must reach to bless */
    uint8_t  failure_display;  /* boot_failure_display_t */
    uint8_t  recovery_enabled; /* 1 = recovery environment entry permitted */
    uint8_t  provenance;       /* boot_status_prov_t for the policy as a whole */
    uint16_t failed_threshold; /* failed boots before recovery escalation */
    uint16_t _pad;
} boot_status_policy_t;

/* Durable cross-boot record. Persisted to NVRAM at the accepted transition;
 * read on the next boot by the A/B rollback + recovery-escalation consumers.
 * Append-only -- bump BOOT_STATUS_RECORD_VERSION on any field change. */
#define BOOT_STATUS_RECORD_VERSION 1u
typedef struct {
    uint32_t schema_version;      /* BOOT_STATUS_RECORD_VERSION */
    uint8_t  last_stage;          /* highest boot_accept_stage_t the prior boot reached */
    uint8_t  failure_bucket;      /* classification owned by recovery escalation (0 = none) */
    uint8_t  recovery_suppressed; /* 1 = prior boot suppressed recovery (policy) */
    uint8_t  rollback_hint;       /* hint for the A/B rollback consumer (0 = none) */
    uint32_t crc32;               /* integrity over the preceding fields */
} boot_status_record_t;

/* Pure resolver (unit-testable, no live state). Maps the validated boot-arg
 * values to the effective policy. `bsp_arg` = bootstatuspolicy enum index,
 * `recovery_arg` = recoveryenabled bool, `from_cmdline` = either came from the
 * command line (vs default/BOOTCFG). Writes the resolved policy to `out`.
 * Out-of-domain inputs clamp to safe defaults (never trust untrusted args). */
void boot_status_policy_resolve(uint8_t bsp_arg, uint8_t recovery_arg,
                                int from_cmdline, boot_status_policy_t *out);

/* Pure CRC helper for the durable record (testable). Computes the CRC32 over
 * every field EXCEPT crc32 itself. */
uint32_t boot_status_record_crc(const boot_status_record_t *r);

/* Pure ledger transition decider (unit-testable, no live state). Given the
 * current stage, a requested stage, and the configured accept stage, returns
 * the new stage (monotonic max) via *out_new and 1 iff this transition first
 * reaches the accept stage (the exactly-once accepted-transition signal). */
int boot_status_ledger_step(boot_accept_stage_t cur, boot_accept_stage_t req,
                            boot_accept_stage_t accept_at,
                            boot_accept_stage_t *out_new);

/* Phase-3 init: resolve the effective policy from the boot args, register the
 * boot-status policy rows in policy_lock (sealed, stricter-only at runtime), and
 * load the prior-boot durable record from NVRAM. Idempotent. */
void boot_status_init(void);

/* The resolved effective policy (NULL before boot_status_init()). */
const boot_status_policy_t *boot_status_policy_get(void);

/* Current ledger stage (lockless atomic load). BOOT_ACCEPT_PENDING before init. */
boot_accept_stage_t boot_status_stage(void);

/* Advance the ledger toward `stage` (monotonic: a lower or equal stage is a
 * no-op). When the advance causes the ledger to first reach the configured
 * acceptance stage, EXACTLY ONE caller wins the accepted transition and fires
 * the bless side effects (A/B mark-good, rollback-counter reset, per-entry
 * MarkGood, durable record write). Returns 1 iff THIS call won that transition,
 * 0 otherwise. Safe to call from any context (BSP Phase 3, compositor first
 * frame, future critical-services thread). */
int boot_status_accept_advance(boot_accept_stage_t stage);

/* 1 once the boot has reached BOOT_ACCEPT_ACCEPTED. */
int boot_status_accepted(void);

/* Record the Phase-3 health-gate verdict. The accepted transition emits the
 * per-entry MarkGood (boot-entry tries retire) ONLY when health passed, so the
 * health gate no longer marks good independently of the ledger -- it routes its
 * verdict through here and the ledger owns the single bless. Call once on the
 * health-gate PASS path. */
void boot_status_note_health_pass(void);

/* 1 if the Phase-3 health gate reported PASS (precondition for per-entry
 * MarkGood at the accepted transition). */
int boot_status_health_passed(void);

/* Copy the prior-boot durable record into `out`. Returns 1 if a valid record
 * was loaded from NVRAM at init, 0 if absent/corrupt (out zeroed). */
int boot_status_last_record(boot_status_record_t *out);

/* Stage name for logs/diagnostics ("pending", "ui-ready", ...). */
const char *boot_status_stage_name(boot_accept_stage_t stage);

#endif /* KERNEL_BOOT_STATUS_H */
