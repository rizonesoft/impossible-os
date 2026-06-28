/* ============================================================================
 * kernel_main -- Kernel entry point
 *
 * Called from entry.asm in 64-bit Long Mode.
 * Dispatches to four sequential boot phases under src/kernel/main/.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/panic.h"          /* panic_evidence_restore_early (section 5) */
#include "kernel/security/stack_canary.h"  /* canary_init -- seed the GS cookie */
#include "main/main_internal.h"

/* Kernel entry point
 *   magic = UEFI_BOOT_MAGIC (0x55454649, ASCII "UEFI") -- the only
 *           accepted bootloader magic. Alternate boot protocols
 *           (Multiboot2, GRUB, Limine, legacy BIOS, EFI stub direct
 *           boot, kexec) are explicit non-goals per the alt-boot
 *           policy doc (docs/boot/alt-boot.md).
 *   mbi   = boot_info pointer at BOOT_INFO_PHYS_ADDR (0x10000)
 */
void kernel_main(uint64_t magic, uint64_t mbi)
{
    boot_phase0(magic, mbi);    /* Critical init: serial, PMM/VMM/heap, CPUID */
    /* Restore any cross-boot panic evidence now that PMM has reserved low
     * memory (0x80000 out of the allocator) and klog is up. Phase-0 boundary;
     * the record lands in kernel-side static storage, X:\Crash emission is
     * deferred to desktop-ready. */
    panic_evidence_restore_early();
    /* Seed the stack-protector cookie now: Phase 0 has run CPUID (RDRAND
     * usable) and the only frames live across this write are kernel_main
     * (never returns) and canary_init (no_stack_protector), so no epilogue
     * compares a stale cookie. Everything from boot_phase1 on is protected
     * with the real cookie. */
    canary_init();
    boot_phase1();              /* Platform services: GDT, IDT, APIC, timer, display */
    boot_phase2();              /* System services: PCI, storage, VFS, registry, SMP */
    boot_phase3();              /* User platform: scheduler, desktop, compositor (never returns) */

    /* Unreachable */
    for (;;)
        __asm__ volatile ("hlt");
}
