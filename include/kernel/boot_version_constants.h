/* ============================================================================
 * boot_version_constants.h -- UEFI-safe shared constants for the
 * boot_version_fault NVRAM schema.
 *
 * The bootloader (src/boot/uefi/bootx64.c) cannot include
 * include/kernel/boot_version.h because that header pulls kernel-only
 * types (kernel/types.h, kernel/boot_init.h). This sub-header carries
 * just the numeric constants both halves need so the producer
 * (bootloader bpp_persist_nvram_fault + bpp_render_rollback_and_halt)
 * and consumer (kernel boot_version_classify + transcribe) cannot
 * silently drift.
 *
 * The kernel header re-uses these macros to define its enum so any
 * renumber on either side fails the build via _Static_assert in
 * src/boot/uefi/bootx64.c.
 * ============================================================================ */

#ifndef KERNEL_BOOT_VERSION_CONSTANTS_H
#define KERNEL_BOOT_VERSION_CONSTANTS_H

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
