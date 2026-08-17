/* ============================================================================
 * boot_proto.c -- emit the kernel's boot_proto_descriptor into a
 *                 dedicated `.bootproto` ELF output section.
 *
 * One TU. It holds the descriptor itself plus the single pure accessor that
 * lets the rest of the kernel read the build-time ABI digest without taking a
 * dependency on the generated boot_proto_sha.h. The bootloader walks the
 * loaded kernel ELF
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

/* Read-only accessor for the build-time ABI-manifest digest.
 *
 * Pure: touches nothing but the immutable `.bootproto` record, which is const
 * for the kernel's lifetime, so this is safe from any CPU at any phase without
 * a lock. The magic and all-zero checks are a Layer-4 canary on read-only data
 * rather than input validation -- both are compile-time-guaranteed true in a
 * correctly built kernel, so observing either failure means the read-only
 * region has been corrupted. The contract in boot_proto_descriptor.h requires
 * callers to treat 0 as a hard failure, never as an absent optional field. */
int boot_proto_abi_digest_from(const struct boot_proto_descriptor *desc,
                               uint8_t out[BOOT_PROTO_ABI_DIGEST_LEN])
{
    uint32_t i;
    uint8_t  nonzero = 0u;

    if (!out)
        return 0;
    /* Zero FIRST, and on every failure path below, so a caller that prefilled
     * the buffer can never mistake stale bytes for a digest. */
    for (i = 0u; i < (uint32_t)BOOT_PROTO_ABI_DIGEST_LEN; i++)
        out[i] = 0u;

    if (!desc || desc->magic != BOOT_PROTO_DESCRIPTOR_MAGIC)
        return 0;

    /* A SHA-256 of any input is never all zero in practice, so an all-zero
     * field means the descriptor was never filled or has been wiped. Scan the
     * whole field with a bitwise OR rather than breaking early: this keeps the
     * helper free of input-dependent control flow. */
    for (i = 0u; i < (uint32_t)BOOT_PROTO_ABI_DIGEST_LEN; i++)
        nonzero |= desc->sha256[i];
    if (nonzero == 0u)
        return 0;

    for (i = 0u; i < (uint32_t)BOOT_PROTO_ABI_DIGEST_LEN; i++)
        out[i] = desc->sha256[i];
    return 1;
}

int boot_proto_abi_digest(uint8_t out[BOOT_PROTO_ABI_DIGEST_LEN])
{
    return boot_proto_abi_digest_from(&kernel_boot_proto, out);
}
