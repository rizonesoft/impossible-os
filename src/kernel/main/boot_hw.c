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
#include "kernel/printk.h"
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
#include "main/main_internal.h"

/* External: Multiboot2 parser */
extern void multiboot2_parse(uintptr_t mbi_addr);

/* UEFI boot magic */
#define UEFI_BOOT_MAGIC 0x55454649ULL  /* "UEFI" */

void boot_hw_init(uint64_t magic, uint64_t mbi)
{
    uint32_t i;

    /* Hyper-V debug: framebuffer progress bars inside boot_hw_init.
     * Read FB info from boot_info at 0x10000 (identity-mapped). */
    volatile struct boot_info *_bi =
        (volatile struct boot_info *)(uintptr_t)0x10000;
    volatile uint32_t *_fb = (volatile uint32_t *)_bi->fb.addr;
    uint32_t _pitch = _bi->fb.pitch / 4;
    uint32_t _fbw = _bi->fb.width;
    uint32_t _fbh = _bi->fb.height;

#define HV_BAR(row, color) do { \
    if (_fb && _fbw > 0) { \
        uint32_t _r, _c; \
        for (_r = (row); _r < (row) + 8 && _r < _fbh; _r++) \
            for (_c = 0; _c < 100 && _c < _fbw; _c++) \
                _fb[_r * _pitch + _c] = (color); \
    } \
} while (0)

    /* Step 1: Initialize serial */
    serial_init();
    HV_BAR(72, 0x0000FF00);   /* Row 72: GREEN = serial_init OK */

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
        serial_write("[UEFI] Boot info received from Impossible OS bootloader\n");
    } else if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        multiboot2_parse((uintptr_t)mbi);
    } else {
        serial_write("[FAIL] Unknown bootloader magic!\n");
        for (;;) __asm__ volatile ("hlt");
    }
    HV_BAR(84, 0x0000FFFF);   /* Row 84: CYAN = boot info parsed */

    /* Log boot.conf values */
    printk("[CONF] boot.conf: debug=%d verbose=%d serial=%d mode=%d splash=%ds heartbeat=%d postcode=%d%s\n",
           g_boot_info.config.debug, g_boot_info.config.verbose,
           g_boot_info.config.serial_debug, g_boot_info.config.boot_mode,
           g_boot_info.config.splash_timeout, g_boot_info.config.heartbeat,
           g_boot_info.config.postcode,
           g_boot_info.config.config_found ? "" : " (defaults)");

    /* Step 3: UEFI config table walker (log discovered tables) */
    uefi_config_init();

    /* Step 3b: UEFI runtime services (SetVirtualAddressMap + RT props) */
    uefi_runtime_init();

    /* Step 3c: UEFI conformance profile (Full UEFI vs EBBR) */
    uefi_conformance_init();

    /* Step 3d: TPM measured boot (parse event log) */
    tpm_init();

    /* Step 3e: ESRT firmware inventory */
    esrt_init();

    /* Step 3f: Boot timing report (TSC + FPDT) */
    boot_timing_init();

    /* Step 4: Initialize physical memory manager */
    pmm_init();
    HV_BAR(96, 0x00FFFF00);   /* Row 96: YELLOW = PMM OK */

    /* Step 5: Initialize virtual memory manager */
    vmm_init();
    HV_BAR(108, 0x000000FF);  /* Row 108: BLUE = VMM OK */

    /* Step 6: Initialize kernel heap */
    heap_init();
    HV_BAR(120, 0x00FF8000);  /* Row 120: ORANGE = heap OK */

    /* Step 6a: Probe CPU features via CPUID */
    cpuid_init();

    /* Step 6b: Enable AVX2 SIMD if supported */
    simd_enable_avx();
    if (simd_avx2_ok)
        klog(LOG_INFO, "simd", "AVX2 enabled (8 pixels/iter)");
    else
        klog(LOG_WARN, "simd", "AVX2 not available, using SSE2 fallback");
    HV_BAR(132, 0x00FFFFFF);  /* Row 132: WHITE = CPUID+SIMD OK */

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Hardware ---------------------------------------------------------------");

    /* Step 7: Initialize disk drivers */
    ata_init();
    virtio_blk_init();
    ahci_init();

    /* Step 7d: Register block devices */
    blkdev_register_all();
    HV_BAR(144, 0x00FF00FF);  /* Row 144: MAGENTA = boot_hw complete */

#undef HV_BAR
}

