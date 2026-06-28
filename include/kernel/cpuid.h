/* ============================================================================
 * cpuid.h -- Centralized CPUID Feature Detection
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

    /* CPUID Leaf 0x0D ECX=1 EAX (XSAVE extensions) */
    CPU_FEATURE_XSAVEOPT  = 49,   /* XSAVEOPT instruction (skip unchanged components) */
    CPU_FEATURE_XSAVEC    = 50,   /* XSAVEC instruction (compacted format) */
    CPU_FEATURE_XSAVES    = 51,   /* XSAVES/XRSTORS (supervisor state) */

    /* CPUID Leaf 0x80000001 EDX (AMD extended) */
    CPU_FEATURE_PAGE1GB   = 52,   /* 1-Gigabyte huge pages */
    CPU_FEATURE_RDTSCP    = 53,   /* RDTSCP instruction */

    /* CPUID Leaf 0x01 EDX */
    CPU_FEATURE_MTRR      = 54,   /* Memory Type Range Registers (EDX bit 12) */

    /* CPUID Leaf 0x01 ECX */
    CPU_FEATURE_CX16      = 55,   /* CMPXCHG16B (ECX bit 13) -- 16-byte DCAS */

    /* Future-silicon detection stubs (CPUID leaf 0x07): detect + log only */
    CPU_FEATURE_LA57      = 56,   /* 57-bit linear addressing (7.0:ECX[16]) */
    CPU_FEATURE_LAM       = 57,   /* Linear Address Masking (7.1:EAX[26]) */
    CPU_FEATURE_LASS      = 58,   /* Linear Address Space Separation (7.1:EAX[6]) */

    CPU_FEATURE_COUNT     = 59    /* total features tracked */
};

/* Confidential-compute guest kind (detected via CPUID; full attestation is
 * deferred). Stored kernel-internally in struct cpu_features (NOT boot_info --
 * the bootloader-side boot_info.cc_kind mirror is owned by the cpu-boot
 * confidential-compute detection TODO). */
typedef enum {
    CC_NONE        = 0,   /* not a confidential-VM guest                 */
    CC_INTEL_TDX   = 1,   /* Intel Trust Domain Extensions               */
    CC_AMD_SEV     = 2,   /* AMD Secure Encrypted Virtualization         */
    CC_AMD_SEV_ES  = 3,   /* SEV Encrypted State                         */
    CC_AMD_SEV_SNP = 4    /* SEV Secure Nested Paging                    */
} cc_kind_t;

/* --- AP feature consistency masks (TODO-09-boot S6) --------------------- */

/* Architectural baseline every CPU (BSP + every AP) MUST share. A CPU missing
 * any of these cannot run the kernel safely -> BUGCHECK_MULTIPROCESSOR_
 * CONFIGURATION_NOT_SUPPORTED. PAE/PGE/LAHF are NOT separately tracked here:
 * they are prerequisites of x86-64 long mode itself, so a CPU lacking them
 * never reaches kernel C code (it could not have entered long mode). LM is
 * included as the explicit Linux verify_cpu.S-style guard. CMPXCHG16B (CX16)
 * IS tracked and required: it is NOT a long-mode prerequisite (the earliest
 * AMD K8 x86-64 steppings lacked it), and the Executive interlocked SLIST
 * issues cmpxchg16b on a hot path, so a CX16-less CPU must halt early with a
 * clear minimum-requirements message rather than #UD at runtime. */
/* X-macro list: the SINGLE canonical source for the required baseline.
 * Generates CPU_FEATURES_REQUIRED_MASK (AP validation, cpu_security.c)
 * AND the BSP minimum-requirements gate table (boot_hw.c). Adding a
 * required feature here updates both consumers; they cannot drift. */
#define CPU_FEATURES_REQUIRED_LIST(X) \
    X(CPU_FEATURE_NX,      "NX (No-Execute) not available -- cannot boot safely") \
    X(CPU_FEATURE_SSE2,    "SSE2 not available -- required for kernel math") \
    X(CPU_FEATURE_LM,      "Long Mode not reported -- CPUID inconsistent with 64-bit execution") \
    X(CPU_FEATURE_SYSCALL, "SYSCALL/SYSRET not available -- required for the syscall fast path") \
    X(CPU_FEATURE_CX16,    "CMPXCHG16B not available -- required for Executive SLIST/DCAS")

#define CPU_FEATURE_REQ_BIT_(feat, diag) | (1ULL << (feat))
#define CPU_FEATURES_REQUIRED_MASK (0ULL CPU_FEATURES_REQUIRED_LIST(CPU_FEATURE_REQ_BIT_))

/* The security-critical feature subset cpuid_probe_ap_features() probes on each
 * AP. Per this section's scope ("validates security-critical feature
 * mismatches"), it is NOT the full feature set: it covers the required baseline
 * plus the optional features that drive CR4/XCR0 enables and hybrid divergence
 * (SMEP/SMAP/UMIP/PKU/AVX/AVX512F/PCID/XSAVE). The global intersection and the
 * optional-mismatch check operate only within this mask. */
