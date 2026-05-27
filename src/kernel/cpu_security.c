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
#include "kernel/smp.h"                 /* per_cpu_data, smp_get_cpu() for AP hardening */
#include "kernel/bugcheck.h"            /* KeBugCheckEx for AP feature validation (S6) */
#include "kernel/topology.h"            /* CORE_TYPE_* for AP core-type probe (S6) */
#include "kernel/cpu_regs.h"            /* CR0/CR4 control-register bit defines (CR pinning) */
#include "kernel/mtrr.h"                /* MTRR snapshot + parity audit (S8) */
#include "registry.h"                   /* HKLM\HARDWARE\CPU registry exposure (S9 audit) */

extern int snprintf(char *buf, size_t size, const char *fmt, ...);  /* per-CPU subkey build (S9) */
#ifdef KERNEL_TESTS
#include "kernel/sched/irql.h"          /* KeGetCurrentIrql for thread-context gate */
#include "kernel/sched/task.h" /* task_current() for task-filter gate */
#endif

/* CR0/CR4 bit definitions: single source of truth in kernel/cpu_regs.h
 * (CR0/CR4 safety-bit pinning resolved the former per-file duplication). */

/* CR4 bits that MUST be identical on every CPU. ap_cpu_harden() uses this
 * mask only for WARN-ONLY verification of an AP against the BSP today (it
 * does NOT force-OR the mask -- blindly setting an architectural CR4 bit
 * before AP feature validation could #GP on a feature-skewed AP). The
 * force-after-validation of any missing bit is owned by the AP feature
 * consistency validation, which knows each AP can take the bit. The BSP and
 * APs share one page table / CR3, so a bit safe on the BSP is safe on an
 * AP once validation confirms the AP supports it. */
#define CR4_UNIFORM_MASK \
    (CR4_OSXSAVE | CR4_UMIP | CR4_SMEP | CR4_SMAP | \
     CR4_PCIDE | CR4_PKE | CR4_FSGSBASE | CR4_CET)

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

