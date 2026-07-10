/* ============================================================================
 * eif.c -- Executable Impossible Format (EIF) loader
 *
 * Loads EIF binaries: validates header, copies segments to user address
 * range (identity-mapped), validates import table against SSDT.
 * Spec: specs/eif-format.md
 * ============================================================================ */

#include "kernel/eif.h"
#include "kernel/elf.h"
#include "kernel/errno.h"
#include "kernel/klog.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/user_range.h"
#include "kernel/nt/ssdt.h"
#include "kernel/timer.h"

/* Segment copy/zero use the kernel scalar memcpy/memset (libc string.c), the
 * same as the ELF loader (section 2) -- byte-at-a-time local loops were a
 * measurable cold-path cost as images grow. Declared here (freestanding: no
 * <string.h>). */
extern void *memcpy(void *dst, const void *src, uint64_t n);
extern void *memset(void *dst, int c, uint64_t n);

/* The segment cap mirrors the ELF loader's program-header cap (both loaders
 * share the identity-mapped user range and a 64-entry ceiling far above any
 * real binary); pin the documented relationship so it cannot silently drift. */
_Static_assert(EIF_MAX_SEGMENTS == ELF_MAX_PHNUM,
    "EIF_MAX_SEGMENTS must match ELF_MAX_PHNUM");

/* ---- Helpers ------------------------------------------------------------ */

/* Read a little-endian uint32 without alignment assumptions */
static uint32_t eif_read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* True when an EIF import's SSDT service number resolves to a REGISTERED
 * main-table handler: in range (< SSDT_MAIN_MAX -- the index-mask 0x0FFF is
 * only for extraction, the table holds SSDT_MAIN_MAX handlers) AND the slot is
 * not the not-implemented stub. The dispatch table's "available" flag means
 * exactly this (spec: "available = registered in SSDT"), so a required import
 * to an unregistered in-range slot fails the load instead of being marked
 * available and faulting at first dispatch. Side-effect free -- it inspects the
 * handler pointer and never dispatches (spec §5: import validation is
 * dispatch-free). EIF imports are main-table only (spec §4: 0x0000-0x03FF). */
