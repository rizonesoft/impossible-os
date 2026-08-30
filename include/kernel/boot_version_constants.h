/* ============================================================================
 * boot_version_constants.h -- UEFI-safe shared constants for the boot_info
 * handoff contract: where the struct lives, and the boot_version_fault
 * NVRAM schema that reports a bad one.
 *
 * The bootloader (src/boot/uefi/bootx64.c) cannot include
 * include/kernel/boot_version.h or include/kernel/boot_info.h because
 * those headers pull kernel-only types (kernel/types.h,
 * kernel/boot_init.h). This sub-header carries just the numeric
 * constants both halves need so the producer (bootloader
 * bpp_persist_nvram_fault + bpp_render_rollback_and_halt) and consumer
 * (kernel boot_version_classify + transcribe) cannot silently drift.
 *
 * The kernel header re-uses these macros to define its enum so any
 * renumber on either side fails the build via _Static_assert in
 * src/boot/uefi/bootx64.c.
 *
 * #defines plus _Static_assert only -- no typedefs, no includes -- so the
 * freestanding UEFI translation unit can consume it unchanged. The asserts
 * mean C11 or later is required, which every current consumer already has
 * (the UEFI TU builds -std=gnu11, src/boot/uefi/Makefile:17); an assembler
 * or non-C consumer would need the value hoisted, unlike the pure-macro
 * include/kernel/mm/memmap_boot.h cited below. Same one-definition
 * discipline as include/kernel/mm/memmap_boot.h, and strictly stronger
 * than the boot_info_mirror.h convention: tools/boot-info-manifest dumps
 * struct FIELDS, so it would never diff a macro that had drifted.
 *
 * WHY constants shared with the bootloader belong in THIS file rather
 * than a new one: the UEFI build has no compiler-generated dependencies
 * (src/boot/uefi/Makefile:15-18 carries no -MMD, while kernel objects get
 * -MMD -MP at Makefile:28), so bootx64.o rebuilds only from the explicit
 * prerequisite lists at src/boot/uefi/Makefile:40-46 and Makefile:412-434.
 * This header is named in BOTH. A brand-new header would be in neither,
 * so an incremental build would rebuild the kernel against a new value
 * while re-linking a BOOTX64.EFI still compiled against the old one --
 * exactly the loader/kernel divergence these constants exist to prevent.
 * ============================================================================ */

#ifndef KERNEL_BOOT_VERSION_CONSTANTS_H
#define KERNEL_BOOT_VERSION_CONSTANTS_H

/* Physical base of the boot_info handoff. The bootloader writes struct
 * boot_info here before jumping to the kernel; the kernel's PMM reserves
 * the region (src/kernel/mm/boot_reserved.c) and the payload validator
 * refuses any typed descriptor that overlaps it
 * (src/kernel/main/boot_payload.c). One definition, so the bootloader's
 * overlap check and the kernel's retained-region validators cannot
 * disagree about where the struct is.
 *
 * Deliberately NOT #ifndef-guarded, unlike BOOT_INFO_MAGIC / VERSION
 * beside it: those carry an override so the stale-ABI fixture harness can
 * build a mismatched loader on purpose. There is no such use for the
 * handoff base, and a guard would let one translation unit -D itself a
 * different address -- reintroducing per-TU divergence as a supported
 * shape. A redefinition here is a build error, which is the intent. */
#define BOOT_INFO_PHYS_ADDR               0x10000ULL

/* Page-aligned: the PMM reserves whole frames, so a base partway into a
 * page would retain a frame the reservation never named. */
_Static_assert(BOOT_INFO_PHYS_ADDR % 0x1000ULL == 0ULL,
    "BOOT_INFO_PHYS_ADDR must be 4 KiB page-aligned");
/* Lower bound for handoff pointers: anything below this is the NULL page,
 * the real-mode IVT, or the BDA -- never a legitimate boot_info location.
 * boot_info_validate_addr() (src/kernel/main/boot_info.c) enforces it at
 * runtime; the assert below pins the compiled base against the same value.
 *
 * Lives HERE rather than beside its validator for the same reason the
 * handoff base does: it had four uncoordinated copies -- the validator's
 * TU-local #define, the assert below, the operator-facing range in the
 * rejection message at src/kernel/main/boot_hw.c, and the boundary case in
 * test_boot_info.c. Raising the runtime floor while the assert kept the old
 * literal leaves the build green and halts Phase 0: a compile-time gate
 * certifying a base that its own validator rejects. */
