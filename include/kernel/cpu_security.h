/* ============================================================================
 * cpu_security.h -- CPU security feature activation
 *
 * Functions to enable NX, SMEP, SMAP, CET, and other CPU security features.
 * Each is safe to call on BSP and APs. No-ops if the feature is unsupported.
 *
 * XREF: 02-kernel-core/TODO-17-kernel-security-hardening.md
 * ============================================================================ */

#pragma once

/* Enable NX (No-Execute) bit via EFER.NXE.
 * Must be called before any PTE NX bits are set. */
void cpu_enable_nx(void);

/* Enable SMEP (Supervisor Mode Execution Prevention) via CR4.SMEP.
 * Prevents kernel from executing user-mode pages. */
void cpu_enable_smep(void);

/* Enable SMAP (Supervisor Mode Access Prevention) via CR4.SMAP.
 * Prevents kernel from reading/writing user-mode pages without CLAC/STAC. */
void cpu_enable_smap(void);

/* Enable all supported CPU security features for the current core.
 * Call on BSP in Phase 0 and on each AP during SMP bringup. */
void cpu_harden(void);
