/* ============================================================================
 * cpuid_platform.c -- CPUID-based Platform Detection
 *
 * Detects hypervisor (or bare metal) by reading CPUID leaves at boot.
 * Called by the Unified Timer Subsystem to choose the right backend.
 *
 * Detection flow:
 *   1. CPUID.01H:ECX bit 31 → hypervisor present?
 *   2. CPUID leaf 0x40000000 → 12-byte vendor string (EBX+ECX+EDX)
 *   3. Match known signatures:
 *        "Microsoft Hv"  → PLATFORM_HYPERV
 *        "VMwareVMware"  → PLATFORM_VMWARE
 *        "VBoxVBoxVBox"  → PLATFORM_VIRTUALBOX
 *        "KVMKVMKVM\0\0\0" → PLATFORM_QEMU_KVM
 *        "TCGTCGTCGTCG"  → PLATFORM_QEMU_TCG
 *   4. No HV bit → PLATFORM_BARE_METAL
 *   5. Unknown HV → PLATFORM_UNKNOWN_HV
 *
 * Reference: Intel SDM Vol. 2A -- CPUID instruction
 * ============================================================================ */

#include "kernel/cpuid_platform.h"
#include "kernel/boot_info.h"
#include "kernel/boot_init.h"
#include "kernel/klog.h"

/* ---- CPUID helper (duplicated from vmbus.c to avoid coupling) ---- */

static inline void cpuid(uint32_t leaf,
                          uint32_t *eax, uint32_t *ebx,
                          uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf));
}

/* ---- State ---- */

static platform_id_t cached_platform = PLATFORM_BARE_METAL;
static int detected;  /* 0 = not yet called */

/* ---- 4-byte comparison (no memcmp in freestanding) ---- */

static int u32_eq(uint32_t reg, const char *s)
{
    const uint8_t *r = (const uint8_t *)&reg;
    return r[0] == (uint8_t)s[0] &&
           r[1] == (uint8_t)s[1] &&
           r[2] == (uint8_t)s[2] &&
           r[3] == (uint8_t)s[3];
}

/* ---- Public API ---- */

