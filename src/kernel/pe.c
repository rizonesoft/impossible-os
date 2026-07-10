/* ============================================================================
 * pe.c -- PE32+ header parser and loader
 *
 * Validates PE32+ (Windows x64) executables: DOS header, PE signature,
 * COFF header, Optional Header, and section table bounds.
 * Section loading and import resolution are separate passes.
 * ============================================================================ */

#include "kernel/pe.h"
#include "kernel/exec.h"
#include "kernel/errno.h"
#include "kernel/klog.h"
#include "kernel/boot_init.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/nt/service_numbers.h"

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

/* Read a uint64_t from an unaligned buffer position (LE byte order). The
 * zero-copy header pointers can land at an odd address (the file controls
 * e_lfanew and SizeOfOptionalHeader), so every multibyte field -- including the
 * only 64-bit one, ImageBase -- must be byte-assembled rather than dereferenced
 * as a naturally-aligned member (aligned-load UB on strict-alignment targets). */
static uint64_t read_u64(const uint8_t *p)
{
    return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4) << 32);
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
    uint64_t pe_hdr_end = (uint64_t)lfanew + PE_SIGNATURE_SIZE + sizeof(pe_coff_header_t);
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
        (const pe_coff_header_t *)(data + lfanew + PE_SIGNATURE_SIZE);
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
    uint64_t opt_offset = (uint64_t)lfanew + PE_SIGNATURE_SIZE + sizeof(pe_coff_header_t);
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
         read_u64((const uint8_t *)&opt->ImageBase));

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

/* ---- Kernel-side Win32 export tables -------------------------------
 * Each table maps function names to SSDT service numbers. Tables are sorted
 * by name for binary search. Export entries are const -- no mutable state. */

static int pe_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

/* Case-insensitive compare for DLL names (kernel32.dll vs KERNEL32.DLL) */
static int pe_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)(uint8_t)ca - (int)(uint8_t)cb;
        a++; b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

/* kernel32.dll exports (sorted by name).
 *
 * Each entry is a name->SSDT-slot mapping. Per the existing pattern
 * (CreateFileA/W -> SSDT_NtCreateFile), the kernel-side table reserves
 * the syscall slot; the user-mode kernel32 trampoline (the
 * Win32 API surface TODO at todo/10-platform-services/TODO-08-win32-api-surface.md,
 * Console & Process API, not yet shipped) handles ANSI/Wide
 * conversion, GUID-string parsing, error-code mapping, and the
 * SystemFirmwareTableInformation packing for the firmware-table API.
 *
 * Adding a raw name->slot entry here does not by itself produce a
 * working Win32 call: the user-mode trampoline gap is tracked in the
 * Win32 API surface TODO with reciprocal XREFs.
 */
static const pe_export_entry_t s_kernel32_exports[] = {
    { "CloseHandle",                      SSDT_NtClose },
    { "CreateFileA",                      SSDT_NtCreateFile },
    { "CreateFileW",                      SSDT_NtCreateFile },
    { "EnumSystemFirmwareTables",         SSDT_NtQuerySystemInformation },
    { "ExitProcess",                      SSDT_NtTerminateProcess },
    { "GetFirmwareEnvironmentVariableA",  SSDT_NtQuerySystemEnvironmentValueEx },
    { "GetFirmwareEnvironmentVariableW",  SSDT_NtQuerySystemEnvironmentValueEx },
    { "GetLastError",                     SSDT_NtQueryInformationThread },
    { "GetSystemFirmwareTable",           SSDT_NtQuerySystemInformation },
    { "ReadFile",                         SSDT_NtReadFile },
    { "SetFirmwareEnvironmentVariableA",  SSDT_NtSetSystemEnvironmentValueEx },
    { "SetFirmwareEnvironmentVariableW",  SSDT_NtSetSystemEnvironmentValueEx },
    { "SetLastError",                     SSDT_NtSetInformationThread },
    { "WriteFile",                        SSDT_NtWriteFile },
};

/* ntdll.dll exports (sorted by name) */
/* Export names MUST be in strictly ascending order (case-sensitive byte
 * compare) because pe_lookup_export() below does binary search. Adding
 * an entry out of order silently breaks import resolution for every
 * name at or beyond the misplaced entry. The assertion in
 * pe_test_ntdll_exports_sorted() verifies this invariant at boot. */
