/* ============================================================================
 * pe_validate.c -- PE32+ header and section-table validation
 *
 * Every check the loader makes on file bytes before it maps a page: the DOS,
 * PE, COFF and optional headers (pe_validate) and the image layout and each
 * section's virtual and raw ranges (pe_preflight). Kept apart from pe.c so it
 * builds with nothing but klog, which is what lets tools/fuzz run it on the
 * host under AddressSanitizer.
 * ============================================================================ */

#include "kernel/pe.h"
#include "kernel/errno.h"
#include "kernel/klog.h"
#include "kernel/le_bytes.h"

/* ---- Validation --------------------------------------------------------- */

pe_validate_result_t pe_validate(const uint8_t *data, uint64_t size)
{
    pe_validate_result_t r;
    r.ok = 0;
    r.err = ENOEXEC;
    r.dos = (const pe_dos_header_t *)0;
    r.coff = (const pe_coff_header_t *)0;
    r.opt = (const pe_optional_header64_t *)0;
    r.sections = (const pe_section_header_t *)0;
    r.num_sections = 0;

    if (!data || size < sizeof(pe_dos_header_t)) {
        klog(LOG_DEBUG, "pe", "Too small for DOS header (%u bytes)", size);
        return r;
    }

    /* ---- DOS Header ---- */
    const pe_dos_header_t *dos = (const pe_dos_header_t *)data;
    if (le16_at(PE_FIELD(dos, pe_dos_header_t, e_magic)) != PE_DOS_MAGIC) {
        klog(LOG_DEBUG, "pe", "Bad MZ magic: 0x%x",
             (uint64_t)le16_at(PE_FIELD(dos, pe_dos_header_t, e_magic)));
        return r;
    }

    uint32_t lfanew = le32_at(PE_FIELD(dos, pe_dos_header_t, e_lfanew));
    if (lfanew < sizeof(pe_dos_header_t)) {
        klog(LOG_DEBUG, "pe", "e_lfanew too small: 0x%x", (uint64_t)lfanew);
        return r;
    }

    /* PE signature (4 bytes) + COFF header (20 bytes) must fit */
    uint64_t pe_hdr_end = (uint64_t)lfanew + PE_SIGNATURE_SIZE + sizeof(pe_coff_header_t);
    if (pe_hdr_end > size) {
        klog(LOG_DEBUG, "pe", "Truncated at PE signature (need %u, have %u)",
             pe_hdr_end, size);
        return r;
    }

    /* ---- PE Signature ---- */
    uint32_t pe_sig = le32_at(data + lfanew);
    if (pe_sig != PE_SIGNATURE) {
        klog(LOG_DEBUG, "pe", "Bad PE signature: 0x%x", (uint64_t)pe_sig);
        return r;
    }

    /* ---- COFF Header ---- */
    const pe_coff_header_t *coff =
        (const pe_coff_header_t *)(data + lfanew + PE_SIGNATURE_SIZE);
    uint16_t machine = le16_at(PE_FIELD(coff, pe_coff_header_t, Machine));
    if (machine != PE_MACHINE_AMD64) {
        if (machine == PE_MACHINE_I386) {
            klog(LOG_DEBUG, "pe", "32-bit PE (i386) rejected -- PE32+ only");
        } else {
            klog(LOG_WARN, "pe", "Unsupported Machine: 0x%x",
                 (uint64_t)machine);
        }
        return r;
    }

    uint16_t num_sections = le16_at(PE_FIELD(coff, pe_coff_header_t, NumberOfSections));
    uint16_t opt_hdr_size = le16_at(
        PE_FIELD(coff, pe_coff_header_t, SizeOfOptionalHeader));

    /* Optional header must be at least the PE32+ size */
    if (opt_hdr_size < sizeof(pe_optional_header64_t)) {
        klog(LOG_DEBUG, "pe",
             "SizeOfOptionalHeader too small: %u (need %u)",
             (uint64_t)opt_hdr_size,
             (uint64_t)sizeof(pe_optional_header64_t));
        return r;
    }

    /* ---- Optional Header ---- */
    uint64_t opt_offset = (uint64_t)lfanew + PE_SIGNATURE_SIZE + sizeof(pe_coff_header_t);
    if (opt_offset + opt_hdr_size > size) {
        klog(LOG_DEBUG, "pe", "Truncated at optional header");
        return r;
    }

    const pe_optional_header64_t *opt =
        (const pe_optional_header64_t *)(data + opt_offset);
    uint16_t magic = le16_at(PE_FIELD(opt, pe_optional_header64_t, Magic));

    if (magic == PE_OPT_MAGIC_PE32) {
        klog(LOG_DEBUG, "pe",
             "32-bit Optional Header (0x10B) rejected -- PE32+ only");
        return r;
    }
    if (magic != PE_OPT_MAGIC_PE32PLUS) {
        klog(LOG_DEBUG, "pe", "Bad Optional Header Magic: 0x%x",
             (uint64_t)magic);
        return r;
    }

    /* ---- Section Headers ---- */
    uint64_t sec_offset = opt_offset + opt_hdr_size;
    uint64_t sec_end = sec_offset +
        (uint64_t)num_sections * sizeof(pe_section_header_t);
    if (sec_end > size) {
        klog(LOG_DEBUG, "pe",
             "Truncated at section headers (need %u, have %u)",
             sec_end, size);
        return r;
    }

    const pe_section_header_t *sections =
        (const pe_section_header_t *)(data + sec_offset);

    /* ---- Success ---- */
    r.ok = 1;
    r.err = 0;
    r.dos = dos;
    r.coff = coff;
    r.opt = opt;
    r.sections = sections;
    r.num_sections = num_sections;

    klog(LOG_INFO, "pe",
         "Valid PE32+ -- %u sections, entry RVA 0x%x, ImageBase 0x%x",
         (uint64_t)num_sections,
         (uint64_t)le32_at(PE_FIELD(opt, pe_optional_header64_t, AddressOfEntryPoint)),
         le64_at(PE_FIELD(opt, pe_optional_header64_t, ImageBase)));

    return r;
}

