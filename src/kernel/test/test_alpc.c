/* ============================================================================
 * test_alpc.c -- ALPC ABI constants & layout tests + port-object tests
 *
 * Covers the static contract shipped in include/kernel/ipc/alpc.h:
 * message header size and per-field offsets (via static asserts
 * re-validated at runtime), message type code uniqueness and range,
 * maximum inline message length, and non-overlap of ALPC_PORTFLG_* and
 * ALPC_MSGFLG_* bit groups.
 *
 * adds port-object tests: ObpAlpcPortType registration, \RPC Control
 * namespace entry, AlpcCreatePort success paths (named + unnamed), and
 * namespace lookup of a named port.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/test/scratch.h" /* TEST_SCRATCH_KBUF -- retrofit (b) */
#include "kernel/test/race_barrier.h" /* test_race_barrier_t -- retrofit (c) */
#include "kernel/test/klog_suppress.h" /* TEST_KLOG_SUPPRESS -- retrofit (a) */
#include "kernel/mm/heap.h" /* kmalloc_fail_next -- retrofit (a) */
#include "kernel/ipc/alpc.h"
#include "kernel/ipc/alpc_port.h"
#include "kernel/quota/quota.h"  /* central sender-quota assertions */
#include "kernel/nt/nt_types.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/sched/task.h"
#include "kernel/atomic.h"              /* Section 7: atomic_read(ref_count) */
#include "kernel/security/token.h"     /* Section 7: ACCESS_TOKEN, RevertToSelf */
#include "kernel/security/sid.h"       /* Section 7: SID, RtlEqualSid */
#include "kernel/nt/zw.h"              /* Section 7: SSDT_{KERNEL,USER}_MODE */

/* snprintf is not in freestanding kernel headers; declared extern here
 * at file scope for the -9 LEAK retrofit cleanup helper. */
extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* ---- Layout (PORT_MESSAGE is 40 bytes, fields at documented offsets) ---- */

static void test_alpc_port_message_layout(void)
{
    TEST_ASSERT_EQ(sizeof(PORT_MESSAGE), 40,
                   "PORT_MESSAGE sizeof == 40");
    TEST_ASSERT_EQ(__builtin_offsetof(PORT_MESSAGE, TotalLength), 0,
                   "PORT_MESSAGE.TotalLength at offset 0");
    TEST_ASSERT_EQ(__builtin_offsetof(PORT_MESSAGE, DataLength), 2,
                   "PORT_MESSAGE.DataLength at offset 2");
    TEST_ASSERT_EQ(__builtin_offsetof(PORT_MESSAGE, Type), 4,
                   "PORT_MESSAGE.Type at offset 4");
    TEST_ASSERT_EQ(__builtin_offsetof(PORT_MESSAGE, DataInfoOffset), 6,
                   "PORT_MESSAGE.DataInfoOffset at offset 6");
    TEST_ASSERT_EQ(__builtin_offsetof(PORT_MESSAGE, ClientId), 8,
                   "PORT_MESSAGE.ClientId at offset 8");
    TEST_ASSERT_EQ(__builtin_offsetof(PORT_MESSAGE, MessageId), 24,
                   "PORT_MESSAGE.MessageId at offset 24");
    TEST_ASSERT_EQ(__builtin_offsetof(PORT_MESSAGE, CallbackId), 32,
                   "PORT_MESSAGE.CallbackId at offset 32");
    TEST_ASSERT_EQ(sizeof(CLIENT_ID), 16, "CLIENT_ID sizeof == 16");
    TEST_ASSERT_EQ(__builtin_offsetof(ALPC_MESSAGE, Body),
                   sizeof(PORT_MESSAGE),
                   "ALPC_MESSAGE.Body follows Header");
}

/* ---- Message type codes are 1..10 and pairwise unique ------------------- */

static void test_alpc_msg_types_unique(void)
{
    static const struct { uint32_t val; const char *name; } types[] = {
        { ALPC_MSG_TYPE_REQUEST,             "REQUEST=1"   },
        { ALPC_MSG_TYPE_REPLY,               "REPLY=2"     },
        { ALPC_MSG_TYPE_DATAGRAM,            "DATAGRAM=3"  },
        { ALPC_MSG_TYPE_LOST_REPLY,          "LOST_REPLY=4"},
        { ALPC_MSG_TYPE_PORT_CLOSED,         "PORT_CLOSED=5"},
        { ALPC_MSG_TYPE_CLIENT_DIED,         "CLIENT_DIED=6"},
        { ALPC_MSG_TYPE_EXCEPTION,           "EXCEPTION=7" },
        { ALPC_MSG_TYPE_DEBUG_EVENT,         "DEBUG_EVENT=8"},
        { ALPC_MSG_TYPE_ERROR_EVENT,         "ERROR_EVENT=9"},
        { ALPC_MSG_TYPE_CONNECTION_REQUEST,  "CONN_REQ=10" },
    };
    uint32_t expected = 1;
    for (uint32_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        TEST_ASSERT_EQ(types[i].val, expected, types[i].name);
        expected++;
        /* Pairwise uniqueness: the strict-monotonic increment above
         * proves all values are distinct without an O(n^2) loop. */
    }
}

/* ---- Max inline message length ----------------------------------------- */

/* ---- ALPC_PORTFLG_* bits are pairwise non-overlapping ------------------ */

static void test_alpc_portflg_nonoverlap(void)
{
    uint32_t or_bits  = ALPC_PORTFLG_LPC_MODE
                      | ALPC_PORTFLG_WAITABLE_PORT
                      | ALPC_PORTFLG_ALLOW_DUP_OBJECT
                      | ALPC_PORTFLG_SYSTEM_PROCESS;
    uint32_t sum_bits = ALPC_PORTFLG_LPC_MODE
                      + ALPC_PORTFLG_WAITABLE_PORT
                      + ALPC_PORTFLG_ALLOW_DUP_OBJECT
                      + ALPC_PORTFLG_SYSTEM_PROCESS;
    TEST_ASSERT_EQ(or_bits, sum_bits,
                   "ALPC_PORTFLG_* bits non-overlapping");
}

/* ---- ALPC_MSGFLG_* bits are pairwise non-overlapping ------------------- */

static void test_alpc_msgflg_nonoverlap(void)
{
    uint32_t or_bits  = ALPC_MSGFLG_REPLY_MESSAGE
                      | ALPC_MSGFLG_LPC_MODE
                      | ALPC_MSGFLG_RELEASE_MESSAGE
                      | ALPC_MSGFLG_SYNC_REQUEST
                      | ALPC_MSGFLG_WAIT_USER_MODE
                      | ALPC_MSGFLG_WAIT_PENDING_CALLBACKS;
    uint32_t sum_bits = ALPC_MSGFLG_REPLY_MESSAGE
                      + ALPC_MSGFLG_LPC_MODE
                      + ALPC_MSGFLG_RELEASE_MESSAGE
                      + ALPC_MSGFLG_SYNC_REQUEST
                      + ALPC_MSGFLG_WAIT_USER_MODE
                      + ALPC_MSGFLG_WAIT_PENDING_CALLBACKS;
    TEST_ASSERT_EQ(or_bits, sum_bits,
                   "ALPC_MSGFLG_* bits non-overlapping");
}

/* ---- ALPC_PORT_ATTRIBUTES layout --------------------------------------- */

static void test_alpc_port_attributes_layout(void)
{
    TEST_ASSERT_EQ(sizeof(ALPC_PORT_ATTRIBUTES), 80,
                   "ALPC_PORT_ATTRIBUTES sizeof == 80");
    TEST_ASSERT_EQ(__builtin_offsetof(ALPC_PORT_ATTRIBUTES, Flags), 0,
                   "PortAttr.Flags at offset 0");
    TEST_ASSERT_EQ(__builtin_offsetof(ALPC_PORT_ATTRIBUTES, SecurityQos), 8,
                   "PortAttr.SecurityQos at offset 8");
    TEST_ASSERT_EQ(__builtin_offsetof(ALPC_PORT_ATTRIBUTES, MaxMessageLength), 24,
                   "PortAttr.MaxMessageLength at offset 24");
    TEST_ASSERT_EQ(__builtin_offsetof(ALPC_PORT_ATTRIBUTES, MaxViewSize), 56,
                   "PortAttr.MaxViewSize at offset 56");
    TEST_ASSERT_EQ(__builtin_offsetof(ALPC_PORT_ATTRIBUTES, DupObjectTypes), 72,
                   "PortAttr.DupObjectTypes at offset 72");
}

/* ---- SECURITY_QUALITY_OF_SERVICE size contract ------------------------- */

static void test_alpc_sqos_layout(void)
{
    TEST_ASSERT_EQ(sizeof(SECURITY_QUALITY_OF_SERVICE), 12,
                   "SECURITY_QUALITY_OF_SERVICE sizeof == 12");
    TEST_ASSERT_EQ((uint32_t)SecurityAnonymous, 0u,
                   "SecurityAnonymous == 0");
    TEST_ASSERT_EQ((uint32_t)SecurityDelegation, 3u,
                   "SecurityDelegation == 3");
}

/* ---- Port object tests ---------------------------------------------- */

static void test_alpc_port_type_registered(void)
{
    TEST_ASSERT_NOT_NULL((void *)ObpAlpcPortType,
                         "ObpAlpcPortType non-NULL after alpc_init");
    if (ObpAlpcPortType) {
        /* name comparison via first-char check (strcmp would drag libc) */
        TEST_ASSERT(ObpAlpcPortType->name &&
                    ObpAlpcPortType->name[0] == 'A' &&
                    ObpAlpcPortType->name[1] == 'L' &&
                    ObpAlpcPortType->name[2] == 'P' &&
                    ObpAlpcPortType->name[3] == 'C',
                    "type name begins with 'ALPC'");
        TEST_ASSERT_EQ(ObpAlpcPortType->body_size, sizeof(ALPC_PORT),
                       "type body_size == sizeof(ALPC_PORT)");
    }
}

static void test_alpc_rpc_control_directory(void)
{
    void *dir = (void *)0;
    int rc = ObLookupObjectByName("\\RPC Control", ObpDirectoryType, 0, &dir);
    TEST_ASSERT_EQ(rc, 0, "ObLookupObjectByName(\\RPC Control) returns 0");
    TEST_ASSERT_NOT_NULL(dir, "\\RPC Control directory body is non-NULL");
    if (dir)
        ObDereferenceObject(dir);
}

/* -9 LEAK retrofit helper: unlink a named port from \RPC Control.
 *
 * AlpcCreatePort puts the port at ref=2 (handle + namespace entry).
 * Closing the handle drops to ref=1 but the directory entry still
 * pins the port body. Unlike \KernelObjects entries (set PERMANENT),
 * ObInsertObject does NOT set OB_FLAG_PERMANENT -- so
 * ObMakeTemporaryObject is a no-op here; only ObpRemoveFromDirectory
 * drops the pinning ref + frees the entry node.
 *
 * Idempotent: returns quietly if the port is already gone (lookup
 * fails). This lets paired-test helpers unlink once even when a
 * test intentionally created-and-destroyed a port.
 */
static void test_alpc_cleanup_named(const char *leaf)
{
    char path[96];
    void *body = (void *)0;
    void *rpc_dir = (void *)0;

    snprintf(path, sizeof(path), "\\RPC Control\\%s", leaf);
    if (ObLookupObjectByName(path, ObpAlpcPortType, 0, &body) != 0 || !body)
        return;
    ObMakeTemporaryObject(body);
    if (ObLookupObjectByName("\\RPC Control", ObpDirectoryType, 0,
                             &rpc_dir) == 0 && rpc_dir) {
        ObpRemoveFromDirectory(rpc_dir, body);
        ObDereferenceObject(rpc_dir);
    }
    ObDereferenceObject(body);
}

static void test_alpc_create_unnamed_port(void)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 (const char *)0,
                                 (ALPC_PORT_ATTRIBUTES *)0, &h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "AlpcCreatePort(NULL name) returns SUCCESS");
    /* HANDLE 0 is slot-0 = legitimate in this kernel; only
     * INVALID_HANDLE_VALUE ((HANDLE)-1) is the failure sentinel. */
    TEST_ASSERT_NEQ(h, INVALID_HANDLE_VALUE, "unnamed port handle valid");
    if (h != INVALID_HANDLE_VALUE)
        NtClose(&task_current()->handle_table, h);
}

static void test_alpc_create_named_port(void)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "TestPort",
                                 (ALPC_PORT_ATTRIBUTES *)0, &h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "AlpcCreatePort(\"TestPort\") returns SUCCESS");
    TEST_ASSERT_NEQ(h, INVALID_HANDLE_VALUE, "named port handle valid");

    /* Verify the port is findable in \RPC Control */
    void *body = (void *)0;
    int rc = ObLookupObjectByName("\\RPC Control\\TestPort",
                                   ObpAlpcPortType, 0, &body);
    TEST_ASSERT_EQ(rc, 0, "ObLookupObjectByName(\\RPC Control\\TestPort) == 0");
    TEST_ASSERT_NOT_NULL(body, "TestPort body non-NULL in namespace");

    if (body) {
        ALPC_PORT *p = (ALPC_PORT *)body;
        TEST_ASSERT_EQ((uint32_t)p->PortType, (uint32_t)AlpcServerConnectionPort,
                       "new port has server-connection type");
        ObDereferenceObject(body);  /* drop lookup ref */
    }
    if (h != INVALID_HANDLE_VALUE)
        NtClose(&task_current()->handle_table, h);
    /* -9 LEAK retrofit. */
    test_alpc_cleanup_named("TestPort");
}

static void test_alpc_duplicate_name_rejected(void)
{
    HANDLE h1 = INVALID_HANDLE_VALUE;
    NTSTATUS st1 = AlpcCreatePort(&task_current()->handle_table,
                                  "DupPort", (ALPC_PORT_ATTRIBUTES *)0, &h1);
    TEST_ASSERT_EQ((uint32_t)st1, (uint32_t)STATUS_SUCCESS,
                   "first DupPort create SUCCESS");

    HANDLE h2 = INVALID_HANDLE_VALUE;
    NTSTATUS st2 = AlpcCreatePort(&task_current()->handle_table,
                                  "DupPort", (ALPC_PORT_ATTRIBUTES *)0, &h2);
    TEST_ASSERT_EQ((uint32_t)st2, (uint32_t)STATUS_OBJECT_NAME_COLLISION,
                   "duplicate name => OBJECT_NAME_COLLISION");
    TEST_ASSERT_EQ(h2, INVALID_HANDLE_VALUE,
                   "duplicate-name handle left INVALID");
    if (h1 != INVALID_HANDLE_VALUE)
        NtClose(&task_current()->handle_table, h1);
    /* -9 LEAK retrofit. */
    test_alpc_cleanup_named("DupPort");
}

static void test_alpc_null_handle_table_rejected(void)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort((HANDLE_TABLE *)0, "ShouldFail",
                                 (ALPC_PORT_ATTRIBUTES *)0, &h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_INVALID_PARAMETER,
                   "NULL ht => STATUS_INVALID_PARAMETER");
}

static void test_alpc_embedded_backslash_rejected(void)
{
    /* Embedded path separator in the leaf must be refused -- otherwise a
     * kernel caller could smuggle a sub-path past ObInsertObject, which
     * treats the argument as a single leaf component. */
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "foo\\bar", (ALPC_PORT_ATTRIBUTES *)0, &h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_OBJECT_NAME_INVALID,
                   "embedded backslash => OBJECT_NAME_INVALID");
    NTSTATUS st2 = AlpcCreatePort(&task_current()->handle_table,
                                  "foo/bar", (ALPC_PORT_ATTRIBUTES *)0, &h);
    TEST_ASSERT_EQ((uint32_t)st2, (uint32_t)STATUS_OBJECT_NAME_INVALID,
                   "embedded forward slash => OBJECT_NAME_INVALID");
    NTSTATUS st3 = AlpcCreatePort(&task_current()->handle_table,
                                  "", (ALPC_PORT_ATTRIBUTES *)0, &h);
    TEST_ASSERT_EQ((uint32_t)st3, (uint32_t)STATUS_OBJECT_NAME_INVALID,
                   "empty leaf => OBJECT_NAME_INVALID");
}

static void test_alpc_privileged_flag_rejected(void)
{
    /* ALPC_PORTFLG_SYSTEM_PROCESS is privileged; gates it via
     * SeAccessCheck. Until then, reject from everywhere so it cannot
     * be set via user-mode. */
    ALPC_PORT_ATTRIBUTES attrs;
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st;

    /* Zero-init then set the privileged flag */
    uint8_t *p = (uint8_t *)&attrs;
    for (uint32_t i = 0; i < sizeof(attrs); i++) p[i] = 0;
    attrs.Flags = ALPC_PORTFLG_SYSTEM_PROCESS;

    st = AlpcCreatePort(&task_current()->handle_table,
                        (const char *)0, &attrs, &h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_INVALID_PARAMETER,
                   "SYSTEM_PROCESS flag => INVALID_PARAMETER");
}

static void test_alpc_reserved_pad_rejected(void)
{
    /* Non-zero reserved pad fields => reject (catches uninitialised
     * user-mode stack bytes from being persisted in the port body). */
    ALPC_PORT_ATTRIBUTES attrs;
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st;

    uint8_t *p = (uint8_t *)&attrs;
    for (uint32_t i = 0; i < sizeof(attrs); i++) p[i] = 0;
    attrs.Pad1 = 0xDEADBEEF;

    st = AlpcCreatePort(&task_current()->handle_table,
                        (const char *)0, &attrs, &h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_INVALID_PARAMETER,
                   "non-zero Pad1 => INVALID_PARAMETER");
}

static void test_alpc_syscall_full_path(void)
{
    /* Exercise the SSDT-dispatched NtAlpcCreatePort handler with a full
     * \RPC Control\TestPortSyscall path in OBJECT_ATTRIBUTES. Verifies
     * (a) the path-split logic peels "\\RPC Control\\" and passes only
     * "TestPortSyscall" to AlpcCreatePort, and (b) the resulting port
     * is findable at the original full path. */
    HANDLE out = 0;
    UNICODE_STRING name_us;
    OBJECT_ATTRIBUTES oa;
    char buf[] = "\\RPC Control\\TestPortSyscall";

    name_us.Length        = (uint16_t)(sizeof(buf) - 1);
    name_us.MaximumLength = (uint16_t)sizeof(buf);
    name_us._pad          = 0;
    name_us.Buffer        = (uint16_t *)buf;   /* ASCII-in-char-buffer, see handler */

    InitializeObjectAttributes(&oa, &name_us, 0, (HANDLE)0, (void *)0);

    NTSTATUS st = ssdt_dispatch(SSDT_NtAlpcCreatePort,
                                (uint64_t)&out, (uint64_t)&oa, 0, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "NtAlpcCreatePort(\\RPC Control\\TestPortSyscall) == SUCCESS");
    TEST_ASSERT_NEQ(out, INVALID_HANDLE_VALUE, "handle not INVALID");

    void *body = (void *)0;
    int rc = ObLookupObjectByName("\\RPC Control\\TestPortSyscall",
                                   ObpAlpcPortType, 0, &body);
    TEST_ASSERT_EQ(rc, 0, "full-path lookup after syscall == 0");
    TEST_ASSERT_NOT_NULL(body, "syscall-created port body non-NULL");
    if (body)
        ObDereferenceObject(body);
    if (out != INVALID_HANDLE_VALUE)
        NtClose(&task_current()->handle_table, out);
    /* -9 LEAK retrofit. */
    test_alpc_cleanup_named("TestPortSyscall");
}

/* =========================================================================
 * Connection state machine tests
 * =======================================================================*/

/* Simple pattern: static state set by the main thread, consumed by the
 * kthread worker. One test at a time uses these -- the test framework
 * runs suites sequentially on a single thread pool. */