static inline uint64_t read_cr0(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline void write_cr0(uint64_t val)
{
    __asm__ volatile ("mov %0, %%cr0" : : "r"(val));
}

/* ---- XCR0 helpers ---- */

static inline uint64_t xcr0_read(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | lo;
}

static inline void xcr0_write(uint64_t mask)
{
    __asm__ volatile ("xsetbv"
                      :: "a"((uint32_t)mask), "d"((uint32_t)(mask >> 32)), "c"(0));
}

/* Read this CPU's live XCR0, or 0 if XSAVE/OSXSAVE is not active here. Safe
 * on any CPU: xgetbv only executes when CR4.OSXSAVE is set on THIS core, so
 * it never traps on a core that has not enabled XSAVE (unlike gating on the
 * BSP-global cpu_has(XSAVE)). */
static uint64_t xcr0_read_safe(void)
{
    if (!(read_cr4() & CR4_OSXSAVE))
        return 0;
    return xcr0_read();
}

/* While > 0, the per-feature cpu_enable_* log lines are suppressed. Each AP
 * raises this around its cpu_harden()/cpu_harden_post_pagetable() calls so the
 * AP does NO serial output before it is marked online (klog -> serial_write
 * busy-waits on the UART with IRQs masked, which could delay the AP online
 * signal or leave it counted-online-but-not-IPI-ready). The BSP emits the
 * authoritative per-AP audit afterward via ap_cpu_harden_log().
 *
 * A NESTING COUNTER, not a boolean: the smp_init launch loop is NOT strictly
 * serialized on the timeout path (a late AP that missed its 100ms+50ms window
 * can still be inside ap_cpu_harden() when the next AP starts). Each AP's own
 * atomic increment keeps the depth >= 1 for the whole duration of its own
 * cpu_harden(), so an overlapping AP's decrement can never drop it to 0 mid-
 * hardening -- every AP stays quiet until it exits. The BSP runs cpu_harden()
 * only in Phase 0 with depth 0, so BSP hardening still logs normally. */
static int s_harden_quiet_depth;
#define HARDEN_KLOG(...) \
    do { if (__atomic_load_n(&s_harden_quiet_depth, __ATOMIC_RELAXED) == 0) \
             klog(__VA_ARGS__); } while (0)

/* ---- NX (No-Execute) via EFER.NXE ---- */

void cpu_enable_nx(void)
{
    if (!cpu_has(CPU_FEATURE_NX))
        return;

    uint64_t efer = msr_read(MSR_IA32_EFER);
    if (!(efer & EFER_NXE)) {
        msr_write(MSR_IA32_EFER, efer | EFER_NXE);
        HARDEN_KLOG(LOG_DEBUG, "cpu", "NX enabled (EFER.NXE)");
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
        HARDEN_KLOG(LOG_DEBUG, "cpu", "SMEP: skipped (kernel pages have User bit -- needs KPTI S6)");
        return;
    }

    uint64_t cr4 = read_cr4();
    if (!(cr4 & CR4_SMEP)) {
        write_cr4(cr4 | CR4_SMEP);
        HARDEN_KLOG(LOG_DEBUG, "cpu", "SMEP enabled (CR4.SMEP)");
    }
}

/* ---- SMAP (Supervisor Mode Access Prevention) via CR4.SMAP ---- */

void cpu_enable_smap(void)
{
    if (!cpu_has(CPU_FEATURE_SMAP))
        return;

    if (!hv_supports_cr4_smep_smap()) {
        HARDEN_KLOG(LOG_DEBUG, "cpu", "SMAP: skipped (kernel pages have User bit -- needs KPTI S6)");
        return;
    }

    uint64_t cr4 = read_cr4();
    if (!(cr4 & CR4_SMAP)) {
        write_cr4(cr4 | CR4_SMAP);
        HARDEN_KLOG(LOG_DEBUG, "cpu", "SMAP enabled (CR4.SMAP)");
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
        HARDEN_KLOG(LOG_DEBUG, "cpu", "UMIP enabled (CR4.UMIP)");
    }
}

/* ---- PKU (Protection Keys for User-mode) via CR4.PKE ---- */

void cpu_enable_pku(void)
{
    if (!cpu_has(CPU_FEATURE_PKU))
        return;

    /* XCR0 bit 9 (PKRU state) must be active on THIS CPU before CR4.PKE.
     * Read the live XCR0, not the BSP-global g_cpu.xcr0_active -- on an AP
     * whose XCR0 was intersected down to a narrower mask, the global would
     * lie and we would set CR4.PKE without the PKRU xstate enabled here. */
    if (!(xcr0_read_safe() & (1UL << 9))) {
        HARDEN_KLOG(LOG_WARN, "cpu", "PKU: XCR0 bit 9 not set on this CPU; skipping CR4.PKE");
        return;
    }

    uint64_t cr4 = read_cr4();
    if (!(cr4 & CR4_PKE)) {
        write_cr4(cr4 | CR4_PKE);
        pku_enabled = 1;
        HARDEN_KLOG(LOG_DEBUG, "cpu", "PKU enabled (CR4.PKE)");
    }
}

/* ---- PAT: reprogram entry 1 to WC (per-CPU MSR) ---- */

/* PAT value: Intel power-on default with ONLY entry 1 changed to WC.
 * Intel default = 0x0007040600070406. Byte layout (PA0 is the low byte):
 *   PA0=0x06 WB | PA1=0x01 WC | PA2=0x07 UC- | PA3=0x00 UC(strong)
 *   PA4=0x06 WB | PA5=0x04 WT | PA6=0x07 UC- | PA7=0x00 UC(strong)
 * Index selection from the PTE PAT/PCD/PWT bits:
 *   PWT only (vmm_map_mmio_wc, PAGE_WRITECOMBINE) -> index 1 = WC
 *   PCD+PWT  (vmm_map_mmio_uc)                    -> index 3 = UC(strong)
 *   PCD only (PAGE_NOCACHE)                       -> index 2 = UC-
 * Prior value 0x0007040600010406 set WC at PA2 (index 2), NOT PA1, so
 * framebuffer/PAGE_WRITECOMBINE silently got WT and PAGE_NOCACHE got WC
 * (TODO-09-boot S8 decode-bug fix 2026-05-24). */
#define PAT_WC_VALUE  0x0007040600070106ULL

/* Layer-1 defense: pin the load-bearing entries at compile time so the decode
 * bug can never recur silently (the unit test is Layer 3). */
_Static_assert(((PAT_WC_VALUE >> 8)  & 0xFF) == 0x01, "PAT index 1 must be WC (framebuffer/PAGE_WRITECOMBINE)");
_Static_assert(((PAT_WC_VALUE >> 16) & 0xFF) == 0x07, "PAT index 2 must be UC- (PAGE_NOCACHE)");
_Static_assert(((PAT_WC_VALUE >> 24) & 0xFF) == 0x00, "PAT index 3 must be UC-strong (vmm_map_mmio_uc)");

void cpu_configure_pat(void)
{
    uint64_t pat_new = PAT_WC_VALUE;
    msr_write(MSR_IA32_PAT, pat_new);
}

/* ---- XSAVE/XCR0 Phase 1 finalize (BSP) -- TODO-09-boot S5 ----
 *
 * The XCR0 base mask was already programmed in Phase 0 by cpu_configure_xcr0()
 * (cpuid_init), because Phase-0 simd_enable_avx() and pku_init() require it,
 * and per-thread XSAVE areas come from pmm_alloc_contiguous() rather than
 * VMM/TEB pages -- so XSAVE has no real VMM dependency to wait on. This
 * finalize runs in Phase 1 AFTER simd_enable_avx512()'s throttle guard and
 * just records the FINAL state. It intentionally does NOT re-run XSETBV: a
 * recompute would re-enable any AVX-512 xstate the throttle guard cleared. */
boot_result_t cpu_xsave_enable(void)
{
    extern struct cpu_features g_cpu;
    uint64_t cr4, xcr0;

    /* BOOT_DEGRADED (not BOOT_OK) for the unsupported skip: the caller marks
     * SUBSYS_XSAVE ready (the step ran) but does NOT emit the *_ENABLED
     * milestone, so the progress/POST stream never claims XSAVE was activated
     * on a CPU that lacks it. */
    if (!cpu_has(CPU_FEATURE_XSAVE)) {
        HARDEN_KLOG(LOG_INFO, "cpu", "[Phase1] XSAVE: not supported, skipped");
        return BOOT_DEGRADED;
    }

    /* Re-assert CR4.OSXSAVE idempotently (defensive against a later CR4 op
     * that might have cleared it); never touch XCR0 itself here. */
    cr4 = read_cr4();
    if (!(cr4 & CR4_OSXSAVE))
        write_cr4(cr4 | CR4_OSXSAVE);

    xcr0 = xcr0_read_safe();
    HARDEN_KLOG(LOG_INFO, "cpu",
                "[Phase1] XSAVE enabled (area=%u bytes, mask=0x%lx)",
                (uint64_t)g_cpu.xsave_size_max, xcr0);
    return BOOT_OK;
}

/* ---- Combined hardening call ---- */

void cpu_harden(void)
{
    cpu_enable_nx();
    cpu_enable_umip();
    cpu_enable_pku();
    /* PAT is NOT programmed here: it is owned by a single authoritative write
     * per CPU (TODO-09-boot S8). The BSP programs PAT in boot_phase0 after the
     * page-table takeover (boot_hw.c, post-CR3-reload); each AP programs it
     * exactly once via the MSR-profile replay in ap_cpu_harden(). Writing it
     * here too would double the AP-side WRMSR and could leave a transient
     * constant-vs-BSP mismatch before the authoritative profile write. */
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

/* Single consolidated activation-state line for the BSP Phase 0 sequence.
 * Owner: TODO-09 cpu-boot-sequencing activation-order section. Do NOT call
 * from APs -- the `[Phase0]` label would mis-attribute AP register state. */
void cpu_security_log_state(const char *phase_label)
{
    uint64_t efer = msr_read(MSR_IA32_EFER);
    uint64_t cr4  = read_cr4();
    unsigned nx   = cpu_has(CPU_FEATURE_NX)   ? ((efer >> 11) & 1u) : 0u;
    unsigned umip = cpu_has(CPU_FEATURE_UMIP) ? ((cr4  >> 11) & 1u) : 0u;
    unsigned pku  = cpu_has(CPU_FEATURE_PKU)  ? ((cr4  >> 22) & 1u) : 0u;
    unsigned smep = cpu_has(CPU_FEATURE_SMEP) ? ((cr4  >> 20) & 1u) : 0u;
    unsigned smap = cpu_has(CPU_FEATURE_SMAP) ? ((cr4  >> 21) & 1u) : 0u;

    klog(LOG_INFO, "cpu",
         "[Phase0] CPU security %s: EFER=0x%lx CR4=0x%lx NX=%u UMIP=%u PKU=%u SMEP=%u SMAP=%u",
         phase_label, efer, cr4,
         (uint64_t)nx, (uint64_t)umip, (uint64_t)pku, (uint64_t)smep, (uint64_t)smap);
}

/* ============================================================================
 * AP CPU hardening -- replicate the BSP's hardened state onto every AP.
 *
 * An AP that boots without the BSP's EFER/CR4/MSR state is a privilege-bypass
 * vector (NX absent, wrong PAT cache type on shared MMIO, etc.). Rather than
 * re-deriving each AP's state from CPUID (which drifts across hybrid P/E cores
 * and microcode revisions, and silently misses every new boot-time MSR), the
 * BSP records the exact values it programmed and each AP replays them.
 * ============================================================================ */

/* ---- Per-CPU MSR replay profile ----
 *
 * cpu_record_bsp_profile() fills `value` for non-per-CPU entries from the
 * BSP's live MSRs before SMP bringup; ap_apply_msr_profile() replays them on
 * each AP. Written once on the BSP (pre-SIPI), then read-only during AP
 * startup, so no lock is needed -- the existing smp_mb() before each SIPI in
 * smp_init() is the release barrier pairing the fill with AP launch.
 *
 * SPEC_CTRL / CET MSR entries are appended when the kernel-security-hardening
 * Spectre and CET setters ship (they own the values). */
#define MSR_PROFILE_ALWAYS  0xFFFFFFFFu   /* feature gate: always apply */

struct msr_profile_entry {
    uint32_t    msr;        /* MSR index */
    uint64_t    value;      /* BSP value, filled at record time; ignored if per_cpu */
    const char *name;       /* for the audit log */
    uint32_t    feature;    /* CPU_FEATURE_* gate, or MSR_PROFILE_ALWAYS */
    uint8_t     per_cpu;    /* 1 = value is per-CPU: write cpu_id, not BSP value */
};

static struct msr_profile_entry s_bsp_msr_profile[] = {
    { MSR_IA32_PAT,     0, "PAT",     MSR_PROFILE_ALWAYS, 0 },
    { MSR_IA32_TSC_AUX, 0, "TSC_AUX", CPU_FEATURE_RDTSCP, 1 },
};
#define MSR_PROFILE_COUNT (sizeof(s_bsp_msr_profile) / sizeof(s_bsp_msr_profile[0]))

/* BSP baseline, captured once by cpu_record_bsp_profile(), read by every AP. */
static uint64_t s_bsp_efer;
static uint64_t s_bsp_cr4;
static uint64_t s_bsp_pat;
static uint64_t s_bsp_xcr0;
static uint64_t s_bsp_required_cr4;   /* s_bsp_cr4 & CR4_UNIFORM_MASK */
static int      s_bsp_profile_ready;

/* BSP MTRR baseline (TODO-09-boot S8). Captured once in cpu_record_bsp_profile();
 * each AP is compared against it in ap_cpu_harden_log() (warn-only audit). */
static struct mtrr_snapshot s_bsp_mtrr;

/* AP feature consistency (TODO-09-boot S6). Global intersection = BSP & every
 * online AP within CPU_FEATURES_AP_PROBE_MASK; 0 until cpu_features_finalize_
 * global() publishes it BSP-side after the online acquire pass. */
static uint64_t s_global_feature_mask;

/* AP feature-validation fault hand-off (TODO-09-boot S6). An AP that fails the
 * required/vendor/Long-Mode gate cannot bug-check itself (the panic path uses
 * BSP-global XCR0/SIMD that #GP/#UD on a skewed AP), so it records the fault
 * here and halts; the BSP raises the 0x3E bug-check in
 * cpu_features_check_ap_faults() where the panic path is safe. */
static volatile uint32_t s_ap_fault_cpu;     /* 0 = none, else cpu_id + 1 */
static volatile uint64_t s_ap_fault_feat;    /* failing AP's probed mask */
static volatile uint32_t s_ap_fault_reason;  /* 1 = vendor, 2 = Long Mode, 3 = required */

void cpu_record_bsp_profile(void)
{
    uint32_t i;
    struct per_cpu_data *bsp;

    s_bsp_efer = msr_read(MSR_IA32_EFER);
    s_bsp_cr4  = read_cr4();
    s_bsp_pat  = msr_read(MSR_IA32_PAT);
    s_bsp_xcr0 = cpu_has(CPU_FEATURE_XSAVE) ? xcr0_read() : 0;
    s_bsp_required_cr4 = s_bsp_cr4 & CR4_UNIFORM_MASK;

    /* Freeze the replicated MSR values from the BSP's live MSRs. Per-CPU
     * entries (TSC_AUX) keep value 0 -- their value is computed on the AP. */
    for (i = 0; i < MSR_PROFILE_COUNT; i++) {
        if (s_bsp_msr_profile[i].per_cpu)
            continue;
        s_bsp_msr_profile[i].value = msr_read(s_bsp_msr_profile[i].msr);
    }

    /* MTRR baseline (S8): snapshot the BSP's MTRR state for AP parity audit. */
    mtrr_capture(&s_bsp_mtrr);

    /* Snapshot the BSP's own block (cpu_id 0) for the register audit trail. */
    bsp = smp_get_cpu(0);
    if (bsp) {
        bsp->efer_at_boot = s_bsp_efer;
        bsp->cr4_at_boot  = s_bsp_cr4;
        bsp->pat_at_boot  = s_bsp_pat;
        bsp->xcr0_at_boot = s_bsp_xcr0;
        bsp->tsc_aux      = 0;
        bsp->mtrr_cap       = s_bsp_mtrr.cap;
        bsp->mtrr_def_type  = s_bsp_mtrr.def_type;
        bsp->mtrr_checksum  = s_bsp_mtrr.checksum;
        bsp->mtrr_var_count = s_bsp_mtrr.var_count;
        bsp->mtrr_supported = s_bsp_mtrr.supported;
    }

    /* Publish the baseline before any AP reads it. The smp_mb() before each
     * SIPI completes the release; this barrier makes the contract explicit. */
    __atomic_store_n(&s_bsp_profile_ready, 1, __ATOMIC_RELEASE);

    klog(LOG_INFO, "cpu",
         "[BSP] hardening baseline EFER=0x%lx CR4=0x%lx PAT=0x%lx XCR0=0x%lx reqCR4=0x%lx",
         s_bsp_efer, s_bsp_cr4, s_bsp_pat, s_bsp_xcr0, s_bsp_required_cr4);
}

/* ---- CR4.PCIDE activation window (BSP + AP) -- TODO-09-boot S5 ----
 *
 * Sets CR4.PCIDE so the architectural PCID feature is on. PCID stays 0 on
 * every CR3 load (and CR3 bit 63 stays 0), so behavior is identical to
 * PCIDE=0 (full non-global TLB flush on every CR3 write) -- enabling the bit
 * now is safe and benign even though no consumer uses it yet. Per-process
 * PCID tagging + NOFLUSH CR3 switches are owned by TODO-10 S7 (blocked on
 * KPTI S4-S6); this function is ONLY the activation window. Safe on BSP and
 * APs: it gates on the calling CPU's own CPUID PCID bit, never the BSP-global
 * flag, and on an AP it additionally mirrors the BSP (never sets a uniform CR4
 * bit the BSP left clear). Logs via HARDEN_KLOG so AP bringup stays silent. */
boot_result_t cpu_pcid_enable(void)
{
    uint32_t eax, ebx, ecx, edx;
    uint64_t cr4, cr3;

    /* Gate on THIS CPU's own CPUID PCID (leaf 1 ECX bit 17), not the
     * BSP-global cpu_has(PCID): CR4.PCIDE is a reserved bit on a core that
     * lacks PCID, so a feature-skewed AP would #GP on the write. */
    /* BOOT_DEGRADED (not BOOT_OK) for legit skips: the caller marks
     * SUBSYS_PCID ready (the step ran) but does NOT emit POSTCODE_PCID_ENABLED,
     * so the progress stream never claims PCID was activated when it was not. */
    cpuid_raw(0x01, 0, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1u << 17))) {
        HARDEN_KLOG(LOG_INFO, "cpu", "[Phase1] PCID: not supported, skipped");
        return BOOT_DEGRADED;   /* PCID absent on this core -- nothing to do */
    }

    /* AP mirror: once the BSP baseline is published (s_bsp_profile_ready set
     * via the same RELEASE/ACQUIRE handshake ap_apply_xcr0 uses), an AP must
     * not set a uniform CR4 bit the BSP left clear, or the AP would diverge
     * from the BSP CR4 with no audit catching the extra bit. On the BSP path
     * (boot_phase1) the profile is not yet recorded (ready == 0), so the BSP
     * is authoritative and proceeds on its own CPUID gate. */
    if (__atomic_load_n(&s_bsp_profile_ready, __ATOMIC_ACQUIRE) &&
        !(s_bsp_cr4 & CR4_PCIDE))
        return BOOT_DEGRADED;   /* BSP did not enable PCIDE -- AP must not either */

    /* Intel SDM Vol. 3A Section 4.10.1: CR4.PCIDE may be set to 1 only when
     * CR3[11:0] == 0, else #GP. The boot PML4 is page-aligned so this is a
     * hard invariant; a nonzero value means CR3 corruption. Report BOOT_FATAL
     * so the caller leaves SUBSYS_PCID NOT ready AND records degraded_mask --
     * otherwise a broken CR3 would publish a false "PCID ready" while PCIDE is
     * actually off, hiding the violation from later consumers and the
     * end-of-boot degraded summary. */
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    if (cr3 & 0xFFFULL) {
        HARDEN_KLOG(LOG_ERROR, "cpu",
                    "[Phase1] PCID: CR3[11:0]=0x%lx nonzero -- CR4.PCIDE NOT set (invariant violated)",
                    cr3 & 0xFFFULL);
        return BOOT_FATAL;
    }

    cr4 = read_cr4();
    if (!(cr4 & CR4_PCIDE))
        write_cr4(cr4 | CR4_PCIDE);

    HARDEN_KLOG(LOG_INFO, "cpu", "[Phase1] PCID enabled (INVPCID %s)",
                cpu_has(CPU_FEATURE_INVPCID) ? "available" : "absent");
    return BOOT_OK;
}

