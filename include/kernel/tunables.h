/* ============================================================================
 * tunables.h -- Runtime Tunable Registry
 *
 * One typed registration surface for mutable kernel policy, replacing one-off
 * globals and ad-hoc parser branches. Each tunable carries type, access flags,
 * a clamped [min,max] range, a default, an owner subsystem, a lock phase, and
 * provenance. Writes clamp out-of-range values, enforce read-only/privileged/
 * boot-only/debug-only access, dispatch an optional change callback at
 * PASSIVE_LEVEL (deferred via the system workqueue when set from a non-passive
 * context), and reject recursive self-writes.
 *
 * Lock phases here are a minimal monotonic substrate (BOOT -> RUNTIME ->
 * LOCKED); the richer tamper-audit lock-phase model is owned by the policy lock
 * phases feature. Module-sourced tunables (source TUNABLE_SRC_MODULE) require
 * the owning module to call kernel_tunable_unregister_owner() before its code is
 * unmapped; the loader-side register/unregister wiring is owned by the kernel
 * module system.
 * ============================================================================ */
#ifndef KERNEL_TUNABLES_H
#define KERNEL_TUNABLES_H

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"

typedef enum {
    TUNABLE_INT  = 0,   /* signed integer */
    TUNABLE_UINT = 1,   /* unsigned integer (stored in int64 range) */
    TUNABLE_BOOL = 2,   /* 0 or 1 */
    TUNABLE_ENUM = 3,   /* small enumerated set, gated by [min,max] */
} tunable_type_t;

/* Access flags (bitmask). */
#define TUNABLE_READONLY    0x0001u  /* never externally settable */
#define TUNABLE_BOOT_ONLY   0x0002u  /* settable only before the boot lock phase ends */
#define TUNABLE_RUNTIME     0x0004u  /* settable at runtime (informational; default) */
#define TUNABLE_PRIVILEGED  0x0008u  /* caller must present TUNABLE_SET_PRIVILEGED */
#define TUNABLE_DEBUG_ONLY  0x0010u  /* settable only when kernel debug is enabled */

/* Provenance: where the registration / current value originated. */
typedef enum {
    TUNABLE_SRC_BUILTIN  = 0,   /* compiled-in core tunable */
    TUNABLE_SRC_CMDLINE  = 1,   /* seeded from a boot argument */
    TUNABLE_SRC_REGISTRY = 2,   /* seeded from registry policy */
    TUNABLE_SRC_MODULE   = 3,   /* registered by a loadable module */
} tunable_source_t;

/* Minimal monotonic lock phase. The policy-lock-phases feature extends this
 * with tamper-audit semantics. */
typedef enum {
    TUNABLE_PHASE_BOOT    = 0,  /* boot-only tunables still settable */
    TUNABLE_PHASE_RUNTIME = 1,  /* boot-only tunables sealed */
    TUNABLE_PHASE_LOCKED  = 2,  /* all external writes refused (lockdown) */
} tunable_phase_t;

/* Change callback. Invoked at PASSIVE_LEVEL with the registry lock NOT held. */
typedef void (*tunable_cb_t)(const char *name, int64_t new_value, void *ctx);

/* set() flags. */
#define TUNABLE_SET_PRIVILEGED  0x01u  /* caller holds the required privilege */
#define TUNABLE_SET_DEFER       0x02u  /* force the callback to run deferred (syscall path) */

/* Register a tunable. Returns STATUS_SUCCESS, STATUS_OBJECT_NAME_COLLISION on a
 * duplicate name, STATUS_INSUFFICIENT_RESOURCES when the table is full, or
 * STATUS_INVALID_PARAMETER on a malformed descriptor (def out of [min,max],
 * min>max, name too long/empty). */
NTSTATUS kernel_tunable_register(const char *name, tunable_type_t type,
                                 uint16_t flags, int64_t min, int64_t max,
                                 int64_t def, tunable_cb_t callback, void *cb_ctx,
                                 uint8_t owner_subsys, tunable_source_t source);

/* Set a tunable's value. Clamps to [min,max] (logging a clamp), enforces the
 * access flags, and dispatches the callback. Returns STATUS_SUCCESS (applied
 * inline), STATUS_PENDING (callback deferred to the workqueue), STATUS_NOT_FOUND,
 * STATUS_ACCESS_DENIED (read-only / unprivileged / boot-only-after-seal /
 * debug-only), STATUS_UNSUCCESSFUL (recursive self-write), or
 * STATUS_INSUFFICIENT_RESOURCES (deferred dispatch could not be scheduled -- the
 * stored value is left unchanged). */
NTSTATUS kernel_tunable_set(const char *name, int64_t value, uint32_t set_flags);

/* Read the current value. STATUS_NOT_FOUND if the name is unknown. */
NTSTATUS kernel_tunable_get(const char *name, int64_t *out_value);

/* Convenience read for unsigned consumers; returns fallback if unknown. */
uint64_t kernel_tunable_get_u64(const char *name, uint64_t fallback);

/* Remove every tunable owned by owner_subsys. Called by the module loader
 * during module unload BEFORE the module image is unmapped, so a stale callback
 * pointer can never be invoked. Quiesces in-flight callbacks before freeing
 * (spins until none are executing), so the loader should flush the system work
 * queue first to drain deferred trampolines. MUST NOT be called from the work
 * queue worker thread itself (it would wait on its own callback). Returns the
 * count removed. */
uint32_t kernel_tunable_unregister_owner(uint8_t owner_subsys);

/* Lock-phase accessors. advance() is monotonic (a request to a lower phase is
 * ignored). Advancing past BOOT seals TUNABLE_BOOT_ONLY tunables. */
tunable_phase_t kernel_tunable_lock_phase_get(void);
void            kernel_tunable_lock_phase_advance(tunable_phase_t to);

/* Register the built-in core tunables (klog, panic, timer, ALPC, handle quota,
 * verifier). Idempotent; safe to call once at Phase 3. */
void kernel_tunables_register_core(void);

/* Number of registered tunables (for audit / dump). */
uint32_t kernel_tunable_count(void);

/* Audit dump descriptor: a flattened snapshot row for kernel_tunable_dump(). */
typedef struct {
    char             name[48];
    int64_t          cur;
    int64_t          def;
    int64_t          min;
    int64_t          max;
    uint16_t         flags;
    uint8_t          type;
    uint8_t          owner_subsys;
    uint8_t          source;
    uint8_t          _pad[3];
} tunable_snapshot_t;

/* Copy up to max_rows snapshots into out; returns the number written. */
uint32_t kernel_tunable_dump(tunable_snapshot_t *out, uint32_t max_rows);

#endif /* KERNEL_TUNABLES_H */
