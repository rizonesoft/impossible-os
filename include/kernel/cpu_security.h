/* ============================================================================
 * cpu_security.h -- CPU security feature activation
 *
 * Functions to enable NX, SMEP, SMAP, CET, and other CPU security features.
 * Enable functions are safe to call on BSP and APs and no-op if the feature
 * is unsupported. EXCEPTIONS are marked per-prototype: cpu_verify_hardening()
 * is BSP-only and halts boot (boot_halt) on NX readback failure.
 *
 * XREF: 02-kernel-core/TODO-10-kernel-security-hardening.md
 * ============================================================================ */

#pragma once

#include "kernel/cpuid.h"
#include "kernel/boot_init.h"   /* boot_result_t for the BOOT_STEP activation fns */

/* Enable NX (No-Execute) bit via EFER.NXE.
 * Must be called before any PTE NX bits are set. */
void cpu_enable_nx(void);

/* Phase 1 XSAVE finalize (TODO-09-boot S5). BSP-only. The XCR0 base mask is
 * configured back in Phase 0 by cpu_configure_xcr0() (cpuid_init) because the
 * Phase-0 SIMD and PKU paths need it; per-thread XSAVE areas come from
 * pmm_alloc_contiguous(), not VMM/TEB, so XSAVE has no real VMM dependency.
 * This finalize runs AFTER simd_enable_avx512()'s throttle guard so it records
 * the FINAL XCR0 mask + xsave area size and logs them. It does NOT re-run
 * XSETBV (that would re-enable any xstate the throttle guard cleared).
 * Returns: BOOT_OK when XSAVE was finalized (feature present); BOOT_DEGRADED
 * when the CPU lacks XSAVE (caller marks the subsystem ready but emits no
 * *_ENABLED milestone). */
boot_result_t cpu_xsave_enable(void);

/* CR4.PCIDE activation window (TODO-09-boot S5). Safe on BSP and APs; gated on
 * the CALLING CPU's own CPUID PCID bit (leaf 1 ECX[17]) so a feature-skewed AP
 * never #GPs setting a reserved CR4 bit. Sets CR4.PCIDE only when CR3[11:0]==0.
 * PCID stays 0 everywhere (legacy TLB behavior) -- per-process PCID tagging +
 * NOFLUSH CR3 is owned by TODO-10 S7 (blocked on KPTI). Logs via HARDEN_KLOG so
 * the AP-bringup path stays serial-silent.
 * Returns: BOOT_OK when CR4.PCIDE was set (feature present, invariant holds);
 * BOOT_DEGRADED for a legit skip (CPU lacks PCID, or an AP mirrors a BSP that
 * did not enable it) -- ready, no milestone; BOOT_FATAL when the CR3[11:0]==0
 * invariant is violated (caller leaves the subsystem not-ready + degraded). */
boot_result_t cpu_pcid_enable(void);

/* Enable SMEP (Supervisor Mode Execution Prevention) via CR4.SMEP.
 * Prevents kernel from executing user-mode pages. */
void cpu_enable_smep(void);

/* Enable SMAP (Supervisor Mode Access Prevention) via CR4.SMAP.
 * Prevents kernel from reading/writing user-mode pages without CLAC/STAC. */
void cpu_enable_smap(void);

/* Enable UMIP (User-Mode Instruction Prevention) via CR4.UMIP.
 * Prevents ring-3 from executing SGDT/SIDT/SLDT/SMSW/STR (#GP on attempt).
 * Eliminates kernel address leaks from user-mode. */
void cpu_enable_umip(void);

/* Enable PKU (Protection Keys for User-mode) via CR4.PKE.
 * Requires XCR0 bit 9 (PKRU state) to be set by cpu_configure_xcr0().
 * PKU only affects ring-3 data accesses; kernel is unaffected. */
void cpu_enable_pku(void);

/* Program PAT MSR entry 1 = WC (Write-Combining) on the current CPU.
 * PAT is per-CPU; must be called on BSP and each AP. Without this,
 * vmm_map_mmio_wc() pages get WT instead of WC (Intel default). */
