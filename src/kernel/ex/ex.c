/* ============================================================================
 * ex.c -- Executive Support Runtime bring-up (S1).
 *
 * Establishes the Ex* primitive layer's init point. The primitives themselves
 * (SLIST, rundown, callbacks, lookaside, fast refs, generic tables, locks,
 * run-once, work items, bugcheck callbacks, verifier) are added in S2-S14;
 * this file owns the boot hook and the layer-ready flag.
 * ============================================================================ */

#include "kernel/ex.h"
#include "kernel/klog.h"

boot_result_t ex_init(void)
{
    /* No allocation, no locks: bring-up adds no shared mutable state of its
     * own today. Future primitives register their static state here as they
     * are added. Readiness publication is owned by the subsystem oracle:
     * boot_phase2() calls kernel_subsystem_set_ready(SUBSYS_EX, ...) right
     * after this returns, which uses a release store readable cross-CPU. */
    klog(LOG_INFO, "ex", "Executive support runtime initialized");

    /* Bring up the Ex callback objects (\Callback\ namespace + built-ins).
     * Runs after ob_init (we are in Phase 2 post-OB) so ObpCallbackType and the
     * namespace root are available. */
    extern void ex_callback_init(void);
    ex_callback_init();

    return BOOT_OK;
}

bool ex_ready(void)
{
    /* Single source of truth: the subsystem readiness oracle (acquire load,
     * SMP-safe). APs are already online at this boot stage, so a duplicate
     * plain-bool flag would be a data race. */
    return kernel_subsystem_ready(SUBSYS_EX);
}