/* Apply the BSP's recorded XCR0 mask on the calling AP, intersected with this
 * AP's own CPUID-reported supported bits (leaf 0x0D) so a narrower AP never
 * #GP's on xsetbv (Intel hybrid P/E parts). Sets CR4.OSXSAVE first. Uses the
 * recorded BSP mask, NOT a CPUID recompute, so an AP cannot re-enable an
 * xstate component the BSP deliberately disabled (e.g. AVX-512 throttle). */
static void ap_apply_xcr0(void)
{
    uint32_t eax, ebx, ecx, edx;
    uint64_t ap_supported, mask, cr4;

    /* Acquire-load pairs with the RELEASE store in cpu_record_bsp_profile()
     * so the BSP's baseline writes (s_bsp_xcr0 etc.) are visible here before
     * we consume them, with no compiler reordering of the plain reads. */
    if (!__atomic_load_n(&s_bsp_profile_ready, __ATOMIC_ACQUIRE))
        return;

    /* Gate on THIS AP's own CPUID-reported XSAVE (leaf 1 ECX bit 26), not
     * the BSP-global cpu_has(XSAVE): a feature-skewed AP that lacks XSAVE
     * must not touch CR4.OSXSAVE or xsetbv (both #GP without XSAVE). */
    cpuid_raw(0x01, 0, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1u << 26)))
        return;

    cpuid_raw(0x0D, 0, &eax, &ebx, &ecx, &edx);
    ap_supported = ((uint64_t)edx << 32) | eax;
    mask = s_bsp_xcr0 & ap_supported;
    if ((mask & 0x3) != 0x3)    /* x87+SSE are mandatory; never xsetbv without them */
        return;

    cr4 = read_cr4();
    write_cr4(cr4 | CR4_OSXSAVE);
    xcr0_write(mask);
}

