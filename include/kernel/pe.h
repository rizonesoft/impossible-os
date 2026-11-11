/* ============================================================================
 * pe.h -- PE32+ (Portable Executable) header structures and parser
 *
 * Defines the Windows x64 PE format structures at exact offsets for loading
 * native Win64 executables. Only PE32+ (64-bit) is supported; PE32 (32-bit)
 * is rejected with ENOEXEC.
 *
 * Reference: Microsoft PE/COFF Specification, revision 11.0
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/le_bytes.h"
#include "kernel/mm/memmap.h"

/* ---- DOS Header (IMAGE_DOS_HEADER) -------------------------------------- */

#define PE_DOS_MAGIC    0x5A4D  /* 'MZ' */

typedef struct pe_dos_header {
    uint16_t    e_magic;        /* 0x00: MZ magic */
    uint16_t    e_cblp;         /* 0x02 */
    uint16_t    e_cp;           /* 0x04 */
    uint16_t    e_crlc;         /* 0x06 */
    uint16_t    e_cparhdr;      /* 0x08 */
    uint16_t    e_minalloc;     /* 0x0A */
    uint16_t    e_maxalloc;     /* 0x0C */
    uint16_t    e_ss;           /* 0x0E */
    uint16_t    e_sp;           /* 0x10 */
    uint16_t    e_csum;         /* 0x12 */
    uint16_t    e_ip;           /* 0x14 */
    uint16_t    e_cs;           /* 0x16 */
    uint16_t    e_lfarlc;       /* 0x18 */
    uint16_t    e_ovno;         /* 0x1A */
    uint16_t    e_res[4];       /* 0x1C */
    uint16_t    e_oemid;        /* 0x24 */
    uint16_t    e_oeminfo;      /* 0x26 */
    uint16_t    e_res2[10];     /* 0x28 */
    uint32_t    e_lfanew;       /* 0x3C: offset to PE signature */
} pe_dos_header_t;

_Static_assert(sizeof(pe_dos_header_t) == 64,
    "pe_dos_header_t must be 64 bytes (IMAGE_DOS_HEADER)");
_Static_assert(__builtin_offsetof(pe_dos_header_t, e_magic) == 0x00,
    "pe_dos_header_t.e_magic at offset 0x00");
_Static_assert(__builtin_offsetof(pe_dos_header_t, e_lfanew) == 0x3C,
    "pe_dos_header_t.e_lfanew at offset 0x3C");

/* ---- PE Signature ------------------------------------------------------- */

#define PE_SIGNATURE    0x00004550  /* 'PE\0\0' */
#define PE_SIGNATURE_SIZE  4        /* bytes of the 'PE\0\0' signature before COFF */

/* ---- COFF Header (IMAGE_FILE_HEADER) ------------------------------------ */

#define PE_MACHINE_AMD64    0x8664
#define PE_MACHINE_I386     0x014C

typedef struct pe_coff_header {
    uint16_t    Machine;            /* 0x00 */
    uint16_t    NumberOfSections;   /* 0x02 */
    uint32_t    TimeDateStamp;      /* 0x04 */
    uint32_t    PointerToSymbolTable; /* 0x08 */
    uint32_t    NumberOfSymbols;    /* 0x0C */
    uint16_t    SizeOfOptionalHeader; /* 0x10 */
    uint16_t    Characteristics;    /* 0x12 */
} pe_coff_header_t;

_Static_assert(sizeof(pe_coff_header_t) == 20,
    "pe_coff_header_t must be 20 bytes (IMAGE_FILE_HEADER)");
_Static_assert(__builtin_offsetof(pe_coff_header_t, NumberOfSections) == 0x02,
    "pe_coff_header_t.NumberOfSections at 0x02");
_Static_assert(__builtin_offsetof(pe_coff_header_t, SizeOfOptionalHeader) == 0x10,
    "pe_coff_header_t.SizeOfOptionalHeader at 0x10");

/* ---- Optional Header Magic ---------------------------------------------- */

#define PE_OPT_MAGIC_PE32      0x10B   /* 32-bit -- rejected */
#define PE_OPT_MAGIC_PE32PLUS  0x20B   /* 64-bit -- supported */

/* ---- Data Directory indices --------------------------------------------- */

