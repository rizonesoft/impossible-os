/* ============================================================================
 * luid.h -- Locally Unique Identifier (LUID)
 *
 * A 64-bit opaque identifier guaranteed unique on this machine for the
 * lifetime of the boot.  Used to identify privileges, logon sessions,
 * and tokens.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* --- LUID type ----------------------------------------------------------- */

typedef struct {
    uint32_t LowPart;
    int32_t  HighPart;
} LUID;

/* --- Inline helpers ------------------------------------------------------ */

static inline int RtlEqualLuid(const LUID *a, const LUID *b)
{
    return a->LowPart == b->LowPart && a->HighPart == b->HighPart;
}

static inline int RtlIsZeroLuid(const LUID *l)
{
    return l->LowPart == 0 && l->HighPart == 0;
}

/* --- API ----------------------------------------------------------------- */

/*
 * RtlLuidFromValue -- split a 64-bit counter value into a LUID's LowPart /
 * (signed) HighPart, matching the Windows LUID representation. Pure helper;
 * exposed so the wrap-carry behavior is unit-testable without exhausting the
 * 32-bit range.
 */
LUID RtlLuidFromValue(uint64_t value);

/*
 * NtAllocateLocallyUniqueId -- return the next unique LUID.
 * Monotonically incrementing, SMP-safe (64-bit atomic counter that carries into
 * HighPart, so the identifier never wraps after ~4 billion allocations).
 */
LUID NtAllocateLocallyUniqueId(void);
