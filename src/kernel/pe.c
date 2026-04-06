/* ============================================================================
 * pe.c -- PE32+ header parser and loader
 *
 * Validates PE32+ (Windows x64) executables: DOS header, PE signature,
 * COFF header, Optional Header, and section table bounds.
 * Section loading (§8) and import resolution (§9) are separate passes.
 * ============================================================================ */

#include "kernel/pe.h"
#include "kernel/exec.h"
#include "kernel/errno.h"
#include "kernel/klog.h"
#include "kernel/boot_init.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"

/* ---- Helpers ------------------------------------------------------------ */

/* Read a uint16_t from an unaligned buffer position (LE byte order). */
static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* Read a uint32_t from an unaligned buffer position (LE byte order). */
static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

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
    if (read_u16((const uint8_t *)&dos->e_magic) != PE_DOS_MAGIC) {
        klog(LOG_DEBUG, "pe", "Bad MZ magic: 0x%x",
             (uint64_t)read_u16((const uint8_t *)&dos->e_magic));
        return r;
    }

    uint32_t lfanew = read_u32((const uint8_t *)&dos->e_lfanew);
    if (lfanew < sizeof(pe_dos_header_t)) {
        klog(LOG_DEBUG, "pe", "e_lfanew too small: 0x%x", (uint64_t)lfanew);
        return r;
    }

    /* PE signature (4 bytes) + COFF header (20 bytes) must fit */
    uint64_t pe_hdr_end = (uint64_t)lfanew + 4 + sizeof(pe_coff_header_t);
    if (pe_hdr_end > size) {
        klog(LOG_DEBUG, "pe", "Truncated at PE signature (need %u, have %u)",
             pe_hdr_end, size);
        return r;
    }

    /* ---- PE Signature ---- */
    uint32_t pe_sig = read_u32(data + lfanew);
    if (pe_sig != PE_SIGNATURE) {
        klog(LOG_DEBUG, "pe", "Bad PE signature: 0x%x", (uint64_t)pe_sig);
        return r;
    }

    /* ---- COFF Header ---- */
    const pe_coff_header_t *coff =
        (const pe_coff_header_t *)(data + lfanew + 4);
    uint16_t machine = read_u16((const uint8_t *)&coff->Machine);
    if (machine != PE_MACHINE_AMD64) {
        if (machine == PE_MACHINE_I386) {
            klog(LOG_DEBUG, "pe", "32-bit PE (i386) rejected -- PE32+ only");
        } else {
            klog(LOG_WARN, "pe", "Unsupported Machine: 0x%x",
                 (uint64_t)machine);
        }
        return r;
    }

    uint16_t num_sections = read_u16((const uint8_t *)&coff->NumberOfSections);
    uint16_t opt_hdr_size = read_u16(
        (const uint8_t *)&coff->SizeOfOptionalHeader);

    /* Optional header must be at least the PE32+ size */
    if (opt_hdr_size < sizeof(pe_optional_header64_t)) {
        klog(LOG_DEBUG, "pe",
             "SizeOfOptionalHeader too small: %u (need %u)",
             (uint64_t)opt_hdr_size,
             (uint64_t)sizeof(pe_optional_header64_t));
        return r;
    }

    /* ---- Optional Header ---- */
    uint64_t opt_offset = (uint64_t)lfanew + 4 + sizeof(pe_coff_header_t);
    if (opt_offset + opt_hdr_size > size) {
        klog(LOG_DEBUG, "pe", "Truncated at optional header");
        return r;
    }

    const pe_optional_header64_t *opt =
        (const pe_optional_header64_t *)(data + opt_offset);
    uint16_t magic = read_u16((const uint8_t *)&opt->Magic);

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
         (uint64_t)read_u32((const uint8_t *)&opt->AddressOfEntryPoint),
         opt->ImageBase);

    return r;
}

/* ---- Helpers for identity-mapped copy ----------------------------------- */

extern void *memcpy(void *dst, const void *src, uint64_t n);
extern void *memset(void *s, int c, uint64_t n);