/* Replay the BSP MSR profile on the calling AP. Returns the number applied. */
static uint32_t ap_apply_msr_profile(uint32_t cpu_id)
{
    uint32_t i, applied = 0;

    for (i = 0; i < MSR_PROFILE_COUNT; i++) {
        struct msr_profile_entry *e = &s_bsp_msr_profile[i];

        if (e->feature != MSR_PROFILE_ALWAYS &&
            !cpu_has((enum cpu_feature)e->feature))
            continue;

        if (e->per_cpu) {
            /* TSC_AUX = logical CPU id, only if the BSP probe confirmed it. */
            if (e->msr == MSR_IA32_TSC_AUX) {
                extern int g_tsc_aux_available;
                if (!g_tsc_aux_available)
                    continue;
                msr_write(MSR_IA32_TSC_AUX, (uint64_t)cpu_id);
            } else {
                continue;   /* unknown per-CPU entry: skip rather than guess */
            }
        } else {
            msr_write(e->msr, e->value);
        }
        applied++;
    }
    return applied;
}

/* ---- AP feature consistency validation (TODO-09-boot S6) --------------- */

uint64_t cpu_feature_global_mask(void)
{
    return __atomic_load_n(&s_global_feature_mask, __ATOMIC_ACQUIRE);
}

void cpu_validate_ap_features(uint32_t cpu_id)
{
    extern struct cpu_features g_cpu;
    struct per_cpu_data *pc = smp_get_cpu(cpu_id);
    uint32_t eax, ebx, ecx, edx, max_leaf;
    uint64_t ap_feat;
    uint8_t  core_type = CORE_TYPE_GENERIC;
    char     vendor[13];
    int      vendor_ok;

    /* No POST16 here: POST16 -> post_display16 -> fb_fill_rect draws to the
     * framebuffer with the BSP-selected SIMD path (AVX-512), which #UDs on an
     * AP whose XCR0/AVX-512 the caller has not enabled yet, and races other
     * CPUs on the shared FB corner. POST16 is a BSP pre-klog triple-fault aid;
     * AP feature validation is diagnosed BSP-side by ap_cpu_harden_log(). */

    /* AP-local probe (does NOT touch g_cpu). */
    ap_feat = cpuid_probe_ap_features();

    /* Vendor string (leaf 0: EBX, EDX, ECX) + core type (leaf 0x1A). */
    cpuid_raw(0x00000000, 0, &max_leaf, &ebx, &ecx, &edx);
    {
        uint32_t *v = (uint32_t *)vendor;
        v[0] = ebx; v[1] = edx; v[2] = ecx;
        vendor[12] = '\0';
    }
    if (max_leaf >= 0x1A) {
        cpuid_raw(0x1A, 0, &eax, &ebx, &ecx, &edx);
        uint8_t t = (uint8_t)((eax >> 24) & 0xFF);
        if (t == CORE_TYPE_P || t == CORE_TYPE_E)
            core_type = t;
    }

    if (pc) {
        pc->features         = ap_feat;
        pc->core_type        = core_type;
        pc->feature_mismatch = 0;
    }

    /* Fatal gate: vendor must match the BSP, Long Mode must be present, and
     * every required baseline feature must be set. A CPU failing any of these
     * cannot run the kernel safely. The AP must NOT KeBugCheckEx() itself: the
     * panic path captures FPU via the BSP-global XCR0 (panic_capture_fpu_state)
     * and draws via the BSP-global SIMD framebuffer path, both of which #GP/#UD
     * on exactly this feature-skewed AP. Instead record the fault and halt this
     * AP locally (no FB/FPU/serial); the BSP raises the 0x3E bug-check from
     * cpu_features_check_ap_faults() after bringup, where the panic path is
     * safe. The halted AP never sets is_online, so it is excluded regardless. */
    vendor_ok = 1;
    for (uint32_t k = 0; k < 12; k++) {
        if (vendor[k] != g_cpu.vendor[k]) { vendor_ok = 0; break; }
    }
    if (!vendor_ok ||
        !(ap_feat & (1ULL << CPU_FEATURE_LM)) ||
        (ap_feat & CPU_FEATURES_REQUIRED_MASK) != CPU_FEATURES_REQUIRED_MASK) {
        s_ap_fault_feat   = ap_feat;
        s_ap_fault_reason = !vendor_ok ? 1u
                          : !(ap_feat & (1ULL << CPU_FEATURE_LM)) ? 2u : 3u;
        __atomic_store_n(&s_ap_fault_cpu, cpu_id + 1, __ATOMIC_RELEASE);
        for (;;)
            __asm__ volatile ("cli; hlt");
    }

    /* Optional skew within the probed subset: any optional feature the BSP has
     * that this AP lacks. Flag only (no AP serial output); the BSP logs it via
     * ap_cpu_harden_log() and the global intersection (finalize) prevents
     * kernel-wide reliance on it. */
    {
        uint64_t bsp_opt = g_cpu.flags & CPU_FEATURES_AP_PROBE_MASK &
                           ~(uint64_t)CPU_FEATURES_REQUIRED_MASK;
        if (pc && (bsp_opt & ~ap_feat) != 0)
            pc->feature_mismatch = 1;
    }
}

void cpu_features_finalize_global(void)
{
    extern struct cpu_features g_cpu;
    uint64_t m = g_cpu.flags & CPU_FEATURES_AP_PROBE_MASK;   /* BSP is the base */
    uint32_t i;

    /* AND in every ONLINE AP's published features. Scan ALL slots, not
     * smp_cpu_count(): that returns the dense online COUNT (1 + online), so on
     * a sparse online set (e.g. AP1 timed out, AP2 online) a count-bounded loop
     * would skip the higher-id online AP and publish an over-broad mask.
     * is_online (acquire) gates each slot; never-started slots are zeroed BSS. */
    for (i = 1; i < MAX_CPUS; i++) {
        struct per_cpu_data *pc = smp_get_cpu(i);
        if (pc && __atomic_load_n(&pc->is_online, __ATOMIC_ACQUIRE))
            m &= pc->features;
    }

    /* xstate-dependent features are usable only if the OS enabled the backing
     * XCR0 component -- CPUID presence alone is not enough (e.g. AVX-512 cleared
     * by simd_enable_avx512()'s throttle guard). The published mask promises
     * "safe to USE on every online CPU", so clear any xstate feature whose XCR0
     * component is not active. XCR0 is uniform across CPUs (ap_apply_xcr0
     * replicates the BSP mask), so the BSP's g_cpu.xcr0_active is authoritative. */
    {
        uint64_t xcr0 = g_cpu.xcr0_active;
        if (xcr0 == 0)                                m &= ~(1ULL << CPU_FEATURE_XSAVE);
        if (!(xcr0 & (1ULL << 2)))                    m &= ~(1ULL << CPU_FEATURE_AVX);
        if ((xcr0 & (7ULL << 5)) != (7ULL << 5))      m &= ~(1ULL << CPU_FEATURE_AVX512F);
        if (!(xcr0 & (1ULL << 9)))                    m &= ~(1ULL << CPU_FEATURE_PKU);
    }

    __atomic_store_n(&s_global_feature_mask, m, __ATOMIC_RELEASE);
    klog(LOG_INFO, "smp",
         "Global CPU feature intersection 0x%lx (probe mask 0x%lx)",
         m, (uint64_t)CPU_FEATURES_AP_PROBE_MASK);
}