static const pe_export_entry_t s_ntdll_exports[] = {
    { "NtAcceptConnectPort",         SSDT_NtAcceptConnectPort },
    { "NtAdjustGroupsToken",         SSDT_NtAdjustGroupsToken },
    { "NtAdjustPrivilegesToken",     SSDT_NtAdjustPrivilegesToken },
    { "NtAllocateLocallyUniqueId",   SSDT_NtAllocateLocallyUniqueId },
    { "NtAlpcAcceptConnectPort",     SSDT_NtAlpcAcceptConnectPort },
    { "NtAlpcCancelMessage",         SSDT_NtAlpcCancelMessage },
    { "NtAlpcConnectPort",           SSDT_NtAlpcConnectPort },
    { "NtAlpcConnectPortEx",         SSDT_NtAlpcConnectPortEx },
    { "NtAlpcCreatePort",            SSDT_NtAlpcCreatePort },
    { "NtAlpcCreatePortSection",     SSDT_NtAlpcCreatePortSection },
    { "NtAlpcCreateResourceReserve", SSDT_NtAlpcCreateResourceReserve },
    { "NtAlpcCreateSectionView",     SSDT_NtAlpcCreateSectionView },
    { "NtAlpcDeletePortSection",     SSDT_NtAlpcDeletePortSection },
    { "NtAlpcDeleteResourceReserve", SSDT_NtAlpcDeleteResourceReserve },
    { "NtAlpcDeleteSectionView",     SSDT_NtAlpcDeleteSectionView },
    { "NtAlpcDisconnectPort",        SSDT_NtAlpcDisconnectPort },
    { "NtAlpcQueryInformation",      SSDT_NtAlpcQueryInformation },
    { "NtAlpcQueryInformationMessage", SSDT_NtAlpcQueryInformationMessage },
    { "NtAlpcSendWaitReceivePort",   SSDT_NtAlpcSendWaitReceivePort },
    { "NtAlpcSetInformation",        SSDT_NtAlpcSetInformation },
    { "NtAreMappedFilesTheSame",     SSDT_NtAreMappedFilesTheSame },
    { "NtCancelTimer",               SSDT_NtCancelTimer },
    { "NtClose",                     SSDT_NtClose },
    { "NtCompleteConnectPort",       SSDT_NtCompleteConnectPort },
    { "NtConnectPort",               SSDT_NtConnectPort },
    { "NtCreateDirectoryObject",     SSDT_NtCreateDirectoryObject },
    { "NtCreateFile",                SSDT_NtCreateFile },
    { "NtCreateKey",                 SSDT_NtCreateKey },
    { "NtCreatePort",                SSDT_NtCreatePort },
    { "NtCreateSection",             SSDT_NtCreateSection },
    { "NtCreateSymbolicLinkObject",  SSDT_NtCreateSymbolicLinkObject },
    { "NtCreateTimer",               SSDT_NtCreateTimer },
    { "NtCreateWaitablePort",        SSDT_NtCreateWaitablePort },
    { "NtDeleteKey",                 SSDT_NtDeleteKey },
    { "NtDeleteValueKey",            SSDT_NtDeleteValueKey },
    { "NtEnumerateKey",              SSDT_NtEnumerateKey },
    { "NtEnumerateValueKey",         SSDT_NtEnumerateValueKey },
    { "NtExtendSection",             SSDT_NtExtendSection },
    { "NtFlushKey",                  SSDT_NtFlushKey },
    { "NtGetRandom",                 SSDT_NtGetRandom },
    { "NtImpersonateClientOfPort",   SSDT_NtImpersonateClientOfPort },
    { "NtListenPort",                SSDT_NtListenPort },
    { "NtLoadKey",                   SSDT_NtLoadKey },
    { "NtLoadKeyEx",                 SSDT_NtLoadKeyEx },
    { "NtMapViewOfSection",          SSDT_NtMapViewOfSection },
    { "NtNotifyChangeKey",           SSDT_NtNotifyChangeKey },
    { "NtOpenDirectoryObject",       SSDT_NtOpenDirectoryObject },
    { "NtOpenKey",                   SSDT_NtOpenKey },
    { "NtOpenKeyEx",                 SSDT_NtOpenKeyEx },
    { "NtOpenProcessToken",          SSDT_NtOpenProcessToken },
    { "NtOpenProcessTokenEx",        SSDT_NtOpenProcessTokenEx },
    { "NtOpenSection",               SSDT_NtOpenSection },
    { "NtOpenSymbolicLinkObject",    SSDT_NtOpenSymbolicLinkObject },
    { "NtOpenThreadToken",           SSDT_NtOpenThreadToken },
    { "NtOpenThreadTokenEx",         SSDT_NtOpenThreadTokenEx },
    { "NtOpenTimer",                 SSDT_NtOpenTimer },
    { "NtPrivilegeCheck",            SSDT_NtPrivilegeCheck },
    { "NtQueryDirectoryObject",      SSDT_NtQueryDirectoryObject },
    { "NtQueryInformationToken",     SSDT_NtQueryInformationToken },
    { "NtQueryKey",                  SSDT_NtQueryKey },
    { "NtQuerySection",              SSDT_NtQuerySection },
    { "NtQuerySymbolicLinkObject",   SSDT_NtQuerySymbolicLinkObject },
    { "NtQueryTimer",                SSDT_NtQueryTimer },
    { "NtQueryValueKey",             SSDT_NtQueryValueKey },
    { "NtReadFile",                  SSDT_NtReadFile },
    { "NtReadRequestData",           SSDT_NtReadRequestData },
    { "NtRenameKey",                 SSDT_NtRenameKey },
    { "NtReplyPort",                 SSDT_NtReplyPort },
    { "NtReplyWaitReceivePort",      SSDT_NtReplyWaitReceivePort },
    { "NtReplyWaitReceivePortEx",    SSDT_NtReplyWaitReceivePortEx },
    { "NtRequestPort",               SSDT_NtRequestPort },
    { "NtRequestWaitReplyPort",      SSDT_NtRequestWaitReplyPort },
    { "NtRestoreKey",                SSDT_NtRestoreKey },
    { "NtSaveKey",                   SSDT_NtSaveKey },
    { "NtSaveKeyEx",                 SSDT_NtSaveKeyEx },
    { "NtSecureConnectPort",         SSDT_NtSecureConnectPort },
    { "NtSetInformationToken",       SSDT_NtSetInformationToken },
    { "NtSetTimer",                  SSDT_NtSetTimer },
    { "NtSetTimerEx",                SSDT_NtSetTimerEx },
    { "NtSetValueKey",               SSDT_NtSetValueKey },
    { "NtTerminateProcess",          SSDT_NtTerminateProcess },
    { "NtUnloadKey",                 SSDT_NtUnloadKey },
    { "NtUnloadKeyEx",               SSDT_NtUnloadKeyEx },
    { "NtUnmapViewOfSection",        SSDT_NtUnmapViewOfSection },
    { "NtWriteFile",                 SSDT_NtWriteFile },
    { "NtWriteRequestData",          SSDT_NtWriteRequestData },
};