/* Minimum ImageBase to prevent loading over kernel memory.
 * PE64 default is 0x140000000. Anything below 16 MiB is suspicious. */
#define PE_MIN_IMAGE_BASE   0x1000000ULL

/* Maximum number of PE sections (Windows linker typically emits < 20) */
#define PE_MAX_SECTIONS     96

/* Maximum image size: 256 MiB. Rejects pathological PE headers. */
#define PE_MAX_IMAGE_SIZE   (256ULL * 1024 * 1024)

/* ---- Rollback tracking -------------------------------------------------- */

/* Track mapped pages for rollback on failure. Stores VAs so we can
 * unmap + free frames on error. Static limit: SizeOfImage / 4K pages,
 * capped by PE_MAX_IMAGE_SIZE / 4K = 65536 pages max. We track the
 * image base and page count for a range-based unmap instead. */
static void pe_rollback(uint64_t image_base, uint32_t pages_mapped)
{
    uint32_t i;
    for (i = 0; i < pages_mapped; i++) {
        uintptr_t va = (uintptr_t)(image_base + (uint64_t)i * VMM_PAGE_SIZE);
        vmm_unmap_page(va, 1);  /* free_frame = 1: return frame to PMM */
    }
}

/* ---- Section loader (§8) ------------------------------------------------
 *
 * SMP note: PMM and VMM do not currently have spinlocks. This is a
 * pre-existing system-wide limitation (ELF loader has the same issue).
 * PE section loading is called from task_exec, which is serial per-process.
 * PMM/VMM SMP safety is tracked in TODO-22 (kernel bulletproofing). */

/* Map one page: allocate frame, zero, copy data, map at VA.
 * Increments *total_pages on success. Returns 0 or -1. */
static int pe_map_one_page(uint64_t va, const uint8_t *data,
                           uint32_t data_offset, uint32_t copy_len,
                           uint64_t flags, uint32_t *total_pages)
{
    uintptr_t frame = pmm_alloc_frame();
    if (!frame) {
        klog(LOG_ERROR, "pe", "Failed to allocate frame for VA 0x%x", va);
        return -1;
    }

    memset((void *)frame, 0, VMM_PAGE_SIZE);
    if (copy_len > 0 && data)
        memcpy((void *)frame, data + data_offset, copy_len);

    if (vmm_map_page((uintptr_t)va, frame, flags) != 0) {
        klog(LOG_ERROR, "pe", "Failed to map page at VA 0x%x", va);
        /* Frame leaked here -- pmm_free_frame not yet implemented.
         * When it is, free the frame. For now, log the loss. */
        return -1;
    }

    (*total_pages)++;
    return 0;
}

/* ---- Loader entry point (exec dispatcher) ------------------------------- */

