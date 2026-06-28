/* ============================================================================
 * msr.h -- Centralised MSR read/write infrastructure
 *
 * Replaces scattered inline rdmsr/wrmsr across smp.c, acpi.c, lapic.c.
 * Provides #GP-safe msr_try_read() for probing unsupported MSRs.
 *
 * XREF: 02-kernel-core/TODO-09-x86-64-architecture.md
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- MSR read/write ---------------------------------------------------- */

static inline uint64_t msr_read(uint32_t index)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(index));
    return ((uint64_t)hi << 32) | lo;
}

static inline void msr_write(uint32_t index, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" :: "c"(index), "a"(lo), "d"(hi) : "memory");
}

/* #GP-safe MSR read -- returns 0 on success, -1 if #GP was caught.
 *
 * WARNING: This is a NO-CRASH guarantee, NOT an existence check.
 * WHPX silently absorbs reads of unknown MSRs (returns 0, no #GP),
 * so ret==0 does NOT prove the MSR exists on every platform.
 *
 * Rule: ALWAYS gate on cpu_has(CPU_FEATURE_*) or CPUID before calling.
 * Use msr_try_read() only as a secondary safety net for edge cases
 * where CPUID does not directly indicate MSR availability.
 *
 * Implemented in msr.c using a temporary #GP handler. */
int msr_try_read(uint32_t index, uint64_t *out);

/* #GP-safe MSR write companion. Returns 0 on success, -1 if the wrmsr faulted
 * (MSR not writable) or the kernel IDT is not yet loaded. For probe-style
 * writes to architectural-but-maybe-unwritable MSRs; gate on CPUID/vendor
 * first, same as msr_try_read(). Implemented in msr.c. */
int msr_try_write(uint32_t index, uint64_t value);

/* ---- MSR index constants ----------------------------------------------- */

/* EFER / SYSCALL */
#define MSR_IA32_EFER               0xC0000080
#define EFER_SCE                    (1ULL << 0)   /* SYSCALL/SYSRET enable */
#define EFER_LME                    (1ULL << 8)   /* Long Mode Enable */
#define EFER_LMA                    (1ULL << 10)  /* Long Mode Active (read-only) */
#define EFER_NXE                    (1ULL << 11)  /* No-Execute Enable */
#define MSR_IA32_STAR               0xC0000081
#define MSR_IA32_LSTAR              0xC0000082
#define MSR_IA32_CSTAR              0xC0000083
#define MSR_IA32_FMASK              0xC0000084

/* GS/FS base (swapgs) */
#define MSR_IA32_FS_BASE            0xC0000100
#define MSR_IA32_GS_BASE            0xC0000101
#define MSR_IA32_KERNEL_GS_BASE     0xC0000102

/* TSC */
#define MSR_IA32_TSC                0x00000010
#define MSR_IA32_TSC_AUX            0xC0000103
#define MSR_IA32_TSC_DEADLINE       0x000006E0

/* APIC */
#define MSR_IA32_APIC_BASE          0x0000001B

/* PAT (Page Attribute Table) */
#define MSR_IA32_PAT                0x00000277

/* MTRR (Memory Type Range Registers) -- Intel SDM Vol 3 11.11.
 * Used for AP parity auditing (TODO-09-boot S8): the BSP snapshots its MTRR
 * state and each AP is compared against it; divergence is logged. MMIO cache
 * correctness itself rides on PAT (UC PAT type always wins over MTRR per SDM
 * 11.5.2), so this audit is defense-in-depth, not the correctness gate. */
#define MSR_IA32_MTRRCAP            0x000000FE  /* read-only capability */
#define MSR_IA32_MTRR_DEF_TYPE      0x000002FF  /* default type + enable bits */
#define MSR_IA32_MTRR_PHYSBASE0     0x00000200  /* variable pair base; MASK = +1 */
/* Fixed-range MTRRs (present only when MTRRCAP.FIX) */
#define MSR_IA32_MTRR_FIX64K_00000  0x00000250
#define MSR_IA32_MTRR_FIX16K_80000  0x00000258
#define MSR_IA32_MTRR_FIX16K_A0000  0x00000259
#define MSR_IA32_MTRR_FIX4K_C0000   0x00000268  /* through 0x26F (8 registers) */