#define PE_DIR_EXPORT           0
#define PE_DIR_IMPORT           1
#define PE_DIR_RESOURCE         2
#define PE_DIR_EXCEPTION        3   /* .pdata */
#define PE_DIR_SECURITY         4
#define PE_DIR_BASERELOC        5
#define PE_DIR_DEBUG            6
#define PE_DIR_TLS              9
#define PE_DIR_LOAD_CONFIG     10
#define PE_DIR_BOUND_IMPORT    11
#define PE_DIR_IAT             12
#define PE_DIR_DELAY_IMPORT    13
#define PE_DIR_CLR             14
#define PE_NUM_DATA_DIRS       16

/* ---- Data Directory entry ----------------------------------------------- */

typedef struct pe_data_directory {
    uint32_t    VirtualAddress;
    uint32_t    Size;
} pe_data_directory_t;

_Static_assert(sizeof(pe_data_directory_t) == 8,
    "pe_data_directory_t must be 8 bytes");

/* ---- Optional Header (IMAGE_OPTIONAL_HEADER64) -------------------------- */

typedef struct pe_optional_header64 {
    uint16_t    Magic;                  /* 0x00: 0x20B for PE32+ */
    uint8_t     MajorLinkerVersion;     /* 0x02 */
    uint8_t     MinorLinkerVersion;     /* 0x03 */
    uint32_t    SizeOfCode;             /* 0x04 */
    uint32_t    SizeOfInitializedData;  /* 0x08 */
    uint32_t    SizeOfUninitializedData;/* 0x0C */
    uint32_t    AddressOfEntryPoint;    /* 0x10 */
    uint32_t    BaseOfCode;             /* 0x14 */
    uint64_t    ImageBase;              /* 0x18 */
    uint32_t    SectionAlignment;       /* 0x20 */
    uint32_t    FileAlignment;          /* 0x24 */
    uint16_t    MajorOperatingSystemVersion; /* 0x28 */
    uint16_t    MinorOperatingSystemVersion; /* 0x2A */
    uint16_t    MajorImageVersion;      /* 0x2C */
    uint16_t    MinorImageVersion;      /* 0x2E */
    uint16_t    MajorSubsystemVersion;  /* 0x30 */
    uint16_t    MinorSubsystemVersion;  /* 0x32 */
    uint32_t    Win32VersionValue;      /* 0x34 */
    uint32_t    SizeOfImage;            /* 0x38 */
    uint32_t    SizeOfHeaders;          /* 0x3C */
    uint32_t    CheckSum;               /* 0x40 */
    uint16_t    Subsystem;              /* 0x44 */
    uint16_t    DllCharacteristics;     /* 0x46 */
    uint64_t    SizeOfStackReserve;     /* 0x48 */
    uint64_t    SizeOfStackCommit;      /* 0x50 */
    uint64_t    SizeOfHeapReserve;      /* 0x58 */
    uint64_t    SizeOfHeapCommit;       /* 0x60 */
    uint32_t    LoaderFlags;            /* 0x68 */
    uint32_t    NumberOfRvaAndSizes;    /* 0x6C */
    pe_data_directory_t DataDirectory[PE_NUM_DATA_DIRS]; /* 0x70 */
} pe_optional_header64_t;

_Static_assert(sizeof(pe_optional_header64_t) == 240,
    "pe_optional_header64_t must be 240 bytes (IMAGE_OPTIONAL_HEADER64)");
_Static_assert(__builtin_offsetof(pe_optional_header64_t, Magic) == 0x00,
    "pe_optional_header64_t.Magic at offset 0x00");
_Static_assert(__builtin_offsetof(pe_optional_header64_t, AddressOfEntryPoint) == 0x10,
    "pe_optional_header64_t.AddressOfEntryPoint at offset 0x10");
_Static_assert(__builtin_offsetof(pe_optional_header64_t, ImageBase) == 0x18,
    "pe_optional_header64_t.ImageBase at offset 0x18");
_Static_assert(__builtin_offsetof(pe_optional_header64_t, SizeOfImage) == 0x38,
    "pe_optional_header64_t.SizeOfImage at offset 0x38");
_Static_assert(__builtin_offsetof(pe_optional_header64_t, DataDirectory) == 0x70,
    "pe_optional_header64_t.DataDirectory at offset 0x70");

