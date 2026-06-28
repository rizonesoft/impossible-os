/* ============================================================================
 * ssdt.h -- System Service Descriptor Table (SSDT)
 *
 * The SSDT is a flat array of function pointers indexed by the 12-bit
 * service number in RAX. ntdll stubs do: mov rax, <number>; syscall.
 *
 * Two tables exist:
 *   Table 0 (main):   indices 0x0000-0x03FF -- NtXxx kernel APIs
 *   Table 1 (shadow): indices 0x1000-0x13FF -- NtGdiXxx/NtUserXxx (Win32k)
 *
 * The high 2 bits of the 14-bit service number select the table:
 *   bits 13:12 = 00 -> main SSDT
 *   bits 13:12 = 01 -> shadow SSDT (Win32k)
 *   bits 11:0         -> index into selected table
 *
 * Unimplemented slots point to ssdt_stub_not_implemented() which
 * returns STATUS_NOT_IMPLEMENTED.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"

/* ---- Handler signature --------------------------------------------------- */

/* All SSDT handlers take up to 6 uint64_t arguments (matching the
 * x64 syscall ABI: RCX/RDX/R8/R9/stack[0]/stack[1] for Windows,
 * or RDI/RSI/RDX/R10/R8/R9 for the current INT 0x80 path). */
typedef NTSTATUS (*SSDT_HANDLER)(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6);

/* ---- Table structure ----------------------------------------------------- */

#define SSDT_MAIN_MAX       1024    /* main table capacity (indices 0x000-0x3FF) */
#define SSDT_SHADOW_MAX     1024    /* shadow table capacity (indices 0x000-0x3FF) */

#define SSDT_TABLE_MAIN     0       /* table selector for main SSDT */
#define SSDT_TABLE_SHADOW   1       /* table selector for Win32k shadow */

#define SSDT_TABLE_SHIFT    12      /* bits 13:12 select table */
#define SSDT_INDEX_MASK     0x0FFF  /* bits 11:0 = index within table */

typedef struct {
    SSDT_HANDLER *handlers;     /* array of function pointers */
    /* count: registered extent (highest registered index+1; starts at the
     * declared service count). NOT a bound, NOT the live total -- use max for
     * bounds and implemented for the live count. */
    uint32_t      count;
    uint32_t      max;          /* handler-array capacity -- the ONLY dispatch/register bound */
    uint32_t      implemented;  /* number of non-stub (live) entries */
    const char   *name;         /* "main" or "shadow" */
} SSDT_TABLE;

/* ---- Public API ---------------------------------------------------------- */

/* Initialize main and shadow SSDT tables. All slots default to
 * STATUS_NOT_IMPLEMENTED stub. Call once during Phase 3 init. */
void ssdt_init(void);

/* Dispatch a syscall by service number. Extracts table selector
 * from bits 13:12, index from bits 11:0, calls the handler.
 * Returns NTSTATUS. Called from syscall entry path. */
NTSTATUS ssdt_dispatch(uint32_t service_number,
                       uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6);

/* Register a handler for a specific service number.
 * Replaces the STATUS_NOT_IMPLEMENTED stub with the real handler.
 * Returns 0 on success, -1 if index out of range. */
int ssdt_register(uint32_t service_number, SSDT_HANDLER handler);

/* Get the SSDT table struct for inspection (e.g., for NtQuerySystemInformation). */
const SSDT_TABLE *ssdt_get_table(uint32_t table_id);

/* Default stub handler -- returns STATUS_NOT_IMPLEMENTED. */
NTSTATUS ssdt_stub_not_implemented(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6);