/* BSP-side: if any AP recorded a feature-validation fault and halted, raise the
 * 0x3E bug-check here (panic path is safe on the BSP). Call after SMP bringup. */
void cpu_features_check_ap_faults(void)
{
    uint32_t c = __atomic_load_n(&s_ap_fault_cpu, __ATOMIC_ACQUIRE);
    if (!c)
        return;
    klog(LOG_FATAL, "smp",
         "[AP%u] feature validation FAILED (reason %u: 1=vendor 2=LongMode 3=required, feat=0x%lx)",
         (uint64_t)(c - 1), (uint64_t)s_ap_fault_reason, s_ap_fault_feat);
    KeBugCheckEx(BUGCHECK_MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED,
                 (uint64_t)(c - 1), s_ap_fault_feat,
                 (uint64_t)CPU_FEATURES_REQUIRED_MASK, (uint64_t)s_ap_fault_reason);
}

/* ---- CR0/CR4 safety-bit pinning (TODO-09-boot S7) ---------------------- */

/* CR4 security bits eligible for pinning (Linux cr4_pin equivalent). Only the
 * bits actually SET on a given CPU at pin time are pinned on that CPU, so a
 * feature-skewed AP pins fewer bits rather than bug-checking on one it lacks.
 * CR4.PKE is included because cpu_enable_pku() turns it on when PKU + the PKRU
 * xstate are present; clearing it after pin would silently drop PKU enforcement
 * otherwise (it stays unpinned on CPUs where PKU was never enabled). */
#define CR4_PINNABLE_MASK \
    (CR4_SMEP | CR4_SMAP | CR4_UMIP | CR4_FSGSBASE | CR4_CET | CR4_PKE)

/* Global enforcement flag. Set by the BSP in cpu_pin_control_regs() at end of
 * Phase 1 (before APs launch); read on every CPU. Per-CPU masks
 * (per_cpu_data.cr0/cr4_pinned) stay 0 until each CPU pins, so a CPU that has
 * not pinned yet enforces nothing. Rollback knob: clear this to disable
 * enforcement if a false positive ever surfaces in the field. */
static volatile int cr_pinning_active;

/* AP -> BSP CR-pin fault hand-off. An AP that finds a cleared pin must NOT run
 * the panic path itself (it uses BSP-global XCR0/SIMD that #GP/#UD on a skewed
 * AP), so it records here and halts; the BSP raises the bug-check from
 * cpu_cr_pin_check(). Same pattern as the AP feature-validation hand-off. */
static volatile uint32_t s_cr_fault_cpu;       /* 0 = none, else cpu_id + 1 */
static volatile uint32_t s_cr_fault_reg;       /* 0 = CR0, 4 = CR4 */
static volatile uint64_t s_cr_fault_expected;  /* pinned mask that should be set */
static volatile uint64_t s_cr_fault_actual;    /* bits actually still set */

void cpu_pin_control_regs(void)
{
    struct per_cpu_data *pc = smp_this_cpu();
    uint64_t cr0_mask = read_cr0() & CR0_WP;
    uint64_t cr4_mask = read_cr4() & CR4_PINNABLE_MASK;

    if (pc) {
        pc->cr0_pinned = cr0_mask;
        pc->cr4_pinned = cr4_mask;
    }
    __atomic_store_n(&cr_pinning_active, 1, __ATOMIC_RELEASE);

    /* BSP-only log (APs run this inside the serial-quiet bringup bracket). */
    if (pc && pc->cpu_id == 0)
        klog(LOG_INFO, "cpu",
             "[Phase1] CR0/CR4 pinned: cr0_mask=0x%lx cr4_mask=0x%lx",
             cr0_mask, cr4_mask);
}

void cr0_write_safe(uint64_t val)
{
    struct per_cpu_data *pc;
    if (__atomic_load_n(&cr_pinning_active, __ATOMIC_ACQUIRE) &&
        (pc = smp_this_cpu()) != (struct per_cpu_data *)0)
        val |= pc->cr0_pinned;   /* a write that clears a pinned bit is corrected */
    write_cr0(val);
}

void cr4_write_safe(uint64_t val)
{
    struct per_cpu_data *pc;
    if (__atomic_load_n(&cr_pinning_active, __ATOMIC_ACQUIRE) &&
        (pc = smp_this_cpu()) != (struct per_cpu_data *)0)
        val |= pc->cr4_pinned;
    write_cr4(val);
}

/* Raise (BSP) or hand off (AP) a CR-pin violation. reg = 0 (CR0) or 4 (CR4). */
static void cr_pin_violation(uint32_t reg, uint64_t expected, uint64_t actual)
{
    struct per_cpu_data *pc = smp_this_cpu();
    uint32_t id = pc ? pc->cpu_id : 0;

    if (id == 0) {
        /* BSP: panic path is feature-safe here. */
        klog(LOG_FATAL, "cpu",
             "CR%u pin violation: expected 0x%lx still-set 0x%lx",
             (uint64_t)reg, expected, actual);
        KeBugCheckEx(BUGCHECK_CRITICAL_STRUCTURE_CORRUPTION,
                     (uint64_t)reg, expected, actual, 0);
    }
    /* AP: record + halt; the BSP raises the bug-check in cpu_cr_pin_check(). */
    s_cr_fault_reg      = reg;
    s_cr_fault_expected = expected;
    s_cr_fault_actual   = actual;
    __atomic_store_n(&s_cr_fault_cpu, id + 1, __ATOMIC_RELEASE);
    for (;;)
        __asm__ volatile ("cli; hlt");
}

void cr0_verify_pinned(void)
{
    struct per_cpu_data *pc = smp_this_cpu();
    uint64_t m, cr0;
    if (!__atomic_load_n(&cr_pinning_active, __ATOMIC_ACQUIRE) || !pc)
        return;
    m = pc->cr0_pinned;
    if (!m)
        return;
    cr0 = read_cr0();
    if ((cr0 & m) != m)
        cr_pin_violation(0, m, cr0 & m);
}

void cr4_verify_pinned(void)
{
    struct per_cpu_data *pc = smp_this_cpu();
    uint64_t m, cr4;
    if (!__atomic_load_n(&cr_pinning_active, __ATOMIC_ACQUIRE) || !pc)
        return;
    m = pc->cr4_pinned;
    if (!m)
        return;
    cr4 = read_cr4();
    if ((cr4 & m) != m)
        cr_pin_violation(4, m, cr4 & m);
}

/* BSP-side: raise BUGCHECK_CRITICAL_STRUCTURE_CORRUPTION if any AP recorded a
 * CR-pin violation and halted. Called from the BSP periodic verify path. */
void cpu_cr_pin_check(void)
{
    uint32_t c = __atomic_load_n(&s_cr_fault_cpu, __ATOMIC_ACQUIRE);
    if (!c)
        return;
    klog(LOG_FATAL, "cpu",
         "[AP%u] CR%u pin violation: expected 0x%lx still-set 0x%lx",
         (uint64_t)(c - 1), (uint64_t)s_cr_fault_reg,
         s_cr_fault_expected, s_cr_fault_actual);
    KeBugCheckEx(BUGCHECK_CRITICAL_STRUCTURE_CORRUPTION,
                 (uint64_t)s_cr_fault_reg, s_cr_fault_expected,
                 s_cr_fault_actual, (uint64_t)c);
}

/* BSP periodic hook (fired from the LAPIC timer ISR; AP LAPIC timers are
 * masked so this is BSP-only): verify the BSP's own pins + poll AP faults. */
void cpu_cr_pin_tick(void)
{
    cr0_verify_pinned();
    cr4_verify_pinned();
    cpu_cr_pin_check();
}

