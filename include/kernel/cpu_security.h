/* ============================================================================
 * cpu_security.h -- CPU security feature activation
 *
 * Functions to enable NX, SMEP, SMAP, CET, and other CPU security features.
 * Each is safe to call on BSP and APs. No-ops if the feature is unsupported.
 *
 * XREF: 02-kernel-core/TODO-17-kernel-security-hardening.md
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

/* Enable NX for the current core.
 * Call on BSP in Phase 0 and on each AP during SMP bringup. */
void cpu_harden(void);

/* Enable SMEP/SMAP after page tables have U/S cleared from kernel pages.
 * Must be called AFTER vmm_apply_nx_policy(). */
void cpu_harden_post_pagetable(void);

/* ---- SMAP user-space access brackets ---- */

/* STAC: Set AC flag — allows kernel to access user pages (SMAP bypass).
 * CLAC: Clear AC flag — re-enables SMAP protection.
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