uint64_t pe_load(const uint8_t *data, uint64_t size)
{
    POST16(0xD80B);

    pe_validate_result_t v = pe_validate(data, size);
    if (!v.ok) {
        klog(LOG_ERROR, "pe", "PE32+ validation failed");
        return 0;
    }

    uint64_t image_base = v.opt->ImageBase;
    uint32_t size_of_image = read_u32(
        (const uint8_t *)&v.opt->SizeOfImage);
    uint32_t size_of_headers = read_u32(
        (const uint8_t *)&v.opt->SizeOfHeaders);
    uint32_t entry_rva = read_u32(
        (const uint8_t *)&v.opt->AddressOfEntryPoint);
    uint32_t num_rva_sizes = read_u32(
        (const uint8_t *)&v.opt->NumberOfRvaAndSizes);
    uint32_t total_pages_mapped = 0;

    /* ---- Pre-flight validation ---- */

    if (image_base < PE_MIN_IMAGE_BASE) {
        klog(LOG_ERROR, "pe",
             "ImageBase 0x%x below minimum 0x%x -- rejected",
             image_base, (uint64_t)PE_MIN_IMAGE_BASE);
        return 0;
    }

    uint64_t image_end = image_base + size_of_image;
    if (image_end < image_base) {
        klog(LOG_ERROR, "pe", "ImageBase + SizeOfImage overflow");
        return 0;
    }

    if (size_of_image > PE_MAX_IMAGE_SIZE) {
        klog(LOG_ERROR, "pe", "SizeOfImage %u exceeds maximum %u",
             (uint64_t)size_of_image, (uint64_t)PE_MAX_IMAGE_SIZE);
        return 0;
    }

    if (size_of_headers > size_of_image) {
        klog(LOG_ERROR, "pe", "SizeOfHeaders %u > SizeOfImage %u",
             (uint64_t)size_of_headers, (uint64_t)size_of_image);
        return 0;
    }

    if (entry_rva >= size_of_image) {
        klog(LOG_ERROR, "pe",
             "Entry RVA 0x%x outside SizeOfImage 0x%x",
             (uint64_t)entry_rva, (uint64_t)size_of_image);
        return 0;
    }

    if (v.num_sections > PE_MAX_SECTIONS) {
        klog(LOG_ERROR, "pe", "Too many sections: %u (max %u)",
             (uint64_t)v.num_sections, (uint64_t)PE_MAX_SECTIONS);
        return 0;
    }

    /* Validate each section fits within SizeOfImage */
    {
        uint16_t s;
        for (s = 0; s < v.num_sections; s++) {
            uint32_t sec_rva = read_u32(
                (const uint8_t *)&v.sections[s].VirtualAddress);
            uint32_t sec_vsize = read_u32(
                (const uint8_t *)&v.sections[s].VirtualSize);
            uint64_t sec_end = (uint64_t)sec_rva + sec_vsize;

            if (sec_end < sec_rva || sec_end > size_of_image) {
                klog(LOG_ERROR, "pe",
                     "Section %u RVA 0x%x + VSize 0x%x exceeds SizeOfImage 0x%x",
                     (uint64_t)s, (uint64_t)sec_rva,
                     (uint64_t)sec_vsize, (uint64_t)size_of_image);
                return 0;
            }
        }
    }

    uint64_t entry_va = image_base + entry_rva;

    POST16(0xD80C);

    /* ---- Map PE headers at ImageBase ---- */
    {
        uint32_t hdr_pages = (size_of_headers + VMM_PAGE_SIZE - 1) /
                             VMM_PAGE_SIZE;
        uint32_t p;
        for (p = 0; p < hdr_pages; p++) {
            uint32_t offset = p * VMM_PAGE_SIZE;
            uint32_t copy_len = VMM_PAGE_SIZE;
            if (offset + copy_len > size_of_headers)
                copy_len = size_of_headers - offset;
            if (offset + copy_len > size)
                copy_len = (offset < size) ? (uint32_t)(size - offset) : 0;

            if (pe_map_one_page(image_base + offset, data, offset, copy_len,
                                VMM_FLAG_PRESENT | VMM_FLAG_USER,
                                &total_pages_mapped) != 0) {
                pe_rollback(image_base, total_pages_mapped);
                return 0;
            }
        }
    }

    /* ---- Map sections ---- */
    {
        uint16_t s;
        for (s = 0; s < v.num_sections; s++) {
            uint32_t sec_rva = read_u32(
                (const uint8_t *)&v.sections[s].VirtualAddress);
            uint32_t sec_vsize = read_u32(
                (const uint8_t *)&v.sections[s].VirtualSize);
            uint32_t raw_sz = read_u32(
                (const uint8_t *)&v.sections[s].SizeOfRawData);
            uint32_t raw_ptr = read_u32(
                (const uint8_t *)&v.sections[s].PointerToRawData);
            uint32_t chars = read_u32(
                (const uint8_t *)&v.sections[s].Characteristics);

            if (sec_vsize == 0)
                continue;

            uint32_t num_pages = (sec_vsize + VMM_PAGE_SIZE - 1) /
                                 VMM_PAGE_SIZE;

            /* Page flags from Characteristics */
            uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
            if (chars & PE_SCN_MEM_WRITE)
                flags |= VMM_FLAG_WRITABLE;
            if (!(chars & PE_SCN_MEM_EXECUTE))
                flags |= VMM_FLAG_NX;

            uint32_t p;
            for (p = 0; p < num_pages; p++) {
                uint32_t page_off = p * VMM_PAGE_SIZE;
                uint32_t copy_len = 0;

                if (page_off < raw_sz && raw_ptr + page_off < size) {
                    copy_len = VMM_PAGE_SIZE;
                    if (page_off + copy_len > raw_sz)
                        copy_len = raw_sz - page_off;
                    if (raw_ptr + page_off + copy_len > size)
                        copy_len = (uint32_t)(size - raw_ptr - page_off);
                }

                if (pe_map_one_page(image_base + sec_rva + page_off,
                                    data, raw_ptr + page_off, copy_len,
                                    flags, &total_pages_mapped) != 0) {
                    pe_rollback(image_base, total_pages_mapped);
                    return 0;
                }
            }

            /* Log section */
            char name[PE_SECTION_NAME_SIZE + 1];
            uint32_t ni;
            for (ni = 0; ni < PE_SECTION_NAME_SIZE; ni++)
                name[ni] = v.sections[s].Name[ni];
            name[PE_SECTION_NAME_SIZE] = 0;

            klog(LOG_DEBUG, "pe", "Mapped section '%s' at 0x%x (%s%s%s)",
                 name, image_base + sec_rva,
                 (chars & PE_SCN_MEM_READ) ? "R" : "",
                 (chars & PE_SCN_MEM_WRITE) ? "W" : "",
                 (chars & PE_SCN_MEM_EXECUTE) ? "X" : "");
        }
    }

    /* ---- Parse .pdata ---- */
    uint64_t pdata_base = 0;
    uint64_t pdata_size_val = 0;

    if (num_rva_sizes > PE_DIR_EXCEPTION) {
        uint32_t pdata_rva = read_u32(
            (const uint8_t *)&v.opt->DataDirectory[PE_DIR_EXCEPTION].VirtualAddress);
        uint32_t pdata_sz = read_u32(
            (const uint8_t *)&v.opt->DataDirectory[PE_DIR_EXCEPTION].Size);

        /* Validate .pdata range within image */
        if (pdata_rva != 0 && pdata_sz != 0) {
            uint64_t pdata_end = (uint64_t)pdata_rva + pdata_sz;
            if (pdata_end > pdata_rva && pdata_end <= size_of_image) {
                pdata_base = image_base + pdata_rva;
                pdata_size_val = pdata_sz;
                klog(LOG_INFO, "pe",
                     "pe: mapped %u sections, .pdata at 0x%x (%u entries)",
                     (uint64_t)v.num_sections, pdata_base,
                     pdata_sz / 12);
            } else {
                klog(LOG_WARN, "pe",
                     ".pdata RVA 0x%x + size 0x%x outside image -- ignored",
                     (uint64_t)pdata_rva, (uint64_t)pdata_sz);
            }
        }
    }

    if (pdata_base == 0) {
        klog(LOG_INFO, "pe", "pe: mapped %u sections, no .pdata",
             (uint64_t)v.num_sections);
    }

    /* ---- Register module (§6) ---- */
    {
        loaded_module_t mod;
        uint8_t *mp = (uint8_t *)&mod;
        uint32_t mi;
        for (mi = 0; mi < sizeof(mod); mi++) mp[mi] = 0;

        mod.base_address = image_base;
        mod.size_of_image = size_of_image;
        mod.entry_point = entry_va;
        mod.pdata_base = pdata_base;
        mod.pdata_size = pdata_size_val;
        mod.format = EXEC_FMT_PE;

        int reg_ret = exec_register_module((process_t *)0, &mod);
        if (reg_ret != 0) {
            klog(LOG_ERROR, "pe",
                 "Module registration failed -- rolling back %u pages",
                 (uint64_t)total_pages_mapped);
            pe_rollback(image_base, total_pages_mapped);
            return 0;
        }
    }

    POST16(0xD80D);

    klog(LOG_INFO, "pe",
         "PE32+ loaded at 0x%x, entry 0x%x, %u sections, %u pages",
         image_base, entry_va,
         (uint64_t)v.num_sections, (uint64_t)total_pages_mapped);

    return entry_va;
}