void ap_cpu_harden(uint32_t cpu_id)
{
    struct per_cpu_data *pc;
    uint64_t efer, cr4, pat, xcr0;
    uint32_t applied;

    /* 1. Match the BSP's validated XCR0 BEFORE cpu_harden() so
     *    cpu_enable_pku() sees this AP's real PKRU xstate -- otherwise the
     *    AP could set CR4.PKE while its XCR0 lacks the PKRU component. */
    ap_apply_xcr0();

    /* 1b. Validate this AP's CPUID against the BSP baseline BEFORE the optional
     *     CR4/MSR enables in cpu_harden() (publish-before-enable). ap_apply_xcr0()
     *     is feature-gated (intersects this AP's CPUID), so it enables nothing
     *     the AP lacks and does not pre-empt this check. On a required/vendor/
     *     Long-Mode failure the AP records the fault and halts (no self
     *     bug-check -- the panic path uses BSP-global XCR0/SIMD that would
     *     #GP/#UD on a skewed AP); the BSP raises 0x3E via
     *     cpu_features_check_ap_faults(). Optional skew is flagged (no AP
     *     serial); the BSP logs it. */
    cpu_validate_ap_features(cpu_id);

    /* 2. Gated security feature enables -- same path the BSP took, idempotent
     *    and per-feature CPUID-gated (NX/UMIP/PKU/PAT, then SMEP/SMAP). Each
     *    cpu_enable_* only sets the bit when the feature is present, so this
     *    is the AP-local-safe way to bring an AP up to the BSP's CR4 state.
     *    We deliberately do NOT force-OR the BSP CR4 mask here: blindly
     *    setting an architectural CR4 bit (FSGSBASE/CET) before the AP
     *    feature-consistency validation exists could #GP on a feature-skewed
     *    AP. Those future bits are replicated by their owning sections (CR4
     *    pinning) under that validation; this path stays warn-only on any
     *    residual CR4 mismatch (verified by the BSP). PCID is the exception:
     *    cpu_pcid_enable() below gates on the AP's own CPUID, so it is the
     *    same kind of feature-gated enable as cpu_enable_* (no #GP risk) and
     *    its owning section (TODO-09-boot S5) replicates it here directly.
     *    The quiet-depth bracket suppresses the per-feature cpu_enable_* log
     *    lines so the AP does NO serial output before it is marked online;
     *    the nesting counter keeps this AP quiet even if a late overlapping
     *    AP enters/exits its own bracket meanwhile. */
    __atomic_fetch_add(&s_harden_quiet_depth, 1, __ATOMIC_RELAXED);
    cpu_harden();
    cpu_harden_post_pagetable();
    /* CR4.PCIDE replication: TODO-09-boot S5 owns PCID activation and replicates
     * it here so APs match the BSP's CR4.PCIDE. Inside the quiet bracket so the
     * AP emits no serial output before it is marked online. */
    cpu_pcid_enable();
    __atomic_fetch_sub(&s_harden_quiet_depth, 1, __ATOMIC_RELAXED);

    /* 3. Replay the BSP MSR profile (PAT verbatim, TSC_AUX per-CPU). */
    applied = ap_apply_msr_profile(cpu_id);

    /* 4. Capture this AP's snapshot for the register audit trail. The XCR0
     *    read gates on THIS AP's live CR4.OSXSAVE (xcr0_read_safe), never the
     *    BSP-global cpu_has(XSAVE) -- a feature-skewed AP that never enabled
     *    OSXSAVE would #GP on xgetbv otherwise. */
    efer = msr_read(MSR_IA32_EFER);
    cr4  = read_cr4();
    pat  = msr_read(MSR_IA32_PAT);
    xcr0 = xcr0_read_safe();

    /* MTRR parity snapshot (S8): read-only rdmsr capture, gated on this AP's
     * LOCAL CPUID with an msr_try_read guard, so a feature-skewed AP records
     * supported=0 rather than faulting. No MTRR writes -- audit only. */
    {
        struct mtrr_snapshot ap_mtrr;
        mtrr_capture(&ap_mtrr);
        pc = smp_get_cpu(cpu_id);
        if (pc) {
            pc->mtrr_cap       = ap_mtrr.cap;
            pc->mtrr_def_type  = ap_mtrr.def_type;
            pc->mtrr_checksum  = ap_mtrr.checksum;
            pc->mtrr_var_count = ap_mtrr.var_count;
            pc->mtrr_supported = ap_mtrr.supported;
        }
    }

    pc = smp_get_cpu(cpu_id);
    if (pc) {
        pc->efer_at_boot        = efer;
        pc->cr4_at_boot         = cr4;
        pc->pat_at_boot         = pat;
        pc->xcr0_at_boot        = xcr0;
        pc->tsc_aux             = (uint64_t)cpu_id;
        pc->msr_profile_applied = applied;
    }

    /* 5. Pin this AP's CR0/CR4 safety bits (TODO-09-boot S7) now that cpu_harden
     *    has set them. Per-CPU mask = this AP's own live bits, so a skewed AP
     *    pins only what it has. cr_pinning_active was already set by the BSP in
     *    Phase 1. The just-pinned bits are trivially still set, but run the
     *    verifiers as the post-ap_cpu_harden self-check the section requires
     *    (a real violation here would record + halt this AP; the BSP raises the
     *    bug-check via cpu_cr_pin_check()). */
    cpu_pin_control_regs();
    cr0_verify_pinned();
    cr4_verify_pinned();

    /* 6. Capture this AP's full register audit snapshot (TODO-09-boot S9).
     *    No serial output (the BSP emits the [CPU%u AUDIT] line post-bringup);
     *    runs after the AP's IDT is loaded so msr_try_read() is live. */
    cpu_audit_registers(cpu_id);

    /* No serial output here: ap_cpu_harden() runs on the AP with IRQs masked
     * around the online transition, and klog -> serial_write busy-waits on
     * the UART unbounded. Emitting here would either delay the AP online
     * signal (pre-increment) or leave the AP counted-online-but-not-IPI-ready
     * (post-increment). The BSP calls ap_cpu_harden_log() for each online AP
     * after bringup, reading the snapshot buffered above. */
}

/* Emit one AP's CPU-hardening audit line + any security-critical mismatch
 * warnings, from the snapshot ap_cpu_harden() buffered into per_cpu_data.
 * Called by the BSP for each online AP after SMP bringup -- never on the AP
 * itself -- so unbounded serial I/O stays off the AP bringup critical path.
 * EFER.SCE deliberately differs (APs have no SYSCALL MSR setup), so only NXE
 * is compared, not the whole EFER register. */
