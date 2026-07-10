/* ============================================================================
 * eif.c -- Executable Impossible Format (EIF) loader
 *
 * Loads EIF binaries: validates header, copies segments to user address
 * range (identity-mapped), validates import table against SSDT.
 * Spec: specs/eif-format.md
 * ============================================================================ */

#include "kernel/eif.h"
#include "kernel/errno.h"
#include "kernel/klog.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/user_range.h"
#include "kernel/nt/ssdt.h"
#include "kernel/timer.h"

/* ---- Helpers ------------------------------------------------------------ */

static void eif_memcpy(uint8_t *dst, const uint8_t *src, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

static void eif_memzero(uint8_t *dst, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++)
        dst[i] = 0;
}

/* Read a little-endian uint32 without alignment assumptions */
static uint32_t eif_read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- Validation --------------------------------------------------------- */

static int eif_validate(const eif_header_t *hdr, uint64_t size)
{
    /* Magic check (byte-safe: struct is packed, magic at offset 0) */
    if (eif_read32((const uint8_t *)&hdr->magic) != EIF_MAGIC) {
        klog(LOG_DEBUG, "eif", "Bad magic");
        return 0;
    }

    if (hdr->version == 0 || hdr->version > EIF_VERSION) {
        klog(LOG_DEBUG, "eif", "Unsupported version %u", (uint64_t)hdr->version);
        return 0;
    }

    if (hdr->arch != EIF_ARCH_X86_64) {
        klog(LOG_DEBUG, "eif", "Wrong architecture %u", (uint64_t)hdr->arch);
        return 0;
    }

    /* Segment table bounds + count cap. The cap bounds the O(n^2) segment
     * overlap walk in eif_load; the 64-bit end computation is wrap-safe. */
    if (hdr->segment_count > 0) {
        uint64_t seg_end = (uint64_t)hdr->segment_offset +
                           (uint64_t)hdr->segment_count * sizeof(eif_segment_t);
        if (hdr->segment_count > EIF_MAX_SEGMENTS) {
            klog(LOG_DEBUG, "eif", "Too many segments: %u (max %u)",
                 (uint64_t)hdr->segment_count, (uint64_t)EIF_MAX_SEGMENTS);
            return 0;
        }
        if (hdr->segment_offset < sizeof(eif_header_t) || seg_end > size) {
            klog(LOG_DEBUG, "eif", "Segment table out of bounds");
            return 0;
        }
    }

    /* Import table bounds + count cap (spec rule 6). The 64-bit end is wrap-safe
     * (uint32 offset + uint32 count * 8 cannot overflow uint64); the cap bounds
     * the two-pass import walk in eif_load and keeps import_count within the
     * dispatch table it writes. */
    if (hdr->import_count > 0) {
        uint64_t imp_end = (uint64_t)hdr->import_offset +
                           (uint64_t)hdr->import_count * sizeof(eif_import_t);
        if (hdr->import_count > EIF_MAX_IMPORTS) {
            klog(LOG_DEBUG, "eif", "Too many imports: %u (max %u)",
                 (uint64_t)hdr->import_count, (uint64_t)EIF_MAX_IMPORTS);
            return 0;
        }
        if (hdr->import_offset < sizeof(eif_header_t) || imp_end > size) {
            klog(LOG_DEBUG, "eif", "Import table out of bounds");
            return 0;
        }
    }

    /* Signature offset (spec rule 3 canonical order): a signed file must carry
     * a non-zero signature_offset; an UNSIGNED file must have it zero. Keying
     * the metadata upper bound below on the SIGNED flag (not merely on
     * signature_offset != 0) stops an unsigned file from supplying a bogus
     * signature_offset to truncate its own metadata range. Full signature-size
     * validation is deferred to section 17. */
    if (hdr->flags & EIF_FLAG_SIGNED) {
        if (hdr->signature_offset == 0) {
            klog(LOG_ERROR, "eif", "SIGNED flag set but signature_offset is 0");
            return 0;
        }
    } else if (hdr->signature_offset != 0) {
        klog(LOG_DEBUG, "eif", "Unsigned file has non-zero signature_offset");
        return 0;
    }

    /* A non-zero signature_offset must lie inside the file: it is the upper
     * bound of the metadata range below (spec rules 2/8) and the start of the
     * signed-content boundary that section 17 verification consumes. */
    if (hdr->signature_offset != 0 && hdr->signature_offset > size) {
        klog(LOG_DEBUG, "eif", "Signature offset past end of file");
        return 0;
    }

    /* Metadata bounds (spec rule 8): when metadata_offset is non-zero its range
     * is [metadata_offset, signature_offset) for a signed file or
     * [metadata_offset, size) for an unsigned file. The range must start after
     * the header and be non-empty, so metadata parsing (deferred) can never read
     * outside the file or past the signed boundary. Canonical order (rule 2)
     * also requires metadata to precede the signature. */
    if (hdr->metadata_offset != 0) {
        uint64_t meta_end = (hdr->flags & EIF_FLAG_SIGNED)
                          ? hdr->signature_offset : size;
        if (hdr->metadata_offset < sizeof(eif_header_t) ||
            hdr->metadata_offset >= meta_end) {
            klog(LOG_DEBUG, "eif", "Metadata range out of bounds");
            return 0;
        }
    }

    return 1;
}

