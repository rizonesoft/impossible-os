/* ============================================================================
 * boot_proto_mirror.h -- UEFI bootloader view of struct
 *                        boot_proto_descriptor.
 *
 * MIRROR of include/kernel/boot_proto_descriptor.h. Layout must match
 * byte-for-byte. Every field name, order, and total size stays aligned
 * with the kernel view; compile-time _Static_asserts at the bottom pin
 * offsets + size invariants.
 *
 * Why a mirror: the bootloader builds with its own `-I.` include path
 * and cannot include kernel-side headers that pull in kernel/types.h.
 * The mirror uses UINT8 / UINT32 typedefs from efi.h, which have the
 * same underlying representation as the kernel's uint8_t / uint32_t
 * (same `unsigned char` / `unsigned int`). The static asserts prove
 * layout equivalence at compile time.
 *
 * Magic + section name + flag mask mirror the kernel-side macros from
 * the same header. Keep values in sync manually; drift is caught by
 * the `.bootproto` content check at runtime (kernel writes
 * BOOT_PROTO_DESCRIPTOR_MAGIC; bootloader reads it; mismatch triggers
 * the pre-jump error screen).
 * ============================================================================ */

#ifndef SRC_BOOT_UEFI_BOOT_PROTO_MIRROR_H
#define SRC_BOOT_UEFI_BOOT_PROTO_MIRROR_H

#include "efi.h"

#define BOOT_PROTO_DESCRIPTOR_MAGIC   0x31445042u   /* "BPD1" */
#define BOOT_PROTO_FLAG_MASK_KNOWN    0x00000000u
#define BOOT_PROTO_SECTION_NAME       ".bootproto"

struct boot_proto_descriptor {
    UINT32 magic;
    UINT32 version;
    UINT32 struct_size;
    UINT8  sha256[32];
    UINT32 flags;
    UINT32 _reserved[2];
};

_Static_assert(sizeof(struct boot_proto_descriptor) == 56,
    "boot_proto_descriptor mirror layout pinned at 56 bytes");
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

#endif /* SRC_BOOT_UEFI_BOOT_PROTO_MIRROR_H */
