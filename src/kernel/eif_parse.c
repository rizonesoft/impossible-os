/* ============================================================================
 * eif_parse.c -- EIF header, segment, import, entry and metadata validation
 *
 * Every decision the EIF loader takes from file bytes, ahead of any write:
 * eif_check runs the header gate, the segment table rules (canonical
 * placement, ascending non-overlapping ranges, user range, dispatch-table
 * hole), the import table, the entry point and the metadata records. It writes
 * nothing but its result. Import availability is asked of a caller-supplied
 * predicate (the loader passes the SSDT lookup), so this file builds for the
 * host with nothing but klog and LZ4, and tools/fuzz runs it under
 * AddressSanitizer. Spec: specs/eif-format.md
 * ============================================================================ */

#include "kernel/eif.h"
#include "kernel/elf.h"
#include "kernel/klog.h"
#include "kernel/mm/user_range.h"
#include "libs/lz4.h"

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

/* ---- Validation --------------------------------------------------------- */

/* True when `flags` sets only bits the loader understands (EIF_FLAG_KNOWN_MASK).
 * The single decision point for the fail-closed unknown-flag rule so producers
 * and the loader share one predicate; eif_validate rejects when this is false. */
int eif_flags_known(uint32_t flags)
{
    return (flags & ~EIF_FLAG_KNOWN_MASK) == 0;
}

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

    /* API-version gate (spec rule 4): api_version is the MINIMUM OS API version
     * the binary requires. Reject a binary that needs a NEWER API than the OS
     * provides; version 0 (unset) and any value <= current stay compatible. */
    if (hdr->api_version > EIF_CURRENT_API_VERSION) {
        klog(LOG_DEBUG, "eif", "binary requires API version %u, OS provides %u",
             (uint64_t)hdr->api_version, (uint64_t)EIF_CURRENT_API_VERSION);
        return 0;
    }
    /* No accept-path log here: eif_validate() runs inside eif_load's timed
     * region (start_ns..end_ns), and klog's spinlock + synchronous serial I/O
     * would inflate the <10 us load budget the region measures. The aggregate
     * success record after the timer stops already reports a successful load. */

    /* Reject any flag bit the loader does not understand (fail-closed). This
     * keeps a newer producer's flag from being silently ignored by this loader:
     * adding a flag is a coordinated producer+loader change. */
    if (!eif_flags_known(hdr->flags)) {
        klog(LOG_DEBUG, "eif", "Unknown header flag bits 0x%x",
             (uint64_t)(hdr->flags & ~EIF_FLAG_KNOWN_MASK));
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

/* ---- Metadata parser ---------------------------------------------------- */

/* Copy a length-prefixed (NOT NUL-terminated) wire value into a fixed-size,
 * always-NUL-terminated field, truncating if the value does not fit. The fields
 * are display/identity hints, so truncation is acceptable (not a security
 * surface); cap is a compile-time sizeof(), always >= 16, so cap - 1 is safe. */
static void meta_copy_str(char *dst, uint32_t cap,
                          const uint8_t *val, uint32_t val_len)
{
    uint32_t n = (val_len < cap - 1) ? val_len : cap - 1;
    if (n)
        memcpy(dst, val, n);
    dst[n] = 0;
}

/* True when a length-prefixed wire key equals the NUL-terminated literal. */
static int meta_key_is(const uint8_t *key, uint32_t key_len, const char *lit)
{
    uint32_t i;
    for (i = 0; i < key_len; i++) {
        if (lit[i] == 0 || key[i] != (uint8_t)lit[i])
            return 0;
    }
    return lit[key_len] == 0;
}

int eif_parse_metadata(const uint8_t *data, uint64_t size,
                       const eif_header_t *hdr, eif_metadata_t *out)
{
    uint64_t cursor, meta_end, remaining;
    uint32_t records = 0;

    memset(out, 0, sizeof(*out));

    if (hdr->metadata_offset == 0)
        return 1;   /* no metadata section: valid, all-zero out */

    /* Range end mirrors eif_validate's metadata bounds (spec rule 8): the signed
     * boundary for a signed file, else EOF. metadata_offset was already validated
     * into [sizeof(header), meta_end), so the cursor starts strictly in range. */
    meta_end = (hdr->flags & EIF_FLAG_SIGNED) ? hdr->signature_offset : size;
    cursor = hdr->metadata_offset;

    while (cursor < meta_end) {
        uint32_t key_len, val_len;
        const uint8_t *key, *val;

        /* key_len: remaining-length check before every length read avoids any
         * cursor + width addition that could wrap; 1-3 trailing bytes here are
         * a malformed record, not a silent stop. */
        remaining = meta_end - cursor;
        if (remaining < 4) {
            klog(LOG_DEBUG, "eif", "Metadata: truncated key length prefix");
            return 0;
        }
        key_len = eif_read32(data + cursor);
        cursor += 4;
        remaining -= 4;

        if (key_len == 0)
            return 1;   /* terminator record: parse complete */

        if (++records > EIF_MAX_METADATA_RECORDS) {
            klog(LOG_DEBUG, "eif", "Metadata: too many records (max %u)",
                 (uint64_t)EIF_MAX_METADATA_RECORDS);
            return 0;
        }

        if (key_len > remaining) {
            klog(LOG_DEBUG, "eif", "Metadata: key runs past range end");
            return 0;
        }
        key = data + cursor;
        cursor += key_len;
        remaining -= key_len;

        if (remaining < 4) {
            klog(LOG_DEBUG, "eif", "Metadata: truncated value length prefix");
            return 0;
        }
        val_len = eif_read32(data + cursor);
        cursor += 4;
        remaining -= 4;

        if (val_len > remaining) {
            klog(LOG_DEBUG, "eif", "Metadata: value runs past range end");
            return 0;
        }
        val = data + cursor;
        cursor += val_len;

        /* Map known keys into the decode struct; unknown keys are skipped so a
         * newer producer's extra keys do not break an older loader (forward-compat). */
        if (meta_key_is(key, key_len, "name"))
            meta_copy_str(out->name, sizeof(out->name), val, val_len);
        else if (meta_key_is(key, key_len, "version"))
            meta_copy_str(out->version, sizeof(out->version), val, val_len);
        else if (meta_key_is(key, key_len, "author"))
            meta_copy_str(out->author, sizeof(out->author), val, val_len);
        else if (meta_key_is(key, key_len, "min_os"))
            meta_copy_str(out->min_os, sizeof(out->min_os), val, val_len);
        else if (meta_key_is(key, key_len, "build_id")) {
            uint32_t n = (val_len < sizeof(out->build_id))
                       ? val_len : (uint32_t)sizeof(out->build_id);
            if (n)
                memcpy(out->build_id, val, n);
            out->build_id_len = n;
        }
    }

    /* Reached the range end with no key_len == 0 terminator. Spec rule 8 allows
     * termination by the range end OR the terminator, whichever comes first. */
    return 1;
}

/* ---- Compressed-segment decompression ----------------------------------- */

/* Decompress one compressed segment's stream (src_size compressed bytes) into
 * dst (capacity dst_cap) and confirm it produces EXACTLY `expect` bytes. Pure:
 * bounded by LZ4_decompress_safe (never reads/writes outside the buffers even on
 * hostile input), writes only into dst, touches no user memory or global state.
 * Returns 1 on success, 0 on a corrupt stream or a decompressed size that is not
 * exactly `expect`. src_size == 0 means "no compressed payload" and succeeds iff
 * expect == 0. This is the preflight primitive: eif_load runs it against a
 * scratch buffer for every compressed segment BEFORE the mutation phase, so a
 * corrupt or size-lying stream rejects the load with no vaddr write, preserving
 * the "mutation cannot be aborted partway" invariant. */
int eif_decompress_segment(const uint8_t *src, uint32_t src_size,
                           uint8_t *dst, uint32_t dst_cap, uint32_t expect)
{
    int produced;
    if (src_size == 0)
        return expect == 0;
    if (dst_cap < expect)
        return 0;
    produced = lz4_decompress(src, src_size, dst, dst_cap);
    return produced >= 0 && (uint32_t)produced == expect;
}

/* ---- Whole-image check ------------------------------------------------- */

int eif_check(const uint8_t *data, uint64_t size,
              int (*import_available)(uint32_t syscall_id),
              eif_check_result_t *out)
{
    const eif_header_t *hdr;
    uint32_t i;
    int compressed;
    uint64_t max_decomp = 0;

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

    /* EIF_FLAG_COMPRESSED (spec: per-file flag). When set, every segment's file
     * bytes are an LZ4 stream: file_size is the compressed length, mem_size the
     * exact decompressed extent (no implicit BSS). Decompression is deferred to
     * the preflight below (validation phase) so a corrupt stream rejects the load
     * before any user-memory write. */
    compressed = (hdr->flags & EIF_FLAG_COMPRESSED) ? 1 : 0;

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

            /* Segment size rule is flag-conditional:
             *  - Uncompressed: file_size <= mem_size (BSS = mem_size - file_size).
             *  - Compressed: file_size is the COMPRESSED length, mem_size the
             *    exact decompressed extent, so file_size may exceed mem_size for
             *    small/incompressible data. A non-empty compressed payload must
             *    produce output (reject mem_size == 0). A file_size==0 compressed
             *    segment is a pure-BSS region (zero-filled, no LZ4). Track the
             *    largest decompressed segment to size the preflight scratch. */
            if (compressed) {
                if (seg->file_size > 0 && seg->mem_size == 0) {
                    klog(LOG_ERROR, "eif",
                         "Segment %u: compressed payload but mem_size 0", (uint64_t)i);
                    return 0;
                }
                if (seg->file_size > 0 && seg->mem_size > max_decomp)
                    max_decomp = seg->mem_size;
            } else if (seg->file_size > seg->mem_size) {
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
            if (!import_available(imp->syscall_id) &&
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

    /* ---- Parse metadata (still validation phase: reject before mutation) ----
     * A malformed metadata record (length past range, over the record cap) must
     * reject the load BEFORE the segment copies below touch user memory. The
     * parser is pure and emits no success log; the parsed name/version/author is
     * logged after end_ns with the aggregate record (klog-timing discipline). A
     * reject klog inside the parser is free here -- this path returns before
     * end_ns is captured, so no timing is ever reported for it. */
    if (!eif_parse_metadata(data, size, hdr, &out->meta)) {
        klog(LOG_DEBUG, "eif", "Malformed metadata section; rejecting binary");
        return 0;
    }

    out->compressed = compressed;
    out->max_decomp = max_decomp;
    return 1;
}
