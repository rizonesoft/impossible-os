/* ============================================================================
 * cpu_security.c -- CPU security feature activation
 *
 * XREF: 02-kernel-core/TODO-17-kernel-security-hardening.md §1-§2
 * ============================================================================ */

#include "kernel/cpu_security.h"
#include "kernel/cpuid.h"
#include "kernel/cpuid_platform.h"
#include "kernel/msr.h"
#include "kernel/boot_init.h"
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

/* Check if the hypervisor supports guest CR4 SMEP/SMAP writes.
 * Hyper-V/WHPX: does NOT handle these VM exits (VP exit code 4).
 * KVM, VMware, VirtualBox: fully support guest SMEP/SMAP.
 * Bare metal: always supported (no VM exits). */
static int hv_supports_cr4_smep_smap(void)
{
    platform_id_t p = platform_detect();
    switch (p) {
        case PLATFORM_BARE_METAL:
            /* Per-process page tables exist for user tasks (TODO-06 §8),
             * but the KERNEL (PID 0) still runs on the boot PML4 which has
             * User bit on ALL 2 MiB pages (entry.asm flag 0x87). Enabling
             * SMEP here would fault because kernel code pages have User bit.
             * Need to fix boot PML4 to clear User on kernel pages first. */
            return 0;
        case PLATFORM_QEMU_KVM:
        case PLATFORM_QEMU_TCG:
        case PLATFORM_VMWARE:
        case PLATFORM_VIRTUALBOX:
            return 1;
        case PLATFORM_HYPERV:
            /* Hyper-V WHPX does not handle guest CR4.SMEP/SMAP VM exits.
             * The host enforces these via EPT/SLAT instead. */
            return 0;
        default:
            return 0;  /* unknown hypervisor -- be safe */
    }
}

void cpu_enable_smep(void)
{
    if (!cpu_has(CPU_FEATURE_SMEP))
        return;

    if (!hv_supports_cr4_smep_smap()) {
        klog(LOG_DEBUG, "cpu", "SMEP: skipped (Hyper-V enforces via EPT)");
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

    if (!hv_supports_cr4_smep_smap()) {
        klog(LOG_DEBUG, "cpu", "SMAP: skipped (Hyper-V enforces via EPT)");
        return;
    }

    uint64_t cr4 = read_cr4();
    if (!(cr4 & CR4_SMAP)) {
        write_cr4(cr4 | CR4_SMAP);
        klog(LOG_DEBUG, "cpu", "SMAP enabled (CR4.SMAP)");
    }
}

/* ---- User-space copy helpers (SMAP-safe) ---- */

int copy_from_user(void *dst, const void *user_src, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)user_src;
    uint32_t i;

    KERNEL_ACCESS_USER_BEGIN();
    for (i = 0; i < len; i++)
        d[i] = s[i];
    KERNEL_ACCESS_USER_END();

    return 0;
}

int copy_to_user(void *user_dst, const void *src, uint32_t len)
{
    uint8_t *d = (uint8_t *)user_dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;

    KERNEL_ACCESS_USER_BEGIN();
    for (i = 0; i < len; i++)
        d[i] = s[i];
    KERNEL_ACCESS_USER_END();

    return 0;
}

/* ---- Combined hardening call ---- */

void cpu_harden(void)
{
    cpu_enable_nx();
}

/* Enable SMEP/SMAP after page tables have been fixed (U/S cleared from
 * kernel pages).  Must be called AFTER vmm_apply_nx_policy(). */
void cpu_harden_post_pagetable(void)
{
    cpu_enable_smep();
    cpu_enable_smap();
}

/* Verify that CPU security features are actually active after enable.
 * Reads back EFER and CR4 and logs any discrepancies. */
void cpu_verify_hardening(void)
{
    uint64_t efer, cr4;

    POST16(0xD900);

    /* Verify NX (EFER.NXE, bit 11) */
    __asm__ volatile("rdmsr" : "=a"((uint32_t){0}), "=d"((uint32_t){0})
                     : "c"(0xC0000080));
    {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
        efer = ((uint64_t)hi << 32) | lo;
    }
    if (cpu_has(CPU_FEATURE_NX)) {
        if (efer & (1ULL << 11))
            klog(LOG_INFO, "cpu", "Verify: NX enabled (EFER.NXE set)");
        else
            klog(LOG_WARN, "cpu", "Verify: NX FAILED -- EFER.NXE not set after enable");
    }
    POST16(0xD901);

    /* Verify SMEP/SMAP (CR4 bits 20, 21) */
    cr4 = read_cr4();
    if (cpu_has(CPU_FEATURE_SMEP)) {
        if (cr4 & CR4_SMEP)
            klog(LOG_INFO, "cpu", "Verify: SMEP enabled (CR4.SMEP set)");
        else if (!hv_supports_cr4_smep_smap())
            klog(LOG_INFO, "cpu", "Verify: SMEP enforced via EPT (Hyper-V)");
        else
            klog(LOG_WARN, "cpu", "Verify: SMEP FAILED -- CR4.SMEP not set");
    }
    POST16(0xD902);

    if (cpu_has(CPU_FEATURE_SMAP)) {
        if (cr4 & CR4_SMAP)
            klog(LOG_INFO, "cpu", "Verify: SMAP enabled (CR4.SMAP set)");
        else if (!hv_supports_cr4_smep_smap())
            klog(LOG_INFO, "cpu", "Verify: SMAP enforced via EPT (Hyper-V)");
        else
            klog(LOG_WARN, "cpu", "Verify: SMAP FAILED -- CR4.SMAP not set");
    }
    POST16(0xD904);
}
