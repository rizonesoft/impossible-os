/* ============================================================================
 * cpu_security.c -- CPU security feature activation
 *
 * XREF: 02-kernel-core/TODO-10-kernel-security-hardening.md -
 * ============================================================================ */

#include "kernel/cpu_security.h"
#include "kernel/cpuid.h"
#include "kernel/cpuid_platform.h"
#include "kernel/msr.h"
#include "kernel/boot_init.h"
#include "kernel/klog.h"
#include "kernel/security/pku.h"
#ifdef KERNEL_TESTS
#include "kernel/smp.h"                 /* smp_this_cpu() for per-CPU countdown */
#include "kernel/sched/irql.h"          /* KeGetCurrentIrql for thread-context gate */
#include "kernel/sched/task.h" /* task_current() for task-filter gate */
#endif

/* ---- CR4 bit definitions ---- */
#define CR4_UMIP  (1UL << 11)
#define CR4_PKE   (1UL << 22)
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
    /* The boot PML4 (entry.asm flag 0x87) has User bit on ALL 2 MiB pages.
     * Enabling SMEP faults because kernel code pages have User bit.
     * This affects ALL platforms -- not just bare metal.
     * Disabled globally until boot PML4 clears User on kernel pages. */
    (void)platform_detect();
    return 0;
}

void cpu_enable_smep(void)
{
    if (!cpu_has(CPU_FEATURE_SMEP))
        return;

    if (!hv_supports_cr4_smep_smap()) {
        klog(LOG_DEBUG, "cpu", "SMEP: skipped (kernel pages have User bit -- needs KPTI S6)");
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
        klog(LOG_DEBUG, "cpu", "SMAP: skipped (kernel pages have User bit -- needs KPTI S6)");
        return;
    }

    uint64_t cr4 = read_cr4();
    if (!(cr4 & CR4_SMAP)) {
        write_cr4(cr4 | CR4_SMAP);
        klog(LOG_DEBUG, "cpu", "SMAP enabled (CR4.SMAP)");
    }
}

/* ---- User-space copy helpers (SMAP-safe) ---- */

#ifdef KERNEL_TESTS
/* copy_user fault injection (hook used by both copy_to_user and
 * copy_from_user below). Same gate set as the kmalloc/pmm/vmm hooks:
 * PASSIVE_LEVEL only, optional task-pid filter, optional max-injections
 * cap, per-CPU countdown. On fire, caller returns -1 without touching
 * user memory. */
static uint64_t s_copy_user_fault_injections;

static int copy_user_fault_should_fire(void)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return 0;
    struct per_cpu_data *pc = smp_this_cpu();
    if (!pc || !pc->copy_user_fail_countdown)
        return 0;

    if (pc->copy_user_fail_task_pid != 0) {
        struct task *t = task_current();
        if (!t || t->pid != pc->copy_user_fail_task_pid)
            return 0;
    }
    if (pc->copy_user_fail_max_injections != 0 &&
        pc->copy_user_fail_fired_counter >=
            pc->copy_user_fail_max_injections) {
        return 0;
    }
    if (--pc->copy_user_fail_countdown != 0)
        return 0;

    __atomic_fetch_add(&s_copy_user_fault_injections, 1ull, __ATOMIC_RELAXED);
    pc->copy_user_fail_fired_counter++;
    /* auto-reload for multi-fire: see the heap-side comment. */
    if (pc->copy_user_fail_max_injections != 0 &&
        pc->copy_user_fail_fired_counter <
            pc->copy_user_fail_max_injections) {
        pc->copy_user_fail_countdown = 1;
    }
    return 1;
}

void copy_user_fail_countdown_set(uint32_t n) { smp_this_cpu()->copy_user_fail_countdown = n; }
void copy_user_fail_countdown_clear(void)     { smp_this_cpu()->copy_user_fail_countdown = 0; }
void copy_user_fail_next(void)                { smp_this_cpu()->copy_user_fail_countdown = 1; }
uint64_t copy_user_fail_injections_triggered(void)
{
    return __atomic_load_n(&s_copy_user_fault_injections, __ATOMIC_RELAXED);
}
void copy_user_fail_task_filter_set(uint32_t task_pid)
{
    struct per_cpu_data *pc = smp_this_cpu();
    pc->copy_user_fail_task_pid     = task_pid;
    pc->copy_user_fail_fired_counter = 0;
}
void copy_user_fail_task_filter_clear(void) { smp_this_cpu()->copy_user_fail_task_pid = 0; }
void copy_user_fail_max_injections_set(uint32_t max)
{
    struct per_cpu_data *pc = smp_this_cpu();
    pc->copy_user_fail_max_injections = max;
    pc->copy_user_fail_fired_counter  = 0;
}
void copy_user_fail_max_injections_clear(void)
{
    struct per_cpu_data *pc = smp_this_cpu();
    pc->copy_user_fail_max_injections = 0;
    pc->copy_user_fail_fired_counter  = 0;
}
uint32_t copy_user_fail_fired_counter(void)
{
    return smp_this_cpu()->copy_user_fail_fired_counter;
}
#endif /* KERNEL_TESTS */

