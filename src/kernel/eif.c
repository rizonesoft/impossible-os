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
#include "libs/lz4.h"

/* Segment copy/zero use the kernel scalar memcpy/memset (libc string.c), the
 * same as the ELF loader (section 2) -- byte-at-a-time local loops were a
 * measurable cold-path cost as images grow. Declared here (freestanding: no
 * <string.h>). */
extern void *memcpy(void *dst, const void *src, uint64_t n);
extern void *memset(void *dst, int c, uint64_t n);

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

/* ---- Loader ------------------------------------------------------------- */

uint64_t eif_load(const uint8_t *data, uint64_t size)
{
    const eif_header_t *hdr;
    uint32_t i;
    uint32_t seg_loaded = 0;
    uint32_t available_count = 0;  /* registered imports, reported after timing */
    uint64_t start_ns, end_ns, load_us;
    eif_metadata_t meta;           /* parsed pre-mutation; logged after timing */
    int compressed;                /* EIF_FLAG_COMPRESSED: all segments LZ4 */
    uint64_t max_decomp = 0;       /* largest mem_size among compressed segments */

    start_ns = uptime_ns();

    {
        eif_check_result_t chk;
        if (!eif_check(data, size, eif_import_available, &chk))
            return 0;
        hdr = (const eif_header_t *)data;
        compressed = chk.compressed;
        max_decomp = chk.max_decomp;
        meta = chk.meta;
    }

    /* ---- Compressed-segment preflight (still validation phase) ----
     * LZ4 decompression can fail on a corrupt or size-lying stream, so it cannot
     * run in the mutation phase without breaking that phase's "cannot be aborted
     * partway" invariant. Decompress every compressed segment into a scratch
     * buffer HERE and verify each produces exactly mem_size. Only if ALL preflights
     * pass does the mutation phase decompress into the user range -- the input is
     * immutable and LZ4 is deterministic, so that second decode cannot fail. One
     * scratch buffer sized to the largest decompressed segment is reused across
     * segments and freed on every path (scratch, user range, and the EIF file
     * buffer are three distinct non-overlapping regions). */
    if (compressed && max_decomp > 0) {
        uint64_t pages = (max_decomp + 4095) / 4096;
        uint32_t cap = (uint32_t)(pages * 4096);
        uint8_t *scratch = (uint8_t *)(uintptr_t)pmm_alloc_contiguous(pages);
        int bad = 0;
        uint64_t p;
        if (!scratch) {
            klog(LOG_ERROR, "eif", "Compressed EIF: no memory for decompress scratch");
            return 0;
        }
        for (i = 0; i < hdr->segment_count && !bad; i++) {
            const eif_segment_t *seg = (const eif_segment_t *)
                (data + hdr->segment_offset + (uint64_t)i * sizeof(eif_segment_t));
            if (seg->file_size == 0)
                continue;   /* pure-BSS: zero-filled in mutation, nothing to decode */
            if (!eif_decompress_segment(data + seg->file_offset, seg->file_size,
                                        scratch, cap, seg->mem_size)) {
                klog(LOG_ERROR, "eif",
                     "Segment %u: corrupt or size-mismatched compressed stream",
                     (uint64_t)i);
                bad = 1;
            }
        }
        for (p = 0; p < pages; p++)
            pmm_free_frame((uintptr_t)scratch + p * 4096);
        if (bad)
            return 0;
    }

    /* ============================================================
     * MUTATION PHASE -- every segment, import, and the entry point
     * passed above (compressed streams preflighted), so the copies and
     * PMM/dispatch-table writes below cannot be aborted partway by a
     * validation failure.
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

        if (compressed) {
            if (seg->file_size > 0) {
                /* Decompress straight into the user range. The preflight proved
                 * this exact immutable stream decodes to mem_size, so this
                 * deterministic decode cannot fail; the guard is defence in depth. */
                if (!eif_decompress_segment(data + seg->file_offset, seg->file_size,
                                            (uint8_t *)vaddr, seg->mem_size,
                                            seg->mem_size)) {
                    klog(LOG_ERROR, "eif",
                         "Segment %u: decompress failed after preflight (unreachable)",
                         (uint64_t)i);
                    return 0;
                }
            } else {
                /* Pure-BSS compressed segment: zero-fill, no LZ4. */
                memset((void *)vaddr, 0, seg->mem_size);
            }
        } else {
            /* Uncompressed: copy file bytes, then zero BSS (mem_size > file_size). */
            if (seg->file_size > 0)
                memcpy((void *)vaddr, data + seg->file_offset, seg->file_size);
            if (seg->mem_size > seg->file_size)
                memset((void *)(vaddr + seg->file_size), 0,
                       seg->mem_size - seg->file_size);
        }

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

        /* Metadata diagnostics, deferred past end_ns like every other success
         * log. Only emitted when the binary carries a metadata section. */
        if (hdr->metadata_offset != 0) {
            klog(LOG_DEBUG, "eif",
                 "metadata: name='%s' version='%s' author='%s' min_os='%s'",
                 meta.name, meta.version, meta.author, meta.min_os);
            if (meta.build_id_len) {
                uint64_t bid = 0;
                uint32_t k, kn = meta.build_id_len < 8 ? meta.build_id_len : 8;
                for (k = 0; k < kn; k++)
                    bid |= (uint64_t)meta.build_id[k] << (k * 8);
                klog(LOG_DEBUG, "eif",
                     "metadata: build_id %u bytes, prefix=0x%x",
                     (uint64_t)meta.build_id_len, bid);
            }
        }
        return entry_va;
    }
}
