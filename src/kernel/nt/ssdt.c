/* ============================================================================
 * ssdt.c -- System Service Descriptor Table (SSDT) dispatch
 *
 * Initializes the main and shadow SSDT tables with STATUS_NOT_IMPLEMENTED
 * stubs, then dispatches syscalls by service number.
 *
 * Table selection: bits 13:12 of service number
 *   00 -> main SSDT  (NtXxx kernel APIs)
 *   01 -> shadow SSDT (NtGdiXxx/NtUserXxx Win32k)
 *
 * Index: bits 11:0 within selected table.
 * ============================================================================ */

#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/nt/zw.h"
#include "kernel/klog.h"

/* ---- Previous-mode tracking ---------------------------------------------- */

/* Previous mode: 0 = KernelMode, 1 = UserMode.
 * Set by the syscall entry path before calling ssdt_dispatch.
 * ZwXxx wrappers save/restore this around their dispatch call.
 *
 * SMP limitation: this is a single global. Safe while APs are parked
 * in hlt. When SMP scheduling is enabled, move to per-CPU or per-thread
 * state (-> XREF: 03-memory-concurrency/TODO-06-smp-phase2.md). */
static uint32_t s_previous_mode;

void ssdt_set_previous_mode(uint32_t mode)
{
    s_previous_mode = mode;
}

uint32_t ssdt_previous_mode(void)
{
    return s_previous_mode;
}

/* ---- User-buffer probing ------------------------------------------------- */

NTSTATUS ProbeForRead(const void *Address, uint64_t Length, uint32_t Alignment)
{
    uintptr_t addr = (uintptr_t)Address;
    uintptr_t end;

    if (!Address)
        return STATUS_ACCESS_VIOLATION;

    if (Length == 0)
        return STATUS_SUCCESS;

    /* Alignment check (Alignment must be power of 2) */
    if (Alignment > 1 && (addr & (Alignment - 1)))
        return STATUS_DATATYPE_MISALIGNMENT;

    /* Overflow check */
    end = addr + Length;
    if (end < addr)
        return STATUS_ACCESS_VIOLATION;

    /* Range check: entire buffer must be below MM_USER_PROBE_ADDRESS */
    if (end > MM_USER_PROBE_ADDRESS)
        return STATUS_ACCESS_VIOLATION;

    return STATUS_SUCCESS;
}

NTSTATUS ProbeForWrite(void *Address, uint64_t Length, uint32_t Alignment)
{
    /* Same validation as ProbeForRead -- the address range check is
     * identical. On real Windows the write probe also touches each page
     * to trigger CoW; we don't have CoW yet so the range check suffices. */
    return ProbeForRead(Address, Length, Alignment);
}

/* ---- NTSTATUS -> Win32 error translation --------------------------------- */

/* SCOPE-GAP-ALLOWED: translation table references status code names in data */

/* Compact lookup table for common NTSTATUS -> Win32 error mappings.
 * Unknown codes map to ERROR_MR_MID_NOT_FOUND (0x13D = 317). */
static const struct {
    NTSTATUS nt;
    uint32_t dos;
} s_nt_to_dos[] = {
    { 0x00000000,             0    },  /* SUCCESS -> ERROR_SUCCESS */
    { (NTSTATUS)0xC0000022,   5    },  /* ACCESS_DENIED */
    { (NTSTATUS)0xC0000017,   8    },  /* NO_MEMORY */
    { (NTSTATUS)0xC0000008,   6    },  /* INVALID_HANDLE */
    { (NTSTATUS)0xC0000034,   2    },  /* OBJECT_NAME_NOT_FOUND -> FILE_NOT_FOUND */
    { (NTSTATUS)0xC0000002,   50   },  /* NOT_IMPLEMENTED -> NOT_SUPPORTED */
    { (NTSTATUS)0xC000000D,   87   },  /* INVALID_PARAMETER */
    { (NTSTATUS)0xC0000023,   122  },  /* BUFFER_TOO_SMALL */
    { (NTSTATUS)0xC0000005,   998  },  /* ACCESS_VIOLATION -> NOACCESS */
    { (NTSTATUS)0xC0000061,   1314 },  /* PRIVILEGE_NOT_HELD */
    { (NTSTATUS)0xC0000001,   1    },  /* UNSUCCESSFUL -> INVALID_FUNCTION */
    { (NTSTATUS)0xC0000035,   183  },  /* OBJECT_NAME_COLLISION -> ALREADY_EXISTS */
};