/* DLL registry -- add new DLLs here */
#define PE_EXPORT_TABLE_COUNT(arr)  (sizeof(arr) / sizeof((arr)[0]))

static const pe_dll_exports_t s_dll_tables[] = {
    { "kernel32.dll", s_kernel32_exports, PE_EXPORT_TABLE_COUNT(s_kernel32_exports) },
    { "ntdll.dll",    s_ntdll_exports,    PE_EXPORT_TABLE_COUNT(s_ntdll_exports) },
};

#define PE_DLL_COUNT  (sizeof(s_dll_tables) / sizeof(s_dll_tables[0]))

/* Binary search for a function name in a sorted export table.
 * Returns SSDT index on success, or (uint32_t)-1 on not found. */
/* Walk every export table and count strict-ordering violations. The
 * binary search in pe_lookup_export() is correct only if each table
 * is sorted ascending by name; one misplaced entry silently breaks
 * import resolution. Exposed so tests (and boot-time gates) can assert
 * the invariant. Returns 0 if all tables are strictly sorted, else
 * the number of violations. */
int pe_exports_sorted_check(void)
{
    uint32_t dll_i;
    int violations = 0;

    for (dll_i = 0; dll_i < PE_DLL_COUNT; dll_i++) {
        const pe_dll_exports_t *dll = &s_dll_tables[dll_i];
        uint32_t i;

        for (i = 1; i < dll->count; i++) {
            if (pe_strcmp(dll->exports[i - 1].name,
                          dll->exports[i].name) >= 0) {
                violations++;
            }
        }
    }
    return violations;
}

