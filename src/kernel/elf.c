/* ============================================================================
 * elf.c -- ELF64 binary loader
 *
 * Validates ELF headers and loads PT_LOAD segments into memory.
 * Used by exec() to run user programs from the filesystem.
 * ============================================================================ */

#include "kernel/elf.h"
#include "kernel/klog.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/user_range.h"

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
    /* Check magic (byte-by-byte to avoid unaligned type-pun) */
    if (hdr->e_ident[0] != 0x7F || hdr->e_ident[1] != 'E' ||
        hdr->e_ident[2] != 'L'  || hdr->e_ident[3] != 'F') {
        klog(LOG_DEBUG, "elf", "Bad magic");
        return 0;
    }

    /* Must be 64-bit */
    if (hdr->e_ident[4] != ELFCLASS64) {
        klog(LOG_DEBUG, "elf", "Not 64-bit (class %u)", (uint64_t)hdr->e_ident[4]);
        return 0;
    }

    /* Must be little-endian */
    if (hdr->e_ident[5] != ELFDATA2LSB) {
        klog(LOG_DEBUG, "elf", "Not little-endian");
        return 0;
    }

    /* Must be executable (ET_EXEC) or PIE (ET_DYN) */
    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN) {
        klog(LOG_DEBUG, "elf", "Not executable (type %u)", (uint64_t)hdr->e_type);
        return 0;
    }

    /* Must be x86-64 */
    if (hdr->e_machine != EM_X86_64) {
        klog(LOG_DEBUG, "elf", "Not x86-64 (machine %u)", (uint64_t)hdr->e_machine);
        return 0;
    }

    /* Program headers must fit in the file */
    if (hdr->e_phoff + (uint64_t)hdr->e_phnum * hdr->e_phentsize > size) {
        klog(LOG_DEBUG, "elf", "Program headers exceed file size");
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
    uint32_t seg_count = 0;
    uint64_t load_base = (uint64_t)-1;
    uint64_t load_end = 0;

    if (size < sizeof(struct elf64_header)) {
        klog(LOG_DEBUG, "elf", "File too small");
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
            klog(LOG_DEBUG, "elf", "Segment %u exceeds file size", (uint64_t)i);
            return result;
        }

        /* Validate segment stays within user address range (overflow-safe) */
        if (phdr->p_vaddr < USER_ELF_BASE ||
            phdr->p_memsz > USER_ELF_SIZE ||
            phdr->p_vaddr + phdr->p_memsz > USER_ELF_END) {
            klog(LOG_ERROR, "elf",
                 "Segment %u outside user range: 0x%x-0x%x (allowed 0x%x-0x%x)",
                 (uint64_t)i, phdr->p_vaddr,
                 phdr->p_vaddr + phdr->p_memsz,
                 (uint64_t)USER_ELF_BASE, (uint64_t)USER_ELF_END);
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

        /* Reserve these physical pages in the PMM so no later allocation
         * (wallpaper, framebuffer, fonts) can overwrite the loaded code.
         * Since we use identity mapping, vaddr == paddr. */
        pmm_mark_region_used((uintptr_t)phdr->p_vaddr, phdr->p_memsz);

        seg_count++;
        klog(LOG_DEBUG, "elf", "Loaded segment %u: vaddr=0x%x filesz=%u memsz=%u flags=%s%s%s",
             (uint64_t)seg_count, phdr->p_vaddr, phdr->p_filesz, phdr->p_memsz,
             (phdr->p_flags & PF_R) ? "R" : "-",
             (phdr->p_flags & PF_W) ? "W" : "-",
             (phdr->p_flags & PF_X) ? "X" : "-");
    }

    if (load_end == 0) {
        klog(LOG_DEBUG, "elf", "No PT_LOAD segments found");
        return result;
    }

    result.entry = hdr->e_entry;
    result.load_base = load_base;
    result.load_end = load_end;
    result.success = 1;

    klog(LOG_DEBUG, "elf", "Loaded %u PT_LOAD segments at 0x%x-0x%x, entry=0x%x%s",
         (uint64_t)seg_count, result.load_base, result.load_end, result.entry,
         (hdr->e_type == ET_DYN) ? " (PIE)" : "");

    return result;
}