void cpu_configure_pat(void);

/* Enable NX, UMIP, PKU, and program PAT for the current core.
 * Call on BSP in Phase 0 and on each AP during SMP bringup. */
void cpu_harden(void);

/* Enable SMEP/SMAP after page tables have U/S cleared from kernel pages.
 * Must be called AFTER vmm_apply_nx_policy(). */
void cpu_harden_post_pagetable(void);

/* Verify CPU security features are active. Call after cpu_harden() +
 * cpu_harden_post_pagetable(). Reads back EFER/CR4 and logs discrepancies.
 * NX readback failure halts boot (NX is a boot minimum). BSP-only. */
void cpu_verify_hardening(void);

/* Freeze the BSP's final boot-time register/MSR baseline (EFER, CR4, PAT,
 * XCR0) and snapshot the per-CPU MSR replay profile values. Call once on
 * the BSP after all Phase 0/1 xstate mutations and before SMP bringup so
 * every AP replicates the BSP's final state rather than a recomputed-from-
 * CPUID approximation. */
void cpu_record_bsp_profile(void);

/* Program the BSP's IA32_UMWAIT_CONTROL anti-DoS dwell bound (TODO-09 S19).
 * Call once on the BSP, post-IDT, on EVERY boot path (incl. no-ACPI/degraded
 * boots that skip smp_init()). Idempotent; CPUID-gated; degrades (warns, leaves
 * dwell unbounded) on a rejected MSR rather than #GP-panicking. */
void cpu_program_bsp_umwait(void);

/* Program the BSP's eIBRS set-once IA32_SPEC_CTRL.IBRS (TODO-10 S8). Call once
 * on the BSP, post-IDT, on every boot path. No-op without Enhanced IBRS (legacy
 * CPUs use retpoline). APs program their own via the SPEC_CTRL profile replay. */
void cpu_program_bsp_eibrs(void);

/* Probe PRED_CMD/IBPB writability once per CPU, post-IDT, to latch a #GP-safe
 * IBPB-active flag (TODO-10 S8). Call on the BSP + every AP before the scheduler
 * issues IBPB. Any CPU lacking IBPB or trapping PRED_CMD disables it globally. */
void cpu_probe_ibpb(void);

/* Flush the indirect branch predictor (IBPB, TODO-10 S8). Called from the
 * scheduler on a security-domain switch. Gated on the latched probe -- never #GPs. */
void cpu_issue_ibpb(void);

/* Decide the MDS VERW gate + apply TAA TSX-disable for the calling CPU (TODO-10
 * S19). Post-IDT: BSP from boot_phase2, each AP at the ap_cpu_harden tail. Sets
 * the global g_mds_verw_active byte the SYSRET/IRET exit asm gates the VERW on. */
void cpu_decide_mds(void);

/* Replicate the BSP's hardened CPU state onto the calling AP and BUFFER a
 * snapshot -- AP-only state replication, no verification, no serial output:
 * match the BSP XCR0 mask (intersected with this AP's own supported bits),
 * enable the gated security features (NX/UMIP/PKU then SMEP/SMAP, each
 * CPUID-gated), replay the BSP MSR profile, then store this AP's EFER/CR4/
 * PAT/XCR0 snapshot into per_cpu_data. Must run after the AP's GS base is set.
 * Emits NO serial output (AP-side serial I/O with IRQs masked around the
 * online transition would delay the online signal or leave the AP counted-
 * online-but-not-IPI-ready). The warn-only verification against the BSP
 * baseline and the audit-line emission happen later in ap_cpu_harden_log(),
 * run by the BSP; the controlled bug-check + force-after-validation of any
 * missing CR4 bit are owned by the AP feature consistency validation. */
void ap_cpu_harden(uint32_t cpu_id);

