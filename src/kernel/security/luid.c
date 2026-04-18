/* ============================================================================
 * luid.c -- LUID allocator
 *
 * Monotonically incrementing LUID counter for the security subsystem.
 * ============================================================================ */

#include "kernel/security/luid.h"
#include "kernel/atomic.h"

/* Start at 1000 to leave room for well-known privilege LUIDs (2–35) */
static atomic_t g_next_luid = ATOMIC_INIT(1000);

LUID NtAllocateLocallyUniqueId(void)
{
    LUID l;
    l.LowPart = (uint32_t)__sync_add_and_fetch(&g_next_luid.val, 1);
    l.HighPart = 0;
    return l;
}
