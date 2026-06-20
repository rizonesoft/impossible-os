/* ============================================================================
 * feature.h -- Feature Flag Gates and Experiment Cohorts
 *
 * Discoverable, auditable feature flags with staged-rollout cohorts, replacing
 * ad-hoc ifdefs and hidden globals. Each flag lives in the reserved
 * "feature.<name>" or "experiment.<name>" namespace with an owner tag.
 *
 * Resolution order for kernel_feature_enabled(name), computed once and cached:
 *   1. explicit boot-arg override ("feature.<name>=on|off" scanned from the raw
 *      kernel command line -- the boot-argument schema parser drops unknown
 *      dynamic keys, so the feature subsystem owns a bounded raw-cmdline
 *      scanner);
 *   2. machine-cohort rollout (stable per machine, skipped when no valid
 *      machine UUID is available -- never hash a sentinel UUID);
 *   3. the registered default.
 *
 * Secure Boot guard: a FEATURE_SECURITY flag can be DISABLED only when Secure
 * Boot is known-off (state readable AND inactive). When Secure Boot is active
 * OR its state is unreadable/unknown, a disable override or cohort-off is
 * REFUSED (fail closed) and logged. The registry-backed override provider is
 * owned by the registry-merge feature and not wired here yet.
 * ============================================================================ */
#ifndef KERNEL_FEATURE_H
#define KERNEL_FEATURE_H

#include "kernel/types.h"

#define FEATURE_NAME_CAP   48u   /* incl. NUL; names start with feature./experiment. */

/* Flags (bitmask). */
#define FEATURE_SECURITY    0x01u  /* cannot be disabled under active/unknown Secure Boot */
#define FEATURE_EXPERIMENT  0x02u  /* experiment.<name> namespace (set automatically) */
#define FEATURE_LOCKED      0x04u  /* registered; no further override accepted */

/* Register a feature flag. name MUST start with "feature." or "experiment.".
 * rollout_percent in [0,100]: 0 = cohort disabled (use default/override only),
 * 100 = full rollout. Returns 0 on success, -1 on a bad name/namespace,
 * duplicate, full table, or out-of-range percent. */
int kernel_feature_register(const char *name, int default_enabled,
                            uint16_t flags, uint32_t rollout_percent,
                            uint8_t owner);

/* Resolve (and cache) whether a feature is enabled. Returns 1 if enabled, 0 if
 * disabled or the name is unknown. First call computes the resolution + audit
 * line; later calls return the cached value (deterministic for the boot). */
int kernel_feature_enabled(const char *name);

/* Register the built-in core features. Idempotent; call once at Phase 3. */
void kernel_features_register_core(void);

/* Number of registered features (for audit / dump). */
uint32_t kernel_feature_count(void);

/* Audit snapshot row for kernel_feature_dump(). */
typedef struct {
    char     name[FEATURE_NAME_CAP];
    uint8_t  default_enabled;
    uint8_t  resolved_state;    /* valid only when resolved != 0 */
    uint8_t  resolved;          /* 1 once kernel_feature_enabled() computed it */
    uint8_t  flags;
    uint8_t  rollout_percent;
    uint8_t  owner;
    uint8_t  source;            /* feature_source_t of the resolved value */
    uint8_t  _pad;
} feature_snapshot_t;

/* Provenance of a resolved feature value. */
typedef enum {
    FEATURE_SRC_DEFAULT  = 0,   /* registered default */
    FEATURE_SRC_CMDLINE  = 1,   /* feature.<name>= boot-arg override */
    FEATURE_SRC_COHORT   = 2,   /* staged-rollout cohort bucket */
    FEATURE_SRC_SB_GUARD = 3,   /* override refused by the Secure Boot guard */
} feature_source_t;

/* Copy up to max_rows snapshots into out; returns the number written. */
uint32_t kernel_feature_dump(feature_snapshot_t *out, uint32_t max_rows);

#endif /* KERNEL_FEATURE_H */
