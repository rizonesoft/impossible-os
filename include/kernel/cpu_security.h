/* ============================================================================
 * cpu_security.h -- CPU security feature activation
 *
 * Functions to enable NX, SMEP, SMAP, CET, and other CPU security features.
 * Each is safe to call on BSP and APs. No-ops if the feature is unsupported.
 *
 * XREF: 02-kernel-core/TODO-10-kernel-security-hardening.md
 * ============================================================================ */

#pragma once

#include "kernel/cpuid.h"

/* Enable NX (No-Execute) bit via EFER.NXE.
 * Must be called before any PTE NX bits are set. */
void cpu_enable_nx(void);

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
 * cpu_harden_post_pagetable(). Reads back EFER/CR4 and logs discrepancies. */
void cpu_verify_hardening(void);

/* Emit a single consolidated `[Phase0] CPU security <phase_label>: EFER=...
 * CR4=... NX=N UMIP=N PKU=N SMEP=N SMAP=N` line. BSP-only; intended for the
 * boot_phase0 activation sequence so log readers can see what was enabled
 * before/after the VMM walk in one structured line. Owner: 01-boot-platform
 * TODO-09 cpu-boot-sequencing activation-order section. */
void cpu_security_log_state(const char *phase_label);

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

/* Copy len bytes from user-space to kernel buffer. SMAP-safe. */
int copy_from_user(void *dst, const void *user_src, uint32_t len);

/* Copy len bytes from kernel buffer to user-space. SMAP-safe. */
int copy_to_user(void *user_dst, const void *src, uint32_t len);

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
