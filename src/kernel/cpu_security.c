/* ============================================================================
 * cpu_security.c -- CPU security feature activation
 *
 * XREF: 02-kernel-core/TODO-17-kernel-security-hardening.md §1-§2
 * ============================================================================ */

#include "kernel/cpu_security.h"
#include "kernel/cpuid.h"
#include "kernel/cpuid_platform.h"
#include "kernel/msr.h"
#include "kernel/klog.h"

/* ---- CR4 bit definitions ---- */
#define CR4_SMEP  (1UL << 20)
#define CR4_SMAP  (1UL << 21)

static inline uint64_t read_cr4(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline void write_cr4(uint64_t val)
{
    __asm__ volatile ("mov %0, %%cr4" : : "r"(val));
}

/* ---- NX (No-Execute) via EFER.NXE ---- */

void cpu_enable_nx(void)
{
    if (!cpu_has(CPU_FEATURE_NX))
        return;

    uint64_t efer = msr_read(MSR_IA32_EFER);
    if (!(efer & EFER_NXE)) {
        msr_write(MSR_IA32_EFER, efer | EFER_NXE);
        klog(LOG_DEBUG, "cpu", "NX enabled (EFER.NXE)");
    }
}

/* ---- SMEP (Supervisor Mode Execution Prevention) via CR4.SMEP ---- */

void cpu_enable_smep(void)
{
    if (!cpu_has(CPU_FEATURE_SMEP))
        return;

    /* Skip on hypervisors — CR4 SMEP/SMAP writes cause VM exits on
     * WHPX/Hyper-V that the hypervisor doesn't handle. The host
     * enforces these protections at the EPT/NPT level instead. */
    if (platform_detect() != PLATFORM_BARE_METAL) {
        klog(LOG_DEBUG, "cpu", "SMEP: skipped (hypervisor enforces via EPT)");
        return;
    }

    uint64_t cr4 = read_cr4();
    if (!(cr4 & CR4_SMEP)) {
        write_cr4(cr4 | CR4_SMEP);
        klog(LOG_DEBUG, "cpu", "SMEP enabled (CR4.SMEP)");
    }
}

/* ---- SMAP (Supervisor Mode Access Prevention) via CR4.SMAP ---- */

void cpu_enable_smap(void)
{
    if (!cpu_has(CPU_FEATURE_SMAP))
        return;

    /* Skip on hypervisors — same reason as SMEP above */
    if (platform_detect() != PLATFORM_BARE_METAL) {
        klog(LOG_DEBUG, "cpu", "SMAP: skipped (hypervisor enforces via EPT)");
        return;
    }

    uint64_t cr4 = read_cr4();
    if (!(cr4 & CR4_SMAP)) {
        write_cr4(cr4 | CR4_SMAP);
        klog(LOG_DEBUG, "cpu", "SMAP enabled (CR4.SMAP)");
    }
}

/* ---- Combined hardening call ---- */

void cpu_harden(void)
{
    cpu_enable_nx();
    cpu_enable_smep();
    cpu_enable_smap();
}
