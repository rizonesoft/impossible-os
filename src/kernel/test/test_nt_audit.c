/* ============================================================================
 * test_nt_audit.c -- Syscall audit hook unit tests (register/unregister/
 * dispatch pre-block/post + pool limits)
 *
 * Exercises the pure kernel-side helpers in nt_audit.c without the syscall
 * marshalling path -- no live boot calls.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_audit.h"

/* Module-static observation state for the test routines. */
static uint32_t s_pre_calls;
static uint32_t s_post_calls;
static uint32_t s_last_service;
static NTSTATUS s_last_status;
static NTSTATUS s_pre_verdict;   /* what the pre routine returns */

static NTSTATUS test_audit_routine(uint32_t service_number, const uint64_t *args,
                                   void *context, uint32_t phase, NTSTATUS status)
{
    (void)args; (void)context;
    s_last_service = service_number;
    if (phase == AUDIT_PHASE_PRE) {
        s_pre_calls++;
        return s_pre_verdict;
    }
    s_post_calls++;
    s_last_status = status;
    return STATUS_SUCCESS;
}

static void reset_obs(void)
{
    s_pre_calls = s_post_calls = s_last_service = 0;
    s_last_status = STATUS_SUCCESS;
    s_pre_verdict = STATUS_SUCCESS;
    nt_audit_reset_for_test();
}

/* Register/unregister round-trip + count. */
static void test_nt_audit_register(void)
{
    reset_obs();
    TEST_ASSERT_EQ(nt_audit_hook_count(), 0, "no hooks initially");

    int32_t h = 0;
    NTSTATUS s = nt_audit_register(test_audit_routine, (void *)0x1234, AUDIT_BOTH, &h);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "register succeeds");
    TEST_ASSERT(h > 0, "handle is positive");
    TEST_ASSERT_EQ(nt_audit_hook_count(), 1, "count is 1 after register");

    s = nt_audit_unregister(h);
    TEST_ASSERT_EQ(s, STATUS_SUCCESS, "unregister succeeds");
    TEST_ASSERT_EQ(nt_audit_hook_count(), 0, "count is 0 after unregister");

    s = nt_audit_unregister(h);
    TEST_ASSERT_EQ(s, STATUS_INVALID_HANDLE, "double unregister is invalid handle");
    nt_audit_reset_for_test();
}

/* Bad inputs rejected. */
static void test_nt_audit_bad_input(void)
{
    reset_obs();
    int32_t h = 0;
    NTSTATUS s = nt_audit_register((SYSCALL_AUDIT_ROUTINE)0, (void *)0, AUDIT_PRE_CALL, &h);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER, "NULL routine rejected");

    s = nt_audit_register(test_audit_routine, (void *)0, 0, &h);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER, "zero flags rejected");

    s = nt_audit_register(test_audit_routine, (void *)0, 0xFF, &h);
    TEST_ASSERT_EQ(s, STATUS_INVALID_PARAMETER, "out-of-range flag bits rejected");

    s = nt_audit_unregister(0);
    TEST_ASSERT_EQ(s, STATUS_INVALID_HANDLE, "handle 0 is invalid");
    nt_audit_reset_for_test();
}

/* A pre-call hook returning non-SUCCESS blocks (dispatch_pre returns it). */
static void test_nt_audit_pre_block(void)
{
    reset_obs();
    int32_t h = 0;
    nt_audit_register(test_audit_routine, (void *)0, AUDIT_PRE_CALL, &h);

    uint64_t args[6] = { 1, 2, 3, 4, 5, 6 };
    nt_audit_session_t sess;

    s_pre_verdict = STATUS_SUCCESS;
    NTSTATUS r = nt_audit_begin(0x00DF, args, &sess);
    if (r == STATUS_SUCCESS)
        nt_audit_end(&sess, 0x00DF, args, STATUS_SUCCESS);
    TEST_ASSERT_EQ(r, STATUS_SUCCESS, "pre returning SUCCESS does not block");
    TEST_ASSERT_EQ(s_pre_calls, 1, "pre hook fired once");
    TEST_ASSERT_EQ(s_last_service, 0x00DFu, "service number passed through");

    s_pre_verdict = STATUS_ACCESS_DENIED;
    r = nt_audit_begin(0x00DF, args, &sess);
    if (r == STATUS_SUCCESS)
        nt_audit_end(&sess, 0x00DF, args, STATUS_SUCCESS);
    TEST_ASSERT_EQ(r, STATUS_ACCESS_DENIED, "pre returning ACCESS_DENIED blocks");
    nt_audit_reset_for_test();
}

