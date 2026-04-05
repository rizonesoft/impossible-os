/* ============================================================================
 * nt_memory.h -- NT virtual memory SSDT handlers and constants
 *
 * NtAllocateVirtualMemory, NtFreeVirtualMemory, NtProtectVirtualMemory,
 * NtQueryVirtualMemory, NtReadVirtualMemory, NtWriteVirtualMemory.
 * SSDT indices 0x0050-0x0062.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"

/* Register all virtual memory SSDT handlers.
 * Call once during Phase 3, after ssdt_init(). */
void nt_memory_register_ssdt(void);

/* ---- Allocation type flags ---------------------------------------------- */

#define MEM_COMMIT      0x00001000
#define MEM_RESERVE     0x00002000
#define MEM_RESET       0x00080000
#define MEM_TOP_DOWN    0x00100000
#define MEM_RELEASE     0x00008000
#define MEM_DECOMMIT    0x00004000

/* ---- Page protection flags ---------------------------------------------- */

#define PAGE_NOACCESS           0x01
#define PAGE_READONLY           0x02
#define PAGE_READWRITE          0x04
#define PAGE_WRITECOPY          0x08
#define PAGE_EXECUTE            0x10
#define PAGE_EXECUTE_READ       0x20
#define PAGE_EXECUTE_READWRITE  0x40
#define PAGE_EXECUTE_WRITECOPY  0x80
#define PAGE_GUARD              0x100
#define PAGE_NOCACHE            0x200
#define PAGE_WRITECOMBINE       0x400

/* ---- Memory state (NtQueryVirtualMemory) -------------------------------- */

#define MEM_FREE        0x00010000
#define MEM_RESERVED    0x00002000   /* same as MEM_RESERVE */
#define MEM_COMMITTED   0x00001000   /* same as MEM_COMMIT */

/* ---- Memory type -------------------------------------------------------- */

#define MEM_PRIVATE     0x00020000
#define MEM_MAPPED      0x00040000
#define MEM_IMAGE       0x01000000

/* ---- Memory information class ------------------------------------------- */

#define MemoryBasicInformation              0
#define MemoryWorkingSetExInformation       4

/* ---- MEMORY_BASIC_INFORMATION structure --------------------------------- */

typedef struct {
    uint64_t BaseAddress;
    uint64_t AllocationBase;
    uint32_t AllocationProtect;
    uint32_t _pad0;
    uint64_t RegionSize;
    uint32_t State;           /* MEM_COMMITTED, MEM_RESERVED, MEM_FREE */
    uint32_t Protect;         /* PAGE_* flags */
    uint32_t Type;            /* MEM_PRIVATE, MEM_MAPPED, MEM_IMAGE */
    uint32_t _pad1;
} MEMORY_BASIC_INFORMATION;
