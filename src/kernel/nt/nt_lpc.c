/* ============================================================================
 * nt_lpc.c -- Legacy LPC port SSDT handlers
 *
 * Reserves SSDT slots 0x0100-0x010E for the NT 3.x-5.x LPC syscall
 * surface. Each handler is a bounded stub that logs the call once and
 * returns a deferred-status sentinel until the LPC engine work in
 * 03-memory-concurrency/TODO-09-win32-ipc-extensions.md and the
 * matching 02-kernel-core/TODO-24-alpc-message-ports.md items ship.
 *
 * SCOPE-GAP-ALLOWED: 15 handlers in this file intentionally return the
 *                    deferred-status sentinel pending the legacy-IPC
 *                    LPC engine retrofit. The retrofit path is tracked
 *                    as a concrete checklist item that enumerates each
 *                    slot. When that item ships, every handler body in
 *                    this file is replaced with a call into lpc.c and
 *                    the sentinel comments are removed.
 *
 * Why register stubs rather than leave the default ssdt_stub_not_
 * implemented handler? Registering makes ownership visible in
 * /audit-ssdt output, routes the call through a named function so
 * runtime traces identify the missing subsystem concretely, and gives
 * the retrofit a single file to edit.
 * ============================================================================ */

#include "kernel/nt/nt_lpc.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/klog.h"

/* Stub return -- no runtime klog. The deferred-feature inventory is
 * maintained by test_nt_lpc_returns_deferred_status() in test_ob.c via
 * TEST_PENDING; that's the single canonical place "what is incomplete?"
 * gets announced. Adding a per-call klog here would just duplicate the
 * test signal and create N noise lines per boot.
 *
 * SCOPE-GAP-ALLOWED: pending the legacy-IPC LPC engine retrofit (the
 * concrete checklist item there enumerates every slot). */
#define LPC_STUB_BODY(name)                                                  \
    do {                                                                     \
        /* name is a bare identifier passed for documentation only;          \
         * the deferred contract is exercised by TEST_PENDING in tests. */   \
        (void)#name;                                                         \
        return STATUS_NOT_IMPLEMENTED;  /* SCOPE-GAP-ALLOWED */              \
    } while (0)

/* ---- 0x0100 NtCreatePort -------------------------------------------------
 * Server creates an LPC server port. XREF: legacy-IPC LPC engine retrofit. */
static NTSTATUS NtCreatePort_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    LPC_STUB_BODY(NtCreatePort);
}

/* ---- 0x0101 NtCreateWaitablePort -----------------------------------------
 * Server port that acts as a waitable object (signalled on message queue). */
static NTSTATUS NtCreateWaitablePort_handler(uint64_t a1, uint64_t a2,
                                             uint64_t a3, uint64_t a4,
                                             uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    LPC_STUB_BODY(NtCreateWaitablePort);
}

/* ---- 0x0102 NtConnectPort ------------------------------------------------
 * Client connects to a named server port under \RPC Control\. */
static NTSTATUS NtConnectPort_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    LPC_STUB_BODY(NtConnectPort);
}

/* ---- 0x0103 NtSecureConnectPort ------------------------------------------
 * NtConnectPort with SID validation via SeAccessCheck. */
static NTSTATUS NtSecureConnectPort_handler(uint64_t a1, uint64_t a2,
                                            uint64_t a3, uint64_t a4,
                                            uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    LPC_STUB_BODY(NtSecureConnectPort);
}

/* ---- 0x0104 NtAcceptConnectPort ------------------------------------------
 * Server accepts or rejects a pending connection request. */
static NTSTATUS NtAcceptConnectPort_handler(uint64_t a1, uint64_t a2,
                                            uint64_t a3, uint64_t a4,
                                            uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    LPC_STUB_BODY(NtAcceptConnectPort);
}

/* ---- 0x0105 NtCompleteConnectPort ---------------------------------------
 * Second phase of the server accept handshake. */
static NTSTATUS NtCompleteConnectPort_handler(uint64_t a1, uint64_t a2,
                                              uint64_t a3, uint64_t a4,
                                              uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    LPC_STUB_BODY(NtCompleteConnectPort);
}

