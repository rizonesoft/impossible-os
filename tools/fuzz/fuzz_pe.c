/* libFuzzer harness: PE32+ headers, section table and import table.
 *
 * Input is a whole executable as exec reads it from disk. pe_preflight is the
 * loader's own gate (src/kernel/pe_validate.c): every check it makes before a
 * page is mapped. An accepted image is then laid out the way pe_load maps it,
 * page by page, with every page no header or section covers left inaccessible
 * (PROT_NONE), as an unmapped user page is in the kernel, and pe_walk_imports
 * fills its IAT. The contract: no input reads outside the buffer or touches a
 * page the loader did not map, an accepted layout is inside the limits the
 * mapping code trusts, and every IAT write lands inside the image.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "kernel/pe.h"

#define PAGE 4096u

static uint32_t g_size_of_image;
static uint8_t *g_image;

static const void *find_dll(void *ctx, const char *name)
{
    (void)ctx;
    if (strlen(name) >= PE_MAX_NAME_LEN)
        abort();
    return name[0] == 'k' ? (const void *)name : (const void *)0;
}

static uint32_t lookup(void *ctx, const void *dll, const char *func)
{
    (void)ctx; (void)dll;
    if (strlen(func) >= PE_MAX_NAME_LEN)
        abort();
    return (strlen(func) & 1u) ? (uint32_t)-1 : 7u;
}

/* Store the entry into the image, as the kernel does, so later reads of the
 * walk see it (an IAT may overlap the INT, a descriptor or a name). Every page
 * below SizeOfImage is mapped in an accepted image, so the write cannot fault. */
static int write_iat(void *ctx, uint32_t rva, uint64_t value)
{
    uint32_t b;

    (void)ctx;
    if ((uint64_t)rva + PE_THUNK64_SIZE > g_size_of_image)
        abort();
    for (b = 0; b < PE_THUNK64_SIZE; b++)
        g_image[rva + b] = (uint8_t)(value >> (b * 8));
    return 1;
}

static void map_range(uint8_t *img, uint32_t mapped_len, uint32_t rva,
                      uint32_t len, const uint8_t *src, uint32_t src_len)
{
    uint32_t first = rva / PAGE * PAGE;
    uint64_t end = (uint64_t)rva + len;

    if (len == 0)
        return;
    if (end > mapped_len)
        abort();   /* a span past the aligned image: the validator let it through */
    if (end > first)
        mprotect(img + first, end - first, PROT_READ | PROT_WRITE);
    if (src_len > len) src_len = len;
    if (src_len)
        memcpy(img + rva, src, src_len);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    pe_validate_result_t v;
    pe_image_layout_t lay;
    pe_import_ops_t ops = { find_dll, lookup, write_iat, (void *)0 };
    uint64_t mapped_len;
    uint32_t dlls, rva, dsz;
    uint16_t s;
    int n;

    if (pe_preflight(data, size, &v, &lay) != 0)
        return 0;

    /* What pe_load relies on once pe_preflight returns 0. */
    if (lay.image_base < PE_MIN_IMAGE_BASE ||
        lay.size_of_image > PE_MAX_IMAGE_SIZE ||
        lay.size_of_headers > lay.size_of_image ||
        lay.entry_rva >= lay.size_of_image ||
        v.num_sections > PE_MAX_SECTIONS)
        abort();
    if ((const uint8_t *)v.sections < data ||
        (const uint8_t *)v.sections + (size_t)v.num_sections * sizeof(pe_section_header_t) > data + size)
        abort();
    if (lay.num_rva_sizes <= PE_DIR_IMPORT)
        return 0;

    /* Lay the image out as pe_load maps it: the headers' and each section's
     * aligned span, read here from the headers rather than from lay, so a
     * layout the validator wrongly accepts shows up as a PROT_NONE read. */
    {
        uint32_t sa;
        memcpy(&sa, PE_FIELD(v.opt, pe_optional_header64_t, SectionAlignment), 4);
        if (sa == 0 || (sa & (sa - 1)) != 0 || sa < PAGE)
            abort();   /* pe_preflight must have refused it */
        mapped_len = pe_align_up(lay.size_of_image, sa);
        if (mapped_len != lay.mapped_size)
            abort();
        g_image = mmap(NULL, mapped_len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_image == MAP_FAILED)
            return 0;
        g_size_of_image = lay.size_of_image;
        map_range(g_image, (uint32_t)mapped_len, 0,
                  (uint32_t)pe_align_up(lay.size_of_headers, sa), data,
                  lay.size_of_headers < size ? lay.size_of_headers : (uint32_t)size);
        for (s = 0; s < v.num_sections; s++) {
            const uint8_t *h = (const uint8_t *)v.sections + (size_t)s * sizeof(pe_section_header_t);
            uint32_t sec_rva, vsize, raw_ptr, raw_sz, avail, span;

            memcpy(&sec_rva, h + __builtin_offsetof(pe_section_header_t, VirtualAddress), 4);
            memcpy(&vsize, h + __builtin_offsetof(pe_section_header_t, VirtualSize), 4);
            memcpy(&raw_ptr, h + __builtin_offsetof(pe_section_header_t, PointerToRawData), 4);
            memcpy(&raw_sz, h + __builtin_offsetof(pe_section_header_t, SizeOfRawData), 4);
            span = vsize ? vsize : raw_sz;
            if (span == 0)
                continue;
            avail = raw_ptr < size ? (uint32_t)(size - raw_ptr) : 0;
            if (raw_sz < avail) avail = raw_sz;
            if (span < avail) avail = span;
            map_range(g_image, (uint32_t)mapped_len, sec_rva,
                      (uint32_t)(pe_align_up((uint64_t)sec_rva + span, sa) - sec_rva),
                      data + (raw_ptr < size ? raw_ptr : 0), avail);
        }
    }

    memcpy(&rva, PE_DATA_DIR_FIELD(v.opt, PE_DIR_IMPORT, VirtualAddress), 4);
    memcpy(&dsz, PE_DATA_DIR_FIELD(v.opt, PE_DIR_IMPORT, Size), 4);
    n = pe_walk_imports(g_image, lay.size_of_image, rva, dsz, &ops, &dlls);
    if (n > 0 && dlls == 0)
        abort();
    munmap(g_image, mapped_len);
    return 0;
}
