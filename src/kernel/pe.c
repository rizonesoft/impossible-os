/* ============================================================================
 * pe.c -- PE32+ header parser and loader
 *
 * Validates PE32+ (Windows x64) executables: DOS header, PE signature,
 * COFF header, Optional Header, and section table bounds.
 * Section loading (§8) and import resolution (§9) are separate passes.
 * ============================================================================ */

#include "kernel/pe.h"
#include "kernel/errno.h"
#include "kernel/klog.h"
#include "kernel/boot_init.h"

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
            klog(LOG_WARN, "pe", "32-bit PE (i386) rejected -- PE32+ only");
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
        klog(LOG_WARN, "pe",
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

/* ---- Loader entry point (exec dispatcher) ------------------------------- */

uint64_t pe_load(const uint8_t *data, uint64_t size)
{
    POST16(0xD80B);

    pe_validate_result_t v = pe_validate(data, size);
    if (!v.ok) {
        klog(LOG_ERROR, "pe", "PE32+ validation failed");
        return 0;
    }

    /* §7 scope: validation and format recognition only.
     * Section loading, import resolution, and actual execution are §8/§9.
     * Return 0 (failure) until the section loader is implemented --
     * returning a fake entry point would cause task_exec to jump to
     * unmapped memory. The PE format is registered so exec_load()
     * recognizes MZ magic and logs the correct format name. */
    klog(LOG_WARN, "pe",
         "PE32+ validated but section loader not yet implemented (§8)");
    return 0;
}