/* ---- 0x0106 NtListenPort -------------------------------------------------
 * Blocking server-side dequeue of the next connection request. */
static NTSTATUS NtListenPort_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    LPC_STUB_BODY(NtListenPort);
}

/* ---- 0x0107 NtReplyPort --------------------------------------------------
 * Fire-and-forget reply to a prior request (no subsequent receive). */
static NTSTATUS NtReplyPort_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    LPC_STUB_BODY(NtReplyPort);
}

/* ---- 0x0108 NtReplyWaitReceivePort --------------------------------------
 * Send reply (if any), then block until a new message arrives. */
static NTSTATUS NtReplyWaitReceivePort_handler(uint64_t a1, uint64_t a2,
                                               uint64_t a3, uint64_t a4,
                                               uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    LPC_STUB_BODY(NtReplyWaitReceivePort);
}

/* ---- 0x0109 NtReplyWaitReceivePortEx ------------------------------------
 * NtReplyWaitReceivePort with an explicit Timeout parameter. */
static NTSTATUS NtReplyWaitReceivePortEx_handler(uint64_t a1, uint64_t a2,
                                                 uint64_t a3, uint64_t a4,
                                                 uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    LPC_STUB_BODY(NtReplyWaitReceivePortEx);
}

/* ---- 0x010A NtRequestPort ------------------------------------------------
 * Fire-and-forget request without waiting for a reply. */
static NTSTATUS NtRequestPort_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    LPC_STUB_BODY(NtRequestPort);
}

/* ---- 0x010B NtRequestWaitReplyPort --------------------------------------
 * Synchronous request+wait rendezvous (the canonical LPC RPC call). */
static NTSTATUS NtRequestWaitReplyPort_handler(uint64_t a1, uint64_t a2,
                                               uint64_t a3, uint64_t a4,
                                               uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    LPC_STUB_BODY(NtRequestWaitReplyPort);
}

/* ---- 0x010C NtImpersonateClientOfPort -----------------------------------
 * Server impersonates the client's token for the duration of a call. */
static NTSTATUS NtImpersonateClientOfPort_handler(uint64_t a1, uint64_t a2,
                                                  uint64_t a3, uint64_t a4,
                                                  uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    LPC_STUB_BODY(NtImpersonateClientOfPort);
}

/* ---- 0x010D NtReadRequestData --------------------------------------------
 * Server reads an out-of-line buffer referenced by a received message. */
static NTSTATUS NtReadRequestData_handler(uint64_t a1, uint64_t a2,
                                          uint64_t a3, uint64_t a4,
                                          uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    LPC_STUB_BODY(NtReadRequestData);
}

/* ---- 0x010E NtWriteRequestData -------------------------------------------
 * Server writes into an out-of-line client buffer. */
static NTSTATUS NtWriteRequestData_handler(uint64_t a1, uint64_t a2,
                                           uint64_t a3, uint64_t a4,
                                           uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    LPC_STUB_BODY(NtWriteRequestData);
}

/* --- Registration -------------------------------------------------------- */

/* Check each ssdt_register return; boot_halt on any failure rather than
 * silently shipping a partially-wired LPC surface. Also guard against
 * prior registration (e.g., double nt_lpc_register_ssdt call from an
 * init-ordering bug) by confirming the slot holds ssdt_stub_not_
 * implemented before we overwrite. */
static int lpc_register_one(uint32_t svc, SSDT_HANDLER h, const char *name)
{
    extern NTSTATUS ssdt_stub_not_implemented(uint64_t, uint64_t, uint64_t,
                                              uint64_t, uint64_t, uint64_t);
    const SSDT_TABLE *tbl = ssdt_get_table(SSDT_TABLE_MAIN);
    uint32_t idx = svc & SSDT_INDEX_MASK;

    if (!tbl) {
        klog(LOG_ERROR, "nt/lpc", "LPC register %s: main SSDT absent", name);
        return -1;
    }
    /* Bounds check BEFORE the handlers[idx] read. SSDT_INDEX_MASK is
     * 0xFFF (4096 slots) but SSDT_MAIN_MAX is smaller (1024); catch a
     * mis-numbered service constant before it reads past the array. */
    if (idx >= SSDT_MAIN_MAX) {
        klog(LOG_ERROR, "nt/lpc",
             "LPC register %s: SSDT index 0x%x out of range (max 0x%x)",
             name, (uint64_t)idx, (uint64_t)SSDT_MAIN_MAX);
        return -1;
    }
    if (tbl->handlers[idx] != ssdt_stub_not_implemented) {
        klog(LOG_ERROR, "nt/lpc",
             "LPC register collision at SSDT 0x%x (%s): slot already taken",
             (uint64_t)svc, name);
        return -1;
    }
    if (ssdt_register(svc, h) != 0) {
        klog(LOG_ERROR, "nt/lpc", "LPC register %s: ssdt_register failed", name);
        return -1;
    }
    return 0;
}