/* Validate the calling AP's CPUID feature set against the BSP baseline
 * (TODO-09-boot S6). Runs at the TOP of ap_cpu_harden() on the AP, before any
 * optional CR4/MSR enable (publish-before-enable). Probes this AP's security-
 * critical feature subset + core type into per_cpu_data; bug-checks
 * (MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED) on vendor mismatch, missing Long
 * Mode, or any missing CPU_FEATURES_REQUIRED_MASK bit. Optional features the
 * BSP has but this AP lacks set per_cpu_data.feature_mismatch (no serial output
 * on the AP; the BSP logs it later via ap_cpu_harden_log). Does NOT mutate the
 * global mask -- that is reduced BSP-side by cpu_features_finalize_global(). */
void cpu_validate_ap_features(uint32_t cpu_id);

/* Reduce the global feature intersection = BSP & every ONLINE AP's published
 * features (within CPU_FEATURES_AP_PROBE_MASK) and publish it once. BSP-only;
 * call AFTER the is_online acquire pass so a late/timed-out AP cannot downgrade
 * an already-published mask. */
void cpu_features_finalize_global(void);

/* 1 when this per-CPU slot belongs to the set the global intersection must
 * cover: it published is_online, OR it won the STARTING->ONLINE bringup CAS and
 * its is_online store has not landed yet (smp_init() waits only a bounded 100 ms
 * for that store). ABANDONED-without-is_online and never-started slots return 0.
 * is_online has PRECEDENCE: a slot reporting both is_online and ABANDONED
 * returns 1. The bringup CAS makes that combination unreachable, and it is the
 * safe precedence anyway -- a CPU that published is_online is live, so dropping
 * it from a capability intersection would over-publish. Acquire-loads both
 * publications, so the slot's ap_cpu_harden() snapshot is ordered for the
 * caller. NOT the predicate for counting online CPUs -- see the definition.
 * NULL-safe. */
struct per_cpu_data;
int cpu_slot_committed_online(const struct per_cpu_data *pc);

/* The published global feature intersection (0 until finalized). Acquire-load.
 * A set bit means the feature is present on EVERY online CPU AND, for
 * xstate-dependent features (AVX/AVX512F/PKU/XSAVE), the OS has enabled the
 * backing XCR0 component -- so it is safe to USE everywhere. Gate cross-CPU
 * feature reliance on this rather than the BSP-only cpu_has(). Returns 0 until
 * cpu_features_finalize_global() publishes the intersection. */
int cpu_feature_global_has(enum cpu_feature feature);

/* BSP-side: raise BUGCHECK_MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED if any AP
 * recorded a feature-validation fault and halted. Call once after SMP bringup;
 * no-op if every AP passed. The bug-check runs on the BSP so the panic path is
 * feature-safe (an AP cannot bug-check itself -- see cpu_validate_ap_features). */
void cpu_features_check_ap_faults(void);

/* ---- CR0/CR4 safety-bit pinning (TODO-09-boot S7) ---- */

/* Pin the CALLING CPU's CR0.WP + enabled CR4 security bits (SMEP/SMAP/UMIP/
 * FSGSBASE/CET) into per_cpu_data and arm enforcement. Per-CPU: each CPU pins
 * only the bits it actually has set, so a feature-skewed AP never bug-checks on
 * a bit it lacks. Called on the BSP at end of Phase 1 (after XSAVE/PCID) and on
 * each AP at the tail of ap_cpu_harden(). */
void cpu_pin_control_regs(void);

/* Pin-aware control-register writes: when pinning is active they force the
 * calling CPU's pinned bits back on, so a write that tries to clear a pinned
 * bit is corrected. All post-pin kernel CR0/CR4 writes should use these. */
void cr0_write_safe(uint64_t val);
void cr4_write_safe(uint64_t val);

/* Verify the calling CPU's pinned CR0/CR4 bits are still set. On the BSP a
 * cleared pin raises BUGCHECK_CRITICAL_STRUCTURE_CORRUPTION directly; on an AP
 * it records the fault + halts and the BSP raises it via cpu_cr_pin_check()
 * (an AP must not run the panic path -- it uses BSP-global XCR0/SIMD). No-op
 * until the calling CPU has pinned. */
