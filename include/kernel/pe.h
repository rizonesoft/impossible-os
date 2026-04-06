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
_Static_assert(__builtin_offsetof(pe_section_header_t, VirtualAddress) == 0x0C,
    "pe_section_header_t.VirtualAddress at offset 0x0C");
_Static_assert(__builtin_offsetof(pe_section_header_t, Characteristics) == 0x24,
    "pe_section_header_t.Characteristics at offset 0x24");

/* ---- Import structures (§9) --------------------------------------------- */

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

/* IMAGE_IMPORT_BY_NAME -- hint + function name string */
typedef struct pe_import_by_name {
    uint16_t    Hint;                /* 0x00: export ordinal hint */
    char        Name[1];             /* 0x02: NUL-terminated function name */
} pe_import_by_name_t;

/* IMAGE_THUNK_DATA64 -- one entry in INT or IAT */
#define PE_ORDINAL_FLAG64   (1ULL << 63)

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

/* PE32+ loader entry point for the exec dispatcher.
 * Currently validates only (§7); section loading in §8.
 * Returns entry point VA on success, 0 on failure. */
uint64_t pe_load(const uint8_t *data, uint64_t size);