static volatile HANDLE s_conn_client_handle;
static volatile NTSTATUS s_conn_client_status;
static volatile int s_conn_worker_done;
static const char *s_conn_target_name;
/* Section 7: optional RequiredServerSid the connect worker passes (NULL = no
 * check). Set before spawning the worker; the test runner is serial so a
 * single shared pointer is race-free across the one active handshake. */
static const SID *s_conn_required_sid;
/* Optional client-side QoS for the handshake worker (NULL = the client imposes
 * no limit, so the listener's QoS governs). Same serial-runner rationale. */
static const SECURITY_QUALITY_OF_SERVICE *s_conn_client_qos;

static void alpc_connect_worker(void *arg)
{
    (void)arg;
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcConnectPort(&task_current()->handle_table,
                                  s_conn_target_name,
                                  /* timeout_ms */ 5000,
                                  s_conn_required_sid, s_conn_client_qos, &h);
    s_conn_client_handle = h;
    s_conn_client_status = st;
    s_conn_worker_done   = 1;
}

static void test_alpc_connect_nonexistent(void)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcConnectPort(&task_current()->handle_table,
                                  "\\RPC Control\\NoSuchPort", 100,
                                  (const SID *)0,
                                  (const SECURITY_QUALITY_OF_SERVICE *)0, &h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_OBJECT_NAME_NOT_FOUND,
                   "connect to nonexistent => OBJECT_NAME_NOT_FOUND");
    TEST_ASSERT_EQ(h, INVALID_HANDLE_VALUE, "handle left INVALID");
}

static void test_alpc_connect_type_mismatch(void)
{
    /* A directory is not an ALPC port. Passing its namespace path to
     * AlpcConnectPort must reject with STATUS_OBJECT_NAME_NOT_FOUND
     * (ObLookupObjectByName filters by type, which returns -1 for any
     * mismatched type). */
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcConnectPort(&task_current()->handle_table,
                                  "\\RPC Control", 100, (const SID *)0,
                                  (const SECURITY_QUALITY_OF_SERVICE *)0, &h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_OBJECT_NAME_NOT_FOUND,
                   "connect to directory => OBJECT_NAME_NOT_FOUND");
}

static void test_alpc_accept_handshake(void)
{
    /* Server thread (this thread) creates a port, then spawns a worker
     * that calls AlpcConnectPort. Server accepts; both sides end with
     * valid handles and cross-linked ConnectedPort pointers. */
    HANDLE server_conn = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "HsServer", (ALPC_PORT_ATTRIBUTES *)0,
                                 &server_conn);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "server CreatePort SUCCESS");
    if (server_conn == INVALID_HANDLE_VALUE)
        return;

    s_conn_target_name   = "\\RPC Control\\HsServer";
    s_conn_required_sid  = (const SID *)0;
    s_conn_client_qos    = (const SECURITY_QUALITY_OF_SERVICE *)0;
    s_conn_client_handle = INVALID_HANDLE_VALUE;
    s_conn_client_status = STATUS_INVALID_PARAMETER;
    s_conn_worker_done   = 0;

    int tid = kthread_create(alpc_connect_worker, (void *)0, 0);
    TEST_ASSERT(tid >= 0, "kthread_create(connect worker) succeeds");
    if (tid < 0) {
        NtClose(&task_current()->handle_table, server_conn);
        test_alpc_cleanup_named("HsServer");
        return;
    }

    /* Accept the request the worker will queue. */
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    st = AlpcAcceptConnectPort(&task_current()->handle_table,
                               server_conn, /* accept */ 1,
                               /* timeout_ms */ 5000, &server_comm);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "accept(TRUE) SUCCESS");
    TEST_ASSERT_NEQ(server_comm, INVALID_HANDLE_VALUE,
                    "server-comm handle valid");

    thread_join((uint32_t)tid);

    TEST_ASSERT_EQ((uint32_t)s_conn_client_status, (uint32_t)STATUS_SUCCESS,
                   "client saw SUCCESS");
    TEST_ASSERT_NEQ(s_conn_client_handle, INVALID_HANDLE_VALUE,
                    "client-comm handle valid");

    /* Cross-link check: walk both handles to their port bodies and
     * assert ConnectedPort is non-NULL on each side. */
    HANDLE_TABLE_ENTRY *se = ObpLookupHandle(&task_current()->handle_table,
                                              server_comm);
    HANDLE_TABLE_ENTRY *ce = ObpLookupHandle(&task_current()->handle_table,
                                              s_conn_client_handle);
    TEST_ASSERT_NOT_NULL(se, "server-comm entry");
    TEST_ASSERT_NOT_NULL(ce, "client-comm entry");
    if (se && ce) {
        ALPC_PORT *sp = (ALPC_PORT *)se->object;
        ALPC_PORT *cp = (ALPC_PORT *)ce->object;
        TEST_ASSERT_NOT_NULL((void *)sp->ConnectedPort,
                             "server-comm->ConnectedPort non-NULL");
        TEST_ASSERT_NOT_NULL((void *)cp->ConnectedPort,
                             "client-comm->ConnectedPort non-NULL");
        TEST_ASSERT_EQ((uint64_t)(uintptr_t)sp->ConnectedPort,
                       (uint64_t)(uintptr_t)cp,
                       "cross-link: server_comm->ConnectedPort == client_comm");
        TEST_ASSERT_EQ((uint64_t)(uintptr_t)cp->ConnectedPort,
                       (uint64_t)(uintptr_t)sp,
                       "cross-link: client_comm->ConnectedPort == server_comm");
    }

    /* -9 LEAK retrofit: break the ConnectedPort cross-link refs before
     * handle close so the comm ports can cascade-free via on_delete. */
    if (server_comm != INVALID_HANDLE_VALUE)
        (void)AlpcDisconnectPort(&task_current()->handle_table, server_comm);

    if (server_comm != INVALID_HANDLE_VALUE)
        NtClose(&task_current()->handle_table, server_comm);
    if (s_conn_client_handle != INVALID_HANDLE_VALUE)
        NtClose(&task_current()->handle_table, s_conn_client_handle);
    NtClose(&task_current()->handle_table, server_conn);
    test_alpc_cleanup_named("HsServer");
}

static void test_alpc_reject_handshake(void)
{
    HANDLE server_conn = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "RjServer", (ALPC_PORT_ATTRIBUTES *)0,
                                 &server_conn);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "server CreatePort SUCCESS");
    if (server_conn == INVALID_HANDLE_VALUE)
        return;

    s_conn_target_name   = "\\RPC Control\\RjServer";
    s_conn_client_handle = INVALID_HANDLE_VALUE;
    s_conn_client_status = STATUS_SUCCESS;
    s_conn_worker_done   = 0;

    int tid = kthread_create(alpc_connect_worker, (void *)0, 0);
    TEST_ASSERT(tid >= 0, "reject: kthread_create succeeds");
    if (tid < 0) {
        NtClose(&task_current()->handle_table, server_conn);
        test_alpc_cleanup_named("RjServer");
        return;
    }

    /* Reject the connection. */
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    st = AlpcAcceptConnectPort(&task_current()->handle_table,
                               server_conn, /* accept */ 0,
                               /* timeout_ms */ 5000, &server_comm);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "server's reject call still returns SUCCESS");
    TEST_ASSERT_EQ(server_comm, INVALID_HANDLE_VALUE,
                   "reject path leaves out-handle untouched");

    thread_join((uint32_t)tid);

    TEST_ASSERT_EQ((uint32_t)s_conn_client_status,
                   (uint32_t)STATUS_PORT_CONNECTION_REFUSED,
                   "client saw CONNECTION_REFUSED");
    TEST_ASSERT_EQ(s_conn_client_handle, INVALID_HANDLE_VALUE,
                   "client handle rolled back on reject");

    NtClose(&task_current()->handle_table, server_conn);
    /* -9 LEAK retrofit. */
    test_alpc_cleanup_named("RjServer");
}

static void test_alpc_disconnect_queues_close_msg(void)
{
    /* Full handshake, then server disconnects. Peer's MessageQueue must
     * pick up a PORT_MESSAGE_ENTRY with Type = ALPC_MSG_TYPE_PORT_CLOSED. */
    HANDLE server_conn = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "DcServer", (ALPC_PORT_ATTRIBUTES *)0,
                                 &server_conn);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "disc server CreatePort SUCCESS");
    if (server_conn == INVALID_HANDLE_VALUE)
        return;

    s_conn_target_name   = "\\RPC Control\\DcServer";
    s_conn_client_handle = INVALID_HANDLE_VALUE;
    s_conn_client_status = STATUS_INVALID_PARAMETER;
    s_conn_worker_done   = 0;

    int tid = kthread_create(alpc_connect_worker, (void *)0, 0);
    if (tid < 0) {
        TEST_ASSERT(0, "disc kthread_create");
        NtClose(&task_current()->handle_table, server_conn);
        test_alpc_cleanup_named("DcServer");
        return;
    }

    HANDLE server_comm = INVALID_HANDLE_VALUE;
    st = AlpcAcceptConnectPort(&task_current()->handle_table,
                               server_conn, 1, 5000, &server_comm);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "disc accept");
    thread_join((uint32_t)tid);

    /* Server disconnects. The client-comm's MessageQueue should gain
     * one ALPC_MSG_TYPE_PORT_CLOSED entry. */
    st = AlpcDisconnectPort(&task_current()->handle_table, server_comm);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "disconnect SUCCESS");

    HANDLE_TABLE_ENTRY *ce = ObpLookupHandle(&task_current()->handle_table,
                                              s_conn_client_handle);
    TEST_ASSERT_NOT_NULL(ce, "client-comm entry after disconnect");
    if (ce) {
        ALPC_PORT *cp = (ALPC_PORT *)ce->object;
        TEST_ASSERT_NEQ(cp->MessageQueue.Count, 0u,
                        "client MessageQueue has the PORT_CLOSED marker");
        if (cp->MessageQueue.Head) {
            TEST_ASSERT_EQ((uint32_t)cp->MessageQueue.Head->Header.Type,
                           (uint32_t)ALPC_MSG_TYPE_PORT_CLOSED,
                           "marker type == ALPC_MSG_TYPE_PORT_CLOSED");
        }
        TEST_ASSERT_EQ((uint32_t)cp->Disconnected, 1u,
                       "client side marked Disconnected");
    }

    if (server_comm != INVALID_HANDLE_VALUE)
        NtClose(&task_current()->handle_table, server_comm);
    if (s_conn_client_handle != INVALID_HANDLE_VALUE)
        NtClose(&task_current()->handle_table, s_conn_client_handle);
    NtClose(&task_current()->handle_table, server_conn);
    /* -9 LEAK retrofit. */
    test_alpc_cleanup_named("DcServer");
}

static void test_alpc_connect_timeout(void)
{
    /* Create a server but do not accept. Client connects with a
     * finite timeout; the call MUST return STATUS_TIMEOUT and
     * leave the out-handle at INVALID_HANDLE_VALUE. STATUS_TIMEOUT
     * passes NT_SUCCESS (it is 0x00000102, non-error severity), so
     * this test guards against the "NT_SUCCESS is too loose" bug. */
    HANDLE server_conn = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "TimeoutSrv", (ALPC_PORT_ATTRIBUTES *)0,
                                 &server_conn);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "timeout srv CreatePort SUCCESS");
    if (server_conn == INVALID_HANDLE_VALUE)
        return;

    HANDLE client_h = INVALID_HANDLE_VALUE;
    st = AlpcConnectPort(&task_current()->handle_table,
                         "\\RPC Control\\TimeoutSrv",
                         /* timeout_ms */ 50, (const SID *)0,
                         (const SECURITY_QUALITY_OF_SERVICE *)0, &client_h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_TIMEOUT,
                   "connect timeout => STATUS_TIMEOUT");
    TEST_ASSERT_EQ(client_h, INVALID_HANDLE_VALUE,
                   "timeout path must NOT publish a handle");

    NtClose(&task_current()->handle_table, server_conn);
    /* -9 LEAK retrofit. */
    test_alpc_cleanup_named("TimeoutSrv");
}

static void test_alpc_accept_timeout(void)
{
    /* Server creates, no client connects, server accepts with finite
     * timeout. Must return STATUS_TIMEOUT and leave out-handle
     * INVALID_HANDLE_VALUE. */
    HANDLE server_conn = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "AcceptTimeoutSrv",
                                 (ALPC_PORT_ATTRIBUTES *)0, &server_conn);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "accept timeout srv CreatePort SUCCESS");
    if (server_conn == INVALID_HANDLE_VALUE)
        return;

    HANDLE server_comm = INVALID_HANDLE_VALUE;
    st = AlpcAcceptConnectPort(&task_current()->handle_table,
                               server_conn, /* accept */ 1,
                               /* timeout_ms */ 50, &server_comm);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_TIMEOUT,
                   "accept with no pending => STATUS_TIMEOUT");
    TEST_ASSERT_EQ(server_comm, INVALID_HANDLE_VALUE,
                   "timeout path leaves out-handle INVALID");

    NtClose(&task_current()->handle_table, server_conn);
    /* -9 LEAK retrofit. */
    test_alpc_cleanup_named("AcceptTimeoutSrv");
}

static void test_alpc_disconnect_unconnected(void)
{
    /* Disconnecting a server connection port with no peer is a no-op
     * success (it just marks Disconnected). */
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "LonelyDc", (ALPC_PORT_ATTRIBUTES *)0, &h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "lonely create");
    if (h == INVALID_HANDLE_VALUE)
        return;
    st = AlpcDisconnectPort(&task_current()->handle_table, h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "disconnect with no peer SUCCESS");
    NtClose(&task_current()->handle_table, h);
    /* -9 LEAK retrofit. */
    test_alpc_cleanup_named("LonelyDc");
}

static void test_alpc_syscall_bad_prefix_rejected(void)
{
    /* A path that is NOT under \RPC Control must be rejected with
     * STATUS_OBJECT_NAME_INVALID -- we don't want user-mode smuggling
     * ports into arbitrary directories. */
    HANDLE out = 0;
    UNICODE_STRING name_us;
    OBJECT_ATTRIBUTES oa;
    char buf[] = "\\BaseNamedObjects\\RogueAlpc";

    name_us.Length        = (uint16_t)(sizeof(buf) - 1);
    name_us.MaximumLength = (uint16_t)sizeof(buf);
    name_us._pad          = 0;
    name_us.Buffer        = (uint16_t *)buf;
    InitializeObjectAttributes(&oa, &name_us, 0, (HANDLE)0, (void *)0);

    NTSTATUS st = ssdt_dispatch(SSDT_NtAlpcCreatePort,
                                (uint64_t)&out, (uint64_t)&oa, 0, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_OBJECT_NAME_INVALID,
                   "path outside \\RPC Control rejected");
}

/* =========================================================================
 * Synchronous Send+Wait+Receive Engine tests
 *
 * Each test sets up a connected pair via the handshake, then drives
 * AlpcSendWaitReceivePort on both endpoints. Worker threads run the
 * client side via kthread_create so the cooperative-yield scheduler
 * can interleave server and client.
 * =======================================================================*/

#include "kernel/sched/task.h"

/* Shared state for worker threads. The framework runs suites
 * sequentially on a single thread pool, so a single set of statics is
 * safe. */
static volatile HANDLE   s_sync_client_handle;
static volatile HANDLE   s_sync_server_conn;
static const char       *s_sync_target_name;
static volatile NTSTATUS s_sync_send_status;
static volatile int      s_sync_worker_done;
/* -9 LEAK retrofit: the leaf name used by alpc_setup_pair so
 * alpc_teardown_pair can unlink the named port from \RPC Control. */
static const char       *s_sync_server_leaf;

/* connect-worker (file-static; replaces inline GCC-nested-function
 * pattern that clang -- the project compiler -- does not support). */
static void s4_connect_worker(void *arg)
{
    (void)arg;
    HANDLE ch = INVALID_HANDLE_VALUE;
    (void)AlpcConnectPort(&task_current()->handle_table,
                          s_sync_target_name, /* timeout */ 5000,
                          (const SID *)0,
                          (const SECURITY_QUALITY_OF_SERVICE *)0, &ch);
    s_sync_client_handle = ch;
    s_sync_worker_done = 1;
}

static HANDLE alpc_setup_pair(const char *server_name, HANDLE *server_comm)
{
    /* Convenience: build a fully connected server/client pair using
     * primitives. Returns the client communication handle. The server
     * connection handle is exposed via s_sync_server_conn so the test
     * can NtClose it afterwards. */
    HANDLE server_conn = INVALID_HANDLE_VALUE;
    HANDLE client_h    = INVALID_HANDLE_VALUE;
    NTSTATUS st;
    char path[96];

    st = AlpcCreatePort(&task_current()->handle_table, server_name,
                        (ALPC_PORT_ATTRIBUTES *)0, &server_conn);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "pair: server CreatePort");
    if (server_conn == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;

    /* Build "\\RPC Control\\<name>". */
    {
        const char prefix[] = "\\RPC Control\\";
        uint32_t pi = 0, li;
        for (li = 0; prefix[li]; li++, pi++) path[pi] = prefix[li];
        for (li = 0; server_name[li] && pi < sizeof(path) - 1; li++, pi++)
            path[pi] = server_name[li];
        path[pi] = '\0';
    }

    s_sync_server_conn = server_conn;
    s_sync_target_name = path;
    s_sync_server_leaf = server_name;

    /* Worker calls AlpcConnectPort; we accept inline. */
    s_sync_client_handle = INVALID_HANDLE_VALUE;
    s_sync_worker_done = 0;
    int tid = kthread_create(s4_connect_worker, (void *)0, 0);
    TEST_ASSERT(tid >= 0, "pair: connect worker spawned");

    HANDLE sc = INVALID_HANDLE_VALUE;
    st = AlpcAcceptConnectPort(&task_current()->handle_table,
                               server_conn, /* accept */ 1,
                               /* timeout */ 5000, &sc);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "pair: accept");
    thread_join((uint32_t)tid);
    TEST_ASSERT_NEQ(s_sync_client_handle, INVALID_HANDLE_VALUE,
                    "pair: client got handle");

    *server_comm = sc;
    client_h = s_sync_client_handle;

    /* -9 LEAK retrofit: if setup never produced a client handle (worker
     * failed to connect, or accept timed out), callers bail without
     * invoking alpc_teardown_pair. Clean up server_conn + the named
     * entry here so the failure path cannot leak. */
    if (client_h == INVALID_HANDLE_VALUE) {
        if (sc != INVALID_HANDLE_VALUE)
            NtClose(&task_current()->handle_table, sc);
        NtClose(&task_current()->handle_table, server_conn);
        s_sync_server_conn = INVALID_HANDLE_VALUE;
        test_alpc_cleanup_named(server_name);
        s_sync_server_leaf = (const char *)0;
    }
    return client_h;
}

static void alpc_teardown_pair(HANDLE client_h, HANDLE server_comm)
{
    HANDLE_TABLE *ht = &task_current()->handle_table;

    /* -9 LEAK retrofit: explicitly disconnect both sides before closing
     * handles. ConnectedPort cross-links each hold a ref on the peer
     * (set at alpc_port.c:786-792); handle close alone drops only the
     * handle ref, leaving the comm ports pinning each other at ref=1
     * forever. AlpcDisconnectPort clears the caller's ConnectedPort +
     * drops that ref, so once both sides disconnect neither comm port
     * has an inbound cross-link and the final handle close cascades
     * into on_delete + body free. */
    if (client_h != INVALID_HANDLE_VALUE)
        (void)AlpcDisconnectPort(ht, client_h);
    if (server_comm != INVALID_HANDLE_VALUE)
        (void)AlpcDisconnectPort(ht, server_comm);

    if (client_h != INVALID_HANDLE_VALUE) NtClose(ht, client_h);
    if (server_comm != INVALID_HANDLE_VALUE) NtClose(ht, server_comm);
    if (s_sync_server_conn != INVALID_HANDLE_VALUE)
        NtClose(ht, s_sync_server_conn);
    s_sync_server_conn = INVALID_HANDLE_VALUE;
    /* Unlink the named connection port from \RPC Control. */
    if (s_sync_server_leaf) {
        test_alpc_cleanup_named(s_sync_server_leaf);
        s_sync_server_leaf = (const char *)0;
    }
}

