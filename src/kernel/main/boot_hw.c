/* ============================================================================
 * boot_hw.c -- Phase 0: Critical Init (Interrupts Disabled)
 *
 * Runs with interrupts off. Only serial, memory, and logging. No drivers,
 * VFS, or network. Any failure in Phase 0 calls boot_halt() on serial --
 * framebuffer is not yet available.
 *
 * Provides: boot_phase0() and the legacy boot_hw_init() wrapper.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/cpuid.h"
#include "kernel/multiboot2.h"
#include "kernel/boot_info.h"
#include "gfx_simd.h"
#include "kernel/drivers/serial.h"
#include "kernel/klog.h"
#include "kernel/boot_timing.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/mm/heap.h"
#include "kernel/version.h"
#include "kernel/uefi_runtime.h"
#include "kernel/tpm.h"
#include "kernel/boot_init.h"
#include "kernel/boot_halt.h"
#include "kernel/cpu_security.h"
#include "kernel/smp.h"
#include "kernel/msr.h"
#include "main/main_internal.h"

/* External: Multiboot2 parser */
extern void multiboot2_parse(uintptr_t mbi_addr);

/* UEFI boot magic */
#define UEFI_BOOT_MAGIC 0x55454649ULL  /* "UEFI" */

/* ---- Phase 0 ------------------------------------------------------------ */