#define CPU_FEATURES_AP_PROBE_MASK \
    (CPU_FEATURES_REQUIRED_MASK | \
     (1ULL << CPU_FEATURE_SMEP) | (1ULL << CPU_FEATURE_SMAP) | \
     (1ULL << CPU_FEATURE_UMIP) | (1ULL << CPU_FEATURE_PKU)  | \
     (1ULL << CPU_FEATURE_AVX)  | (1ULL << CPU_FEATURE_AVX512F) | \
     (1ULL << CPU_FEATURE_PCID) | (1ULL << CPU_FEATURE_XSAVE) | \
     (1ULL << CPU_FEATURE_RDTSCP) | (1ULL << CPU_FEATURE_SSE4_2))

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
    uint64_t xcr0_supported;      /* supported XCR0 bits (from CPUID leaf 0x0D) */
    uint64_t xcr0_active;         /* actually enabled XCR0 bits (after cpu_configure_xcr0) */

    /* Feature flags -- one bit per CPU_FEATURE_* */
    uint64_t flags;

    /* Confidential-compute guest kind (cc_kind_t); CC_NONE on bare/normal VM */
    uint8_t  cc_kind;

    /* Address sizes (from leaf 0x80000008) */
    uint8_t  phys_addr_bits;      /* physical address width (typically 48) */
    uint8_t  linear_addr_bits;    /* linear address width (48, or 57 if LA57) */
    uint16_t num_cores;           /* number of physical cores (uint8 wraps at 256) */

    /* Zen topology (from leaf 0x8000001E, AMD only) */
    uint32_t ext_apic_id;         /* extended APIC ID */
    uint8_t  compute_unit_id;     /* physical core within CCD */
    uint8_t  node_id;             /* NUMA domain identifier */
    uint8_t  threads_per_core;    /* SMT siblings per core (1 = no SMT); 0 = unknown */
    uint8_t  _pad1;               /* alignment padding */

    /* PKRU XSAVE component (from leaf 0x0D subleaf 9) */
    uint32_t pkru_xsave_offset;   /* byte offset of PKRU in XSAVE area (0 = not available) */
    uint32_t pkru_xsave_size;     /* size of PKRU component (4 bytes on Intel) */

    /* AMD OSVW (OS Visible Workarounds) */
    uint32_t osvw_length;         /* number of errata tracked (0 = OSVW not available) */
    uint64_t osvw_status;         /* bitmask of active errata */
};

/* --- API --- */

/* Initialize CPUID detection. Call once at boot, after heap_init(). */
void cpuid_init(void);

/* Probe the security-critical CPU feature subset (CPU_FEATURES_AP_PROBE_MASK)
 * on the CALLING CPU and return it in the g_cpu.flags bit layout. AP-safe: does
 * NOT touch g_cpu, so an AP can publish its own mask without racing the BSP
 * global. Used by cpu_validate_ap_features() (TODO-09-boot S6). */
uint64_t cpuid_probe_ap_features(void);

/* Configure XCR0 based on detected CPU features.
 * Enables x87+SSE+AVX always, AVX-512 if supported, PKRU if supported.
 * Sets CR4.OSXSAVE first. Stores result in g_cpu.xcr0_active.
 * Safe to call on BSP and each AP. */
void cpu_configure_xcr0(void);

/* Check if a CPU feature is supported. Returns 1 if available, 0 if not. */
static inline int cpu_has(enum cpu_feature feat)
{
    extern struct cpu_features g_cpu;
    return (g_cpu.flags >> feat) & 1;
}

/* Get pointer to global CPU features struct (for reading vendor/brand/etc). */
const struct cpu_features *cpuid_get(void);

/* Check if AMD erratum N is active (via OSVW MSR). Returns 1 if active.
 * Safe for n >= 64 (osvw_status is uint64_t; shift UB prevented by clamp). */
static inline int cpu_has_erratum(uint32_t n)
{
    extern struct cpu_features g_cpu;
    if (n >= g_cpu.osvw_length || n >= 64) return 0;
    return (int)((g_cpu.osvw_status >> n) & 1);
}

/* RDTSCP: read TSC and CPU ID atomically.
 * Returns TSC value; *cpu_id receives IA32_TSC_AUX (logical CPU index).
 * Requires CPU_FEATURE_RDTSCP. */
static inline uint64_t rdtscp_read(uint32_t *cpu_id)
{
    uint32_t lo, hi, aux;
    __asm__ volatile ("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    if (cpu_id) *cpu_id = aux;
    return ((uint64_t)hi << 32) | lo;
}

/* Low-level CPUID wrapper (for use by other subsystems). */
void cpuid_raw(uint32_t leaf, uint32_t subleaf,
               uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