void cr0_verify_pinned(void);
void cr4_verify_pinned(void);

/* True when CR0.WP is set on the calling CPU, i.e. supervisor-mode writes honor
 * read-only PTEs. Kernel-image W^X relies on this; the call site fails closed if
 * it returns 0. cpu_pin_control_regs() forces WP on, so this is true after it. */
int cpu_wp_enforced(void);

/* BSP-side: raise the bug-check if any AP recorded a CR-pin violation. */
void cpu_cr_pin_check(void);

/* BSP periodic hook (LAPIC timer ISR; AP timers are masked): verify the BSP's
 * own pins, poll for AP-recorded faults, and broadcast a re-verify IPI to the
 * online APs once armed. */
void cpu_cr_pin_tick(void);

/* Register the CR-pin verify-IPI handler and arm the broadcast (TODO-09-boot
 * S10). BSP, once, after SMP bringup. */
void cpu_cr_verify_ipi_init(void);

/* Pure helper (TODO-09-boot S10): the CR4 bits an AP with `features` (S6
 * AP-probe-mask layout) may have forced. Excludes FSGSBASE/CET (not AP-probed).
 * Exposed for unit testing the exclusion. */
uint64_t cpu_ap_forceable_cr4(cpu_feature_mask_t features);

/* Pure helper (TODO-09-boot S10): cpu_ap_forceable_cr4() gated by this AP's live
 * register state. Two CR4 bits have hardware preconditions beyond CPUID presence
 * that the safe-enable paths enforce and the blind force path must mirror, or it
 * reintroduces the AP-skew #GP this section hardens against:
 *   - CR4.PKE   needs XCR0.PKRU (bit 9) live (mirrors cpu_enable_pku)
 *   - CR4.PCIDE needs CR3[11:0] == 0         (mirrors cpu_pcid_enable; SDM 4.10.1)
 * PKE is dropped when xcr0 bit 9 is clear; PCIDE is dropped when cr3 low 12 bits
 * are nonzero. Exposed for unit testing. */
uint64_t cpu_ap_forceable_cr4_live(cpu_feature_mask_t features, uint64_t xcr0, uint64_t cr3);

/* Verify one AP's buffered snapshot against the BSP baseline (warn-only on
 * EFER.NXE / required-CR4 / PAT drift) and emit its "[AP%u] CPU hardening
 * applied ..." audit line. Called by the BSP for each online AP after SMP
 * bringup, never on the AP itself, to keep unbounded serial I/O off the AP
 * bringup critical path. */
void ap_cpu_harden_log(uint32_t cpu_id);

#ifdef KERNEL_TESTS
/* Test-only read accessors for the BSP per-CPU MSR replay profile. */
struct mtrr_snapshot;  /* fwd decl; full def in kernel/mtrr.h */
uint32_t cpu_msr_profile_count(void);
int      cpu_msr_profile_entry(uint32_t idx, uint32_t *msr_out,
                               uint64_t *value_out, int *per_cpu_out);
uint64_t cpu_bsp_pat_baseline(void);
void     cpu_bsp_mtrr_baseline(struct mtrr_snapshot *out);
#endif

/* Emit a single consolidated `[Phase0] CPU security <phase_label>: EFER=...
 * CR4=... NX=N UMIP=N PKU=N SMEP=N SMAP=N` line. BSP-only; intended for the
 * boot_phase0 activation sequence so log readers can see what was enabled
 * before/after the VMM walk in one structured line. Owner: 01-boot-platform
 * TODO-09 cpu-boot-sequencing activation-order section. */
void cpu_security_log_state(const char *phase_label);

/* CPU register state audit trail (TODO-09-boot S9). cpu_audit_registers()
 * captures the calling CPU's security-relevant registers into per_cpu_data (no
 * serial output -- safe on the quiet AP path); cpu_audit_log() emits the single
 * consolidated `[CPU%u AUDIT]` line BSP-side; cpu_audit_consistency_check()
 * compares all APs against the BSP after bringup; cpu_audit_populate_registry()
 * exposes the snapshots under HKLM\HARDWARE\CPU\%u\Registers (Phase 2, after
 * registry_init()). */
