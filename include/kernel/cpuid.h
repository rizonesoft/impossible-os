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

    /* CPUID Leaf 0x07 ECX=0 (CET_SS is ECX[7]; CET_IBT/IBRS/... are EDX) */
    CPU_FEATURE_CET_SS    = 29,   /* CET Shadow Stacks -- probed from ECX[7] */
    CPU_FEATURE_CET_IBT   = 30,   /* CET Indirect Branch Tracking -- EDX[20] */
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

    /* Additional CPU feature adoption (CPUID leaf 0x07) */
    CPU_FEATURE_WAITPKG   = 59,   /* UMONITOR/UMWAIT/TPAUSE (7.0:ECX[5]) */
    CPU_FEATURE_SERIALIZE = 60,   /* SERIALIZE instruction (7.0:EDX[14]) */
    CPU_FEATURE_RDPID     = 61,   /* RDPID instruction (7.0:ECX[22]) */

    /* Spectre v2 mitigation. Bits 62/63 fill the first 64-bit word; the feature
     * surface is a 128-bit cpu_feature_mask_t (below), so predictor-policy
     * features live in the second word. */
    CPU_FEATURE_ENHANCED_IBRS = 62, /* IA32_ARCH_CAPABILITIES[1] IBRS_ALL (set-once IBRS) */
    CPU_FEATURE_IBPB      = 63,   /* IBPB: Intel 7.0:EDX[26] / AMD 0x80000008:EBX[12] */

    /* Word 1 (bit >= 64) -- the 128-bit cpu_feature_mask_t surface. */
    CPU_FEATURE_MD_CLEAR  = 64,   /* MD_CLEAR: VERW clears CPU buffers (7.0:EDX[10]) */

    /* Ring-0 MONITOR/MWAIT. This is a DIFFERENT instruction family from
     * CPU_FEATURE_WAITPKG above (UMONITOR/UMWAIT/TPAUSE, ring 3, 7.0:ECX[5]);
     * neither implies the other and MWAIT must be gated on THIS bit. */
    CPU_FEATURE_MONITOR   = 65,   /* MONITOR/MWAIT (1:ECX[3]) */

    CPU_FEATURE_COUNT     = 66    /* total features tracked */
};

/* --- 128-bit feature bitset ------------------------------------------------
 * One bit per CPU_FEATURE_*. Two 64-bit words so the surface can grow past 64
 * without the silent (1ULL << feat) truncation that would let AP validation
 * publish an over-broad global mask or skip a mitigation on one CPU. ALWAYS
 * route bit access through these helpers; never `1ULL << CPU_FEATURE_*` on a
 * mask. word = feat >> 6, bit = feat & 63. */
typedef struct { uint64_t w[2]; } cpu_feature_mask_t;

_Static_assert(CPU_FEATURE_COUNT <= 128,
    "CPU feature surface exceeds the 128-bit cpu_feature_mask_t; widen w[].");

static inline void cpu_feature_set(cpu_feature_mask_t *m, enum cpu_feature f)
{
    m->w[(unsigned)f >> 6] |= 1ULL << ((unsigned)f & 63);
}
static inline void cpu_feature_clear(cpu_feature_mask_t *m, enum cpu_feature f)
{
    m->w[(unsigned)f >> 6] &= ~(1ULL << ((unsigned)f & 63));
}
static inline int cpu_feature_test(const cpu_feature_mask_t *m, enum cpu_feature f)
{
    return (int)((m->w[(unsigned)f >> 6] >> ((unsigned)f & 63)) & 1);
}
static inline cpu_feature_mask_t cpu_feature_and(cpu_feature_mask_t a, cpu_feature_mask_t b)
{
    a.w[0] &= b.w[0]; a.w[1] &= b.w[1]; return a;
}
static inline cpu_feature_mask_t cpu_feature_andnot(cpu_feature_mask_t a, cpu_feature_mask_t b)
{
    a.w[0] &= ~b.w[0]; a.w[1] &= ~b.w[1]; return a;
}
/* True when every bit set in `sub` is also set in `sup` (sub is a subset). */
static inline int cpu_feature_subset(cpu_feature_mask_t sub, cpu_feature_mask_t sup)
{
    return ((sub.w[0] & ~sup.w[0]) | (sub.w[1] & ~sup.w[1])) == 0;
}
static inline int cpu_feature_iszero(cpu_feature_mask_t m)
{
    return (m.w[0] | m.w[1]) == 0;
}
static inline int cpu_feature_eq(cpu_feature_mask_t a, cpu_feature_mask_t b)
{
    return a.w[0] == b.w[0] && a.w[1] == b.w[1];
}

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