/* ---- Loader ------------------------------------------------------------- */

uint64_t eif_load(const uint8_t *data, uint64_t size)
{
    const eif_header_t *hdr;
    uint32_t i;
    uint32_t seg_loaded = 0;
    uint64_t start_ns, end_ns, load_us;

    start_ns = uptime_ns();

    if (!data || size < sizeof(eif_header_t)) {
        klog(LOG_DEBUG, "eif", "Data too small");
        return 0;
    }

    hdr = (const eif_header_t *)data;

    if (!eif_validate(hdr, size))
        return 0;

    /* Reject SIGNED binaries until signature verification is implemented (section 17) */
    if (hdr->flags & EIF_FLAG_SIGNED) {
        klog(LOG_WARN, "eif", "Signed EIF rejected: signature verification not yet implemented");
        return 0;
    }

    if (hdr->segment_count == 0) {
        klog(LOG_DEBUG, "eif", "No segments loaded");
        return 0;
    }

    /* ============================================================
     * VALIDATION PHASE -- no memory is mutated until EVERY segment,
     * import, and the entry point has passed. Mirrors the ELF loader
     * (section 2): a rejected malformed EIF leaves the shared user
     * frames, the import dispatch table, and PMM state untouched, so a
     * failed exec is atomic rather than destructive. Ordering matters --
     * segment/import/entry validation ALL precede the first copy so a
     * valid-segments-but-bad-import (or bad-entry) file cannot overwrite
     * the prior process image before it is rejected.
     * ============================================================ */

    /* ---- Validate segments (spec rule 7: ascending, non-overlapping) ---- */
    {
        uint64_t prev_vaddr_end = 0;   /* end of previous segment's mem range */
        uint64_t prev_file_end = 0;    /* end of previous file-backed range */
        int have_prev = 0;             /* prev_vaddr_end valid */
        int have_prev_file = 0;        /* prev_file_end valid */

        for (i = 0; i < hdr->segment_count; i++) {
            const eif_segment_t *seg = (const eif_segment_t *)
                (data + hdr->segment_offset + (uint64_t)i * sizeof(eif_segment_t));
            uint64_t vaddr = hdr->load_base + seg->vaddr;

            /* file_size must not exceed mem_size (BSS = mem_size - file_size) */
            if (seg->file_size > seg->mem_size) {
                klog(LOG_ERROR, "eif", "Segment %u: file_size > mem_size", (uint64_t)i);
                return 0;
            }

            /* Validate segment data fits in file */
            if (seg->file_size > 0) {
                if (seg->file_offset >= size ||
                    seg->file_size > size - seg->file_offset) {
                    klog(LOG_ERROR, "eif", "Segment %u data out of bounds", (uint64_t)i);
                    return 0;
                }
            }

            /* Validate vaddr computation doesn't overflow */
            if (hdr->load_base > USER_ELF_END || seg->vaddr > USER_ELF_END - hdr->load_base) {
                klog(LOG_ERROR, "eif", "Segment %u: vaddr overflow", (uint64_t)i);
                return 0;
            }

            /* Validate segment stays within user address range (overflow-safe)
             * and does not overlap the dispatch table region */
            if (vaddr < USER_ELF_BASE ||
                seg->mem_size > USER_ELF_END - vaddr ||
                (vaddr + seg->mem_size > EIF_DISPATCH_TABLE_ADDR &&
                 vaddr < EIF_DISPATCH_TABLE_ADDR +
                         EIF_DISPATCH_TABLE_MAX * sizeof(eif_dispatch_entry_t))) {
                klog(LOG_ERROR, "eif", "Segment %u outside user range: 0x%x",
                     (uint64_t)i, vaddr);
                return 0;
            }

            /* Spec rule 7: segments MUST be sorted by ascending vaddr with
             * monotonic non-overlapping [vaddr, vaddr+mem_size) and
             * [file_offset, file_offset+file_size) ranges, so non-overlap is a
             * single O(n) walk (compare each entry to the previous end) instead
             * of the ELF loader's O(n^2). Both prior ends are bounded above
             * (user-range / file-size checks), so the compares never wrap. A
             * file_size==0 (pure-BSS) segment carries no file range, so it does
             * not advance or check prev_file_end. */
            if (have_prev && vaddr < prev_vaddr_end) {
                klog(LOG_ERROR, "eif",
                     "Segment %u vaddr 0x%x overlaps/precedes prev end 0x%x",
                     (uint64_t)i, vaddr, prev_vaddr_end);
                return 0;
            }
            if (seg->file_size > 0 && have_prev_file &&
                (uint64_t)seg->file_offset < prev_file_end) {
                klog(LOG_ERROR, "eif",
                     "Segment %u file range overlaps previous segment", (uint64_t)i);
                return 0;
            }

            prev_vaddr_end = vaddr + seg->mem_size;
            have_prev = 1;
            if (seg->file_size > 0) {
                prev_file_end = (uint64_t)seg->file_offset + seg->file_size;
                have_prev_file = 1;
            }
        }
    }

    /* ---- Validate imports (range-check every required syscall id) ---- */
    if (hdr->import_count > 0) {
        /* Guaranteed by eif_validate's EIF_MAX_IMPORTS cap (== dispatch table
         * size); kept as a local guard on the dispatch-table write below. */
        if (hdr->import_count > EIF_DISPATCH_TABLE_MAX) {
            klog(LOG_ERROR, "eif", "Too many imports: %u (max %u)",
                 (uint64_t)hdr->import_count,
                 (uint64_t)EIF_DISPATCH_TABLE_MAX);
            return 0;
        }

        for (i = 0; i < hdr->import_count; i++) {
            const eif_import_t *imp = (const eif_import_t *)
                (data + hdr->import_offset +
                 (uint64_t)i * sizeof(eif_import_t));

            /* EIF imports are main-table SSDT service numbers only (spec §4:
             * 0x0000-0x03FF). SSDT_INDEX_MASK (0x0FFF) is the index EXTRACTION
             * mask, not the dispatch bound -- the main table holds SSDT_MAIN_MAX
             * (1024) handlers, so ssdt_dispatch rejects any index >= that bound.
             * Bounding here at SSDT_MAIN_MAX keeps "available" honest: an id in
             * [0x400, 0xFFF] would otherwise pass and be marked available, then
             * fault at first dispatch. */
            int in_range = (imp->syscall_id < SSDT_MAIN_MAX);

            if (!in_range && !(imp->flags & EIF_IMP_OPTIONAL)) {
                klog(LOG_ERROR, "eif",
                     "Required import SSDT 0x%x out of range",
                     (uint64_t)imp->syscall_id);
                return 0;
            }
        }
    }

    /* ---- Validate entry point is within user range ---- */
    {
        uint64_t entry_va = hdr->load_base + hdr->entry_point;
        /* Overflow check */
        if (hdr->entry_point > USER_ELF_END ||
            hdr->load_base > USER_ELF_END - hdr->entry_point) {
            klog(LOG_ERROR, "eif", "Entry point overflow");
            return 0;
        }
        if (entry_va < USER_ELF_BASE || entry_va >= USER_ELF_END) {
            klog(LOG_ERROR, "eif", "Entry point 0x%x outside user range", entry_va);
            return 0;
        }
    }

    /* ============================================================
     * MUTATION PHASE -- every segment, import, and the entry point
     * passed above, so the copies and PMM/dispatch-table writes below
     * cannot be aborted partway by a validation failure.
     * ============================================================ */

    /* ---- Copy segments (identity-mapped: vaddr == paddr) ---- */
    for (i = 0; i < hdr->segment_count; i++) {
        const eif_segment_t *seg = (const eif_segment_t *)
            (data + hdr->segment_offset + (uint64_t)i * sizeof(eif_segment_t));
        uint64_t vaddr = hdr->load_base + seg->vaddr;

        /* Copy file data */
        if (seg->file_size > 0)
            eif_memcpy((uint8_t *)vaddr, data + seg->file_offset, seg->file_size);

        /* Zero BSS (mem_size > file_size) */
        if (seg->mem_size > seg->file_size)
            eif_memzero((uint8_t *)(vaddr + seg->file_size),
                        seg->mem_size - seg->file_size);

        /* Reserve physical pages */
        pmm_mark_region_used((uintptr_t)vaddr, seg->mem_size);

        seg_loaded++;
        klog(LOG_DEBUG, "eif", "Segment %u: vaddr=0x%x size=%u flags=%s%s%s",
             (uint64_t)i, vaddr, seg->mem_size,
             (seg->flags & EIF_SEG_READ)  ? "R" : "-",
             (seg->flags & EIF_SEG_WRITE) ? "W" : "-",
             (seg->flags & EIF_SEG_EXEC)  ? "X" : "-");
    }

    /* ---- Write the per-process import dispatch table ----
     * Identity-mapped: kernel writes directly to the user-space address.
     * SMP-safe because task_exec() runs in the calling task's context and no
     * other CPU accesses this task's user range during exec. True per-process
     * isolation requires per-process physical pages (future VMM prerequisite). */
    if (hdr->import_count > 0) {
        eif_dispatch_entry_t *table =
            (eif_dispatch_entry_t *)EIF_DISPATCH_TABLE_ADDR;
        uint32_t available_count = 0;

        eif_memzero((uint8_t *)table,
                    hdr->import_count * sizeof(eif_dispatch_entry_t));

        for (i = 0; i < hdr->import_count; i++) {
            const eif_import_t *imp = (const eif_import_t *)
                (data + hdr->import_offset +
                 (uint64_t)i * sizeof(eif_import_t));

            /* Same main-table bound as the validation pass above -- an id the
             * dispatcher cannot reach must be marked unavailable, not available. */
            int in_range = (imp->syscall_id < SSDT_MAIN_MAX);

            table[i].syscall_id = imp->syscall_id;
            table[i].available = in_range ? 1 : 0;

            if (in_range)
                available_count++;
        }

        pmm_mark_region_used(EIF_DISPATCH_TABLE_ADDR,
            hdr->import_count * sizeof(eif_dispatch_entry_t));

        klog(LOG_DEBUG, "eif",
             "Dispatch table: %u/%u imports at 0x%x",
             (uint64_t)available_count,
             (uint64_t)hdr->import_count,
             (uint64_t)EIF_DISPATCH_TABLE_ADDR);
    }

    /* ---- Performance measurement ---- */
    end_ns = uptime_ns();
    load_us = (end_ns - start_ns) / 1000;  /* ns -> us */

    {
        uint64_t entry_va = hdr->load_base + hdr->entry_point;
        klog(LOG_DEBUG, "eif", "Loaded %u segments, %u imports in %u us, entry=0x%x",
             (uint64_t)seg_loaded, (uint64_t)hdr->import_count,
             load_us, entry_va);
        return entry_va;
    }
}
