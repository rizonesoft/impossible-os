/* ============================================================================
 * boot_proto.c -- emit the kernel's boot_proto_descriptor into a
 *                 dedicated `.bootproto` ELF output section.
 *
 * One TU, no runtime code. The bootloader walks the loaded kernel ELF
 * BEFORE ExitBootServices, finds this section by name, reads the 56
 * bytes, and compares the 4-tuple { magic, version, struct_size,
 * sha256 } against its own compile-time expected values. A mismatch
 * triggers a pre-jump UCS-2 error on the UEFI console, persists a
 * boot_version_fault NVRAM record, stalls 10 seconds, and resets.
 *
 * The section name `.bootproto` matches the output section declared in
 * src/boot/linker.ld and the literal scanned by the bootloader parser
 * in src/boot/uefi/elf_bootproto.c.
 *
 * `used` attribute: prevent LTO / dead-code elimination from dropping
 * the descriptor. `aligned(8)`: bootloader parser asserts 8-byte
 * alignment. `section(".bootproto")`: forces the symbol into the
 * dedicated section so the linker script can place it in its own
 * PT_LOAD segment (the segment is placed inside .rodata's page so no
 * separate PT_LOAD is required today).
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/boot_proto_descriptor.h"
#include "boot_proto_sha.h"   /* from $(BUILD_DIR); provides KERNEL_ABI_SHA256 */

const struct boot_proto_descriptor kernel_boot_proto
    __attribute__((section(".bootproto"), aligned(8), used)) = {
    .magic       = BOOT_PROTO_DESCRIPTOR_MAGIC,
    .version     = (uint32_t)BOOT_INFO_VERSION,
    .struct_size = (uint32_t)sizeof(struct boot_info),
    .sha256      = KERNEL_ABI_SHA256,
    .flags       = 0u,
    ._reserved   = { 0u, 0u },
};
