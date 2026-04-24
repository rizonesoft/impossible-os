/* ============================================================================
 * boot_proto_descriptor.h -- kernel / bootloader ABI handshake descriptor.
 *
 * The kernel embeds one const instance of `struct boot_proto_descriptor`
 * in a dedicated ELF output section named `.bootproto`. The bootloader
 * walks the loaded kernel image BEFORE ExitBootServices, finds that
 * section, reads the 56 bytes, and compares the 4-tuple
 *   { magic, version, struct_size, sha256 }
 * against its own compile-time expected values.
 *
 * Mismatch -> pre-EBS UCS-2 error on gST->ConOut, persisted
 * boot_version_fault record in NVRAM, 10s stall, ResetSystem. Match ->
 * proceed to ExitBootServices + kernel jump as usual.
 *
 * Canonical doc: the bootloader pre-jump ABI mismatch screen feature is
 * tracked in the boot-protocol handoff roadmap (see
 * docs/boot/boot-protocol.md and todo/01-boot-platform/).
 *
 * The kernel-side declaration lives in src/kernel/main/boot_proto.c.
 * The bootloader-side parser lives in src/boot/uefi/elf_bootproto.c.
 * The sha256 bytes come from build/boot_proto_sha.h (auto-generated from
 * build/boot-info-abi.kernel.json).
 * ============================================================================ */

#ifndef KERNEL_BOOT_PROTO_DESCRIPTOR_H
#define KERNEL_BOOT_PROTO_DESCRIPTOR_H

#include "kernel/types.h"

/* Magic tag stored in the descriptor. Separate from BOOT_INFO_MAGIC so a
 * corrupted boot_info handoff cannot be mistaken for a valid
 * .bootproto record. ASCII "BPD1" (Boot Proto Descriptor v1). */
#define BOOT_PROTO_DESCRIPTOR_MAGIC   0x31445042u

/* Reserved flag bits. bits 0-15 are descriptor-schema flags (how the
 * record itself should be interpreted). bits 16-31 are boot-policy
 * flags (how the bootloader should treat a degraded handshake). All
 * zero today; grows via forward-compat additions guarded by the
 * BOOT_PROTO_FLAG_MASK_KNOWN closed mask on the validator. */
#define BOOT_PROTO_FLAG_MASK_KNOWN    0x00000000u

struct boot_proto_descriptor {
    uint32_t magic;          /* BOOT_PROTO_DESCRIPTOR_MAGIC */
    uint32_t version;        /* mirrors BOOT_INFO_VERSION; 16 bits would
                              * work today but a 32-bit field lets the
                              * ABI grow past 65535 without a schema bump */
    uint32_t struct_size;    /* sizeof(struct boot_info) as seen by the
                              * kernel at build time */
    uint8_t  sha256[32];     /* SHA-256 of build/boot-info-abi.kernel.json */
    uint32_t flags;          /* BOOT_PROTO_FLAG_* */
    uint32_t _reserved[2];   /* zero-filled; grows via forward-compat */
};

_Static_assert(sizeof(struct boot_proto_descriptor) == 56,
    "boot_proto_descriptor layout pinned at 56 bytes");
_Static_assert(__builtin_offsetof(struct boot_proto_descriptor, magic)       == 0,
    "boot_proto_descriptor.magic at offset 0");
_Static_assert(__builtin_offsetof(struct boot_proto_descriptor, version)     == 4,
    "boot_proto_descriptor.version at offset 4");
_Static_assert(__builtin_offsetof(struct boot_proto_descriptor, struct_size) == 8,
    "boot_proto_descriptor.struct_size at offset 8");
_Static_assert(__builtin_offsetof(struct boot_proto_descriptor, sha256)      == 12,
    "boot_proto_descriptor.sha256 at offset 12");
_Static_assert(__builtin_offsetof(struct boot_proto_descriptor, flags)       == 44,
    "boot_proto_descriptor.flags at offset 44");
_Static_assert(__builtin_offsetof(struct boot_proto_descriptor, _reserved)   == 48,
    "boot_proto_descriptor._reserved at offset 48");

/* Expected section name in the kernel ELF. The bootloader's ELF parser
 * walks `Elf64_Shdr[]` + the section-header string table looking for
 * this literal. Must match the `__attribute__((section(".bootproto")))`
 * attribute in src/kernel/main/boot_proto.c and the `.bootproto` output
 * section in src/boot/linker.ld. */
#define BOOT_PROTO_SECTION_NAME  ".bootproto"

#endif /* KERNEL_BOOT_PROTO_DESCRIPTOR_H */
