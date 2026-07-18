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
#include "kernel/nt/nt_audit.h"
#include "kernel/nt/syscall_filter.h"
#include "kernel/nt/pledge.h"   /* pledge_check_syscall / pledge_terminate */
#include "kernel/sched/task.h"  /* thread_current() for per-thread previous_mode */
#include "kernel/klog.h"

/* ---- Previous-mode tracking ---------------------------------------------- */

/* Previous mode: 0 = KernelMode, 1 = UserMode.
 * Set by the syscall entry path before calling ssdt_dispatch; ZwXxx
 * wrappers save/restore it around their dispatch call.
 *
 * Stored PER-THREAD (struct thread.previous_mode), not in a global. This
 * removes the inter-thread clobber the old single global had: a
 * time-shared thread's previous mode is no longer overwritten when a
 * sibling thread makes a Zw kernel call between its yields (which
 * previously let a user syscall handler skip ProbeFor*IfUser()).
 *
 * NOT yet fully SMP-closed: ssdt_previous_mode() resolves the flag via
 * thread_current(), which still reads the GLOBAL current-thread cursor,
 * not per-CPU state. On real multi-CPU, CPU A could observe CPU B's
 * current thread. Closing that needs the per-CPU current-thread cursor
 * work (SMP phase-2 per-CPU run queues); tracked as the deferred item in
 * the ZwXxx alias-layer TODO.
 *
 * A NULL current thread (very early boot, before any thread exists)
 * defaults to KernelMode: there are no user syscalls in that window, and
 * early-boot Zw callers must NOT probe their kernel pointers. */
void ssdt_set_previous_mode(uint32_t mode)
{
    struct thread *t = thread_current();
    if (t)
        t->previous_mode = mode;
}

uint32_t ssdt_previous_mode(void)
{
    struct thread *t = thread_current();
    return t ? t->previous_mode : SSDT_KERNEL_MODE;
}

/* System-service flag (0x3B vs 0x1E bugcheck classification). Bracket the true
 * ring-3 -> ring-0 syscall dispatch only; NOT called from zw_dispatch, so a fault
 * inside a nested Zw within a user syscall still reads 1. A plain store (not a
 * ++/-- RMW): a thread runs one syscall at a time, so the flag never nests, and
 * the store carries the same per-thread resolution and not-yet-CPU-local SMP
 * caveat as previous_mode above -- no worse, and with no RMW lost-update risk. A
 * self-terminating syscall (noreturn thread_exit) leaves the flag set; the slot
 * is cleared on reuse alongside previous_mode. A NULL current thread (very early
 * boot) has no system service in flight. */
void ssdt_enter_system_service(void)
{
    struct thread *t = thread_current();
    if (t)
        t->in_system_service = 1;
}