/* ---- Datagram send delivers to peer's MessageQueue --------------- */

static void test_alpc_datagram_delivery(void)
{
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h = alpc_setup_pair("DgPair", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    /* Client sends a 16-byte datagram. */
    uint8_t buf[sizeof(PORT_MESSAGE) + 16];
    PORT_MESSAGE *msg = (PORT_MESSAGE *)buf;
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    msg->TotalLength = sizeof(PORT_MESSAGE) + 16;
    msg->DataLength  = 16;
    msg->Type        = ALPC_MSG_TYPE_DATAGRAM;
    for (uint32_t i = 0; i < 16; i++)
        ((uint8_t *)msg + sizeof(PORT_MESSAGE))[i] = (uint8_t)(0xA0 + i);

    NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                          client_h, /* flags */ 0,
                                          msg, (PORT_MESSAGE *)0, 0,
                                          /* timeout */ 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "datagram send returns SUCCESS");

    /* Server receives. */
    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 64];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    for (uint32_t i = 0; i < sizeof(rxbuf); i++) rxbuf[i] = 0;
    st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                 server_comm, /* flags */ 0,
                                 (PORT_MESSAGE *)0, rx, sizeof(rxbuf),
                                 /* timeout */ 1000);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "server receive datagram SUCCESS");
    TEST_ASSERT_EQ((uint32_t)rx->Type, (uint32_t)ALPC_MSG_TYPE_DATAGRAM,
                   "received Type=DATAGRAM");
    TEST_ASSERT_EQ((uint32_t)rx->DataLength, 16u, "DataLength=16");
    TEST_ASSERT_EQ(((uint8_t *)rx + sizeof(PORT_MESSAGE))[0], 0xA0u,
                   "body byte 0 round-trips");
    TEST_ASSERT_EQ(((uint8_t *)rx + sizeof(PORT_MESSAGE))[15], 0xAFu,
                   "body byte 15 round-trips");

    alpc_teardown_pair(client_h, server_comm);
}

/* ---- Sync request + reply round-trip ----------------------------- */

static void sync_server_worker(void *arg)
{
    HANDLE server_comm = (HANDLE)(uintptr_t)arg;
    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 128];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    for (uint32_t i = 0; i < sizeof(rxbuf); i++) rxbuf[i] = 0;

    /* Receive request */
    NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                          server_comm, 0,
                                          (PORT_MESSAGE *)0, rx,
                                          sizeof(rxbuf), 5000);
    if (st != STATUS_SUCCESS) {
        s_sync_send_status = st;
        s_sync_worker_done = 1;
        return;
    }

    /* Build a reply: echo body bytes XORed with 0xFF. */
    uint8_t txbuf[sizeof(PORT_MESSAGE) + 128];
    PORT_MESSAGE *tx = (PORT_MESSAGE *)txbuf;
    for (uint32_t i = 0; i < sizeof(txbuf); i++) txbuf[i] = 0;
    uint16_t n = rx->DataLength;
    if (n > 128) n = 128;
    tx->TotalLength = sizeof(PORT_MESSAGE) + n;
    tx->DataLength  = n;
    tx->Type        = ALPC_MSG_TYPE_REPLY;
    tx->MessageId   = rx->MessageId;
    for (uint16_t i = 0; i < n; i++)
        ((uint8_t *)tx + sizeof(PORT_MESSAGE))[i] =
            ((uint8_t *)rx + sizeof(PORT_MESSAGE))[i] ^ 0xFFu;

    s_sync_send_status = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                                 server_comm,
                                                 ALPC_MSGFLG_REPLY_MESSAGE,
                                                 tx, (PORT_MESSAGE *)0, 0, 0);
    s_sync_worker_done = 1;
}

static void test_alpc_sync_request_reply(void)
{
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h = alpc_setup_pair("SyncPair", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    /* Spawn server worker that will receive + reply. */
    s_sync_send_status = STATUS_INVALID_PARAMETER;
    s_sync_worker_done = 0;
    int tid = kthread_create(sync_server_worker,
                             (void *)(uintptr_t)server_comm, 0);
    TEST_ASSERT(tid >= 0, "sync: server worker spawned");

    /* Client sends sync request, blocks for reply. */
    uint8_t txbuf[sizeof(PORT_MESSAGE) + 64];
    PORT_MESSAGE *tx = (PORT_MESSAGE *)txbuf;
    for (uint32_t i = 0; i < sizeof(txbuf); i++) txbuf[i] = 0;
    tx->TotalLength = sizeof(PORT_MESSAGE) + 64;
    tx->DataLength  = 64;
    tx->Type        = ALPC_MSG_TYPE_REQUEST;
    for (uint32_t i = 0; i < 64; i++)
        ((uint8_t *)tx + sizeof(PORT_MESSAGE))[i] = (uint8_t)i;

    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 128];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    for (uint32_t i = 0; i < sizeof(rxbuf); i++) rxbuf[i] = 0;

    NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                          client_h,
                                          ALPC_MSGFLG_SYNC_REQUEST,
                                          tx, rx, sizeof(rxbuf), 5000);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "sync request SUCCESS");
    TEST_ASSERT_EQ((uint32_t)rx->Type, (uint32_t)ALPC_MSG_TYPE_REPLY,
                   "reply Type=REPLY");
    TEST_ASSERT_EQ((uint32_t)rx->DataLength, 64u, "reply DataLength=64");
    TEST_ASSERT_EQ(((uint8_t *)rx + sizeof(PORT_MESSAGE))[0], 0xFFu,
                   "reply byte 0 = 0x00 ^ 0xFF");
    TEST_ASSERT_EQ(((uint8_t *)rx + sizeof(PORT_MESSAGE))[63], 0xC0u,
                   "reply byte 63 = 0x3F ^ 0xFF");

    thread_join((uint32_t)tid);
    TEST_ASSERT_EQ((uint32_t)s_sync_send_status, (uint32_t)STATUS_SUCCESS,
                   "server's reply call returned SUCCESS");

    alpc_teardown_pair(client_h, server_comm);
}

/* ---- Reply with bogus MessageId ---------------------------------- */

static void test_alpc_reply_mismatch(void)
{
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h = alpc_setup_pair("MmPair", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    /* Server sends a reply with a MessageId that has no pending entry
     * on the client's PendingQueue (the client never sent a sync
     * request). Must return STATUS_REPLY_MESSAGE_MISMATCH. */
    uint8_t txbuf[sizeof(PORT_MESSAGE)];
    PORT_MESSAGE *tx = (PORT_MESSAGE *)txbuf;
    for (uint32_t i = 0; i < sizeof(txbuf); i++) txbuf[i] = 0;
    tx->TotalLength = sizeof(PORT_MESSAGE);
    tx->DataLength  = 0;
    tx->Type        = ALPC_MSG_TYPE_REPLY;
    tx->MessageId   = 0xDEADBEEFu;

    NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                          server_comm,
                                          ALPC_MSGFLG_REPLY_MESSAGE,
                                          tx, (PORT_MESSAGE *)0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_REPLY_MESSAGE_MISMATCH,
                   "reply with unknown MessageId => REPLY_MESSAGE_MISMATCH");

    alpc_teardown_pair(client_h, server_comm);
}

/* ---- Datagram exceeding MaxMessageLength ------------------------- */

static void test_alpc_send_too_large(void)
{
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h = alpc_setup_pair("TooBig", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    /* Construct a header claiming an oversized DataLength; the engine
     * must reject without allocating. We don't actually need that many
     * bytes on the stack -- the engine validates DataLength up front. */
    PORT_MESSAGE hdr;
    uint8_t *zp = (uint8_t *)&hdr;
    for (uint32_t i = 0; i < sizeof(hdr); i++) zp[i] = 0;
    hdr.TotalLength = 0;     /* invalid intentionally */
    hdr.DataLength  = ALPC_MAX_ALLOWED_MESSAGE_LENGTH + 1;
    hdr.Type        = ALPC_MSG_TYPE_DATAGRAM;

    NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                          client_h, 0,
                                          &hdr, (PORT_MESSAGE *)0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_BUFFER_TOO_SMALL,
                   "DataLength > max => STATUS_BUFFER_TOO_SMALL");

    alpc_teardown_pair(client_h, server_comm);
}

/* ---- Receive on empty queue with timeout=0 ----------------------- */

static void test_alpc_receive_empty_no_wait(void)
{
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h = alpc_setup_pair("EmptyRx", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    uint8_t rxbuf[sizeof(PORT_MESSAGE)];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                          server_comm, 0,
                                          (PORT_MESSAGE *)0, rx,
                                          sizeof(rxbuf), 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_TIMEOUT,
                   "empty queue + timeout=0 => STATUS_TIMEOUT");

    alpc_teardown_pair(client_h, server_comm);
}

/* ---- Disconnect wakes blocked sync waiter ------------------------ */

static void disconnect_after_delay_worker(void *arg)
{
    /* Yield a few times to let the client get into its sync wait, then
     * disconnect. Bounded retries -- if the client doesn't get there,
     * we still disconnect and the test fails meaningfully. */
    HANDLE port = (HANDLE)(uintptr_t)arg;
    for (int i = 0; i < 200; i++)
        thread_yield();
    (void)AlpcDisconnectPort(&task_current()->handle_table, port);
    s_sync_worker_done = 1;
}

static void test_alpc_sync_wait_disconnect(void)
{
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h = alpc_setup_pair("DcWake", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    /* Spawn a worker that disconnects the SERVER side after a short
     * delay. The client (running inline here) blocks in a sync wait
     * with a long timeout; the disconnect path must wake it with
     * STATUS_PORT_DISCONNECTED before the timeout elapses. */
    s_sync_worker_done = 0;
    int tid = kthread_create(disconnect_after_delay_worker,
                             (void *)(uintptr_t)server_comm, 0);
    TEST_ASSERT(tid >= 0, "dc-wake: worker spawned");

    uint8_t txbuf[sizeof(PORT_MESSAGE) + 8];
    PORT_MESSAGE *tx = (PORT_MESSAGE *)txbuf;
    for (uint32_t i = 0; i < sizeof(txbuf); i++) txbuf[i] = 0;
    tx->TotalLength = sizeof(PORT_MESSAGE) + 8;
    tx->DataLength  = 8;
    tx->Type        = ALPC_MSG_TYPE_REQUEST;

    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 64];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;

    NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                          client_h,
                                          ALPC_MSGFLG_SYNC_REQUEST,
                                          tx, rx, sizeof(rxbuf),
                                          /* timeout */ 5000);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_PORT_DISCONNECTED,
                   "sync wait wakes with PORT_DISCONNECTED");

    thread_join((uint32_t)tid);
    alpc_teardown_pair(client_h, server_comm);
}

/* ---- Pool quota exceeded ----------------------------------------- */

static void test_alpc_pool_quota_exceeded(void)
{
    /* Build a server with a tight MaxPoolUsage so the quota path is
     * exercised. Sending one too-big datagram should return
     * STATUS_INSUFFICIENT_RESOURCES from the allocator. */
    HANDLE server_conn = INVALID_HANDLE_VALUE;
    ALPC_PORT_ATTRIBUTES attrs;
    uint8_t *zp = (uint8_t *)&attrs;
    for (uint32_t i = 0; i < sizeof(attrs); i++) zp[i] = 0;
    attrs.MaxMessageLength = 256;
    attrs.MaxPoolUsage     = 128;     /* much less than one entry needs */

    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "TightPool", &attrs, &server_conn);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "tight-pool: server CreatePort");
    if (server_conn == INVALID_HANDLE_VALUE)
        return;

    s_sync_target_name = "\\RPC Control\\TightPool";
    s_sync_client_handle = INVALID_HANDLE_VALUE;
    s_sync_worker_done = 0;
    int tid = kthread_create(s4_connect_worker, (void *)0, 0);
    TEST_ASSERT(tid >= 0, "tight-pool: worker spawned");

    HANDLE server_comm = INVALID_HANDLE_VALUE;
    (void)AlpcAcceptConnectPort(&task_current()->handle_table,
                                server_conn, 1, 5000, &server_comm);
    thread_join((uint32_t)tid);

    HANDLE client_h = s_sync_client_handle;
    if (client_h == INVALID_HANDLE_VALUE) {
        if (server_comm != INVALID_HANDLE_VALUE)
            NtClose(&task_current()->handle_table, server_comm);
        NtClose(&task_current()->handle_table, server_conn);
        test_alpc_cleanup_named("TightPool");
        return;
    }

    /* Send 200 bytes -- header(40) + 200 = 240 > MaxPoolUsage(128). */
    uint8_t buf[sizeof(PORT_MESSAGE) + 200];
    PORT_MESSAGE *msg = (PORT_MESSAGE *)buf;
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    msg->TotalLength = sizeof(PORT_MESSAGE) + 200;
    msg->DataLength  = 200;
    msg->Type        = ALPC_MSG_TYPE_DATAGRAM;

    st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                 client_h, 0,
                                 msg, (PORT_MESSAGE *)0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_INSUFFICIENT_RESOURCES,
                   "send exceeding MaxPoolUsage => INSUFFICIENT_RESOURCES");

    /* -9 LEAK retrofit: disconnect to break the cross-link ref cycle. */
    (void)AlpcDisconnectPort(&task_current()->handle_table, server_comm);
    NtClose(&task_current()->handle_table, client_h);
    NtClose(&task_current()->handle_table, server_comm);
    NtClose(&task_current()->handle_table, server_conn);
    test_alpc_cleanup_named("TightPool");
}

/* ---- Receive-only path: FIFO + truncation ------------------------ */

static void test_alpc_receive_fifo_truncation(void)
{
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h = alpc_setup_pair("FifoTrunc", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    /* Send two datagrams: 32B (pattern A) then 8B (pattern B). */
    {
        uint8_t buf[sizeof(PORT_MESSAGE) + 32];
        PORT_MESSAGE *msg = (PORT_MESSAGE *)buf;
        for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
        msg->TotalLength = sizeof(PORT_MESSAGE) + 32;
        msg->DataLength  = 32;
        msg->Type        = ALPC_MSG_TYPE_DATAGRAM;
        for (uint32_t i = 0; i < 32; i++)
            ((uint8_t *)msg + sizeof(PORT_MESSAGE))[i] = (uint8_t)(0xA0 + i);
        NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                              client_h, 0, msg,
                                              (PORT_MESSAGE *)0, 0, 0);
        TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                       "FIFO: first datagram sent");
    }
    {
        uint8_t buf[sizeof(PORT_MESSAGE) + 8];
        PORT_MESSAGE *msg = (PORT_MESSAGE *)buf;
        for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
        msg->TotalLength = sizeof(PORT_MESSAGE) + 8;
        msg->DataLength  = 8;
        msg->Type        = ALPC_MSG_TYPE_DATAGRAM;
        for (uint32_t i = 0; i < 8; i++)
            ((uint8_t *)msg + sizeof(PORT_MESSAGE))[i] = (uint8_t)(0xB0 + i);
        NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                              client_h, 0, msg,
                                              (PORT_MESSAGE *)0, 0, 0);
        TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                       "FIFO: second datagram sent");
    }

    /* First receive with truncated buffer (room for only 4 body bytes).
     * Sentinel byte at offset (header+4) verifies no overrun. */
    {
        uint8_t buf[sizeof(PORT_MESSAGE) + 16];
        for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0xCC;
        PORT_MESSAGE *rx = (PORT_MESSAGE *)buf;
        NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                              server_comm, 0,
                                              (PORT_MESSAGE *)0, rx,
                                              sizeof(PORT_MESSAGE) + 4, 0);
        TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                       "FIFO: truncated receive SUCCESS");
        TEST_ASSERT_EQ((uint32_t)rx->Type, (uint32_t)ALPC_MSG_TYPE_DATAGRAM,
                       "FIFO: header preserved on truncation");
        TEST_ASSERT_EQ((uint32_t)rx->DataLength, 32u,
                       "FIFO: original DataLength preserved (truncation is on body only)");
        TEST_ASSERT_EQ(((uint8_t *)rx + sizeof(PORT_MESSAGE))[0], 0xA0u,
                       "FIFO: truncated body[0] = pattern A");
        TEST_ASSERT_EQ(((uint8_t *)rx + sizeof(PORT_MESSAGE))[3], 0xA3u,
                       "FIFO: truncated body[3] = pattern A");
        TEST_ASSERT_EQ(((uint8_t *)rx + sizeof(PORT_MESSAGE))[4], 0xCCu,
                       "FIFO: sentinel past truncation untouched");
    }

    /* Second receive returns the SECOND datagram (FIFO), not the
     * remainder of the first. */
    {
        uint8_t buf[sizeof(PORT_MESSAGE) + 16];
        for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
        PORT_MESSAGE *rx = (PORT_MESSAGE *)buf;
        NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                              server_comm, 0,
                                              (PORT_MESSAGE *)0, rx,
                                              sizeof(buf), 0);
        TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                       "FIFO: second receive SUCCESS");
        TEST_ASSERT_EQ((uint32_t)rx->DataLength, 8u, "FIFO: second msg DataLength=8");
        TEST_ASSERT_EQ(((uint8_t *)rx + sizeof(PORT_MESSAGE))[0], 0xB0u,
                       "FIFO: second msg body[0] = pattern B");
        TEST_ASSERT_EQ(((uint8_t *)rx + sizeof(PORT_MESSAGE))[7], 0xB7u,
                       "FIFO: second msg body[7] = pattern B");
    }

    alpc_teardown_pair(client_h, server_comm);
}

/* ---- Sync timeout + late reply mismatch -------------------------- */

static void test_alpc_sync_timeout_late_reply(void)
{
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h = alpc_setup_pair("LateReply", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    /* Client sends sync request with a SHORT timeout and no server is
     * receiving yet. Sync wait must return STATUS_TIMEOUT. */
    uint8_t txbuf[sizeof(PORT_MESSAGE) + 8];
    PORT_MESSAGE *tx = (PORT_MESSAGE *)txbuf;
    for (uint32_t i = 0; i < sizeof(txbuf); i++) txbuf[i] = 0;
    tx->TotalLength = sizeof(PORT_MESSAGE) + 8;
    tx->DataLength  = 8;
    tx->Type        = ALPC_MSG_TYPE_REQUEST;

    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 32];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    for (uint32_t i = 0; i < sizeof(rxbuf); i++) rxbuf[i] = 0;

    NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                          client_h,
                                          ALPC_MSGFLG_SYNC_REQUEST,
                                          tx, rx, sizeof(rxbuf),
                                          /* timeout */ 50);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_TIMEOUT,
                   "sync wait timed out");

    /* The request entry is still on server's MessageQueue (we never
     * received it). Server now receives, observes the request, and tries
     * to reply -- but the client already cleaned up its pending record,
     * so the reply must miss. */
    uint8_t srvrx[sizeof(PORT_MESSAGE) + 32];
    PORT_MESSAGE *srx = (PORT_MESSAGE *)srvrx;
    for (uint32_t i = 0; i < sizeof(srvrx); i++) srvrx[i] = 0;
    st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                 server_comm, 0,
                                 (PORT_MESSAGE *)0, srx, sizeof(srvrx),
                                 /* timeout */ 100);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "server received the timed-out request");
    TEST_ASSERT_EQ((uint32_t)srx->Type, (uint32_t)ALPC_MSG_TYPE_REQUEST,
                   "received was a REQUEST");

    /* Late reply with the captured MessageId -- pending record gone. */
    uint8_t reply_buf[sizeof(PORT_MESSAGE) + 4];
    PORT_MESSAGE *reply = (PORT_MESSAGE *)reply_buf;
    for (uint32_t i = 0; i < sizeof(reply_buf); i++) reply_buf[i] = 0;
    reply->TotalLength = sizeof(PORT_MESSAGE) + 4;
    reply->DataLength  = 4;
    reply->Type        = ALPC_MSG_TYPE_REPLY;
    reply->MessageId   = srx->MessageId;
    st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                 server_comm,
                                 ALPC_MSGFLG_REPLY_MESSAGE,
                                 reply, (PORT_MESSAGE *)0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_REPLY_MESSAGE_MISMATCH,
                   "late reply => REPLY_MESSAGE_MISMATCH");

    /* Pair must still be usable: send a fresh datagram, server receives it. */
    uint8_t dg_buf[sizeof(PORT_MESSAGE) + 4];
    PORT_MESSAGE *dg = (PORT_MESSAGE *)dg_buf;
    for (uint32_t i = 0; i < sizeof(dg_buf); i++) dg_buf[i] = 0;
    dg->TotalLength = sizeof(PORT_MESSAGE) + 4;
    dg->DataLength  = 4;
    dg->Type        = ALPC_MSG_TYPE_DATAGRAM;
    st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                 client_h, 0, dg, (PORT_MESSAGE *)0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "post-timeout send still works");

    alpc_teardown_pair(client_h, server_comm);
}