/* Compile-time word/bit split: a feature contributes to word 0 when its bit is
 * < 64, else to word 1. The `& 63` keeps every shift in range (no UB) even on
 * the branch that evaluates to 0. Variadic so it absorbs the required-list diag
 * string AND the empty second arg the optional AP-probe rows pass. */
#define CPU_FEAT_W0_(feat, ...) | ((feat) <  64 ? (1ULL << ((feat) & 63)) : 0ULL)
#define CPU_FEAT_W1_(feat, ...) | ((feat) >= 64 ? (1ULL << ((feat) & 63)) : 0ULL)
#define CPU_FEATURES_REQUIRED_MASK \
    ((cpu_feature_mask_t){ { 0ULL CPU_FEATURES_REQUIRED_LIST(CPU_FEAT_W0_), \
                             0ULL CPU_FEATURES_REQUIRED_LIST(CPU_FEAT_W1_) } })

/* The security-critical feature subset cpuid_probe_ap_features() probes on each
 * AP. Per this section's scope ("validates security-critical feature
 * mismatches"), it is NOT the full feature set: it covers the required baseline
 * plus the optional features that drive CR4/XCR0 enables and hybrid divergence
 * (SMEP/SMAP/UMIP/PKU/AVX/AVX512F/PCID/XSAVE/RDTSCP/WAITPKG/MONITOR). WAITPKG drives
 * the UMWAIT_CONTROL MSR replay the way RDTSCP drives TSC_AUX; MONITOR is probed
 * for the same reason its ring-3 twin WAITPKG is -- a monitored wait issued on an
 * AP needs the ALL-CPU answer, and cpu_has() alone reports only the BSP's. The global
 * intersection and the optional-mismatch check operate only within this mask. */
/* AP-probe set = required baseline + the optional features that drive CR4/XCR0
 * enables and hybrid divergence. Reuses the required X-macro list (diag rows)
 * plus the optional rows (empty second arg); both feed only the word-split
 * macros, so the arity difference is absorbed by the variadic `...`. */
#define CPU_FEATURES_AP_PROBE_LIST(X) \
    CPU_FEATURES_REQUIRED_LIST(X) \
    X(CPU_FEATURE_SMEP,)   X(CPU_FEATURE_SMAP,)    X(CPU_FEATURE_UMIP,) \
    X(CPU_FEATURE_PKU,)    X(CPU_FEATURE_AVX,)     X(CPU_FEATURE_AVX512F,) \
    X(CPU_FEATURE_PCID,)   X(CPU_FEATURE_XSAVE,)   X(CPU_FEATURE_RDTSCP,) \
    X(CPU_FEATURE_SSE4_2,) X(CPU_FEATURE_WAITPKG,) X(CPU_FEATURE_SPEC_CTRL,) \
    X(CPU_FEATURE_MONITOR,)
#define CPU_FEATURES_AP_PROBE_MASK \
    ((cpu_feature_mask_t){ { 0ULL CPU_FEATURES_AP_PROBE_LIST(CPU_FEAT_W0_), \
                             0ULL CPU_FEATURES_AP_PROBE_LIST(CPU_FEAT_W1_) } })

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

    /* Feature flags -- one bit per CPU_FEATURE_* (128-bit, two words) */
    cpu_feature_mask_t flags;

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
cpu_feature_mask_t cpuid_probe_ap_features(void);

/* Configure XCR0 based on detected CPU features.
 * Enables x87+SSE+AVX always, AVX-512 if supported, PKRU if supported.
 * Sets CR4.OSXSAVE first. Stores result in g_cpu.xcr0_active.
 * Safe to call on BSP and each AP. */
void cpu_configure_xcr0(void);

/* Check if a CPU feature is supported. Returns 1 if available, 0 if not. */
static inline int cpu_has(enum cpu_feature feat)
{
    extern struct cpu_features g_cpu;
    return cpu_feature_test(&g_cpu.flags, feat);
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
