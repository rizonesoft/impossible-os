/* ============================================================================
 * luid.c -- LUID allocator
 *
 * Monotonically incrementing LUID counter for the security subsystem. The
 * counter is 64-bit: once the low 32 bits wrap it carries into HighPart, so
 * the identifier stays unique for the boot instead of recycling after ~4
 * billion allocations.
 * ============================================================================ */

#include "kernel/security/luid.h"
#include "kernel/atomic.h"

/* Start at 1000 to leave room for well-known privilege LUIDs (2 through 35) */
static atomic64_t g_next_luid = ATOMIC64_INIT(1000);

LUID RtlLuidFromValue(uint64_t value)
{
    LUID l;
    l.LowPart  = (uint32_t)value;
    l.HighPart = (int32_t)(value >> 32);
    return l;
}

LUID NtAllocateLocallyUniqueId(void)
{
    /* atomic64_fetch_add returns the PREVIOUS value; the +1 yields the freshly
     * allocated post-increment value, preserving the prior monotonic semantics
     * (first allocation is 1001) while guaranteeing no two callers -- on any CPU
     * -- observe the same value. */
    return RtlLuidFromValue((uint64_t)atomic64_fetch_add(&g_next_luid, 1) + 1);
}