/* ---- Section Header (IMAGE_SECTION_HEADER) ------------------------------ */

#define PE_SECTION_NAME_SIZE    8

/* Section characteristics flags */
#define PE_SCN_CNT_CODE             0x00000020
#define PE_SCN_CNT_INITIALIZED_DATA 0x00000040
#define PE_SCN_CNT_UNINITIALIZED    0x00000080
#define PE_SCN_MEM_EXECUTE          0x20000000
#define PE_SCN_MEM_READ             0x40000000
#define PE_SCN_MEM_WRITE            0x80000000

typedef struct pe_section_header {
    char        Name[PE_SECTION_NAME_SIZE]; /* 0x00 */
    uint32_t    VirtualSize;        /* 0x08 */
    uint32_t    VirtualAddress;     /* 0x0C */
    uint32_t    SizeOfRawData;      /* 0x10 */
    uint32_t    PointerToRawData;   /* 0x14 */
    uint32_t    PointerToRelocations; /* 0x18 */
    uint32_t    PointerToLinenumbers; /* 0x1C */
    uint16_t    NumberOfRelocations;/* 0x20 */
    uint16_t    NumberOfLinenumbers;/* 0x22 */
    uint32_t    Characteristics;    /* 0x24 */
} pe_section_header_t;

_Static_assert(sizeof(pe_section_header_t) == 40,
    "pe_section_header_t must be 40 bytes (IMAGE_SECTION_HEADER)");
_Static_assert(__builtin_offsetof(pe_section_header_t, VirtualSize) == 0x08,
    "pe_section_header_t.VirtualSize at offset 0x08");
_Static_assert(__builtin_offsetof(pe_section_header_t, VirtualAddress) == 0x0C,
    "pe_section_header_t.VirtualAddress at offset 0x0C");
_Static_assert(__builtin_offsetof(pe_section_header_t, SizeOfRawData) == 0x10,
    "pe_section_header_t.SizeOfRawData at offset 0x10");
_Static_assert(__builtin_offsetof(pe_section_header_t, PointerToRawData) == 0x14,
    "pe_section_header_t.PointerToRawData at offset 0x14");
_Static_assert(__builtin_offsetof(pe_section_header_t, Characteristics) == 0x24,
    "pe_section_header_t.Characteristics at offset 0x24");

/* ---- Import structures --------------------------------------------------- */

/* IMAGE_IMPORT_DESCRIPTOR -- one per imported DLL */
typedef struct pe_import_descriptor {
    uint32_t    OriginalFirstThunk;  /* 0x00: RVA of Import Name Table (INT) */
    uint32_t    TimeDateStamp;       /* 0x04 */
    uint32_t    ForwarderChain;      /* 0x08 */
    uint32_t    Name;                /* 0x0C: RVA of DLL name string */
    uint32_t    FirstThunk;          /* 0x10: RVA of Import Address Table (IAT) */
} pe_import_descriptor_t;

_Static_assert(sizeof(pe_import_descriptor_t) == 20,
    "pe_import_descriptor_t must be 20 bytes");
/* The resolver reads these fields by pointer-to-member on an untrusted image;
 * pin the load-bearing PE/COFF ABI offsets so a field reorder (which keeps
 * sizeof==20) cannot silently break every RVA read. */
_Static_assert(__builtin_offsetof(pe_import_descriptor_t, OriginalFirstThunk) == 0x00,
    "pe_import_descriptor_t.OriginalFirstThunk at 0x00");
_Static_assert(__builtin_offsetof(pe_import_descriptor_t, Name) == 0x0C,
    "pe_import_descriptor_t.Name at 0x0C");
_Static_assert(__builtin_offsetof(pe_import_descriptor_t, FirstThunk) == 0x10,
    "pe_import_descriptor_t.FirstThunk at 0x10");

/* IMAGE_IMPORT_BY_NAME -- hint + function name string */
typedef struct pe_import_by_name {
    uint16_t    Hint;                /* 0x00: export ordinal hint */
    char        Name[1];             /* 0x02: NUL-terminated function name */
} pe_import_by_name_t;

