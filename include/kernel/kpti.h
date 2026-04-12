/* ============================================================================
 * kpti.h -- Kernel Page Table Isolation (Meltdown mitigation)
 *
 * KPTI maintains two page tables per process:
 *   kernel_cr3: full kernel + user mappings (used in ring 0)
 *   user_cr3:   sparse user + trampoline mappings (used in ring 3)
 *
 * On ring transitions (SYSCALL, SYSRET, IDT entry, IRETQ), assembly
 * stubs in the trampoline page swap CR3 between the two tables.
 *
 * XREF: 02-kernel-core/TODO-17-kernel-security-hardening.md S3-S6
 * ============================================================================ */

#ifndef KERNEL_KPTI_H
#define KERNEL_KPTI_H

#include "kernel/types.h"

/* ---- Trampoline page VA ---- */

/* Fixed virtual address for the KPTI trampoline page. This page is mapped
 * in BOTH kernel_cr3 and user_cr3 so ring transitions can execute the
 * CR3 swap code. Must be in the kernel half of VA space (bit 63 set)
 * and must not collide with any other fixed mapping. */
#define KPTI_TRAMPOLINE_VA  0xFFFFFFFFFFFFF000ULL

/* ---- Trampoline stub entry points (offsets within the trampoline page) ----
 * These are populated by kpti_init() when it copies assembly stubs into
 * the trampoline page. S4 will redirect LSTAR to KPTI_TRAMPOLINE_VA +
 * KPTI_OFF_SYSCALL_ENTRY. S5 will redirect IDT entries to
 * KPTI_TRAMPOLINE_VA + KPTI_OFF_ISR_ENTRY. */
#define KPTI_OFF_SYSCALL_ENTRY  0x000
#define KPTI_OFF_SYSCALL_RETURN 0x080
#define KPTI_OFF_ISR_ENTRY      0x100
#define KPTI_OFF_ISR_RETURN     0x180

/* ---- Per-CPU struct offsets for assembly (must match smp.h) ---- */
#define PCPU_KERNEL_CR3  104   /* gs:104 = kernel_cr3 */
#define PCPU_USER_CR3    112   /* gs:112 = user_cr3 */

/* ---- Pages required in user_cr3 (documented per S3 checklist) ----
 *
 * The following pages MUST be present in every user_cr3 with
 * supervisor-only permissions (Present + Writable + NX, NO User bit):
 *
 * 1. Trampoline page (KPTI_TRAMPOLINE_VA) -- executable, no NX
 * 2. Per-CPU RSP0 stack page(s) -- TSS RSP0 for ring 3->0 transitions
 * 3. IST stack pages (DF/NMI/MCE) -- IST stacks for critical exceptions
 * 4. GDT/TSS page -- CPU reads GDT on interrupt delivery
 * 5. Per-CPU data page -- trampoline reads gs:kernel_cr3 before swap
 *
 * Without these, IDT delivery or SYSCALL would fault before the
 * trampoline code can swap to kernel_cr3. */

/* ---- API ---- */

/* Initialize the KPTI trampoline page. Called once from boot_phase0()
 * after VMM is ready. Allocates a physical frame, maps it at
 * KPTI_TRAMPOLINE_VA with executable permissions, and copies the
 * trampoline assembly stubs into it.
 * Does NOT activate CR3 swapping -- that's S4 (SYSCALL) and S5 (IDT). */
void kpti_init(void);

/* Returns 1 if KPTI is active (trampoline mapped, CR3 swapping enabled) */
int kpti_active(void);

#endif /* KERNEL_KPTI_H */
