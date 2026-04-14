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
}

#endif /* KERNEL_TESTS */
