/* ============================================================================
 * test_alpc.c -- ALPC ABI constants & layout tests + port-object tests
 *
 * Covers the static contract shipped in include/kernel/ipc/alpc.h:
 * message header size and per-field offsets (via static asserts
 * re-validated at runtime), message type code uniqueness and range,
 * maximum inline message length, and non-overlap of ALPC_PORTFLG_* and
 * ALPC_MSGFLG_* bit groups.
 *
 * §2 adds port-object tests: ObpAlpcPortType registration, \RPC Control
 * namespace entry, AlpcCreatePort success paths (named + unnamed), and
 * namespace lookup of a named port.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/ipc/alpc.h"
#include "kernel/ipc/alpc_port.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/sched/task.h"

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
    TEST_ASSERT_EQ(ALPC_MSG_TYPE_CONNECTION_REQUEST, 10u,
                   "highest ALPC_MSG_TYPE is 10");
}

/* ---- Max inline message length ----------------------------------------- */

static void test_alpc_max_message_length(void)
{
    TEST_ASSERT_EQ(ALPC_MAX_ALLOWED_MESSAGE_LENGTH, 65528u,
                   "ALPC_MAX_ALLOWED_MESSAGE_LENGTH == 65528");
}

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
    TEST_ASSERT_EQ(ALPC_PORTFLG_LPC_MODE,         0x00020000u,
                   "ALPC_PORTFLG_LPC_MODE == 0x20000");
    TEST_ASSERT_EQ(ALPC_PORTFLG_WAITABLE_PORT,    0x00040000u,
                   "ALPC_PORTFLG_WAITABLE_PORT == 0x40000");
    TEST_ASSERT_EQ(ALPC_PORTFLG_ALLOW_DUP_OBJECT, 0x00080000u,
                   "ALPC_PORTFLG_ALLOW_DUP_OBJECT == 0x80000");
    TEST_ASSERT_EQ(ALPC_PORTFLG_SYSTEM_PROCESS,   0x00100000u,
                   "ALPC_PORTFLG_SYSTEM_PROCESS == 0x100000");
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
    TEST_ASSERT_EQ(ALPC_MSGFLG_REPLY_MESSAGE,          0x1u,
                   "ALPC_MSGFLG_REPLY_MESSAGE == 0x1");
    TEST_ASSERT_EQ(ALPC_MSGFLG_LPC_MODE,               0x2u,
                   "ALPC_MSGFLG_LPC_MODE == 0x2");
    TEST_ASSERT_EQ(ALPC_MSGFLG_RELEASE_MESSAGE,        0x10u,
                   "ALPC_MSGFLG_RELEASE_MESSAGE == 0x10");
    TEST_ASSERT_EQ(ALPC_MSGFLG_SYNC_REQUEST,           0x20000u,
                   "ALPC_MSGFLG_SYNC_REQUEST == 0x20000");
    TEST_ASSERT_EQ(ALPC_MSGFLG_WAIT_USER_MODE,         0x100000u,
                   "ALPC_MSGFLG_WAIT_USER_MODE == 0x100000");
    TEST_ASSERT_EQ(ALPC_MSGFLG_WAIT_PENDING_CALLBACKS, 0x200000u,
                   "ALPC_MSGFLG_WAIT_PENDING_CALLBACKS == 0x200000");
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
    TEST_ASSERT_EQ(SECURITY_CONTEXT_TRACKING_STATIC, 0u,
                   "CONTEXT_TRACKING_STATIC == 0");
    TEST_ASSERT_EQ(SECURITY_CONTEXT_TRACKING_DYNAMIC, 1u,
                   "CONTEXT_TRACKING_DYNAMIC == 1");
}

/* ---- §2 Port object tests ---------------------------------------------- */

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
    /* ALPC_PORTFLG_SYSTEM_PROCESS is privileged; §7 gates it via
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
     * is findable at the original full path. This is the exact test the
     * §2 Codex review asked for. */
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
}

/* =========================================================================
 * §3 Connection state machine tests
 * =======================================================================*/

/* Simple pattern: static state set by the main thread, consumed by the
 * kthread worker. One test at a time uses these -- the test framework
 * runs suites sequentially on a single thread pool. */
static volatile HANDLE s_conn_client_handle;
static volatile NTSTATUS s_conn_client_status;
static volatile int s_conn_worker_done;
static const char *s_conn_target_name;

static void alpc_connect_worker(void *arg)
{
    (void)arg;
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcConnectPort(&task_current()->handle_table,
                                  s_conn_target_name,
                                  /* timeout_ms */ 5000, &h);
    s_conn_client_handle = h;
    s_conn_client_status = st;
    s_conn_worker_done   = 1;
}

static void test_alpc_connect_nonexistent(void)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = AlpcConnectPort(&task_current()->handle_table,
                                  "\\RPC Control\\NoSuchPort", 100, &h);
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
                                  "\\RPC Control", 100, &h);
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
    s_conn_client_handle = INVALID_HANDLE_VALUE;
    s_conn_client_status = STATUS_INVALID_PARAMETER;
    s_conn_worker_done   = 0;

    int tid = kthread_create(alpc_connect_worker, (void *)0, 0);
    TEST_ASSERT(tid >= 0, "kthread_create(connect worker) succeeds");
    if (tid < 0) {
        NtClose(&task_current()->handle_table, server_conn);
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

    if (server_comm != INVALID_HANDLE_VALUE)
        NtClose(&task_current()->handle_table, server_comm);
    if (s_conn_client_handle != INVALID_HANDLE_VALUE)
        NtClose(&task_current()->handle_table, s_conn_client_handle);
    NtClose(&task_current()->handle_table, server_conn);
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
                         /* timeout_ms */ 50, &client_h);
    TEST_ASSERT_EQ((uint32_t)st, (uint32_t)STATUS_TIMEOUT,
                   "connect timeout => STATUS_TIMEOUT");
    TEST_ASSERT_EQ(client_h, INVALID_HANDLE_VALUE,
                   "timeout path must NOT publish a handle");

    NtClose(&task_current()->handle_table, server_conn);
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

/* ---- Registration ------------------------------------------------------ */

void test_register_alpc(void)
{
    test_suite_register_cat("alpc: PORT_MESSAGE layout",
                            test_alpc_port_message_layout, TEST_CAT_IPC);
    test_suite_register_cat("alpc: message type codes",
                            test_alpc_msg_types_unique, TEST_CAT_IPC);
    test_suite_register_cat("alpc: max message length",
                            test_alpc_max_message_length, TEST_CAT_IPC);
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
    /* §3 connection state machine */
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
}

#endif /* KERNEL_TESTS */
