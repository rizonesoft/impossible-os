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
#include "kernel/klog.h"

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

    klog(LOG_INFO, "ssdt", "SSDT initialized: %u main slots, shadow stub ready",
         (uint64_t)SSDT_MAIN_COUNT);
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