/* IMAGE_THUNK_DATA64 -- one entry in INT or IAT */
#define PE_ORDINAL_FLAG64   (1ULL << 63)
#define PE_THUNK64_SIZE     8   /* bytes per IMAGE_THUNK_DATA64 (INT/IAT entry) */
#define PE_IMPORT_HINT_SIZE 2   /* bytes of IMAGE_IMPORT_BY_NAME.Hint before Name */

/* Kernel-side export table entry: maps function name -> SSDT index */
typedef struct pe_export_entry {
    const char *name;               /* function name (e.g., "ExitProcess") */
    uint32_t    ssdt_index;         /* SSDT service number */
} pe_export_entry_t;

/* Kernel-side DLL export table: DLL name + sorted export array */
typedef struct pe_dll_exports {
    const char              *dll_name;  /* e.g., "kernel32.dll" */
    const pe_export_entry_t *exports;   /* sorted by name for binary search */
    uint32_t                 count;     /* number of entries */
} pe_dll_exports_t;

/* ---- Field access ------------------------------------------------------ */

/* The address of FIELD in the header of TYPE at P, as bytes. Every header sits
 * at an offset the file chooses (e_lfanew, SizeOfOptionalHeader, an RVA), so it
 * can be at an odd address, and naming a member through such a pointer is
 * undefined behaviour even to take its address. Read fields only through this
 * and the le*_at loads, never as p->FIELD. */
#define PE_FIELD(p, type, field) \
    ((const uint8_t *)(p) + __builtin_offsetof(type, field))
#define PE_SECTION_FIELD(secs, i, field) \
    ((const uint8_t *)(secs) + (uint64_t)(i) * sizeof(pe_section_header_t) + \
     __builtin_offsetof(pe_section_header_t, field))
#define PE_DATA_DIR_FIELD(opt, i, field) \
    (PE_FIELD(opt, pe_optional_header64_t, DataDirectory) + \
     (uint64_t)(i) * sizeof(pe_data_directory_t) + \
     __builtin_offsetof(pe_data_directory_t, field))

/* ---- PE validation result ----------------------------------------------- */

typedef struct pe_validate_result {
    int                             ok;     /* 1 = valid PE32+, 0 = error */
    int                             err;    /* errno on failure (ENOEXEC) */
    const pe_dos_header_t          *dos;    /* -> DOS header in data */
    const pe_coff_header_t         *coff;   /* -> COFF header in data */
    const pe_optional_header64_t   *opt;    /* -> Optional header in data */
    const pe_section_header_t      *sections; /* -> first section header */
    uint16_t                        num_sections;
} pe_validate_result_t;

/* ---- Public API --------------------------------------------------------- */

/* Validate a PE32+ binary in memory. Does NOT load or map sections.
 * On success, result.ok=1 and the header pointers reference locations
 * within the 'data' buffer (zero-copy). On failure, result.ok=0 and
 * result.err is set (ENOEXEC).
 *
 * Checks performed:
 *   - MZ magic at offset 0
 *   - e_lfanew within bounds
 *   - PE signature (0x00004550) at e_lfanew
 *   - Machine == 0x8664 (AMD64)
 *   - Optional Header Magic == 0x20B (PE32+)
 *   - SizeOfOptionalHeader >= sizeof(pe_optional_header64_t)
 *   - Section headers within data bounds
 */
pe_validate_result_t pe_validate(const uint8_t *data, uint64_t size);

/* Loader limits on the image the file describes. */
/* The whole aligned image lies in [PE_MIN_IMAGE_BASE, PE_MAX_IMAGE_END): above
 * the low identity map and below the end of the user half, so a file-chosen
 * ImageBase can never land on kernel, MMIO or non-canonical addresses. */
#define PE_MIN_IMAGE_BASE   MM_USER_LOW_IDENTITY_END
#define PE_MAX_IMAGE_END    MM_USER_END
#define PE_MAX_SECTIONS     96                      /* linkers emit < 20 */
#define PE_MAX_IMAGE_SIZE   (256ULL * 1024 * 1024)  /* 256 MiB */
_Static_assert(PE_MIN_IMAGE_BASE + PE_MAX_IMAGE_SIZE < PE_MAX_IMAGE_END,
               "the PE image window must hold the largest image");

/* ---- Import table walk (pe_validate.c) ---- */

