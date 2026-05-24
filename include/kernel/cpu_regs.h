/* ============================================================================
 * cpu_regs.h -- x86-64 control-register bit definitions (single source of truth)
 *
 * CR0 / CR4 bit positions per Intel SDM Vol. 3A Table 2-1 / 2-2. Shared so that
 * cpuid.c, cpu_security.c, and the CR0/CR4 safety-bit pinning (TODO-09-boot S7)
 * all reference one definition instead of scattering magic shifts. No
 * dependencies: just numeric #defines. ARCH: x86-64.
 * ============================================================================ */

#pragma once

/* ---- CR0 (Intel SDM Vol. 3A Table 2-1) ---- */
#define CR0_PE       (1UL << 0)   /* Protection Enable */
#define CR0_MP       (1UL << 1)   /* Monitor Coprocessor */
#define CR0_EM       (1UL << 2)   /* Emulation */
#define CR0_TS       (1UL << 3)   /* Task Switched */
#define CR0_NE       (1UL << 5)   /* Numeric Error */
#define CR0_WP       (1UL << 16)  /* Write Protect -- supervisor honors PTE.W=0 */
#define CR0_AM       (1UL << 18)  /* Alignment Mask */
#define CR0_NW       (1UL << 29)  /* Not Write-through */
#define CR0_CD       (1UL << 30)  /* Cache Disable */
#define CR0_PG       (1UL << 31)  /* Paging */

/* ---- CR4 (Intel SDM Vol. 3A Table 2-2) ---- */
#define CR4_UMIP     (1UL << 11)  /* User-Mode Instruction Prevention */
#define CR4_LA57     (1UL << 12)  /* 57-bit linear addresses */
#define CR4_VMXE     (1UL << 13)  /* VMX Enable */
#define CR4_FSGSBASE (1UL << 16)  /* RD/WR FS/GS BASE instructions */
#define CR4_PCIDE    (1UL << 17)  /* Process-Context Identifiers */
#define CR4_OSXSAVE  (1UL << 18)  /* XSAVE + processor extended states */
#define CR4_SMEP     (1UL << 20)  /* Supervisor Mode Execution Prevention */
#define CR4_SMAP     (1UL << 21)  /* Supervisor Mode Access Prevention */
#define CR4_PKE      (1UL << 22)  /* Protection Keys for User pages */
#define CR4_CET      (1UL << 23)  /* Control-flow Enforcement Technology */
#define CR4_PKS      (1UL << 24)  /* Protection Keys for Supervisor pages */