static uint32_t pe_lookup_export(const pe_dll_exports_t *dll, const char *name)
{
    uint32_t lo = 0, hi = dll->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int cmp = pe_strcmp(dll->exports[mid].name, name);
        if (cmp < 0)
            lo = mid + 1;
        else if (cmp > 0)
            hi = mid;
        else
            return dll->exports[mid].ssdt_index;
    }
    return (uint32_t)-1;
}

/* Public introspection: resolve an ntdll export name to its SSDT index, or
 * (uint32_t)-1 if the name is not exported. Lets tests and boot-time gates
 * assert that a newly-wired native syscall is actually reachable through the
 * ntdll import surface (not just registered in the SSDT). */
uint32_t pe_ntdll_export_ssdt(const char *name)
{
    uint32_t dll_i;

    for (dll_i = 0; dll_i < PE_DLL_COUNT; dll_i++) {
        if (pe_strcmp(s_dll_tables[dll_i].dll_name, "ntdll.dll") == 0)
            return pe_lookup_export(&s_dll_tables[dll_i], name);
    }
    return (uint32_t)-1;
}

/* Find a DLL export table by name (case-insensitive). */
static const pe_dll_exports_t *pe_find_dll(const char *dll_name)
{
    uint32_t i;
    for (i = 0; i < PE_DLL_COUNT; i++) {
        if (pe_stricmp(s_dll_tables[i].dll_name, dll_name) == 0)
            return &s_dll_tables[i];
    }
    return (const pe_dll_exports_t *)0;
}

/* Stub thunk address for unresolved imports. NULL causes a clean fault
 * on call. A proper stub page returning STATUS_NOT_IMPLEMENTED will be
 * wired when the Win32 subsystem CRT (TODO-07 domain 10) is implemented. */
#define PE_STUB_THUNK_ADDR  0ULL

/* Maximum import descriptors to prevent DoS from malicious PE */
#define PE_MAX_IMPORT_DLLS  64

/* Maximum thunks per DLL to prevent unbounded IAT walks */
#define PE_MAX_THUNKS_PER_DLL  4096

/* Maximum string length for DLL/function names within the image */
#define PE_MAX_NAME_LEN  256

/* Bounded string length: returns length up to max, or max if no NUL found. */
static uint32_t pe_strnlen(const char *s, uint32_t max)
{
    uint32_t i = 0;
    while (i < max && s[i]) i++;
    return i;
}

/* ---- Import resolver ----------------------------------------------- */

