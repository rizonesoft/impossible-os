/* ============================================================================
 * boot_hw.c — Phase 0: Critical Init (Interrupts Disabled)
 *
 * Runs with interrupts off. Only serial, memory, and logging. No drivers,
 * VFS, or network. Any failure in Phase 0 calls boot_halt() on serial —
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
#include "kernel/hv_bar.h"
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
    serial_init();
    kernel_subsystem_set_ready(SUBSYS_SERIAL, true);
    boot_progress(0, "SERIAL", POSTCODE_SERIAL_INIT);

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
    boot_progress(0, "BOOT_INFO", POSTCODE_BOOT_CFG);

    /* boot_config_parse: boot.conf is parsed by the UEFI bootloader before
     * kernel entry and delivered in g_boot_info.config.  Log the values. */
    klog(LOG_INFO, "CONF", "boot.conf: debug=%d verbose=%d serial=%d mode=%d splash=%ds heartbeat=%d postcode=%d%s",
           g_boot_info.config.debug, g_boot_info.config.verbose,
           g_boot_info.config.serial_debug, g_boot_info.config.boot_mode,
           g_boot_info.config.splash_timeout, g_boot_info.config.heartbeat,
           g_boot_info.config.postcode,
           g_boot_info.config.config_found ? "" : " (defaults)");

    /* --- UEFI runtime services (SetVirtualAddressMap + RT props) --- */
    {
        boot_result_t r = uefi_runtime_init();
        if (r == BOOT_DEGRADED)
            klog(LOG_WARN, "UEFI", "Runtime services unavailable -- degraded");
    }

    /* --- Read prior boot POST code from UEFI NVRAM --- */
    {
        int last_post = boot_post_read();
        if (last_post >= 0) {
            serial_write("[POST] Last boot code: 0x");
            {
                static const char hex[] = "0123456789ABCDEF";
                serial_putchar(hex[(last_post >> 4) & 0xF]);
                serial_putchar(hex[last_post & 0xF]);
            }
            if (last_post == 0xFF)
                serial_write(" (OK)\n");
            else if (last_post == 0xFE)
                serial_write(" (FAILED -- prior boot crashed)\n");
            else
                serial_write(" (incomplete -- prior boot did not finish)\n");
        }
        boot_progress(10, "post-code-log", 0x11);
    }

    /* --- UEFI variable services (NVRAM enumeration) --- */
    uefi_vars_init();

    /* --- UEFI RTC time (seed wall clock) --- */
    uefi_time_init();

    /* --- Secure Boot state detection --- */
    uefi_secureboot_init();

    /* --- TPM measured boot (parse event log) --- */
    tpm_init();

    /* --- Boot integrity verification (PCR golden value check) --- */
    tpm_integrity_init();

    /* --- Physical memory manager: BOOT_FATAL if fails --- */
    pmm_init();
    kernel_subsystem_set_ready(SUBSYS_PMM, true);
    boot_progress(0, "PMM", POSTCODE_PMM_INIT);
    HV_BAR(15); /* sanity check: bars still work on bare metal */

    /* --- Virtual memory manager: BOOT_FATAL if PMM not ready --- */
    if (!kernel_subsystem_ready(SUBSYS_PMM))
        boot_halt("PMM not ready -- cannot init VMM");
    vmm_init();
    kernel_subsystem_set_ready(SUBSYS_VMM, true);
    boot_progress(0, "VMM", POSTCODE_VMM_INIT);

    /* --- Kernel heap: BOOT_FATAL if VMM not ready --- */
    if (!kernel_subsystem_ready(SUBSYS_VMM))
        boot_halt("VMM not ready -- cannot init heap");
    heap_init();
    kernel_subsystem_set_ready(SUBSYS_HEAP, true);
    boot_progress(0, "HEAP", POSTCODE_HEAP_INIT);

    /* --- klog early init: ring buffer + serial only (no disk yet) --- */
    klog_early_init();
    kernel_subsystem_set_ready(SUBSYS_KLOG, true);
    boot_progress(0, "KLOG", POSTCODE_KLOG_INIT);

    /* --- CPUID: probe CPU features --- */
    cpuid_init();
    boot_progress(0, "CPUID", POSTCODE_CPUID_INIT);

    /* --- CPU security hardening: NX (SMEP/SMAP deferred until page tables fixed) --- */
    cpu_harden();

    /* --- Apply NX policy + clear User bit from kernel pages --- */
    vmm_apply_nx_policy();

    /* --- Now safe to enable SMEP/SMAP (kernel pages no longer User) --- */
    cpu_harden_post_pagetable();

    /* --- SIMD: enable AVX2 or fall back to SSE2 --- */
    simd_enable_avx();
    if (simd_avx2_ok)
        klog(LOG_INFO, "simd", "AVX2 enabled (8 pixels/iter)");
    else
        klog(LOG_WARN, "simd", "AVX2 not available, using SSE2 fallback");
    boot_progress(0, "SIMD", POSTCODE_SIMD_INIT);

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "[PHASE0] complete -- serial, memory, klog, CPUID, SIMD ready");
}

/* ---- Legacy wrapper (until main.c switches to boot_phase0) -------------- */

void boot_hw_init(uint64_t magic, uint64_t mbi)
{
    boot_phase0(magic, mbi);
}