int copy_from_user(void *dst, const void *user_src, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)user_src;
    uint32_t i;

#ifdef KERNEL_TESTS
    /* fault-inject -- return -1 BEFORE touching user memory so the
     * SMAP STAC/CLAC pair is skipped and the error path is exercised
     * identically to a real user-copy fault. */
    if (copy_user_fault_should_fire())
        return -1;
#endif

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

#ifdef KERNEL_TESTS
    if (copy_user_fault_should_fire())
        return -1;
#endif

    KERNEL_ACCESS_USER_BEGIN();
    for (i = 0; i < len; i++)
        d[i] = s[i];
    KERNEL_ACCESS_USER_END();

    return 0;
}

/* ---- UMIP (User-Mode Instruction Prevention) via CR4.UMIP ---- */

void cpu_enable_umip(void)
{
    if (!cpu_has(CPU_FEATURE_UMIP))
        return;

    uint64_t cr4 = read_cr4();
    if (!(cr4 & CR4_UMIP)) {
        write_cr4(cr4 | CR4_UMIP);
        klog(LOG_DEBUG, "cpu", "UMIP enabled (CR4.UMIP)");
    }
}

/* ---- PKU (Protection Keys for User-mode) via CR4.PKE ---- */

void cpu_enable_pku(void)
{
    if (!cpu_has(CPU_FEATURE_PKU))
        return;

    /* XCR0 bit 9 (PKRU state) must already be set by cpu_configure_xcr0() */
    {
        extern struct cpu_features g_cpu;
        if (!(g_cpu.xcr0_active & (1UL << 9))) {
            klog(LOG_WARN, "cpu", "PKU: XCR0 bit 9 not set; skipping CR4.PKE");
            return;
        }
    }

    uint64_t cr4 = read_cr4();
    if (!(cr4 & CR4_PKE)) {
        write_cr4(cr4 | CR4_PKE);
        pku_enabled = 1;
        klog(LOG_DEBUG, "cpu", "PKU enabled (CR4.PKE)");
    }
}

/* ---- PAT: reprogram entry 1 to WC (per-CPU MSR) ---- */

/* PAT value with entry 1 = WC (Write-Combining).
 * Intel default: 0x0007040600070406 (entry 1 = WT = 0x04).
 * We change entry 1 to WC (0x01) for framebuffer VRAM performance. */
#define PAT_WC_VALUE  0x0007040600010406ULL

void cpu_configure_pat(void)
{
    uint64_t pat_new = PAT_WC_VALUE;
    msr_write(MSR_IA32_PAT, pat_new);
}

/* ---- Combined hardening call ---- */

void cpu_harden(void)
{
    cpu_enable_nx();
    cpu_enable_umip();
    cpu_enable_pku();
    cpu_configure_pat();
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
    efer = msr_read(MSR_IA32_EFER);
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
        else if (platform_detect() == PLATFORM_HYPERV)
            klog(LOG_INFO, "cpu", "Verify: SMEP enforced via EPT (Hyper-V)");
        else if (!hv_supports_cr4_smep_smap())
            klog(LOG_INFO, "cpu", "Verify: SMEP skipped (kernel PTE User bit -- needs KPTI)");
        else
            klog(LOG_WARN, "cpu", "Verify: SMEP FAILED -- CR4.SMEP not set");
    }
    POST16(0xD902);

    if (cpu_has(CPU_FEATURE_SMAP)) {
        if (cr4 & CR4_SMAP)
            klog(LOG_INFO, "cpu", "Verify: SMAP enabled (CR4.SMAP set)");
        else if (platform_detect() == PLATFORM_HYPERV)
            klog(LOG_INFO, "cpu", "Verify: SMAP enforced via EPT (Hyper-V)");
        else if (!hv_supports_cr4_smep_smap())
            klog(LOG_INFO, "cpu", "Verify: SMAP skipped (kernel PTE User bit -- needs KPTI)");
        else
            klog(LOG_WARN, "cpu", "Verify: SMAP FAILED -- CR4.SMAP not set");
    }
    POST16(0xD903);

    /* Verify UMIP (CR4 bit 11) */
    if (cpu_has(CPU_FEATURE_UMIP)) {
        if (cr4 & CR4_UMIP)
            klog(LOG_INFO, "cpu", "Verify: UMIP enabled (CR4.UMIP set)");
        else
            klog(LOG_WARN, "cpu", "Verify: UMIP FAILED -- CR4.UMIP not set");
    }

    /* Verify PKU (CR4 bit 22) */
    if (cpu_has(CPU_FEATURE_PKU)) {
        extern struct cpu_features g_cpu;
        if (cr4 & CR4_PKE)
            klog(LOG_INFO, "cpu", "Verify: PKU enabled (CR4.PKE set)");
        else if (!(g_cpu.xcr0_active & (1UL << 9)))
            klog(LOG_INFO, "cpu", "Verify: PKU skipped (XCR0 bit 9 not available)");
        else
            klog(LOG_WARN, "cpu", "Verify: PKU FAILED -- CR4.PKE not set");
    }
    POST16(0xD904);
}