void boot_phase0(uint64_t magic, uint64_t mbi)
{
    uint32_t i;

    /* --- BSP per-CPU data: GS_BASE must be valid before ANY interrupt fires.
     * On bare metal, GS_BASE defaults to 0; gs:0 reads IVT garbage instead
     * of NULL, crashing the IRQL tracking in isr_handler. */
    smp_early_bsp_init();

    /* --- Serial: absolute first call, no dependencies --- */
    POST16(POST16_SERIAL);
    serial_init();
    POST16(POST16_SERIAL_OK);
    kernel_subsystem_set_ready(SUBSYS_SERIAL, true);
    boot_progress(0, "SERIAL", POST16_SERIAL_OK);

    klog(LOG_DEBUG, "", "========================================================================");
    klog(LOG_DEBUG, "", "  Impossible OS -- Boot Log");
    klog(LOG_DEBUG, "", "========================================================================");
    version_print();

    /* --- Boot info parse: UEFI or Multiboot2 --- */
    if (magic == UEFI_BOOT_MAGIC) {
        struct boot_info *src = (struct boot_info *)(uintptr_t)mbi;
        uint8_t *d = (uint8_t *)&g_boot_info;
        const uint8_t *s = (const uint8_t *)src;
        for (i = 0; i < sizeof(struct boot_info); i++)
            d[i] = s[i];
        klog(LOG_INFO, "UEFI", "Boot info received from Impossible OS bootloader");
    } else if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        multiboot2_parse((uintptr_t)mbi);
    } else {
        boot_halt("Unknown bootloader magic");
    }
    boot_progress(0, "BOOT_INFO", 0x0026);

    /* boot_config sanity check: verify the struct wasn't corrupted in transit.
     * The bootloader fills boot_config at a fixed layout; if cmdline contains
     * non-printable garbage in the first 4 bytes, the struct shifted. */
    {
        const char *cmd = g_boot_info.config.cmdline;
        int i;
        for (i = 0; i < 4 && cmd[i] != '\0'; i++) {
            if (cmd[i] < 0x20 || cmd[i] > 0x7E) {
                klog(LOG_FATAL, "CONF",
                     "boot_config.cmdline contains non-ASCII at byte %d "
                     "(0x%02X) -- struct layout mismatch?",
                     (uint64_t)i, (uint64_t)(uint8_t)cmd[i]);
                boot_halt("boot_config struct layout corrupted");
            }
        }
    }

    /* boot_config_parse: boot.conf is parsed by the UEFI bootloader before
     * kernel entry and delivered in g_boot_info.config.  Log the values. */
    klog(LOG_INFO, "CONF",
         "boot.conf: debug=%d verbose=%d serial=%d mode=%d "
         "splash=%ds heartbeat=%d postcode=%d test=%d%s",
           g_boot_info.config.debug, g_boot_info.config.verbose,
           g_boot_info.config.serial_debug, g_boot_info.config.boot_mode,
           g_boot_info.config.splash_timeout, g_boot_info.config.heartbeat,
           g_boot_info.config.postcode, g_boot_info.config.test,
           g_boot_info.config.config_found ? "" : " (defaults)");

    /* --- UEFI runtime services (SetVirtualAddressMap + RT props) --- */
    POST16(POST16_UEFI_RT);
    {
        boot_result_t r = uefi_runtime_init();
        if (r == BOOT_DEGRADED)
            klog(LOG_WARN, "UEFI", "Runtime services unavailable -- degraded");
    }

    /* --- Read prior boot POST code, then mark "booting" in NVRAM ---
     * Read MUST happen before the first NVRAM write -- otherwise we read
     * our own value.  NVRAM writes per boot (5 total, flash-safe):
     *   1. Here: mark "booting" (POST16_SERIAL = 0x0010)
     *   2. Phase 0 complete (POST16_SIMD_OK)
     *   3. Phase 1 complete (POST16_TIMER_OK)
     *   4. Phase 2 complete (POST16_REGISTRY_OK)
     *   5. Phase 3 complete (POST16_BOOT_OK) */
    {
        int last_post = boot_post_read16();
        boot_post_nvram_write16(POST16_SERIAL);  /* NVRAM: booting */
        POST16(POST16_UEFI_RT_OK);

        if (last_post >= 0) {
            extern void vpd_crash_banner(uint16_t);
            if (last_post == (int)POST16_BOOT_OK)
                serial_write("[BOOT] Last boot succeeded\n");
            else
                serial_write("[BOOT] Last boot failed\n");
            vpd_crash_banner((uint16_t)last_post);
        }
        boot_progress(0, "post-code-log", 0x11);
    }

    /* Read previous boot perf data from NVRAM (before we overwrite it) */
    boot_perf_read_prev();

    /* Initialize VPD Tier 1 -- after crash banner so s_banner_shown is set */
    {
        extern void vpd_init(void);
        vpd_init();
    }

    /* --- UEFI variable services (NVRAM enumeration) --- */
    POST16(POST16_UEFI_VARS);
    uefi_vars_init();
    uefi_time_init();
    POST16(POST16_UEFI_VARS_OK);

    /* --- Secure Boot state detection --- */
    POST16(POST16_SECUREBOOT);
    uefi_secureboot_init();
    POST16(POST16_SECUREBOOT_OK);

    /* --- TPM measured boot (parse event log) --- */
    POST16(POST16_TPM);
    tpm_init();
    tpm_integrity_init();
    POST16(POST16_TPM_OK);

    /* --- Physical memory manager: BOOT_FATAL if fails --- */
    POST16(POST16_PMM);
    if (pmm_init() != BOOT_OK)
        boot_halt("PMM init failed -- no usable physical memory");
    POST16(POST16_PMM_OK);
    kernel_subsystem_set_ready(SUBSYS_PMM, true);
    boot_progress(0, "PMM", POST16_PMM_OK);

    /* --- Virtual memory manager: BOOT_FATAL if PMM not ready --- */
    if (!kernel_subsystem_ready(SUBSYS_PMM))
        boot_halt("PMM not ready -- cannot init VMM");
    POST16(POST16_VMM);
    if (vmm_init() != BOOT_OK)
        boot_halt("VMM init failed -- cannot set up page tables");
    POST16(POST16_VMM_OK);
    kernel_subsystem_set_ready(SUBSYS_VMM, true);
    boot_progress(0, "VMM", POST16_VMM_OK);

    /* Validate vmm_map_mmio_uc: map LAPIC base as UC, compare with identity-mapped read */
    POST16(0xD100);
    {
        volatile uint32_t *lapic_id = (volatile uint32_t *)0xFEE00020;
        uint32_t id_identity = *lapic_id;
        void *uc = vmm_map_mmio_uc(0xFEE00000, 4096);
        POST16(0xD101);
        if (uc) {
            volatile uint32_t *uc_id = (volatile uint32_t *)((uintptr_t)uc + 0x20);
            uint32_t id_uc = *uc_id;
            if (id_identity == id_uc)
                klog(LOG_INFO, "mm", "vmm_map_mmio_uc: LAPIC ID match (%p -> 0x%x)",
                     (uint64_t)(uintptr_t)uc, (uint64_t)id_uc);
            else
                klog(LOG_WARN, "mm", "vmm_map_mmio_uc: LAPIC ID mismatch (0x%x vs 0x%x)",
                     (uint64_t)id_identity, (uint64_t)id_uc);
            vmm_unmap_mmio(uc, 4096);
        } else {
            klog(LOG_WARN, "mm", "vmm_map_mmio_uc: LAPIC test failed (NULL)");
        }
    }

    /* --- Kernel heap: BOOT_FATAL if VMM not ready --- */
    if (!kernel_subsystem_ready(SUBSYS_VMM))
        boot_halt("VMM not ready -- cannot init heap");
    POST16(POST16_HEAP);
    if (heap_init() != BOOT_OK)
        boot_halt("Heap init failed -- cannot allocate kernel heap");
    POST16(POST16_HEAP_OK);
    kernel_subsystem_set_ready(SUBSYS_HEAP, true);
    boot_progress(0, "HEAP", POST16_HEAP_OK);

    /* --- klog early init: ring buffer + serial only (no disk yet) --- */
    POST16(POST16_KLOG);
    klog_early_init();
    POST16(POST16_KLOG_OK);
    kernel_subsystem_set_ready(SUBSYS_KLOG, true);
    boot_progress(0, "KLOG", POST16_KLOG_OK);

    /* Recover crash log from previous boot (needs PMM + UEFI runtime) */
    klog_crash_recover();

    /* --- CPUID: probe CPU features --- */
    POST16(POST16_CPUID);
    cpuid_init();
    POST16(POST16_CPUID_OK);
    boot_progress(0, "CPUID", POST16_CPUID_OK);

    /* --- CPU feature minimum requirements --- */
    {
        int missing = 0;
        if (!cpu_has(CPU_FEATURE_NX)) {
            klog(LOG_FATAL, "cpu", "MINIMUM: NX (No-Execute) not available -- cannot boot safely");
            missing = 1;
        }
        if (!cpu_has(CPU_FEATURE_SSE2)) {
            klog(LOG_FATAL, "cpu", "MINIMUM: SSE2 not available -- required for kernel math");
            missing = 1;
        }
        if (missing)
            boot_halt("CPU does not meet minimum requirements (NX + SSE2)");

        /* Recommended features: warn if missing, continue */
        if (!cpu_has(CPU_FEATURE_SMEP))
            klog(LOG_WARN, "cpu", "RECOMMENDED: SMEP not available");
        if (!cpu_has(CPU_FEATURE_SMAP))
            klog(LOG_WARN, "cpu", "RECOMMENDED: SMAP not available");
        if (!cpu_has(CPU_FEATURE_RDRAND))
            klog(LOG_WARN, "cpu", "RECOMMENDED: RDRAND not available");
    }

    /* --- CPU security hardening: NX (SMEP/SMAP deferred until page tables fixed) --- */
    POST16(POST16_CPU_HARDEN);
    cpu_harden();
    POST16(POST16_CPU_HARDEN_OK);

    /* --- Apply NX policy + clear User bit from kernel pages --- */
    POST16(POST16_NX_POLICY);
    vmm_apply_nx_policy();
    POST16(POST16_NX_POLICY_OK);

    /* --- PAT: reprogram entry 1 from WT to WC for framebuffer VRAM ---
     * Intel default PAT: 0x0007040600070406
     *   Entry 0=WB(06) 1=WT(04) 2=UC-(07) 3=UC(00) 4=WB 5=WT 6=UC- 7=UC
     * We change entry 1 to WC(01).  PTE bits PWT=1,PCD=0 select entry 1.
     * Nothing in the kernel uses PWT=1 alone, so this is safe.  UC (entry 3,
     * PWT=1+PCD=1) and WB (entry 0, PWT=0+PCD=0) are unchanged. */
    POST16(POST16_PAT);
    {
        uint64_t pat_old = msr_read(MSR_IA32_PAT);
        uint64_t pat_new = 0x0007040600010406ULL;  /* entry 1: WT(04)->WC(01) */
        msr_write(MSR_IA32_PAT, pat_new);

        /* Guardrail: readback verify -- if MSR write was ignored or trapped,
         * vmm_map_mmio_wc() will silently produce WT pages instead of WC. */
        uint64_t pat_verify = msr_read(MSR_IA32_PAT);
        if (pat_verify != pat_new) {
            klog(LOG_FATAL, "mm", "PAT MSR readback mismatch: wrote 0x%016llx, read 0x%016llx",
                 pat_new, pat_verify);
            boot_halt("PAT MSR write failed -- WC framebuffer not possible");
        }
        klog(LOG_INFO, "mm", "PAT: 0x%016llx -> 0x%016llx (entry 1 = WC)",
             pat_old, pat_new);
    }
    POST16(POST16_PAT_OK);

    /* --- Now safe to enable SMEP/SMAP (kernel pages no longer User) --- */
    cpu_harden_post_pagetable();
    cpu_verify_hardening();

    /* --- SIMD: enable AVX2 or fall back to SSE2 --- */
    POST16(POST16_SIMD);
    simd_enable_avx();
    if (simd_avx2_ok)
        klog(LOG_INFO, "simd", "AVX2 enabled (8 pixels/iter)");
    else
        klog(LOG_WARN, "simd", "AVX2 not available, using SSE2 fallback");
    boot_progress(0, "SIMD", POST16_SIMD_OK);

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "[PHASE0] complete -- serial, memory, klog, CPUID, SIMD ready");

    /* NVRAM write: Phase 0 complete (0x0091 = SIMD_OK) */
    boot_post_nvram_write16(POST16_SIMD_OK);
}

/* ---- Legacy wrapper (until main.c switches to boot_phase0) -------------- */

void boot_hw_init(uint64_t magic, uint64_t mbi)
{
    boot_phase0(magic, mbi);
}