/* MTRRCAP fields */
#define MTRRCAP_VCNT_MASK           0xFFu        /* bits [7:0]: variable range count */
#define MTRRCAP_FIX                 (1ULL << 8)  /* fixed-range MTRRs supported */
#define MTRRCAP_WC                  (1ULL << 10) /* write-combining type supported */
/* MTRR_DEF_TYPE fields */
#define MTRR_DEF_TYPE_TYPE_MASK     0xFFu        /* bits [7:0]: default memory type */
#define MTRR_DEF_TYPE_FE            (1ULL << 10) /* fixed-range MTRRs enabled */
#define MTRR_DEF_TYPE_E             (1ULL << 11) /* MTRRs enabled */
#define MTRR_VARIABLE_MAX           255u         /* MTRRCAP.VCNT is an 8-bit field */

/* Speculation control (TODO-10 S8 Spectre v2) */
#define MSR_IA32_SPEC_CTRL          0x00000048
#define SPEC_CTRL_IBRS              (1ULL << 0)   /* Indirect Branch Restricted Speculation */
#define SPEC_CTRL_STIBP             (1ULL << 1)   /* Single Thread IBP (S18) */
#define SPEC_CTRL_SSBD              (1ULL << 2)   /* Speculative Store Bypass Disable (S18) */
#define MSR_IA32_PRED_CMD           0x00000049
#define PRED_CMD_IBPB               (1ULL << 0)   /* Indirect Branch Prediction Barrier */
#define MSR_IA32_ARCH_CAPS          0x0000010A
#define ARCH_CAP_RDCL_NO           (1ULL << 0)   /* not susceptible to Meltdown (RDCL) */
#define ARCH_CAP_IBRS_ALL          (1ULL << 1)   /* eIBRS: IBRS provides always-on protection */
#define ARCH_CAP_MDS_NO            (1ULL << 5)   /* not susceptible to MDS (no VERW clear needed) */
#define ARCH_CAP_TSX_CTRL          (1ULL << 7)   /* IA32_TSX_CTRL MSR is present */
#define ARCH_CAP_TAA_NO            (1ULL << 8)   /* not susceptible to TSX Async Abort */
#define ARCH_CAP_RFDS_NO           (1ULL << 27)  /* not susceptible to Register File Data Sampling */
#define ARCH_CAP_RFDS_CLEAR        (1ULL << 28)  /* VERW also clears the register file (RFDS) */
/* TSX control (TAA mitigation): disable RTM + force CPUID HLE/RTM clear. */
#define MSR_IA32_TSX_CTRL           0x00000122
#define TSX_CTRL_RTM_DISABLE       (1ULL << 0)   /* XBEGIN always aborts */
#define TSX_CTRL_CPUID_CLEAR       (1ULL << 1)   /* CPUID stops enumerating HLE/RTM */

/* CPU register audit (TODO-09-boot S9) */
#define MSR_IA32_MISC_ENABLE        0x000001A0  /* feature-enable bits */
#define MSR_IA32_BIOS_SIGN_ID       0x0000008B  /* microcode signature/revision */

/* WAITPKG: bound user UMWAIT/TPAUSE dwell (bits[31:2]=max TSC time, bit0=C0.2-disable) */
#define MSR_IA32_UMWAIT_CONTROL     0x000000E1

/* Performance monitoring */
#define MSR_IA32_MPERF              0x000000E7
#define MSR_IA32_APERF              0x000000E8
#define MSR_IA32_PERF_GLOBAL_CTRL   0x0000038F
#define MSR_IA32_FIXED_CTR0         0x00000309
#define MSR_IA32_FIXED_CTR_CTRL     0x0000038D

/* AMD-specific */
#define MSR_AMD_OSVW_ID_LEN         0xC0010140
#define MSR_AMD_OSVW_STATUS         0xC0010141
#define MSR_AMD64_SEV              0xC0010131  /* SEV_STATUS: bit0=SEV bit1=ES bit2=SNP active */
#define MSR_AMD_PERF_CTL0           0xC0010200
#define MSR_AMD_PERF_CTR0           0xC0010201

/* Hyper-V synthetic (TLFS Table 2-2) */
#define MSR_HV_TSC_FREQUENCY        0x40000022
#define MSR_HV_APIC_FREQUENCY       0x40000023
