/* ============================================================================
 * cpuid.c -- Centralized CPUID Feature Detection
 *
 * Probes all relevant CPUID leaves at boot and stores results in a global
 * struct cpu_features. Replaces ad-hoc inline asm CPUID scattered across
 * hw_dump.c, gfx_simd.c, and registry.c.
 *
 * Call cpuid_init() once from kernel_main() after heap_init().
 * ============================================================================ */

#include "kernel/cpuid.h"
#include "kernel/klog.h"
#include "kernel/msr.h"
#include "kernel/cpu_regs.h"   /* CR4_OSXSAVE (shared CR-bit defines) */

/* Global CPU features -- zero-initialized at startup */
struct cpu_features g_cpu;

/* TSC_AUX MSR availability (set by boot_hw.c after BSP probe) */
int g_tsc_aux_available;

/* --- Low-level CPUID wrapper --- */

void cpuid_raw(uint32_t leaf, uint32_t subleaf,
               uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile ("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf));
}

/* --- Helpers --- */

static void set_flag_if(cpu_feature_mask_t *flags, enum cpu_feature feat,
                        uint32_t reg, uint32_t bit)
{
    if (reg & (1U << bit))
        cpu_feature_set(flags, feat);
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

        /* Family/Model/Stepping -- Intel display model algorithm */
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
        set_flag_if(&g_cpu.flags, CPU_FEATURE_MTRR,  edx, 12);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_MMX,   edx, 23);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SSE,   edx, 25);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SSE2,  edx, 26);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_HTT,   edx, 28);

        /* ECX feature flags */
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SSE3,    ecx,  0);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SSSE3,   ecx,  9);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SSE4_1,  ecx, 19);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SSE4_2,  ecx, 20);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_VMX,     ecx,  5);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_AES,     ecx, 25);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_XSAVE,   ecx, 26);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_OSXSAVE, ecx, 27);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_AVX,     ecx, 28);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_RDRAND,  ecx, 30);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_PCID,    ecx, 17);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_TSC_DL,  ecx, 24);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_CX16,    ecx, 13);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_MONITOR, ecx,  3);
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
        set_flag_if(&g_cpu.flags, CPU_FEATURE_WAITPKG,  ecx,  5);
        /* SHSTK is CPUID.(07H,0):ECX[7] per Intel SDM -- NOT EDX[7] (reserved). */
        set_flag_if(&g_cpu.flags, CPU_FEATURE_CET_SS,   ecx,  7);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_LA57,     ecx, 16);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_RDPID,    ecx, 22);

        /* EDX */
        set_flag_if(&g_cpu.flags, CPU_FEATURE_UINTR,     edx,  5);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_CET_IBT,   edx, 20);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_IBRS,      edx, 26);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_STIBP,     edx, 27);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SERIALIZE, edx, 14);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_MD_CLEAR,  edx, 10);  /* VERW buffer clear (MDS) */
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SPEC_CTRL, edx, 26);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_IBPB,      edx, 26);  /* Intel: IBPB from 7.0:EDX[26]; AMD below (S8) */
        set_flag_if(&g_cpu.flags, CPU_FEATURE_ARCH_CAP,  edx, 29);

        /* eIBRS (TODO-10 S8): IBRS_ALL is in IA32_ARCH_CAPABILITIES[1], not a
         * CPUID bit. Plain msr_read is safe here -- ARCH_CAP (just set above)
         * is the CPUID gate guaranteeing the MSR exists; cpuid_init runs in
         * Phase 0, so no msr_try (no IDT yet), same pattern as the S15 SEV read. */
        if (cpu_has(CPU_FEATURE_ARCH_CAP) &&
            (msr_read(MSR_IA32_ARCH_CAPS) & ARCH_CAP_IBRS_ALL))
            cpu_feature_set(&g_cpu.flags, CPU_FEATURE_ENHANCED_IBRS);

        /* ---- Leaf 0x07 ECX=1: FRED, LKGS, AVX10, APX ---- */
        if (eax >= 1) {  /* eax from leaf 0x07 ECX=0 = max subleaf */
            cpuid_raw(0x07, 1, &eax, &ebx, &ecx, &edx);

            /* EAX */
            set_flag_if(&g_cpu.flags, CPU_FEATURE_LASS,  eax,  6);
            set_flag_if(&g_cpu.flags, CPU_FEATURE_FRED,  eax, 17);
            set_flag_if(&g_cpu.flags, CPU_FEATURE_LKGS,  eax, 18);
            set_flag_if(&g_cpu.flags, CPU_FEATURE_LAM,   eax, 26);

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

        /* Sub-leaf 1: XSAVE extensions */
        cpuid_raw(0x0D, 1, &eax, &ebx, &ecx, &edx);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_XSAVEOPT, eax, 0);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_XSAVEC,   eax, 1);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_XSAVES,   eax, 3);
    }

    /* ---- Leaf 0x0D subleaf 9: PKRU XSAVE component ---- */
    if (g_cpu.max_leaf >= 0x0D && (g_cpu.xcr0_supported & (1UL << 9))) {
        cpuid_raw(0x0D, 9, &eax, &ebx, &ecx, &edx);
        g_cpu.pkru_xsave_size   = eax;  /* component size (4 bytes) */
        g_cpu.pkru_xsave_offset = ebx;  /* byte offset in XSAVE area */
    }

    /* ---- Extended leaves: 0x80000001, 0x80000002 to 4, 0x80000007 ---- */
    cpuid_raw(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    g_cpu.max_ext_leaf = eax;

    /* Leaf 0x80000001: AMD extended features */
    if (g_cpu.max_ext_leaf >= 0x80000001) {
        cpuid_raw(0x80000001, 0, &eax, &ebx, &ecx, &edx);

        /* EDX -- common extended features */
        set_flag_if(&g_cpu.flags, CPU_FEATURE_NX,      edx, 20);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_PAGE1GB,  edx, 26);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_RDTSCP,   edx, 27);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_LM,      edx, 29);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SYSCALL,  edx, 11);

        /* ECX -- AMD extended features */
        set_flag_if(&g_cpu.flags, CPU_FEATURE_SVM,      ecx,  2);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_OSVW,     ecx,  9);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_IBS,      ecx, 10);
        set_flag_if(&g_cpu.flags, CPU_FEATURE_TOPO_EXT, ecx, 22);
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

    /* ---- Leaf 0x80000008: Address sizes and core count ---- */
    if (g_cpu.max_ext_leaf >= 0x80000008) {
        cpuid_raw(0x80000008, 0, &eax, &ebx, &ecx, &edx);
        g_cpu.phys_addr_bits   = (uint8_t)(eax & 0xFF);            /* EAX[7:0] */
        g_cpu.linear_addr_bits = (uint8_t)((eax >> 8) & 0xFF);     /* EAX[15:8] */
        g_cpu.num_cores        = (uint16_t)((ecx & 0xFF) + 1);     /* ECX[7:0] + 1; uint16 holds 256 */
        set_flag_if(&g_cpu.flags, CPU_FEATURE_IBPB, ebx, 12);      /* AMD: IBPB = 0x80000008:EBX[12] (S8) */
    }

    /* ---- Leaf 0x8000001E: Zen chiplet topology (AMD only) ---- */
    if (cpu_has(CPU_FEATURE_TOPO_EXT) &&
        g_cpu.max_ext_leaf >= 0x8000001E) {
        cpuid_raw(0x8000001E, 0, &eax, &ebx, &ecx, &edx);
        g_cpu.ext_apic_id      = eax;                              /* EAX[31:0] */
        g_cpu.compute_unit_id  = (uint8_t)(ebx & 0xFF);            /* EBX[7:0]  ComputeUnitId */
        {
            /* EBX[15:8] = ThreadsPerComputeUnit - 1 (AMD APM Vol 3); clamp +1 result at 255 */
            uint32_t tpc = ((ebx >> 8) & 0xFF) + 1;
            g_cpu.threads_per_core = (uint8_t)(tpc > 255 ? 255 : tpc);
        }
        g_cpu.node_id          = (uint8_t)(ecx & 0xFF);            /* ECX[7:0] */
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

        /* Log feature summary -- build a compact feature string */
        klog(LOG_INFO, "cpu", "Features: %s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
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
             cpu_has(CPU_FEATURE_VMX)     ? "VT-x "    : "",
             cpu_has(CPU_FEATURE_SVM)     ? "SVM "     : "",
             cpu_has(CPU_FEATURE_IBS)     ? "IBS "     : "",
             cpu_has(CPU_FEATURE_OSVW)    ? "OSVW "    : "",
             cpu_has(CPU_FEATURE_PAGE1GB) ? "Page1GB " : "",
             cpu_has(CPU_FEATURE_RDTSCP)  ? "RDTSCP "  : "",
             cpu_has(CPU_FEATURE_TOPO_EXT)? "TopoExt " : "",
             "");

        if (g_cpu.phys_addr_bits > 0) {
            klog(LOG_INFO, "cpu", "Address bits: phys=%u linear=%u, cores=%u",
                 (uint64_t)g_cpu.phys_addr_bits,
                 (uint64_t)g_cpu.linear_addr_bits,
                 (uint64_t)g_cpu.num_cores);
        }

        if (cpu_has(CPU_FEATURE_TOPO_EXT)) {
            klog(LOG_INFO, "cpu", "Zen topology: extAPIC=%u CU=%u node=%u",
                 (uint64_t)g_cpu.ext_apic_id,
                 (uint64_t)g_cpu.compute_unit_id,
                 (uint64_t)g_cpu.node_id);
        }

        /* AMD SVM detection (gate on max_ext_leaf to avoid bogus CPUID data) */
        if (cpu_has(CPU_FEATURE_SVM) && g_cpu.max_ext_leaf >= 0x8000000A) {
            uint32_t svm_eax, svm_ebx, svm_ecx2, svm_edx;
            cpuid_raw(0x8000000A, 0, &svm_eax, &svm_ebx, &svm_ecx2, &svm_edx);
            klog(LOG_INFO, "cpu", "AMD-V: rev %u, NPT=%s, ASIDs=%u",
                 (uint64_t)(svm_eax & 0xFF),
                 (svm_edx & 1) ? "yes" : "no",
                 (uint64_t)svm_ebx);
        } else if (cpu_has(CPU_FEATURE_SVM)) {
            klog(LOG_INFO, "cpu", "AMD-V: SVM present (detail leaf 0x8000000A not available)");
        }

        /* Intel VT-x detection (gate on Intel vendor + msr_try_read) */
        if (cpu_has(CPU_FEATURE_VMX) && g_cpu.vendor[0] == 'G') {
            uint64_t feat_ctrl;
            if (msr_try_read(0x3A, &feat_ctrl) == 0) {
                int locked = (feat_ctrl & 1) ? 1 : 0;
                int vmx_enabled = (feat_ctrl & (1 << 2)) ? 1 : 0;

                klog(LOG_INFO, "cpu", "Intel VT-x: locked=%s, enabled=%s",
                     locked ? "yes" : "no",
                     vmx_enabled ? "yes" : "no");
            } else {
                klog(LOG_INFO, "cpu", "Intel VT-x: FEATURE_CONTROL MSR unreadable");
            }
        }
    }

    /* ---- OSVW (OS Visible Workarounds, AMD only) ----
     * Gate on both CPUID feature AND AMD vendor. OSVW MSRs are AMD-
     * specific; a non-AMD CPU with CPUID ECX[9] set would #GP on raw
     * msr_read. Use msr_try_read for both MSRs as safety net. */
    if (cpu_has(CPU_FEATURE_OSVW) && g_cpu.vendor[0] == 'A') {
        uint64_t id_len, status;
        if (msr_try_read(MSR_AMD_OSVW_ID_LEN, &id_len) == 0 &&
            msr_try_read(MSR_AMD_OSVW_STATUS, &status) == 0) {
            g_cpu.osvw_length = (uint32_t)id_len;
            g_cpu.osvw_status = status;
            klog(LOG_INFO, "cpu", "OSVW: %u errata tracked, mask=0x%llx",
                 (uint64_t)g_cpu.osvw_length, g_cpu.osvw_status);
        } else {
            klog(LOG_WARN, "cpu", "OSVW: CPUID reports support but MSR probe failed");
        }
    }

    /* Configure XCR0 after all features are detected */
    cpu_configure_xcr0();

    /* ---- AVX10 detection ---- */
    if (cpu_has(CPU_FEATURE_AVX10) && g_cpu.max_leaf >= 0x24) {
        uint32_t avx10_ebx;
        cpuid_raw(0x24, 0, &eax, &avx10_ebx, &ecx, &edx);
        klog(LOG_INFO, "cpu",
             "[SIMD] AVX10 v%u detected; not yet enabled",
             (uint64_t)(avx10_ebx & 0xFF));
    }

    /* ---- APX detection (CPUID.(7,1):EDX[21]) ---- */
    if (cpu_has(CPU_FEATURE_APX)) {
        klog(LOG_INFO, "cpu",
             "[SIMD] APX detected (R16-R31); not yet enabled");
    }

    /* ---- Additional CPU feature adoption (WAITPKG/SERIALIZE/RDPID) ---- */
    if (cpu_has(CPU_FEATURE_WAITPKG) || cpu_has(CPU_FEATURE_SERIALIZE) ||
        cpu_has(CPU_FEATURE_RDPID)) {
        klog(LOG_INFO, "cpu",
             "[cpu] feature adoption: WAITPKG=%u SERIALIZE=%u RDPID=%u",
             (uint64_t)(cpu_has(CPU_FEATURE_WAITPKG) ? 1 : 0),
             (uint64_t)(cpu_has(CPU_FEATURE_SERIALIZE) ? 1 : 0),
             (uint64_t)(cpu_has(CPU_FEATURE_RDPID) ? 1 : 0));
    }

    /* ---- Future-silicon detection stubs (detect + log only; enablement is
     * deferred to each feature's owning subsystem) ---- */
    if (cpu_has(CPU_FEATURE_UINTR))
        klog(LOG_INFO, "cpu", "[cpu] UINTR present; not yet enabled");
    if (cpu_has(CPU_FEATURE_LA57))
        klog(LOG_INFO, "cpu", "[vmm] LA57 (57-bit VA) detected; 4-level paging active");
    if (cpu_has(CPU_FEATURE_LAM))
        klog(LOG_INFO, "cpu", "[cpu] LAM (pointer masking) detected; not yet enabled");
    if (cpu_has(CPU_FEATURE_LASS))
        klog(LOG_INFO, "cpu", "[cpu] LASS detected; enablement owned by security hardening");

    /* ---- Confidential-compute guest detection (kernel-internal cc_kind;
     * full attestation deferred) ---- */
    g_cpu.cc_kind = CC_NONE;
    if (g_cpu.max_leaf >= 0x21) {
        /* Intel TDX: leaf 0x21 sub-0 returns "IntelTDX    " in EBX/EDX/ECX */
        cpuid_raw(0x21, 0, &eax, &ebx, &ecx, &edx);
        if (ebx == 0x65746E49u && edx == 0x5844546Cu && ecx == 0x20202020u) {
            g_cpu.cc_kind = CC_INTEL_TDX;
            klog(LOG_INFO, "cpu",
                 "[cpu] confidential VM: Intel TDX guest; attestation deferred");
        }
    }
    if (g_cpu.cc_kind == CC_NONE && g_cpu.max_ext_leaf >= 0x8000001Fu) {
        /* AMD SEV: leaf 0x8000001F EAX[1] = SEV *supported* (capability) -- this
         * is true on SEV-capable bare metal too. The SEV_STATUS MSR is what says
         * we are actually a guest: bit0=SEV active, bit1=ES active, bit2=SNP
         * active. The MSR exists once SEV is supported, so reading it is safe. */
        cpuid_raw(0x8000001F, 0, &eax, &ebx, &ecx, &edx);
        if (eax & (1u << 1)) {
            /* SEV supported -> SEV_STATUS MSR is architecturally present (AMD
             * APM), so a plain rdmsr is safe and faults only on a spec-violating
             * CPU. msr_try_read is NOT usable here: cpuid_init() runs in Phase 0
             * before the IDT loads, where the #GP-trapping probe always
             * returns -1 (idt_is_loaded() == 0). */
            uint64_t sev_status = msr_read(MSR_AMD64_SEV);
            if (sev_status & 1u) {
                if (sev_status & (1u << 2))      g_cpu.cc_kind = CC_AMD_SEV_SNP;
                else if (sev_status & (1u << 1)) g_cpu.cc_kind = CC_AMD_SEV_ES;
                else                             g_cpu.cc_kind = CC_AMD_SEV;
                klog(LOG_INFO, "cpu",
                     "[cpu] confidential VM: AMD SEV guest (kind=%u); attestation deferred",
                     (uint64_t)g_cpu.cc_kind);
            }
        }
    }

    /* Suppress unused warning */
    (void)cpuid_memcpy;
}