/* ---- Disconnect-then-send returns DISCONNECTED ----------------- */

static void test_alpc_disconnect_then_send(void)
{
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h = alpc_setup_pair("DcThenSend", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    /* Disconnect the local (client) side. Subsequent sends must fail
     * fast with STATUS_PORT_DISCONNECTED. */
    NTSTATUS st = AlpcDisconnectPort(&task_current()->handle_table,
                                     client_h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "local disconnect SUCCESS");

    uint8_t buf[sizeof(PORT_MESSAGE) + 4];
    PORT_MESSAGE *msg = (PORT_MESSAGE *)buf;
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    msg->TotalLength = sizeof(PORT_MESSAGE) + 4;
    msg->DataLength  = 4;
    msg->Type        = ALPC_MSG_TYPE_DATAGRAM;

    st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                 client_h, 0, msg, (PORT_MESSAGE *)0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_PORT_DISCONNECTED,
                   "datagram on disconnected port => PORT_DISCONNECTED");

    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 32];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    msg->Type = ALPC_MSG_TYPE_REQUEST;
    st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                 client_h, ALPC_MSGFLG_SYNC_REQUEST,
                                 msg, rx, sizeof(rxbuf), 50);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_PORT_DISCONNECTED,
                   "sync request on disconnected port => PORT_DISCONNECTED");

    alpc_teardown_pair(client_h, server_comm);
}

/* ---- Central sender quota on queued messages (TODO-25 s6) ---------- */

/* PoolUsageBytes caps ONE port's bytes; QUOTA_RES_ALPC_MESSAGE caps how many
 * messages a single USER can leave queued across every port. These tests
 * observe the CENTRAL charge, which the pre-existing pool tests cannot see --
 * a stranded or double-returned sender charge is invisible to PoolUsageBytes.
 *
 * Usage is read from the current task's PROCESS block, which every chain
 * charge bills alongside the user and job blocks. */
static void test_alpc_message_quota_roundtrip(void)
{
    struct task *t = task_current();
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h;
    uint64_t before;
    /* The attribution tag on the REAL charge site. The taxonomy helpers are tested
     * in test_quota.c; what only this can catch is the wiring -- an untagged or
     * mis-tagged AlpcAllocateMessage would keep those tests green while the
     * dashboard blamed the wrong subsystem for every queued message. */
    int64_t ipc_before = quota_source_usage(QUOTA_SOURCE_IPC,
                                            QUOTA_RES_ALPC_MESSAGE);

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        return;
    }
    client_h = alpc_setup_pair("QuotaRT", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    before = quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE);

    uint8_t buf[sizeof(PORT_MESSAGE) + 16];
    PORT_MESSAGE *msg = (PORT_MESSAGE *)buf;
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    msg->TotalLength = sizeof(PORT_MESSAGE) + 16;
    msg->DataLength  = 16;
    msg->Type        = ALPC_MSG_TYPE_DATAGRAM;

    TEST_ASSERT_EQ((uint32_t)AlpcSendWaitReceivePort(&t->handle_table, client_h,
                                                     0, msg, (PORT_MESSAGE *)0,
                                                     0, 0),
                   (uint32_t)STATUS_SUCCESS, "datagram queued");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before + 1,
                   "a queued message charges the sender exactly one");
    TEST_ASSERT_EQ((uint64_t)(quota_source_usage(QUOTA_SOURCE_IPC,
                                                QUOTA_RES_ALPC_MESSAGE)
                              - ipc_before),
                   1ull, "and attributes it to IPC, not to the unattributed row");

    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 64];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    TEST_ASSERT_EQ((uint32_t)AlpcSendWaitReceivePort(&t->handle_table,
                                                     server_comm, 0,
                                                     (PORT_MESSAGE *)0, rx,
                                                     sizeof(rxbuf), 100),
                   (uint32_t)STATUS_SUCCESS, "message received");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before,
                   "receiving the message returns the sender's charge");
    TEST_ASSERT_EQ((uint64_t)(quota_source_usage(QUOTA_SOURCE_IPC,
                                                QUOTA_RES_ALPC_MESSAGE)
                              - ipc_before),
                   0ull, "and credits the IPC attribution back with it");

    alpc_teardown_pair(client_h, server_comm);
}

/* The teardown case the entry-scoped release helper exists for: messages still
 * queued when the port dies are freed by the drain path, which has no port to
 * uncharge. If the central charge were returned only in AlpcFreeMessage, every
 * undrained message would strand the sender's quota permanently. */
static void test_alpc_message_quota_returned_on_teardown(void)
{
    struct task *t = task_current();
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h;
    uint64_t before;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        return;
    }
    client_h = alpc_setup_pair("QuotaTeardown", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    before = quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE);

    uint8_t buf[sizeof(PORT_MESSAGE) + 16];
    PORT_MESSAGE *msg = (PORT_MESSAGE *)buf;
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    msg->TotalLength = sizeof(PORT_MESSAGE) + 16;
    msg->DataLength  = 16;
    msg->Type        = ALPC_MSG_TYPE_DATAGRAM;

    for (uint32_t i = 0; i < 3; i++)
        TEST_ASSERT_EQ((uint32_t)AlpcSendWaitReceivePort(&t->handle_table,
                                                         client_h, 0, msg,
                                                         (PORT_MESSAGE *)0, 0, 0),
                       (uint32_t)STATUS_SUCCESS, "datagram queued for teardown");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before + 3,
                   "three queued messages charge three");

    /* Tear the pair down WITHOUT draining: the port-delete queue drain must
     * return all three charges. */
    alpc_teardown_pair(client_h, server_comm);

    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), before,
                   "port teardown returns every undrained message's charge");
}

/* A sender at its message cap must get STATUS_QUOTA_EXCEEDED -- distinct from
 * the STATUS_INSUFFICIENT_RESOURCES a full port pool returns -- and the
 * refused send must leave the port's byte reservation untouched. */
static void test_alpc_message_quota_refusal_status(void)
{
    struct task *t = task_current();
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h;
    uint64_t restore, usage_now;
    HANDLE_TABLE_ENTRY *server_entry;
    ALPC_PORT *server_port;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        return;
    }
    client_h = alpc_setup_pair("QuotaRefuse", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    server_entry = ObpLookupHandle(&t->handle_table, server_comm);
    TEST_ASSERT_NOT_NULL(server_entry, "server entry lookup");
    if (!server_entry) {
        alpc_teardown_pair(client_h, server_comm);
        return;
    }
    server_port = (ALPC_PORT *)server_entry->object;

    restore   = quota_limit(t->quota, QUOTA_RES_ALPC_MESSAGE);
    usage_now = quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE);

    /* Cap at usage + 1 rather than at usage: a limit of 0 is
     * QUOTA_LIMIT_UNLIMITED, so capping at a zero usage would refuse nothing.
     * One message therefore fits and the second must not. */
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(t->quota, QUOTA_RES_ALPC_MESSAGE,
                                             usage_now + 1),
                   (uint64_t)STATUS_SUCCESS, "message budget capped");

    uint8_t buf[sizeof(PORT_MESSAGE) + 16];
    PORT_MESSAGE *msg = (PORT_MESSAGE *)buf;
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    msg->TotalLength = sizeof(PORT_MESSAGE) + 16;
    msg->DataLength  = 16;
    msg->Type        = ALPC_MSG_TYPE_DATAGRAM;

    TEST_ASSERT_EQ((uint32_t)AlpcSendWaitReceivePort(&t->handle_table, client_h,
                                                     0, msg, (PORT_MESSAGE *)0,
                                                     0, 0),
                   (uint32_t)STATUS_SUCCESS, "the message that fits is queued");

    uint64_t pool_at_cap = server_port->PoolUsageBytes;
    TEST_ASSERT_EQ((uint32_t)AlpcSendWaitReceivePort(&t->handle_table, client_h,
                                                     0, msg, (PORT_MESSAGE *)0,
                                                     0, 0),
                   (uint32_t)STATUS_QUOTA_EXCEEDED,
                   "over-cap send reports quota, not insufficient resources");
    TEST_ASSERT_EQ((uint64_t)server_port->PoolUsageBytes, pool_at_cap,
                   "a quota-refused send releases its port byte reservation");

    /* Deliberately NOT asserting PoolUsageBytes after the teardown below:
     * alpc_teardown_pair closes the last handles, so the port body is freed
     * and server_port becomes a dangling pointer. The reservation assertion
     * above, taken while the port is live, is the meaningful one. */
    (void)quota_set_limit(t->quota, QUOTA_RES_ALPC_MESSAGE, restore);
    alpc_teardown_pair(client_h, server_comm);
}

/* Charge-path cost reduction: a send refused by the message quota must perform NO
 * allocation at all.
 *
 * AlpcAllocateMessage used to kmalloc the entry (up to the maximum message
 * length) and only then consult the quota, freeing the allocation again on
 * refusal. A sender parked at its limit could therefore drive an unbounded
 * allocate/free cycle through a limit that was supposed to stop exactly that. The
 * charge now happens first, so the refusal costs no allocation.
 *
 * Proving a NEGATIVE uses the single-shot kmalloc injection as a tripwire rather
 * than as a fault: arm it, perform the over-cap send, and confirm the injection
 * is STILL ARMED afterwards. If any kmalloc had run inside the refused send it
 * would have consumed the countdown. A plain "did it return an error" assertion
 * cannot distinguish the old order from the new one. */
static void test_alpc_quota_refused_send_allocates_nothing(void)
{
    struct task *t = task_current();
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h;
    uint64_t restore, usage_now;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        return;
    }
    client_h = alpc_setup_pair("QuotaNoAlloc", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE) {
        TEST_SKIP("could not establish an ALPC port pair");
        return;
    }

    restore   = quota_limit(t->quota, QUOTA_RES_ALPC_MESSAGE);
    usage_now = quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE);

    /* Cap AT the current usage +1 so the first send fits and the second cannot,
     * matching the sibling refusal test (a limit of 0 means unlimited). */
    TEST_ASSERT_EQ((uint64_t)quota_set_limit(t->quota, QUOTA_RES_ALPC_MESSAGE,
                                             usage_now + 1),
                   (uint64_t)STATUS_SUCCESS, "message budget capped");

    uint8_t buf[sizeof(PORT_MESSAGE) + 16];
    PORT_MESSAGE *msg = (PORT_MESSAGE *)buf;
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    msg->TotalLength = sizeof(PORT_MESSAGE) + 16;
    msg->DataLength  = 16;
    msg->Type        = ALPC_MSG_TYPE_DATAGRAM;

    TEST_ASSERT_EQ((uint32_t)AlpcSendWaitReceivePort(&t->handle_table, client_h,
                                                     0, msg, (PORT_MESSAGE *)0,
                                                     0, 0),
                   (uint32_t)STATUS_SUCCESS, "the message that fits is queued");

    uint64_t injections_before = kmalloc_fail_injections_triggered();
    uint64_t usage_before      = quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE);

    /* Arm the tripwire, then make the over-cap send. */
    kmalloc_fail_next();
    NTSTATUS st = AlpcSendWaitReceivePort(&t->handle_table, client_h, 0, msg,
                                          (PORT_MESSAGE *)0, 0, 0);
    uint64_t injections_after = kmalloc_fail_injections_triggered();

    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_QUOTA_EXCEEDED,
                   "the over-cap send is refused by quota, and the armed "
                   "allocation failure is NOT what refused it");
    TEST_ASSERT_EQ(injections_after, injections_before,
                   "a quota-refused send consumed no allocation: the armed "
                   "kmalloc injection never fired");

    /* The countdown must still be live. Spend it deliberately so the arming
     * cannot leak into a later test, and confirm it was there to spend. */
    void *probe = kmalloc(16);
    TEST_ASSERT_NULL(probe,
                     "the injection was still armed after the refused send, "
                     "proving the refusal happened before any allocation");
    if (probe)
        kfree(probe);
    kmalloc_fail_countdown_clear();

    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), usage_before,
                   "and the refused send charged nothing");

    (void)quota_set_limit(t->quota, QUOTA_RES_ALPC_MESSAGE, restore);
    alpc_teardown_pair(client_h, server_comm);
}

/* The rollback branch the charge-before-allocate reorder CREATED: the quota
 * charge is live before the entry is allocated, so an allocation failure must
 * RETURN that chain. If it ever stops doing so, one QUOTA_RES_ALPC_MESSAGE unit
 * is stranded in every block of the sender's chain, permanently and with no log
 * line -- a silent leak, which is exactly why it needs an assertion rather than
 * only a code comment.
 *
 * The datagram send path's FIRST kmalloc is the entry allocation itself, so a
 * single-shot injection lands precisely on the branch under test. */
static void test_alpc_quota_returned_when_entry_alloc_fails(void)
{
    struct task *t = task_current();
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h;

    if (!t || !t->quota) {
        TEST_ASSERT(0, "current task has a process quota block");
        return;
    }
    client_h = alpc_setup_pair("QuotaAllocFail", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE) {
        TEST_SKIP("could not establish an ALPC port pair");
        return;
    }

    uint8_t buf[sizeof(PORT_MESSAGE) + 16];
    PORT_MESSAGE *msg = (PORT_MESSAGE *)buf;
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    msg->TotalLength = sizeof(PORT_MESSAGE) + 16;
    msg->DataLength  = 16;
    msg->Type        = ALPC_MSG_TYPE_DATAGRAM;

    uint64_t usage_before = quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE);

    /* Force the entry allocation to fail AFTER the charge has been placed. */
    kmalloc_fail_next();
    NTSTATUS st = AlpcSendWaitReceivePort(&t->handle_table, client_h, 0, msg,
                                         (PORT_MESSAGE *)0, 0, 0);
    kmalloc_fail_countdown_clear();

    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_INSUFFICIENT_RESOURCES,
                   "an entry allocation failure reports insufficient resources, "
                   "not quota");
    TEST_ASSERT_EQ(quota_usage(t->quota, QUOTA_RES_ALPC_MESSAGE), usage_before,
                   "the charge placed before the failed allocation is RETURNED, "
                   "stranding nothing in the sender's chain");

    alpc_teardown_pair(client_h, server_comm);
}

/* The changed allocator contract: NULL arguments and an over-length request
 * are argument errors, and out_entry must be untouched on every failure. */
static void test_alpc_allocate_message_bad_args(void)
{
    PORT_MESSAGE_ENTRY *entry = (PORT_MESSAGE_ENTRY *)0xA5A5A5A5u;
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h = alpc_setup_pair("QuotaArgs", &server_comm);
    HANDLE_TABLE_ENTRY *e;

    if (client_h == INVALID_HANDLE_VALUE)
        return;
    e = ObpLookupHandle(&task_current()->handle_table, server_comm);
    TEST_ASSERT_NOT_NULL(e, "server entry lookup");
    if (!e) {
        alpc_teardown_pair(client_h, server_comm);
        return;
    }

    TEST_ASSERT_EQ((uint32_t)AlpcAllocateMessage((ALPC_PORT *)0, 16, &entry),
                   (uint32_t)STATUS_INVALID_PARAMETER, "NULL port refused");
    TEST_ASSERT_EQ((uint32_t)AlpcAllocateMessage((ALPC_PORT *)e->object, 16,
                                                 (PORT_MESSAGE_ENTRY **)0),
                   (uint32_t)STATUS_INVALID_PARAMETER, "NULL out_entry refused");
    TEST_ASSERT_EQ((uint32_t)AlpcAllocateMessage((ALPC_PORT *)e->object,
                                                 ALPC_MAX_ALLOWED_MESSAGE_LENGTH + 1,
                                                 &entry),
                   (uint32_t)STATUS_INVALID_PARAMETER, "over-length request refused");
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)entry, (uint64_t)0xA5A5A5A5u,
                   "out_entry is untouched on every failure");

    alpc_teardown_pair(client_h, server_comm);
}

/* ----.10a PORT_CLOSED marker is uncharged (regression) ------------- */

static void test_alpc_port_closed_marker_uncharged(void)
{
    /* Catches the Codex-quality finding: a PORT_CLOSED marker queued by
     * AlpcDisconnectPort must NOT decrement peer->PoolUsageBytes when
     * the receiver dequeues it, otherwise it can subtract quota that
     * belongs to other in-flight entries. The fix stores ChargedSize=0
     * on the marker; AlpcFreeMessage skips the decrement for ChargedSize=0. */
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h = alpc_setup_pair("ClosedAcct", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    /* Send a charged datagram so server's PoolUsageBytes is non-zero. */
    uint8_t buf[sizeof(PORT_MESSAGE) + 32];
    PORT_MESSAGE *msg = (PORT_MESSAGE *)buf;
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    msg->TotalLength = sizeof(PORT_MESSAGE) + 32;
    msg->DataLength  = 32;
    msg->Type        = ALPC_MSG_TYPE_DATAGRAM;
    NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                          client_h, 0, msg,
                                          (PORT_MESSAGE *)0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "datagram queued");

    /* Snapshot server's PoolUsageBytes via the port body. The handle
     * table lookup goes through ObpLookupHandle. */
    HANDLE_TABLE_ENTRY *server_entry = ObpLookupHandle(
        &task_current()->handle_table, server_comm);
    TEST_ASSERT_NOT_NULL(server_entry, "server entry lookup");
    if (!server_entry) {
        alpc_teardown_pair(client_h, server_comm);
        return;
    }
    ALPC_PORT *server_port = (ALPC_PORT *)server_entry->object;
    uint64_t pool_before = server_port->PoolUsageBytes;
    TEST_ASSERT(pool_before > 0u,
                "server PoolUsageBytes >0 after charged send");

    /* Disconnect the client side -- this enqueues an UNCHARGED
     * PORT_CLOSED marker on the server. The server's PoolUsageBytes
     * must NOT change. */
    st = AlpcDisconnectPort(&task_current()->handle_table, client_h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "client disconnect");
    TEST_ASSERT_EQ((uint64_t)server_port->PoolUsageBytes, (uint64_t)pool_before,
                   "PORT_CLOSED enqueue must not charge peer quota");

    /* Now drain BOTH messages. After the charged datagram is freed,
     * pool drops by exactly the charged amount. After the uncharged
     * marker is freed, pool stays the same. */
    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 64];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                 server_comm, 0, (PORT_MESSAGE *)0, rx,
                                 sizeof(rxbuf), 100);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "drain 1");
    st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                 server_comm, 0, (PORT_MESSAGE *)0, rx,
                                 sizeof(rxbuf), 100);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "drain 2");
    TEST_ASSERT_EQ((uint64_t)server_port->PoolUsageBytes, 0u,
                   "after draining one charged + one uncharged, quota = 0");

    alpc_teardown_pair(client_h, server_comm);
}

