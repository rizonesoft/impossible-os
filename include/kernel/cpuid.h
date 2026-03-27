/* ============================================================================
 * cpuid.h — Centralized CPUID Feature Detection
 *
 * Call cpuid_init() once at boot (after heap_init) to probe all processor
 * capabilities. Use cpu_has(CPU_FEATURE_XXX) to check features anywhere
 * in the kernel.
 *
 * Replaces ad-hoc inline asm CPUID in hw_dump.c, gfx_simd.c, registry.c.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* --- Feature constants (bit positions in cpu_features.flags) --- */

enum cpu_feature {
    /* CPUID Leaf 0x01 EDX */
    CPU_FEATURE_FPU       =  0,   /* x87 FPU on-chip */
    CPU_FEATURE_TSC       =  1,   /* Time Stamp Counter */
    CPU_FEATURE_APIC      =  2,   /* On-chip APIC */
    CPU_FEATURE_MMX       =  3,   /* MMX instructions */
    CPU_FEATURE_SSE       =  4,   /* SSE instructions */
    CPU_FEATURE_SSE2      =  5,   /* SSE2 instructions */
    CPU_FEATURE_HTT       =  6,   /* Hyper-Threading Technology */

    /* CPUID Leaf 0x01 ECX */
    CPU_FEATURE_SSE3      =  7,   /* SSE3 instructions */
    CPU_FEATURE_SSSE3     =  8,   /* Supplemental SSE3 */
    CPU_FEATURE_SSE4_1    =  9,   /* SSE4.1 instructions */
    CPU_FEATURE_SSE4_2    = 10,   /* SSE4.2 instructions */
    CPU_FEATURE_AES       = 11,   /* AES-NI instructions */
    CPU_FEATURE_AVX       = 12,   /* AVX instructions */
    CPU_FEATURE_XSAVE     = 13,   /* XSAVE/XRSTOR hardware */
    CPU_FEATURE_OSXSAVE   = 14,   /* OS has enabled XSAVE (CR4.OSXSAVE) */
    CPU_FEATURE_RDRAND    = 15,   /* RDRAND instruction */
    CPU_FEATURE_PCID      = 16,   /* Process-Context Identifiers */
    CPU_FEATURE_TSC_DL    = 17,   /* TSC-Deadline APIC timer */

    /* CPUID Leaf 0x07 ECX=0 EBX */
    CPU_FEATURE_AVX2      = 18,   /* AVX2 instructions */
    CPU_FEATURE_BMI1      = 19,   /* Bit Manipulation Group 1 */
    CPU_FEATURE_BMI2      = 20,   /* Bit Manipulation Group 2 */
    CPU_FEATURE_AVX512F   = 21,   /* AVX-512 Foundation */
    CPU_FEATURE_SMEP      = 22,   /* Supervisor Mode Execution Prevention */
    CPU_FEATURE_SMAP      = 23,   /* Supervisor Mode Access Prevention */
    CPU_FEATURE_RDSEED    = 24,   /* RDSEED instruction */
    CPU_FEATURE_ADX       = 25,   /* Multi-Precision Add-Carry */
    CPU_FEATURE_INVPCID   = 26,   /* INVPCID instruction */

    /* CPUID Leaf 0x07 ECX=0 ECX */
    CPU_FEATURE_UMIP      = 27,   /* User-Mode Instruction Prevention */
    CPU_FEATURE_PKU       = 28,   /* Protection Keys for User-mode */

    /* CPUID Leaf 0x07 ECX=0 EDX */
    CPU_FEATURE_CET_SS    = 29,   /* CET Shadow Stacks */
    CPU_FEATURE_CET_IBT   = 30,   /* CET Indirect Branch Tracking */
    CPU_FEATURE_IBRS      = 31,   /* Indirect Branch Restricted Speculation */
    CPU_FEATURE_STIBP     = 32,   /* Single Thread Indirect Branch Predictors */
    CPU_FEATURE_UINTR     = 33,   /* User-Level Interrupts */
    CPU_FEATURE_SPEC_CTRL = 34,   /* IA32_SPEC_CTRL MSR present */
    CPU_FEATURE_ARCH_CAP  = 35,   /* IA32_ARCH_CAPABILITIES MSR present */