/* ---- AP feature probe (TODO-09-boot S6) -------------------------------- */

/* Re-probe the security-critical feature subset on the calling CPU into a
 * LOCAL mask (never g_cpu). Bit positions mirror cpuid_init() exactly so the
 * AP mask and the BSP g_cpu.flags are directly comparable -- keep these in
 * lockstep with the set_flag_if() calls in cpuid_init() above. Only the
 * CPU_FEATURES_AP_PROBE_MASK subset is probed (this section validates
 * security-critical mismatches, not the full feature set). */
cpu_feature_mask_t cpuid_probe_ap_features(void)
{
    uint32_t eax, ebx, ecx, edx, max_leaf, max_ext;
    cpu_feature_mask_t m = { { 0, 0 } };

    cpuid_raw(0x00000000, 0, &max_leaf, &ebx, &ecx, &edx);
    if (max_leaf >= 0x01) {
        cpuid_raw(0x01, 0, &eax, &ebx, &ecx, &edx);
        set_flag_if(&m, CPU_FEATURE_SSE2,   edx, 26);
        set_flag_if(&m, CPU_FEATURE_SSE4_2, ecx, 20);
        set_flag_if(&m, CPU_FEATURE_XSAVE,  ecx, 26);
        set_flag_if(&m, CPU_FEATURE_AVX,    ecx, 28);
        set_flag_if(&m, CPU_FEATURE_PCID,   ecx, 17);
        set_flag_if(&m, CPU_FEATURE_CX16,   ecx, 13);
    }
    if (max_leaf >= 0x07) {
        cpuid_raw(0x07, 0, &eax, &ebx, &ecx, &edx);
        set_flag_if(&m, CPU_FEATURE_SMEP,    ebx,  7);
        set_flag_if(&m, CPU_FEATURE_AVX512F, ebx, 16);
        set_flag_if(&m, CPU_FEATURE_SMAP,    ebx, 20);
        set_flag_if(&m, CPU_FEATURE_UMIP,    ecx,  2);
        set_flag_if(&m, CPU_FEATURE_PKU,     ecx,  3);
        set_flag_if(&m, CPU_FEATURE_WAITPKG, ecx,  5);  /* drives UMWAIT_CONTROL replay */
        set_flag_if(&m, CPU_FEATURE_SPEC_CTRL, edx, 26); /* drives SPEC_CTRL eIBRS replay (S8) */
    }
    cpuid_raw(0x80000000, 0, &max_ext, &ebx, &ecx, &edx);
    if (max_ext >= 0x80000001) {
        cpuid_raw(0x80000001, 0, &eax, &ebx, &ecx, &edx);
        set_flag_if(&m, CPU_FEATURE_SYSCALL, edx, 11);
        set_flag_if(&m, CPU_FEATURE_NX,      edx, 20);
        set_flag_if(&m, CPU_FEATURE_RDTSCP,  edx, 27);
        set_flag_if(&m, CPU_FEATURE_LM,      edx, 29);
    }
    return cpu_feature_and(m, CPU_FEATURES_AP_PROBE_MASK);
}

