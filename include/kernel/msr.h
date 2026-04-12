/* ============================================================================
 * msr.h -- Centralised MSR read/write infrastructure
 *
 * Replaces scattered inline rdmsr/wrmsr across smp.c, acpi.c, lapic.c.
 * Provides #GP-safe msr_try_read() for probing unsupported MSRs.
 *
 * XREF: 02-kernel-core/TODO-19-x86-64-architecture.md §3
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

/* #GP-safe MSR read -- returns 0 on success, -1 if MSR is unsupported.
 * Implemented in msr.c using a temporary #GP handler. */
int msr_try_read(uint32_t index, uint64_t *out);

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

/* Speculation control */
#define MSR_IA32_SPEC_CTRL          0x00000048
#define MSR_IA32_PRED_CMD           0x00000049
#define MSR_IA32_ARCH_CAPS          0x0000010A

/* Performance monitoring */
#define MSR_IA32_MPERF              0x000000E7
#define MSR_IA32_APERF              0x000000E8
#define MSR_IA32_PERF_GLOBAL_CTRL   0x0000038F
#define MSR_IA32_FIXED_CTR0         0x00000309
#define MSR_IA32_FIXED_CTR_CTRL     0x0000038D

/* AMD-specific */
#define MSR_AMD_OSVW_ID_LEN         0xC0010140
#define MSR_AMD_OSVW_STATUS         0xC0010141
#define MSR_AMD_PERF_CTL0           0xC0010200
#define MSR_AMD_PERF_CTR0           0xC0010201

/* Hyper-V synthetic (TLFS Table 2-2) */
#define MSR_HV_TSC_FREQUENCY        0x40000022
#define MSR_HV_APIC_FREQUENCY       0x40000023
