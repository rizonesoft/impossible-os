/* ============================================================================
 * test_alpc.c -- ALPC ABI constants & layout tests
 *
 * Covers the static contract shipped in include/kernel/ipc/alpc.h:
 * message header size and per-field offsets (via static asserts
 * re-validated at runtime), message type code uniqueness and range,
 * maximum inline message length, and non-overlap of ALPC_PORTFLG_* and
 * ALPC_MSGFLG_* bit groups.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/ipc/alpc.h"

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
    TEST_ASSERT_EQ(__builtin_offsetof(PORT_MESSAGE, MessageId), 16,
                   "PORT_MESSAGE.MessageId at offset 16");
    TEST_ASSERT_EQ(__builtin_offsetof(PORT_MESSAGE, CallbackId), 24,
                   "PORT_MESSAGE.CallbackId at offset 24");
    TEST_ASSERT_EQ(sizeof(CLIENT_ID), 8, "CLIENT_ID sizeof == 8");
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
}

#endif /* KERNEL_TESTS */
