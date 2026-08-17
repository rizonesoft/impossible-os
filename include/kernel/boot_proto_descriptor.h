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

/* Length of the ABI-manifest digest (SHA-256), pinned against the descriptor
 * field below so the two can never drift apart. */
#define BOOT_PROTO_ABI_DIGEST_LEN  32

_Static_assert(sizeof(((struct boot_proto_descriptor *)0)->sha256) ==
                   BOOT_PROTO_ABI_DIGEST_LEN,
    "BOOT_PROTO_ABI_DIGEST_LEN must match boot_proto_descriptor.sha256");

/* The kernel's own descriptor instance, emitted into `.bootproto` by
 * src/kernel/main/boot_proto.c. Declared here so kernel code can read the
 * build-time ABI identity without including the generated
 * build/boot_proto_sha.h -- only that one TU should depend on the generator's
 * output shape. */
extern const struct boot_proto_descriptor kernel_boot_proto;

/* Copy this kernel's build-time ABI-manifest digest (the SHA-256 of
 * build/boot-info-abi.kernel.json) into `out`, which must have room for
 * BOOT_PROTO_ABI_DIGEST_LEN bytes.
 *
 * Returns 1 and fills the digest when the descriptor is intact. Returns 0 and
 * leaves `out` ZEROED when it is not: wrong magic, or an all-zero digest.
 *
 * FAIL-CLOSED CONTRACT -- a 0 return is NOT "this kernel has no ABI identity".
 * The digest is a compile-time constant and the build cannot link without it
 * (gen-proto-sha-header.sh exits non-zero when the manifest is missing), so the
 * only way to observe 0 is corruption of read-only kernel data or a broken
 * build invariant. Callers must treat it as a hard failure, never as an
 * optional field being absent. tpm_baseline_snapshot() aborts the entire
 * snapshot on 0 rather than enrolling a baseline with the identity silently
 * dropped, which would let a corrupted kernel enroll an unbound baseline. */
int boot_proto_abi_digest(uint8_t out[BOOT_PROTO_ABI_DIGEST_LEN]);

/* Pure form of the above over a CALLER-SUPPLIED descriptor. Same contract and
 * same return values; boot_proto_abi_digest() is exactly this applied to
 * kernel_boot_proto.
 *
 * This seam exists so the fail-closed branches stay testable. kernel_boot_proto
 * is const and linked into the image, so a test cannot corrupt it, and without
 * this entry point the bad-magic and all-zero paths -- which ARE the safety
 * property this accessor exists to provide -- could never be exercised. */
int boot_proto_abi_digest_from(const struct boot_proto_descriptor *desc,
                               uint8_t out[BOOT_PROTO_ABI_DIGEST_LEN]);

/* Expected section name in the kernel ELF. The bootloader's ELF parser
 * walks `Elf64_Shdr[]` + the section-header string table looking for
 * this literal. Must match the `__attribute__((section(".bootproto")))`
 * attribute in src/kernel/main/boot_proto.c and the `.bootproto` output
 * section in src/boot/linker.ld. */
#define BOOT_PROTO_SECTION_NAME  ".bootproto"

#endif /* KERNEL_BOOT_PROTO_DESCRIPTOR_H */
