/* ============================================================================
 * crashdump.h -- MDMP (Minidump) binary format structures
 *
 * WinDbg-compatible minidump format definitions for crash dump files.
 * All structures use pack(4) to match the Windows SDK dbghelp.h layout
 * (pshpack4.h). Every struct size is verified by static assert.
 *
 * XREF: 02-kernel-core/TODO-27-crash-dump-generation.md SS4
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "build_info.h"

/* ---- MDMP signature and version ----------------------------------------- */

#define MDMP_SIGNATURE  0x504D444D  /* "MDMP" little-endian */
#define MDMP_VERSION    0x0000A793  /* Windows minidump version (42899) */

/* ---- MINIDUMP_TYPE flags ------------------------------------------------ */

#define MiniDumpNormal              0x00000000
#define MiniDumpWithFullMemory      0x00000002
#define MiniDumpFilterMemory        0x00000008
#define MiniDumpWithCodeSegs        0x00002000
#define MiniDumpWithoutOptionalData 0x00000400

/* ---- Stream type constants ---------------------------------------------- */

#define ThreadListStream            3
#define ModuleListStream            4
#define MemoryListStream            5
#define ExceptionStream             6
#define SystemInfoStream            7
#define ThreadExListStream          8
#define Memory64ListStream          9
#define UnloadedModuleListStream   14
#define MiscInfoStream             15
/* Impossible OS extension stream (vendor range 0x8000+) */
#define ImpossibleOSInfoStream     0x8001

/* ---- Processor architecture --------------------------------------------- */

#define PROCESSOR_ARCHITECTURE_AMD64  9

/* ---- Platform ID -------------------------------------------------------- */

#define VER_PLATFORM_WIN32_NT  2
#define VER_NT_WORKSTATION     1

/* ---- Pack(4) to match Windows SDK dbghelp.h layout ---------------------- */

#pragma pack(push, 4)

/* ---- MINIDUMP_HEADER (32 bytes) ----------------------------------------- */

typedef struct _MINIDUMP_HEADER {
    uint32_t Signature;           /* MDMP_SIGNATURE */
    uint32_t Version;             /* MDMP_VERSION | (impl_version << 16) */
    uint32_t NumberOfStreams;
    uint32_t StreamDirectoryRva;  /* byte offset to directory array */
    uint32_t CheckSum;            /* CRC32 of entire dump, 0 = not computed */
    uint32_t TimeDateStamp;       /* Unix timestamp */
    uint64_t Flags;               /* MINIDUMP_TYPE flags */
} MINIDUMP_HEADER;

/* ---- MINIDUMP_DIRECTORY (12 bytes) -------------------------------------- */

typedef struct _MINIDUMP_DIRECTORY {
    uint32_t StreamType;    /* stream type code */
    uint32_t DataSize;      /* byte length of stream data */
    uint32_t Rva;           /* byte offset from file start */
} MINIDUMP_DIRECTORY;

/* ---- MINIDUMP_LOCATION_DESCRIPTOR (8 bytes) ----------------------------- */

typedef struct _MINIDUMP_LOCATION_DESCRIPTOR {
    uint32_t DataSize;
    uint32_t Rva;
} MINIDUMP_LOCATION_DESCRIPTOR;

/* ---- MINIDUMP_MEMORY_DESCRIPTOR (16 bytes) ------------------------------ */

typedef struct _MINIDUMP_MEMORY_DESCRIPTOR {
    uint64_t StartOfMemoryRange;    /* virtual or physical base */
    MINIDUMP_LOCATION_DESCRIPTOR Memory;
} MINIDUMP_MEMORY_DESCRIPTOR;

/* ---- MINIDUMP_EXCEPTION (152 bytes) ------------------------------------- */

typedef struct _MINIDUMP_EXCEPTION {
    uint32_t ExceptionCode;             /* = bugcheck code */
    uint32_t ExceptionFlags;
    uint64_t ExceptionRecord;           /* VA of chained record, 0 if none */
    uint64_t ExceptionAddress;          /* = crashing RIP */
    uint32_t NumberParameters;          /* = 4 (bugcheck params) */
    uint32_t __unusedAlignment;
    uint64_t ExceptionInformation[15];  /* bugcheck params p1-p4 in [0]-[3] */
} MINIDUMP_EXCEPTION;