int pe_preflight(const uint8_t *data, uint64_t size, pe_validate_result_t *vout,
                 pe_image_layout_t *out)
{
    pe_validate_result_t v = pe_validate(data, size);
    *vout = v;
    if (!v.ok)
        return -1;

    uint64_t image_base = le64_at(PE_FIELD(v.opt, pe_optional_header64_t, ImageBase));
    uint32_t size_of_image = le32_at(
        PE_FIELD(v.opt, pe_optional_header64_t, SizeOfImage));
    uint32_t size_of_headers = le32_at(
        PE_FIELD(v.opt, pe_optional_header64_t, SizeOfHeaders));
    uint32_t entry_rva = le32_at(
        PE_FIELD(v.opt, pe_optional_header64_t, AddressOfEntryPoint));
    uint32_t num_rva_sizes = le32_at(
        PE_FIELD(v.opt, pe_optional_header64_t, NumberOfRvaAndSizes));

    /* ---- Pre-flight validation ---- */

    if (image_base < PE_MIN_IMAGE_BASE) {
        klog(LOG_DEBUG, "pe",
             "ImageBase 0x%x inside the low identity map (below 0x%x) -- rejected",
             image_base, (uint64_t)PE_MIN_IMAGE_BASE);
        return -1;
    }

    uint64_t image_end = image_base + size_of_image;
    if (image_end < image_base) {
        klog(LOG_ERROR, "pe", "ImageBase + SizeOfImage overflow");
        return -1;
    }

    if (size_of_image > PE_MAX_IMAGE_SIZE) {
        klog(LOG_ERROR, "pe", "SizeOfImage %u exceeds maximum %u",
             (uint64_t)size_of_image, (uint64_t)PE_MAX_IMAGE_SIZE);
        return -1;
    }

    if (size_of_headers > size_of_image) {
        klog(LOG_ERROR, "pe", "SizeOfHeaders %u > SizeOfImage %u",
             (uint64_t)size_of_headers, (uint64_t)size_of_image);
        return -1;
    }

    if (entry_rva >= size_of_image) {
        klog(LOG_ERROR, "pe",
             "Entry RVA 0x%x outside SizeOfImage 0x%x",
             (uint64_t)entry_rva, (uint64_t)size_of_image);
        return -1;
    }

    if (v.num_sections > PE_MAX_SECTIONS) {
        klog(LOG_ERROR, "pe", "Too many sections: %u (max %u)",
             (uint64_t)v.num_sections, (uint64_t)PE_MAX_SECTIONS);
        return -1;
    }

    /* Validate each section fits within SizeOfImage AND its raw-data
     * range fits within the file. Doing both in 64-bit arithmetic
     * before any copy prevents uint32 wraparound (raw_ptr + raw_sz
     * overflowing past 4 GiB) from smuggling a section whose raw
     * bytes lie outside the input buffer. */
    {
        uint16_t s;
        for (s = 0; s < v.num_sections; s++) {
            uint32_t sec_rva = le32_at(
                PE_SECTION_FIELD(v.sections, s, VirtualAddress));
            uint32_t sec_vsize = le32_at(
                PE_SECTION_FIELD(v.sections, s, VirtualSize));
            uint32_t raw_ptr = le32_at(
                PE_SECTION_FIELD(v.sections, s, PointerToRawData));
            uint32_t raw_sz = le32_at(
                PE_SECTION_FIELD(v.sections, s, SizeOfRawData));
            uint64_t sec_end = (uint64_t)sec_rva + (sec_vsize ? sec_vsize : raw_sz);
            uint64_t raw_end = (uint64_t)raw_ptr + raw_sz;

            if (sec_end < sec_rva || sec_end > size_of_image) {
                klog(LOG_ERROR, "pe",
                     "Section %u RVA 0x%x + VSize 0x%x exceeds SizeOfImage 0x%x",
                     (uint64_t)s, (uint64_t)sec_rva,
                     (uint64_t)sec_vsize, (uint64_t)size_of_image);
                return -1;
            }
            if (raw_sz != 0 && (raw_end < raw_ptr || raw_end > size)) {
                klog(LOG_ERROR, "pe",
                     "Section %u raw 0x%x + 0x%x exceeds file size 0x%x",
                     (uint64_t)s, (uint64_t)raw_ptr,
                     (uint64_t)raw_sz, size);
                return -1;
            }
        }
    }

    /* ---- Image layout (PE/COFF spec, Optional Header Windows-Specific
     * Fields and Section Table): SectionAlignment a power of two no smaller
     * than a page, FileAlignment a power of two in [512, 64 KiB] no larger
     * than SectionAlignment, ImageBase a multiple of 64 KiB, and the sections
     * "in ascending order and adjacent", each at a multiple of
     * SectionAlignment. pe_load maps the headers' and every section's whole
     * aligned span, so an accepted image has every page below SizeOfImage
     * mapped and any RVA the file names inside it is readable. Windows refuses
     * an image with a gap the same way. Images with SectionAlignment below a
     * page (the "low alignment" form, file mapped as is) are not loaded. */
    {
        uint32_t sa = le32_at(PE_FIELD(v.opt, pe_optional_header64_t, SectionAlignment));
        uint32_t fa = le32_at(PE_FIELD(v.opt, pe_optional_header64_t, FileAlignment));
        uint64_t next, mapped;
        uint16_t s;

        if (sa < PE_PAGE_SIZE || (sa & (sa - 1)) != 0) {
            klog(LOG_ERROR, "pe", "SectionAlignment 0x%x is not a power of two of at least 0x%x",
                 (uint64_t)sa, (uint64_t)PE_PAGE_SIZE);
            return -1;
        }
        if (fa < 512 || fa > 0x10000 || (fa & (fa - 1)) != 0 || fa > sa) {
            klog(LOG_ERROR, "pe", "FileAlignment 0x%x invalid for SectionAlignment 0x%x",
                 (uint64_t)fa, (uint64_t)sa);
            return -1;
        }
        if ((image_base & 0xFFFFu) != 0) {
            klog(LOG_ERROR, "pe", "ImageBase 0x%x is not a multiple of 64 KiB", image_base);
            return -1;
        }
        mapped = pe_align_up(size_of_image, sa);
        if (mapped > PE_MAX_IMAGE_SIZE || image_base + mapped < image_base) {
            klog(LOG_ERROR, "pe", "Aligned image size 0x%x exceeds the maximum", mapped);
            return -1;
        }
        if (image_base > PE_MAX_IMAGE_END - mapped) {
            klog(LOG_ERROR, "pe", "Image 0x%x+0x%x runs past the user address space",
                 image_base, mapped);
            return -1;
        }
        next = pe_align_up(size_of_headers, sa);
        for (s = 0; s < v.num_sections; s++) {
            uint32_t rva = le32_at(PE_SECTION_FIELD(v.sections, s, VirtualAddress));
            uint32_t span = pe_section_span(v.sections, s);

            if (rva != next) {
                klog(LOG_ERROR, "pe",
                     "Section %u at RVA 0x%x, expected 0x%x: sections must be ascending and adjacent",
                     (uint64_t)s, (uint64_t)rva, next);
                return -1;
            }
            next = pe_align_up((uint64_t)rva + span, sa);
        }
        if (next != mapped) {
            klog(LOG_ERROR, "pe", "Sections end at 0x%x but SizeOfImage rounds to 0x%x",
                 next, mapped);
            return -1;
        }
        out->section_alignment = sa;
        out->mapped_size = (uint32_t)mapped;
    }

    out->image_base = image_base;
    out->size_of_image = size_of_image;
    out->size_of_headers = size_of_headers;
    out->entry_rva = entry_rva;
    out->num_rva_sizes = num_rva_sizes;
    return 0;
}