static int eif_import_available(uint32_t syscall_id)
{
    const SSDT_TABLE *tbl;

    if (syscall_id >= SSDT_MAIN_MAX)
        return 0;
    tbl = ssdt_get_table(SSDT_TABLE_MAIN);
    if (!tbl || !tbl->handlers)
        return 0;
    return tbl->handlers[syscall_id] != ssdt_stub_not_implemented;
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
    } else if (hdr->segment_offset != 0) {
        /* Rule 2: an absent table (count 0) MUST carry offset 0 -- otherwise a
         * non-zero, out-of-order offset would skip the canonical-order cursor. */
        klog(LOG_DEBUG, "eif", "Segment table absent but offset nonzero");
        return 0;
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
    } else if (hdr->import_offset != 0) {
        /* Rule 2: an absent import table (count 0) MUST carry offset 0. */
        klog(LOG_DEBUG, "eif", "Import table absent but offset nonzero");
        return 0;
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

    /* Canonical section ordering + non-overlap (spec rule 2): the header-declared
     * regions MUST appear in the fixed order segment table -> import table ->
     * metadata -> signature, each fully after the previous and non-overlapping.
     * A single ascending cursor enforces both -- every present region must start
     * at or after the end of the previous one. Each region's end was already
     * bounds-checked <= size above, so the cursor never exceeds the file.
     * (Segment DATA placement between the import table and metadata is bounded
     * per-segment against file size in eif_load; a tighter data-vs-metadata
     * overlap check is deferred with signing, section 17.) */
    {
        uint64_t cursor = sizeof(eif_header_t);
        if (hdr->segment_count > 0) {
            if (hdr->segment_offset < cursor) {
                klog(LOG_DEBUG, "eif", "Segment table out of canonical order");
                return 0;
            }
            cursor = (uint64_t)hdr->segment_offset +
                     (uint64_t)hdr->segment_count * sizeof(eif_segment_t);
        }
        if (hdr->import_count > 0) {
            if (hdr->import_offset < cursor) {
                klog(LOG_DEBUG, "eif", "Import table out of canonical order");
                return 0;
            }
            cursor = (uint64_t)hdr->import_offset +
                     (uint64_t)hdr->import_count * sizeof(eif_import_t);
        }
        if (hdr->metadata_offset != 0) {
            if (hdr->metadata_offset < cursor) {
                klog(LOG_DEBUG, "eif", "Metadata out of canonical order");
                return 0;
            }
            cursor = (hdr->flags & EIF_FLAG_SIGNED)
                   ? hdr->signature_offset : size;
        }
        if (hdr->signature_offset != 0 && hdr->signature_offset < cursor) {
            klog(LOG_DEBUG, "eif", "Signature out of canonical order");
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
    uint32_t available_count = 0;  /* registered imports, reported after timing */
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

    /* Reject COMPRESSED binaries until EIF segment decompression is implemented.
     * The segment copy below moves file bytes verbatim into the executable user
     * range; loading a compressed image would run the LZ4 stream as code. Fail
     * closed rather than execute garbage. */
    if (hdr->flags & EIF_FLAG_COMPRESSED) {
        klog(LOG_WARN, "eif", "Compressed EIF rejected: decompression not yet implemented");
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

        /* Segment DATA canonical placement (spec rule 2): the fixed file order
         * is header -> segment table -> import table -> segment data -> metadata
         * -> signature. Every file-backed segment range must therefore begin at
         * or after the end of BOTH tables (tables_end) and end at or before the
         * metadata/signature region (data_limit). Canonical order of the tables
         * themselves was enforced in eif_validate, so tables_end is just the end
         * of whichever table is present last. */
        uint64_t tables_end = sizeof(eif_header_t);
        uint64_t data_limit = size;
        if (hdr->import_count > 0)
            tables_end = (uint64_t)hdr->import_offset +
                         (uint64_t)hdr->import_count * sizeof(eif_import_t);
        else
            tables_end = (uint64_t)hdr->segment_offset +
                         (uint64_t)hdr->segment_count * sizeof(eif_segment_t);
        if (hdr->metadata_offset != 0)
            data_limit = hdr->metadata_offset;
        else if (hdr->signature_offset != 0)
            data_limit = hdr->signature_offset;

        for (i = 0; i < hdr->segment_count; i++) {
            const eif_segment_t *seg = (const eif_segment_t *)
                (data + hdr->segment_offset + (uint64_t)i * sizeof(eif_segment_t));
            uint64_t vaddr = hdr->load_base + seg->vaddr;

            /* Spec: the segment `reserved` field MUST be 0. Reject non-zero on
             * untrusted disk-sourced data so the constraint can't be quietly
             * repurposed by a crafted binary. */
            if (seg->reserved != 0) {
                klog(LOG_DEBUG, "eif", "Segment %u: reserved field nonzero", (uint64_t)i);
                return 0;
            }

            /* file_size must not exceed mem_size (BSS = mem_size - file_size) */
            if (seg->file_size > seg->mem_size) {
                klog(LOG_ERROR, "eif", "Segment %u: file_size > mem_size", (uint64_t)i);
                return 0;
            }

            /* Validate segment data fits in file, and sits inside the canonical
             * segment-data region [tables_end, data_limit) (spec rule 2 -- after
             * both tables, before metadata/signature). data_limit <= size, so
             * the second compare also enforces the in-file bound. */
            if (seg->file_size > 0) {
                if (seg->file_offset < tables_end ||
                    seg->file_offset >= data_limit ||
                    seg->file_size > data_limit - seg->file_offset) {
                    klog(LOG_ERROR, "eif", "Segment %u data outside canonical region", (uint64_t)i);
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

            /* A REQUIRED import must resolve to a registered main-table handler
             * (in range AND non-stub); an optional import may be absent (it is
             * marked unavailable in the dispatch table below). Registration is a
             * side-effect-free handler-pointer inspection, not a dispatch. */
            if (!eif_import_available(imp->syscall_id) &&
                !(imp->flags & EIF_IMP_OPTIONAL)) {
                klog(LOG_ERROR, "eif",
                     "Required import SSDT 0x%x unavailable (out of range or unregistered)",
                     (uint64_t)imp->syscall_id);
                return 0;
            }
        }
    }

    /* ---- Validate entry point ---- */
    {
        uint64_t entry_va = hdr->load_base + hdr->entry_point;
        int entry_in_exec = 0;

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

        /* The entry point must land inside a loaded EXECUTABLE segment, not
         * merely the user window. The shared identity-mapped user range may
         * still hold a previous process's bytes, so an entry in an inter-segment
         * gap or a non-exec segment would run stale residue. Mirrors the ELF
         * loader's e_entry-in-exec-segment check. All segment bounds were
         * validated above, so vaddr + mem_size does not wrap. */
        for (i = 0; i < hdr->segment_count; i++) {
            const eif_segment_t *seg = (const eif_segment_t *)
                (data + hdr->segment_offset + (uint64_t)i * sizeof(eif_segment_t));
            uint64_t vaddr = hdr->load_base + seg->vaddr;

            if ((seg->flags & EIF_SEG_EXEC) &&
                entry_va >= vaddr && entry_va < vaddr + seg->mem_size) {
                entry_in_exec = 1;
                break;
            }
        }
        if (!entry_in_exec) {
            klog(LOG_ERROR, "eif",
                 "Entry point 0x%x not in an executable segment", entry_va);
            return 0;
        }
    }

    /* ============================================================
     * MUTATION PHASE -- every segment, import, and the entry point
     * passed above, so the copies and PMM/dispatch-table writes below
     * cannot be aborted partway by a validation failure.
     * ============================================================ */

    /* ---- Copy segments (identity-mapped: vaddr == paddr) ----
     * Per-segment success logging is intentionally omitted from this cold exec
     * path: it added synchronous serial I/O (spinlock + up to 64 klog calls)
     * inside the timed region, blowing the <10 us load budget; the single
     * aggregate record below carries the segment count. Rejected segments still
     * log their specific error in the validation phase. */
    for (i = 0; i < hdr->segment_count; i++) {
        const eif_segment_t *seg = (const eif_segment_t *)
            (data + hdr->segment_offset + (uint64_t)i * sizeof(eif_segment_t));
        uint64_t vaddr = hdr->load_base + seg->vaddr;

        /* Copy file data */
        if (seg->file_size > 0)
            memcpy((void *)vaddr, data + seg->file_offset, seg->file_size);

        /* Zero BSS (mem_size > file_size) */
        if (seg->mem_size > seg->file_size)
            memset((void *)(vaddr + seg->file_size), 0,
                   seg->mem_size - seg->file_size);

        /* Reserve physical pages */
        pmm_mark_region_used((uintptr_t)vaddr, seg->mem_size);

        seg_loaded++;
    }

    /* ---- Write the per-process import dispatch table ----
     * Identity-mapped: kernel writes directly to the user-space address. There
     * is NO exec-path serializing lock; safety today rests on the single shared
     * user address space (EIF_DISPATCH_TABLE_ADDR and the user range are one
     * fixed global region, so only one process image can exist at a time) -- NOT
     * on task-context locality. Concurrent exec becomes safe only with the
     * future per-process physical pages / per-process VMM prerequisite. */
    if (hdr->import_count > 0) {
        eif_dispatch_entry_t *table =
            (eif_dispatch_entry_t *)EIF_DISPATCH_TABLE_ADDR;

        memset((void *)table, 0,
               hdr->import_count * sizeof(eif_dispatch_entry_t));

        for (i = 0; i < hdr->import_count; i++) {
            const eif_import_t *imp = (const eif_import_t *)
                (data + hdr->import_offset +
                 (uint64_t)i * sizeof(eif_import_t));

            /* "available" means registered (in range AND non-stub) -- the same
             * check the validation pass enforced for required imports. */
            int available = eif_import_available(imp->syscall_id);

            table[i].syscall_id = imp->syscall_id;
            table[i].available = available ? 1 : 0;

            if (available)
                available_count++;
        }

        pmm_mark_region_used(EIF_DISPATCH_TABLE_ADDR,
            hdr->import_count * sizeof(eif_dispatch_entry_t));
    }

    /* ---- Performance measurement ----
     * Capture end_ns BEFORE any success-path logging: klog takes the log
     * spinlock and does synchronous serial I/O, so a log inside the timed
     * region would blow the <10 us load budget it is meant to measure. All
     * diagnostics (segment/import/available counts) go in the single aggregate
     * record emitted AFTER the timer stops. */
    end_ns = uptime_ns();
    load_us = (end_ns - start_ns) / 1000;  /* ns -> us */

    {
        uint64_t entry_va = hdr->load_base + hdr->entry_point;
        klog(LOG_DEBUG, "eif",
             "Loaded %u segments, %u/%u imports available in %u us, entry=0x%x",
             (uint64_t)seg_loaded, (uint64_t)available_count,
             (uint64_t)hdr->import_count, load_us, entry_va);
        return entry_va;
    }
}
