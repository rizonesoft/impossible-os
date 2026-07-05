/* ============================================================================
 * generic_mapping.h -- GENERIC_MAPPING type
 *
 * Maps the four GENERIC_* access rights (GENERIC_READ/WRITE/EXECUTE/ALL) to a
 * particular object type's specific + standard rights. This is the shared
 * Windows ABI type; the per-object-type instances (File, Process, Thread,
 * Token, Registry Key, ...) and RtlMapGenericMask are owned by the
 * SeAccessCheck access-decision engine.
 * ============================================================================ */

#pragma once

#include "kernel/nt/nt_types.h"    /* ACCESS_MASK */

typedef struct generic_mapping {
    ACCESS_MASK GenericRead;
    ACCESS_MASK GenericWrite;
    ACCESS_MASK GenericExecute;
    ACCESS_MASK GenericAll;
} GENERIC_MAPPING;

/* Windows ABI: four ACCESS_MASK (uint32) fields, 16 bytes. */
_Static_assert(sizeof(GENERIC_MAPPING) == 16,
    "GENERIC_MAPPING must be 16 bytes (4x ACCESS_MASK, Windows ABI)");