void ssdt_leave_system_service(void)
{
    struct thread *t = thread_current();
    if (t)
        t->in_system_service = 0;
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

    /* Alignment is a caller contract: it MUST be a power of two (1, 2, 4,
     * 8, ...). A zero or non-power-of-two value would make (Alignment - 1)
     * a bogus mask -- 0 silently disables the check, 3 tests the wrong
     * bits and can accept a misaligned address. Reject those up front so a
     * bad caller cannot bypass the misalignment guard. */
    if (Alignment == 0 || (Alignment & (Alignment - 1)) != 0)
        return STATUS_DATATYPE_MISALIGNMENT;

    /* Alignment check (Alignment is a validated power of 2) */
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
    /* --- file / path class --- */
    { STATUS_NO_SUCH_FILE,          2    },  /* -> FILE_NOT_FOUND */
    { STATUS_OBJECT_PATH_NOT_FOUND, 3    },  /* -> PATH_NOT_FOUND */
    { STATUS_OBJECT_PATH_INVALID,   161  },  /* -> BAD_PATHNAME */
    { STATUS_OBJECT_NAME_INVALID,   123  },  /* -> INVALID_NAME */
    { (NTSTATUS)0x80000006,         18   },  /* NO_MORE_FILES -> NO_MORE_FILES */
    { STATUS_END_OF_FILE,           38   },  /* -> HANDLE_EOF */
    { (NTSTATUS)0xC00000BA,         5    },  /* FILE_IS_A_DIRECTORY -> ACCESS_DENIED */
    { (NTSTATUS)0xC0000103,         267  },  /* NOT_A_DIRECTORY -> DIRECTORY */
    { (NTSTATUS)0xC0000101,         145  },  /* DIRECTORY_NOT_EMPTY -> DIR_NOT_EMPTY */
    { STATUS_DISK_FULL,             112  },  /* -> DISK_FULL */
    { (NTSTATUS)0xC0000121,         5    },  /* CANNOT_DELETE -> ACCESS_DENIED */
    /* --- sharing / lock class --- */
    { (NTSTATUS)0xC0000043,   32   },  /* SHARING_VIOLATION -> SHARING_VIOLATION */
    { (NTSTATUS)0xC0000054,   33   },  /* FILE_LOCK_CONFLICT -> LOCK_VIOLATION */
    { (NTSTATUS)0xC0000055,   33   },  /* LOCK_NOT_GRANTED -> LOCK_VIOLATION */
    /* --- buffer / length class --- */
    { (NTSTATUS)0xC0000004,   24   },  /* INFO_LENGTH_MISMATCH -> BAD_LENGTH */
    { (NTSTATUS)0x80000005,   234  },  /* BUFFER_OVERFLOW -> MORE_DATA */
    { STATUS_NAME_TOO_LONG,   206  },  /* -> FILENAME_EXCED_RANGE */
    { (NTSTATUS)0xC000009A,   1450 },  /* INSUFFICIENT_RESOURCES -> NO_SYSTEM_RESOURCES */
    /* --- process / handle / device class --- */
    { (NTSTATUS)0xC0000024,   6    },  /* OBJECT_TYPE_MISMATCH -> INVALID_HANDLE */
    { (NTSTATUS)0xC000010A,   5    },  /* PROCESS_IS_TERMINATING -> ACCESS_DENIED */
    { STATUS_CHILD_PROCESS_BLOCKED, 367 },  /* -> ERROR_CHILD_PROCESS_BLOCKED */
    { (NTSTATUS)0xC0000010,   1    },  /* INVALID_DEVICE_REQUEST -> INVALID_FUNCTION */
    { (NTSTATUS)0xC00000BB,   50   },  /* NOT_SUPPORTED -> NOT_SUPPORTED */
    /* --- sync class (error-severity) --- */
    { (NTSTATUS)0xC0000046,   288  },  /* MUTANT_NOT_OWNED -> NOT_OWNER */
    { (NTSTATUS)0xC0000047,   298  },  /* SEMAPHORE_LIMIT_EXCEEDED -> TOO_MANY_POSTS */
    /* --- registry class --- */
    { (NTSTATUS)0xC000017C,   1018 },  /* KEY_DELETED -> KEY_DELETED */
    /* --- success-severity codes that map to a specific Win32 error --- */
    { (NTSTATUS)0x00000102,   1460 },  /* TIMEOUT -> ERROR_TIMEOUT */
    { (NTSTATUS)0x00000103,   997  },  /* PENDING -> ERROR_IO_PENDING */
    { (NTSTATUS)0x00000080,   735  },  /* ABANDONED_WAIT_0 -> ERROR_ABANDONED_WAIT_0 */
};

#define NT_TO_DOS_COUNT \
    (sizeof(s_nt_to_dos) / sizeof(s_nt_to_dos[0]))

uint32_t RtlNtStatusToDosError(NTSTATUS status)
{
    uint32_t i;

    if (status == 0)
        return 0;  /* exact STATUS_SUCCESS -> ERROR_SUCCESS */

    /* Table lookup runs BEFORE the NT_SUCCESS fallback so success-severity
     * codes that Windows maps to a specific error (TIMEOUT, PENDING,
     * ABANDONED) get their real Win32 code instead of ERROR_SUCCESS. */
    for (i = 0; i < NT_TO_DOS_COUNT; i++) {
        if (s_nt_to_dos[i].nt == status)
            return s_nt_to_dos[i].dos;
    }

    /* Not in the table: any other success-severity status is a benign
     * success (ERROR_SUCCESS); everything else is an unmapped error. */
    if (NT_SUCCESS(status))
        return 0;
    return 317;  /* ERROR_MR_MID_NOT_FOUND */
}

/* ---- Static tables ------------------------------------------------------- */

static SSDT_HANDLER s_main_handlers[SSDT_MAIN_MAX];
static SSDT_HANDLER s_shadow_handlers[SSDT_SHADOW_MAX];

/* Bind the service-number metadata invariants to the REAL capacity constant
 * (SSDT_MAIN_MAX) here, where both ssdt.h and service_numbers.h are visible.
 * service_numbers.h asserts against a literal 1024; if SSDT_MAIN_MAX is ever
 * lowered, these catch declared metadata that would exceed the array. */
_Static_assert(SSDT_MAIN_COUNT <= SSDT_MAIN_MAX,
    "SSDT_MAIN_COUNT must fit the main handler array (SSDT_MAIN_MAX)");
_Static_assert(SSDT_LAST_MAIN_INDEX < SSDT_MAIN_MAX,
    "SSDT_LAST_MAIN_INDEX must be a valid index into the main handler array");

static SSDT_TABLE s_main_table = {
    .handlers    = s_main_handlers,
    .count       = SSDT_MAIN_COUNT,
    .max         = SSDT_MAIN_MAX,
    .implemented = 0,
    .name        = "main",
};

