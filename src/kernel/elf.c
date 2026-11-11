/* ============================================================================
 * elf.c -- ELF64 binary loader
 *
 * Copies the PT_LOAD segments of an image that elf_parse (elf_parse.c) has
 * accepted. Every decision made from file bytes lives there; this file only
 * moves them. Used by exec() to run user programs from the filesystem.
 * ============================================================================ */

#include "kernel/elf.h"
#include "kernel/klog.h"
#include "kernel/mm/pmm.h"

/* Segment copy/zero use the kernel scalar memcpy/memset (libc string.c).
 * This path runs at CPL=0 during load with no FPU-save context, so it must
 * stay scalar -- never route through the SIMD memops dispatchers. */
extern void *memcpy(void *dst, const void *src, uint64_t n);
extern void *memset(void *dst, int c, uint64_t n);

struct elf_load_result elf_load(const uint8_t *data, uint64_t size)
{
    struct elf_load_result result = elf_parse(data, size);
    struct elf64_header hdr_buf;
    const struct elf64_header *hdr = &hdr_buf;
    struct elf64_phdr ph_buf;
    const struct elf64_phdr *phdr;
    uint32_t seg_count;
    uint16_t i;

    if (!result.success)
        return result;

    __builtin_memcpy(&hdr_buf, data, sizeof(hdr_buf));

    /* --- Pass 2: mutate. All fatal validation passed in pass 1. --- */
    seg_count = 0;
    for (i = 0; i < hdr->e_phnum; i++) {
        elf_phdr_read(data, hdr, i, &ph_buf);
        phdr = &ph_buf;

        if (phdr->p_type != PT_LOAD)
            continue;

        /* Copy file data to the target virtual address.
         * Since we use identity mapping, vaddr == paddr. */
        if (phdr->p_filesz > 0) {
            memcpy((void *)phdr->p_vaddr,
                   data + phdr->p_offset,
                   phdr->p_filesz);
        }

        /* Zero BSS (memsz > filesz) */
        if (phdr->p_memsz > phdr->p_filesz) {
            memset((void *)(phdr->p_vaddr + phdr->p_filesz), 0,
                   phdr->p_memsz - phdr->p_filesz);
        }

        /* Reserve these physical pages in the PMM so no later allocation
         * (wallpaper, framebuffer, fonts) can overwrite the loaded code. */
        pmm_mark_region_used((uintptr_t)phdr->p_vaddr, phdr->p_memsz);

        seg_count++;
        klog(LOG_DEBUG, "elf", "Loaded segment %u: vaddr=0x%x filesz=%u memsz=%u flags=%s%s%s",
             (uint64_t)seg_count, phdr->p_vaddr, phdr->p_filesz, phdr->p_memsz,
             (phdr->p_flags & PF_R) ? "R" : "-",
             (phdr->p_flags & PF_W) ? "W" : "-",
             (phdr->p_flags & PF_X) ? "X" : "-");
    }

    return result;
}