/* A post-call hook sees the handler status. */
static void test_nt_audit_post(void)
{
    reset_obs();
    int32_t h = 0;
    nt_audit_register(test_audit_routine, (void *)0, AUDIT_POST_CALL, &h);

    uint64_t args[6] = { 0 };
    nt_audit_session_t sess;
    /* begin does NOT fire a post-only hook's PRE phase. */
    NTSTATUS r = nt_audit_begin(0x0100, args, &sess);
    TEST_ASSERT_EQ(r, STATUS_SUCCESS, "post-only hook does not affect pre");
    TEST_ASSERT_EQ(s_pre_calls, 0, "post-only hook did not fire on pre");

    nt_audit_end(&sess, 0x0100, args, STATUS_OBJECT_NAME_NOT_FOUND);
    TEST_ASSERT_EQ(s_post_calls, 1, "post hook fired");
    TEST_ASSERT_EQ(s_last_status, STATUS_OBJECT_NAME_NOT_FOUND, "post hook saw handler status");
    nt_audit_reset_for_test();
}

/* The fixed pool rejects the (MAX+1)th hook. */
static void test_nt_audit_pool_full(void)
{
    reset_obs();
    int32_t h = 0;
    for (uint32_t i = 0; i < NT_AUDIT_MAX_HOOKS; i++) {
        NTSTATUS s = nt_audit_register(test_audit_routine, (void *)0, AUDIT_PRE_CALL, &h);
        TEST_ASSERT_EQ(s, STATUS_SUCCESS, "fill register succeeds");
    }
    TEST_ASSERT_EQ(nt_audit_hook_count(), NT_AUDIT_MAX_HOOKS, "pool is full");

    NTSTATUS s = nt_audit_register(test_audit_routine, (void *)0, AUDIT_PRE_CALL, &h);
    TEST_ASSERT_EQ(s, STATUS_INSUFFICIENT_RESOURCES, "full pool rejects new hook");
    nt_audit_reset_for_test();
}

/* Recursion guard: a hook that re-enters the audit path must be invoked
 * exactly once (the nested dispatch returns without re-invoking). */
static uint32_t s_recursion_depth;

static NTSTATUS test_recursive_routine(uint32_t service_number, const uint64_t *args,
                                       void *context, uint32_t phase, NTSTATUS status)
{
    (void)context; (void)status;
    if (phase == AUDIT_PHASE_PRE) {
        s_recursion_depth++;
        if (s_recursion_depth < 8) {
            nt_audit_session_t nested;
            (void)nt_audit_begin(service_number, args, &nested);  /* guarded no-op */
            nt_audit_end(&nested, service_number, args, STATUS_SUCCESS);
        }
    }
    return STATUS_SUCCESS;
}

static void test_nt_audit_recursion_guard(void)
{
    reset_obs();
    s_recursion_depth = 0;
    int32_t h = 0;
    nt_audit_register(test_recursive_routine, (void *)0, AUDIT_PRE_CALL, &h);

    uint64_t args[6] = { 0 };
    nt_audit_session_t sess;
    if (nt_audit_begin(0x00DF, args, &sess) == STATUS_SUCCESS)
        nt_audit_end(&sess, 0x00DF, args, STATUS_SUCCESS);
    TEST_ASSERT_EQ(s_recursion_depth, 1, "recursion guard invokes the hook exactly once");
    nt_audit_reset_for_test();
}

/* Re-entry guard: a hook that calls the audit register/unregister API from
 * inside its callback must fail fast (STATUS_UNSUCCESSFUL), never deadlock on
 * s_audit_lock. */
static NTSTATUS s_reentry_status;

static NTSTATUS test_reentry_routine(uint32_t service_number, const uint64_t *args,
                                     void *context, uint32_t phase, NTSTATUS status)
{
    (void)service_number; (void)args; (void)status;
    if (phase == AUDIT_PHASE_PRE)
        s_reentry_status = nt_audit_unregister(*(int32_t *)context);
    return STATUS_SUCCESS;
}

static void test_nt_audit_reentry_guard(void)
{
    reset_obs();
    s_reentry_status = STATUS_SUCCESS;
    int32_t h = 0;
    nt_audit_register(test_reentry_routine, &h, AUDIT_PRE_CALL, &h);

    uint64_t args[6] = { 0 };
    nt_audit_session_t sess;
    if (nt_audit_begin(0x00DF, args, &sess) == STATUS_SUCCESS)
        nt_audit_end(&sess, 0x00DF, args, STATUS_SUCCESS);
    TEST_ASSERT_EQ(s_reentry_status, STATUS_UNSUCCESSFUL,
                   "audit-API re-entry from a hook fails without deadlock");
    nt_audit_reset_for_test();
}

void test_register_nt_audit(void)
{
    test_suite_register_cat("NT: audit register/unregister", test_nt_audit_register, TEST_CAT_ABI);
    test_suite_register_cat("NT: audit bad input", test_nt_audit_bad_input, TEST_CAT_ABI);
    test_suite_register_cat("NT: audit pre-call block", test_nt_audit_pre_block, TEST_CAT_ABI);
    test_suite_register_cat("NT: audit post-call status", test_nt_audit_post, TEST_CAT_ABI);
    test_suite_register_cat("NT: audit pool full", test_nt_audit_pool_full, TEST_CAT_ABI);
    test_suite_register_cat("NT: audit recursion guard", test_nt_audit_recursion_guard, TEST_CAT_ABI);
    test_suite_register_cat("NT: audit API re-entry guard", test_nt_audit_reentry_guard, TEST_CAT_ABI);
}
