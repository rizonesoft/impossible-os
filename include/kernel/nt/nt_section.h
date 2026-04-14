/* ============================================================================
 * nt_section.h -- Section object SSDT (TODO-05 §18)
 *
 * Registers NtCreateSection, NtOpenSection, NtMapViewOfSection,
 * NtUnmapViewOfSection, NtExtendSection, NtQuerySection,
 * NtAreMappedFilesTheSame at SSDT 0x005C-0x0062.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/ob/ob_section.h"
#include "kernel/ob/peb.h"

/* SECTION_INFORMATION_CLASS */
#define SectionBasicInformation   0u
#define SectionImageInformation   1u

/* NtMapViewOfSection: optional extended arguments when a5 is non-NULL.
 * All pointers are caller addresses (user or kernel); handlers probe
 * user pointers when ssdt_previous_mode() is UserMode. */
typedef struct _NT_MAPVIEW_ARGS {
    uint64_t ZeroBits;
    uint64_t CommitSize;
    int64_t  SectionOffsetQuad;
    uint64_t *ViewSize;
    uint32_t InheritDisposition;
    uint32_t AllocationType;
    uint32_t Win32Protect;
    uint32_t _pad;
} NT_MAPVIEW_ARGS;

/* SECTION_BASIC_INFORMATION: field order matches Windows SDK ntddk.h.
 *   offset 0:  PVOID           BaseAddress;
 *   offset 8:  ULONG           AllocationAttributes;  (4B + 4B pad)
 *   offset 16: LARGE_INTEGER   MaximumSize;
 * Total: 24 bytes. */
typedef struct _SECTION_BASIC_INFORMATION {
    void           *BaseAddress;            /* 0x00 */
    uint32_t        AllocationAttributes;   /* 0x08 */
    uint32_t        _pad;                   /* 0x0C */
    LARGE_INTEGER   MaximumSize;            /* 0x10 */
} SECTION_BASIC_INFORMATION;
_Static_assert(__builtin_offsetof(SECTION_BASIC_INFORMATION, BaseAddress) == 0,
               "SECTION_BASIC_INFORMATION.BaseAddress must be at offset 0 (Win ABI)");
_Static_assert(__builtin_offsetof(SECTION_BASIC_INFORMATION, AllocationAttributes) == 8,
               "SECTION_BASIC_INFORMATION.AllocationAttributes must be at offset 8 (Win ABI)");
_Static_assert(__builtin_offsetof(SECTION_BASIC_INFORMATION, MaximumSize) == 16,
               "SECTION_BASIC_INFORMATION.MaximumSize must be at offset 16 (Win ABI)");
_Static_assert(sizeof(SECTION_BASIC_INFORMATION) == 24,
               "SECTION_BASIC_INFORMATION size must be 24 bytes (Win ABI)");

typedef struct _SECTION_IMAGE_INFORMATION {
    void     *TransferAddress;
    uint32_t  ZeroBits;
    uint32_t  _pad0;
    uint64_t  MaximumStackSize;
    uint64_t  CommittedStackSize;
    uint32_t  SubSystemType;
    uint32_t  SubSystemMajorVersion;
} SECTION_IMAGE_INFORMATION;

void nt_section_register_ssdt(void);