/* ---- Import table walk ---------------------------------------------- */

/* Bounded string length: the length up to max, or max when no NUL is found. */
static uint32_t pe_strnlen(const char *s, uint32_t max)
{
    uint32_t i = 0;
    while (i < max && s[i]) i++;
    return i;
}

/* Resolve the imports of one descriptor. Returns the number resolved, or -1. */
static int pe_walk_dll_imports(const uint8_t *image, uint32_t size_of_image,
                               const uint8_t *desc, const pe_import_ops_t *ops)
{
    uint32_t name_rva = le32_at(PE_FIELD(desc, pe_import_descriptor_t, Name));
    uint32_t oft_rva  = le32_at(PE_FIELD(desc, pe_import_descriptor_t, OriginalFirstThunk));
    uint32_t ft_rva   = le32_at(PE_FIELD(desc, pe_import_descriptor_t, FirstThunk));
    uint32_t name_max, int_rva, resolved = 0, stubbed = 0, write_failed = 0, idx;
    const char *dll_name;
    const void *dll;

    /* Room for at least one character and the NUL. Subtraction form: the RVA
     * is the file's, and `name_rva + 2` wraps at UINT32_MAX. */
    if (name_rva >= size_of_image || size_of_image - name_rva < 2) {
        klog(LOG_DEBUG, "pe", "Import DLL name RVA 0x%x outside image",
             (uint64_t)name_rva);
        return -1;
    }
    name_max = size_of_image - name_rva;
    if (name_max > PE_MAX_NAME_LEN) name_max = PE_MAX_NAME_LEN;
    dll_name = (const char *)(image + name_rva);
    if (pe_strnlen(dll_name, name_max) >= name_max) {
        klog(LOG_DEBUG, "pe", "Import DLL name at RVA 0x%x not NUL-terminated",
             (uint64_t)name_rva);
        return -1;
    }

    dll = ops->find_dll(ops->ctx, dll_name);

    /* With no INT, the IAT is both the source and the destination. */
    int_rva = (oft_rva != 0) ? oft_rva : ft_rva;
    if (int_rva >= size_of_image || ft_rva >= size_of_image) {
        klog(LOG_DEBUG, "pe", "Import thunk RVA outside image for '%s'", dll_name);
        return -1;
    }

    for (idx = 0; idx < PE_MAX_THUNKS_PER_DLL; idx++) {
        uint64_t int_offset = int_rva + (uint64_t)idx * PE_THUNK64_SIZE;
        uint64_t iat_offset = ft_rva + (uint64_t)idx * PE_THUNK64_SIZE;
        uint64_t thunk_val;
        uint32_t ssdt_idx = (uint32_t)-1;

        if (int_offset + PE_THUNK64_SIZE > size_of_image ||
            iat_offset + PE_THUNK64_SIZE > size_of_image)
            break;
        thunk_val = le64_at(image + int_offset);
        if (thunk_val == 0)
            break;

        /* No per-thunk klog: a file can present PE_MAX_THUNKS_PER_DLL thunks
         * in each of PE_MAX_IMPORT_DLLS descriptors, and a synchronous serial
         * line per thunk would stall exec for minutes. Outcomes are summed. */
        if (thunk_val & PE_ORDINAL_FLAG64) {
            stubbed++;   /* import by ordinal is not resolved yet */
        } else {
            /* Import by name: the low 32 bits are the RVA of IMAGE_IMPORT_BY_NAME. */
            uint32_t hint_rva = (uint32_t)thunk_val;
            if (hint_rva >= size_of_image || size_of_image - hint_rva < 3) {
                stubbed++;
            } else {
                /* The 2-byte Hint precedes the name; the >= 3 check above
                 * keeps this subtraction from underflowing. */
                const char *func = (const char *)(image + hint_rva + PE_IMPORT_HINT_SIZE);
                uint32_t func_max = size_of_image - hint_rva - PE_IMPORT_HINT_SIZE;
                if (func_max > PE_MAX_NAME_LEN) func_max = PE_MAX_NAME_LEN;
                if (pe_strnlen(func, func_max) < func_max && dll)
                    ssdt_idx = ops->lookup(ops->ctx, dll, func);
                if (ssdt_idx != (uint32_t)-1) resolved++;
                else stubbed++;
            }
        }

        /* An unmapped IAT page means the import was not installed: a write
         * failure, never a silent success. */
        if (!ops->write_iat(ops->ctx, (uint32_t)iat_offset,
                            ssdt_idx != (uint32_t)-1 ? (uint64_t)ssdt_idx
                                                     : PE_STUB_THUNK_ADDR))
            write_failed++;
    }

    if (write_failed) {
        klog(LOG_WARN, "pe",
             "pe: resolved %u imports from %s (%u stubbed, %u IAT writes failed)",
             (uint64_t)resolved, dll_name, (uint64_t)stubbed, (uint64_t)write_failed);
        return -1;
    }
    klog(LOG_INFO, "pe", "pe: resolved %u imports from %s (%u stubbed)",
         (uint64_t)resolved, dll_name, (uint64_t)stubbed);
    return (int)resolved;
}