void ap_cpu_harden_log(uint32_t cpu_id)
{
    struct per_cpu_data *pc = smp_get_cpu(cpu_id);
    uint64_t efer, cr4, pat, xcr0;
    uint32_t applied;

    if (!pc)
        return;
    efer    = pc->efer_at_boot;
    cr4     = pc->cr4_at_boot;
    pat     = pc->pat_at_boot;
    xcr0    = pc->xcr0_at_boot;
    applied = pc->msr_profile_applied;

    if (__atomic_load_n(&s_bsp_profile_ready, __ATOMIC_ACQUIRE)) {
        if ((efer & EFER_NXE) != (s_bsp_efer & EFER_NXE))
            klog(LOG_WARN, "smp",
                 "[AP%u] EFER.NXE mismatch: AP=0x%lx BSP=0x%lx",
                 (uint64_t)cpu_id, efer, s_bsp_efer);
        if ((cr4 & s_bsp_required_cr4) != s_bsp_required_cr4)
            klog(LOG_WARN, "smp",
                 "[AP%u] CR4 required-bit mismatch: AP=0x%lx need=0x%lx",
                 (uint64_t)cpu_id, cr4, s_bsp_required_cr4);
        if (pat != s_bsp_pat)
            klog(LOG_WARN, "smp",
                 "[AP%u] PAT mismatch: AP=0x%lx BSP=0x%lx",
                 (uint64_t)cpu_id, pat, s_bsp_pat);
        else
            klog(LOG_INFO, "smp",
                 "[AP%u] PAT synced: 0x%lx", (uint64_t)cpu_id, pat);

        /* MTRR parity audit (S8): warn-only -- no reprogramming. MMIO cache
         * correctness rides on PAT (UC PAT type always wins over MTRR, SDM
         * 11.5.2), so divergence here is defense-in-depth, not a hard fault. */
        if (pc->mtrr_supported && s_bsp_mtrr.supported) {
            struct mtrr_snapshot ap = {
                .cap = pc->mtrr_cap, .def_type = pc->mtrr_def_type,
                .var_count = pc->mtrr_var_count, .supported = pc->mtrr_supported,
                .checksum = pc->mtrr_checksum,
            };
            if (!mtrr_snapshot_equal(&ap, &s_bsp_mtrr))
                klog(LOG_WARN, "smp",
                     "[AP%u] MTRR mismatch: AP def=0x%lx sum=0x%lx vcnt=%u | "
                     "BSP def=0x%lx sum=0x%lx vcnt=%u (firmware MTRR skew)",
                     (uint64_t)cpu_id, pc->mtrr_def_type, pc->mtrr_checksum,
                     (uint64_t)pc->mtrr_var_count, s_bsp_mtrr.def_type,
                     s_bsp_mtrr.checksum, (uint64_t)s_bsp_mtrr.var_count);
            else
                klog(LOG_INFO, "smp",
                     "[AP%u] MTRR synced: def=0x%lx vcnt=%u",
                     (uint64_t)cpu_id, pc->mtrr_def_type,
                     (uint64_t)pc->mtrr_var_count);
        } else if (pc->mtrr_supported != s_bsp_mtrr.supported) {
            klog(LOG_WARN, "smp",
                 "[AP%u] MTRR support skew: AP=%u BSP=%u",
                 (uint64_t)cpu_id, (uint64_t)pc->mtrr_supported,
                 (uint64_t)s_bsp_mtrr.supported);
        }
    }

    klog(LOG_INFO, "smp",
         "[AP%u] CPU hardening applied EFER=0x%lx CR4=0x%lx PAT=0x%lx XCR0=0x%lx MSRs=%u",
         (uint64_t)cpu_id, efer, cr4, pat, xcr0, (uint64_t)applied);

    /* AP feature consistency result (S6). The AP buffered features/mismatch in
     * cpu_validate_ap_features(); a missing-required mismatch would already
     * have bug-checked, so reaching here means required features are present. */
    {
        extern struct cpu_features g_cpu;
        if (pc->feature_mismatch) {
            uint64_t bsp_opt = g_cpu.flags & CPU_FEATURES_AP_PROBE_MASK &
                               ~(uint64_t)CPU_FEATURES_REQUIRED_MASK;
            klog(LOG_WARN, "smp",
                 "[AP%u] FEATURE MISMATCH: BSP optional 0x%lx, AP 0x%lx, missing 0x%lx (core_type 0x%x)",
                 (uint64_t)cpu_id, bsp_opt, pc->features,
                 bsp_opt & ~pc->features, (uint64_t)pc->core_type);
        } else {
            klog(LOG_INFO, "smp", "[AP%u] Feature validation OK (core_type 0x%x)",
                 (uint64_t)cpu_id, (uint64_t)pc->core_type);
        }
    }
}

/* ---- CPU register state audit trail (TODO-09-boot S9) ------------------- */

/* Read this CPU's microcode revision. MSR 0x8B (IA32_BIOS_SIGN_ID) is
 * architectural on Intel + AMD. Intel reports the revision in the HIGH dword,
 * but only after a CPUID(1) reload of the signature register -- the documented
 * sequence is "clear the MSR, execute CPUID, re-read". AMD exposes the patch
 * level directly in the LOW dword with no write. We probe #GP-safely first, and
 * ONLY Intel takes the write path (gating strictly on GenuineIntel keeps the
 * wrmsr off any vendor that does not define the write semantics). */
static uint32_t cpu_read_microcode_rev(void)
{
    extern struct cpu_features g_cpu;
    static const char intel_vendor[12] =
        { 'G','e','n','u','i','n','e','I','n','t','e','l' };
    uint64_t sig;
    uint32_t a, b, c, d, i;
    int is_intel = 1;

    /* EXACT GenuineIntel match -- a loose vendor[0]=='G' could let a spoofed or
     * odd vendor string take the Intel write path. */
    for (i = 0; i < 12; i++) {
        if (g_cpu.vendor[i] != intel_vendor[i]) { is_intel = 0; break; }
    }

    if (msr_try_read(MSR_IA32_BIOS_SIGN_ID, &sig) != 0)
        return 0;  /* MSR not present on this CPU */

    if (is_intel) {
        /* Intel reports the revision in the HIGH dword only after a CPUID(1)
         * reload of the signature register: clear the MSR, CPUID, re-read. The
         * clear uses msr_try_write() so even a vendor-string spoof that is not
         * really Intel cannot #GP us during BSP/AP bringup. */
        if (msr_try_write(MSR_IA32_BIOS_SIGN_ID, 0) != 0)
            return 0;
        cpuid_raw(0x01, 0, &a, &b, &c, &d);
        if (msr_try_read(MSR_IA32_BIOS_SIGN_ID, &sig) != 0)
            return 0;
        return (uint32_t)(sig >> 32);
    }
    return (uint32_t)(sig & 0xFFFFFFFFu);  /* AMD + others: low-dword patch level */
}

/* Capture the calling CPU's security-relevant register state into per_cpu_data.
 * Runs ON the CPU being audited: the BSP in Phase 2 (after the IDT is loaded --
 * msr_try_read needs it), and each AP at the tail of ap_cpu_harden(). Emits NO
 * serial output, so it is safe on the serial-quiet pre-online AP path; the BSP
 * emits the consolidated line later via cpu_audit_log(). CPUID gates each
 * optional MSR, with msr_try_read() as the #GP-safe net underneath. */
void cpu_audit_registers(uint32_t cpu_id)
{
    struct per_cpu_data *pc = smp_get_cpu(cpu_id);
    uint64_t cr4, val;

    if (!pc)
        return;

    cr4 = read_cr4();
    pc->efer_at_boot = msr_read(MSR_IA32_EFER);
    pc->cr0_at_boot  = read_cr0();
    pc->cr4_at_boot  = cr4;
    pc->xcr0_at_boot = (cr4 & CR4_OSXSAVE) ? xcr0_read_safe() : 0;
    pc->pat_at_boot  = msr_read(MSR_IA32_PAT);
    pc->misc_enable  = (msr_try_read(MSR_IA32_MISC_ENABLE, &val) == 0) ? val : 0;
    pc->arch_caps    = (cpu_has(CPU_FEATURE_ARCH_CAP) &&
                        msr_try_read(MSR_IA32_ARCH_CAPS, &val) == 0) ? val : 0;
    pc->spec_ctrl_at_boot = (cpu_has(CPU_FEATURE_SPEC_CTRL) &&
                        msr_try_read(MSR_IA32_SPEC_CTRL, &val) == 0) ? val : 0;
    pc->ucode_rev    = cpu_read_microcode_rev();
    pc->audit_captured = 1;
}

/* Emit the single consolidated `[CPU%u AUDIT]` line from the buffered snapshot.
 * BSP-side only (never on the AP itself -- keeps serial I/O off the AP bringup
 * critical path). Feature flags are derived from the captured register BITS
 * (per-CPU truth), not BSP-global cpu_has(). This is the authoritative,
 * fixed-format, CI-greppable per-CPU register line. */
void cpu_audit_log(uint32_t cpu_id)
{
    struct per_cpu_data *pc = smp_get_cpu(cpu_id);
    uint64_t efer, cr0, cr4, xcr0;

    if (!pc || !pc->audit_captured)
        return;
    efer = pc->efer_at_boot;
    cr0  = pc->cr0_at_boot;
    cr4  = pc->cr4_at_boot;
    xcr0 = pc->xcr0_at_boot;

    klog(LOG_INFO, "cpu",
         "[CPU%u AUDIT] EFER=0x%lx CR0=0x%lx CR4=0x%lx XCR0=0x%lx PAT=0x%lx "
         "MISC=0x%lx ARCH_CAPS=0x%lx SPEC_CTRL=0x%lx UCODE=0x%x "
         "NX=%u SMEP=%u SMAP=%u UMIP=%u WP=%u PCID=%u OSXSAVE=%u",
         (uint64_t)cpu_id, efer, cr0, cr4, xcr0, pc->pat_at_boot,
         pc->misc_enable, pc->arch_caps, pc->spec_ctrl_at_boot,
         (uint64_t)pc->ucode_rev,
         (uint64_t)((efer >> 11) & 1u),   /* NX (EFER.NXE) */
         (uint64_t)((cr4 >> 20) & 1u),    /* SMEP */
         (uint64_t)((cr4 >> 21) & 1u),    /* SMAP */
         (uint64_t)((cr4 >> 11) & 1u),    /* UMIP */
         (uint64_t)((cr0 >> 16) & 1u),    /* CR0.WP */
         (uint64_t)((cr4 >> 17) & 1u),    /* PCID (CR4.PCIDE) */
         (uint64_t)((cr4 >> 18) & 1u));   /* OSXSAVE */
}

