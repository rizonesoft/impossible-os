/* ============================================================================
 * kernel_main -- Kernel entry point
 *
 * Called from entry.asm in 64-bit Long Mode.
 * Dispatches to four sequential boot phases under src/kernel/main/.
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
    boot_phase0(magic, mbi);    /* Critical init: serial, PMM/VMM/heap, CPUID */
    boot_phase1();              /* Platform services: GDT, IDT, APIC, timer, display */
    boot_phase2();              /* System services: PCI, storage, VFS, registry, SMP */
    boot_phase3();              /* User platform: scheduler, desktop, compositor (never returns) */

    /* Unreachable */
    for (;;)
        __asm__ volatile ("hlt");
}
