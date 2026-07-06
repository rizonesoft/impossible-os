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

/* Segment copy/zero use the kernel scalar memcpy/memset (libc string.c).
 * This path runs at CPL=0 during load with no FPU-save context, so it must
 * stay scalar -- never route through the SIMD memops dispatchers. */
extern void *memcpy(void *dst, const void *src, uint64_t n);
extern void *memset(void *dst, int c, uint64_t n);

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
 * callback, per the design choice to keep the dispatcher contract additive
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

    /* Each program-header entry must be exactly an elf64_phdr: the phdr
     * walkers dereference full struct objects, so a smaller e_phentsize
     * would let a crafted header point struct reads past the file buffer. */
    if (hdr->e_phentsize != sizeof(struct elf64_phdr)) {
        klog(LOG_DEBUG, "elf", "Bad e_phentsize %u", (uint64_t)hdr->e_phentsize);
        return 0;
    }

    /* Cap the program-header count before any phdr walk. e_phnum is a uint16
     * (max 65535), and elf_load does an O(n^2) PT_LOAD overlap check, so an
     * uncapped count lets a crafted ELF (tens of thousands of zero-sized
     * PT_LOADs, well under the 16 MiB exec image cap) force ~2 billion
     * iterations -- a kernel-time DoS during exec. 64 matches the bootloader
     * precedent (bootx64.c) and is far above any real binary. */
    if (hdr->e_phnum > ELF_MAX_PHNUM) {
        klog(LOG_DEBUG, "elf", "Too many program headers: %u", (uint64_t)hdr->e_phnum);
        return 0;
    }

    /* Program header table must fit in the file. Subtraction form: e_phoff
     * and e_phnum are attacker-controlled, so `e_phoff + phnum*phentsize`
     * could wrap uint64 and pass a naive `> size` compare. */
    if (hdr->e_phoff > size ||
        hdr->e_phnum > (size - hdr->e_phoff) / sizeof(struct elf64_phdr)) {
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
    int entry_ok = 0;

    if (size < sizeof(struct elf64_header)) {
        klog(LOG_DEBUG, "elf", "File too small");
        return result;
    }

    hdr = (const struct elf64_header *)data;

    if (!elf_validate(hdr, size))
        return result;

    /* --- Pass 1: validate every PT_LOAD WITHOUT mutating memory. ---
     * All fatal checks (file/user-range bounds, filesz<=memsz, no overlapping
     * segments, and e_entry inside an executable segment) run BEFORE any
     * memcpy/memset/pmm_mark_region_used in pass 2, so a rejected malformed
     * ELF leaves the shared user ELF frames and PMM state untouched -- a
     * failed exec is atomic rather than destructive. */
    for (i = 0; i < hdr->e_phnum; i++) {
        phdr = (const struct elf64_phdr *)(data + hdr->e_phoff +
                                            (uint64_t)i * hdr->e_phentsize);

        if (phdr->p_type != PT_LOAD)
            continue;

        /* Segment fits in file (subtraction form: p_offset and p_filesz are
         * attacker-controlled uint64, so `p_offset + p_filesz` could wrap and
         * pass a naive `> size` compare, then the pass-2 memcpy would read
         * data + p_offset out of bounds). */
        if (phdr->p_offset > size || phdr->p_filesz > size - phdr->p_offset) {
            klog(LOG_DEBUG, "elf", "Segment %u exceeds file size", (uint64_t)i);
            return result;
        }

        /* The pass-2 copy moves p_filesz bytes to p_vaddr, but the user-range
         * check only bounds p_memsz. Enforce the ELF invariant p_filesz <=
         * p_memsz so a crafted segment cannot write past the validated range. */
        if (phdr->p_filesz > phdr->p_memsz) {
            klog(LOG_ERROR, "elf", "Segment %u filesz > memsz", (uint64_t)i);
            return result;
        }

        /* Segment stays within user address range. Subtraction form with an
         * explicit p_vaddr upper bound: a high p_vaddr (near UINT64_MAX) plus
         * a small p_memsz would otherwise wrap below USER_ELF_END. This bound
         * also guarantees p_vaddr + p_memsz cannot wrap below. */
        if (phdr->p_vaddr < USER_ELF_BASE ||
            phdr->p_vaddr > USER_ELF_END ||
            phdr->p_memsz > USER_ELF_END - phdr->p_vaddr) {
            klog(LOG_ERROR, "elf",
                 "Segment %u outside user range: 0x%x-0x%x (allowed 0x%x-0x%x)",
                 (uint64_t)i, phdr->p_vaddr,
                 phdr->p_vaddr + phdr->p_memsz,
                 (uint64_t)USER_ELF_BASE, (uint64_t)USER_ELF_END);
            return result;
        }

        /* Reject overlapping PT_LOAD ranges. The ELF spec requires loadable
         * segments to occupy disjoint memory; without this a crafted file
         * could lay a tiny PF_X segment over e_entry and then a later non-PF_X
         * segment at the same vaddr whose bytes win the copy, defeating the
         * executable-entry check below. All sums are user-range-bounded above,
         * so no wrap. O(n^2) over e_phnum on a cold exec path. */
        {
            uint16_t j;
            for (j = 0; j < i; j++) {
                const struct elf64_phdr *pj =
                    (const struct elf64_phdr *)(data + hdr->e_phoff +
                                                (uint64_t)j * hdr->e_phentsize);
                if (pj->p_type != PT_LOAD)
                    continue;
                if (phdr->p_vaddr < pj->p_vaddr + pj->p_memsz &&
                    pj->p_vaddr < phdr->p_vaddr + phdr->p_memsz) {
                    klog(LOG_ERROR, "elf", "Segment %u overlaps segment %u",
                         (uint64_t)i, (uint64_t)j);
                    return result;
                }
            }
        }

        /* e_entry must land inside an executable loadable segment (checked
         * across all segments; overlap is already rejected above). */
        if ((phdr->p_flags & PF_X) &&
            hdr->e_entry >= phdr->p_vaddr &&
            hdr->e_entry < phdr->p_vaddr + phdr->p_memsz)
            entry_ok = 1;

        if (phdr->p_vaddr < load_base)
            load_base = phdr->p_vaddr;
        if (phdr->p_vaddr + phdr->p_memsz > load_end)
            load_end = phdr->p_vaddr + phdr->p_memsz;
        seg_count++;
    }

    if (load_end == 0) {
        klog(LOG_DEBUG, "elf", "No PT_LOAD segments found");
        return result;
    }

    /* The entry point must land inside an executable PT_LOAD segment, not
     * merely the aggregate [load_base, load_end) envelope. A crafted binary
     * with in-range segments but e_entry in an inter-segment gap or a non-exec
     * segment would otherwise run stale residue from a previous exec (the user
     * ELF range shares physical frames across processes). Verified in pass 1
     * before any mutation, so rejection here is non-destructive. */
    if (!entry_ok) {
        klog(LOG_DEBUG, "elf",
             "Entry 0x%x not in an executable segment", hdr->e_entry);
        return result;
    }

    /* --- Pass 2: mutate. All fatal validation passed in pass 1. --- */
    seg_count = 0;
    for (i = 0; i < hdr->e_phnum; i++) {
        phdr = (const struct elf64_phdr *)(data + hdr->e_phoff +
                                            (uint64_t)i * hdr->e_phentsize);

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

    result.entry = hdr->e_entry;
    result.load_base = load_base;
    result.load_end = load_end;
    result.success = 1;

    /*: derive auxv phdr metadata from the parsed program headers.
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
                    /* "requested", not "enforced": elf_exec_wrapper currently
                     * drops result.nx_stack, so no stack PTE NX bit is set yet
                     * (threading of the security fields is a separate open
                     * item). Do not claim enforcement we do not perform. */
                    klog(LOG_DEBUG, "elf", "NX stack requested (enforcement pending)");
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
                 * Overflow-safe bounds: subtraction-based checks.
                 * The note walk below is driven by p_filesz, so cap it: an
                 * uncapped payload (up to the 16 MiB exec image), times up to
                 * 64 PT_GNU_PROPERTY headers, is an exec-path CPU amplifier.
                 * Real notes are tens of bytes; ELF_GNU_PROPERTY_MAX (4 KiB)
                 * bounds each walk to a few hundred iterations. */
                if (phdr->p_filesz >= 16 &&
                    phdr->p_filesz <= ELF_GNU_PROPERTY_MAX &&
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
                                /* Advance in uint64: computing 8 + align(psize)
                                 * in uint32 wraps to 0 for psize near UINT32_MAX
                                 * (e.g. 0xFFFFFFF1), which would spin this loop
                                 * forever in kernel exec. adv is always >= 8, so
                                 * progress is guaranteed; break if the property
                                 * claims more than the descriptor has left. */
                                {
                                    uint64_t adv = 8 + (((uint64_t)psize + 7) & ~(uint64_t)7);
                                    if (adv > descsz - doff)
                                        break;
                                    doff += adv;
                                }
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

/*: cheap metadata-only walker for the ELF program header table.
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
