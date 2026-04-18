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

/* =========================================================================
 * §4 Synchronous Send+Wait+Receive Engine tests
 *
 * Each test sets up a connected pair via the §3 handshake, then drives
 * AlpcSendWaitReceivePort on both endpoints. Worker threads run the
 * client side via kthread_create so the cooperative-yield scheduler
 * can interleave server and client.
 * =======================================================================*/

#include "kernel/sched/task.h"

/* Shared state for §4 worker threads. The framework runs suites
 * sequentially on a single thread pool, so a single set of statics is
 * safe. */
static volatile HANDLE   s_sync_client_handle;
static volatile HANDLE   s_sync_server_conn;
static const char       *s_sync_target_name;
static volatile NTSTATUS s_sync_send_status;
static volatile int      s_sync_worker_done;

/* §4 connect-worker (file-static; replaces inline GCC-nested-function
 * pattern that clang -- the project compiler -- does not support). */
static void s4_connect_worker(void *arg)
{
    (void)arg;
    HANDLE ch = INVALID_HANDLE_VALUE;
    (void)AlpcConnectPort(&task_current()->handle_table,
                          s_sync_target_name, /* timeout */ 5000, &ch);
    s_sync_client_handle = ch;
    s_sync_worker_done = 1;
}

static HANDLE alpc_setup_pair(const char *server_name, HANDLE *server_comm)
{
    /* Convenience: build a fully connected server/client pair using §3
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
    return client_h;
}

static void alpc_teardown_pair(HANDLE client_h, HANDLE server_comm)
{
    HANDLE_TABLE *ht = &task_current()->handle_table;
    if (client_h != INVALID_HANDLE_VALUE) NtClose(ht, client_h);
    if (server_comm != INVALID_HANDLE_VALUE) NtClose(ht, server_comm);
    if (s_sync_server_conn != INVALID_HANDLE_VALUE)
        NtClose(ht, s_sync_server_conn);
    s_sync_server_conn = INVALID_HANDLE_VALUE;
}

/* ---- §4.1 Datagram send delivers to peer's MessageQueue --------------- */

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

/* ---- §4.2 Sync request + reply round-trip ----------------------------- */

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

/* ---- §4.3 Reply with bogus MessageId ---------------------------------- */

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

/* ---- §4.4 Datagram exceeding MaxMessageLength ------------------------- */

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

/* ---- §4.5 Receive on empty queue with timeout=0 ----------------------- */

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

/* ---- §4.6 Disconnect wakes blocked sync waiter ------------------------ */

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

/* ---- §4.7 Pool quota exceeded ----------------------------------------- */

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
        NtClose(&task_current()->handle_table, server_conn);
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

    NtClose(&task_current()->handle_table, client_h);
    NtClose(&task_current()->handle_table, server_comm);
    NtClose(&task_current()->handle_table, server_conn);
}

/* ---- §4.8 Receive-only path: FIFO + truncation ------------------------ */

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

/* ---- §4.9 Sync timeout + late reply mismatch -------------------------- */

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

/* ---- §4.10 Disconnect-then-send returns DISCONNECTED ----------------- */

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

/* ---- §4.10a PORT_CLOSED marker is uncharged (regression) ------------- */

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

/* ---- §4.10b Remote disconnect then send => DISCONNECTED -------------- */

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

/* ---- §4.11 Sync request with no peer (server connection port) -------- */

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
}

/* ---- §4.12 NtAlpcSendWaitReceivePort syscall validation -------------- */

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
 * §5 Asynchronous Delivery & Completion List tests
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

/* ---- §5.3 Waitable port: NtWaitForSingleObject signalled on msg ------ */

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
        NtClose(&task_current()->handle_table, server_conn);
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

    NtClose(&task_current()->handle_table, client_h);
    NtClose(&task_current()->handle_table, server_comm);
    NtClose(&task_current()->handle_table, server_conn);
}

/* ---- §5.3 Non-waitable port rejected ---------------------------------- */

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
}

/* ---- §5.1/§5.2 NtAlpcSetInformation: AssociateCompletionPort --------- */

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
        NtClose(&task_current()->handle_table, server_conn);
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

    NtClose(&task_current()->handle_table, client_h);
    NtClose(&task_current()->handle_table, server_comm);
    NtClose(&task_current()->handle_table, server_conn);
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
    /* §4 send+wait+receive engine */
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
    test_suite_register_cat("alpc: remote disconnect-then-send => DISCONNECTED",
                            test_alpc_remote_disconnect_then_send, TEST_CAT_IPC);
    test_suite_register_cat("alpc: sync request with no peer",
                            test_alpc_sync_no_peer, TEST_CAT_IPC);
    test_suite_register_cat("alpc: NtAlpcSendWaitReceivePort syscall validation",
                            test_alpc_syscall_validation, TEST_CAT_IPC);
    /* §5 async delivery + waitable port */
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
}

#endif /* KERNEL_TESTS */
