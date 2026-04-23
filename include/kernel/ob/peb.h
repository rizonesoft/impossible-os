/* ============================================================================
 * peb.h -- Process Environment Block (PEB)
 *
 * Windows x64-compatible PEB layout at exact offsets so ntdll startup code
 * can walk PEB->ProcessParameters, PEB->Ldr, and PEB->OS version fields
 * without patching.
 *
 * Also defines RTL_USER_PROCESS_PARAMETERS, UNICODE_STRING, PEB_LDR_DATA,
 * LDR_DATA_TABLE_ENTRY, LIST_ENTRY, and LARGE_INTEGER -- all at Windows-
 * compatible sizes and alignments.
 *
 * Reference: Windows x64 PEB offsets from winternl.h / NtInternals
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Win64 primitive types (user-mode ABI) ------------------------------ */

/* Win64 HANDLE is pointer-sized (8 bytes), unlike our kernel HANDLE (int32).
 * Use UHANDLE in user-mode ABI structs to avoid confusion. */
typedef uint64_t UHANDLE;

#define UHANDLE_INVALID ((UHANDLE)(int64_t)-1)

typedef struct {
    int64_t QuadPart;
} LARGE_INTEGER;

/* Doubly-linked list (NT kernel list head / entry) */
typedef struct list_entry {
    struct list_entry *Flink;
    struct list_entry *Blink;
} LIST_ENTRY;

/* UTF-16 counted string (used throughout Win32/NT) */
typedef struct {
    uint16_t Length;            /* byte count (not including NUL) */
    uint16_t MaximumLength;    /* allocated buffer size in bytes */
    uint32_t _pad;             /* align Buffer to 8 bytes on x64 */
    uint16_t *Buffer;          /* UTF-16 string data */
} UNICODE_STRING;

/* ---- RTL_USER_PROCESS_PARAMETERS ---------------------------------------- */

typedef struct {
    uint32_t MaximumLength;    /* offset 0x00 */
    uint32_t Length;           /* offset 0x04 */
    uint32_t Flags;            /* offset 0x08 */
    uint32_t DebugFlags;       /* offset 0x0C */
    void    *ConsoleHandle;    /* offset 0x10 */
    uint32_t ConsoleFlags;     /* offset 0x18 */
    uint32_t _pad0;            /* offset 0x1C -- align to 8 */
    UHANDLE  StandardInput;    /* offset 0x20 */
    UHANDLE  StandardOutput;   /* offset 0x28 */
    UHANDLE  StandardError;    /* offset 0x30 */

    /* CurrentDirectory */
    UNICODE_STRING CurrentDirectoryDosPath; /* offset 0x38 */
    UHANDLE        CurrentDirectoryHandle;  /* offset 0x48 */

    UNICODE_STRING DllPath;        /* offset 0x50 */
    UNICODE_STRING ImagePathName;  /* offset 0x60 */
    UNICODE_STRING CommandLine;    /* offset 0x70 */
    void          *Environment;    /* offset 0x80 -- UTF-16 env block */
} RTL_USER_PROCESS_PARAMETERS;

/* ---- PEB_LDR_DATA (stub; full loader-data implementation pending) -------- */

typedef struct peb_ldr_data {
    uint32_t   Length;                            /* offset 0x00 */
    uint8_t    Initialized;                       /* offset 0x04 */
    uint8_t    _pad0[3];                          /* offset 0x05 */
    void      *SsHandle;                          /* offset 0x08 */
    LIST_ENTRY InLoadOrderModuleList;             /* offset 0x10 */
    LIST_ENTRY InMemoryOrderModuleList;           /* offset 0x20 */
    LIST_ENTRY InInitializationOrderModuleList;   /* offset 0x30 */
} PEB_LDR_DATA;

/* ---- LDR_DATA_TABLE_ENTRY ----------------------------------------------- */

typedef struct ldr_data_table_entry {
    LIST_ENTRY     InLoadOrderLinks;              /* offset 0x00 */
    LIST_ENTRY     InMemoryOrderLinks;            /* offset 0x10 */
    LIST_ENTRY     InInitializationOrderLinks;    /* offset 0x20 */
    void          *DllBase;                       /* offset 0x30 */
    void          *EntryPoint;                    /* offset 0x38 */
    uint64_t       SizeOfImage;                   /* offset 0x40 */
    UNICODE_STRING FullDllName;                   /* offset 0x48 */
    UNICODE_STRING BaseDllName;                   /* offset 0x58 */
    uint32_t       Flags;                         /* offset 0x68 */
    uint16_t       LoadCount;                     /* offset 0x6C */
    uint16_t       TlsIndex;                      /* offset 0x6E */
} LDR_DATA_TABLE_ENTRY;