/* ---- XCR0 configuration ------------------------------------------------ */

void cpu_configure_xcr0(void)
{
    uint64_t mask;

    if (!cpu_has(CPU_FEATURE_XSAVE))
        return;

    /* Set CR4.OSXSAVE to enable XGETBV/XSETBV */
    {
        uint64_t cr4;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= CR4_OSXSAVE;
        __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
    }

    /* Build mask from supported features */
    mask = 0x03;  /* bits 0,1: x87 + SSE (always required) */

    if (cpu_has(CPU_FEATURE_AVX))
        mask |= (1UL << 2);  /* bit 2: AVX (YMM) */

    if (cpu_has(CPU_FEATURE_AVX512F)) {
        mask |= (1UL << 5);  /* bit 5: opmask (k0-k7) */
        mask |= (1UL << 6);  /* bit 6: ZMM_Hi256 */
        mask |= (1UL << 7);  /* bit 7: Hi16_ZMM */
    }

    if (cpu_has(CPU_FEATURE_PKU)) {
        /* PKRU (bit 9) -- per-thread PKRU state saved/restored via XSAVE.
         * Intel SDM Vol. 1 Section 2.7: PKRU is XCR0 component 9. */
        if (g_cpu.xcr0_supported & (1UL << 9))
            mask |= (1UL << 9);
    }

    /* Filter to only bits the CPU actually supports */
    mask &= g_cpu.xcr0_supported;

    /* Write XCR0 */
    {
        uint32_t lo = (uint32_t)mask;
        uint32_t hi = (uint32_t)(mask >> 32);
        __asm__ volatile ("xsetbv" : : "a"(lo), "d"(hi), "c"((uint32_t)0));
    }

    g_cpu.xcr0_active = mask;
}