/* Compare every online AP's audit snapshot against the BSP's and emit a single
 * verdict line. Security-relevant subset: EFER.NXE, the uniform CR4 bits, PAT,
 * XCR0, ARCH_CAPABILITIES. Divergence is WARN-only (informational audit; the
 * hard AP-vs-BSP guards live in S4/S6). BSP-side, after SMP bringup. */
void cpu_audit_consistency_check(uint32_t total_cpus)
{
    struct per_cpu_data *bsp = smp_get_cpu(0);
    uint32_t i, divergent = 0;

    if (!bsp || !bsp->audit_captured)
        return;

    /* Iterate ALL slots filtered on is_online, NOT i < total_cpus: AP logical
     * IDs are slot-allocated, so after a partial bringup (AP1 times out, AP2
     * online) the live AP sits past the dense online count. Bounding by
     * total_cpus would skip it and falsely report consistency. */
    for (i = 1; i < MAX_CPUS; i++) {
        struct per_cpu_data *pc = smp_get_cpu(i);
        if (!pc || !__atomic_load_n(&pc->is_online, __ATOMIC_ACQUIRE) ||
            !pc->audit_captured)
            continue;
        if (((pc->efer_at_boot ^ bsp->efer_at_boot) & EFER_NXE) ||
            ((pc->cr4_at_boot & s_bsp_required_cr4) != s_bsp_required_cr4) ||
            pc->pat_at_boot != bsp->pat_at_boot ||
            pc->xcr0_at_boot != bsp->xcr0_at_boot ||
            pc->arch_caps != bsp->arch_caps) {
            klog(LOG_WARN, "smp",
                 "[CPU%u AUDIT] register divergence from BSP: "
                 "EFER=0x%lx/0x%lx CR4=0x%lx/0x%lx PAT=0x%lx/0x%lx "
                 "XCR0=0x%lx/0x%lx ARCH_CAPS=0x%lx/0x%lx",
                 (uint64_t)i, pc->efer_at_boot, bsp->efer_at_boot,
                 pc->cr4_at_boot, bsp->cr4_at_boot, pc->pat_at_boot,
                 bsp->pat_at_boot, pc->xcr0_at_boot, bsp->xcr0_at_boot,
                 pc->arch_caps, bsp->arch_caps);
            divergent++;
        }
    }

    if (divergent == 0)
        klog(LOG_INFO, "smp",
             "[SMP] All %u CPUs register-consistent", (uint64_t)total_cpus);
    else
        klog(LOG_WARN, "smp",
             "[SMP] %u of %u CPUs diverge from BSP register state",
             (uint64_t)divergent, (uint64_t)total_cpus);
}

/* Guarantee the BSP register audit ran, on EVERY boot path. smp_init() does it
 * on the ACPI path (single-CPU + SMP), but a non-ACPI/degraded boot skips
 * smp_init() entirely -- this fallback (idempotent via audit_captured) keeps
 * the [CPU0 AUDIT] line + consistency verdict present there too. Called from
 * boot_phase2 after the SMP-bringup block. */
void cpu_audit_ensure_bsp(void)
{
    struct per_cpu_data *bsp = smp_get_cpu(0);
    if (bsp && bsp->audit_captured)
        return;  /* smp_init() already audited the BSP */
    cpu_audit_registers(0);
    cpu_audit_log(0);
    cpu_audit_consistency_check(1);
}

/* Expose each CPU's buffered audit snapshot under HKLM\HARDWARE\CPU\%u\Registers.
 * Called from registry_populate_defaults() (Phase 2, AFTER registry_init() --
 * smp_init() runs earlier, so the data is captured but the registry is not yet
 * up at audit time). Read-only consumer of the per_cpu_data audit fields. */
void cpu_audit_populate_registry(void)
{
    uint32_t i;

    /* Iterate ALL slots filtered on is_online (NOT smp_cpu_count(), which
     * collapses sparse slots after a partial bringup): otherwise an online AP
     * past the dense count would be omitted from the registry exactly when the
     * audit trail matters most. cpu0 (BSP) has is_online == 1. */
    for (i = 0; i < MAX_CPUS; i++) {
        struct per_cpu_data *pc = smp_get_cpu(i);
        char subkey[48];
        HKEY hKey;
        uint32_t disp;
        long rc;

        if (!pc || !__atomic_load_n(&pc->is_online, __ATOMIC_ACQUIRE) ||
            !pc->audit_captured)
            continue;
        snprintf(subkey, sizeof(subkey), "HARDWARE\\CPU\\%u\\Registers", i);
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, subkey, 0, (const char *)0, 0,
                           KEY_ALL_ACCESS, (void *)0, &hKey, &disp) != ERROR_SUCCESS)
            continue;
        /* Create the completeness marker FIRST so it is present even if the
         * fixed-size registry value pool is exhausted by the field writes
         * below. If even this allocation fails, the pool is already full --
         * skip the key rather than publish one with no marker. */
        if (RegSetDword(hKey, "AuditComplete", 0u) != ERROR_SUCCESS) {
            klog(LOG_WARN, "cpu",
                 "[CPU%u AUDIT] registry export skipped (value pool full)",
                 (uint64_t)i);
            RegCloseKey(hKey);
            continue;
        }
        /* Accumulate setter results; RegSetQword can fail (ERROR_OUTOFMEMORY)
         * once the pool is exhausted. */
        rc  = RegSetQword(hKey, "EFER",       pc->efer_at_boot);
        rc |= RegSetQword(hKey, "CR0",        pc->cr0_at_boot);
        rc |= RegSetQword(hKey, "CR4",        pc->cr4_at_boot);
        rc |= RegSetQword(hKey, "XCR0",       pc->xcr0_at_boot);
        rc |= RegSetQword(hKey, "PAT",        pc->pat_at_boot);
        rc |= RegSetQword(hKey, "MiscEnable", pc->misc_enable);
        rc |= RegSetQword(hKey, "ArchCaps",   pc->arch_caps);
        rc |= RegSetQword(hKey, "SpecCtrl",   pc->spec_ctrl_at_boot);
        rc |= RegSetDword(hKey, "MicrocodeRev", pc->ucode_rev);
        if (rc == 0)
            /* All fields written -- overwrite the existing marker IN PLACE
             * (no new allocation, so this cannot fail for lack of pool). */
            RegSetDword(hKey, "AuditComplete", 1u);
        else
            klog(LOG_WARN, "cpu",
                 "[CPU%u AUDIT] registry export incomplete (value pool full); "
                 "AuditComplete stays 0", (uint64_t)i);
        RegCloseKey(hKey);
    }
}

#ifdef KERNEL_TESTS
uint32_t cpu_msr_profile_count(void)
{
    return (uint32_t)MSR_PROFILE_COUNT;
}

int cpu_msr_profile_entry(uint32_t idx, uint32_t *msr_out,
                          uint64_t *value_out, int *per_cpu_out)
{
    if (idx >= MSR_PROFILE_COUNT)
        return -1;
    if (msr_out)     *msr_out     = s_bsp_msr_profile[idx].msr;
    if (value_out)   *value_out   = s_bsp_msr_profile[idx].value;
    if (per_cpu_out) *per_cpu_out = s_bsp_msr_profile[idx].per_cpu;
    return 0;
}

uint64_t cpu_bsp_pat_baseline(void)
{
    return s_bsp_pat;
}

void cpu_bsp_mtrr_baseline(struct mtrr_snapshot *out)
{
    if (out)
        *out = s_bsp_mtrr;
}
#endif /* KERNEL_TESTS */