/* ---- PEB ---------------------------------------------------------------- */
/* Full Process Environment Block at exact Windows x64 offsets.
 *
 * NtCurrentPeb() = gs:[0x60] → TEB.ProcessEnvironmentBlock → PEB.
 * PEB->ProcessParameters = offset 0x20.
 * PEB->Ldr = offset 0x18.
 * PEB->ImageBaseAddress = offset 0x10.
 */

typedef struct peb {
    /* 0x00 */ uint8_t   InheritedAddressSpace;
    /* 0x01 */ uint8_t   ReadImageFileExecOptions;
    /* 0x02 */ uint8_t   BeingDebugged;
    /* 0x03 */ uint8_t   BitField;
    /* 0x04 */ uint8_t   _pad0[4];   /* align to 0x08 */
    /* 0x08 */ void     *Mutant;
    /* 0x10 */ void     *ImageBaseAddress;
    /* 0x18 */ PEB_LDR_DATA *Ldr;
    /* 0x20 */ RTL_USER_PROCESS_PARAMETERS *ProcessParameters;

    /* 0x28 – 0xA3: SubSystemData, ProcessHeap, FastPebLock, etc.
     * Reserved -- pad to preserve offsets for version fields. */
    uint8_t  _reserved_0028[0xA4 - 0x28];

    /* 0xA4 */ uint32_t  OSMajorVersion;
    /* 0xA8 */ uint32_t  OSMinorVersion;
    /* 0xAC */ uint16_t  OSBuildNumber;
    /* 0xAE */ uint16_t  OSCSDVersion;
    /* 0xB0 */ uint32_t  OSPlatformId;
    /* 0xB4 */ uint32_t  ImageSubsystem;
    /* 0xB8 */ uint32_t  NumberOfProcessors;

    /* 0xBC */ uint32_t  NtGlobalFlag;

    /* 0xC0 – 0xC7: padding to align CriticalSectionTimeout */
    uint8_t  _reserved_00C0[0xC8 - 0xC0];

    /* 0xC8 */ LARGE_INTEGER CriticalSectionTimeout;
    /* 0xD0 */ uint64_t  HeapSegmentReserve;
    /* 0xD8 */ uint64_t  HeapSegmentCommit;
    /* 0xE0 */ uint64_t  HeapDeCommitTotalFreeThreshold;
    /* 0xE8 */ uint64_t  HeapDeCommitFreeBlockThreshold;

    /* 0xF0 – 0x22F: many more Windows-internal fields.
     * Reserved -- pad to preserve TLS bitmap offset. */
    uint8_t  _reserved_00F0[0x230 - 0xF0];

    /* 0x230 */ uint64_t  TlsBitmap;
    /* 0x238 */ uint32_t  TlsBitmapBits[2];

    /* Pad to page boundary */
    uint8_t  _reserved_0240[0x1000 - 0x240];
} PEB;

/* ---- Compile-time offset verification ----------------------------------- */

_Static_assert(
    __builtin_offsetof(PEB, InheritedAddressSpace) == 0x00,
    "PEB.InheritedAddressSpace");
_Static_assert(
    __builtin_offsetof(PEB, BeingDebugged) == 0x02,
    "PEB.BeingDebugged");
_Static_assert(
    __builtin_offsetof(PEB, Mutant) == 0x08,
    "PEB.Mutant");
_Static_assert(
    __builtin_offsetof(PEB, ImageBaseAddress) == 0x10,
    "PEB.ImageBaseAddress");
_Static_assert(
    __builtin_offsetof(PEB, Ldr) == 0x18,
    "PEB.Ldr");
_Static_assert(
    __builtin_offsetof(PEB, ProcessParameters) == 0x20,
    "PEB.ProcessParameters");
_Static_assert(
    __builtin_offsetof(PEB, OSMajorVersion) == 0xA4,
    "PEB.OSMajorVersion");
_Static_assert(
    __builtin_offsetof(PEB, OSMinorVersion) == 0xA8,
    "PEB.OSMinorVersion");
_Static_assert(
    __builtin_offsetof(PEB, OSBuildNumber) == 0xAC,
    "PEB.OSBuildNumber");
_Static_assert(
    __builtin_offsetof(PEB, NumberOfProcessors) == 0xB8,
    "PEB.NumberOfProcessors");
_Static_assert(
    __builtin_offsetof(PEB, NtGlobalFlag) == 0xBC,
    "PEB.NtGlobalFlag");
_Static_assert(
    __builtin_offsetof(PEB, CriticalSectionTimeout) == 0xC8,
    "PEB.CriticalSectionTimeout");
_Static_assert(
    __builtin_offsetof(PEB, HeapSegmentReserve) == 0xD0,
    "PEB.HeapSegmentReserve");
_Static_assert(
    __builtin_offsetof(PEB, TlsBitmap) == 0x230,
    "PEB.TlsBitmap");
_Static_assert(
    __builtin_offsetof(PEB, TlsBitmapBits) == 0x238,
    "PEB.TlsBitmapBits");
