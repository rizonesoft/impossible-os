/* ============================================================================
 * cpu_security.c -- CPU security feature activation
 *
 * XREF: 02-kernel-core/TODO-10-kernel-security-hardening.md -
 * ============================================================================ */

#include "kernel/cpu_security.h"
#include "kernel/cpuid.h"
#include "kernel/mm/memmap.h"           /* MM_IS_CANONICAL_4LVL for __kstack_read_u64 */
#include "kernel/cpuid_platform.h"
#include "kernel/msr.h"
#include "kernel/boot_init.h"
#include "kernel/boot_halt.h"           /* boot_halt for NX verify hard-fail (BSP-only) */
#include "kernel/klog.h"
#include "kernel/security/pku.h"
#include "kernel/smp.h"                 /* per_cpu_data, smp_get_cpu() for AP hardening */
#include "kernel/bugcheck.h"            /* KeBugCheckEx for AP feature validation (S6) */
#include "kernel/topology.h"            /* CORE_TYPE_* for AP core-type probe (S6) */
#include "kernel/cpu_regs.h"            /* CR0/CR4 control-register bit defines (CR pinning) */
#include "kernel/mtrr.h"                /* MTRR snapshot + parity audit (S8) */
#include "kernel/idt.h"                 /* idt_register_handler, interrupt_frame (S10 verify-IPI) */
#include "kernel/drivers/lapic.h"       /* IPI_VECTOR_CR_VERIFY, lapic_send_ipi, lapic_eoi (S10) */
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

/* AP-LOCAL feature predicate (TODO-09-boot S10). The optional CR4 enables
 * (UMIP/PKU/SMEP/SMAP) must gate on the CALLING CPU's own capability, not the
 * BSP-global g_cpu bitmap -- a feature-skewed AP (BSP has UMIP, this AP does
 * not) would otherwise #GP setting the CR4 bit. On the BSP (cpu_id 0) this is
 * just cpu_has(); on an AP it reads pc->features, the AP-local CPUID subset
 * that cpu_validate_ap_features() (S6) probed and stored BEFORE cpu_harden()
 * runs the enables. Feature must be inside CPU_FEATURES_AP_PROBE_MASK to be
 * represented in pc->features. */
static int cpu_feature_local(enum cpu_feature feature)
{
    struct per_cpu_data *pc = smp_this_cpu();
    if (pc && pc->cpu_id != 0)
        return cpu_feature_test(&pc->features, feature);
    return cpu_has(feature) ? 1 : 0;
}

/* ---- NX (No-Execute) via EFER.NXE ---- */