int nt_lpc_register_ssdt(void)
{
    int failures = 0;

    failures += lpc_register_one(SSDT_NtCreatePort,
                                 (SSDT_HANDLER)NtCreatePort_handler,
                                 "NtCreatePort") != 0;
    failures += lpc_register_one(SSDT_NtCreateWaitablePort,
                                 (SSDT_HANDLER)NtCreateWaitablePort_handler,
                                 "NtCreateWaitablePort") != 0;
    failures += lpc_register_one(SSDT_NtConnectPort,
                                 (SSDT_HANDLER)NtConnectPort_handler,
                                 "NtConnectPort") != 0;
    failures += lpc_register_one(SSDT_NtSecureConnectPort,
                                 (SSDT_HANDLER)NtSecureConnectPort_handler,
                                 "NtSecureConnectPort") != 0;
    failures += lpc_register_one(SSDT_NtAcceptConnectPort,
                                 (SSDT_HANDLER)NtAcceptConnectPort_handler,
                                 "NtAcceptConnectPort") != 0;
    failures += lpc_register_one(SSDT_NtCompleteConnectPort,
                                 (SSDT_HANDLER)NtCompleteConnectPort_handler,
                                 "NtCompleteConnectPort") != 0;
    failures += lpc_register_one(SSDT_NtListenPort,
                                 (SSDT_HANDLER)NtListenPort_handler,
                                 "NtListenPort") != 0;
    failures += lpc_register_one(SSDT_NtReplyPort,
                                 (SSDT_HANDLER)NtReplyPort_handler,
                                 "NtReplyPort") != 0;
    failures += lpc_register_one(SSDT_NtReplyWaitReceivePort,
                                 (SSDT_HANDLER)NtReplyWaitReceivePort_handler,
                                 "NtReplyWaitReceivePort") != 0;
    failures += lpc_register_one(SSDT_NtReplyWaitReceivePortEx,
                                 (SSDT_HANDLER)NtReplyWaitReceivePortEx_handler,
                                 "NtReplyWaitReceivePortEx") != 0;
    failures += lpc_register_one(SSDT_NtRequestPort,
                                 (SSDT_HANDLER)NtRequestPort_handler,
                                 "NtRequestPort") != 0;
    failures += lpc_register_one(SSDT_NtRequestWaitReplyPort,
                                 (SSDT_HANDLER)NtRequestWaitReplyPort_handler,
                                 "NtRequestWaitReplyPort") != 0;
    failures += lpc_register_one(SSDT_NtImpersonateClientOfPort,
                                 (SSDT_HANDLER)NtImpersonateClientOfPort_handler,
                                 "NtImpersonateClientOfPort") != 0;
    failures += lpc_register_one(SSDT_NtReadRequestData,
                                 (SSDT_HANDLER)NtReadRequestData_handler,
                                 "NtReadRequestData") != 0;
    failures += lpc_register_one(SSDT_NtWriteRequestData,
                                 (SSDT_HANDLER)NtWriteRequestData_handler,
                                 "NtWriteRequestData") != 0;

    if (failures != 0) {
        klog(LOG_ERROR, "nt/lpc",
             "NT legacy LPC: %d of 15 handlers FAILED to register",
             (uint64_t)failures);
        return failures;
    }
    klog(LOG_INFO, "nt/lpc",
         "NT legacy LPC: 15 stub handlers registered (SSDT 0x0100-0x010E)");
    return 0;
}
