/* ============================================================================
 * cpuid.c — Centralized CPUID Feature Detection
 *
 * Probes all relevant CPUID leaves at boot and stores results in a global
 * struct cpu_features. Replaces ad-hoc inline asm CPUID scattered across
 * hw_dump.c, gfx_simd.c, and registry.c.
 *
 * Call cpuid_init() once from kernel_main() after heap_init().
 * ============================================================================ */

#include "kernel/cpuid.h"
#include "kernel/klog.h"

/* Global CPU features — zero-initialized at startup */
struct cpu_features g_cpu;

/* --- Low-level CPUID wrapper --- */

void cpuid_raw(uint32_t leaf, uint32_t subleaf,
               uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile ("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf));
}

/* --- Helpers --- */

static void set_flag_if(uint64_t *flags, enum cpu_feature feat,
                        uint32_t reg, uint32_t bit)
{
    if (reg & (1U << bit))
        *flags |= (1ULL << feat);
}

/* Copy n bytes (no memcpy in freestanding) */
static void cpuid_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
}

/* Zero n bytes */
static void cpuid_memzero(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    for (i = 0; i < n; i++)
        d[i] = 0;
}

/* --- Public API --- */

const struct cpu_features *cpuid_get(void)
{
    return &g_cpu;
}

void cpuid_init(void)
{
    uint32_t eax, ebx, ecx, edx;

    cpuid_memzero(&g_cpu, sizeof(g_cpu));

    /* ---- Leaf 0x00: Vendor string & max basic leaf ---- */
    cpuid_raw(0x00, 0, &eax, &ebx, &ecx, &edx);
    g_cpu.max_leaf = eax;

    /* Vendor is EBX-EDX-ECX (note: EDX before ECX) */
    *(uint32_t *)(g_cpu.vendor + 0) = ebx;
    *(uint32_t *)(g_cpu.vendor + 4) = edx;
    *(uint32_t *)(g_cpu.vendor + 8) = ecx;
    g_cpu.vendor[12] = '\0';

    /* ---- Leaf 0x01: Family/Model/Stepping + feature flags ---- */
    if (g_cpu.max_leaf >= 0x01) {
        cpuid_raw(0x01, 0, &eax, &ebx, &ecx, &edx);

        /* Family/Model/Stepping — Intel display model algorithm */
        {
            uint32_t base_family  = (eax >> 8) & 0xF;
            uint32_t ext_family   = (eax >> 20) & 0xFF;
            uint32_t base_model   = (eax >> 4) & 0xF;
            uint32_t ext_model    = (eax >> 16) & 0xF;

            g_cpu.stepping = eax & 0xF;

            if (base_family == 0xF)
                g_cpu.family = base_family + ext_family;
            else
                g_cpu.family = base_family;

            if (base_family == 0x6 || base_family == 0xF)
                g_cpu.model = (ext_model << 4) | base_model;
            else
                g_cpu.model = base_model;
        }

        /* EDX feature flags */
        set_flag_if(&g_cpu.flags, CPU_FEATURE_FPU,   edx,  0);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_TSC,   edx,  4);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_APIC,  edx,  9);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_MMX,   edx, 23);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SSE,   edx, 25);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SSE2,  edx, 26);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_HTT,   edx, 28);

        /* ECX feature flags */
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SSE3,    ecx,  0);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SSSE3,   ecx,  9);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SSE4_1,  ecx, 19);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SSE4_2,  ecx, 20);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_AES,     ecx, 25);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_XSAVE,   ecx, 26);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_OSXSAVE, ecx, 27);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_AVX,     ecx, 28);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_RDRAND,  ecx, 30);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_PCID,    ecx, 17);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_TSC_DL,  ecx, 24);
    }

    /* ---- Leaf 0x07 ECX=0: Extended features ---- */
    if (g_cpu.max_leaf >= 0x07) {
        cpuid_raw(0x07, 0, &eax, &ebx, &ecx, &edx);

        /* EBX */
        set_flag_if(&g_cpu.flags, CPU_FEATURE_BMI1,    ebx,  3);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_AVX2,    ebx,  5);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_BMI2,    ebx,  8);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SMEP,    ebx,  7);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_INVPCID,  ebx, 10);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_AVX512F,  ebx, 16);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_RDSEED,   ebx, 18);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_ADX,      ebx, 19);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SMAP,     ebx, 20);

        /* ECX */
        set_flag_if(&g_cpu.flags, CPU_FEATURE_UMIP,     ecx,  2);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_PKU,      ecx,  3);

        /* EDX */
        set_flag_if(&g_cpu.flags, CPU_FEATURE_UINTR,     edx,  5);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_CET_SS,    edx,  7);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_CET_IBT,   edx, 20);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_IBRS,      edx, 26);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_STIBP,     edx, 27);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SPEC_CTRL, edx, 26);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_ARCH_CAP,  edx, 29);

        /* ---- Leaf 0x07 ECX=1: FRED, LKGS, AVX10, APX ---- */
        if (eax >= 1) {  /* eax from leaf 0x07 ECX=0 = max subleaf */
            cpuid_raw(0x07, 1, &eax, &ebx, &ecx, &edx);

            /* EAX */
            set_flag_if(&g_cpu.flags, CPU_FEATURE_FRED,  eax, 17);
            set_flag_if(&g_cpu.flags, CPU_FEATURE_LKGS,  eax, 18);

            /* EDX */
            set_flag_if(&g_cpu.flags, CPU_FEATURE_AVX10, edx, 19);
            set_flag_if(&g_cpu.flags, CPU_FEATURE_APX,   edx, 21);
        }
    }

    /* ---- Leaf 0x0D ECX=0: XSAVE state sizes ---- */
    if (g_cpu.max_leaf >= 0x0D) {
        cpuid_raw(0x0D, 0, &eax, &ebx, &ecx, &edx);
        g_cpu.xsave_size     = ebx;  /* current enabled size */
        g_cpu.xsave_size_max = ecx;  /* max size for all features */
        g_cpu.xcr0_supported = ((uint64_t)edx << 32) | eax;
    }

    /* ---- Extended leaves: 0x80000001, 0x80000002–4, 0x80000007 ---- */
    cpuid_raw(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    g_cpu.max_ext_leaf = eax;

    /* Leaf 0x80000001: AMD extended features */
    if (g_cpu.max_ext_leaf >= 0x80000001) {
        cpuid_raw(0x80000001, 0, &eax, &ebx, &ecx, &edx);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_NX,      edx, 20);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_LM,      edx, 29);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SYSCALL,  edx, 11);
    }

    /* Leaf 0x80000002–4: Brand string */
    if (g_cpu.max_ext_leaf >= 0x80000004) {
        uint32_t *b = (uint32_t *)g_cpu.brand;
        uint32_t leaf;
        for (leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid_raw(leaf, 0, &eax, &ebx, &ecx, &edx);
            *b++ = eax; *b++ = ebx; *b++ = ecx; *b++ = edx;
        }
        g_cpu.brand[48] = '\0';
    }

    /* Leaf 0x80000007: Invariant TSC */
    if (g_cpu.max_ext_leaf >= 0x80000007) {
        cpuid_raw(0x80000007, 0, &eax, &ebx, &ecx, &edx);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_TSC_INV, edx, 8);
    }

    /* ---- Log results ---- */
    {
        /* Skip leading spaces in brand string */
        const char *brand = g_cpu.brand;
        while (*brand == ' ') brand++;

        klog(LOG_INFO, "cpu", "%s", brand[0] ? brand : g_cpu.vendor);
        klog(LOG_INFO, "cpu", "family=%u model=%u stepping=%u",
             (uint64_t)g_cpu.family, (uint64_t)g_cpu.model,
             (uint64_t)g_cpu.stepping);

        /* Log feature summary — build a compact feature string */
        klog(LOG_INFO, "cpu", "Features: %s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
             cpu_has(CPU_FEATURE_SSE2)    ? "SSE2 "    : "",
             cpu_has(CPU_FEATURE_SSE4_2)  ? "SSE4.2 "  : "",
             cpu_has(CPU_FEATURE_AES)     ? "AES "     : "",
             cpu_has(CPU_FEATURE_AVX)     ? "AVX "     : "",
             cpu_has(CPU_FEATURE_AVX2)    ? "AVX2 "    : "",
             cpu_has(CPU_FEATURE_AVX512F) ? "AVX-512 " : "",
             cpu_has(CPU_FEATURE_XSAVE)   ? "XSAVE "   : "",
             cpu_has(CPU_FEATURE_PCID)    ? "PCID "    : "",
             cpu_has(CPU_FEATURE_SMEP)    ? "SMEP "    : "",
             cpu_has(CPU_FEATURE_SMAP)    ? "SMAP "    : "",
             cpu_has(CPU_FEATURE_UMIP)    ? "UMIP "    : "",
             cpu_has(CPU_FEATURE_RDRAND)  ? "RDRAND "  : "",
             cpu_has(CPU_FEATURE_TSC_INV) ? "InvTSC "  : "",
             cpu_has(CPU_FEATURE_NX)      ? "NX "      : "",
             "");

        if (g_cpu.xsave_size > 0) {
            klog(LOG_DEBUG, "cpu", "XSAVE area: %u bytes (max %u)",
                 (uint64_t)g_cpu.xsave_size,
                 (uint64_t)g_cpu.xsave_size_max);
        }
    }

    /* Suppress unused warning */
    (void)cpuid_memcpy;
}