    /* CPUID Leaf 0x07 ECX=1 EAX */
    CPU_FEATURE_FRED      = 36,   /* Flexible Return and Event Delivery */
    CPU_FEATURE_LKGS      = 37,   /* Load Kernel GS Base instruction */

    /* CPUID Leaf 0x07 ECX=1 EDX */
    CPU_FEATURE_AVX10     = 38,   /* AVX10 Converged Vector ISA */
    CPU_FEATURE_APX       = 39,   /* Advanced Performance Extensions (R16-R31) */

    /* CPUID Leaf 0x80000001 EDX */
    CPU_FEATURE_NX        = 40,   /* No-Execute (NX) bit */
    CPU_FEATURE_LM        = 41,   /* Long Mode (64-bit) */
    CPU_FEATURE_SYSCALL   = 42,   /* SYSCALL/SYSRET instructions */

    /* CPUID Leaf 0x80000007 EDX */
    CPU_FEATURE_TSC_INV   = 43,   /* Invariant TSC */

    /* CPUID Leaf 0x80000001 ECX (AMD extended) */
    CPU_FEATURE_SVM       = 44,   /* Secure Virtual Machine (AMD-V) */
    CPU_FEATURE_OSVW      = 45,   /* OS Visible Workarounds */
    CPU_FEATURE_IBS       = 46,   /* Instruction Based Sampling */
    CPU_FEATURE_TOPO_EXT  = 47,   /* TopologyExtensions (enables leaf 0x8000001E) */

    /* CPUID Leaf 0x01 ECX (Intel VT-x) */
    CPU_FEATURE_VMX       = 48,   /* Intel Virtual Machine Extensions */

    /* CPUID Leaf 0x80000001 EDX (AMD extended) */
    CPU_FEATURE_PAGE1GB   = 49,   /* 1-Gigabyte huge pages */
    CPU_FEATURE_RDTSCP    = 50,   /* RDTSCP instruction */

    CPU_FEATURE_COUNT     = 51    /* total features tracked */
};

/* --- Global CPU feature structure --- */

struct cpu_features {
    /* Identity */
    char     vendor[13];          /* "GenuineIntel" or "AuthenticAMD" */
    char     brand[49];           /* full brand string (e.g., "Intel Core i7-12700K") */
    uint32_t family;              /* display family (base + extended) */
    uint32_t model;               /* display model (base + extended) */
    uint32_t stepping;            /* stepping number */
    uint32_t max_leaf;            /* max basic CPUID leaf */
    uint32_t max_ext_leaf;        /* max extended CPUID leaf */

    /* XSAVE sizes */
    uint32_t xsave_size;          /* current XSAVE area size (bytes) */
    uint32_t xsave_size_max;      /* max XSAVE area size (bytes) */
    uint64_t xcr0_supported;      /* supported XCR0 bits */

    /* Feature flags — one bit per CPU_FEATURE_* */
    uint64_t flags;

    /* Address sizes (from leaf 0x80000008) */
    uint8_t  phys_addr_bits;      /* physical address width (typically 48) */
    uint8_t  linear_addr_bits;    /* linear address width (48, or 57 if LA57) */
    uint8_t  num_cores;           /* number of physical cores */
    uint8_t  _pad0;               /* alignment padding */

    /* Zen topology (from leaf 0x8000001E, AMD only) */
    uint32_t ext_apic_id;         /* extended APIC ID */
    uint8_t  compute_unit_id;     /* physical core within CCD */
    uint8_t  node_id;             /* NUMA domain identifier */
    uint8_t  _pad1[2];            /* alignment padding */
};

/* --- API --- */

/* Initialize CPUID detection. Call once at boot, after heap_init(). */
void cpuid_init(void);

/* Check if a CPU feature is supported. Returns 1 if available, 0 if not. */
static inline int cpu_has(enum cpu_feature feat)
{
    extern struct cpu_features g_cpu;
    return (g_cpu.flags >> feat) & 1;
}

/* Get pointer to global CPU features struct (for reading vendor/brand/etc). */
const struct cpu_features *cpuid_get(void);

/* Low-level CPUID wrapper (for use by other subsystems). */
void cpuid_raw(uint32_t leaf, uint32_t subleaf,
               uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
