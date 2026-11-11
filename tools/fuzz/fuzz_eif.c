/* libFuzzer harness: EIF header, segment, import, entry and metadata checks.
 *
 * Input is a whole EIF executable. eif_check (src/kernel/eif_parse.c) is the
 * loader's whole validation phase; for a compressed image the loader then
 * decodes each segment into scratch before writing anything, which this
 * harness repeats with eif_decompress_segment and the kernel's own LZ4.
 * Contract under test: no input makes either read outside the buffer or write
 * past the scratch capacity, and every segment of an accepted image lies in
 * the file and the user range.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/eif.h"
#include "kernel/mm/user_range.h"

/* The SSDT is not on the host. Odd service numbers count as registered, so
 * both the required-import accept and reject paths are reachable. */
static int import_available(uint32_t id)
{
    return (id & 1u) != 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    eif_check_result_t chk;
    eif_header_t h;

    if (!eif_check(data, size, import_available, &chk))
        return 0;

    memcpy(&h, data, sizeof(h));
    for (uint32_t i = 0; i < h.segment_count; i++) {
        eif_segment_t s;
        uint64_t va;

        memcpy(&s, data + h.segment_offset + (uint64_t)i * sizeof(s), sizeof(s));
        va = h.load_base + s.vaddr;
        if (va < USER_ELF_BASE || s.mem_size > USER_ELF_END - va)
            abort();
        if (s.file_size > 0 &&
            ((uint64_t)s.file_offset + s.file_size > size))
            abort();
        if (chk.compressed && s.file_size > 0) {
            /* The loader's scratch: max_decomp rounded up to a page. */
            uint32_t cap = (uint32_t)((chk.max_decomp + 4095) & ~4095ull);
            uint8_t *scratch = malloc(cap ? cap : 1);
            if (!scratch)
                return 0;
            (void)eif_decompress_segment(data + s.file_offset, s.file_size,
                                         scratch, cap, s.mem_size);
            free(scratch);
        }
    }
    return 0;
}