/* ----.10b Remote disconnect then send => DISCONNECTED -------------- */

static void test_alpc_remote_disconnect_then_send(void)
{
    /* Tear down the SERVER side, then attempt sends from the client.
     * The client's port is still alive but the peer is marked
     * Disconnected; the engine must report STATUS_PORT_DISCONNECTED
     * and NOT alias the failure as STATUS_INSUFFICIENT_RESOURCES
     * (a regression Codex caught in the v3 implementation review). */
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h = alpc_setup_pair("RemoteDc", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    NTSTATUS st = AlpcDisconnectPort(&task_current()->handle_table,
                                     server_comm);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "remote disconnect SUCCESS");

    /* Drain the PORT_CLOSED marker the disconnect leaves on our queue
     * so the next receive doesn't shadow the test. */
    {
        uint8_t junk[sizeof(PORT_MESSAGE) + 8];
        PORT_MESSAGE *jx = (PORT_MESSAGE *)junk;
        for (uint32_t i = 0; i < sizeof(junk); i++) junk[i] = 0;
        (void)AlpcSendWaitReceivePort(&task_current()->handle_table,
                                      client_h, 0, (PORT_MESSAGE *)0, jx,
                                      sizeof(junk), 100);
    }

    uint8_t buf[sizeof(PORT_MESSAGE) + 4];
    PORT_MESSAGE *msg = (PORT_MESSAGE *)buf;
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    msg->TotalLength = sizeof(PORT_MESSAGE) + 4;
    msg->DataLength  = 4;
    msg->Type        = ALPC_MSG_TYPE_DATAGRAM;

    st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                 client_h, 0, msg, (PORT_MESSAGE *)0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_PORT_DISCONNECTED,
                   "datagram after remote disconnect => PORT_DISCONNECTED (not INSUFFICIENT_RESOURCES)");

    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 32];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    msg->Type = ALPC_MSG_TYPE_REQUEST;
    st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                 client_h, ALPC_MSGFLG_SYNC_REQUEST,
                                 msg, rx, sizeof(rxbuf), 50);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_PORT_DISCONNECTED,
                   "sync after remote disconnect => PORT_DISCONNECTED");

    alpc_teardown_pair(client_h, server_comm);
}

/* ---- Sync request with no peer (server connection port) -------- */

static void test_alpc_sync_no_peer(void)
{
    /* Server connection ports never have a ConnectedPort -- a sync
     * request issued on one must immediately return PORT_DISCONNECTED
     * without allocating anything. */
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table, "NoPeer",
                                 (ALPC_PORT_ATTRIBUTES *)0, &h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "no-peer: server CreatePort");
    if (h == INVALID_HANDLE_VALUE)
        return;

    uint8_t txbuf[sizeof(PORT_MESSAGE) + 4];
    PORT_MESSAGE *tx = (PORT_MESSAGE *)txbuf;
    for (uint32_t i = 0; i < sizeof(txbuf); i++) txbuf[i] = 0;
    tx->TotalLength = sizeof(PORT_MESSAGE) + 4;
    tx->DataLength  = 4;
    tx->Type        = ALPC_MSG_TYPE_REQUEST;

    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 32];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    st = AlpcSendWaitReceivePort(&task_current()->handle_table, h,
                                 ALPC_MSGFLG_SYNC_REQUEST,
                                 tx, rx, sizeof(rxbuf), 50);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_PORT_DISCONNECTED,
                   "sync send with no peer => PORT_DISCONNECTED");

    NtClose(&task_current()->handle_table, h);
    /* -9 LEAK retrofit. */
    test_alpc_cleanup_named("NoPeer");
}

/* ---- NtAlpcSendWaitReceivePort syscall validation -------------- */

static void test_alpc_syscall_validation(void)
{
    /* Both pointers NULL => INVALID_PARAMETER. */
    NTSTATUS st = ssdt_dispatch(SSDT_NtAlpcSendWaitReceivePort,
                                0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_INVALID_PARAMETER,
                   "syscall: send=NULL, recv=NULL => INVALID_PARAMETER");

    /* recv_buf_len < sizeof(PORT_MESSAGE) but recv_msg present =>
     * BUFFER_TOO_SMALL. */
    {
        PORT_MESSAGE rx;
        st = ssdt_dispatch(SSDT_NtAlpcSendWaitReceivePort,
                           0, 0, 0, (uint64_t)&rx,
                           sizeof(PORT_MESSAGE) - 1, 0);
        TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_BUFFER_TOO_SMALL,
                       "syscall: tiny recv_buf_len => BUFFER_TOO_SMALL");
    }

    /* send_msg with bogus DataLength => INVALID_PARAMETER. */
    {
        PORT_MESSAGE tx;
        uint8_t *zp = (uint8_t *)&tx;
        for (uint32_t i = 0; i < sizeof(tx); i++) zp[i] = 0;
        tx.TotalLength = sizeof(tx);
        tx.DataLength  = ALPC_MAX_ALLOWED_MESSAGE_LENGTH + 1;
        tx.Type        = ALPC_MSG_TYPE_DATAGRAM;
        st = ssdt_dispatch(SSDT_NtAlpcSendWaitReceivePort, 0, 0,
                           (uint64_t)&tx, 0, 0, 0);
        TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_INVALID_PARAMETER,
                       "syscall: oversized DataLength => INVALID_PARAMETER");
    }

    /* recv_msg=NULL with SYNC_REQUEST flag => INVALID_PARAMETER. */
    {
        PORT_MESSAGE tx;
        uint8_t *zp = (uint8_t *)&tx;
        for (uint32_t i = 0; i < sizeof(tx); i++) zp[i] = 0;
        tx.TotalLength = sizeof(tx);
        tx.DataLength  = 0;
        tx.Type        = ALPC_MSG_TYPE_REQUEST;
        st = ssdt_dispatch(SSDT_NtAlpcSendWaitReceivePort, 0,
                           ALPC_MSGFLG_SYNC_REQUEST,
                           (uint64_t)&tx, 0, 0, 0);
        TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_INVALID_PARAMETER,
                       "syscall: SYNC_REQUEST without recv => INVALID_PARAMETER");
    }
}

/* =========================================================================
 * retrofits -- ALPC-side test gaps closed by the kernel test harness
 * primitives (kmalloc fault injection, race barrier, scratch helper,
 * klog level demotion). The three gaps below were originally deferred
 * because the kernel test harness lacked deterministic kmalloc-fault
 * injection, two-thread race fences, and PMM-backed scratch buffers.
 *
 *   (a) Allocator kmalloc-failure rollback (uncharge-under-lock) using
 *       `kmalloc_fail_next()` + `TEST_KLOG_SUPPRESS("alpc")`.
 *   (b) ReplyBodyCap clamping with `recv_buf_len > 65528` using
 *       `TEST_SCRATCH_KBUF` (PMM route, > 4 KiB).
 *   (c) Address-ordered two-port locking concurrency stress using
 *       `test_race_barrier_t` between two sender threads going in
 *       opposite directions.
 * =======================================================================*/

/* ---- retrofit (a): kmalloc-failure rollback uncharges under lock --- */

static void test_alpc_kmalloc_fail_rollback(void)
{
    /* Arming kmalloc_fail_next() makes the very next kmalloc on this
     * CPU return NULL. The first kmalloc inside alpc_sync_request is
     * inside alpc_alloc_pending_reply, which has already incremented
     * sender_port->PoolUsageBytes under the port lock before calling
     * kmalloc. The rollback path must re-take the lock and decrement
     * PoolUsageBytes back to its prior value. The leaked bytes / pool
     * accounting is the regression we're locking down.
     *
     * Suppress alpc-subsystem klog noise -- the engine logs the
     * INSUFFICIENT_RESOURCES path at LOG_DEBUG so this is purely
     * cosmetic; the suppress proves the primitive integrates with
     * a real failure-path test. */
    TEST_KLOG_SUPPRESS("alpc");

    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h = alpc_setup_pair("KmFailRb", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    /* Snapshot the sender port's PoolUsageBytes via the handle table. */
    HANDLE_TABLE_ENTRY *client_entry = ObpLookupHandle(
        &task_current()->handle_table, client_h);
    TEST_ASSERT_NOT_NULL(client_entry, "client port entry lookup");
    if (!client_entry) {
        alpc_teardown_pair(client_h, server_comm);
        return;
    }
    ALPC_PORT *client_port = (ALPC_PORT *)client_entry->object;
    uint64_t pool_before = client_port->PoolUsageBytes;

    /* Build a small sync request -- the actual body length doesn't
     * matter; only the allocator path does. */
    uint8_t txbuf[sizeof(PORT_MESSAGE) + 16];
    PORT_MESSAGE *tx = (PORT_MESSAGE *)txbuf;
    for (uint32_t i = 0; i < sizeof(txbuf); i++) txbuf[i] = 0;
    tx->TotalLength = sizeof(PORT_MESSAGE) + 16;
    tx->DataLength  = 16;
    tx->Type        = ALPC_MSG_TYPE_REQUEST;

    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 64];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    for (uint32_t i = 0; i < sizeof(rxbuf); i++) rxbuf[i] = 0;

    /* Arm the next-kmalloc-fails trap and immediately call the engine.
     * The engine's first kmalloc is inside alpc_alloc_pending_reply,
     * which is the function under test for the rollback path. */
    kmalloc_fail_next();

    NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                          client_h,
                                          ALPC_MSGFLG_SYNC_REQUEST,
                                          tx, rx, sizeof(rxbuf), 100);

    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_INSUFFICIENT_RESOURCES,
                   "kmalloc-fail in alpc_alloc_pending_reply -> INSUFFICIENT_RESOURCES");

    /* The whole point of the test: PoolUsageBytes must be unchanged
     * after the rollback. A leak would surface as a positive delta. */
    TEST_ASSERT_EQ((uint64_t)client_port->PoolUsageBytes,
                   (uint64_t)pool_before,
                   "PoolUsageBytes restored on kmalloc rollback path");

    alpc_teardown_pair(client_h, server_comm);
}

/* ---- retrofit (b): ReplyBodyCap clamps to 65528 when recv > 65528 -- */

static volatile NTSTATUS s_clamp_server_status;

static void clamp_server_worker(void *arg)
{
    HANDLE server_comm = (HANDLE)(uintptr_t)arg;
    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 64];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    for (uint32_t i = 0; i < sizeof(rxbuf); i++) rxbuf[i] = 0;

    NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                          server_comm, 0,
                                          (PORT_MESSAGE *)0, rx,
                                          sizeof(rxbuf), 5000);
    if (st != STATUS_SUCCESS) {
        s_clamp_server_status = st;
        return;
    }

    /* Reply with a 32-byte body. The reply size doesn't probe the
     * clamp -- the clamp lives on the SENDER's pending-record cap.
     * What we want is for the round-trip to complete cleanly so the
     * client's PoolUsageBytes can be inspected post-completion. */
    uint8_t txbuf[sizeof(PORT_MESSAGE) + 32];
    PORT_MESSAGE *tx = (PORT_MESSAGE *)txbuf;
    for (uint32_t i = 0; i < sizeof(txbuf); i++) txbuf[i] = 0;
    tx->TotalLength = sizeof(PORT_MESSAGE) + 32;
    tx->DataLength  = 32;
    tx->Type        = ALPC_MSG_TYPE_REPLY;
    tx->MessageId   = rx->MessageId;
    for (uint32_t i = 0; i < 32; i++)
        ((uint8_t *)tx + sizeof(PORT_MESSAGE))[i] = (uint8_t)i;

    s_clamp_server_status = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                                    server_comm,
                                                    ALPC_MSGFLG_REPLY_MESSAGE,
                                                    tx, (PORT_MESSAGE *)0, 0, 0);
}

static void test_alpc_reply_body_cap_clamped(void)
{
    /* alpc_sync_request computes:
     *   cap = recv_buf_len - sizeof(PORT_MESSAGE);
     *   if (cap > ALPC_MAX_ALLOWED_MESSAGE_LENGTH) cap = ALPC_MAX_ALLOWED_MESSAGE_LENGTH;
     *
     * Pre-clamp this would overflow the pending record's inline body
     * size. We pass recv_buf_len = 65536 + 64 (raw cap = 65600 > 65528),
     * forcing the clamp branch. The PMM-backed TEST_SCRATCH_KBUF route
     * is required because the buffer is > 4 KiB. After the round-trip,
     * the sender port's PoolUsageBytes returns to its pre-call value
     * because alpc_free_pending_reply uncharges the clamped allocation,
     * not the raw caller-provided length. */
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h = alpc_setup_pair("ClampPair", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    HANDLE_TABLE_ENTRY *client_entry = ObpLookupHandle(
        &task_current()->handle_table, client_h);
    TEST_ASSERT_NOT_NULL(client_entry, "clamp: client port entry lookup");
    if (!client_entry) {
        alpc_teardown_pair(client_h, server_comm);
        return;
    }
    ALPC_PORT *client_port = (ALPC_PORT *)client_entry->object;
    uint64_t pool_before = client_port->PoolUsageBytes;

    /* PMM-backed scratch -- 65 KiB + 64 B exceeds the 4 KiB kmalloc
     * threshold, so test_scratch_alloc dispatches to
     * pmm_alloc_contiguous(17 pages) and registers cleanup via
     * test_add_action. */
    TEST_SCRATCH_KBUF(rxbuf, 65536 + 64);
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    /* zero the header portion -- the full 65 KiB does not need
     * pre-zero for the engine, only the header needs clean fields. */
    for (uint32_t i = 0; i < sizeof(PORT_MESSAGE); i++)
        ((uint8_t *)rx)[i] = 0;

    s_clamp_server_status = STATUS_INVALID_PARAMETER;
    int tid = kthread_create(clamp_server_worker,
                             (void *)(uintptr_t)server_comm, 0);
    TEST_ASSERT(tid >= 0, "clamp: server worker spawned");

    uint8_t txbuf[sizeof(PORT_MESSAGE) + 16];
    PORT_MESSAGE *tx = (PORT_MESSAGE *)txbuf;
    for (uint32_t i = 0; i < sizeof(txbuf); i++) txbuf[i] = 0;
    tx->TotalLength = sizeof(PORT_MESSAGE) + 16;
    tx->DataLength  = 16;
    tx->Type        = ALPC_MSG_TYPE_REQUEST;

    /* recv_buf_len = 65536 + 64 -- forces the clamp branch in
     * alpc_sync_request. Without the clamp, alpc_alloc_pending_reply
     * would attempt to allocate ALPC_PENDING_REPLY + 65560 bytes,
     * which exceeds the protocol ceiling. With the clamp, the alloc
     * is bounded at ALPC_PENDING_REPLY + 65528 bytes. */
    NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                          client_h,
                                          ALPC_MSGFLG_SYNC_REQUEST,
                                          tx, rx, 65536 + 64, 5000);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "clamped sync request returns SUCCESS");
    TEST_ASSERT_EQ((uint32_t)rx->Type, (uint32_t)ALPC_MSG_TYPE_REPLY,
                   "clamped path: reply Type=REPLY");
    TEST_ASSERT_EQ((uint32_t)rx->DataLength, 32u,
                   "clamped path: reply DataLength=32 (not 65528)");

    thread_join((uint32_t)tid);
    TEST_ASSERT_EQ((uint32_t)s_clamp_server_status, (uint32_t)STATUS_SUCCESS,
                   "clamp: server reply succeeded");

    /* Pool usage returned to baseline -- proves alpc_free_pending_reply
     * uncharged the CLAMPED allocation size, not the raw recv_buf_len. */
    TEST_ASSERT_EQ((uint64_t)client_port->PoolUsageBytes,
                   (uint64_t)pool_before,
                   "PoolUsageBytes restored after clamped pending free");

    alpc_teardown_pair(client_h, server_comm);
}

/* ---- retrofit (c): two-port lock-order concurrency stress ---------- */

static test_race_barrier_t s_lock_order_barrier;

/* The address-ordered double-lock in alpc_lock_two only matters when
 * two threads enter alpc_sync_request from OPPOSITE endpoints of the
 * same connected pair. Sender A sends client -> server (sender_port =
 * client_port, peer = server_port); sender B sends server -> client
 * (sender_port = server_port, peer = client_port). The two paths
 * present the SAME two ALPC_PORT objects in OPPOSITE logical order;
 * without alpc_lock_two normalizing by address, a naive
 * `spin_lock(sender); spin_lock(peer);` would deadlock on real SMP.
 *
 * Test topology (4 kthreads per iteration):
 *   - sender A (client -> server)    -- pinned at barrier 'a'
 *   - sender B (server -> client)    -- pinned at barrier 'b'
 *   - receiver A (drains server side, replies to A)
 *   - receiver B (drains client side, replies to B)
 *
 * SCOPE LIMITATION (Codex quality review, 2026-04-19): the
 * race_barrier pins the senders BEFORE they enter
 * AlpcSendWaitReceivePort, not at the alpc_lock_two acquisition
 * point itself. In the cooperative single-CPU case (TCG without
 * -smp), sender A can run through validation, alloc, and the lock
 * acquire/release cycle BEFORE sender B is rescheduled, so the
 * test cannot deterministically pin the lock window. To compensate:
 *
 *   - On single-CPU it is a smoke test for opposite-direction
 *     sync_request semantics (which is itself useful coverage --
 * no other test exercises both directions of a single pair
 *     in the same suite).
 *   - On 2-CPU WHPX / KVM-multi-cpu / bare metal, both senders
 *     hit alpc_sync_request within microseconds of each other,
 *     and the inner work is brief enough that the lock acquisition
 *     windows OVERLAP probabilistically. We loop the
 *     spawn-release-join cycle 4 times to multiply the catch rate
 *     for a true lock-order regression.
 *
 * Deterministic lock-window pinning would require a test-only hook
 * inside alpc_lock_two (a yield between the two spin_lock calls in
 * the broken / unpatched form). That belongs with future ALPC SMP
 * hardening once the engine grows test instrumentation; out of the
 * test-harness retrofit scope tracked here. */

#define LOCK_ORDER_STRESS_ITERS 4

static volatile NTSTATUS s_lo_sender_a_status;
static volatile NTSTATUS s_lo_sender_b_status;
static volatile NTSTATUS s_lo_recv_a_status;
static volatile NTSTATUS s_lo_recv_b_status;
static HANDLE            s_lo_client_h;
static HANDLE            s_lo_server_comm;

static void lock_order_sender_a(void *arg)
{
    /* client -> server direction: alpc_lock_two(client_port, server_port). */
    (void)arg;
    uint8_t txbuf[sizeof(PORT_MESSAGE) + 8];
    PORT_MESSAGE *tx = (PORT_MESSAGE *)txbuf;
    for (uint32_t i = 0; i < sizeof(txbuf); i++) txbuf[i] = 0;
    tx->TotalLength = sizeof(PORT_MESSAGE) + 8;
    tx->DataLength  = 8;
    tx->Type        = ALPC_MSG_TYPE_REQUEST;
    for (uint32_t i = 0; i < 8; i++)
        ((uint8_t *)tx + sizeof(PORT_MESSAGE))[i] = (uint8_t)('A');

    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 32];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    for (uint32_t i = 0; i < sizeof(rxbuf); i++) rxbuf[i] = 0;

    test_race_barrier_arrive_a(&s_lock_order_barrier);

    s_lo_sender_a_status = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                                   s_lo_client_h,
                                                   ALPC_MSGFLG_SYNC_REQUEST,
                                                   tx, rx, sizeof(rxbuf), 5000);
}

