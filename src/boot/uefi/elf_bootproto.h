/* ============================================================================
 * elf_bootproto.h -- bootloader-side parser for the .bootproto ELF
 *                    section embedded in the kernel image.
 *
 * Runs PRE-ExitBootServices. Takes a pointer to the loaded kernel ELF
 * bytes (file_buf), walks the ELF section header table + section string
 * table, locates the section named ".bootproto", bounds-checks every
 * step, and copies the 56-byte boot_proto_descriptor into the caller's
 * buffer. All integer arithmetic is subtraction-based so a crafted
 * image with extreme offsets cannot wrap around.
 *
 * Owner: the bootloader pre-jump ABI mismatch screen feature (tracked in
 * todo/01-boot-platform/).
 *
 * Bootloader uses UINT8/UINT32/UINT64 typedefs from efi.h which are
 * byte-compatible with uint8_t/uint32_t/uint64_t. The parser returns a
 * plain int enum -- no UEFI types -- so the same code could be linked
 * into a host-side unit test without an EFI shim.
 * ============================================================================ */

#ifndef SRC_BOOT_UEFI_ELF_BOOTPROTO_H
#define SRC_BOOT_UEFI_ELF_BOOTPROTO_H

#include "efi.h"
#include "boot_proto_mirror.h"

enum bootproto_result {
    BOOTPROTO_OK              = 0,
    BOOTPROTO_ERR_NULL_IMAGE  = 1,   /* image ptr NULL or size 0 */
    BOOTPROTO_ERR_EHDR_BOUNDS = 2,   /* image_size < sizeof(Elf64_Ehdr) */
    BOOTPROTO_ERR_NOT_ELF64   = 3,   /* e_magic/e_class/e_machine mismatch */
    BOOTPROTO_ERR_SHT_BOUNDS  = 4,   /* e_shoff + e_shnum*entsize > size */
    BOOTPROTO_ERR_SHT_ENTSIZE = 5,   /* e_shentsize != sizeof(Elf64_Shdr) */
    BOOTPROTO_ERR_SHSTR_IDX   = 6,   /* e_shstrndx >= e_shnum or == 0 */
    BOOTPROTO_ERR_SHSTR_BOUNDS = 7,  /* shstrtab content past EOF */
    BOOTPROTO_ERR_NAME_BOUNDS = 8,   /* sh_name offset past shstrtab */
    BOOTPROTO_ERR_NOT_FOUND   = 9,   /* no `.bootproto` section */
    BOOTPROTO_ERR_SECT_BOUNDS = 10,  /* .bootproto data past EOF */
    BOOTPROTO_ERR_SECT_ALIGN  = 11,  /* .bootproto not 8-byte aligned */
    BOOTPROTO_ERR_SECT_SIZE   = 12,  /* .bootproto size != 56 bytes */
};

/* Locate and copy out the `.bootproto` descriptor.
 *
 * Returns BOOTPROTO_OK on success and writes 56 bytes to *out_desc.
 * Returns a BOOTPROTO_ERR_* value on any failure; *out_desc is zeroed
 * on failure so callers cannot accidentally consume stale stack bytes.
 *
 * Parameters:
 *   elf_image   -- pointer to the loaded kernel ELF bytes (file_buf
 *                  inside load_kernel()); must be non-NULL.
 *   image_size  -- size in bytes of the buffer at elf_image; used for
 *                  every bounds check.
 *   out_desc    -- caller-owned buffer for the 56-byte descriptor.
 *
 * Thread-safety: pure function on its inputs; no globals touched.
 *
 * The parser accepts the kernel ELF format emitted by
 * src/boot/linker.ld + clang --target=x86_64-elf. If the toolchain
 * changes and sh_entsize no longer matches sizeof(Elf64_Shdr), the
 * BOOTPROTO_ERR_SHT_ENTSIZE result surfaces that explicitly rather
 * than silently misparsing.
 */
int bootproto_find(const UINT8 *elf_image, UINT64 image_size,
                   struct boot_proto_descriptor *out_desc);

/* Stringify a BOOTPROTO_ERR_* enum value for diagnostics. Returns a
 * constant string; never NULL. Unknown values return "unknown". */
const char *bootproto_result_name(int result);

#endif /* SRC_BOOT_UEFI_ELF_BOOTPROTO_H */