void cpu_audit_registers(uint32_t cpu_id);
void cpu_audit_log(uint32_t cpu_id);
void cpu_audit_consistency_check(uint32_t total_cpus);
void cpu_audit_ensure_bsp(void);
void cpu_audit_populate_registry(void);

/* ---- Panic-safe CPU identity (ARCH: x86-64 -- will move to arch/) ----
 *
 * The 8-bit initial APIC ID from CPUID leaf 1, EBX[31:24]. This is the identity
 * every abort-context consumer must use, and it is deliberately NOT
 * smp_this_cpu()->cpu_id or ->lapic_id:
 *
 *  - smp_this_cpu() reads gs:0 AND falls back to &cpu_data[0] when it is NULL
 *    (smp.c), so an entrant with no valid per-CPU identity silently records
 *    itself as CPU 0 -- after which the REAL CPU 0 also matches.
 *  - per_cpu_data.lapic_id is sourced from the LAPIC ID register on the BSP and
 *    from the ACPI MADT on APs (smp.c), a DIFFERENT derivation. Comparing a
 *    value from that source against one from this helper is the failure mode
 *    these consumers exist to prevent, and it fails SILENTLY.
 *
 * CPUID has no memory operand, so this cannot fault and needs no mapped GS --
 * usable from NMI, #MC and #DF context. It IS a serializing instruction (a VM
 * exit under KVM/WHPX), so hoist it out of per-byte loops; it is not free.
 *
 * Consumers that MUST agree on this exact derivation: the g_serial_lock owner
 * word and the emergency-charge ledger (drivers/serial.c), the NMI nesting
 * depth (idt.c), and the crash-evidence cpu_id (panic.c). One helper, so a
 * change lands on all of them at once.
 *
 * Identity is only ever compared against another value from THIS helper, so the
 * absolute numbering does not matter -- only that it is stable and per-CPU
 * unique. Aliases above 255 logical CPUs; x2APIC systems with more than 255 CPUs
 * are unsupported repo-wide (xAPIC throughout: SIPI target, lapic_id() and
 * cpu_info.apic_id are all 8-bit). */
#define CPU_PANIC_SAFE_ID_MASK  0xFFu
#define CPU_PANIC_SAFE_ID_COUNT (CPU_PANIC_SAFE_ID_MASK + 1u)

/* The DERIVATION, not just its result width. isr_stubs.asm computes this same
 * id inline -- `mov eax,1; xor ecx,ecx; cpuid; shr ebx,24` -- because the NMI
 * entry stub must index the depth counter before any C runs. Naming the leaf,
 * subleaf and shift here means the two encodings are pinned to one another:
 * idt.c static-asserts these three against the values the assembly hardcodes,
 * so changing the derivation on either side breaks the build. Pinning only the
 * 8-bit result width would not: a different leaf that still yields 8 bits would
 * leave the assembly incrementing one CPU's slot while idt_in_nmi() reads
 * another's, and every existing assertion would stay green. */
#define CPU_PANIC_SAFE_ID_LEAF    1u
#define CPU_PANIC_SAFE_ID_SUBLEAF 0u
#define CPU_PANIC_SAFE_ID_SHIFT   24u

static inline uint32_t cpu_panic_safe_apic_id(void)
{
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(CPU_PANIC_SAFE_ID_LEAF), "c"(CPU_PANIC_SAFE_ID_SUBLEAF));
    (void)eax; (void)ecx; (void)edx;
    return (ebx >> CPU_PANIC_SAFE_ID_SHIFT) & CPU_PANIC_SAFE_ID_MASK;
}

/* ---- SMAP user-space access brackets ---- */

/* STAC: Set AC flag -- allows kernel to access user pages (SMAP bypass).
 * CLAC: Clear AC flag -- re-enables SMAP protection.
 * No-ops if SMAP is not active on this platform. */