static void lock_order_sender_b(void *arg)
{
    /* server -> client direction: alpc_lock_two(server_port, client_port).
     * Same two ALPC_PORT objects as sender A but in opposite logical
     * order. alpc_lock_two normalizes by address; without it this is a
     * lock-order inversion. */
    (void)arg;
    uint8_t txbuf[sizeof(PORT_MESSAGE) + 8];
    PORT_MESSAGE *tx = (PORT_MESSAGE *)txbuf;
    for (uint32_t i = 0; i < sizeof(txbuf); i++) txbuf[i] = 0;
    tx->TotalLength = sizeof(PORT_MESSAGE) + 8;
    tx->DataLength  = 8;
    tx->Type        = ALPC_MSG_TYPE_REQUEST;
    for (uint32_t i = 0; i < 8; i++)
        ((uint8_t *)tx + sizeof(PORT_MESSAGE))[i] = (uint8_t)('B');

    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 32];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    for (uint32_t i = 0; i < sizeof(rxbuf); i++) rxbuf[i] = 0;

    test_race_barrier_arrive_b(&s_lock_order_barrier);

    s_lo_sender_b_status = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                                   s_lo_server_comm,
                                                   ALPC_MSGFLG_SYNC_REQUEST,
                                                   tx, rx, sizeof(rxbuf), 5000);
}

static void lock_order_recv_a(void *arg)
{
    /* Drain on server side, reply to sender A's request. */
    (void)arg;
    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 32];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    for (uint32_t i = 0; i < sizeof(rxbuf); i++) rxbuf[i] = 0;

    NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                          s_lo_server_comm, 0,
                                          (PORT_MESSAGE *)0, rx,
                                          sizeof(rxbuf), 5000);
    if (st != STATUS_SUCCESS) {
        s_lo_recv_a_status = st;
        return;
    }

    uint8_t txbuf[sizeof(PORT_MESSAGE) + 8];
    PORT_MESSAGE *tx = (PORT_MESSAGE *)txbuf;
    for (uint32_t i = 0; i < sizeof(txbuf); i++) txbuf[i] = 0;
    tx->TotalLength = sizeof(PORT_MESSAGE) + 8;
    tx->DataLength  = 8;
    tx->Type        = ALPC_MSG_TYPE_REPLY;
    tx->MessageId   = rx->MessageId;
    for (uint32_t i = 0; i < 8; i++)
        ((uint8_t *)tx + sizeof(PORT_MESSAGE))[i] =
            ((uint8_t *)rx + sizeof(PORT_MESSAGE))[i] ^ 0xFFu;

    s_lo_recv_a_status = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                                 s_lo_server_comm,
                                                 ALPC_MSGFLG_REPLY_MESSAGE,
                                                 tx, (PORT_MESSAGE *)0, 0, 0);
}

static void lock_order_recv_b(void *arg)
{
    /* Drain on client side, reply to sender B's request. */
    (void)arg;
    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 32];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    for (uint32_t i = 0; i < sizeof(rxbuf); i++) rxbuf[i] = 0;

    NTSTATUS st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                          s_lo_client_h, 0,
                                          (PORT_MESSAGE *)0, rx,
                                          sizeof(rxbuf), 5000);
    if (st != STATUS_SUCCESS) {
        s_lo_recv_b_status = st;
        return;
    }

    uint8_t txbuf[sizeof(PORT_MESSAGE) + 8];
    PORT_MESSAGE *tx = (PORT_MESSAGE *)txbuf;
    for (uint32_t i = 0; i < sizeof(txbuf); i++) txbuf[i] = 0;
    tx->TotalLength = sizeof(PORT_MESSAGE) + 8;
    tx->DataLength  = 8;
    tx->Type        = ALPC_MSG_TYPE_REPLY;
    tx->MessageId   = rx->MessageId;
    for (uint32_t i = 0; i < 8; i++)
        ((uint8_t *)tx + sizeof(PORT_MESSAGE))[i] =
            ((uint8_t *)rx + sizeof(PORT_MESSAGE))[i] ^ 0xFFu;

    s_lo_recv_b_status = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                                 s_lo_client_h,
                                                 ALPC_MSGFLG_REPLY_MESSAGE,
                                                 tx, (PORT_MESSAGE *)0, 0, 0);
}

static void test_alpc_two_port_lock_order_stress(void)
{
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    HANDLE client_h = alpc_setup_pair("LockOrder", &server_comm);
    if (client_h == INVALID_HANDLE_VALUE)
        return;

    s_lo_client_h    = client_h;
    s_lo_server_comm = server_comm;

    /* Loop the concurrent spawn-release-join cycle to multiply the
     * SMP catch rate for a true lock-order regression. Each iteration
     * spawns 4 fresh kthreads and reinitialises the barrier. On
     * single-CPU TCG this is just a slightly more expensive smoke
     * test (~4 sync round-trips); on multi-CPU guests it raises the
     * probability of overlapping lock acquisition windows. */
    for (int iter = 0; iter < LOCK_ORDER_STRESS_ITERS; iter++) {
        test_race_barrier_init(&s_lock_order_barrier);
        s_lo_sender_a_status = STATUS_INVALID_PARAMETER;
        s_lo_sender_b_status = STATUS_INVALID_PARAMETER;
        s_lo_recv_a_status   = STATUS_INVALID_PARAMETER;
        s_lo_recv_b_status   = STATUS_INVALID_PARAMETER;

        /* Spawn receivers first so they're blocked on
         * AlpcSendWaitReceive (receive-only) when senders fire. */
        int recv_a_tid = kthread_create(lock_order_recv_a, (void *)0, 0);
        TEST_ASSERT(recv_a_tid >= 0, "lock-order: receiver A spawned");
        int recv_b_tid = kthread_create(lock_order_recv_b, (void *)0, 0);
        TEST_ASSERT(recv_b_tid >= 0, "lock-order: receiver B spawned");
        int send_a_tid = kthread_create(lock_order_sender_a, (void *)0, 0);
        TEST_ASSERT(send_a_tid >= 0, "lock-order: sender A spawned");
        int send_b_tid = kthread_create(lock_order_sender_b, (void *)0, 0);
        if (send_b_tid < 0) {
            /* Slot pressure: release barrier so sender A is not
             * parked forever, then join everyone so the test exits
             * cleanly instead of deadlocking. */
            test_race_barrier_release(&s_lock_order_barrier, /*a_first=*/1);
            thread_join((uint32_t)send_a_tid);
            thread_join((uint32_t)recv_a_tid);
            thread_join((uint32_t)recv_b_tid);
            TEST_ASSERT(send_b_tid >= 0, "lock-order: sender B spawned");
            alpc_teardown_pair(client_h, server_comm);
            return;
        }

        /* Both senders pinned at the barrier; release a-first. */
        test_race_barrier_release(&s_lock_order_barrier, /*a_first=*/1);

        thread_join((uint32_t)send_a_tid);
        thread_join((uint32_t)send_b_tid);
        thread_join((uint32_t)recv_a_tid);
        thread_join((uint32_t)recv_b_tid);

        /* If alpc_lock_two were broken (naive sender-then-peer
         * locking), the opposite-direction senders would deadlock
         * on SMP and the 5s AlpcSendWaitReceivePort timeout would
         * fire on at least one worker -- the SUCCESS assertion
         * catches that regression. We assert per-iteration so a
         * mid-loop regression surfaces with the iteration count. */
        TEST_ASSERT_EQ((uint32_t)s_lo_sender_a_status, (uint32_t)STATUS_SUCCESS,
                       "lock-order: sender A (client->server) sync_request SUCCESS");
        TEST_ASSERT_EQ((uint32_t)s_lo_sender_b_status, (uint32_t)STATUS_SUCCESS,
                       "lock-order: sender B (server->client) sync_request SUCCESS");
        TEST_ASSERT_EQ((uint32_t)s_lo_recv_a_status, (uint32_t)STATUS_SUCCESS,
                       "lock-order: receiver A reply SUCCESS");
        TEST_ASSERT_EQ((uint32_t)s_lo_recv_b_status, (uint32_t)STATUS_SUCCESS,
                       "lock-order: receiver B reply SUCCESS");
        TEST_ASSERT_EQ((uint8_t)s_lock_order_barrier.release_timed_out, 0u,
                       "lock-order: barrier release did not time out");
    }

    alpc_teardown_pair(client_h, server_comm);
}

/* =========================================================================
 * Asynchronous Delivery & Completion List tests
 *
 * Covers:
 *   - ALPC_COMPLETION_LIST_ITEM ABI (sizeof lock)
 *   - Waitable port: NtWaitForSingleObject signalled on message, cleared on drain
 *   - Non-waitable port rejected by NtWaitForSingleObject
 *   - NtAlpcSetInformation(AlpcAssociateCompletionPort) success + bad handles
 *   - IOCP notification: datagram send with associated completion port
 *     enqueues both on MessageQueue (body stays) AND IOCP (notification)
 * =======================================================================*/

#include "kernel/nt/nt_file.h"          /* IO_COMPLETION_PORT + io_completion_post */

static void test_alpc_completion_list_item_layout(void)
{
    TEST_ASSERT_EQ(sizeof(ALPC_COMPLETION_LIST_ITEM), 24u,
                   "ALPC_COMPLETION_LIST_ITEM sizeof == 24");
    TEST_ASSERT_EQ(__builtin_offsetof(ALPC_COMPLETION_LIST_ITEM, Message), 0u,
                   "ALPC_COMPLETION_LIST_ITEM.Message at offset 0");
    TEST_ASSERT_EQ(__builtin_offsetof(ALPC_COMPLETION_LIST_ITEM, PortContext), 8u,
                   "ALPC_COMPLETION_LIST_ITEM.PortContext at offset 8");
    TEST_ASSERT_EQ(__builtin_offsetof(ALPC_COMPLETION_LIST_ITEM, MessageFlags), 16u,
                   "ALPC_COMPLETION_LIST_ITEM.MessageFlags at offset 16");
}

/* ---- Waitable port: NtWaitForSingleObject signalled on msg ------ */

static void test_alpc_waitable_port_signal(void)
{
    HANDLE server_conn = INVALID_HANDLE_VALUE;
    ALPC_PORT_ATTRIBUTES attrs;
    uint8_t *zp = (uint8_t *)&attrs;
    for (uint32_t i = 0; i < sizeof(attrs); i++) zp[i] = 0;
    attrs.Flags = ALPC_PORTFLG_WAITABLE_PORT;

    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "WaitPort", &attrs, &server_conn);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "waitable: server CreatePort");
    if (server_conn == INVALID_HANDLE_VALUE)
        return;

    s_sync_target_name = "\\RPC Control\\WaitPort";
    s_sync_client_handle = INVALID_HANDLE_VALUE;
    s_sync_worker_done = 0;
    int tid = kthread_create(s4_connect_worker, (void *)0, 0);
    TEST_ASSERT(tid >= 0, "waitable: worker");
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    (void)AlpcAcceptConnectPort(&task_current()->handle_table,
                                server_conn, 1, 5000, &server_comm);
    thread_join((uint32_t)tid);
    HANDLE client_h = s_sync_client_handle;
    if (client_h == INVALID_HANDLE_VALUE) {
        if (server_comm != INVALID_HANDLE_VALUE)
            NtClose(&task_current()->handle_table, server_comm);
        NtClose(&task_current()->handle_table, server_conn);
        test_alpc_cleanup_named("WaitPort");
        return;
    }

    /* With empty queue, NtWaitForSingleObject with timeout=0 returns TIMEOUT. */
    int64_t zero_timeout = 0;
    st = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForSingleObject,
                                 (uint64_t)(uintptr_t)server_comm, 0,
                                 (uint64_t)(uintptr_t)&zero_timeout,
                                 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_TIMEOUT,
                   "waitable empty + timeout=0 => TIMEOUT");

    /* Client sends a datagram -> port becomes signalled. */
    uint8_t buf[sizeof(PORT_MESSAGE) + 4];
    PORT_MESSAGE *msg = (PORT_MESSAGE *)buf;
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    msg->TotalLength = sizeof(PORT_MESSAGE) + 4;
    msg->DataLength  = 4;
    msg->Type        = ALPC_MSG_TYPE_DATAGRAM;
    st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                 client_h, 0, msg, (PORT_MESSAGE *)0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "datagram sent");

    /* Wait with timeout=0 should now succeed (queue non-empty). */
    st = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForSingleObject,
                                 (uint64_t)(uintptr_t)server_comm, 0,
                                 (uint64_t)(uintptr_t)&zero_timeout,
                                 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "waitable + msg queued => SUCCESS");

    /* Drain the message, port should become unsignalled. */
    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 32];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                 server_comm, 0, (PORT_MESSAGE *)0, rx,
                                 sizeof(rxbuf), 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "drain");

    st = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForSingleObject,
                                 (uint64_t)(uintptr_t)server_comm, 0,
                                 (uint64_t)(uintptr_t)&zero_timeout,
                                 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_TIMEOUT,
                   "waitable drained => TIMEOUT again");

    /* -9 LEAK retrofit: break cross-link before close. */
    (void)AlpcDisconnectPort(&task_current()->handle_table, server_comm);
    NtClose(&task_current()->handle_table, client_h);
    NtClose(&task_current()->handle_table, server_comm);
    NtClose(&task_current()->handle_table, server_conn);
    test_alpc_cleanup_named("WaitPort");
}

/* ---- Non-waitable port rejected ---------------------------------- */

static void test_alpc_nonwaitable_rejects(void)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "NotWaitable", (ALPC_PORT_ATTRIBUTES *)0, &h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "nw: create");
    if (h == INVALID_HANDLE_VALUE)
        return;

    int64_t zero = 0;
    st = (NTSTATUS)ssdt_dispatch(SSDT_NtWaitForSingleObject,
                                 (uint64_t)(uintptr_t)h, 0,
                                 (uint64_t)(uintptr_t)&zero,
                                 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_OBJECT_TYPE_MISMATCH,
                   "non-waitable port rejects NtWaitForSingleObject");

    NtClose(&task_current()->handle_table, h);
    /* -9 LEAK retrofit. */
    test_alpc_cleanup_named("NotWaitable");
}

/* ----.1/NtAlpcSetInformation: AssociateCompletionPort --------- */

static void test_alpc_associate_completion_port(void)
{
    /* Create an IOCP. */
    HANDLE iocp = INVALID_HANDLE_VALUE;
    NTSTATUS st = (NTSTATUS)ssdt_dispatch(SSDT_NtCreateIoCompletion,
                                          (uint64_t)(uintptr_t)&iocp,
                                          0, 0, 0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "iocp: create");
    TEST_ASSERT_NEQ(iocp, INVALID_HANDLE_VALUE, "iocp: handle valid");

    /* Create an ALPC port (non-waitable is fine -- IOCP path is
     * orthogonal to waitable port). */
    HANDLE server_conn = INVALID_HANDLE_VALUE;
    st = AlpcCreatePort(&task_current()->handle_table,
                       "IocpPort", (ALPC_PORT_ATTRIBUTES *)0, &server_conn);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "iocp: alpc create");

    s_sync_target_name = "\\RPC Control\\IocpPort";
    s_sync_client_handle = INVALID_HANDLE_VALUE;
    s_sync_worker_done = 0;
    int tid = kthread_create(s4_connect_worker, (void *)0, 0);
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    (void)AlpcAcceptConnectPort(&task_current()->handle_table,
                                server_conn, 1, 5000, &server_comm);
    thread_join((uint32_t)tid);
    HANDLE client_h = s_sync_client_handle;
    if (client_h == INVALID_HANDLE_VALUE) {
        if (server_comm != INVALID_HANDLE_VALUE)
            NtClose(&task_current()->handle_table, server_comm);
        NtClose(&task_current()->handle_table, server_conn);
        if (iocp != INVALID_HANDLE_VALUE)
            NtClose(&task_current()->handle_table, iocp);
        test_alpc_cleanup_named("IocpPort");
        return;
    }

    /* Associate the IOCP with the server-comm port. */
    ALPC_PORT_ASSOCIATE_COMPLETION_PORT assoc;
    assoc.CompletionPort = iocp;
    assoc.CompletionKey  = 0xCAFE5678ul;
    st = (NTSTATUS)ssdt_dispatch(
        SSDT_NtAlpcSetInformation,
        (uint64_t)(uintptr_t)server_comm,
        AlpcAssociateCompletionPortInformation,
        (uint64_t)(uintptr_t)&assoc,
        sizeof(assoc), 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "SetInfo: AssociateCompletionPort SUCCESS");

    /* Client sends a datagram. The IOCP MUST receive a notification. */
    uint8_t buf[sizeof(PORT_MESSAGE) + 8];
    PORT_MESSAGE *msg = (PORT_MESSAGE *)buf;
    for (uint32_t i = 0; i < sizeof(buf); i++) buf[i] = 0;
    msg->TotalLength = sizeof(PORT_MESSAGE) + 8;
    msg->DataLength  = 8;
    msg->Type        = ALPC_MSG_TYPE_DATAGRAM;
    st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                 client_h, 0, msg, (PORT_MESSAGE *)0, 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "iocp: datagram");

    /* Dequeue the completion packet. Key must match, Information == DataLength. */
    uint64_t key_out = 0, apc_out = 0;
    IO_STATUS_BLOCK iosb;
    iosb.Status = 0xDEADBEEFu;
    iosb.Information = 0xBAD0BAD0u;
    st = (NTSTATUS)ssdt_dispatch(SSDT_NtRemoveIoCompletion,
                                 (uint64_t)(uintptr_t)iocp,
                                 (uint64_t)(uintptr_t)&key_out,
                                 (uint64_t)(uintptr_t)&apc_out,
                                 (uint64_t)(uintptr_t)&iosb,
                                 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "iocp: RemoveIoCompletion returns the notification");
    TEST_ASSERT_EQ(key_out, 0xCAFE5678ul,
                   "iocp: CompletionKey round-trips");
    TEST_ASSERT_EQ((uint64_t)iosb.Information, 8ul,
                   "iocp: IoStatusInformation == DataLength");
    TEST_ASSERT_EQ((uint32_t)iosb.Status, (uint32_t)STATUS_SUCCESS,
                   "iocp: IoStatus == SUCCESS");

    /* The message body MUST still be on MessageQueue (notification, not
     * delivery replacement). Drain it via normal receive. */
    uint8_t rxbuf[sizeof(PORT_MESSAGE) + 32];
    PORT_MESSAGE *rx = (PORT_MESSAGE *)rxbuf;
    st = AlpcSendWaitReceivePort(&task_current()->handle_table,
                                 server_comm, 0, (PORT_MESSAGE *)0, rx,
                                 sizeof(rxbuf), 100);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS,
                   "iocp: body still on MessageQueue (notification-only)");
    TEST_ASSERT_EQ((uint32_t)rx->DataLength, 8u,
                   "iocp: body DataLength intact");

    /* -9 LEAK retrofit: break cross-link + drop IOCP handle. */
    (void)AlpcDisconnectPort(&task_current()->handle_table, server_comm);
    NtClose(&task_current()->handle_table, client_h);
    NtClose(&task_current()->handle_table, server_comm);
    NtClose(&task_current()->handle_table, server_conn);
    NtClose(&task_current()->handle_table, iocp);
    test_alpc_cleanup_named("IocpPort");
}

/* ---- SetInfo with bad IOCP handle ------------------------------------- */

static void test_alpc_associate_bad_iocp(void)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "BadIocp", (ALPC_PORT_ATTRIBUTES *)0, &h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "badiocp: create");
    if (h == INVALID_HANDLE_VALUE)
        return;

    /* Handle 0x0000 is below the IOCP offset -- must reject. */
    ALPC_PORT_ASSOCIATE_COMPLETION_PORT assoc;
    assoc.CompletionPort = (HANDLE)0;
    assoc.CompletionKey  = 0;
    st = (NTSTATUS)ssdt_dispatch(
        SSDT_NtAlpcSetInformation,
        (uint64_t)(uintptr_t)h,
        AlpcAssociateCompletionPortInformation,
        (uint64_t)(uintptr_t)&assoc,
        sizeof(assoc), 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_INVALID_HANDLE,
                   "bad IOCP handle => INVALID_HANDLE");

    NtClose(&task_current()->handle_table, h);
    /* -9 LEAK retrofit. */
    test_alpc_cleanup_named("BadIocp");
}