platform_id_t platform_detect(void)
{
    uint32_t eax, ebx, ecx, edx;

    if (detected)
        return cached_platform;

    detected = 1;

    /* Step 1: Check CPUID.01H:ECX bit 31 -- hypervisor present bit */
    cpuid(0x01, &eax, &ebx, &ecx, &edx);

    if (!(ecx & (1u << 31))) {
        /* No hypervisor -- bare metal */
        cached_platform = PLATFORM_BARE_METAL;
        klog(LOG_INFO, "platform", "Detected: Bare Metal (no hypervisor bit)");
        return cached_platform;
    }

    /* Step 2: Read CPUID leaf 0x40000000 -- hypervisor vendor string.
     * EAX = max supported HV CPUID leaf; EBX:ECX:EDX = 12-byte ASCII vendor ID
     * (not null-terminated). */
    cpuid(0x40000000, &eax, &ebx, &ecx, &edx);
    uint32_t hv_max_leaf = eax;

    /* Step 3: Match vendor strings.
     * EBX=chars[0..3], ECX=chars[4..7], EDX=chars[8..11] */

    /* "Microsoft Hv" -- Hyper-V (Gen 1 and Gen 2).
     * TLFS privilege flags (CPUID 0x40000003 EAX): bit 9 =
     * AccessPartitionReferenceTsc (MSR 0x40000021), bit 11 =
     * AccessFrequencyMsrs (MSR 0x40000022, 0x40000023). TLB-flush
     * hypercall is advertised by CPUID 0x40000004 EAX bit 2
     * (UseHypercallForRemoteFlush). A vendor-string match alone is
     * not sufficient -- constrained partitions can omit any of these. */
    if (u32_eq(ebx, "Micr") && u32_eq(ecx, "osof") && u32_eq(edx, "t Hv")) {
        uint32_t hv_flags = 0;
        cached_platform = PLATFORM_HYPERV;
        if (hv_max_leaf >= 0x40000003) {
            uint32_t p_eax, p_ebx, p_ecx, p_edx;
            cpuid(0x40000003, &p_eax, &p_ebx, &p_ecx, &p_edx);
            if (p_eax & (1u << 9))  hv_flags |= HV_FLAG_TSC_ENLIGHTENMENT;
            if (p_eax & (1u << 11)) hv_flags |= HV_FLAG_APIC_FREQ_MSR;
        }
        if (hv_max_leaf >= 0x40000004) {
            uint32_t r_eax, r_ebx, r_ecx, r_edx;
            cpuid(0x40000004, &r_eax, &r_ebx, &r_ecx, &r_edx);
            if (r_eax & (1u << 2)) hv_flags |= HV_FLAG_TLBFLUSH_HYPERCALL;
        }
        g_boot_info.hv_flags = hv_flags;
        {
            const char *s = "Microsoft Hv";
            int k;
            for (k = 0; s[k] && k < 15; k++) g_boot_info.hv_vendor[k] = s[k];
            g_boot_info.hv_vendor[k] = '\0';
        }
        klog(LOG_INFO, "platform",
             "Detected: Hyper-V (CPUID 0x40000000, max_leaf=0x%x, flags=0x%x)",
             (uint64_t)hv_max_leaf, (uint64_t)g_boot_info.hv_flags);
        return cached_platform;
    }

    /* "VMwareVMware" -- VMware Workstation / Fusion / ESXi.
     * VMware has no CPUID feature bit for the backdoor protocol itself --
     * the "VMwareVMware" vendor-string match IS the canonical backdoor
     * presence indicator per VMware KB 1009458. APIC bus frequency lives
     * in CPUID leaf 0x40000010 EBX (kHz); gate the flag on a non-zero
     * value so a constrained VMware partition that omits the leaf does
     * not advertise a fast-path that fails. */
    if (u32_eq(ebx, "VMwa") && u32_eq(ecx, "reVM") && u32_eq(edx, "ware")) {
        uint32_t hv_flags = HV_FLAG_VMWARE_BACKDOOR;
        cached_platform = PLATFORM_VMWARE;
        if (hv_max_leaf >= 0x40000010) {
            uint32_t f_eax, f_ebx, f_ecx, f_edx;
            cpuid(0x40000010, &f_eax, &f_ebx, &f_ecx, &f_edx);
            if (f_ebx != 0) hv_flags |= HV_FLAG_APIC_FREQ_MSR;
        }
        g_boot_info.hv_flags = hv_flags;
        {
            const char *s = "VMwareVMware";
            int k;
            for (k = 0; s[k] && k < 15; k++) g_boot_info.hv_vendor[k] = s[k];
            g_boot_info.hv_vendor[k] = '\0';
        }
        klog(LOG_INFO, "platform",
             "Detected: VMware (CPUID 0x40000000, flags=0x%x)",
             (uint64_t)g_boot_info.hv_flags);
        return cached_platform;
    }

    /* "VBoxVBoxVBox" -- Oracle VirtualBox */
    if (u32_eq(ebx, "VBox") && u32_eq(ecx, "VBox") && u32_eq(edx, "VBox")) {
        cached_platform = PLATFORM_VIRTUALBOX;
        {
            const char *s = "VBoxVBoxVBox";
            int k;
            for (k = 0; s[k] && k < 15; k++) g_boot_info.hv_vendor[k] = s[k];
            g_boot_info.hv_vendor[k] = '\0';
        }
        klog(LOG_INFO, "platform", "Detected: VirtualBox (CPUID 0x40000000)");
        return cached_platform;
    }

    /* "KVMKVMKVM\0\0\0" -- KVM (Linux host, hardware virtualization).
     * KVM publishes paravirt features via CPUID 0x40000001 EAX:
     *   bit 0 = KVM_FEATURE_CLOCKSOURCE
     *   bit 3 = KVM_FEATURE_CLOCKSOURCE2
     *   bit 5 = KVM_FEATURE_STEAL_TIME
     * Vendor-string match alone is not sufficient; a KVM guest with steal
     * time disabled on the host (`-cpu host,-kvm-steal-time`) still has
     * the vendor string but no MSR. Reference: Documentation/virt/kvm/cpuid.rst. */
    if (u32_eq(ebx, "KVMK") && u32_eq(ecx, "VMKV") && u32_eq(edx, "M\0\0\0")) {
        uint32_t hv_flags = 0;
        cached_platform = PLATFORM_QEMU_KVM;
        if (hv_max_leaf >= 0x40000001) {
            uint32_t k_eax, k_ebx, k_ecx, k_edx;
            cpuid(0x40000001, &k_eax, &k_ebx, &k_ecx, &k_edx);
            if (k_eax & (1u << 5)) hv_flags |= HV_FLAG_KVM_STEAL_TIME;
        }
        /* APIC bus frequency leaf (0x40000010 EBX, kHz) -- same capability
         * probe as VMware above: most KVM configs cap max_leaf at
         * 0x40000001, so the flag stays clear and calibration falls
         * through to the measured tiers instead of trusting a missing
         * leaf's garbage. */
        if (hv_max_leaf >= 0x40000010) {
            uint32_t f_eax, f_ebx, f_ecx, f_edx;
            cpuid(0x40000010, &f_eax, &f_ebx, &f_ecx, &f_edx);
            if (f_ebx != 0) hv_flags |= HV_FLAG_APIC_FREQ_MSR;
        }
        g_boot_info.hv_flags = hv_flags;
        {
            const char *s = "KVMKVMKVM";
            int k;
            for (k = 0; s[k] && k < 15; k++) g_boot_info.hv_vendor[k] = s[k];
            g_boot_info.hv_vendor[k] = '\0';
        }
        klog(LOG_INFO, "platform",
             "Detected: QEMU/KVM (CPUID 0x40000000, max_leaf=0x%x, flags=0x%x)",
             (uint64_t)hv_max_leaf, (uint64_t)g_boot_info.hv_flags);
        return cached_platform;
    }

    /* "TCGTCGTCGTCG" -- QEMU TCG (software emulation, no hardware virt) */
    if (u32_eq(ebx, "TCGT") && u32_eq(ecx, "CGTC") && u32_eq(edx, "GTCG")) {
        cached_platform = PLATFORM_QEMU_TCG;
        klog(LOG_INFO, "platform", "Detected: QEMU/TCG (CPUID 0x40000000)");
        return cached_platform;
    }

    /* Unknown hypervisor -- log the raw bytes for debugging */
    cached_platform = PLATFORM_UNKNOWN_HV;
    klog(LOG_WARN, "platform",
         "Unknown hypervisor: CPUID 0x40000000 = 0x%x-%x-%x",
         (uint64_t)ebx, (uint64_t)ecx, (uint64_t)edx);
    return cached_platform;
}

