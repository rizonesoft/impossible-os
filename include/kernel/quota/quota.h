/* ============================================================================
 * quota.h -- Kernel resource-accounting type registry
 *
 * The single authority for the taxonomy of chargeable kernel resources. Every
 * resource that a quota block can charge (section 2) has exactly one entry
 * here with an accounting unit, a default limit, the privilege that may exceed
 * it, and a human-readable name. Later sections (charge API, object/handle,
 * pool, registry/ALPC/notification, CPU/IO, syscalls) consume this table; none
 * of them may invent a resource type outside it.
 *
 * Design invariants:
 *   - The descriptor table is `static const`, so every field is available at
 *     link time: before any consumer can charge. `quota_register_types()` only
 *     VALIDATES the table and marks the registry ready; it does not build it.
 *     Validation is separate from the dump (`quota_types_dump`).
 *   - `quota_register_types()` returns `boot_result_t`; a malformed table
 *     (missing name, bad unit, count mismatch) returns BOOT_FATAL so boot halts
 *     rather than running with an inconsistent taxonomy.
 *   - Storage/volume-quota bytes are intentionally NOT a type here: they are
 *     provider-owned (IXFS per-volume, keyed by (volume, owner)), not a scalar
 *     central type. See the IXFS volume-quota provider for storage quota.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_init.h"        /* boot_result_t */
#include "kernel/security/luid.h"    /* LUID */

/* --- Resource types ------------------------------------------------------ *
 * Index into the descriptor table. APPEND new types immediately before
 * QUOTA_RESOURCE_TYPE_COUNT; never renumber (later sections key off the
 * enum value). */
typedef enum {
    QUOTA_RES_HANDLE = 0,        /* open handle-table entries        */
    QUOTA_RES_OBJECT_BODY,       /* Object Manager object bodies     */
    QUOTA_RES_NAMESPACE_ENTRY,   /* named object-directory entries   */
    QUOTA_RES_PAGED_POOL,        /* paged pool bytes                 */
    QUOTA_RES_NONPAGED_POOL,     /* nonpaged pool bytes              */
    QUOTA_RES_REGISTRY_BYTES,    /* registry key/value/data bytes    */
    QUOTA_RES_ALPC_MESSAGE,      /* queued ALPC messages             */
    QUOTA_RES_NOTIFICATION_STATE,/* notification state objects       */
    QUOTA_RES_TIMER,             /* timer objects                    */
    QUOTA_RES_THREAD,            /* threads                          */
    QUOTA_RES_PROCESS,           /* processes                        */
    QUOTA_RES_SECTION,           /* section (shared-memory) objects  */
    QUOTA_RES_MAPPED_VIEW,       /* mapped views of sections         */
    QUOTA_RES_CRASH_BUFFER,      /* retained crash-dump buffers      */
    QUOTA_RESOURCE_TYPE_COUNT
} quota_resource_type_t;

/* --- Accounting unit ----------------------------------------------------- */
typedef enum {
    QUOTA_UNIT_COUNT = 0,  /* discrete objects (handles, threads, timers, ...) */
    QUOTA_UNIT_BYTES       /* byte-denominated (pool, registry, payloads)      */
} quota_unit_t;

/* A default_limit of 0 means "no cap" (unlimited). Concrete numeric caps are a
 * policy decision owned by the kernel configuration layer; the charge-time
 * limit precedence (documented contract, enforced by the charge API) is:
 *   per-block explicit limit  >  kernel-config override  >  default_limit. */
#define QUOTA_LIMIT_UNLIMITED  0ULL

/* --- Descriptor ---------------------------------------------------------- *
 * Immutable per-type metadata. `name` is never NULL. */
typedef struct {
    const char   *name;               /* human-readable, never NULL           */
    LUID          override_privilege; /* privilege that may exceed the cap     */
    uint64_t      default_limit;      /* QUOTA_LIMIT_UNLIMITED (0) = no cap     */
    quota_unit_t  unit;
} quota_resource_desc_t;

/* --- Registry API -------------------------------------------------------- */

/* Validate the static descriptor table and mark the registry ready. Returns
 * BOOT_FATAL on a malformed table so boot halts. Call in Phase 2 BEFORE the
 * first quota consumer (Object Manager). Idempotent: a second call re-validates
 * and returns the same result. */
boot_result_t quota_register_types(void);

/* Descriptor for a type, or NULL if `type` is out of range. The table is const,
 * so reads are lock-free and SMP-safe once quota_register_types has run. */
const quota_resource_desc_t *quota_resource_desc(quota_resource_type_t type);

/* Human-readable name for a type, or "?" if out of range. */
const char *quota_resource_type_name(quota_resource_type_t type);

/* Number of registered resource types (== QUOTA_RESOURCE_TYPE_COUNT). */
uint32_t quota_resource_type_count(void);

/* Non-zero once quota_register_types has validated the table successfully. */
int quota_registry_ready(void);

/* Dump the registered type table (name / unit / limit) to the serial log.
 * Distinct from the per-block charge-state dump quota_dump() added by the
 * quota dashboard. */
void quota_types_dump(void);