/* ---- SetInfo unknown info class --------------------------------------- */

static void test_alpc_setinfo_unknown_class(void)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "BadClass", (ALPC_PORT_ATTRIBUTES *)0, &h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "badclass: create");
    if (h == INVALID_HANDLE_VALUE)
        return;

    uint64_t dummy = 0;
    st = (NTSTATUS)ssdt_dispatch(SSDT_NtAlpcSetInformation,
                                 (uint64_t)(uintptr_t)h,
                                 /* info_class */ 99,
                                 (uint64_t)(uintptr_t)&dummy,
                                 sizeof(dummy), 0, 0);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_NOT_IMPLEMENTED,
                   "unknown info class => NOT_IMPLEMENTED (pending section 9)");

    NtClose(&task_current()->handle_table, h);
    /* -9 LEAK retrofit. */
    test_alpc_cleanup_named("BadClass");
}

/* ================= Section 7: security context & impersonation ========= */
/* The kernel test task may not yet carry a primary token (SRM assigns one
 * lazily). Client-token capture needs the connecting task to have one, so
 * these tests install a temporary SYSTEM token when absent and restore the
 * prior value afterwards. Tests run serially, so a single saved slot is safe. */
static void *s_s7_prev_token;
static int   s_s7_token_created;

static void alpc_s7_ensure_token(void)
{
    struct task *cur = task_current();
    s_s7_prev_token   = cur->token;
    s_s7_token_created = 0;
    if (!cur->token) {
        ACCESS_TOKEN *t = SeCreateSystemToken();
        if (t) {
            cur->token = t;
            s_s7_token_created = 1;
        }
    }
}

static void alpc_s7_restore_token(void)
{
    struct task *cur = task_current();
    if (s_s7_token_created && cur->token) {
        ObDereferenceObject(cur->token);
        cur->token = s_s7_prev_token;
    }
    s_s7_token_created = 0;
}

static SID *alpc_s7_current_user_sid(void)
{
    ACCESS_TOKEN *t = (ACCESS_TOKEN *)task_current()->token;
    return t ? t->UserSid : (SID *)0;
}

static ALPC_PORT *alpc_s7_body(HANDLE h)
{
    HANDLE_TABLE_ENTRY *e = ObpLookupHandle(&task_current()->handle_table, h);
    return (e && e->object) ? (ALPC_PORT *)e->object : (ALPC_PORT *)0;
}

/* Run a full 2-thread handshake at the given QoS level and return the
 * server-side comm handle (client handle in s_conn_client_handle).
 * *out_server_conn receives the listener handle. INVALID on setup failure. */
static HANDLE alpc_s7_handshake(const char *leaf,
                                SECURITY_IMPERSONATION_LEVEL level,
                                const SID *required_sid,
                                const SECURITY_QUALITY_OF_SERVICE *client_qos,
                                HANDLE *out_server_conn)
{
    static char s7_path[96];
    ALPC_PORT_ATTRIBUTES attrs = {0};
    HANDLE server_conn = INVALID_HANDLE_VALUE;
    HANDLE server_comm = INVALID_HANDLE_VALUE;
    NTSTATUS st;
    int tid;

    *out_server_conn = INVALID_HANDLE_VALUE;
    attrs.SecurityQos.Length            = (uint32_t)sizeof(attrs.SecurityQos);
    attrs.SecurityQos.ImpersonationLevel = level;

    st = AlpcCreatePort(&task_current()->handle_table, leaf, &attrs, &server_conn);
    if (st != STATUS_SUCCESS)
        return INVALID_HANDLE_VALUE;

    snprintf(s7_path, sizeof(s7_path), "\\RPC Control\\%s", leaf);
    s_conn_target_name   = s7_path;
    s_conn_required_sid  = required_sid;
    s_conn_client_qos    = client_qos;
    s_conn_client_handle = INVALID_HANDLE_VALUE;
    s_conn_client_status = STATUS_INVALID_PARAMETER;
    s_conn_worker_done   = 0;

    tid = kthread_create(alpc_connect_worker, (void *)0, 0);
    if (tid < 0) {
        NtClose(&task_current()->handle_table, server_conn);
        test_alpc_cleanup_named(leaf);
        return INVALID_HANDLE_VALUE;
    }
    st = AlpcAcceptConnectPort(&task_current()->handle_table, server_conn,
                               /* accept */ 1, /* timeout */ 5000, &server_comm);
    thread_join((uint32_t)tid);
    if (st != STATUS_SUCCESS) {
        NtClose(&task_current()->handle_table, server_conn);
        test_alpc_cleanup_named(leaf);
        return INVALID_HANDLE_VALUE;
    }
    *out_server_conn = server_conn;
    return server_comm;
}

static void alpc_s7_teardown(const char *leaf, HANDLE server_conn,
                             HANDLE server_comm)
{
    if (server_comm != INVALID_HANDLE_VALUE) {
        (void)AlpcDisconnectPort(&task_current()->handle_table, server_comm);
        NtClose(&task_current()->handle_table, server_comm);
    }
    if (s_conn_client_handle != INVALID_HANDLE_VALUE)
        NtClose(&task_current()->handle_table, s_conn_client_handle);
    if (server_conn != INVALID_HANDLE_VALUE)
        NtClose(&task_current()->handle_table, server_conn);
    test_alpc_cleanup_named(leaf);
}

/* AlpcCreatePort snapshots the creator's primary-token UserSid into OwnerSid. */
static void test_alpc_owner_sid_snapshot(void)
{
    alpc_s7_ensure_token();
    SID *my_sid = alpc_s7_current_user_sid();
    if (!my_sid) {
        alpc_s7_restore_token();
        TEST_SKIP("no primary token to snapshot");
        return;
    }
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "OwnerSidP", (ALPC_PORT_ATTRIBUTES *)0, &h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "create OwnerSidP");
    if (h != INVALID_HANDLE_VALUE) {
        ALPC_PORT *p = alpc_s7_body(h);
        TEST_ASSERT_NOT_NULL(p ? (void *)p->OwnerSid : (void *)0,
                             "OwnerSid snapshot non-NULL");
        if (p && p->OwnerSid)
            TEST_ASSERT(RtlEqualSid(p->OwnerSid, my_sid) == 1,
                        "OwnerSid equals creator UserSid");
        NtClose(&task_current()->handle_table, h);
    }
    test_alpc_cleanup_named("OwnerSidP");
    alpc_s7_restore_token();
}

/* A connect carrying a RequiredServerSid that does not match the listener's
 * OwnerSid is refused with STATUS_SERVER_SID_MISMATCH (single-threaded: the
 * check runs before the request is queued). */
static void test_alpc_required_sid_mismatch(void)
{
    alpc_s7_ensure_token();
    HANDLE srv = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "SidMismatch", (ALPC_PORT_ATTRIBUTES *)0, &srv);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "create SidMismatch");
    if (srv != INVALID_HANDLE_VALUE) {
        /* A well-formed SID that cannot equal the creator's UserSid:
         * S-1-5-9999 (a bogus domain RID). */
        struct { uint8_t rev, cnt, auth[6]; uint32_t sub[1]; } bogus = {
            SID_REVISION, 1, {0,0,0,0,0,5}, {9999u}
        };
        HANDLE h = INVALID_HANDLE_VALUE;
        st = AlpcConnectPort(&task_current()->handle_table,
                             "\\RPC Control\\SidMismatch", 100,
                             (const SID *)&bogus,
                             (const SECURITY_QUALITY_OF_SERVICE *)0, &h);
        TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SERVER_SID_MISMATCH,
                       "wrong RequiredServerSid => SERVER_SID_MISMATCH");
        TEST_ASSERT_EQ(h, INVALID_HANDLE_VALUE, "no client handle on mismatch");
        NtClose(&task_current()->handle_table, srv);
    }
    test_alpc_cleanup_named("SidMismatch");
    alpc_s7_restore_token();
}

/* A connect carrying the matching RequiredServerSid completes the handshake. */
static void test_alpc_required_sid_match(void)
{
    alpc_s7_ensure_token();
    SID *my_sid = alpc_s7_current_user_sid();
    if (!my_sid) {
        alpc_s7_restore_token();
        TEST_SKIP("no primary token for SID match");
        return;
    }
    HANDLE srv = INVALID_HANDLE_VALUE;
    HANDLE comm = alpc_s7_handshake("SidMatch", SecurityAnonymous, my_sid,
                                    (const SECURITY_QUALITY_OF_SERVICE *)0, &srv);
    TEST_ASSERT_NEQ(comm, INVALID_HANDLE_VALUE, "matching SID => handshake OK");
    TEST_ASSERT_EQ((uint32_t)s_conn_client_status, (uint32_t)STATUS_SUCCESS,
                   "client connect SUCCESS with matching SID");
    alpc_s7_teardown("SidMatch", srv, comm);
    alpc_s7_restore_token();
}

/* Capture at accept + full impersonation lifecycle with exact refcounts. */
static void test_alpc_capture_and_impersonate(void)
{
    alpc_s7_ensure_token();
    if (!task_current()->token) {
        alpc_s7_restore_token();
        TEST_SKIP("no primary token to capture");
        return;
    }
    HANDLE srv = INVALID_HANDLE_VALUE;
    HANDLE comm = alpc_s7_handshake("ImpSrv", SecurityImpersonation,
                                    (const SID *)0,
                                    (const SECURITY_QUALITY_OF_SERVICE *)0, &srv);
    TEST_ASSERT_NEQ(comm, INVALID_HANDLE_VALUE, "impersonation handshake OK");
    if (comm != INVALID_HANDLE_VALUE) {
        ALPC_PORT *sp = alpc_s7_body(comm);
        TEST_ASSERT_NOT_NULL(sp ? (void *)sp->ClientToken : (void *)0,
                             "ClientToken captured at accept");
        if (sp && sp->ClientToken) {
            ACCESS_TOKEN *ct = sp->ClientToken;
            TEST_ASSERT_EQ((uint32_t)ct->ImpersonationLevel,
                           (uint32_t)SecurityImpersonation,
                           "captured token stamped at negotiated level");
            OBJECT_HEADER *th = OB_HEADER_FROM_BODY(ct);
            TEST_ASSERT_EQ(atomic_read(&th->ref_count), 1,
                           "port owns exactly one token ref");
            NTSTATUS ist = AlpcImpersonateClientOfPort(sp, SSDT_KERNEL_MODE);
            TEST_ASSERT_EQ((uint32_t)ist, (uint32_t)STATUS_SUCCESS,
                           "impersonate SUCCESS");
            TEST_ASSERT_EQ((uint64_t)(uintptr_t)thread_current()->impersonation_token,
                           (uint64_t)(uintptr_t)ct,
                           "thread now impersonating captured token");
            TEST_ASSERT_EQ(atomic_read(&th->ref_count), 2,
                           "thread took its own token ref");
            RevertToSelf();
            TEST_ASSERT_EQ((uint64_t)(uintptr_t)thread_current()->impersonation_token,
                           0ull, "RevertToSelf cleared impersonation");
            TEST_ASSERT_EQ(atomic_read(&th->ref_count), 1,
                           "revert dropped the thread ref");
        }
    }
    alpc_s7_teardown("ImpSrv", srv, comm);
    alpc_s7_restore_token();
}

/* An identification-only captured token cannot be used to ACT as the client. */
static void test_alpc_impersonate_identification_rejected(void)
{
    alpc_s7_ensure_token();
    if (!task_current()->token) {
        alpc_s7_restore_token();
        TEST_SKIP("no primary token to capture");
        return;
    }
    HANDLE srv = INVALID_HANDLE_VALUE;
    HANDLE comm = alpc_s7_handshake("IdSrv", SecurityIdentification,
                                    (const SID *)0,
                                    (const SECURITY_QUALITY_OF_SERVICE *)0, &srv);
    if (comm != INVALID_HANDLE_VALUE) {
        ALPC_PORT *sp = alpc_s7_body(comm);
        NTSTATUS ist = AlpcImpersonateClientOfPort(sp, SSDT_KERNEL_MODE);
        TEST_ASSERT_EQ((uint32_t)ist, (uint32_t)STATUS_BAD_IMPERSONATION_LEVEL,
                       "identification-only => BAD_IMPERSONATION_LEVEL");
    }
    alpc_s7_teardown("IdSrv", srv, comm);
    alpc_s7_restore_token();
}

/* A comm port that captured nothing (QoS below Identification) has no token. */
static void test_alpc_impersonate_no_token(void)
{
    HANDLE srv = INVALID_HANDLE_VALUE;
    HANDLE comm = alpc_s7_handshake("AnonSrv", SecurityAnonymous,
                                    (const SID *)0,
                                    (const SECURITY_QUALITY_OF_SERVICE *)0, &srv);
    if (comm != INVALID_HANDLE_VALUE) {
        ALPC_PORT *sp = alpc_s7_body(comm);
        TEST_ASSERT((sp ? sp->ClientToken : (ACCESS_TOKEN *)0) == (ACCESS_TOKEN *)0,
                    "no capture below SecurityIdentification");
        NTSTATUS ist = AlpcImpersonateClientOfPort(sp, SSDT_KERNEL_MODE);
        TEST_ASSERT_EQ((uint32_t)ist, (uint32_t)STATUS_NO_TOKEN,
                       "no captured token => STATUS_NO_TOKEN");
    }
    alpc_s7_teardown("AnonSrv", srv, comm);
}

/* Impersonation requires a server COMMUNICATION port, not a connection port. */
static void test_alpc_impersonate_wrong_port_type(void)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcCreatePort(&task_current()->handle_table,
                                 "WrongType", (ALPC_PORT_ATTRIBUTES *)0, &h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_SUCCESS, "create WrongType");
    if (h != INVALID_HANDLE_VALUE) {
        ALPC_PORT *p = alpc_s7_body(h);   /* AlpcServerConnectionPort */
        NTSTATUS ist = AlpcImpersonateClientOfPort(p, SSDT_KERNEL_MODE);
        TEST_ASSERT_EQ((uint32_t)ist, (uint32_t)STATUS_INVALID_PORT_HANDLE,
                       "connection port => INVALID_PORT_HANDLE");
        NtClose(&task_current()->handle_table, h);
    }
    test_alpc_cleanup_named("WrongType");
}

/* Repeated impersonation of the same token is ref-safe (no leak, no UAF). */
static void test_alpc_impersonate_repeated(void)
{
    alpc_s7_ensure_token();
    if (!task_current()->token) {
        alpc_s7_restore_token();
        TEST_SKIP("no primary token to capture");
        return;
    }
    HANDLE srv = INVALID_HANDLE_VALUE;
    HANDLE comm = alpc_s7_handshake("RepSrv", SecurityImpersonation,
                                    (const SID *)0,
                                    (const SECURITY_QUALITY_OF_SERVICE *)0, &srv);
    if (comm != INVALID_HANDLE_VALUE) {
        ALPC_PORT *sp = alpc_s7_body(comm);
        if (sp && sp->ClientToken) {
            OBJECT_HEADER *th = OB_HEADER_FROM_BODY(sp->ClientToken);
            NTSTATUS a = AlpcImpersonateClientOfPort(sp, SSDT_KERNEL_MODE);
            NTSTATUS b = AlpcImpersonateClientOfPort(sp, SSDT_KERNEL_MODE);
            TEST_ASSERT_EQ((uint32_t)a, (uint32_t)STATUS_SUCCESS, "impersonate #1");
            TEST_ASSERT_EQ((uint32_t)b, (uint32_t)STATUS_SUCCESS, "impersonate #2");
            /* One port ref + one thread ref -- the displaced #1 ref was
             * dropped when #2 installed the same token again. */
            TEST_ASSERT_EQ(atomic_read(&th->ref_count), 2,
                           "repeat impersonation keeps refcount at 2");
            RevertToSelf();
            TEST_ASSERT_EQ(atomic_read(&th->ref_count), 1,
                           "revert leaves only the port ref");
        }
    }
    alpc_s7_teardown("RepSrv", srv, comm);
    alpc_s7_restore_token();
}

/* A UserMode server impersonating a client of the SAME identity is allowed
 * without SeImpersonatePrivilege (self-impersonation); the captured token's
 * UserSid matches the server's effective UserSid. */
static void test_alpc_impersonate_user_mode_same_identity(void)
{
    alpc_s7_ensure_token();
    if (!task_current()->token) {
        alpc_s7_restore_token();
        TEST_SKIP("no primary token to capture");
        return;
    }
    HANDLE srv = INVALID_HANDLE_VALUE;
    HANDLE comm = alpc_s7_handshake("SameIdSrv", SecurityImpersonation,
                                    (const SID *)0,
                                    (const SECURITY_QUALITY_OF_SERVICE *)0, &srv);
    if (comm != INVALID_HANDLE_VALUE) {
        ALPC_PORT *sp = alpc_s7_body(comm);
        if (sp && sp->ClientToken) {
            NTSTATUS ist = AlpcImpersonateClientOfPort(sp, SSDT_USER_MODE);
            TEST_ASSERT_EQ((uint32_t)ist, (uint32_t)STATUS_SUCCESS,
                           "UserMode self-identity impersonation allowed");
            RevertToSelf();
        }
    }
    alpc_s7_teardown("SameIdSrv", srv, comm);
    alpc_s7_restore_token();
}

/* A matching UserSid does NOT authorize impersonation on its own: under the
 * split-token model one user owns both a filtered Medium-IL token and an
 * elevated High-IL one. A Medium-IL server (SeCreateUserToken(admin=0) carries
 * ChangeNotify/Shutdown/Undock only -- no SeImpersonatePrivilege) must be
 * REFUSED when the captured client conveys authority it does not hold, even
 * though both tokens share a UserSid. The non-amplifying case must still pass,
 * so this covers both directions of the gate. */