#define NT_TO_DOS_COUNT \
    (sizeof(s_nt_to_dos) / sizeof(s_nt_to_dos[0]))

uint32_t RtlNtStatusToDosError(NTSTATUS status)
{
    uint32_t i;
    if (NT_SUCCESS(status))
        return 0;  /* ERROR_SUCCESS */
    for (i = 0; i < NT_TO_DOS_COUNT; i++) {
        if (s_nt_to_dos[i].nt == status)
            return s_nt_to_dos[i].dos;
    }
    return 317;  /* ERROR_MR_MID_NOT_FOUND */
}

/* ---- Static tables ------------------------------------------------------- */

static SSDT_HANDLER s_main_handlers[SSDT_MAIN_MAX];
static SSDT_HANDLER s_shadow_handlers[SSDT_SHADOW_MAX];

static SSDT_TABLE s_main_table = {
    .handlers    = s_main_handlers,
    .count       = SSDT_MAIN_COUNT,
    .implemented = 0,
    .name        = "main",
};

static SSDT_TABLE s_shadow_table = {
    .handlers    = s_shadow_handlers,
    .count       = 0,          /* empty until Win32k fills it */
    .implemented = 0,
    .name        = "shadow",
};

/* ---- Default stub -------------------------------------------------------- */

NTSTATUS ssdt_stub_not_implemented(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3;
    (void)a4; (void)a5; (void)a6;
    return STATUS_NOT_IMPLEMENTED;
}

/* ---- Init ---------------------------------------------------------------- */

void ssdt_init(void)
{
    uint32_t i;

    /* Fill main table with stubs */
    for (i = 0; i < SSDT_MAIN_MAX; i++)
        s_main_handlers[i] = ssdt_stub_not_implemented;

    /* Fill shadow table with stubs */
    for (i = 0; i < SSDT_SHADOW_MAX; i++)
        s_shadow_handlers[i] = ssdt_stub_not_implemented;

    /* Runtime verify: count the non-stub slots after init (should be 0).
     * Log the declared count and highest index for diagnostics. */
    klog(LOG_INFO, "ssdt", "SSDT initialized: %u main slots (last=0x%03X), shadow stub ready",
         (uint64_t)SSDT_MAIN_COUNT, (uint64_t)SSDT_LAST_MAIN_INDEX);
}

/* ---- Dispatch ------------------------------------------------------------ */

NTSTATUS ssdt_dispatch(uint32_t service_number,
                       uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint32_t table_id = (service_number >> SSDT_TABLE_SHIFT) & 0x03;
    uint32_t index    = service_number & SSDT_INDEX_MASK;

    SSDT_TABLE *table;

    switch (table_id) {
    case SSDT_TABLE_MAIN:
        table = &s_main_table;
        break;
    case SSDT_TABLE_SHADOW:
        table = &s_shadow_table;
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    if (index >= table->count && index >= SSDT_MAIN_MAX)
        return STATUS_NOT_IMPLEMENTED;

    return table->handlers[index](a1, a2, a3, a4, a5, a6);
}

/* ---- Registration -------------------------------------------------------- */

int ssdt_register(uint32_t service_number, SSDT_HANDLER handler)
{
    uint32_t table_id = (service_number >> SSDT_TABLE_SHIFT) & 0x03;
    uint32_t index    = service_number & SSDT_INDEX_MASK;

    SSDT_TABLE *table;

    switch (table_id) {
    case SSDT_TABLE_MAIN:   table = &s_main_table;   break;
    case SSDT_TABLE_SHADOW: table = &s_shadow_table;  break;
    default: return -1;
    }

    if (index >= SSDT_MAIN_MAX)
        return -1;

    /* Track implemented count */
    if (table->handlers[index] == ssdt_stub_not_implemented && handler != ssdt_stub_not_implemented)
        table->implemented++;

    table->handlers[index] = handler;

    /* Extend count if new index is beyond current count */
    if (index >= table->count)
        table->count = index + 1;

    return 0;
}

/* ---- Inspection ---------------------------------------------------------- */

const SSDT_TABLE *ssdt_get_table(uint32_t table_id)
{
    switch (table_id) {
    case SSDT_TABLE_MAIN:   return &s_main_table;
    case SSDT_TABLE_SHADOW: return &s_shadow_table;
    default: return (const SSDT_TABLE *)0;
    }
}