/* Resolve imports for one DLL. Returns number of resolved imports, or -1 on error. */
static int pe_resolve_dll_imports(uint64_t image_base, uint32_t size_of_image,
                                  const uint8_t *mapped_image,
                                  const pe_import_descriptor_t *desc)
{
    uint32_t name_rva = read_u32((const uint8_t *)&desc->Name);
    uint32_t oft_rva  = read_u32((const uint8_t *)&desc->OriginalFirstThunk);
    uint32_t ft_rva   = read_u32((const uint8_t *)&desc->FirstThunk);

    /* DLL name must be within image with room for at least 1 char + NUL */
    if (name_rva + 2 > size_of_image) {
        klog(LOG_DEBUG, "pe", "Import DLL name RVA 0x%x outside image",
             (uint64_t)name_rva);
        return -1;
    }

    /* Verify DLL name is NUL-terminated within image bounds */
    uint32_t name_max = size_of_image - name_rva;
    if (name_max > PE_MAX_NAME_LEN) name_max = PE_MAX_NAME_LEN;
    const char *dll_name = (const char *)(mapped_image + name_rva);
    if (pe_strnlen(dll_name, name_max) >= name_max) {
        klog(LOG_DEBUG, "pe", "Import DLL name at RVA 0x%x not NUL-terminated",
             (uint64_t)name_rva);
        return -1;
    }

    const pe_dll_exports_t *dll = pe_find_dll(dll_name);

    /* If INT is zero, use IAT as both source and destination */
    uint32_t int_rva = (oft_rva != 0) ? oft_rva : ft_rva;
    if (int_rva >= size_of_image || ft_rva >= size_of_image) {
        klog(LOG_DEBUG, "pe", "Import thunk RVA outside image for '%s'",
             dll_name);
        return -1;
    }

    uint32_t resolved = 0;
    uint32_t stubbed = 0;
    uint32_t idx;

    for (idx = 0; idx < PE_MAX_THUNKS_PER_DLL; idx++) {
        uint64_t int_offset = int_rva + (uint64_t)idx * 8;
        uint64_t iat_offset = ft_rva + (uint64_t)idx * 8;

        if (int_offset + 8 > size_of_image || iat_offset + 8 > size_of_image)
            break;

        /* Read INT entry (8 bytes, little-endian) */
        const uint8_t *int_ptr = mapped_image + int_offset;
        uint64_t thunk_val = (uint64_t)read_u32(int_ptr) |
                             ((uint64_t)read_u32(int_ptr + 4) << 32);

        /* NULL terminator */
        if (thunk_val == 0)
            break;

        uint32_t ssdt_idx = (uint32_t)-1;

        if (thunk_val & PE_ORDINAL_FLAG64) {
            /* Import by ordinal -- not supported yet, stub it */
            klog(LOG_DEBUG, "pe", "  ordinal import 0x%x -- stubbed",
                 thunk_val & 0xFFFF);
            stubbed++;
        } else {
            /* Import by name: thunk_val is RVA to IMAGE_IMPORT_BY_NAME */
            uint32_t hint_rva = (uint32_t)thunk_val;
            if (hint_rva + 3 >= size_of_image) {
                klog(LOG_DEBUG, "pe", "  hint RVA 0x%x outside image",
                     (uint64_t)hint_rva);
                stubbed++;
            } else {
                /* Verify function name is NUL-terminated within image */
                const char *func_name =
                    (const char *)(mapped_image + hint_rva + 2);
                uint32_t func_max = size_of_image - hint_rva - 2;
                if (func_max > PE_MAX_NAME_LEN) func_max = PE_MAX_NAME_LEN;

                if (pe_strnlen(func_name, func_max) >= func_max) {
                    klog(LOG_DEBUG, "pe",
                         "  func name at hint RVA 0x%x not NUL-terminated",
                         (uint64_t)hint_rva);
                    stubbed++;
                } else {
                    if (dll) {
                        ssdt_idx = pe_lookup_export(dll, func_name);
                        if (ssdt_idx != (uint32_t)-1)
                            resolved++;
                    }

                    if (ssdt_idx == (uint32_t)-1) {
                        klog(LOG_DEBUG, "pe",
                             "  %s!%s -- unresolved, stubbed",
                             dll_name, func_name);
                        stubbed++;
                    }
                }
            }
        }

        /* Write SSDT thunk address into IAT.
         * IAT pages were mapped in, so image_base + iat_offset is valid
         * for any offset within size_of_image (already bounds-checked above).
         * Write via the physical frame address (identity-mapped). */
        uintptr_t iat_phys = vmm_get_physical(
            (uintptr_t)(image_base + iat_offset));
        if (iat_phys == 0) {
            klog(LOG_DEBUG, "pe", "  IAT VA 0x%x not mapped -- skip",
                 image_base + iat_offset);
            continue;
        }

        uint64_t thunk_addr = (ssdt_idx != (uint32_t)-1)
            ? (uint64_t)ssdt_idx
            : PE_STUB_THUNK_ADDR;

        uint8_t *iat_ptr = (uint8_t *)iat_phys;
        iat_ptr[0] = (uint8_t)(thunk_addr);
        iat_ptr[1] = (uint8_t)(thunk_addr >> 8);
        iat_ptr[2] = (uint8_t)(thunk_addr >> 16);
        iat_ptr[3] = (uint8_t)(thunk_addr >> 24);
        iat_ptr[4] = (uint8_t)(thunk_addr >> 32);
        iat_ptr[5] = (uint8_t)(thunk_addr >> 40);
        iat_ptr[6] = (uint8_t)(thunk_addr >> 48);
        iat_ptr[7] = (uint8_t)(thunk_addr >> 56);
    }

    klog(LOG_INFO, "pe", "pe: resolved %u imports from %s (%u stubbed)",
         (uint64_t)resolved, dll_name, (uint64_t)stubbed);

    return (int)resolved;
}

/* Walk the Import Directory and resolve all DLL imports.
 * Returns total resolved count, or -1 on structural error. */
