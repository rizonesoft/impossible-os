/* ============================================================================
 * nt_alpc.c -- Modern ALPC port SSDT handlers (TODO-05 §31)
 *
 * Reserves SSDT slots 0x010F-0x011E for the Vista+ ALPC syscall
 * surface. Each handler is a bounded stub that logs the call once
 * (atomic one-time flag, load-fast / CAS-on-transition) and returns a
 * deferred-status sentinel until the ALPC engine in
 * 02-kernel-core/TODO-12 §8 is implemented.
 *
 * SCOPE-GAP-ALLOWED: 16 handlers in this file intentionally return
 *                    STATUS_NOT_IMPLEMENTED pending TODO-12 §8 ALPC
 *                    engine. Retrofit path is a concrete checklist
 *                    item in TODO-12 §8 that enumerates each slot.
 *                    When that item ships, every handler body in this
 *                    file is replaced with a call into the ALPC
 *                    subsystem and the sentinel comments are removed.
 *
 * Registration helper (alpc_register_one) detects collisions with
 * ssdt_stub_not_implemented, captures ssdt_register return, and
 * per-slot logs failures -- mirroring the §20 LPC pattern.
 * ============================================================================ */

#include "kernel/nt/nt_alpc.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/klog.h"

/* Log the first call to each handler so missing ALPC calls are visible
 * in serial/klog without drowning the log on repeat calls.
 *
 * Fast path: relaxed __atomic_load_n handles the steady state in one
 * uncontended read. Only the first-ever call per handler enters the
 * compare_exchange, and only the winning CPU logs. Prevents user-mode
 * callers who hammer an unimplemented ALPC syscall from forcing
 * cross-CPU cacheline traffic beyond the first transition. */
#define ALPC_STUB_BODY(name)                                                 \
    do {                                                                     \
        static uint32_t s_warned = 0;                                        \
        uint32_t expected = 0;                                               \
        if (__atomic_load_n(&s_warned, __ATOMIC_RELAXED) == 0                \
            && __atomic_compare_exchange_n(&s_warned, &expected, 1, 0,       \
                                           __ATOMIC_ACQ_REL,                 \
                                           __ATOMIC_RELAXED)) {              \
            klog(LOG_WARN, "nt/alpc",                                        \
                 #name " called -- ALPC subsystem deferred to TODO-12 §8");  \
        }                                                                    \
        return STATUS_NOT_IMPLEMENTED;  /* SCOPE-GAP-ALLOWED */              \
    } while (0)

/* ---- 0x010F NtAlpcCreatePort -------------------------------------------- */
static NTSTATUS NtAlpcCreatePort_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcCreatePort);
}

/* ---- 0x0110 NtAlpcConnectPort ------------------------------------------- */
static NTSTATUS NtAlpcConnectPort_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcConnectPort);
}

/* ---- 0x0111 NtAlpcConnectPortEx ----------------------------------------- */
static NTSTATUS NtAlpcConnectPortEx_handler(uint64_t a1, uint64_t a2,
                                            uint64_t a3, uint64_t a4,
                                            uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcConnectPortEx);
}

/* ---- 0x0112 NtAlpcAcceptConnectPort ------------------------------------- */
static NTSTATUS NtAlpcAcceptConnectPort_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcAcceptConnectPort);
}

/* ---- 0x0113 NtAlpcSendWaitReceivePort ----------------------------------- */
static NTSTATUS NtAlpcSendWaitReceivePort_handler(uint64_t a1, uint64_t a2,
                                                  uint64_t a3, uint64_t a4,
                                                  uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcSendWaitReceivePort);
}

/* ---- 0x0114 NtAlpcDisconnectPort ---------------------------------------- */
static NTSTATUS NtAlpcDisconnectPort_handler(uint64_t a1, uint64_t a2,
                                             uint64_t a3, uint64_t a4,
                                             uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcDisconnectPort);
}

