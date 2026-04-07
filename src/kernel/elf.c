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

/* Compute AT_PHDR/AT_PHENT/AT_PHNUM auxv metadata for an ELF image.
 *
 * Walks the program headers once. Sets *phdr_vaddr to PT_PHDR->p_vaddr if a
 * PT_PHDR entry exists; otherwise derives it from the PT_LOAD that contains
 * the file offset 'e_phoff'. Sets *phnum and *phent from the ELF header.
 *
 * Returns 1 on success, 0 on failure (no PT_LOAD covers e_phoff -- a malformed
 * or stripped ELF). On failure, output values are zeroed.
 *
 * This function is the SINGLE SOURCE OF TRUTH for auxv phdr derivation -- both
 * elf_load() (full segment-copying loader) and elf_extract_phdr_info() (cheap
 * metadata-only walker called from task_exec) call into this helper, so the
 * two paths cannot disagree about what AT_PHDR points at. The exec dispatcher
 * (exec.c) intentionally only carries the entry point through its loader
 * callback, per the §13 design choice to keep the dispatcher contract additive
 * (PE/EIF would otherwise need a metadata-fill no-op). */
static int elf_compute_phdr_info(const struct elf64_header *hdr,
                                  const uint8_t *data,
                                  uint64_t *out_phdr_vaddr,
                                  uint16_t *out_phnum,
                                  uint16_t *out_phent)
{
    const struct elf64_phdr *phdr;
    uint16_t i;
    uint64_t phdr_vaddr = 0;
    int found_load_for_phoff = 0;

    *out_phdr_vaddr = 0;
    *out_phnum = 0;
    *out_phent = 0;

    /* Two-pass walk: first pass finds PT_PHDR and stores its vaddr.
     * Second pass falls back to PT_LOAD covering e_phoff if PT_PHDR was
     * absent OR had a zero vaddr (a rare malformed ELF where PT_PHDR is
     * present but useless). */
    for (i = 0; i < hdr->e_phnum; i++) {
        phdr = (const struct elf64_phdr *)(data + hdr->e_phoff +
                                            (uint64_t)i * hdr->e_phentsize);
        if (phdr->p_type == PT_PHDR) {
            phdr_vaddr = phdr->p_vaddr;
            /* If vaddr is non-zero, PT_PHDR is authoritative -- we still
             * could break here, but it's cheap to keep scanning in case
             * a later PT_PHDR somehow has a different value (shouldn't
             * happen in a well-formed ELF, but loop is bounded by e_phnum). */
        }
    }

    /* Fallback pass: only if PT_PHDR was absent or had vaddr=0, derive
     * the program-header virtual address from the PT_LOAD that contains
     * the file offset of the program header table. */
    if (phdr_vaddr == 0) {
        for (i = 0; i < hdr->e_phnum; i++) {
            phdr = (const struct elf64_phdr *)(data + hdr->e_phoff +
                                                (uint64_t)i * hdr->e_phentsize);
            if (phdr->p_type != PT_LOAD)
                continue;
            /* Does this PT_LOAD cover e_phoff? Use subtraction-based bounds
             * to avoid overflow. */
            if (hdr->e_phoff < phdr->p_offset)
                continue;
            if (hdr->e_phoff - phdr->p_offset >= phdr->p_filesz)
                continue;
            phdr_vaddr = phdr->p_vaddr + (hdr->e_phoff - phdr->p_offset);
            found_load_for_phoff = 1;
            break;
        }
    }

    if (phdr_vaddr == 0 && !found_load_for_phoff) {
        /* Stripped ELF with no PT_PHDR and no PT_LOAD covering the program
         * header table -- the dynamic linker can't use AT_PHDR at all. */
        return 0;
    }

    *out_phdr_vaddr = phdr_vaddr;
    *out_phnum = hdr->e_phnum;
    *out_phent = hdr->e_phentsize;
    return 1;
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
    struct elf_load_result result;
    /* Zero all fields, then set defaults */
    {
        uint8_t *p = (uint8_t *)&result;
        uint64_t n;
        for (n = 0; n < sizeof(result); n++) p[n] = 0;
    }
    result.nx_stack = 1;  /* default: NX stack (safe, matches Linux 6.x) */
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

    /* §13: derive auxv phdr metadata from the parsed program headers.
     * Failure here is non-fatal -- the binary still runs, but AT_PHDR will
     * be 0 in the auxv (dynamic linker can't use it). */
    if (!elf_compute_phdr_info(hdr, data,
                                &result.phdr_vaddr,
                                &result.phnum,
                                &result.phent)) {
        klog(LOG_DEBUG, "elf",
             "Could not derive AT_PHDR (no PT_PHDR, no PT_LOAD covers e_phoff)");
    }

    /* --- Second pass: parse GNU security segments --- */
    {
        int found_gnu_stack = 0;
        result.nx_stack = 1;    /* default: NX stack (safe, matches Linux 6.x) */
        result.has_relro = 0;
        result.cet_ibt = 0;
        result.cet_shstk = 0;
        result.relro_start = 0;
        result.relro_size = 0;

        for (i = 0; i < hdr->e_phnum; i++) {
            phdr = (const struct elf64_phdr *)(data + hdr->e_phoff +
                                                (uint64_t)i * hdr->e_phentsize);

            switch (phdr->p_type) {
            case PT_GNU_STACK:
                found_gnu_stack = 1;
                if (phdr->p_flags & PF_X) {
                    result.nx_stack = 0;
                    klog(LOG_WARN, "elf", "Executable stack requested (legacy binary)");
                } else {
                    result.nx_stack = 1;
                    klog(LOG_DEBUG, "elf", "NX stack enforced");
                }
                break;

            case PT_GNU_RELRO:
                result.has_relro = 1;
                result.relro_start = phdr->p_vaddr;
                result.relro_size = phdr->p_memsz;
                klog(LOG_DEBUG, "elf", "RELRO range: 0x%x-0x%x (%u bytes)",
                     phdr->p_vaddr, phdr->p_vaddr + phdr->p_memsz,
                     phdr->p_memsz);
                break;

            case PT_GNU_PROPERTY:
                /* Parse GNU property notes for CET flags.
                 * Use byte reads to avoid unaligned type-pun UB.
                 * Overflow-safe bounds: subtraction-based checks. */
                if (phdr->p_filesz >= 16 &&
                    phdr->p_offset <= size &&
                    phdr->p_filesz <= size - phdr->p_offset) {
                    const uint8_t *note = data + phdr->p_offset;
                    uint64_t off = 0;
                    while (off + 12 <= phdr->p_filesz) {
                        uint32_t namesz = (uint32_t)note[off] |
                            ((uint32_t)note[off+1] << 8) |
                            ((uint32_t)note[off+2] << 16) |
                            ((uint32_t)note[off+3] << 24);
                        uint32_t descsz = (uint32_t)note[off+4] |
                            ((uint32_t)note[off+5] << 8) |
                            ((uint32_t)note[off+6] << 16) |
                            ((uint32_t)note[off+7] << 24);
                        uint32_t ntype = (uint32_t)note[off+8] |
                            ((uint32_t)note[off+9] << 8) |
                            ((uint32_t)note[off+10] << 16) |
                            ((uint32_t)note[off+11] << 24);
                        uint32_t name_aligned = (namesz + 7) & ~(uint32_t)7;
                        uint32_t desc_off = 12 + name_aligned;

                        if (desc_off > phdr->p_filesz - off ||
                            descsz > phdr->p_filesz - off - desc_off)
                            break;

                        /* NT_GNU_PROPERTY_TYPE_0: scan property entries */
                        if (ntype == 5 && descsz >= 8) {
                            const uint8_t *desc = note + off + desc_off;
                            uint64_t doff = 0;
                            while (doff + 8 <= descsz) {
                                uint32_t ptype = (uint32_t)desc[doff] |
                                    ((uint32_t)desc[doff+1] << 8) |
                                    ((uint32_t)desc[doff+2] << 16) |
                                    ((uint32_t)desc[doff+3] << 24);
                                uint32_t psize = (uint32_t)desc[doff+4] |
                                    ((uint32_t)desc[doff+5] << 8) |
                                    ((uint32_t)desc[doff+6] << 16) |
                                    ((uint32_t)desc[doff+7] << 24);
                                if (ptype == GNU_PROPERTY_X86_FEATURE_1_AND &&
                                    psize >= 4 && doff + 12 <= descsz) {
                                    uint32_t features = (uint32_t)desc[doff+8] |
                                        ((uint32_t)desc[doff+9] << 8) |
                                        ((uint32_t)desc[doff+10] << 16) |
                                        ((uint32_t)desc[doff+11] << 24);
                                    result.cet_ibt = (features & GNU_PROPERTY_X86_FEATURE_1_IBT) ? 1 : 0;
                                    result.cet_shstk = (features & GNU_PROPERTY_X86_FEATURE_1_SHSTK) ? 1 : 0;
                                    if (result.cet_ibt || result.cet_shstk)
                                        klog(LOG_DEBUG, "elf", "CET flags: IBT=%u SHSTK=%u",
                                             (uint64_t)result.cet_ibt, (uint64_t)result.cet_shstk);
                                }
                                doff += 8 + ((psize + 7) & ~(uint32_t)7);
                            }
                        }
                        off += desc_off + ((descsz + 7) & ~(uint32_t)7);
                    }
                }
                break;
            }
        }

        if (!found_gnu_stack)
            klog(LOG_DEBUG, "elf", "No PT_GNU_STACK -- defaulting to NX stack");
    }

    klog(LOG_DEBUG, "elf", "Loaded %u PT_LOAD segments at 0x%x-0x%x, entry=0x%x%s",
         (uint64_t)seg_count, result.load_base, result.load_end, result.entry,
         (hdr->e_type == ET_DYN) ? " (PIE)" : "");

    return result;
}

