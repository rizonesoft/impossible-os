/* ============================================================================
 * elf_bootproto.c -- bootloader-side parser for the .bootproto ELF
 *                    section embedded in the kernel image.
 *
 * Runs BEFORE ExitBootServices. The bootloader invokes bootproto_find()
 * with a pointer to the loaded kernel ELF bytes (file_buf inside
 * load_kernel) and receives the 56-byte boot_proto_descriptor on
 * success, OR a specific BOOTPROTO_ERR_* code on failure.
 *
 * Hardening invariants (crafted-image defense):
 *   1. Every offset + size pair is checked via subtraction so a 64-bit
 *      addition cannot wrap around.
 *   2. sh_entsize is compared against sizeof(Elf64_Shdr) before any
 *      per-index arithmetic so a 0 or rogue value cannot advance past
 *      the buffer.
 *   3. The section name string table is bounds-checked twice: once for
 *      its header content, once for the per-section sh_name offset.
 *   4. The descriptor section is size-pinned (56 bytes) and alignment-
 *      pinned (8 bytes) so the copy-out is known-safe.
 *   5. On any error, *out_desc is zeroed so callers cannot consume
 *      stale stack bytes.
 * ============================================================================ */

#include "elf_bootproto.h"
#include "elf_types.h"

/* Minimal memset to avoid pulling in efi_memset (which may live in a
 * different TU). The parser runs at UEFI BootServices time; a
 * bootloader-local helper keeps the parser self-contained. */
static void bpp_memset(void *dst, UINT8 val, UINTN n)
{
    UINT8 *p = (UINT8 *)dst;
    UINTN i;
    for (i = 0; i < n; i++)
        p[i] = val;
}

static void bpp_memcpy(void *dst, const void *src, UINTN n)
{
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    UINTN i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
}

/* Compare a section-name string (NUL-terminated) against the literal
 * BOOT_PROTO_SECTION_NAME. bounded: stops at the earlier of NUL or
 * `max` so a non-terminated string cannot read past its own section.
 * Returns 1 on exact match (including NUL), 0 otherwise. */
static int bpp_section_name_matches(const char *candidate, UINTN max,
                                    const char *target)
{
    UINTN i = 0;
    while (i < max) {
        if (candidate[i] != target[i])
            return 0;
        if (target[i] == '\0')
            return 1;
        i++;
    }
    /* candidate ran past max without NUL: not a valid string, reject */
    return 0;
}

const char *bootproto_result_name(int result)
{
    switch (result) {
    case BOOTPROTO_OK:                return "OK";
    case BOOTPROTO_ERR_NULL_IMAGE:    return "NULL_IMAGE";
    case BOOTPROTO_ERR_EHDR_BOUNDS:   return "EHDR_BOUNDS";
    case BOOTPROTO_ERR_NOT_ELF64:     return "NOT_ELF64";
    case BOOTPROTO_ERR_SHT_BOUNDS:    return "SHT_BOUNDS";
    case BOOTPROTO_ERR_SHT_ENTSIZE:   return "SHT_ENTSIZE";
    case BOOTPROTO_ERR_SHSTR_IDX:     return "SHSTR_IDX";
    case BOOTPROTO_ERR_SHSTR_BOUNDS:  return "SHSTR_BOUNDS";
    case BOOTPROTO_ERR_NAME_BOUNDS:   return "NAME_BOUNDS";
    case BOOTPROTO_ERR_NOT_FOUND:     return "NOT_FOUND";
    case BOOTPROTO_ERR_SECT_BOUNDS:   return "SECT_BOUNDS";
    case BOOTPROTO_ERR_SECT_ALIGN:    return "SECT_ALIGN";
    case BOOTPROTO_ERR_SECT_SIZE:     return "SECT_SIZE";
    default:                          return "unknown";
    }
}

int bootproto_find(const UINT8 *elf_image, UINT64 image_size,
                   struct boot_proto_descriptor *out_desc)
{
    if (out_desc != (struct boot_proto_descriptor *)0)
        bpp_memset(out_desc, 0, sizeof(*out_desc));

    if (elf_image == (const UINT8 *)0 || image_size == 0 ||
        out_desc == (struct boot_proto_descriptor *)0)
        return BOOTPROTO_ERR_NULL_IMAGE;

    /* ELF header fits. */
    if (image_size < sizeof(Elf64_Ehdr))
        return BOOTPROTO_ERR_EHDR_BOUNDS;

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_image;

