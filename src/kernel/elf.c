/* ============================================================================
 * elf.c — ELF64 binary loader
 *
 * Validates ELF headers and loads PT_LOAD segments into memory.
 * Used by exec() to run user programs from the initrd.
 * ============================================================================ */

#include "elf.h"
#include "printk.h"
#include "heap.h"

/* Simple memcpy for loading segments */
static void elf_memcpy(uint8_t *dst, const uint8_t *src, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

/* Zero memory (for BSS portion of segments) */
static void elf_memzero(uint8_t *dst, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++)
        dst[i] = 0;
}

/* Validate an ELF64 header */
static int elf_validate(const struct elf64_header *hdr, uint64_t size)
{
    /* Check magic */
    if (*(uint32_t *)hdr->e_ident != ELF_MAGIC) {
        printk("[ELF] Bad magic\n");
        return 0;
    }

    /* Must be 64-bit */
    if (hdr->e_ident[4] != ELFCLASS64) {
        printk("[ELF] Not 64-bit (class %u)\n", (uint64_t)hdr->e_ident[4]);
        return 0;
    }

    /* Must be little-endian */
    if (hdr->e_ident[5] != ELFDATA2LSB) {
        printk("[ELF] Not little-endian\n");
        return 0;
    }

    /* Must be executable */
    if (hdr->e_type != ET_EXEC) {
        printk("[ELF] Not executable (type %u)\n", (uint64_t)hdr->e_type);
        return 0;
    }

    /* Must be x86-64 */
    if (hdr->e_machine != EM_X86_64) {
        printk("[ELF] Not x86-64 (machine %u)\n", (uint64_t)hdr->e_machine);
        return 0;
    }

    /* Program headers must fit in the file */
    if (hdr->e_phoff + (uint64_t)hdr->e_phnum * hdr->e_phentsize > size) {
        printk("[ELF] Program headers exceed file size\n");
        return 0;
    }

    return 1;
}

struct elf_load_result elf_load(const uint8_t *data, uint64_t size)
{
    struct elf_load_result result = { 0, 0, 0, 0 };
    const struct elf64_header *hdr;
    const struct elf64_phdr *phdr;
    uint16_t i;
    uint64_t load_base = (uint64_t)-1;
    uint64_t load_end = 0;

    if (size < sizeof(struct elf64_header)) {
        printk("[ELF] File too small\n");
        return result;
    }

    hdr = (const struct elf64_header *)data;

    if (!elf_validate(hdr, size))
        return result;

    /* Iterate program headers and load PT_LOAD segments */
    for (i = 0; i < hdr->e_phnum; i++) {
        phdr = (const struct elf64_phdr *)(data + hdr->e_phoff +
                                            (uint64_t)i * hdr->e_phentsize);

        if (phdr->p_type != PT_LOAD)
            continue;

        /* Validate segment fits in file */
        if (phdr->p_offset + phdr->p_filesz > size) {
            printk("[ELF] Segment %u exceeds file size\n", (uint64_t)i);
            return result;
        }

        /* Copy file data to the target virtual address.
         * Since we use identity mapping, vaddr == paddr. */
        if (phdr->p_filesz > 0) {
            elf_memcpy((uint8_t *)phdr->p_vaddr,
                       data + phdr->p_offset,
                       phdr->p_filesz);
        }

        /* Zero BSS (memsz > filesz) */
        if (phdr->p_memsz > phdr->p_filesz) {
            elf_memzero((uint8_t *)(phdr->p_vaddr + phdr->p_filesz),
                        phdr->p_memsz - phdr->p_filesz);
        }

        /* Track loaded region */
        if (phdr->p_vaddr < load_base)
            load_base = phdr->p_vaddr;
        if (phdr->p_vaddr + phdr->p_memsz > load_end)
            load_end = phdr->p_vaddr + phdr->p_memsz;

        printk("[ELF] Loaded segment: vaddr=%p filesz=%u memsz=%u\n",
               phdr->p_vaddr, phdr->p_filesz, phdr->p_memsz);
    }

    if (load_end == 0) {
        printk("[ELF] No PT_LOAD segments found\n");
        return result;
    }

    result.entry = hdr->e_entry;
    result.load_base = load_base;
    result.load_end = load_end;
    result.success = 1;

    printk("[ELF] Entry point: %p (loaded %p - %p)\n",
           result.entry, result.load_base, result.load_end);

    return result;
}
