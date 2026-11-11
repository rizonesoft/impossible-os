/* ============================================================================
 * pe.c -- PE32+ header parser and loader
 *
 * Loads PE32+ (Windows x64) executables for the exec dispatcher: maps the
 * headers and sections pe_preflight (pe_validate.c) accepted, resolves imports
 * through pe_walk_imports, parses .pdata and registers the module.
 * ============================================================================ */

#include "kernel/pe.h"
#include "kernel/le_bytes.h"
#include "kernel/exec.h"
#include "kernel/errno.h"
#include "kernel/klog.h"
#include "kernel/boot_init.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/nt/service_numbers.h"

/* ---- Helpers for identity-mapped copy ----------------------------------- */

extern void *memcpy(void *dst, const void *src, uint64_t n);
extern void *memset(void *s, int c, uint64_t n);

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
    { "NtAssignProcessToJobObject",  SSDT_NtAssignProcessToJobObject },
    { "NtCancelTimer",               SSDT_NtCancelTimer },
    { "NtClose",                     SSDT_NtClose },
    { "NtCompleteConnectPort",       SSDT_NtCompleteConnectPort },
    { "NtConnectPort",               SSDT_NtConnectPort },
    { "NtCreateDirectoryObject",     SSDT_NtCreateDirectoryObject },
    { "NtCreateFile",                SSDT_NtCreateFile },
    { "NtCreateJobObject",           SSDT_NtCreateJobObject },
    { "NtCreateJobSet",              SSDT_NtCreateJobSet },
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
    { "NtIsProcessInJob",            SSDT_NtIsProcessInJob },
    { "NtListenPort",                SSDT_NtListenPort },
    { "NtLoadKey",                   SSDT_NtLoadKey },
    { "NtLoadKeyEx",                 SSDT_NtLoadKeyEx },
    { "NtMapViewOfSection",          SSDT_NtMapViewOfSection },
    { "NtNotifyChangeKey",           SSDT_NtNotifyChangeKey },
    { "NtOpenDirectoryObject",       SSDT_NtOpenDirectoryObject },
    { "NtOpenJobObject",             SSDT_NtOpenJobObject },
    { "NtOpenKey",                   SSDT_NtOpenKey },
    { "NtOpenKeyEx",                 SSDT_NtOpenKeyEx },
    { "NtOpenProcessToken",          SSDT_NtOpenProcessToken },
    { "NtOpenProcessTokenEx",        SSDT_NtOpenProcessTokenEx },
    { "NtOpenSection",               SSDT_NtOpenSection },
    { "NtOpenSymbolicLinkObject",    SSDT_NtOpenSymbolicLinkObject },
    { "NtOpenThreadToken",           SSDT_NtOpenThreadToken },
    { "NtOpenThreadTokenEx",         SSDT_NtOpenThreadTokenEx },
    { "NtOpenTimer",                 SSDT_NtOpenTimer },
    { "NtPledge",                    SSDT_NtPledge },
    { "NtPrivilegeCheck",            SSDT_NtPrivilegeCheck },
    { "NtQueryDirectoryObject",      SSDT_NtQueryDirectoryObject },
    { "NtQueryInformationJobObject", SSDT_NtQueryInformationJobObject },
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
    { "NtSetInformationJobObject",   SSDT_NtSetInformationJobObject },
    { "NtSetInformationToken",       SSDT_NtSetInformationToken },
    { "NtSetTimer",                  SSDT_NtSetTimer },
    { "NtSetTimerEx",                SSDT_NtSetTimerEx },
    { "NtSetValueKey",               SSDT_NtSetValueKey },
    { "NtTerminateJobObject",        SSDT_NtTerminateJobObject },
    { "NtTerminateProcess",          SSDT_NtTerminateProcess },
    { "NtUnloadKey",                 SSDT_NtUnloadKey },
    { "NtUnloadKeyEx",               SSDT_NtUnloadKeyEx },
    { "NtUnmapViewOfSection",        SSDT_NtUnmapViewOfSection },
    { "NtUnveil",                    SSDT_NtUnveil },
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

/* ---- Import resolver ----------------------------------------------- */

/* Write an 8-byte LE value into an IAT entry at user VA `va`. The PE/COFF spec
 * does NOT require the ILT/IAT to be 8-aligned, so an entry may straddle a 4 KiB
 * page boundary; user pages are per-page frames whose virtual neighbours are NOT
 * physical neighbours, so a naive 8-byte write off the first byte's physical
 * address could corrupt an unrelated frame. Translate page-aware: the common
 * (page-contained) case is one translation; a straddling entry pre-translates
 * every byte's frame and aborts before any write if a covered page is unmapped,
 * so a failed entry is never left partially written. Returns 1 on success, 0 if
 * any covered page is unmapped. */
static int pe_write_iat_entry(uint64_t va, uint64_t value)
{
    int b;

    if ((va & (VMM_PAGE_SIZE - 1)) <= (VMM_PAGE_SIZE - PE_THUNK64_SIZE)) {
        uint8_t *p = (uint8_t *)vmm_get_physical((uintptr_t)va);
        if (p == (uint8_t *)0)
            return 0;
        for (b = 0; b < PE_THUNK64_SIZE; b++)
            p[b] = (uint8_t)(value >> (b * 8));
        return 1;
    }

    {
        uintptr_t phys[PE_THUNK64_SIZE];
        for (b = 0; b < PE_THUNK64_SIZE; b++) {
            phys[b] = vmm_get_physical((uintptr_t)(va + b));
            if (phys[b] == 0)
                return 0;
        }
        for (b = 0; b < PE_THUNK64_SIZE; b++)
            *(uint8_t *)phys[b] = (uint8_t)(value >> (b * 8));
    }
    return 1;
}