static void test_alpc_impersonate_user_mode_no_amplification(void)
{
    struct task  *cur = task_current();
    ACCESS_TOKEN *prev = (ACCESS_TOKEN *)cur->token;
    ACCESS_TOKEN *medium = SeCreateUserToken(SeBuiltinUsersSid, /* admin */ 0);

    if (!medium) {
        TEST_SKIP("could not create a Medium-IL server token");
        return;
    }
    cur->token = medium;

    HANDLE srv = INVALID_HANDLE_VALUE;
    HANDLE comm = alpc_s7_handshake("NoAmpSrv", SecurityImpersonation,
                                    (const SID *)0,
                                    (const SECURITY_QUALITY_OF_SERVICE *)0, &srv);
    ALPC_PORT *sp = (comm != INVALID_HANDLE_VALUE) ? alpc_s7_body(comm)
                                                   : (ALPC_PORT *)0;

    /* Assert every prerequisite: the runner counts a suite green when it
     * records no FAILURE, so a setup that quietly fell through would let this
     * named security test report pass while exercising no vector at all. */
    TEST_ASSERT(comm != INVALID_HANDLE_VALUE, "no-amp: handshake established");
    TEST_ASSERT(sp != (ALPC_PORT *)0, "no-amp: server comm port body resolved");
    if (comm != INVALID_HANDLE_VALUE && sp) {
        ACCESS_TOKEN *ct = sp->ClientToken;
        NTSTATUS ist;

        TEST_ASSERT(ct != (ACCESS_TOKEN *)0, "no-amp: client token captured");
        if (ct) {
            /* Baseline: the captured client is a duplicate of the server's own
             * Medium token -- same principal, same IL, same authority, neither
             * elevated. This amplifies nothing, so no privilege is required. */
            ist = AlpcImpersonateClientOfPort(sp, SSDT_USER_MODE);
            TEST_ASSERT_EQ((uint32_t)ist, (uint32_t)STATUS_SUCCESS,
                           "same-SID non-amplifying self-impersonation allowed");
            if (ist == STATUS_SUCCESS)
                RevertToSelf();

            /* Vector 1 -- integrity amplification: same UserSid, client at High
             * IL against a Medium-IL server. */
            ct->IntegrityLevelSid = (SID *)SeILHigh;
            ist = AlpcImpersonateClientOfPort(sp, SSDT_USER_MODE);
            TEST_ASSERT_EQ((uint32_t)ist, (uint32_t)STATUS_PRIVILEGE_NOT_HELD,
                           "same-SID higher-IL client refused without SeImpersonate");
            if (ist == STATUS_SUCCESS)
                RevertToSelf();

            /* Vector 2 -- elevation amplification: IL back to parity, but the
             * client is an elevated (split-token Full) principal. */
            ct->IntegrityLevelSid = (SID *)SeILMedium;
            ct->IsElevated        = 1;
            ist = AlpcImpersonateClientOfPort(sp, SSDT_USER_MODE);
            TEST_ASSERT_EQ((uint32_t)ist, (uint32_t)STATUS_PRIVILEGE_NOT_HELD,
                           "same-SID elevated client refused without SeImpersonate");
            if (ist == STATUS_SUCCESS)
                RevertToSelf();

            /* Vector 3 -- privilege amplification: UserSid, IL and elevation
             * all match, but the client holds a privilege the server does not.
             * UserSid parity alone would wave this through. */
            ct->IsElevated = 0;
            if (ct->PrivilegeCount < TOKEN_MAX_PRIVS) {
                ct->Privileges[ct->PrivilegeCount].Luid = SeDebugPrivilege;
                ct->Privileges[ct->PrivilegeCount].Attributes =
                    SE_PRIVILEGE_ENABLED;
                ct->PrivilegeCount++;
                ist = AlpcImpersonateClientOfPort(sp, SSDT_USER_MODE);
                TEST_ASSERT_EQ((uint32_t)ist, (uint32_t)STATUS_PRIVILEGE_NOT_HELD,
                               "same-SID client with extra privilege refused");
                if (ist == STATUS_SUCCESS)
                    RevertToSelf();
                ct->PrivilegeCount--;
            }

            /* Vector 4 -- group amplification: an enabled group the server
             * does not hold is authority the server cannot borrow. */
            if (ct->GroupCount < TOKEN_MAX_GROUPS) {
                ct->Groups[ct->GroupCount].Sid =
                    (SID *)SeBuiltinAdministratorsSid;
                ct->Groups[ct->GroupCount].Attributes = SE_GROUP_ENABLED;
                ct->GroupCount++;
                ist = AlpcImpersonateClientOfPort(sp, SSDT_USER_MODE);
                TEST_ASSERT_EQ((uint32_t)ist, (uint32_t)STATUS_PRIVILEGE_NOT_HELD,
                               "same-SID client with extra enabled group refused");
                if (ist == STATUS_SUCCESS)
                    RevertToSelf();
                ct->GroupCount--;
            }

            /* Vector 5 -- enabled client vs DISABLED server, same SID. The
             * server cannot enable a disabled group on the token it is
             * impersonating (NtOpenThreadToken opens the PROCESS primary
             * token), so a disabled server group is NOT authority it holds and
             * must not authorize an enabled client group. */
            if (medium->GroupCount < TOKEN_MAX_GROUPS &&
                ct->GroupCount < TOKEN_MAX_GROUPS) {
                medium->Groups[medium->GroupCount].Sid =
                    (SID *)SeBuiltinAdministratorsSid;
                medium->Groups[medium->GroupCount].Attributes = 0; /* disabled */
                medium->GroupCount++;
                ct->Groups[ct->GroupCount].Sid =
                    (SID *)SeBuiltinAdministratorsSid;
                ct->Groups[ct->GroupCount].Attributes = SE_GROUP_ENABLED;
                ct->GroupCount++;
                ist = AlpcImpersonateClientOfPort(sp, SSDT_USER_MODE);
                TEST_ASSERT_EQ((uint32_t)ist, (uint32_t)STATUS_PRIVILEGE_NOT_HELD,
                               "enabled client group vs disabled server group refused");
                if (ist == STATUS_SUCCESS)
                    RevertToSelf();

                /* Vector 6 -- DISABLED client vs present server, same SID:
                 * conveys nothing the server lacks, so it must be ALLOWED
                 * (this is what keeps identical-token self-impersonation
                 * working when the token carries disabled groups). */
                ct->Groups[ct->GroupCount - 1].Attributes = 0;   /* disabled */
                ist = AlpcImpersonateClientOfPort(sp, SSDT_USER_MODE);
                TEST_ASSERT_EQ((uint32_t)ist, (uint32_t)STATUS_SUCCESS,
                               "disabled client group vs present server allowed");
                if (ist == STATUS_SUCCESS)
                    RevertToSelf();
                ct->GroupCount--;
                medium->GroupCount--;
            }

            /* Vector 7 -- dropped denial: a group the SERVER carries deny-only
             * is negative authority. A client that does not carry it deny-only
             * escapes a denial the server is subject to. Walking only the
             * client's groups cannot see this. */
            if (medium->GroupCount < TOKEN_MAX_GROUPS) {
                medium->Groups[medium->GroupCount].Sid =
                    (SID *)SeBuiltinAdministratorsSid;
                medium->Groups[medium->GroupCount].Attributes =
                    SE_GROUP_USE_FOR_DENY_ONLY;
                medium->GroupCount++;   /* client dup does NOT carry it */
                ist = AlpcImpersonateClientOfPort(sp, SSDT_USER_MODE);
                TEST_ASSERT_EQ((uint32_t)ist, (uint32_t)STATUS_PRIVILEGE_NOT_HELD,
                               "client dropping a server deny-only group refused");
                if (ist == STATUS_SUCCESS)
                    RevertToSelf();
                medium->GroupCount--;
            }

            /* Vector 8 -- restricted token: equal RestrictedSidCount cannot
             * prove equal restriction, so any restricted token fails closed. */
            ct->Flags |= TOKEN_IS_RESTRICTED;
            ist = AlpcImpersonateClientOfPort(sp, SSDT_USER_MODE);
            TEST_ASSERT_EQ((uint32_t)ist, (uint32_t)STATUS_PRIVILEGE_NOT_HELD,
                           "restricted token fails closed");
            if (ist == STATUS_SUCCESS)
                RevertToSelf();
            ct->Flags &= ~(uint32_t)TOKEN_IS_RESTRICTED;

            /* Vector 9 -- unprovable identity: a malformed IL label must fail
             * closed rather than default to Medium and slip through. */
            ct->IntegrityLevelSid = (SID *)0;
            ist = AlpcImpersonateClientOfPort(sp, SSDT_USER_MODE);
            TEST_ASSERT_EQ((uint32_t)ist, (uint32_t)STATUS_PRIVILEGE_NOT_HELD,
                           "unprovable client IL fails closed");
            if (ist == STATUS_SUCCESS)
                RevertToSelf();

            /* Restore so the port's teardown dereferences a well-formed token. */
            ct->IntegrityLevelSid = (SID *)SeILMedium;
        }
    }
    alpc_s7_teardown("NoAmpSrv", srv, comm);

    cur->token = prev;
    ObDereferenceObject(medium);
}

/* The CLIENT's SecurityQos is a LIMIT, not a suggestion: an impersonation-hungry
 * listener must not be able to raise a client's exposure. A client connecting at
 * SecurityIdentification to a SecurityImpersonation listener must yield a token
 * stamped Identification -- which the impersonation gate then refuses. */
static void test_alpc_client_qos_caps_listener(void)
{
    SECURITY_QUALITY_OF_SERVICE cq = {0};
    HANDLE srv = INVALID_HANDLE_VALUE;

    alpc_s7_ensure_token();
    if (!task_current()->token) {
        alpc_s7_restore_token();
        TEST_SKIP("no primary token to capture");
        return;
    }

    cq.Length              = (uint32_t)sizeof(cq);
    cq.ImpersonationLevel  = SecurityIdentification;   /* client's ceiling */

    /* Listener asks for the maximum; the client asks for less. */
    HANDLE comm = alpc_s7_handshake("QosCapSrv", SecurityImpersonation,
                                    (const SID *)0, &cq, &srv);
    TEST_ASSERT_NEQ(comm, INVALID_HANDLE_VALUE, "qos-cap: handshake OK");
    if (comm != INVALID_HANDLE_VALUE) {
        ALPC_PORT *sp = alpc_s7_body(comm);
        TEST_ASSERT(sp != (ALPC_PORT *)0, "qos-cap: port body resolved");
        if (sp && sp->ClientToken) {
            /* Negotiated down to the client's ceiling, NOT the listener's. */
            TEST_ASSERT_EQ((uint32_t)sp->ClientToken->ImpersonationLevel,
                           (uint32_t)SecurityIdentification,
                           "client QoS caps the listener's requested level");
            /* And an identification-only token cannot be acted as. */
            NTSTATUS ist = AlpcImpersonateClientOfPort(sp, SSDT_KERNEL_MODE);
            TEST_ASSERT_EQ((uint32_t)ist,
                           (uint32_t)STATUS_BAD_IMPERSONATION_LEVEL,
                           "listener cannot impersonate past the client's QoS");
            if (ist == STATUS_SUCCESS)
                RevertToSelf();
        }
    }
    alpc_s7_teardown("QosCapSrv", srv, comm);
    alpc_s7_restore_token();
}

/* ---- Registration ------------------------------------------------------ */

void test_register_alpc(void)
{
    test_suite_register_cat("alpc: PORT_MESSAGE layout",
                            test_alpc_port_message_layout, TEST_CAT_IPC);
    test_suite_register_cat("alpc: message type codes",
                            test_alpc_msg_types_unique, TEST_CAT_IPC);
    test_suite_register_cat("alpc: ALPC_PORTFLG_* non-overlap",
                            test_alpc_portflg_nonoverlap, TEST_CAT_IPC);
    test_suite_register_cat("alpc: ALPC_MSGFLG_* non-overlap",
                            test_alpc_msgflg_nonoverlap, TEST_CAT_IPC);
    test_suite_register_cat("alpc: SECURITY_QUALITY_OF_SERVICE",
                            test_alpc_sqos_layout, TEST_CAT_IPC);
    test_suite_register_cat("alpc: ALPC_PORT_ATTRIBUTES layout",
                            test_alpc_port_attributes_layout, TEST_CAT_IPC);
    test_suite_register_cat("alpc: ObpAlpcPortType registered",
                            test_alpc_port_type_registered, TEST_CAT_IPC);
    test_suite_register_cat("alpc: \\RPC Control directory",
                            test_alpc_rpc_control_directory, TEST_CAT_IPC);
    test_suite_register_cat("alpc: AlpcCreatePort unnamed",
                            test_alpc_create_unnamed_port, TEST_CAT_IPC);
    test_suite_register_cat("alpc: AlpcCreatePort named",
                            test_alpc_create_named_port, TEST_CAT_IPC);
    test_suite_register_cat("alpc: duplicate name rejected",
                            test_alpc_duplicate_name_rejected, TEST_CAT_IPC);
    test_suite_register_cat("alpc: NULL handle table rejected",
                            test_alpc_null_handle_table_rejected, TEST_CAT_IPC);
    test_suite_register_cat("alpc: embedded backslash rejected",
                            test_alpc_embedded_backslash_rejected, TEST_CAT_IPC);
    test_suite_register_cat("alpc: privileged SYSTEM_PROCESS rejected",
                            test_alpc_privileged_flag_rejected, TEST_CAT_IPC);
    test_suite_register_cat("alpc: reserved pad non-zero rejected",
                            test_alpc_reserved_pad_rejected, TEST_CAT_IPC);
    test_suite_register_cat("alpc: syscall full-path split",
                            test_alpc_syscall_full_path, TEST_CAT_IPC);
    test_suite_register_cat("alpc: syscall bad prefix rejected",
                            test_alpc_syscall_bad_prefix_rejected, TEST_CAT_IPC);
    /* connection state machine */
    test_suite_register_cat("alpc: connect to nonexistent",
                            test_alpc_connect_nonexistent, TEST_CAT_IPC);
    test_suite_register_cat("alpc: connect type mismatch",
                            test_alpc_connect_type_mismatch, TEST_CAT_IPC);
    test_suite_register_cat("alpc: accept handshake (2 threads)",
                            test_alpc_accept_handshake, TEST_CAT_IPC);
    test_suite_register_cat("alpc: reject handshake (2 threads)",
                            test_alpc_reject_handshake, TEST_CAT_IPC);
    test_suite_register_cat("alpc: disconnect queues PORT_CLOSED",
                            test_alpc_disconnect_queues_close_msg, TEST_CAT_IPC);
    test_suite_register_cat("alpc: connect timeout (no acceptor)",
                            test_alpc_connect_timeout, TEST_CAT_IPC);
    test_suite_register_cat("alpc: accept timeout (no connector)",
                            test_alpc_accept_timeout, TEST_CAT_IPC);
    test_suite_register_cat("alpc: disconnect unconnected noop",
                            test_alpc_disconnect_unconnected, TEST_CAT_IPC);
    /* send+wait+receive engine */
    test_suite_register_cat("alpc: datagram delivery (2 threads)",
                            test_alpc_datagram_delivery, TEST_CAT_IPC);
    test_suite_register_cat("alpc: sync request+reply round-trip",
                            test_alpc_sync_request_reply, TEST_CAT_IPC);
    test_suite_register_cat("alpc: reply unknown MessageId rejected",
                            test_alpc_reply_mismatch, TEST_CAT_IPC);
    test_suite_register_cat("alpc: send oversized => BUFFER_TOO_SMALL",
                            test_alpc_send_too_large, TEST_CAT_IPC);
    test_suite_register_cat("alpc: receive empty timeout=0 => TIMEOUT",
                            test_alpc_receive_empty_no_wait, TEST_CAT_IPC);
    test_suite_register_cat("alpc: disconnect wakes sync waiter",
                            test_alpc_sync_wait_disconnect, TEST_CAT_IPC);
    test_suite_register_cat("alpc: pool quota exceeded",
                            test_alpc_pool_quota_exceeded, TEST_CAT_IPC);
    test_suite_register_cat("alpc: receive FIFO + truncation",
                            test_alpc_receive_fifo_truncation, TEST_CAT_IPC);
    test_suite_register_cat("alpc: sync timeout + late reply mismatch",
                            test_alpc_sync_timeout_late_reply, TEST_CAT_IPC);
    test_suite_register_cat("alpc: disconnect-then-send => DISCONNECTED",
                            test_alpc_disconnect_then_send, TEST_CAT_IPC);
    test_suite_register_cat("alpc: PORT_CLOSED marker uncharged",
                            test_alpc_port_closed_marker_uncharged, TEST_CAT_IPC);
    test_suite_register_cat("ALPC: sender message quota round-trip",
                            test_alpc_message_quota_roundtrip, TEST_CAT_IPC);
    test_suite_register_cat("ALPC: message quota returned on port teardown",
                            test_alpc_message_quota_returned_on_teardown, TEST_CAT_IPC);
    test_suite_register_cat("ALPC: message quota refusal status",
                            test_alpc_message_quota_refusal_status, TEST_CAT_IPC);
    test_suite_register_cat("ALPC: a quota-refused send allocates nothing",
                            test_alpc_quota_refused_send_allocates_nothing,
                            TEST_CAT_IPC);
    test_suite_register_cat("ALPC: a failed entry allocation returns the charge",
                            test_alpc_quota_returned_when_entry_alloc_fails,
                            TEST_CAT_IPC);
    test_suite_register_cat("ALPC: AlpcAllocateMessage bad args",
                            test_alpc_allocate_message_bad_args, TEST_CAT_IPC);
    test_suite_register_cat("alpc: remote disconnect-then-send => DISCONNECTED",
                            test_alpc_remote_disconnect_then_send, TEST_CAT_IPC);
    test_suite_register_cat("alpc: sync request with no peer",
                            test_alpc_sync_no_peer, TEST_CAT_IPC);
    test_suite_register_cat("alpc: NtAlpcSendWaitReceivePort syscall validation",
                            test_alpc_syscall_validation, TEST_CAT_IPC);
    /* retrofits -- closes the ALPC deferred-test-gaps block via
     * the kernel test-harness primitives (kmalloc fault inject, race
     * barrier, PMM scratch helper, klog level demotion). */
    test_suite_register_cat("alpc: kmalloc-fail in pending alloc rolls back PoolUsageBytes",
                            test_alpc_kmalloc_fail_rollback, TEST_CAT_IPC);
    test_suite_register_cat("alpc: ReplyBodyCap clamps recv_buf_len > 65528",
                            test_alpc_reply_body_cap_clamped, TEST_CAT_IPC);
    test_suite_register_cat("alpc: two-port lock-order stress (race-barrier paired senders)",
                            test_alpc_two_port_lock_order_stress, TEST_CAT_IPC);
    /* async delivery + waitable port */
    test_suite_register_cat("alpc: ALPC_COMPLETION_LIST_ITEM layout",
                            test_alpc_completion_list_item_layout, TEST_CAT_IPC);
    test_suite_register_cat("alpc: waitable port signal/drain",
                            test_alpc_waitable_port_signal, TEST_CAT_IPC);
    test_suite_register_cat("alpc: non-waitable port rejects wait",
                            test_alpc_nonwaitable_rejects, TEST_CAT_IPC);
    test_suite_register_cat("alpc: associate completion port + notification",
                            test_alpc_associate_completion_port, TEST_CAT_IPC);
    test_suite_register_cat("alpc: associate with bad IOCP => INVALID_HANDLE",
                            test_alpc_associate_bad_iocp, TEST_CAT_IPC);
    test_suite_register_cat("alpc: SetInfo unknown class => NOT_IMPLEMENTED",
                            test_alpc_setinfo_unknown_class, TEST_CAT_IPC);
    /* Section 7: security context capture & impersonation */
    test_suite_register_cat("alpc: OwnerSid snapshot at create",
                            test_alpc_owner_sid_snapshot, TEST_CAT_IPC);
    test_suite_register_cat("alpc: RequiredServerSid mismatch",
                            test_alpc_required_sid_mismatch, TEST_CAT_IPC);
    test_suite_register_cat("alpc: RequiredServerSid match",
                            test_alpc_required_sid_match, TEST_CAT_IPC);
    test_suite_register_cat("alpc: capture + impersonate lifecycle",
                            test_alpc_capture_and_impersonate, TEST_CAT_IPC);
    test_suite_register_cat("alpc: impersonate identification rejected",
                            test_alpc_impersonate_identification_rejected,
                            TEST_CAT_IPC);
    test_suite_register_cat("alpc: impersonate no captured token",
                            test_alpc_impersonate_no_token, TEST_CAT_IPC);
    test_suite_register_cat("alpc: impersonate wrong port type",
                            test_alpc_impersonate_wrong_port_type, TEST_CAT_IPC);
    test_suite_register_cat("alpc: repeated impersonation ref-safe",
                            test_alpc_impersonate_repeated, TEST_CAT_IPC);
    test_suite_register_cat("alpc: UserMode same-identity impersonation",
                            test_alpc_impersonate_user_mode_same_identity,
                            TEST_CAT_IPC);
    test_suite_register_cat("alpc: client QoS caps listener level",
                            test_alpc_client_qos_caps_listener,
                            TEST_CAT_IPC);
    test_suite_register_cat("alpc: impersonate UserMode no-amplification gate",
                            test_alpc_impersonate_user_mode_no_amplification,
                            TEST_CAT_IPC);
}

#endif /* KERNEL_TESTS */