void cpu_enable_nx(void)
{
    if (!cpu_has(CPU_FEATURE_NX))   /* NX is REQUIRED (S6 bug-checks if absent) -- global is safe */
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
    if (!cpu_feature_local(CPU_FEATURE_SMEP))   /* AP-local: optional, may be skewed */
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
    if (!cpu_feature_local(CPU_FEATURE_SMAP))   /* AP-local: optional, may be skewed */
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

/* ========================================================================
 * Fault-recoverable user access
 *
 * __uaccess_copy_from / __uaccess_copy_to / __uaccess_touch_w perform the
 * actual user-memory touch behind a STATIC exception table keyed by the EXACT
 * faulting instruction RIP. page_fault_handler (vmm.c) recognizes a #PF taken
 * at a __uaccess_*_fault label and redirects RIP to the paired __uaccess_*_fixup
 * label, turning a bad user pointer into a graceful failure instead of a kernel
 * bugcheck. There is NO per-CPU state and NO cli: the faulting RIP alone
 * identifies the guarded instruction, so recovery is inherently SMP- and
 * preempt-safe. A per-CPU return-slot + cli would be misredirectable by a
 * nested NMI/MCE #PF and would hold interrupts off for the whole copy.
 *
 * COPY DIRECTION MATTERS. There are no per-process page tables, so the kernel
 * heap lives at LOW identity-mapped VAs -- an operand address alone cannot say
 * whether a fault hit the user or the kernel operand. So the from/to copies use
 * DISTINCT fault labels and the handler recovers only when the fault DIRECTION
 * matches the user operand: a copy_FROM_user recovers only a READ fault (the
 * user SOURCE), a copy_TO_user / write-touch recovers only a WRITE fault (the
 * user DEST). A wrong-direction fault is the KERNEL operand and stays terminal,
 * so a kernel-buffer overflow into a guard page still bugchecks.
 *
 * NOT an isolation boundary: an in-range user pointer that aliases mapped
 * kernel data still resolves without faulting. True per-process isolation is
 * owned by the per-process page-table work in
 * 03-memory-concurrency/TODO-01-vmm-memory-protection.md.
 * ARCH: x86-64 -- rep movsb / lock orb are x86 string/atomic ops.
 * ======================================================================== */

/* Generate a guarded byte-copy with its own fault/fixup labels. Returns bytes
 * NOT copied (0 == full success). The `rep movsb` is the guarded instruction
 * (its RIP == <name>_fault); on a #PF mid-copy the handler redirects to
 * <name>_fixup, leaving RCX = bytes remaining. noinline so the global labels
 * are emitted exactly once; cld pins a forward copy regardless of the caller's
 * DF. Two directions get separate labels so the fault handler can tell a user
 * SOURCE read fault (copy_from) from a user DEST write fault (copy_to). */
#define UACCESS_COPY_FN(name)                                                 \
    __attribute__((noinline))                                                 \
    uint64_t name(void *dst, const void *src, uint64_t n)                     \
    {                                                                         \
        __asm__ volatile (                                                    \
            "cld\n\t"                                                         \
            ".globl " #name "_fault\n\t"                                      \
            ".globl " #name "_fixup\n\t"                                      \
            #name "_fault:\n\t"                                               \
            "rep movsb\n\t"                                                   \
            #name "_fixup:\n\t"                                               \
            : "+c"(n), "+D"(dst), "+S"(src)                                   \
            :                                                                 \
            : "memory", "cc");                                                \
        return n;                                                             \
    }

UACCESS_COPY_FN(__uaccess_copy_from)   /* user SOURCE  -> read fault recovers  */
UACCESS_COPY_FN(__uaccess_copy_to)     /* user DEST    -> write fault recovers */

/* Write-probe a single address WITHOUT changing its contents: `lock orb $0` is
 * a read-modify-write that sets no bits but faults on a read-only or unmapped
 * page. Returns 0 if writable, -1 if it faulted (redirected to the fixup). The
 * touch is a write, so the handler recovers it only on a WRITE fault. */
__attribute__((noinline))
int __uaccess_touch_w(void *addr)
{
    int failed = 0;
    __asm__ volatile (
        ".globl __uaccess_touch_fault\n\t"
        ".globl __uaccess_touch_fixup\n\t"
        "__uaccess_touch_fault:\n\t"
        "lock orb $0, (%[a])\n\t"
        "jmp 1f\n\t"
        "__uaccess_touch_fixup:\n\t"
        "movl $1, %[f]\n\t"
        "1:\n\t"
        : [f]"+r"(failed)
        : [a]"r"(addr)
        : "memory", "cc");
    return failed ? -1 : 0;
}

/* Fault-recoverable single-QWORD read from a KERNEL address (TODO-23 s7 stack
 * walking). The `movq` is the guarded instruction (its RIP == __kstack_read_fault);
 * a #PF on a corrupt/off-stack/guard-page RBP is redirected by page_fault_handler
 * to __kstack_read_fixup, which sets failed = 1. Distinct labels from __uaccess_*
 * because this reads a KERNEL VA and the handler matches it by RIP alone (read
 * direction, any CR2) ahead of the pager -- see vmm.c. noinline so the global
 * labels emit exactly once. Returns 0 on success (*out = value), -1 on fault. */
__attribute__((noinline))
int __kstack_read_u64(uint64_t *out, const void *addr)
{
    int failed = 0;
    uint64_t val = 0;
    uint64_t a = (uint64_t)(uintptr_t)addr;

    if (!out)
        return -1;
    /* The full 8-byte load is [a, a+7]. Reject a top-of-address-space wrap (a+7
     * overflowing) BEFORE the canonical test -- a wrapped endpoint could re-enter
     * canonical space and let the load straddle the linear-address boundary and
     * raise #GP, which the #PF-only fixup cannot recover. Then require BOTH the
     * base and the last byte canonical (a non-canonical operand also #GPs).
     * NOTE: recovery relies on normal #PF delivery, so this is only fault-safe for
     * RESIDENT kernel memory reached in a context that can take a #PF (not from a
     * #DF/#MC/NMI abort handler); pageable/user memory read here faults and
     * returns -1 rather than being paged in. */
    if (a > ~(uint64_t)0 - 7u)
        return -1;
    if (!MM_IS_CANONICAL_4LVL(a) || !MM_IS_CANONICAL_4LVL(a + 7u))
        return -1;

    __asm__ volatile (
        ".globl __kstack_read_fault\n\t"
        ".globl __kstack_read_fixup\n\t"
        "__kstack_read_fault:\n\t"
        "movq (%[a]), %[v]\n\t"
        "jmp 1f\n\t"
        "__kstack_read_fixup:\n\t"
        "movl $1, %[f]\n\t"
        "1:\n\t"
        : [v]"+r"(val), [f]"+r"(failed)
        : [a]"r"(addr)
        : "memory", "cc");
    if (failed)
        return -1;
    *out = val;
    return 0;
}

/* Fault-suppressed single-BYTE read from an arbitrary KERNEL VA.
 *
 * Same static RIP-keyed mechanism as __kstack_read_u64 above -- distinct labels
 * because page_fault_handler matches the EXACT faulting instruction, so two
 * guarded loads cannot share one label pair. noinline so the globals emit once.
 *
 * Exists for the panic path, which walks caller-supplied C strings (a panic
 * description, a __FILE__) that may themselves be part of the corruption being
 * reported: serial.c's emergency writer and panic.c's evidence collector. A byte
 * load needs no straddle check -- one byte cannot cross the canonical hole -- so
 * only the address itself is canonical-tested; a non-canonical operand raises
 * #GP, which a #PF-keyed fixup cannot recover.
 *
 * SAME CONTEXT CAVEAT AS __kstack_read_u64, and it is load-bearing here rather
 * than theoretical: recovery works by taking a real #PF and IRETing out of it.
 * Inside an already-running #DF/#MC handler that is fine -- shutdown requires a
 * fault while DELIVERING #DF, not one taken by a handler already running. From
 * an NMI handler it is NOT: the fixup returns through IRETQ, which re-arms NMI
 * delivery while the outer NMI is still live on IST2, so a second NMI reuses
 * that stack and overwrites the frames. Callers on the panic path therefore
 * DECLARE their context rather than probing it -- see serial.h PANIC_CTX_*.
 * Returns 0 on success (*out = byte), -1 on fault. */
__attribute__((noinline))
int __kread_u8(uint8_t *out, const void *addr)
{
    int      failed = 0;
    uint64_t val    = 0;
    uint64_t a      = (uint64_t)(uintptr_t)addr;

    if (!out)
        return -1;
    if (!MM_IS_CANONICAL_4LVL(a))
        return -1;

    __asm__ volatile (
        ".globl __kread_u8_fault\n\t"
        ".globl __kread_u8_fixup\n\t"
        "__kread_u8_fault:\n\t"
        "movzbl (%[a]), %k[v]\n\t"
        "jmp 1f\n\t"
        "__kread_u8_fixup:\n\t"
        "movl $1, %[f]\n\t"
        "1:\n\t"
        : [v]"+r"(val), [f]"+r"(failed)
        : [a]"r"(addr)
        : "memory", "cc");
    if (failed)
        return -1;
    *out = (uint8_t)val;
    return 0;
}

/* Pure routing decision for the __kread_u8 guarded load, factored out of
 * page_fault_handler so the wiring is testable without provoking a real #PF.
 *
 * The end-to-end path (fault -> handler -> fixup -> resumed walk) genuinely
 * needs a real page fault and stays serial-validated, but the DECISION -- exact
 * RIP, read direction only -- is the part that silently rots: a mistyped label
 * or a dropped direction check would still boot, and the guarded read would
 * either stop recovering or start swallowing unrelated kernel write faults.
 *
 * Returns 1 and writes the fixup address when this fault belongs to the guarded
 * load; 0 otherwise, leaving *fixup_out untouched. */
int kread_u8_fixup_lookup(uint64_t rip, int is_write, uint64_t *fixup_out)
{
    extern char __kread_u8_fault[], __kread_u8_fixup[];

    if (is_write || !fixup_out)
        return 0;
    if (rip != (uint64_t)(uintptr_t)__kread_u8_fault)
        return 0;
    *fixup_out = (uint64_t)(uintptr_t)__kread_u8_fixup;
    return 1;
}

int copy_from_user(void *dst, const void *user_src, uint32_t len)
{
    int r;

#ifdef KERNEL_TESTS
    /* fault-inject -- return -1 BEFORE touching user memory so the
     * SMAP STAC/CLAC pair is skipped and the error path is exercised
     * identically to a real user-copy fault. */
    if (copy_user_fault_should_fire())
        return -1;
#endif

    /* Fault-recoverable: a bad user_src returns -1 instead of bugchecking. The
     * #PF is recovered INSIDE __uaccess_copy_from and control returns here
     * normally, so KERNEL_ACCESS_USER_END still clears AC on the fault path. */
    KERNEL_ACCESS_USER_BEGIN();
    r = (__uaccess_copy_from(dst, user_src, (uint64_t)len) == 0) ? 0 : -1;
    KERNEL_ACCESS_USER_END();

    return r;
}

int copy_to_user(void *user_dst, const void *src, uint32_t len)
{
    int r;

#ifdef KERNEL_TESTS
    if (copy_user_fault_should_fire())
        return -1;
#endif

    KERNEL_ACCESS_USER_BEGIN();
    r = (__uaccess_copy_to(user_dst, src, (uint64_t)len) == 0) ? 0 : -1;
    KERNEL_ACCESS_USER_END();

    return r;
}

/* ---- UMIP (User-Mode Instruction Prevention) via CR4.UMIP ---- */

void cpu_enable_umip(void)
{
    if (!cpu_feature_local(CPU_FEATURE_UMIP))   /* AP-local: optional, may be skewed */
        return;

    uint64_t cr4 = read_cr4();
    if (!(cr4 & CR4_UMIP)) {
        write_cr4(cr4 | CR4_UMIP);
        HARDEN_KLOG(LOG_DEBUG, "cpu", "UMIP enabled (CR4.UMIP)");
    }
}

/* ---- PKU (Protection Keys for User-mode) via CR4.PKE ---- */

/* One-shot latch: the FIRST cpu_enable_pku() caller is the BSP in boot_phase0,
 * which runs before smp_init() starts any AP, so the BSP is the only online CPU
 * and its result IS the online-CPU intersection at that moment. Every later
 * caller is an AP inside ap_cpu_harden(), and an AP NEVER writes pku_enabled
 * (see below). Written once by the BSP with no AP running, then read-only. */
static int s_pku_published = 0;

void cpu_enable_pku(void)
{
    int have_pke = 0;

    /* AP-local: PKU is optional and may be skewed. A CPU without it simply
     * contributes have_pke = 0 to the intersection below. */
    if (cpu_feature_local(CPU_FEATURE_PKU)) {
        /* XCR0 bit 9 (PKRU state) must be active on THIS CPU before CR4.PKE.
         * Read the live XCR0, not the BSP-global g_cpu.xcr0_active -- on an AP
         * whose XCR0 was intersected down to a narrower mask, the global would
         * lie and we would set CR4.PKE without the PKRU xstate enabled here. */
        if (!(xcr0_read_safe() & (1UL << 9))) {
            HARDEN_KLOG(LOG_WARN, "cpu", "PKU: XCR0 bit 9 not set on this CPU; skipping CR4.PKE");
        } else {
            uint64_t cr4 = read_cr4();
            if (!(cr4 & CR4_PKE)) {
                write_cr4(cr4 | CR4_PKE);
                HARDEN_KLOG(LOG_DEBUG, "cpu", "PKU enabled (CR4.PKE)");
            }
            /* Set whenever the bit ENDS UP set, not only when this call wrote
             * it: cpu_force_ap_required_cr4() and a re-entered cpu_harden() can
             * both leave CR4.PKE already on, and the old write-only publication
             * then reported "no PKU" for a CPU that has it. */
            have_pke = 1;
        }
    }

    /* pku_enabled means "RDPKRU/WRPKRU are safe on EVERY online CPU" -- the
     * instructions #GP when CR4.PKE is clear on the EXECUTING CPU, so a global
     * set by whichever CPU happened to enable it is unsound (TODO-09 S11).
     *
     * An AP NEVER writes the flag, not even to narrow it. ap_entry() calls
     * ap_cpu_harden() BEFORE the STARTING->ONLINE abandon CAS (smp.c), so an AP
     * the BSP already timed out on still reaches this function and would
     * otherwise store into a global the BSP had already finalized -- a CPU that
     * never publishes is_online is not in the online set and must not move the
     * intersection. The AP's contribution is therefore its cr4_at_boot snapshot,
     * which cpu_features_finalize_global() reads under the is_online acquire
     * edge, making the BSP the sole publisher once bringup is done. */
    {
        /* Publish only from the BSP. The one-shot latch ALONE would rest on call
         * ORDER (boot_hw.c's cpu_harden() running before smp_init() starts any
         * AP) -- true today, but comment-only and silent if that order ever
         * moves. Testing for the BSP makes the rule explicit; the latch then
         * just keeps the publication single. Same smp_this_cpu() idiom
         * cpu_feature_local() above uses: a NULL pc is the pre-GS_BASE BSP in
         * boot_phase0, which is exactly the caller that should publish. */
        struct per_cpu_data *self = smp_this_cpu();
        if ((!self || self->cpu_id == 0) && !s_pku_published) {
            s_pku_published = 1;
            __atomic_store_n(&pku_enabled, have_pke, __ATOMIC_RELEASE);
        }
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
    /* WAITPKG (UMWAIT_CONTROL) is NOT programmed here. cpu_harden() runs in
     * boot_phase0 on the BSP -- before idt_init() -- so a CPUID-gated optional
     * MSR write here could not degrade through a #GP handler (TODO-09 S19). The
     * BSP is programmed post-IDT by cpu_program_bsp_umwait() (called from
     * boot_phase2 on every boot path); each AP writes the bound itself as a
     * computed per-CPU profile entry (the WAITPKG-gated UMWAIT_MAX_DWELL_TSC
     * write in ap_apply_msr_profile(), NOT a BSP snapshot like PAT).
     *
     * PAT is NOT programmed here either: it is owned by a single authoritative write
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
 * Reads back EFER and CR4 and logs any discrepancies. NX is a boot
 * minimum: a failed NX readback halts (boot_halt) instead of warning.
 * BSP-only -- the single caller is boot_phase0 (boot_hw.c). */
void cpu_verify_hardening(void)
{
    uint64_t efer, cr4;

    POST16(0xD900);

    /* Verify NX (EFER.NXE, bit 11) */
    efer = msr_read(MSR_IA32_EFER);
    if (cpu_has(CPU_FEATURE_NX)) {
        if (efer & EFER_NXE) {
            klog(LOG_INFO, "cpu", "Verify: NX enabled (EFER.NXE set)");
        } else {
            /* NX is a boot minimum (boot_hw.c CPU-minimum gate): CPUID
             * reported NX but EFER.NXE did not stick (trapped/ignored
             * WRMSR). vmm_apply_nx_policy() already marked pages NX, so
             * continuing would silently run with NO no-execute enforcement.
             * This readback is the last chance to catch that. BSP-only
             * caller (boot_phase0), so boot_halt owns the halt; LOG_ERROR
             * (not LOG_FATAL) so the styled halt screen still renders. */
            klog(LOG_ERROR, "cpu", "Verify: NX FAILED -- EFER.NXE not set after enable");
            boot_halt("CPU NX activation failed (EFER.NXE not set)");
        }
    }
    POST16(0xD901);

    /* Verify SMEP/SMAP (CR4 bits 20, 21) */
    cr4 = read_cr4();
    /* SMEP/SMAP are skipped on EVERY platform today: hv_supports_cr4_smep_smap()
     * returns 0 because the boot PML4 carries the User bit on all kernel pages,
     * so enabling CR4.SMEP would #PF on kernel code fetch. The skip path is the
     * honest report on all platforms -- there is NO "enforced via EPT" branch:
     * EPT is second-level address translation and does not provide SMEP's
     * supervisor-no-execute-user-page semantics, so claiming Hyper-V enforces
     * SMEP/SMAP via EPT would be a false security signal to an operator. */
    if (cpu_has(CPU_FEATURE_SMEP)) {
        if (cr4 & CR4_SMEP)
            klog(LOG_INFO, "cpu", "Verify: SMEP enabled (CR4.SMEP set)");
        else if (!hv_supports_cr4_smep_smap())
            klog(LOG_INFO, "cpu", "Verify: SMEP skipped (kernel PTE User bit -- needs KPTI)");
        else
            klog(LOG_WARN, "cpu", "Verify: SMEP FAILED -- CR4.SMEP not set");
    }
    POST16(0xD902);

    if (cpu_has(CPU_FEATURE_SMAP)) {
        if (cr4 & CR4_SMAP)
            klog(LOG_INFO, "cpu", "Verify: SMAP enabled (CR4.SMAP set)");
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

/* WAITPKG anti-DoS dwell bound (TODO-09 S19): bits[31:2] = max TSC-quanta a
 * user UMWAIT/TPAUSE may park a logical CPU; bit0 = 0 leaves C0.2 allowed; bit1
 * reserved. 100000 is already 4-aligned, so masking the low 2 bits is a no-op
 * that documents the reserved-bit contract and avoids a #GP. */
#define UMWAIT_MAX_DWELL_TSC  (100000u & ~3u)

struct msr_profile_entry {
    uint32_t    msr;        /* MSR index */
    uint64_t    value;      /* BSP value, filled at record time; ignored if per_cpu */
    const char *name;       /* for the audit log */
    uint32_t    feature;    /* CPU_FEATURE_* gate, or MSR_PROFILE_ALWAYS */
    uint8_t     per_cpu;    /* 1 = value is per-CPU: write cpu_id, not BSP value */
};

static struct msr_profile_entry s_bsp_msr_profile[] = {
    { MSR_IA32_PAT,            0, "PAT",      MSR_PROFILE_ALWAYS, 0 },
    { MSR_IA32_TSC_AUX,        0, "TSC_AUX",  CPU_FEATURE_RDTSCP,  1 },
    { MSR_IA32_UMWAIT_CONTROL, 0, "UMWAIT",   CPU_FEATURE_WAITPKG, 1 },
    { MSR_IA32_SPEC_CTRL,      0, "SPEC_CTRL", CPU_FEATURE_SPEC_CTRL, 1 }, /* S8 eIBRS, computed per-CPU */
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
 * online AP within CPU_FEATURES_AP_PROBE_MASK. The mask itself is a 128-bit
 * struct (no 16-byte atomic on x86), so visibility rides a separate publish
 * flag: cpu_features_finalize_global() writes the words then release-stores
 * s_global_mask_published=1; readers acquire-load the flag before the mask.
 * Empty (all-zero) and unpublished until finalize runs BSP-side. */
static cpu_feature_mask_t s_global_feature_mask;
static volatile int       s_global_mask_published;

/* AP feature-validation fault hand-off (TODO-09-boot S6). An AP that fails the
 * required/vendor/Long-Mode gate cannot bug-check itself (the panic path uses
 * BSP-global XCR0/SIMD that #GP/#UD on a skewed AP), so it records the fault
 * here and halts; the BSP raises the 0x3E bug-check in
 * cpu_features_check_ap_faults() where the panic path is safe. */
static volatile uint32_t s_ap_fault_cpu;     /* 0 = none, else cpu_id + 1 */
static volatile uint64_t s_ap_fault_feat;    /* reasons 1-3: AP probed mask; reason 4: offending CR3 */
static volatile uint32_t s_ap_fault_reason;  /* 1=vendor, 2=Long Mode, 3=required, 4=PCID CR3 invariant */
static volatile uint32_t s_ap_fault_claim;   /* 0 = unclaimed; first faulting AP CASes 0->1 */

/* Record one AP's bringup fault for the BSP (TODO-09-boot S6 + S10). The slot is
 * single-writer: the first faulting AP to CAS s_ap_fault_claim 0->1 owns the
 * record and publishes (cpu_id, detail, reason); later faulting APs still halt
 * but do NOT overwrite the winner's tuple, so the BSP 0x3E payload always names
 * one real, self-consistent fault even when multiple APs fault concurrently.
 * s_ap_fault_cpu is release-stored LAST so cpu_features_check_ap_faults()'s
 * acquire-load sees the detail/reason writes that precede it. */
static void cpu_record_ap_fault(uint32_t cpu_id, uint64_t detail, uint32_t reason)
{
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&s_ap_fault_claim, &expected, 1u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;   /* another AP already owns the slot */
    s_ap_fault_feat   = detail;
    s_ap_fault_reason = reason;
    __atomic_store_n(&s_ap_fault_cpu, cpu_id + 1, __ATOMIC_RELEASE);
}

/* Program the BSP's IA32_UMWAIT_CONTROL anti-DoS bound (TODO-09 S19). Called
 * UNCONDITIONALLY from boot_phase2 (post-IDT) so the bound applies on every boot
 * path -- including a no-ACPI/degraded boot that skips smp_init(). Idempotent
 * (rewrites the same constant). msr_try_write degrades (logs, leaves user dwell
 * unbounded) instead of #GP-panicking if a platform exposes WAITPKG in CPUID but
 * rejects the MSR; the CPUID gate is still the primary existence check. */
void cpu_program_bsp_umwait(void)
{
    if (!cpu_has(CPU_FEATURE_WAITPKG))
        return;
    if (msr_try_write(MSR_IA32_UMWAIT_CONTROL, UMWAIT_MAX_DWELL_TSC) != 0)
        klog(LOG_WARN, "cpu",
             "WAITPKG present but IA32_UMWAIT_CONTROL write rejected; "
             "user UMWAIT/TPAUSE dwell unbounded");
}

/* eIBRS set-once (TODO-10 S8): on a CPU with Enhanced IBRS, set
 * IA32_SPEC_CTRL.IBRS PERMANENTLY (no per-entry toggle). Writes
 * baseline | IBRS so firmware/microcode SPEC_CTRL bits (and S18's future
 * SSBD/STIBP) are preserved. Called UNCONDITIONALLY from boot_phase2 (post-IDT)
 * on the BSP; each AP programs its own via the SPEC_CTRL profile replay. Legacy
 * IBRS (no eIBRS) gets retpoline instead -- no SPEC_CTRL write here. */
void cpu_program_bsp_eibrs(void)
{
    uint64_t base = 0;
    if (!cpu_has(CPU_FEATURE_ENHANCED_IBRS))
        return;
    if (cpu_has(CPU_FEATURE_SPEC_CTRL))
        (void)msr_try_read(MSR_IA32_SPEC_CTRL, &base);   /* baseline, #GP-safe */
    if (msr_try_write(MSR_IA32_SPEC_CTRL, base | SPEC_CTRL_IBRS) != 0)
        klog(LOG_WARN, "cpu",
             "eIBRS present but IA32_SPEC_CTRL write rejected; IBRS not active");
}

/* IBPB writability latch (TODO-10 S8). Starts active; any CPU that lacks IBPB or
 * traps PRED_CMD clears it, so the scheduler hot path is a single atomic load and
 * never #GPs. PRED_CMD is write-only and per-CPU-trap-detectable only by trying. */
static volatile int s_ibpb_active = 1;

/* Probe PRED_CMD writability once per CPU, post-IDT (BSP from boot_phase2, each AP
 * at the ap_cpu_harden tail). cpu_has(IBPB) is the BSP-global CPUID bit; the
 * #GP-safe try-write is the definitive per-CPU usability check (catches both
 * no-MSR and CPUID-says-yes-but-trapped). Issues one harmless IBPB on success. */
void cpu_probe_ibpb(void)
{
    if (!cpu_has(CPU_FEATURE_IBPB) ||
        msr_try_write(MSR_IA32_PRED_CMD, PRED_CMD_IBPB) != 0)
        __atomic_store_n(&s_ibpb_active, 0, __ATOMIC_RELEASE);
}

/* IBPB (TODO-10 S8): flush the indirect branch predictor on a security-domain
 * switch. Gated on the latched s_ibpb_active (probed #GP-safe on every CPU), so
 * this scheduler-hot-path call is a plain wrmsr that cannot fault. */
void cpu_issue_ibpb(void)
{
    if (__atomic_load_n(&s_ibpb_active, __ATOMIC_ACQUIRE))
        msr_write(MSR_IA32_PRED_CMD, PRED_CMD_IBPB);
}

/* MDS/MMIO/RFDS VERW gate (TODO-10 S19). The SYSRET/IRET return-to-user asm paths
 * read this global byte RIP-relative (cmp byte [rel g_mds_verw_active]); 0 = skip
 * the VERW (no leak boundary or no MD_CLEAR), 1 = clear CPU buffers before the
 * ring transition. NON-static: referenced by syscall_entry.asm + isr_stubs.asm.
 * Monotonic: cpu_decide_mds only ever stores 1, so on a skewed SMP set the flag
 * latches on if ANY online CPU needs it (conservative-correct). */
volatile uint8_t g_mds_verw_active = 0;

/* VERW memory operand: a 16-bit selector the exit-path `verw word [rel ...]`
 * reads. VERW's architectural side effect (clearing the CPU buffers) is what
 * matters, not the verify result; GDT_KERNEL_DATA (0x10) is a present, readable
 * descriptor so the verify never faults. Const -> lives in RO `.rodata`. */
const uint16_t g_mds_verw_sel = 0x10;   /* GDT_KERNEL_DATA */

/* Once-per-CPU MDS/TAA decision, post-IDT (BSP from boot_phase2, each AP at the
 * ap_cpu_harden tail). VERW clears CPU buffers only when MD_CLEAR microcode is
 * present AND the CPU is still exposed (not MDS_NO, or RFDS applies). TAA: when
 * the CPU exposes IA32_TSX_CTRL and is not TAA_NO, disable TSX outright (stronger
 * than relying on VERW, and removes the abort side channel) -- a per-CPU MSR write
 * via the #GP-safe try-write, so it re-applies correctly on every AP. */
void cpu_decide_mds(void)
{
    uint32_t a, b, c, d, max_leaf;
    uint64_t caps = 0;
    int has_md_clear, has_arch_cap;

    /* Leaf 7 must exist before reading it -- on a max-leaf < 7 CPU, CPUID 7
     * returns the highest-leaf garbage, which would misread MD_CLEAR/RTM. No
     * leaf 7 -> none of these mitigations are enumerated; gate stays off. */
    cpuid_raw(0x00, 0, &max_leaf, &b, &c, &d);
    if (max_leaf < 0x07)
        return;

    /* AP-local CPUID, NOT cpu_has() (which reflects the BSP-global mask): a
     * feature-skewed AP must decide on its OWN MD_CLEAR / ARCH_CAP so the global
     * VERW gate latches when ANY online CPU needs the clear. Leaf 7,0:EDX[10] =
     * MD_CLEAR, EDX[29] = ARCH_CAPABILITIES present. */
    cpuid_raw(0x07, 0, &a, &b, &c, &d);
    has_md_clear = (int)((d >> 10) & 1);
    has_arch_cap = (int)((d >> 29) & 1);

    if (has_arch_cap)
        (void)msr_try_read(MSR_IA32_ARCH_CAPS, &caps);

    if (has_md_clear) {
        int need = !(caps & ARCH_CAP_MDS_NO) ||
                   ((caps & ARCH_CAP_RFDS_CLEAR) && !(caps & ARCH_CAP_RFDS_NO));
        if (need)
            __atomic_store_n(&g_mds_verw_active, 1, __ATOMIC_RELEASE);
    }

    /* TAA applies ONLY on a TSX (RTM, leaf 7,0:EBX[11]) CPU not marked TAA_NO.
     * Prefer disabling TSX outright via TSX_CTRL; but TSX_CTRL may be absent OR
     * trap the write, in which case TSX stays enabled and the VERW clear is the
     * fallback (it clears TAA-leaked buffers on return-to-user). Gating on
     * TSX_CTRL availability alone would miss the absent-MSR case entirely. */
    if (((b >> 11) & 1) && !(caps & ARCH_CAP_TAA_NO)) {
        int tsx_disabled = 0;
        if (caps & ARCH_CAP_TSX_CTRL) {
            uint64_t tsx = 0;
            (void)msr_try_read(MSR_IA32_TSX_CTRL, &tsx);
            tsx_disabled = (msr_try_write(MSR_IA32_TSX_CTRL,
                            tsx | TSX_CTRL_RTM_DISABLE | TSX_CTRL_CPUID_CLEAR) == 0);
        }
        if (!tsx_disabled) {
            /* Security fallback runs on EVERY CPU: latch VERW so the still-enabled
             * TSX is covered. The diagnostic klog is BSP-only -- AP hardening must
             * emit no serial (klog/serial_write can busy-wait with IRQs masked and
             * perturb the online handshake); the posture report is the AP channel. */
            struct per_cpu_data *pc = smp_this_cpu();
            if (has_md_clear)
                __atomic_store_n(&g_mds_verw_active, 1, __ATOMIC_RELEASE);
            if (!pc || pc->cpu_id == 0)
                klog(LOG_WARN, "cpu", "TAA: TSX not disabled (TSX_CTRL %s); %s",
                     (caps & ARCH_CAP_TSX_CTRL) ? "write rejected" : "absent",
                     has_md_clear ? "VERW fallback active"
                                  : "UNMITIGATED (no MD_CLEAR)");
        }
    }
}

void cpu_record_bsp_profile(void)
{
    uint32_t i;
    struct per_cpu_data *bsp;

    s_bsp_efer = msr_read(MSR_IA32_EFER);
    s_bsp_cr4  = read_cr4();
    s_bsp_pat  = msr_read(MSR_IA32_PAT);
    s_bsp_xcr0 = cpu_has(CPU_FEATURE_XSAVE) ? xcr0_read() : 0;
    s_bsp_required_cr4 = s_bsp_cr4 & CR4_UNIFORM_MASK;

    /* WAITPKG anti-DoS (TODO-09 S19): the BSP's IA32_UMWAIT_CONTROL is NOT
     * programmed here -- it is owned by cpu_program_bsp_umwait(), called
     * UNCONDITIONALLY from boot_phase2 (post-IDT) so the bound applies even on a
     * no-ACPI/degraded boot that never reaches smp_init()/this function. UMWAIT
     * is a computed per-CPU profile entry (a fixed constant, NOT a BSP snapshot),
     * so each AP writes it independently in ap_apply_msr_profile() gated on its
     * OWN WAITPKG -- an AP-only WAITPKG core is bounded regardless of the BSP. */

    /* Freeze the replicated MSR values from the BSP's live MSRs. Per-CPU entries
     * (TSC_AUX, UMWAIT) keep value 0 -- their value is computed on the AP. The
     * feature gate honors the struct's `feature` field for any future non-per_cpu
     * gated entry: reading an MSR the CPU lacks would #GP. */
    for (i = 0; i < MSR_PROFILE_COUNT; i++) {
        if (s_bsp_msr_profile[i].per_cpu)
            continue;
        if (s_bsp_msr_profile[i].feature != MSR_PROFILE_ALWAYS &&
            !cpu_has((enum cpu_feature)s_bsp_msr_profile[i].feature))
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
            !cpu_feature_local((enum cpu_feature)e->feature))   /* AP-local: RDTSCP/TSC_AUX may be skewed (S10) */
            continue;

        if (e->per_cpu) {
            /* TSC_AUX = logical CPU id, only if the BSP probe confirmed it. */
            if (e->msr == MSR_IA32_TSC_AUX) {
                extern int g_tsc_aux_available;
                if (!g_tsc_aux_available)
                    continue;
                msr_write(MSR_IA32_TSC_AUX, (uint64_t)cpu_id);
            } else if (e->msr == MSR_IA32_UMWAIT_CONTROL) {
                /* UMWAIT = a fixed anti-DoS bound, NOT a BSP-snapshotted value
                 * (S19). Write the constant gated on this AP's own WAITPKG (the
                 * loop's cpu_feature_local check above) so an AP-only WAITPKG core
                 * is bounded even when the BSP lacked WAITPKG. msr_try_write
                 * degrades instead of #GP-panicking AP bringup if the MSR is
                 * rejected; record the failure (the BSP surfaces it in
                 * ap_cpu_harden_log) so it is not silent, and skip applied++. */
                if (msr_try_write(MSR_IA32_UMWAIT_CONTROL,
                                  UMWAIT_MAX_DWELL_TSC) != 0) {
                    struct per_cpu_data *upc = smp_get_cpu(cpu_id);
                    if (upc)
                        upc->umwait_unbounded = 1;
                    continue;
                }
            } else if (e->msr == MSR_IA32_SPEC_CTRL) {
                /* eIBRS set-once (S8): the loop gated on this AP's own SPEC_CTRL
                 * (CPUID), but Enhanced IBRS is MSR-derived (ARCH_CAPABILITIES[1]),
                 * so check it HERE on the AP. Write `own baseline | IBRS` (NOT a
                 * BSP snapshot -- that would clobber AP-local SPEC_CTRL bits). All
                 * MSR access is #GP-safe (msr_try_*): a CPUID-says-eIBRS but
                 * MSR-rejects platform records a per-CPU failure (surfaced by the
                 * BSP in ap_cpu_harden_log) rather than #GP-ing AP bringup. A
                 * non-eIBRS AP takes no write (retpoline covers it). */
                uint64_t ac, sc;
                uint32_t a, b, c, d;
                cpuid_raw(7, 0, &a, &b, &c, &d);     /* 7.0:EDX[29] = ARCH_CAP */
                if (!((d >> 29) & 1u))
                    continue;
                if (msr_try_read(MSR_IA32_ARCH_CAPS, &ac) != 0) {
                    /* CPUID advertised ARCH_CAP but the MSR trapped: degraded
                     * platform -- mark it (BSP warns), do not silently skip. */
                    struct per_cpu_data *spc = smp_get_cpu(cpu_id);
                    if (spc)
                        spc->eibrs_unset = 1;
                    continue;
                }
                if (!(ac & ARCH_CAP_IBRS_ALL))
                    continue;       /* legitimately non-eIBRS: silent, retpoline */
                if (msr_try_read(MSR_IA32_SPEC_CTRL, &sc) != 0 ||
                    msr_try_write(MSR_IA32_SPEC_CTRL,
                                  sc | SPEC_CTRL_IBRS) != 0) {
                    struct per_cpu_data *spc = smp_get_cpu(cpu_id);
                    if (spc)
                        spc->eibrs_unset = 1;
                    continue;
                }
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

int cpu_feature_global_has(enum cpu_feature feature)
{
    /* Acquire the publish flag; the release in cpu_features_finalize_global
     * orders the mask words before it. Unpublished -> not yet known -> absent. */
    if (!__atomic_load_n(&s_global_mask_published, __ATOMIC_ACQUIRE))
        return 0;
    return cpu_feature_test(&s_global_feature_mask, feature);
}

void cpu_validate_ap_features(uint32_t cpu_id)
{
    extern struct cpu_features g_cpu;
    struct per_cpu_data *pc = smp_get_cpu(cpu_id);
    uint32_t eax, ebx, ecx, edx, max_leaf;
    cpu_feature_mask_t ap_feat;
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
        !cpu_feature_test(&ap_feat, CPU_FEATURE_LM) ||
        !cpu_feature_subset(CPU_FEATURES_REQUIRED_MASK, ap_feat)) {
        /* detail = low word: every required-baseline feature is bit < 64. */
        cpu_record_ap_fault(cpu_id, ap_feat.w[0],
                            !vendor_ok ? 1u
                          : !cpu_feature_test(&ap_feat, CPU_FEATURE_LM) ? 2u : 3u);
        for (;;)
            __asm__ volatile ("cli; hlt");
    }

    /* Optional skew within the probed subset: any optional feature the BSP has
     * that this AP lacks. Flag only (no AP serial output); the BSP logs it via
     * ap_cpu_harden_log() and the global intersection (finalize) prevents
     * kernel-wide reliance on it. */
    {
        cpu_feature_mask_t bsp_opt = cpu_feature_andnot(
            cpu_feature_and(g_cpu.flags, CPU_FEATURES_AP_PROBE_MASK),
            CPU_FEATURES_REQUIRED_MASK);
        /* mismatch when some BSP optional feature is absent on this AP */
        if (pc && !cpu_feature_subset(bsp_opt, ap_feat))
            pc->feature_mismatch = 1;
    }
}

/* Is this slot part of the set the global intersection must cover?
 *
 * COMMITTED-online, not merely published-online: an AP that won the
 * STARTING->ONLINE CAS is going live even if its is_online release store has not
 * landed yet, and smp_init() waits only a BOUNDED 100 ms for that store
 * (smp.c:507) before continuing. An is_online-only test therefore omits a
 * stalled-but-committed AP, which then comes online under a mask that never
 * intersected it. The AP's ap_cpu_harden() snapshot (features, cr4_at_boot)
 * precedes its ACQ_REL CAS, so an acquire load of EITHER publication orders
 * those writes for the caller. That is a SECOND publication edge beside
 * is_online: the AP's ACQ_REL CAS releases the same prior writes.
 *
 * is_online has PRECEDENCE over ap_bringup_state, so a slot reporting both
 * is_online and ABANDONED returns 1. The bringup CAS makes that combination
 * unreachable (exactly one of {AP-ONLINE, BSP-ABANDONED} wins, and the AP
 * publishes is_online only after winning), and is_online-wins is the SAFE
 * precedence regardless: a CPU that published is_online IS live, and dropping
 * it from a capability intersection would publish a capability it may not have.
 * ABANDONED-without-is_online and never-started slots are excluded.
 *
 * This predicate is deliberately NOT the one smp_init() counts total_cpus with
 * or that cpu_audit_consistency_check() filters on -- those use is_online alone
 * and should. The directions differ: admitting a committed-but-unpublished AP
 * to an INTERSECTION is conservative (it can only narrow the published
 * capability set), while admitting it to a COUNT that gates scheduling and IPI
 * targeting would be optimistic about a CPU that may not service interrupts yet.
 *
 * Extracted from cpu_features_finalize_global() so the shipped reduction and the
 * unit test run the SAME predicate over synthetic slots rather than two copies
 * of the rule (TODO-09 S11 review round 4). */
int cpu_slot_committed_online(const struct per_cpu_data *pc)
{
    if (!pc)
        return 0;
    if (__atomic_load_n(&pc->is_online, __ATOMIC_ACQUIRE))
        return 1;
    return __atomic_load_n(&pc->ap_bringup_state, __ATOMIC_ACQUIRE) == AP_BRINGUP_ONLINE;
}

void cpu_features_finalize_global(void)
{
    extern struct cpu_features g_cpu;
    cpu_feature_mask_t m = cpu_feature_and(g_cpu.flags, CPU_FEATURES_AP_PROBE_MASK); /* BSP base */
    uint32_t i;

    /* CR4.PKE intersection (TODO-09 S11). Consumers (pku.c, task.c) execute
     * RDPKRU/WRPKRU, which #GP when CR4.PKE is clear on the EXECUTING CPU, so
     * pku_enabled must describe the ONLINE-CPU INTERSECTION of the LIVE bit --
     * not the CPUID feature and not "some CPU enabled it". This runs on the BSP,
     * so the BSP's CR4 is read live; each online AP contributes its cr4_at_boot
     * snapshot, published under the same is_online release edge that orders
     * pc->features below. */
    int pke_all = (read_cr4() & CR4_PKE) ? 1 : 0;

    /* AND in every COMMITTED-online AP's published features. Scan ALL slots, not
     * smp_cpu_count(): that returns the dense online COUNT (1 + online), so on
     * a sparse online set (e.g. AP1 timed out, AP2 online) a count-bounded loop
     * would skip the higher-id online AP and publish an over-broad mask.
     * Never-started slots are zeroed BSS.
     *
     * COMMITTED-online, not merely published-online: an AP that won the
     * STARTING->ONLINE CAS is going live even if its is_online release store has
     * not landed yet, and smp_init() waits only a BOUNDED 100 ms for that store
     * (smp.c:507) before continuing. An is_online-only test therefore omits a
     * stalled-but-committed AP, which then comes online under a mask that never
     * intersected it -- over-broad for both the feature set and CR4.PKE. Reading
     * ap_bringup_state closes that window: the AP's ap_cpu_harden() snapshot
     * (features, cr4_at_boot) precedes its ACQ_REL CAS, so an acquire load of
     * EITHER publication orders those writes here. ABANDONED slots are excluded
     * -- that AP parks dark and never goes live. */
    for (i = 1; i < MAX_CPUS; i++) {
        struct per_cpu_data *pc = smp_get_cpu(i);
        if (!cpu_slot_committed_online(pc))
            continue;
        m = cpu_feature_and(m, pc->features);
        if (!(pc->cr4_at_boot & CR4_PKE))
            pke_all = 0;
    }

    /* xstate-dependent features are usable only if the OS enabled the backing
     * XCR0 component -- CPUID presence alone is not enough (e.g. AVX-512 cleared
     * by simd_enable_avx512()'s throttle guard). The published mask promises
     * "safe to USE on every online CPU", so clear any xstate feature whose XCR0
     * component is not active. XCR0 is uniform across CPUs (ap_apply_xcr0
     * replicates the BSP mask), so the BSP's g_cpu.xcr0_active is authoritative. */
    {
        uint64_t xcr0 = g_cpu.xcr0_active;
        if (xcr0 == 0)                                cpu_feature_clear(&m, CPU_FEATURE_XSAVE);
        if (!(xcr0 & (1ULL << 2)))                    cpu_feature_clear(&m, CPU_FEATURE_AVX);
        if ((xcr0 & (7ULL << 5)) != (7ULL << 5))      cpu_feature_clear(&m, CPU_FEATURE_AVX512F);
        if (!(xcr0 & (1ULL << 9)))                    cpu_feature_clear(&m, CPU_FEATURE_PKU);
    }

    /* Authoritative pku_enabled publication (TODO-09 S11): every online CPU has
     * CR4.PKE AND the intersected mask still carries PKU (the XCR0 bit-9 clear
     * above already folded in the xstate requirement). This can only NARROW what
     * cpu_enable_pku() published on the BSP, so a consumer that read 1 earlier
     * was correct for the online set that existed then. Release-store pairs with
     * the acquire loads in pku.c / task.c.
     *
     * The 1 -> 0 transition cannot strand live PKRU state. TWO shapes were
     * checked, not just the obvious one:
     *   - an allocated protection key: pku_alloc_key() has no caller outside
     *     pku.c and the test suite, so none can exist here;
     *   - PKRU already stamped into a per-task XSAVE area: task.c sets
     *     XSTATE_BV bit 9 whenever it read pku_enabled as 1, and XRSTOR faults
     *     if XSTATE_BV bit 9 is set on a CPU whose XCR0 lacks it. Unreachable
     *     on this boot order -- XSAVE areas are allocated lazily on first FPU
     *     use and preemption only starts in Phase 3, both strictly after this
     *     runs inside smp_init().
     * A future consumer of either kind that can run BEFORE smp_init() must
     * revoke on this edge rather than rely on those orderings.
     *
     * The published value is only as sound as single-writer ownership of the
     * slots it reduces: the S10 impostor window (ap_cpu_harden at smp.c:160
     * runs before the LAPIC-identity park at smp.c:176) can put a foreign CPU's
     * cr4_at_boot in a slot. Pre-existing and shared with the feature mask
     * above; owned by S10's AP_DATA consume-ack handshake, NOT closed here. */
    if (!cpu_feature_test(&m, CPU_FEATURE_PKU))
        pke_all = 0;
    if (!pke_all && __atomic_load_n(&pku_enabled, __ATOMIC_ACQUIRE))
        klog(LOG_WARN, "smp", "PKU disabled kernel-wide: CR4.PKE is not set on every online CPU");
    __atomic_store_n(&pku_enabled, pke_all, __ATOMIC_RELEASE);

    /* Publish the words, then release the flag so readers (acquire) see a fully
     * written mask. No 16-byte atomic on x86, hence the flag instead of an
     * atomic store of the struct. */
    s_global_feature_mask = m;
    __atomic_store_n(&s_global_mask_published, 1, __ATOMIC_RELEASE);
    {
        cpu_feature_mask_t pm = CPU_FEATURES_AP_PROBE_MASK;
        klog(LOG_INFO, "smp",
             "Global CPU feature intersection 0x%lx:%lx (probe mask 0x%lx:%lx)",
             m.w[1], m.w[0], pm.w[1], pm.w[0]);
    }
}

/* BSP-side: if any AP recorded a feature-validation fault and halted, raise the
 * 0x3E bug-check here (panic path is safe on the BSP). Call after SMP bringup. */
void cpu_features_check_ap_faults(void)
{
    uint32_t c = __atomic_load_n(&s_ap_fault_cpu, __ATOMIC_ACQUIRE);
    if (!c)
        return;
    /* LOG_ERROR, NOT LOG_FATAL: LOG_FATAL halts in an hlt loop (klog.c), which
     * would pre-empt the KeBugCheckEx below and lose the structured 0x3E
     * bugcheck payload. KeBugCheckEx owns the fatal path. (Reachable for a
     * genuinely-absent required feature like CX16, not just long-mode prereqs.) */
    klog(LOG_ERROR, "smp",
         "[AP%u] validation FAILED (reason %u: 1=vendor 2=LongMode 3=required 4=PCID-CR3; feat/cr3=0x%lx)",
         (uint64_t)(c - 1), (uint64_t)s_ap_fault_reason, s_ap_fault_feat);
    KeBugCheckEx(BUGCHECK_MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED,
                 (uint64_t)(c - 1), s_ap_fault_feat,
                 CPU_FEATURES_REQUIRED_MASK.w[0], (uint64_t)s_ap_fault_reason);
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

    /* Force CR0.WP on before capturing the pin mask. Kernel-image W^X
     * (STRICT_KERNEL_RWX) needs supervisor-mode writes to honor read-only kernel
     * PTEs; with WP clear, ring-0 ignores the PTE WRITABLE bit and .text/.rodata
     * stay writable even after vmm_set_ro. UEFI may hand off with WP clear, so
     * set it here (on the BSP and on every AP, which also runs this) so WP is
     * both active and pinned -- never left to firmware state. */
    uint64_t cr0 = read_cr0();
    if (!(cr0 & CR0_WP))
        write_cr0(cr0 | CR0_WP);

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

int cpu_wp_enforced(void)
{
    return (read_cr0() & CR0_WP) != 0;
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

/* CR-pin verify-IPI (TODO-09-boot S10). AP LAPIC timers are masked, so S7 only
 * re-verified an AP at its ap_cpu_harden() tail + on #GP. This closes that gap:
 * the BSP's cpu_cr_pin_tick() broadcasts IPI_VECTOR_CR_VERIFY to every online AP,
 * which re-runs the pin verify in the IPI handler. Ready only after SMP bringup
 * registers the handler (cpu_cr_verify_ipi_init); cleared/0 disables it. */
static volatile int s_cr_verify_ipi_ready;

/* AP-side handler: re-verify this CPU's CR0/CR4 pins, then EOI. Minimal by
 * design -- the verifiers only read CR0/CR4 and, on a real violation, record
 * the fault + halt this AP (cr_pin_violation); the BSP raises the bug-check
 * from cpu_cr_pin_check() on its next tick. No klog on the match path. */
static uint64_t cr_verify_ipi_handler(struct interrupt_frame *frame)
{
    cr0_verify_pinned();
    cr4_verify_pinned();
    lapic_eoi();
    return (uint64_t)frame;
}

/* Register the verify-IPI handler and arm the broadcast. Called once by the BSP
 * after SMP bringup (APs online, IDT live). The handler lives in the shared
 * handlers[] table, so one registration covers every CPU's ISR dispatch. */
void cpu_cr_verify_ipi_init(void)
{
    idt_register_handler(IPI_VECTOR_CR_VERIFY, cr_verify_ipi_handler);
    __atomic_store_n(&s_cr_verify_ipi_ready, 1, __ATOMIC_RELEASE);
    klog(LOG_INFO, "cpu",
         "CR-pin verify-IPI armed (vector 0x%x)",
         (uint64_t)IPI_VECTOR_CR_VERIFY);
}

/* Rotating cursor over AP slots [1, MAX_CPUS) so the verify-IPI covers every
 * online AP fairly (TODO-09-boot S10). */
static uint32_t s_cr_verify_cursor;

/* Send a verify-IPI to ONE online AP per timer tick, round-robin (BSP-only,
 * from the timer ISR). ISR-SAFE: lapic_send_ipi_nowait() does a single
 * delivery-status check and never spins. Sending to exactly one AP per tick
 * (advancing the cursor) guarantees fairness -- a fixed slot-1-first scan would
 * systematically starve higher APs whenever an earlier send leaves ICR
 * delivery-status pending. Over n_online ticks every online AP is re-verified;
 * the verify is periodic defense-in-depth, so the per-AP cadence is fine. */
static void cpu_cr_verify_broadcast(void)
{
    uint32_t tries;
    for (tries = 1; tries < MAX_CPUS; tries++) {
        uint32_t slot = 1u + (s_cr_verify_cursor++ % (uint32_t)(MAX_CPUS - 1));
        struct per_cpu_data *pc = smp_get_cpu(slot);
        if (pc && __atomic_load_n(&pc->is_online, __ATOMIC_ACQUIRE)) {
            lapic_send_ipi_nowait((uint8_t)pc->lapic_id, IPI_VECTOR_CR_VERIFY);
            return;  /* one IPI per tick; next tick continues from the cursor */
        }
    }
}

/* BSP periodic hook (fired from the LAPIC timer ISR; AP LAPIC timers are
 * masked so this is BSP-only): verify the BSP's own pins, poll AP faults, and
 * broadcast a re-verify IPI to the online APs (once armed). */
void cpu_cr_pin_tick(void)
{
    cr0_verify_pinned();
    cr4_verify_pinned();
    cpu_cr_pin_check();
    if (__atomic_load_n(&s_cr_verify_ipi_ready, __ATOMIC_ACQUIRE))
        cpu_cr_verify_broadcast();
}

/* Force the BSP required-CR4 bits this AP can prove it supports (TODO-09-boot
 * S10) -- closes S4's warn-only gap (the BSP verified the uniform mask but
 * never forced it). Runs AFTER cpu_validate_ap_features() so pc->features is
 * populated, and BEFORE cpu_pin_control_regs() so the forced bits are pinned.
 *
 * Safety: forced = s_bsp_required_cr4 & ap_forceable. s_bsp_required_cr4 only
 * contains bits the BSP actually set (so globally-disabled SMEP/SMAP are not in
 * it), and ap_forceable is restricted to the CR4 bits whose CPUID feature is in
 * the S6 AP probe mask -- FSGSBASE/CET are NOT probed, so they are excluded and
 * stay owned by their feature enables. The intersection is provably safe to OR
 * on this AP. Routed through cr4_write_safe() to preserve the S7 pins. */
/* Pure mapping: which CR4 bits an AP with `features` (S6 AP-probe-mask layout)
 * can safely have forced. ONLY the CR4 bits whose CPUID feature is in the AP
 * probe mask appear here -- CR4_FSGSBASE and CR4_CET are deliberately EXCLUDED
 * because the AP probe does not cover them, so forcing them on a skewed AP
 * could #GP. Extracted (and exported under KERNEL_TESTS) so the exclusion is
 * unit-testable. */
uint64_t cpu_ap_forceable_cr4(cpu_feature_mask_t features)
{
    uint64_t forceable = 0;
    if (cpu_feature_test(&features, CPU_FEATURE_XSAVE)) forceable |= CR4_OSXSAVE;
    if (cpu_feature_test(&features, CPU_FEATURE_UMIP))  forceable |= CR4_UMIP;
    if (cpu_feature_test(&features, CPU_FEATURE_SMEP))  forceable |= CR4_SMEP;
    if (cpu_feature_test(&features, CPU_FEATURE_SMAP))  forceable |= CR4_SMAP;
    if (cpu_feature_test(&features, CPU_FEATURE_PCID))  forceable |= CR4_PCIDE;
    if (cpu_feature_test(&features, CPU_FEATURE_PKU))   forceable |= CR4_PKE;
    return forceable;
}

uint64_t cpu_ap_forceable_cr4_live(cpu_feature_mask_t features, uint64_t xcr0, uint64_t cr3)
{
    uint64_t forceable = cpu_ap_forceable_cr4(features);
    /* CR4.PKE needs the PKRU xstate live in XCR0 (bit 9), not just the PKU
     * CPUID bit -- cpu_enable_pku() refuses PKE without it for the AP-XCR0-skew
     * case (CPUID reports PKU but XSAVE leaf 0x0D lacks PKRU state, so
     * ap_apply_xcr0() never set bit 9). Forcing PKE there would #GP. Drop it. */
    if (!(xcr0 & (1ULL << 9)))
        forceable &= ~(uint64_t)CR4_PKE;
    /* CR4.PCIDE may be set only when CR3[11:0] == 0 (Intel SDM 4.10.1), else
     * #GP. cpu_pcid_enable() treats a nonzero low CR3 as BOOT_FATAL and does NOT
     * set PCIDE; the force path must mirror that refusal rather than re-OR it. */
    if (cr3 & 0xFFFULL)
        forceable &= ~(uint64_t)CR4_PCIDE;
    return forceable;
}

static void cpu_force_ap_required_cr4(void)
{
    struct per_cpu_data *pc = smp_this_cpu();
    uint64_t forced, cr3;

    if (!pc)
        return;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    forced = s_bsp_required_cr4 &
             cpu_ap_forceable_cr4_live(pc->features, xcr0_read_safe(), cr3);
    if (forced)
        cr4_write_safe(read_cr4() | forced);
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
     * AP emits no serial output before it is marked online. A BOOT_FATAL return
     * means CR3[11:0] != 0 -- real CR3 corruption, NOT a legit BOOT_DEGRADED skip
     * (PCID absent / BSP left PCIDE clear). cpu_force_ap_required_cr4() below now
     * drops PCIDE for exactly this CR3 state, so the AP would otherwise come
     * online with a corrupt CR3 and the fatal invariant downgraded to a warn-only
     * CR4 mismatch (the HARDEN_KLOG error is suppressed in this quiet bracket).
     * Instead halt: balance the quiet depth, record the fault for the BSP
     * (reason 4, s_ap_fault_feat = offending CR3) and spin -- mirroring the S6
     * feature-fault handoff (the AP cannot self-bug-check; the panic path uses
     * BSP-global XCR0/SIMD that would #GP/#UD here). The AP never publishes
     * is_online; cpu_features_check_ap_faults() raises 0x3E on the BSP. */
    if (cpu_pcid_enable() == BOOT_FATAL) {
        uint64_t bad_cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(bad_cr3));
        __atomic_fetch_sub(&s_harden_quiet_depth, 1, __ATOMIC_RELAXED);
        cpu_record_ap_fault(cpu_id, bad_cr3, 4u);
        for (;;)
            __asm__ volatile ("cli; hlt");
    }
    /* Force the AP-proven subset of the BSP required-CR4 mask (S10): guarantees
     * this AP matches the BSP's uniform CR4 state for every bit it supports,
     * closing S4's verify-only gap. Done before cpu_pin_control_regs() so the
     * forced bits get pinned. */
    cpu_force_ap_required_cr4();
    __atomic_fetch_sub(&s_harden_quiet_depth, 1, __ATOMIC_RELAXED);

    /* 3. Replay the BSP MSR profile (PAT verbatim, TSC_AUX/UMWAIT/SPEC_CTRL per-CPU). */
    applied = ap_apply_msr_profile(cpu_id);

    /* 3b. IBPB writability latch (S8): probe PRED_CMD on this AP (post-IDT, #GP-safe)
     *     so the scheduler IBPB hot path stays fault-free; clears the latch globally
     *     if this AP lacks IBPB or traps PRED_CMD. */
    cpu_probe_ibpb();

    /* 3c. MDS VERW gate + TAA TSX-disable (S19): re-decide per-AP (reads this AP's
     *     IA32_ARCH_CAPABILITIES, applies its own TSX_CTRL; latches g_mds_verw_active
     *     on if this AP needs the clear). */
    cpu_decide_mds();

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

    /* S19: WAITPKG present on this AP but its UMWAIT_CONTROL write was rejected
     * -- the anti-DoS dwell bound is not applied on this core. Surfaced here
     * (BSP-side) so the degrade is not silent (mirrors the BSP's own warn in
     * cpu_program_bsp_umwait). Warn-only: an optional feature, not a halt. */
    if (pc->umwait_unbounded)
        klog(LOG_WARN, "smp",
             "[AP%u] WAITPKG present but UMWAIT_CONTROL write rejected; "
             "user UMWAIT/TPAUSE dwell unbounded", (uint64_t)cpu_id);
    if (pc->eibrs_unset)
        klog(LOG_WARN, "smp",
             "[AP%u] eIBRS present but IA32_SPEC_CTRL write rejected; "
             "IBRS not active on this AP (retpoline still applies)", (uint64_t)cpu_id);

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
            cpu_feature_mask_t bsp_opt = cpu_feature_andnot(
                cpu_feature_and(g_cpu.flags, CPU_FEATURES_AP_PROBE_MASK),
                CPU_FEATURES_REQUIRED_MASK);
            cpu_feature_mask_t missing = cpu_feature_andnot(bsp_opt, pc->features);
            klog(LOG_WARN, "smp",
                 "[AP%u] FEATURE MISMATCH: BSP optional 0x%lx:%lx, AP 0x%lx:%lx, missing 0x%lx:%lx (core_type 0x%x)",
                 (uint64_t)cpu_id, bsp_opt.w[1], bsp_opt.w[0],
                 pc->features.w[1], pc->features.w[0],
                 missing.w[1], missing.w[0], (uint64_t)pc->core_type);
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