static inline void stac(void) { __asm__ volatile ("stac" ::: "memory"); }
static inline void clac(void) { __asm__ volatile ("clac" ::: "memory"); }

/* Safe wrappers that check SMAP support before emitting STAC/CLAC.
 * Use these around every intentional user-space memory access. */
#define KERNEL_ACCESS_USER_BEGIN() \
    do { if (cpu_has(CPU_FEATURE_SMAP)) stac(); } while (0)
#define KERNEL_ACCESS_USER_END() \
    do { if (cpu_has(CPU_FEATURE_SMAP)) clac(); } while (0)

/* Copy len bytes from user-space to kernel buffer. SMAP-safe. Fault-recoverable:
 * a bad user_src returns -1 instead of bugchecking (delegates to __uaccess_copy). */
int copy_from_user(void *dst, const void *user_src, uint32_t len);

/* Copy len bytes from kernel buffer to user-space. SMAP-safe. Fault-recoverable:
 * a bad user_dst returns -1 instead of bugchecking (delegates to __uaccess_copy). */
int copy_to_user(void *user_dst, const void *src, uint32_t len);

/* Fault-recoverable byte copies behind a static RIP-keyed exception table.
 * Return bytes NOT copied (0 == full success); a #PF on the user operand is
 * turned into a partial-copy result by page_fault_handler instead of a kernel
 * bugcheck. Separate from/to entry points so the handler can tell a user SOURCE
 * read fault (copy_from) from a user DEST write fault (copy_to) -- see
 * cpu_security.c (no per-CPU state, no cli). */
uint64_t __uaccess_copy_from(void *dst, const void *src, uint64_t n);
uint64_t __uaccess_copy_to(void *dst, const void *src, uint64_t n);

/* Non-destructive single-address write probe (lock orb $0). Returns 0 if the
 * address is present+writable, -1 if the touch faulted. Used by ProbeForWrite. */
int __uaccess_touch_w(void *addr);

/* Fault-recoverable single-QWORD read from a possibly-corrupt/untrusted
 * RESIDENT KERNEL address -- the fault-safe primitive for kernel stack walking
 * (TODO-23 s7 RtlCaptureStackBackTrace and crash-time frame-chain walks). A #PF
 * taken at the guarded load is redirected by page_fault_handler to its fixup
 * (RIP-keyed, no per-CPU state, SMP/preempt-safe), so a bad RBP terminates the
 * walk instead of bugchecking. Unlike __uaccess_* (recovered only for a
 * CR2 < MM_USER_END operand), this targets KERNEL VAs and the handler matches it
 * by RIP alone (read direction), redirected BEFORE the swap/mmap pager. SCOPE: it
 * is for ALWAYS-RESIDENT kernel memory (kernel stacks) reached in a context that
 * can take a #PF -- it does NOT page in swapped/demand/user memory (such a read
 * faults and returns -1, never resolved).
 *
 * CONTEXT: identical to __kread_u8 below, which states the per-context table in
 * full -- #DF and #MC are SAFE, NMI is UNSAFE, and a GS-corrupt panic is outside
 * recovery entirely. This block previously said "recovery is unavailable from a
 * #DF/#MC/NMI abort context", which contradicted that table for the SAME
 * mechanism; the coarse wording was the stale one, and the panic-path frame
 * walker (panic_capture_frames) depends on the #DF case being usable, since a
 * kernel stack overflow arriving as #DF is the motivating reason IST exists.
 * Rejects NULL out, a top-of-address-space wrap, and
 * non-canonical operands up front (those would #GP, not #PF). Returns 0 on
 * success (*out = value read), -1 if it faulted or was rejected (*out untouched). */
int __kstack_read_u64(uint64_t *out, const void *addr);

