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
    /* Phase 0: Critical init — serial, boot info, PMM/VMM/heap, CPUID, SIMD */
    boot_phase0(magic, mbi);

    /* Phase 1: Platform services -- GDT, IDT, ACPI, APIC, timer, display */
    boot_phase1();

    /* Phase 2: System services -- PCI, storage, VFS, registry, SMP, network */
    boot_phase2();

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