int pe_walk_imports(const uint8_t *image, uint32_t size_of_image,
                    uint32_t import_rva, uint32_t import_size,
                    const pe_import_ops_t *ops, uint32_t *dlls_out)
{
    uint64_t import_end;
    uint32_t dll_count = 0;
    int total = 0;

    if (dlls_out) *dlls_out = 0;
    if (import_rva == 0 || import_size == 0)
        return 0;
    import_end = (uint64_t)import_rva + import_size;
    if (import_end > size_of_image) {
        klog(LOG_DEBUG, "pe", "Import directory outside image bounds");
        return -1;
    }

    while (dll_count < PE_MAX_IMPORT_DLLS) {
        /* The descriptor walk is bounded by the directory, not the image. */
        uint64_t off = import_rva + (uint64_t)dll_count * sizeof(pe_import_descriptor_t);
        const uint8_t *desc;
        int n;

        if (off + sizeof(pe_import_descriptor_t) > import_end)
            break;
        desc = image + off;
        /* An all-zero Name and FirstThunk end the list. */
        if (le32_at(PE_FIELD(desc, pe_import_descriptor_t, Name)) == 0 &&
            le32_at(PE_FIELD(desc, pe_import_descriptor_t, FirstThunk)) == 0)
            break;
        n = pe_walk_dll_imports(image, size_of_image, desc, ops);
        if (n < 0) {
            klog(LOG_ERROR, "pe",
                 "Malformed import descriptor %u -- aborting import resolution",
                 (uint64_t)dll_count);
            return -1;
        }
        total += n;
        dll_count++;
    }

    klog(LOG_INFO, "pe", "pe: import resolution complete -- %u DLLs, %u total resolved",
         (uint64_t)dll_count, (uint64_t)total);
    if (dlls_out) *dlls_out = dll_count;
    return total;
}