/* Fault-recoverable single-BYTE read from a possibly-corrupt KERNEL address --
 * the panic path's primitive for walking a caller-supplied C string (a panic
 * description, a __FILE__) that may itself be part of the corruption being
 * reported. Same RIP-keyed mechanism and same KERNEL-VA scope as
 * __kstack_read_u64 above, with its own label pair (the handler matches the
 * exact faulting instruction, so guarded loads cannot share labels). A byte
 * needs no straddle check, so only the address is canonical-tested.
 *
 * CONTEXT, stated precisely because the coarser "no abort context" wording is
 * over-broad and this primitive exists to be used from abort handlers: recovery
 * takes a real #PF and returns through IRETQ.
 *   - #DF / #MC handler: SAFE. Shutdown requires a fault while DELIVERING #DF,
 *     not one taken by a #DF handler that is already running, and the fixup is
 *     matched before the pager so it cannot enter blocking I/O.
 *   - NMI handler: UNSAFE. The fixup IRETQ re-arms NMI delivery while the outer
 *     NMI still owns IST2, so a second NMI reuses that stack and overwrites the
 *     frames. NMI-context callers must NOT use this; they declare their context
 *     instead of probing it (serial.h PANIC_CTX_*).
 *   - GS-CORRUPT panic: recovery does NOT apply. Delivery of the provoked #PF
 *     runs the common ISR entry first, which reads and dereferences `gs:0` to
 *     verify swapgs symmetry (`isr_handler`, idt.c) BEFORE the RIP fixup is ever
 *     consulted, and halts the CPU when that check fails. So a guarded read on a
 *     machine whose GS base is corrupt halts exactly as an unguarded one would.
 *     This is a LIMIT of the guarantee, not a regression -- the same pointer
 *     faulted to the same place before -- but it means "survives a corrupt
 *     pointer" holds only while GS is intact. The pre-arbitration panic dump is
 *     GS-independent in its own accounting and routing; the fault RECOVERY
 *     underneath it is not, and cannot be without an IST-backed #PF gate.
 * Returns 0 on success (*out = byte read), -1 if it faulted or was rejected. */
int __kread_u8(uint8_t *out, const void *addr);

/* The exception-table routing decision for __kread_u8, as a pure function so it
 * can be unit tested: page_fault_handler consults it before the pager. Returns 1
 * and writes the fixup RIP when `rip` is exactly the guarded load and the fault
 * is a READ; 0 otherwise. A write fault at that RIP is NOT ours -- recovering it
 * would swallow an unrelated kernel bug. */
int kread_u8_fixup_lookup(uint64_t rip, int is_write, uint64_t *fixup_out);

/* Exception-table label symbols emitted by the __uaccess_* primitives.
 * page_fault_handler compares the faulting RIP to the *_fault labels and, when
 * the fault direction matches the user operand, redirects to the *_fixup label:
 * copy_from recovers a READ fault, copy_to / touch recover a WRITE fault. */
extern char __uaccess_copy_from_fault[], __uaccess_copy_from_fixup[];
extern char __uaccess_copy_to_fault[], __uaccess_copy_to_fixup[];
extern char __uaccess_touch_fault[], __uaccess_touch_fixup[];

#ifdef KERNEL_TESTS
/* ---- Test-only copy_to_user / copy_from_user fault injection
 * (kernel-test-harness roadmap; mirrors the kmalloc / pmm / vmm_map
 * fault-inject API).
 * Arms a per-CPU countdown that forces the next (or Nth) copy_to_user
 * or copy_from_user to return -1 WITHOUT attempting the real user-
 * space memory access. Syscall tests use this to prove their error
 * paths propagate a user-copy failure without corrupting kernel state.
 *
 * Same gates: PASSIVE_LEVEL only, optional task-pid filter, optional
 * max-injections cap. Released builds compile out. */
void     copy_user_fail_countdown_set(uint32_t n);
void     copy_user_fail_countdown_clear(void);
void     copy_user_fail_next(void);
uint64_t copy_user_fail_injections_triggered(void);
void     copy_user_fail_task_filter_set(uint32_t task_pid);
void     copy_user_fail_task_filter_clear(void);
void     copy_user_fail_max_injections_set(uint32_t max);
void     copy_user_fail_max_injections_clear(void);
uint32_t copy_user_fail_fired_counter(void);
#endif /* KERNEL_TESTS */