platform_id_t platform_get(void)
{
    return cached_platform;
}

const char *platform_name(void)
{
    switch (cached_platform) {
    case PLATFORM_BARE_METAL:  return "Bare Metal";
    case PLATFORM_HYPERV:      return "Hyper-V";
    case PLATFORM_VMWARE:      return "VMware";
    case PLATFORM_VIRTUALBOX:  return "VirtualBox";
    case PLATFORM_QEMU_KVM:    return "QEMU/KVM";
    case PLATFORM_QEMU_TCG:    return "QEMU/TCG";
    case PLATFORM_UNKNOWN_HV:  return "Unknown Hypervisor";
    }
    return "Unknown";
}

int platform_is_tcg(void)
{
    return cached_platform == PLATFORM_QEMU_TCG;
}

int platform_has_apic_freq_msr(void)
{
    switch (cached_platform) {
    case PLATFORM_HYPERV:
        /* Hyper-V: MSR 0x40000023 (HV_X64_MSR_APIC_FREQUENCY).
         * Constrained Hyper-V partitions can omit the AccessFrequencyMsrs
         * privilege (CPUID 0x40000003 EAX bit 11); platform_detect() decodes
         * that into HV_FLAG_APIC_FREQ_MSR. Mirror the flag here so callers
         * see the same truth that cal_try_hyperv_msr gates on. */
        return (g_boot_info.hv_flags & HV_FLAG_APIC_FREQ_MSR) ? 1 : 0;

    case PLATFORM_VMWARE:
    case PLATFORM_QEMU_KVM:
        /* VMware/KVM: CPUID leaf 0x40000010 EBX = APIC bus freq (kHz).
         * platform_detect() probes the leaf (max_leaf + nonzero EBX) and
         * records HV_FLAG_APIC_FREQ_MSR; mirror that flag so callers and
         * cal_try_vmware_cpuid() all see one truth. */
        return (g_boot_info.hv_flags & HV_FLAG_APIC_FREQ_MSR) ? 1 : 0;

    case PLATFORM_BARE_METAL: {
        /* Bare metal: check CPUID leaf 0x15 (Time Stamp Counter) */
        uint32_t eax, ebx, ecx, edx;
        cpuid(0x00, &eax, &ebx, &ecx, &edx);
        if (eax >= 0x15) {
            cpuid(0x15, &eax, &ebx, &ecx, &edx);
            /* If EAX and EBX are non-zero, crystal clock ratio is available */
            if (eax != 0 && ebx != 0)
                return 1;
        }
        return 0;
    }

    default:
        return 0;
    }
}
