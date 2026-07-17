/* ============================================================================
 * memmap_boot.h -- UEFI-safe single source for the HHDM address-space
 * constants the bootloader must mirror.
 *
 * PURE #defines (no kernel/types.h, no typedefs) so BOTH sides include this
 * ONE file:
 *   - include/kernel/mm/memmap.h  (the canonical kernel layout header)
 *   - src/boot/uefi/bootx64.c     (the freestanding UEFI bootloader)
 *
 * Because there is exactly one definition, the bootloader cannot drift from
 * the kernel the way a hand-copied literal can -- this is stronger than the
 * boot_info_mirror.h convention, which leaves cross-checking to the
 * boot-info-manifest tool. Any change here changes both consumers at once.
 *
 * Only the constants the bootloader genuinely needs live here; the rest of
 * the virtual layout (windows, translation helpers, containment asserts)
 * stays in memmap.h, which includes this file.
 * ============================================================================ */

#pragma once

/* HHDM: fixed-offset direct map of physical RAM. Linux-compatible base;
 * sparse RAM-only, NX + writable, never User (alias policy in the design doc). */
#define MM_HHDM_BASE          0xffff888000000000ULL  /* alias of physical 0 */
#define MM_HHDM_SIZE          0x0000400000000000ULL  /* 64 TiB of RAM */

/* Hard ceiling for the kernel PML4 physical frame: the AP trampoline loads
 * CR3 with a 32-bit `mov cr3, eax`, so the root must stay below 4 GiB. */
#define MM_PML4_PHYS_LIMIT    0x0000000100000000ULL  /* 4 GiB */

/* PML4 slot the HHDM base lands in ((BASE >> 39) & 0x1ff = 273); the 64 TiB
 * window occupies 128 slots (273..400 inclusive). The bootloader installs
 * leaves per slot; the section-9 walker uses these to find the right entry. */
#define MM_HHDM_PML4_SLOT       ((MM_HHDM_BASE >> 39) & 0x1ffULL)
#define MM_HHDM_PML4_SLOT_LAST  (MM_HHDM_PML4_SLOT + (MM_HHDM_SIZE >> 39) - 1ULL)

_Static_assert(MM_HHDM_PML4_SLOT == 273ULL,
    "HHDM base must live in PML4 slot 273");
_Static_assert(MM_HHDM_PML4_SLOT_LAST == 400ULL,
    "HHDM 64 TiB window must end at PML4 slot 400");