    /* Sanity: ELF magic + 64-bit + x86_64 machine. Mirrors the existing
     * load_kernel check in bootx64.c so a non-ELF buffer rejects here
     * with a distinct error instead of trying to read a bogus shoff. */
    if (ehdr->e_magic != ELF_MAGIC || ehdr->e_class != 2 ||
        ehdr->e_machine != 0x3E)
        return BOOTPROTO_ERR_NOT_ELF64;

    /* Section-header entry size must match our Elf64_Shdr layout.
     * A rogue 0 or over-large value would let a single index advance
     * past the image or alias into foreign bytes. */
    if (ehdr->e_shentsize != sizeof(Elf64_Shdr))
        return BOOTPROTO_ERR_SHT_ENTSIZE;

    /* Short-circuit: zero sections means no .bootproto. */
    if (ehdr->e_shnum == 0)
        return BOOTPROTO_ERR_NOT_FOUND;

    /* e_shoff must fit inside image_size. Subtraction-based to prevent
     * wraparound: compute the remaining bytes at e_shoff, then check
     * the total section-header table fits within that. */
    if (ehdr->e_shoff > image_size)
        return BOOTPROTO_ERR_SHT_BOUNDS;
    UINT64 sht_remainder = image_size - ehdr->e_shoff;
    /* ehdr->e_shnum is 16-bit; sizeof(Elf64_Shdr) is 64. Their product
     * fits in 32 bits (max 65535 * 64 = 4,194,240 bytes) so no 64-bit
     * overflow concern here, but keep the division-based check for
     * defense in depth. */
    if ((UINT64)ehdr->e_shnum > sht_remainder / sizeof(Elf64_Shdr))
        return BOOTPROTO_ERR_SHT_BOUNDS;

    /* Section-header string table index: must point at a valid section
     * that is NOT the null section (SHT_NULL at index 0 has no name
     * table anyway). */
    if (ehdr->e_shstrndx == 0 || ehdr->e_shstrndx >= ehdr->e_shnum)
        return BOOTPROTO_ERR_SHSTR_IDX;

    const Elf64_Shdr *shdr =
        (const Elf64_Shdr *)(elf_image + ehdr->e_shoff);

    /* Locate + bounds-check the section-header string table. */
    const Elf64_Shdr *shstr = &shdr[ehdr->e_shstrndx];
    if (shstr->sh_offset > image_size)
        return BOOTPROTO_ERR_SHSTR_BOUNDS;
    UINT64 shstr_remainder = image_size - shstr->sh_offset;
    if (shstr->sh_size > shstr_remainder)
        return BOOTPROTO_ERR_SHSTR_BOUNDS;
    const char *shstrtab = (const char *)(elf_image + shstr->sh_offset);
    UINT64 shstr_size = shstr->sh_size;

    /* Walk sections. Skip SHT_NULL (index 0 is always SHT_NULL per
     * spec; a well-formed file has no others). */
    UINT16 i;
    for (i = 1; i < ehdr->e_shnum; i++) {
        /* Name offset must fit in the string table. */
        if ((UINT64)shdr[i].sh_name >= shstr_size)
            return BOOTPROTO_ERR_NAME_BOUNDS;

        UINT64 name_max = shstr_size - (UINT64)shdr[i].sh_name;
        const char *name = shstrtab + shdr[i].sh_name;

        if (!bpp_section_name_matches(name, (UINTN)name_max,
                                      BOOT_PROTO_SECTION_NAME))
            continue;

        /* Found `.bootproto`. Validate its body before copying out. */
        if (shdr[i].sh_size != sizeof(struct boot_proto_descriptor))
            return BOOTPROTO_ERR_SECT_SIZE;

        /* 8-byte alignment: the kernel TU declares aligned(8). If the
         * linker violated the attribute (or the ELF is crafted), the
         * bootloader rejects rather than reading a misaligned struct
         * -- some SetVariable attribute paths downstream assume the
         * 8-byte alignment. */
        if ((shdr[i].sh_offset & 0x7u) != 0)
            return BOOTPROTO_ERR_SECT_ALIGN;

        /* Section data bounds check: subtraction-based, avoids 64-bit
         * wrap if sh_offset + sh_size would otherwise overflow. */
        if (shdr[i].sh_offset > image_size)
            return BOOTPROTO_ERR_SECT_BOUNDS;
        UINT64 sect_remainder = image_size - shdr[i].sh_offset;
        if (shdr[i].sh_size > sect_remainder)
            return BOOTPROTO_ERR_SECT_BOUNDS;

        bpp_memcpy(out_desc, elf_image + shdr[i].sh_offset,
                   sizeof(struct boot_proto_descriptor));
        return BOOTPROTO_OK;
    }

    return BOOTPROTO_ERR_NOT_FOUND;
}