static int pe_resolve_imports(uint64_t image_base, uint32_t size_of_image,
                              uint32_t num_rva_sizes,
                              const pe_optional_header64_t *opt)
{
    if (num_rva_sizes <= PE_DIR_IMPORT)
        return 0;

    uint32_t import_rva = read_u32(
        (const uint8_t *)&opt->DataDirectory[PE_DIR_IMPORT].VirtualAddress);
    uint32_t import_size = read_u32(
        (const uint8_t *)&opt->DataDirectory[PE_DIR_IMPORT].Size);

    if (import_rva == 0 || import_size == 0)
        return 0;

    /* Validate import directory within image */
    uint64_t import_end = (uint64_t)import_rva + import_size;
    if (import_end > size_of_image) {
        klog(LOG_DEBUG, "pe", "Import directory outside image bounds");
        return -1;
    }

    POST16(0xD80E);

    /* The mapped image is at image_base (identity-mapped after mapping) */
    const uint8_t *mapped_image = (const uint8_t *)image_base;

    int total_resolved = 0;
    uint32_t dll_count = 0;

    while (dll_count < PE_MAX_IMPORT_DLLS) {
        /* Bound descriptor walk to import directory, not image */
        uint64_t desc_offset = import_rva +
            (uint64_t)dll_count * sizeof(pe_import_descriptor_t);
        if (desc_offset + sizeof(pe_import_descriptor_t) > import_end)
            break;

        const pe_import_descriptor_t *desc =
            (const pe_import_descriptor_t *)(mapped_image + desc_offset);

        uint32_t desc_name = read_u32((const uint8_t *)&desc->Name);
        uint32_t desc_ft   = read_u32((const uint8_t *)&desc->FirstThunk);

        /* All-zero descriptor terminates the list */
        if (desc_name == 0 && desc_ft == 0)
            break;

        int n = pe_resolve_dll_imports(image_base, size_of_image,
                                       mapped_image, desc);
        if (n < 0) {
            klog(LOG_ERROR, "pe",
                 "Malformed import descriptor %u -- aborting import resolution",
                 (uint64_t)dll_count);
            return -1;
        }
        total_resolved += n;

        dll_count++;
    }

    POST16(0xD80F);

    klog(LOG_INFO, "pe", "pe: import resolution complete -- %u DLLs, %u total resolved",
         (uint64_t)dll_count, (uint64_t)total_resolved);

    return total_resolved;
}

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

/* ---- Section loader ------------------------------------------------
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

    uint64_t image_base = read_u64((const uint8_t *)&v.opt->ImageBase);
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
        klog(LOG_DEBUG, "pe",
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

    /* Validate each section fits within SizeOfImage AND its raw-data
     * range fits within the file. Doing both in 64-bit arithmetic
     * before any copy prevents uint32 wraparound (raw_ptr + raw_sz
     * overflowing past 4 GiB) from smuggling a section whose raw
     * bytes lie outside the input buffer. */
    {
        uint16_t s;
        for (s = 0; s < v.num_sections; s++) {
            uint32_t sec_rva = read_u32(
                (const uint8_t *)&v.sections[s].VirtualAddress);
            uint32_t sec_vsize = read_u32(
                (const uint8_t *)&v.sections[s].VirtualSize);
            uint32_t raw_ptr = read_u32(
                (const uint8_t *)&v.sections[s].PointerToRawData);
            uint32_t raw_sz = read_u32(
                (const uint8_t *)&v.sections[s].SizeOfRawData);
            uint64_t sec_end = (uint64_t)sec_rva + sec_vsize;
            uint64_t raw_end = (uint64_t)raw_ptr + raw_sz;

            if (sec_end < sec_rva || sec_end > size_of_image) {
                klog(LOG_ERROR, "pe",
                     "Section %u RVA 0x%x + VSize 0x%x exceeds SizeOfImage 0x%x",
                     (uint64_t)s, (uint64_t)sec_rva,
                     (uint64_t)sec_vsize, (uint64_t)size_of_image);
                return 0;
            }
            if (raw_sz != 0 && (raw_end < raw_ptr || raw_end > size)) {
                klog(LOG_ERROR, "pe",
                     "Section %u raw 0x%x + 0x%x exceeds file size 0x%x",
                     (uint64_t)s, (uint64_t)raw_ptr,
                     (uint64_t)raw_sz, size);
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

    /* ---- Resolve imports ---- */
    {
        int import_ret = pe_resolve_imports(image_base, size_of_image,
                                            num_rva_sizes, v.opt);
        if (import_ret < 0) {
            klog(LOG_ERROR, "pe",
                 "Import resolution failed -- rolling back %u pages",
                 (uint64_t)total_pages_mapped);
            pe_rollback(image_base, total_pages_mapped);
            return 0;
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

    /* ---- Register module ---- */
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