/* ---- 0x0115 NtAlpcCancelMessage ----------------------------------------- */
static NTSTATUS NtAlpcCancelMessage_handler(uint64_t a1, uint64_t a2,
                                            uint64_t a3, uint64_t a4,
                                            uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcCancelMessage);
}

/* ---- 0x0116 NtAlpcCreatePortSection ------------------------------------- */
static NTSTATUS NtAlpcCreatePortSection_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcCreatePortSection);
}

/* ---- 0x0117 NtAlpcDeletePortSection ------------------------------------- */
static NTSTATUS NtAlpcDeletePortSection_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcDeletePortSection);
}

/* ---- 0x0118 NtAlpcCreateSectionView ------------------------------------- */
static NTSTATUS NtAlpcCreateSectionView_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcCreateSectionView);
}

/* ---- 0x0119 NtAlpcDeleteSectionView ------------------------------------- */
static NTSTATUS NtAlpcDeleteSectionView_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcDeleteSectionView);
}

/* ---- 0x011A NtAlpcCreateResourceReserve --------------------------------- */
static NTSTATUS NtAlpcCreateResourceReserve_handler(uint64_t a1, uint64_t a2,
                                                    uint64_t a3, uint64_t a4,
                                                    uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcCreateResourceReserve);
}

/* ---- 0x011B NtAlpcDeleteResourceReserve --------------------------------- */
static NTSTATUS NtAlpcDeleteResourceReserve_handler(uint64_t a1, uint64_t a2,
                                                    uint64_t a3, uint64_t a4,
                                                    uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcDeleteResourceReserve);
}

/* ---- 0x011C NtAlpcQueryInformation -------------------------------------- */
static NTSTATUS NtAlpcQueryInformation_handler(uint64_t a1, uint64_t a2,
                                               uint64_t a3, uint64_t a4,
                                               uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcQueryInformation);
}

/* ---- 0x011D NtAlpcSetInformation ---------------------------------------- */
static NTSTATUS NtAlpcSetInformation_handler(uint64_t a1, uint64_t a2,
                                             uint64_t a3, uint64_t a4,
                                             uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcSetInformation);
}

/* ---- 0x011E NtAlpcQueryInformationMessage ------------------------------- */
static NTSTATUS NtAlpcQueryInformationMessage_handler(uint64_t a1, uint64_t a2,
                                                      uint64_t a3, uint64_t a4,
                                                      uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcQueryInformationMessage);
}

/* --- Registration -------------------------------------------------------- */

/* Per-slot register-and-verify helper. Detects collision with the
 * default stub (catches double registration or conflicting claims on
 * the same SSDT index) and reports per-slot ssdt_register failures. */
static int alpc_register_one(uint32_t svc, SSDT_HANDLER h, const char *name)
{
    extern NTSTATUS ssdt_stub_not_implemented(uint64_t, uint64_t, uint64_t,
                                              uint64_t, uint64_t, uint64_t);
    const SSDT_TABLE *tbl = ssdt_get_table(SSDT_TABLE_MAIN);
    uint32_t idx = svc & 0xFFF;

    if (!tbl) {
        klog(LOG_ERROR, "nt/alpc",
             "ALPC register %s: main SSDT absent", name);
        return -1;
    }
    if (tbl->handlers[idx] != ssdt_stub_not_implemented) {
        klog(LOG_ERROR, "nt/alpc",
             "ALPC register collision at SSDT 0x%x (%s): slot already taken",
             (uint64_t)svc, name);
        return -1;
    }
    if (ssdt_register(svc, h) != 0) {
        klog(LOG_ERROR, "nt/alpc",
             "ALPC register %s: ssdt_register failed", name);
        return -1;
    }
    return 0;
}