static SSDT_TABLE s_shadow_table = {
    .handlers    = s_shadow_handlers,
    .count       = 0,          /* empty until Win32k fills it */
    .max         = SSDT_SHADOW_MAX,
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

/* Cold audited path: kept out-of-line so the common no-hook syscall path in
 * ssdt_dispatch reserves no session frame and makes no call -- it is just the
 * inline nt_audit_hook_count() load + branch + the handler call. One session
 * spans PRE -> handler -> POST so an AUDIT_BOTH hook cannot be unregistered
 * mid-syscall; a PRE hook returning non-SUCCESS blocks the syscall. */
static NTSTATUS __attribute__((noinline))
ssdt_dispatch_audited(SSDT_HANDLER handler, uint32_t service_number,
                      uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint64_t audit_args[6] = { a1, a2, a3, a4, a5, a6 };
    nt_audit_session_t sess;
    NTSTATUS pre = nt_audit_begin(service_number, audit_args, &sess);
    if (pre != STATUS_SUCCESS)
        return pre;
    NTSTATUS handler_status = handler(a1, a2, a3, a4, a5, a6);
    /* Capture + clear this thread's pledge provenance BEFORE the POST hooks run.
     * A POST hook may issue a nested Zw call whose dispatch tail would otherwise
     * exchange this frame's flag and terminate mid-audit (stranding hook refs).
     * Consuming it here makes provenance dispatch-frame-scoped; termination
     * happens only AFTER nt_audit_end releases the session. */
    {
        /* pledge_pending is thread-local (set + consumed on the same thread in
         * the same syscall; migration carries the field and the context switch
         * is the barrier), so a relaxed load + conditional plain clear replaces
         * a locked atomic RMW on the universal syscall hot path -- the flag is
         * set only on the rare violation. Capture BEFORE the POST hooks so a
         * nested Zw from a hook cannot consume this frame's provenance. */
        struct thread *cth = thread_current();
        int pending = 0;
        if (cth && __atomic_load_n(&cth->pledge_pending, __ATOMIC_RELAXED)) {
            cth->pledge_pending = 0;
            pending = 1;
        }
        nt_audit_end(&sess, service_number, audit_args, handler_status);
        if (pending)
            pledge_terminate(service_number);   /* noreturn */
    }
    return handler_status;
}

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

    /* Bound by the per-table handler-array capacity, NOT the hardcoded
     * main-table size: the shadow array may be sized independently, and an
     * index in [table->max, SSDT_MAIN_MAX) would otherwise be an OOB call.
     * Unregistered in-range slots hold ssdt_stub_not_implemented.
     * SCOPE-GAP-ALLOWED: out-of-range service number returns NOT_IMPLEMENTED is
     * the Windows-correct terminal behavior, not a stub for deferred work. */
    if (index >= table->max)
        return STATUS_NOT_IMPLEMENTED;   /* SCOPE-GAP-ALLOWED: Windows-correct terminal status */

    /* Per-process syscall filter (seccomp / SystemCallDisablePolicy parity):
     * runs BEFORE the audit hooks and the handler, per the section contract
     * (bitmap filter first). Zero cost when no process is filtered -- the
     * inlined relaxed-atomic load of g_syscall_filter_count short-circuits.
     * A denied index returns STATUS_ACCESS_DENIED (audit mode logs + allows). */
    if (syscall_filter_active() != 0) {
        NTSTATUS filt = syscall_filter_check(table_id, index, service_number);
        if (filt != STATUS_SUCCESS)
            return filt;
    }

    /* Coarse per-syscall pledge gate. Runs AFTER the bitmap filter and BEFORE
     * the audit hooks, so a violation terminates the process with NO in-flight
     * audit session to strand. Only user-originated calls are gated (a kernel
     * Zw call is the kernel acting, not the pledged process). */
    {
        struct task *pt = task_current();
        if (pt && __atomic_load_n(&pt->pledge_mask, __ATOMIC_ACQUIRE) != 0 &&
            ssdt_previous_mode() == SSDT_USER_MODE) {
            if (pledge_check_syscall(pt, table_id, index, service_number)
                    != STATUS_SUCCESS)
                pledge_terminate(service_number);   /* noreturn */
        }
    }

    /* Audited path handles its own pledge-provenance consumption + termination
     * inside ssdt_dispatch_audited (before POST hooks / after nt_audit_end). */
    if (nt_audit_hook_count() != 0)
        return ssdt_dispatch_audited(table->handlers[index], service_number,
                                     a1, a2, a3, a4, a5, a6);

    /* Fast path (no audit hook): the handler returns directly here. Fine
     * handler-side pledge checks set THIS thread's pledge_pending and return
     * STATUS_PLEDGE_VIOLATION; terminate on the provenance FLAG (not the
     * NTSTATUS value -- an audit path could also carry it). A fine check returns
     * immediately on violation, so the handler makes no nested call after
     * setting the flag; the tail consumes it in the same dispatch frame. */
    {
        NTSTATUS st = table->handlers[index](a1, a2, a3, a4, a5, a6);
        /* Relaxed load + conditional plain clear (thread-local flag; see the
         * audited path). No locked RMW on the common unpledged syscall. */
        struct thread *cth = thread_current();
        if (cth && __atomic_load_n(&cth->pledge_pending, __ATOMIC_RELAXED)) {
            cth->pledge_pending = 0;
            pledge_terminate(service_number);   /* noreturn */
        }
        return st;
    }
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

    if (index >= table->max)
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