/* The import walk itself (every bound on the file's descriptors, thunks and
 * names) is pe_walk_imports in pe_validate.c, which the host fuzzer runs; the
 * loader supplies the export tables and the page-aware IAT write. */

static const void *pe_import_find_dll(void *ctx, const char *dll_name)
{
    (void)ctx;
    return pe_find_dll(dll_name);
}

static uint32_t pe_import_lookup(void *ctx, const void *dll, const char *func)
{
    (void)ctx;
    return pe_lookup_export((const pe_dll_exports_t *)dll, func);
}

static int pe_import_write(void *ctx, uint32_t iat_rva, uint64_t value)
{
    return pe_write_iat_entry(*(const uint64_t *)ctx + iat_rva, value);
}

/* Resolve every import of the image mapped at image_base. Returns the total
 * resolved count, or -1 on a structural error. */
static int pe_resolve_imports(uint64_t image_base, uint32_t size_of_image,
                              uint32_t num_rva_sizes,
                              const pe_optional_header64_t *opt)
{
    pe_import_ops_t ops;

    if (num_rva_sizes <= PE_DIR_IMPORT)
        return 0;
    ops.find_dll = pe_import_find_dll;
    ops.lookup = pe_import_lookup;
    ops.write_iat = pe_import_write;
    ops.ctx = &image_base;
    /* The mapped image is at image_base in the current address space. */
    return pe_walk_imports((const uint8_t *)image_base, size_of_image,
        le32_at(PE_DATA_DIR_FIELD(opt, PE_DIR_IMPORT, VirtualAddress)),
        le32_at(PE_DATA_DIR_FIELD(opt, PE_DIR_IMPORT, Size)),
        &ops, (uint32_t *)0);
}

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
        pmm_free_frame(frame);   /* never mapped, so rollback cannot reach it */
        return -1;
    }

    (*total_pages)++;
    return 0;
}

/* ---- Loader entry point (exec dispatcher) ------------------------------- */

uint64_t pe_load(const uint8_t *data, uint64_t size)
{

    pe_validate_result_t v;
    pe_image_layout_t lay;
    if (pe_preflight(data, size, &v, &lay) != 0) {
        klog(LOG_ERROR, "pe", "PE32+ validation failed");
        return 0;
    }

    uint64_t image_base = lay.image_base;
    uint32_t size_of_image = lay.size_of_image;
    uint32_t size_of_headers = lay.size_of_headers;
    uint32_t entry_rva = lay.entry_rva;
    uint32_t num_rva_sizes = lay.num_rva_sizes;
    uint32_t total_pages_mapped = 0;

    uint64_t entry_va = image_base + entry_rva;


    /* ---- Map PE headers at ImageBase ---- */
    {
        /* The headers' whole aligned span: pe_preflight placed the first
         * section exactly at its end, so no page below SizeOfImage is left
         * unmapped for a file-chosen RVA to land in. */
        uint32_t hdr_pages = (uint32_t)(pe_align_up(size_of_headers,
                                                    lay.section_alignment) /
                                        VMM_PAGE_SIZE);
        uint32_t p;
        for (p = 0; p < hdr_pages; p++) {
            uint32_t offset = p * VMM_PAGE_SIZE;
            uint32_t copy_len = VMM_PAGE_SIZE;
            if (offset >= size_of_headers)
                copy_len = 0;
            else if (offset + copy_len > size_of_headers)
                copy_len = size_of_headers - offset;
            if (offset + copy_len > size)
                copy_len = (offset < size) ? (uint32_t)(size - offset) : 0;

            /* Headers are data: read-only and never executable, as Windows
             * maps them. */
            if (pe_map_one_page(image_base + offset, data, offset, copy_len,
                                VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NX,
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
            uint32_t sec_rva = le32_at(
                PE_SECTION_FIELD(v.sections, s, VirtualAddress));
            uint32_t raw_sz = le32_at(
                PE_SECTION_FIELD(v.sections, s, SizeOfRawData));
            uint32_t raw_ptr = le32_at(
                PE_SECTION_FIELD(v.sections, s, PointerToRawData));
            uint32_t chars = le32_at(
                PE_SECTION_FIELD(v.sections, s, Characteristics));

            /* Map the section's whole aligned span (the next section starts
             * at its end); bytes past its data read as zero. A section whose
             * span is 0 occupies no memory. */
            uint32_t span = pe_section_span(v.sections, s);
            if (span == 0)
                continue;
            if (raw_sz > span)
                raw_sz = span;   /* file bytes past the section's size are not loaded */

            uint32_t num_pages = (uint32_t)((pe_align_up((uint64_t)sec_rva + span,
                                                         lay.section_alignment) -
                                             sec_rva) / VMM_PAGE_SIZE);

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
                name[ni] = PE_SECTION_FIELD(v.sections, s, Name)[ni];
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
        uint32_t pdata_rva = le32_at(
            PE_DATA_DIR_FIELD(v.opt, PE_DIR_EXCEPTION, VirtualAddress));
        uint32_t pdata_sz = le32_at(
            PE_DATA_DIR_FIELD(v.opt, PE_DIR_EXCEPTION, Size));

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


    klog(LOG_INFO, "pe",
         "PE32+ loaded at 0x%x, entry 0x%x, %u sections, %u pages",
         image_base, entry_va,
         (uint64_t)v.num_sections, (uint64_t)total_pages_mapped);

    return entry_va;
}