int nt_alpc_register_ssdt(void)
{
    int failures = 0;

    failures += alpc_register_one(SSDT_NtAlpcCreatePort,
                                  (SSDT_HANDLER)NtAlpcCreatePort_handler,
                                  "NtAlpcCreatePort") != 0;
    failures += alpc_register_one(SSDT_NtAlpcConnectPort,
                                  (SSDT_HANDLER)NtAlpcConnectPort_handler,
                                  "NtAlpcConnectPort") != 0;
    failures += alpc_register_one(SSDT_NtAlpcConnectPortEx,
                                  (SSDT_HANDLER)NtAlpcConnectPortEx_handler,
                                  "NtAlpcConnectPortEx") != 0;
    failures += alpc_register_one(SSDT_NtAlpcAcceptConnectPort,
                                  (SSDT_HANDLER)NtAlpcAcceptConnectPort_handler,
                                  "NtAlpcAcceptConnectPort") != 0;
    failures += alpc_register_one(SSDT_NtAlpcSendWaitReceivePort,
                                  (SSDT_HANDLER)NtAlpcSendWaitReceivePort_handler,
                                  "NtAlpcSendWaitReceivePort") != 0;
    failures += alpc_register_one(SSDT_NtAlpcDisconnectPort,
                                  (SSDT_HANDLER)NtAlpcDisconnectPort_handler,
                                  "NtAlpcDisconnectPort") != 0;
    failures += alpc_register_one(SSDT_NtAlpcCancelMessage,
                                  (SSDT_HANDLER)NtAlpcCancelMessage_handler,
                                  "NtAlpcCancelMessage") != 0;
    failures += alpc_register_one(SSDT_NtAlpcCreatePortSection,
                                  (SSDT_HANDLER)NtAlpcCreatePortSection_handler,
                                  "NtAlpcCreatePortSection") != 0;
    failures += alpc_register_one(SSDT_NtAlpcDeletePortSection,
                                  (SSDT_HANDLER)NtAlpcDeletePortSection_handler,
                                  "NtAlpcDeletePortSection") != 0;
    failures += alpc_register_one(SSDT_NtAlpcCreateSectionView,
                                  (SSDT_HANDLER)NtAlpcCreateSectionView_handler,
                                  "NtAlpcCreateSectionView") != 0;
    failures += alpc_register_one(SSDT_NtAlpcDeleteSectionView,
                                  (SSDT_HANDLER)NtAlpcDeleteSectionView_handler,
                                  "NtAlpcDeleteSectionView") != 0;
    failures += alpc_register_one(SSDT_NtAlpcCreateResourceReserve,
                                  (SSDT_HANDLER)NtAlpcCreateResourceReserve_handler,
                                  "NtAlpcCreateResourceReserve") != 0;
    failures += alpc_register_one(SSDT_NtAlpcDeleteResourceReserve,
                                  (SSDT_HANDLER)NtAlpcDeleteResourceReserve_handler,
                                  "NtAlpcDeleteResourceReserve") != 0;
    failures += alpc_register_one(SSDT_NtAlpcQueryInformation,
                                  (SSDT_HANDLER)NtAlpcQueryInformation_handler,
                                  "NtAlpcQueryInformation") != 0;
    failures += alpc_register_one(SSDT_NtAlpcSetInformation,
                                  (SSDT_HANDLER)NtAlpcSetInformation_handler,
                                  "NtAlpcSetInformation") != 0;
    failures += alpc_register_one(SSDT_NtAlpcQueryInformationMessage,
                                  (SSDT_HANDLER)NtAlpcQueryInformationMessage_handler,
                                  "NtAlpcQueryInformationMessage") != 0;

    if (failures != 0) {
        klog(LOG_ERROR, "nt/alpc",
             "NT modern ALPC: %d of 16 handlers FAILED to register",
             (uint64_t)failures);
        return failures;
    }
    klog(LOG_INFO, "nt/alpc",
         "NT modern ALPC: 16 stub handlers registered (SSDT 0x010F-0x011E)");
    return 0;
}
