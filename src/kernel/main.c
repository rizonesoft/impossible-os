/* ============================================================================
 * kernel_main — Kernel entry point
 *
 * Called from entry.asm in 64-bit Long Mode.
 * Dispatches to boot phase modules under src/kernel/main/.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "main/main_internal.h"

/* Kernel entry point
 *   magic = MULTIBOOT2_BOOTLOADER_MAGIC (0x36D76289) for GRUB
 *           or UEFI_BOOT_MAGIC (0x55454649) for our UEFI bootloader
 *   mbi   = Multiboot2 info pointer (GRUB) or boot_info pointer (UEFI)
 */
void kernel_main(uint64_t magic, uint64_t mbi)
{
    /* === Hyper-V debug: write directly to framebuffer BEFORE any call ===
     * boot_info is at physical 0x10000 (identity-mapped).
     * Draw a MAGENTA bar at row 60 = "kernel_main entered".
     * Gated on config.debug so it is invisible during normal boots. */
    {
        volatile struct boot_info *bi =
            (volatile struct boot_info *)(uintptr_t)0x10000;
        if (bi->config.debug && bi->fb_available && bi->fb.addr && bi->fb.width > 0) {
            volatile uint32_t *px = (volatile uint32_t *)bi->fb.addr;
            uint32_t pitch_px = bi->fb.pitch / 4;
            uint32_t r, c;
            for (r = 60; r < 70 && r < bi->fb.height; r++)
                for (c = 0; c < 100 && c < bi->fb.width; c++)
                    px[r * pitch_px + c] = 0x00FF00FF; /* MAGENTA */
        }
    }

    /* Phase 1: Hardware — serial, boot info, PMM/VMM/heap, SIMD, drivers */
    boot_hw_init(magic, mbi);

    /* Phase 2: Interrupts — GDT, IDT, PIC, PIT, framebuffer, PCI, NIC */
    boot_interrupts_init();

    /* Phase 3: Storage — VFS, partitions, ACPI/SMP, registry, services */
    boot_storage_init(magic);

    /* Phase 4: Boot tests — VFS, IXFS, scheduler, IPC, swap, mmap */
    boot_tests_run();

    /* Phase 5: Desktop — fonts, icons, cursors, WM, demo window */
    boot_desktop_init();

    /* Phase 6: Compositor — event loop (never returns) */
    compositor_run();

    /* halt (unreachable) */
    for (;;)
        __asm__ volatile ("hlt");
}
