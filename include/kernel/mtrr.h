/*
 * mtrr.h -- Memory Type Range Register snapshot + parity audit (TODO-09-boot S8)
 *
 * MTRRs (Intel SDM Vol 3 11.11) set the default cache type for physical
 * address ranges. Both Windows and Linux keep them identical across all CPUs;
 * a divergent AP MTRR map can give the same PTE a different effective memory
 * type. This module snapshots a CPU's MTRR state into a compact struct so the
 * BSP can compare each AP against its own baseline and warn on divergence.
 *
 * Scope: snapshot + audit ONLY. Reprogramming a divergent AP requires an
 * all-CPU cache/MTRR rendezvous (SDM 11.11.8) that this kernel does not yet
 * have -- tracked in TODO-09-boot S8. MMIO cache correctness does NOT depend
 * on this audit: device MMIO is mapped UC via PAT index 3, and a UC PAT type
 * always wins over any MTRR type (SDM 11.5.2 Table 11-7), so MTRR divergence
 * cannot make UC MMIO cached. It only affects WC framebuffer performance.
 */
#ifndef KERNEL_MTRR_H
#define KERNEL_MTRR_H

#include "kernel/types.h"

/* Compact, comparable snapshot of a CPU's MTRR state. var_count + checksum
 * fold the variable PHYSBASE/PHYSMASK pairs and (when present) the fixed-range
 * MTRRs into one value; equal (cap, def_type, var_count, checksum) across two
 * CPUs means identical MTRR configuration. */
struct mtrr_snapshot {
    uint64_t cap;        /* IA32_MTRRCAP (0 when MTRR unsupported) */
    uint64_t def_type;   /* IA32_MTRR_DEF_TYPE */
    uint32_t var_count;  /* MTRRCAP.VCNT (variable range pairs captured) */
    uint32_t supported;  /* 1 = this CPU has MTRRs and they were read */
    uint64_t checksum;   /* FNV-1a over variable + fixed MTRR registers */
};

/* True if the CALLING CPU advertises MTRR support (CPUID.01h:EDX[12]). Uses a
 * local CPUID so an AP with feature skew is judged on its own capability, not
 * the BSP-global feature bitmap. */
int mtrr_supported(void);

/* Capture the calling CPU's MTRR state into *out. Safe on any CPU: gated on
 * the local CPUID bit and uses msr_try_read() for the capability probe so a
 * CPU without MTRRs records supported=0 instead of faulting. */
void mtrr_capture(struct mtrr_snapshot *out);

/* True if two snapshots describe identical MTRR configuration. */
int mtrr_snapshot_equal(const struct mtrr_snapshot *a,
                        const struct mtrr_snapshot *b);

#endif /* KERNEL_MTRR_H */