/* §13: cheap metadata-only walker for the ELF program header table.
 *
 * Used by task_exec() after exec_load() has already loaded the binary via
 * the format dispatcher. The dispatcher contract only carries 'entry' back,
 * so this helper re-parses the header bytes to derive AT_PHDR/PHENT/PHNUM
 * for the auxv. The actual program-header walk is shared with elf_load()
 * via elf_compute_phdr_info() -- single source of truth.
 *
 * This is a constant-time validation pass; it does NOT copy segments or
 * touch any user-mode memory. Safe to call from task_exec context. */
int elf_extract_phdr_info(const uint8_t *data, uint64_t size,
                          uint64_t *phdr_vaddr, uint16_t *phnum,
                          uint16_t *phent)
{
    const struct elf64_header *hdr;

    /* Validate output pointers FIRST -- if any out-pointer is NULL we
     * cannot honor the "outputs zeroed on failure" contract, so reject
     * the call without touching anything. */
    if (!phdr_vaddr || !phnum || !phent)
        return 0;

    /* Per the header contract: clear out-pointers BEFORE any failure path
     * so callers that ignore the return value never observe stale values.
     * This must happen even when 'data' is NULL or 'size' is too small --
     * the only path that skips clearing is the NULL-out-pointer case
     * above (where there is nothing to clear). */
    *phdr_vaddr = 0;
    *phnum = 0;
    *phent = 0;

    if (!data)
        return 0;

    if (size < sizeof(struct elf64_header))
        return 0;

    hdr = (const struct elf64_header *)data;

    if (!elf_validate(hdr, size))
        return 0;

    return elf_compute_phdr_info(hdr, data, phdr_vaddr, phnum, phent);
}