/* ---- MINIDUMP_EXCEPTION_STREAM (168 bytes) ------------------------------ */

typedef struct _MINIDUMP_EXCEPTION_STREAM {
    uint32_t ThreadId;
    uint32_t __alignment;
    MINIDUMP_EXCEPTION ExceptionRecord;
    MINIDUMP_LOCATION_DESCRIPTOR ThreadContext;  /* -> CONTEXT */
} MINIDUMP_EXCEPTION_STREAM;

/* ---- VS_FIXEDFILEINFO (52 bytes) ---------------------------------------- */

typedef struct _VS_FIXEDFILEINFO {
    uint32_t dwSignature;
    uint32_t dwStrucVersion;
    uint32_t dwFileVersionMS;
    uint32_t dwFileVersionLS;
    uint32_t dwProductVersionMS;
    uint32_t dwProductVersionLS;
    uint32_t dwFileFlagsMask;
    uint32_t dwFileFlags;
    uint32_t dwFileOS;
    uint32_t dwFileType;
    uint32_t dwFileSubtype;
    uint32_t dwFileDateMS;
    uint32_t dwFileDateLS;
} VS_FIXEDFILEINFO;

/* ---- MINIDUMP_MODULE (108 bytes) ---------------------------------------- */

typedef struct _MINIDUMP_MODULE {
    uint64_t BaseOfImage;
    uint32_t SizeOfImage;
    uint32_t CheckSum;
    uint32_t TimeDateStamp;
    uint32_t ModuleNameRva;         /* RVA -> MINIDUMP_STRING (length-prefixed UTF-16) */
    VS_FIXEDFILEINFO VersionInfo;
    MINIDUMP_LOCATION_DESCRIPTOR CvRecord;
    MINIDUMP_LOCATION_DESCRIPTOR MiscRecord;
    uint64_t Reserved0;
    uint64_t Reserved1;
} MINIDUMP_MODULE;

/* ---- MINIDUMP_THREAD (48 bytes) ----------------------------------------- */

typedef struct _MINIDUMP_THREAD {
    uint32_t ThreadId;
    uint32_t SuspendCount;
    uint32_t PriorityClass;
    uint32_t Priority;
    uint64_t Teb;                       /* TEB virtual address */
    MINIDUMP_MEMORY_DESCRIPTOR Stack;   /* stack memory range + data RVA */
    MINIDUMP_LOCATION_DESCRIPTOR ThreadContext;  /* -> CONTEXT */
} MINIDUMP_THREAD;

/* ---- CPU_INFORMATION (24 bytes) ----------------------------------------- */

typedef union _CPU_INFORMATION {
    struct {
        uint32_t VendorId[3];               /* CPUID 0: EBX, EDX, ECX */
        uint32_t VersionInformation;        /* CPUID 1: EAX */
        uint32_t FeatureInformation;        /* CPUID 1: EDX */
        uint32_t AMDExtendedCpuFeatures;    /* CPUID 0x80000001: EDX */
    } X86CpuInfo;
    struct {
        uint64_t ProcessorFeatures[2];
    } OtherCpuInfo;
} CPU_INFORMATION;

/* ---- MINIDUMP_SYSTEM_INFO (56 bytes) ------------------------------------ */

typedef struct _MINIDUMP_SYSTEM_INFO {
    uint16_t ProcessorArchitecture;     /* PROCESSOR_ARCHITECTURE_AMD64 = 9 */
    uint16_t ProcessorLevel;            /* CPU family (e.g. 6 for P6+) */
    uint16_t ProcessorRevision;         /* model << 8 | stepping */
    union {
        uint16_t Reserved0;
        struct {
            uint8_t NumberOfProcessors;
            uint8_t ProductType;        /* VER_NT_WORKSTATION = 1 */
        };
    };
    uint32_t MajorVersion;              /* 10 for Win10 compat */
    uint32_t MinorVersion;              /* 0 */
    uint32_t BuildNumber;               /* BUILD_NUMBER */
    uint32_t PlatformId;                /* VER_PLATFORM_WIN32_NT = 2 */
    uint32_t CSDVersionRva;             /* RVA -> OS name string (UTF-16) */
    union {
        uint32_t Reserved1;
        struct {
            uint16_t SuiteMask;
            uint16_t Reserved2;
        };
    };
    CPU_INFORMATION Cpu;
} MINIDUMP_SYSTEM_INFO;