#define PE_STUB_THUNK_ADDR     0ULL  /* IAT value for an unresolved import: a call faults cleanly */
#define PE_MAX_IMPORT_DLLS     64    /* descriptors walked, so a file cannot make the walk unbounded */
#define PE_MAX_THUNKS_PER_DLL  4096  /* thunks walked per descriptor, for the same reason */
#define PE_MAX_NAME_LEN        256   /* longest DLL or function name read from the image */

/* What the walk needs from the loader. find_dll returns a handle for a DLL
 * the system exports, or NULL; lookup returns the SSDT index of one of its
 * functions, or (uint32_t)-1; write_iat stores an 8-byte IAT value at an RVA
 * the walk has bounded inside the image and returns 1, or 0 when the page
 * holding it is unmapped. */
typedef struct pe_import_ops {
    const void *(*find_dll)(void *ctx, const char *dll_name);
    uint32_t    (*lookup)(void *ctx, const void *dll, const char *func);
    int         (*write_iat)(void *ctx, uint32_t iat_rva, uint64_t value);
    void        *ctx;
} pe_import_ops_t;

/* Walk the import directory [import_rva, import_rva + import_size) of an image
 * mapped at `image` (size_of_image bytes, all readable) and fill its IAT
 * through ops. Every descriptor, thunk and name the file names is bounded
 * inside the image before it is read. Returns the number of imports resolved
 * (0 when there is no import directory), or -1 when the table is malformed or
 * an IAT entry could not be written. *dlls_out, if given, counts the DLLs. */
int pe_walk_imports(const uint8_t *image, uint32_t size_of_image,
                    uint32_t import_rva, uint32_t import_size,
                    const pe_import_ops_t *ops, uint32_t *dlls_out);

#define PE_PAGE_SIZE 4096u

static inline uint64_t pe_align_up(uint64_t v, uint32_t align)
{
    return (v + align - 1) & ~(uint64_t)(align - 1);
}

/* A section's size in memory: VirtualSize, or SizeOfRawData when VirtualSize
 * is 0, as the Windows loader reads it. */
static inline uint32_t pe_section_span(const pe_section_header_t *secs, uint32_t i)
{
    uint32_t vs = le32_at(PE_SECTION_FIELD(secs, i, VirtualSize));
    return vs ? vs : le32_at(PE_SECTION_FIELD(secs, i, SizeOfRawData));
}

/* The image layout pe_preflight accepted, read once from the headers. */
typedef struct pe_image_layout {
    uint64_t image_base;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint32_t entry_rva;
    uint32_t num_rva_sizes;
    uint32_t section_alignment;  /* power of two, at least PE_PAGE_SIZE */
    uint32_t mapped_size;        /* SizeOfImage rounded up to it: every byte below is mapped */
} pe_image_layout_t;

/* Every check made on file bytes before a page is mapped: pe_validate, then
 * ImageBase, SizeOfImage, SizeOfHeaders, the entry RVA, the section count, and
 * each section's virtual range against SizeOfImage and raw range against the
 * file, all in 64-bit arithmetic. 0 = accepted (*vout and *out filled),
 * -1 = rejected (*vout holds pe_validate's result). Pure: no allocation and no
 * mapping, so the host fuzzer runs it as the loader does. */
int pe_preflight(const uint8_t *data, uint64_t size, pe_validate_result_t *vout,
                 pe_image_layout_t *out);

/* PE32+ loader entry point for the exec dispatcher: pe_preflight, then map the
 * headers and sections, resolve imports, parse .pdata and register the module.
 * Returns entry point VA on success, 0 on failure (every page it mapped is
 * unmapped again). */
uint64_t pe_load(const uint8_t *data, uint64_t size);

/* Verify every DLL export table in pe.c is strictly sorted ascending by
 * name (required by the binary-search lookup in pe_lookup_export).
 * Returns 0 if all tables are sorted, else the number of violations.
 * Intended for boot-time gating and unit tests. */
int pe_exports_sorted_check(void);

/* Resolve an ntdll export name to its SSDT index, or (uint32_t)-1 if the name
 * is not exported. Lets tests assert a native syscall is reachable through the
 * ntdll import surface, not merely registered in the SSDT. */
uint32_t pe_ntdll_export_ssdt(const char *name);
