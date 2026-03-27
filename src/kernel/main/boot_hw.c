/* ============================================================================
 * boot_hw.c — Hardware initialization phase
 *
 * Serial, boot info parsing, PMM/VMM/heap, CPUID, SIMD, disk driver init,
 * block device registration.
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
#include "kernel/drivers/ata.h"
#include "kernel/drivers/virtio_blk.h"
#include "kernel/drivers/ahci.h"
#include "kernel/version.h"
#include "kernel/boot_splash.h"
#include "kernel/uefi_config.h"
#include "kernel/uefi_runtime.h"
#include "kernel/tpm.h"
#include "kernel/boot_timing.h"
#include "kernel/boot_init.h"
#include "kernel/smbios.h"
#include "main/main_internal.h"

/* External: Multiboot2 parser */
extern void multiboot2_parse(uintptr_t mbi_addr);

/* UEFI boot magic */
#define UEFI_BOOT_MAGIC 0x55454649ULL  /* "UEFI" */

void boot_hw_init(uint64_t magic, uint64_t mbi)
{
    uint32_t i;

    /* Step 1: Initialize serial */
    serial_init();
    boot_progress(0, "SERIAL", POSTCODE_SERIAL_INIT);

    klog(LOG_DEBUG, "", "========================================================================");
    klog(LOG_DEBUG, "", "  Impossible OS -- Boot Log");
    klog(LOG_DEBUG, "", "========================================================================");
    version_print();

    /* Step 2: Parse boot info based on bootloader type */
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
        klog(LOG_FATAL, "boot", "Unknown bootloader magic!");
        for (;;) __asm__ volatile ("hlt");
    }
    boot_progress(0, "BOOT_INFO", POSTCODE_BOOT_CFG);

    /* Log boot.conf values */
    klog(LOG_INFO, "CONF", "boot.conf: debug=%d verbose=%d serial=%d mode=%d splash=%ds heartbeat=%d postcode=%d%s",
           g_boot_info.config.debug, g_boot_info.config.verbose,
           g_boot_info.config.serial_debug, g_boot_info.config.boot_mode,
           g_boot_info.config.splash_timeout, g_boot_info.config.heartbeat,
           g_boot_info.config.postcode,
           g_boot_info.config.config_found ? "" : " (defaults)");

    /* Step 3: UEFI config table walker (log discovered tables) */
    uefi_config_init();

    /* Step 3b: UEFI runtime services (SetVirtualAddressMap + RT props) */
    uefi_runtime_init();

    /* Step 3b2: UEFI variable services (enumerate NVRAM) */
    uefi_vars_init();

    /* Step 3b3: UEFI RTC time (seed wall clock) */
    uefi_time_init();

    /* Step 3b4: Secure Boot state detection */
    uefi_secureboot_init();

    /* Step 3b5: Secure Boot key enumeration (db/dbx/dbt) */
    secureboot_keys_init();

    /* Step 3b6: Crypto agility (UEFI 2.10 algorithm negotiation) */
    uefi_crypto_agility_init();

    /* Step 3b7: Capsule firmware update capabilities (query-only) */
    uefi_capsule_init();

    /* Step 3c: UEFI conformance profile (Full UEFI vs EBBR) */
    uefi_conformance_init();

    /* Step 3d: TPM measured boot (parse event log) */
    tpm_init();

    /* Step 3d2: Boot integrity verification (PCR golden value check) */
    tpm_integrity_init();

    /* Step 3e: ESRT firmware inventory */
    esrt_init();

    /* Step 3f: Memory Attributes Table (W^X) */
    mat_init();

    /* Step 3g: SMBIOS system information */
    smbios_init();

    /* Step 3h: GOP mode enumeration report */
    {
        static const char *pf_names[] = { "RGBX", "BGRX", "BitMask" };
        uint32_t mc = g_boot_info.gop_mode_count;
        uint32_t sel = g_boot_info.gop_mode_selected;
        if (mc > 0) {
            const char *pf = (g_boot_info.fb.pixel_format < 3) ?
                pf_names[g_boot_info.fb.pixel_format] : "Unknown";
            klog(LOG_INFO, "GOP",
                 "%ux%u %s (mode %u of %u available)",
                 g_boot_info.fb.width, g_boot_info.fb.height,
                 pf, sel, mc);
        }
    }

    /* Step 3i: Boot timing report (TSC + FPDT) */
    boot_timing_init();

    /* Step 4: Initialize physical memory manager */
    pmm_init();
    boot_progress(0, "PMM", POSTCODE_PMM_INIT);

    /* Step 5: Initialize virtual memory manager */
    vmm_init();
    boot_progress(0, "VMM", POSTCODE_VMM_INIT);

    /* Step 6: Initialize kernel heap */
    heap_init();
    boot_progress(0, "HEAP", POSTCODE_HEAP_INIT);

    /* Step 6a: Probe CPU features via CPUID */
    cpuid_init();

    /* Step 6b: Enable AVX2 SIMD if supported */
    simd_enable_avx();
    if (simd_avx2_ok)
        klog(LOG_INFO, "simd", "AVX2 enabled (8 pixels/iter)");
    else
        klog(LOG_WARN, "simd", "AVX2 not available, using SSE2 fallback");
    boot_progress(0, "CPUID_SIMD", POSTCODE_SIMD_INIT);

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Hardware ---------------------------------------------------------------");

    /* Step 7: Initialize disk drivers */
    ata_init();
    virtio_blk_init();
    ahci_init();

    /* Step 7d: Register block devices */
    blkdev_register_all();
    boot_progress(1, "STORAGE_DRV", POSTCODE_STORAGE_INIT);

}