/* ---- IMPOSSIBLE_OS_INFO (custom extension stream, 436 bytes) ------------ */

#define IMPOSSIBLE_OS_INFO_MAGIC  0x10DEAD00

typedef struct _IMPOSSIBLE_OS_INFO {
    uint32_t Magic;                     /* IMPOSSIBLE_OS_INFO_MAGIC */
    uint32_t KernelBuild;              /* BUILD_NUMBER */
    uint64_t BugCheckCode;
    uint64_t BugCheckParams[4];        /* p1-p4 */
    char     BugCheckName[64];         /* e.g. "PAGE_FAULT_IN_NONPAGED_AREA" */
    uint64_t PanicTimestamp;           /* TSC + wall clock */
    uint64_t PhysicalMemoryKiB;
    uint32_t CpuCount;
    char     CpuBrandString[48];       /* CPUID 0x80000002-0x80000004 */
    char     KernelPath[256];          /* "C:\\Impossible\\System32\\kernel.exe" */
} IMPOSSIBLE_OS_INFO;

#pragma pack(pop)

/* ---- Static size asserts ------------------------------------------------ */

_Static_assert(sizeof(MINIDUMP_HEADER) == 32,
               "MINIDUMP_HEADER must be 32 bytes (Windows SDK)");
_Static_assert(sizeof(MINIDUMP_DIRECTORY) == 12,
               "MINIDUMP_DIRECTORY must be 12 bytes");
_Static_assert(sizeof(MINIDUMP_LOCATION_DESCRIPTOR) == 8,
               "MINIDUMP_LOCATION_DESCRIPTOR must be 8 bytes");
_Static_assert(sizeof(MINIDUMP_MEMORY_DESCRIPTOR) == 16,
               "MINIDUMP_MEMORY_DESCRIPTOR must be 16 bytes");
_Static_assert(sizeof(MINIDUMP_EXCEPTION) == 152,
               "MINIDUMP_EXCEPTION must be 152 bytes");
_Static_assert(sizeof(MINIDUMP_EXCEPTION_STREAM) == 168,
               "MINIDUMP_EXCEPTION_STREAM must be 168 bytes");
_Static_assert(sizeof(VS_FIXEDFILEINFO) == 52,
               "VS_FIXEDFILEINFO must be 52 bytes");
_Static_assert(sizeof(MINIDUMP_MODULE) == 108,
               "MINIDUMP_MODULE must be 108 bytes (Windows SDK pack(4))");
_Static_assert(sizeof(MINIDUMP_THREAD) == 48,
               "MINIDUMP_THREAD must be 48 bytes");
_Static_assert(sizeof(CPU_INFORMATION) == 24,
               "CPU_INFORMATION must be 24 bytes");
_Static_assert(sizeof(MINIDUMP_SYSTEM_INFO) == 56,
               "MINIDUMP_SYSTEM_INFO must be 56 bytes");
_Static_assert(sizeof(IMPOSSIBLE_OS_INFO) == 436,
               "IMPOSSIBLE_OS_INFO must be 436 bytes");

/* ---- Signature and constant validation ---------------------------------- */

_Static_assert(MDMP_SIGNATURE == 0x504D444D,
               "MDMP_SIGNATURE must be 'MDMP' (0x504D444D)");
_Static_assert(ImpossibleOSInfoStream == 0x8001,
               "ImpossibleOSInfoStream must be 0x8001");

/* MINIDUMP_TYPE flag ABI guards (must match Windows SDK dbghelp.h) */
_Static_assert(MiniDumpNormal == 0x00000000,
               "MiniDumpNormal must be 0");
_Static_assert(MiniDumpWithFullMemory == 0x00000002,
               "MiniDumpWithFullMemory must be 0x02");
_Static_assert(MiniDumpFilterMemory == 0x00000008,
               "MiniDumpFilterMemory must be 0x08");
_Static_assert(MiniDumpWithCodeSegs == 0x00002000,
               "MiniDumpWithCodeSegs must be 0x2000");
_Static_assert(MiniDumpWithoutOptionalData == 0x00000400,
               "MiniDumpWithoutOptionalData must be 0x400");
