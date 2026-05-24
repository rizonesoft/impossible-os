/*
 * mtrr.c -- MTRR snapshot + parity audit helpers (TODO-09-boot S8)
 *
 * See mtrr.h for the design rationale. This module only READS MTRRs; it never
 * reprograms them. The BSP snapshots its baseline in cpu_record_bsp_profile()
 * and each AP captures its own in ap_cpu_harden(); ap_cpu_harden_log() (BSP
 * side) compares them and warns on divergence.
 */
#include "kernel/mtrr.h"
#include "kernel/msr.h"
#include "kernel/cpuid.h"

/* FNV-1a 64-bit constants. */
#define MTRR_FNV_OFFSET 1469598103934665603ULL
#define MTRR_FNV_PRIME  1099511628211ULL

static inline uint64_t fnv1a_step(uint64_t hash, uint64_t value)
{
    /* Mix the 8 bytes of `value` so PHYSBASE/PHYSMASK ordering matters. */
    for (uint32_t i = 0; i < 8; i++) {
        hash ^= (value >> (i * 8)) & 0xFFu;
        hash *= MTRR_FNV_PRIME;
    }
    return hash;
}

int mtrr_supported(void)
{
    uint32_t eax, ebx, ecx, edx;
    /* Local CPUID -- executes on whichever CPU calls this, so an AP is judged
     * on its own capability rather than the BSP-global feature bitmap. MTRR is
     * CPUID.01h:EDX[12]. */
    cpuid_raw(0x01, 0, &eax, &ebx, &ecx, &edx);
    return (edx & (1u << 12)) != 0;
}

/* Ceiling on variable MTRR pairs. The contiguous variable-MTRR MSR block runs
 * from IA32_MTRR_PHYSBASE0 (0x200) up to but NOT including the fixed-range
 * block at IA32_MTRR_FIX64K_00000 (0x250) -- so at most (0x250-0x200)/2 = 40
 * PHYSBASE/PHYSMASK pairs fit before the indexed reads would collide with the
 * fixed MTRRs and IA32_PAT (0x277). Real CPUs expose <= ~10; a VCNT above this
 * ceiling is a malformed/virtualized report and the snapshot is abandoned
 * rather than folding non-variable MSR state into the parity checksum. */
#define MTRR_VAR_SANE_MAX \
    ((MSR_IA32_MTRR_FIX64K_00000 - MSR_IA32_MTRR_PHYSBASE0) / 2u)

/* The highest MSR the variable loop can read at the ceiling must stay strictly
 * below the fixed-range block, so an inflated VCNT can never read a fixed MTRR
 * or PAT as if it were a variable pair. */
_Static_assert(MSR_IA32_MTRR_PHYSBASE0 + 2u * MTRR_VAR_SANE_MAX
                   <= MSR_IA32_MTRR_FIX64K_00000,
               "variable MTRR read window must not reach the fixed-MTRR block");

void mtrr_capture(struct mtrr_snapshot *out)
{
    uint64_t cap = 0, def_type = 0, val = 0;
    uint64_t checksum = MTRR_FNV_OFFSET;
    uint32_t vcnt, i;

    if (!out)
        return;

    out->cap = 0;
    out->def_type = 0;
    out->var_count = 0;
    out->supported = 0;
    out->checksum = 0;

    if (!mtrr_supported())
        return;

    /* EVERY MTRR read goes through msr_try_read(): CPUID said MTRR is present,
     * but on a pre-online AP (IRQs masked, not yet published) a hypervisor that
     * advertises the bit while leaving any MTRR MSR un-backed -- or reports an
     * inflated VCNT -- would #GP on a raw rdmsr and hang SMP bringup. CPUID is
     * the primary gate (CLAUDE.md); msr_try_read() is the net under it. On any
     * failure we abandon the snapshot (supported stays 0) rather than ship a
     * partial one that could read the wrong answer as "synced". */
    if (msr_try_read(MSR_IA32_MTRRCAP, &cap) != 0)
        return;
    if (msr_try_read(MSR_IA32_MTRR_DEF_TYPE, &def_type) != 0)
        return;

    /* Reject an implausible VCNT before issuing any indexed read -- a value
     * past the sane ceiling means the MTRRCAP report is not trustworthy. */
    vcnt = (uint32_t)(cap & MTRRCAP_VCNT_MASK);
    if (vcnt > MTRR_VAR_SANE_MAX)
        return;

    checksum = fnv1a_step(checksum, def_type);
    for (i = 0; i < vcnt; i++) {
        if (msr_try_read(MSR_IA32_MTRR_PHYSBASE0 + (uint32_t)(i * 2), &val) != 0)
            return;
        checksum = fnv1a_step(checksum, val);
        if (msr_try_read(MSR_IA32_MTRR_PHYSBASE0 + (uint32_t)(i * 2) + 1, &val) != 0)
            return;
        checksum = fnv1a_step(checksum, val);
    }

    /* Fixed-range MTRRs only exist when MTRRCAP.FIX is set. */
    if (cap & MTRRCAP_FIX) {
        static const uint32_t fixed[] = {
            MSR_IA32_MTRR_FIX64K_00000,
            MSR_IA32_MTRR_FIX16K_80000,
            MSR_IA32_MTRR_FIX16K_A0000,
            MSR_IA32_MTRR_FIX4K_C0000 + 0, MSR_IA32_MTRR_FIX4K_C0000 + 1,
            MSR_IA32_MTRR_FIX4K_C0000 + 2, MSR_IA32_MTRR_FIX4K_C0000 + 3,
            MSR_IA32_MTRR_FIX4K_C0000 + 4, MSR_IA32_MTRR_FIX4K_C0000 + 5,
            MSR_IA32_MTRR_FIX4K_C0000 + 6, MSR_IA32_MTRR_FIX4K_C0000 + 7,
        };
        for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
            if (msr_try_read(fixed[i], &val) != 0)
                return;
            checksum = fnv1a_step(checksum, val);
        }
    }

    /* All reads succeeded -- publish the complete, trusted snapshot. */
    out->cap = cap;
    out->def_type = def_type;
    out->var_count = vcnt;
    out->supported = 1;
    out->checksum = checksum;
}

int mtrr_snapshot_equal(const struct mtrr_snapshot *a,
                        const struct mtrr_snapshot *b)
{
    if (!a || !b)
        return 0;
    return a->supported == b->supported &&
           a->cap == b->cap &&
           a->def_type == b->def_type &&
           a->var_count == b->var_count &&
           a->checksum == b->checksum;
}
