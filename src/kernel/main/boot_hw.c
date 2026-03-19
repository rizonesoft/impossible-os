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
#include "main/main_internal.h"

/* External: Multiboot2 parser */
extern void multiboot2_parse(uintptr_t mbi_addr);

/* UEFI boot magic */
#define UEFI_BOOT_MAGIC 0x55454649ULL  /* "UEFI" */

void boot_hw_init(uint64_t magic, uint64_t mbi)
{
    uint32_t i;

    /* Step 1: Initialize serial (always works, even without display) */
    serial_init();
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

    /* Step 4: Initialize physical memory manager */
    pmm_init();

    /* Step 5: Initialize virtual memory manager */
    vmm_init();

    /* Step 6: Initialize kernel heap */
    heap_init();

    /* Step 6a: Probe CPU features via CPUID */
    cpuid_init();

    /* Step 6b: Enable AVX2 SIMD if supported */
    simd_enable_avx();
    if (simd_avx2_ok)
        klog(LOG_INFO, "simd", "AVX2 enabled (8 pixels/iter)");
    else
        klog(LOG_WARN, "simd", "AVX2 not available, using SSE2 fallback");

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Hardware ---------------------------------------------------------------");

    /* Step 7: Initialize disk drivers */
    ata_init();
    virtio_blk_init();
    ahci_init();

    /* Step 7d: Register block devices */
    blkdev_register_all();
}
