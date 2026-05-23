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
#ifdef KERNEL_TESTS
#include "kernel/sched/irql.h"          /* KeGetCurrentIrql for thread-context gate */
#include "kernel/sched/task.h" /* task_current() for task-filter gate */
#endif

/* ---- CR4 bit definitions ---- */
#define CR4_FSGSBASE (1UL << 16)
#define CR4_PCIDE    (1UL << 17)
#define CR4_OSXSAVE  (1UL << 18)
#define CR4_UMIP     (1UL << 11)
#define CR4_PKE      (1UL << 22)
#define CR4_SMEP     (1UL << 20)
#define CR4_SMAP     (1UL << 21)
#define CR4_CET      (1UL << 23)

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

    /* Snapshot the BSP's own block (cpu_id 0) for the register audit trail. */
    bsp = smp_get_cpu(0);
    if (bsp) {
        bsp->efer_at_boot = s_bsp_efer;
        bsp->cr4_at_boot  = s_bsp_cr4;
        bsp->pat_at_boot  = s_bsp_pat;
        bsp->xcr0_at_boot = s_bsp_xcr0;
        bsp->tsc_aux      = 0;
    }

    /* Publish the baseline before any AP reads it. The smp_mb() before each
     * SIPI completes the release; this barrier makes the contract explicit. */
    __atomic_store_n(&s_bsp_profile_ready, 1, __ATOMIC_RELEASE);

    klog(LOG_INFO, "cpu",
         "[BSP] hardening baseline EFER=0x%lx CR4=0x%lx PAT=0x%lx XCR0=0x%lx reqCR4=0x%lx",
         s_bsp_efer, s_bsp_cr4, s_bsp_pat, s_bsp_xcr0, s_bsp_required_cr4);
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

void ap_cpu_harden(uint32_t cpu_id)
{
    struct per_cpu_data *pc;
    uint64_t efer, cr4, pat, xcr0;
    uint32_t applied;

    /* 1. Match the BSP's validated XCR0 BEFORE cpu_harden() so
     *    cpu_enable_pku() sees this AP's real PKRU xstate -- otherwise the
     *    AP could set CR4.PKE while its XCR0 lacks the PKRU component. */
    ap_apply_xcr0();

    /* 2. Gated security feature enables -- same path the BSP took, idempotent
     *    and per-feature CPUID-gated (NX/UMIP/PKU/PAT, then SMEP/SMAP). Each
     *    cpu_enable_* only sets the bit when the feature is present, so this
     *    is the AP-local-safe way to bring an AP up to the BSP's CR4 state.
     *    We deliberately do NOT force-OR the BSP CR4 mask here: blindly
     *    setting an architectural CR4 bit (PCIDE/FSGSBASE/CET) before the AP
     *    feature-consistency validation exists could #GP on a feature-skewed
     *    AP. Those future bits are replicated by their owning sections (PCID
     *    activation, CR4 pinning) under that validation; this path stays
     *    warn-only on any residual CR4 mismatch (verified by the BSP).
     *    The quiet-depth bracket suppresses the per-feature cpu_enable_* log
     *    lines so the AP does NO serial output before it is marked online;
     *    the nesting counter keeps this AP quiet even if a late overlapping
     *    AP enters/exits its own bracket meanwhile. */
    __atomic_fetch_add(&s_harden_quiet_depth, 1, __ATOMIC_RELAXED);
    cpu_harden();
    cpu_harden_post_pagetable();
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

    pc = smp_get_cpu(cpu_id);
    if (pc) {
        pc->efer_at_boot        = efer;
        pc->cr4_at_boot         = cr4;
        pc->pat_at_boot         = pat;
        pc->xcr0_at_boot        = xcr0;
        pc->tsc_aux             = (uint64_t)cpu_id;
        pc->msr_profile_applied = applied;
    }

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
    }

    klog(LOG_INFO, "smp",
         "[AP%u] CPU hardening applied EFER=0x%lx CR4=0x%lx PAT=0x%lx XCR0=0x%lx MSRs=%u",
         (uint64_t)cpu_id, efer, cr4, pat, xcr0, (uint64_t)applied);
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
#endif /* KERNEL_TESTS */