#define BOOT_INFO_MIN_ADDR                0x1000ULL

/* Clear of the NULL page / BDA: a base below the validator's floor could
 * never pass the kernel's own check. Page alignment alone does not imply
 * this -- 0x0 is aligned and still invalid. */
_Static_assert(BOOT_INFO_PHYS_ADDR >= BOOT_INFO_MIN_ADDR,
    "BOOT_INFO_PHYS_ADDR must clear the NULL page (validate_addr floor)");

/* Physical page holding struct panic_evidence (include/kernel/panic.h)
 * across a reset. The bootloader pins it with AllocatePages(AllocateAddress)
 * before any other allocation it makes, so neither firmware nor BOOTX64 can
 * hand the page out and overwrite the previous boot's crash record between
 * the reset and the kernel's restore (TODO-14 sec14).
 *
 * Lives HERE for the same reason the handoff base does: the bootloader cannot
 * include panic.h (kernel-only types), so a loader-side literal would be a
 * second uncoordinated copy of an address the kernel already pins. panic.h
 * asserts its own PANIC_EVIDENCE_ADDR against this value, so a change on
 * either side fails the build instead of producing a loader that reserves a
 * page the kernel never reads. */
#define PANIC_EVIDENCE_PHYS_ADDR          0x80000ULL

/* Page-aligned: AllocateAddress allocates whole frames, and a base partway
 * into a page would pin a frame that does not start where the record does. */
_Static_assert(PANIC_EVIDENCE_PHYS_ADDR % 0x1000ULL == 0ULL,
    "PANIC_EVIDENCE_PHYS_ADDR must be 4 KiB page-aligned");
/* Clear of the NULL page / real-mode IVT / BDA, same floor the handoff base
 * clears -- a record below it could never be a legitimate allocation. */
_Static_assert(PANIC_EVIDENCE_PHYS_ADDR >= BOOT_INFO_MIN_ADDR,
    "PANIC_EVIDENCE_PHYS_ADDR must clear the NULL page");
/* Distinct from the handoff base: the two are pinned independently and a
 * collision would have the loader reserve one page for both purposes. */
_Static_assert(PANIC_EVIDENCE_PHYS_ADDR != BOOT_INFO_PHYS_ADDR,
    "PANIC_EVIDENCE_PHYS_ADDR must not collide with BOOT_INFO_PHYS_ADDR");
/* Inside the legacy first MiB, which the kernel PMM reserves wholesale
 * (src/kernel/mm/pmm.c). That blanket reservation is what protects the page
 * from kernel entry onward; the loader-side pin covers the window before it. */
_Static_assert(PANIC_EVIDENCE_PHYS_ADDR < 0x100000ULL,
    "PANIC_EVIDENCE_PHYS_ADDR must sit inside the PMM-reserved first MiB");

/* BVPF = "Boot Version Protocol Fault". */
#define BOOT_VERSION_FAULT_MAGIC          0x42565046u

#define BOOT_VERSION_FAULT_VAL_OK             0u
#define BOOT_VERSION_FAULT_VAL_NULL_HDR       1u
#define BOOT_VERSION_FAULT_VAL_BAD_MAGIC      2u
#define BOOT_VERSION_FAULT_VAL_BAD_VERSION    3u
#define BOOT_VERSION_FAULT_VAL_BAD_SIZE       4u
#define BOOT_VERSION_FAULT_VAL_SEC_ROLLBACK   5u
#define BOOT_VERSION_FAULT_VAL_BAD_SHA        6u
#define BOOT_VERSION_FAULT_VAL_BAD_PARSE      7u
#define BOOT_VERSION_FAULT_VAL_PT_LOAD_FORBIDDEN 8u

#endif /* KERNEL_BOOT_VERSION_CONSTANTS_H */
