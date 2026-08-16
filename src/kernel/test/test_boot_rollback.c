/* ============================================================================
 * test_boot_rollback.c -- unit tests for the anti-rollback validator
 * and the Phase-3 raise-counter decision helper.
 *
 * Covers:
 *   - NULL info rejection
 *   - happy path (first-ever boot, required=0, any shipped)
 *   - unknown flag bit rejected
 *   - version > BOOT_SECURITY_VERSION_MAX rejected
 *   - raise decision: opt-out (never raise), opt-in same version
 *     (skip), opt-in newer version (raise to shipped value)
 *
 * Matches the 4 fixture scenarios the TODO checkpoint calls for.
 * The actual NVRAM write path (uefi_set_variable) is NOT tested here
 * -- it requires live UEFI RT which the test harness cannot reach.
 * Host smoke test covers the round trip manually on QEMU OVMF.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/test/klog_suppress.h"
#include "kernel/boot_info.h"
#include "kernel/uefi_runtime.h"
#include "kernel/sched/workqueue.h"

static struct boot_info s_rb_buf;

static void rb_zero(void)
{
    uint64_t *p = (uint64_t *)&s_rb_buf;
    uint32_t i;
    for (i = 0u; i < sizeof(s_rb_buf) / sizeof(uint64_t); i++)
        p[i] = 0u;
}

static void test_boot_rollback_null_info(void)
{
    TEST_KLOG_SUPPRESS("boot");
    enum boot_rollback_error err = BOOT_ROLLBACK_ERR_OK;
    boot_result_t r = boot_rollback_validate((struct boot_info *)0, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                    "NULL info -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_ROLLBACK_ERR_NULL_INFO, "err=NULL_INFO");
}

static void test_boot_rollback_first_ever_boot(void)
{
    TEST_KLOG_SUPPRESS("boot");  /* LOG_INFO summary */
    rb_zero();
    /* First-ever boot: NVRAM variable absent -> required=0, shipped=N
     * passes because shipped >= required for any N. */
    s_rb_buf.flags                       = 0u;
    s_rb_buf.os_loader_security_version  = 5u;
    s_rb_buf.required_security_version   = 0u;

    enum boot_rollback_error err = BOOT_ROLLBACK_ERR_OK;
    boot_result_t r = boot_rollback_validate(&s_rb_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,                "first-ever boot -> OK");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_ROLLBACK_ERR_OK,   "err=OK");
}

static void test_boot_rollback_equal_versions(void)
{
    TEST_KLOG_SUPPRESS("boot");
    rb_zero();
    /* shipped == required (5==5): accepted; no raise. */
    s_rb_buf.os_loader_security_version  = 5u;
    s_rb_buf.required_security_version   = 5u;

    enum boot_rollback_error err = BOOT_ROLLBACK_ERR_OK;
    boot_result_t r = boot_rollback_validate(&s_rb_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK, "equal versions -> OK");

    uint32_t new_value = 0xBEEF;
    int raise = boot_rollback_should_raise(&s_rb_buf, /*opt_in*/ 1, &new_value);
    TEST_ASSERT_EQ((uint64_t)raise, 0u, "equal versions -> no raise");
    /* new_value must not be written when raise returns 0 */
    TEST_ASSERT_EQ((uint64_t)new_value, 0xBEEFu, "new_value untouched on no-raise");
}

static void test_boot_rollback_unknown_flag_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    rb_zero();
    s_rb_buf.flags                       = (1u << 31);
    s_rb_buf.os_loader_security_version  = 1u;
    s_rb_buf.required_security_version   = 0u;

    enum boot_rollback_error err = BOOT_ROLLBACK_ERR_OK;
    boot_result_t r = boot_rollback_validate(&s_rb_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                     "unknown flag -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_ROLLBACK_ERR_UNKNOWN_FLAG, "err=UNKNOWN_FLAG");
}

static void test_boot_rollback_version_oor_rejected(void)
{
    TEST_KLOG_SUPPRESS("boot");
    rb_zero();
    s_rb_buf.os_loader_security_version  = BOOT_SECURITY_VERSION_MAX + 1u;
    s_rb_buf.required_security_version   = 0u;

    enum boot_rollback_error err = BOOT_ROLLBACK_ERR_OK;
    boot_result_t r = boot_rollback_validate(&s_rb_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                     "version OOR -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_ROLLBACK_ERR_VERSION_OOR,  "err=VERSION_OOR");
}

static void test_boot_rollback_reserved_pad_nonzero_rejected(void)
{
    /* Codex 2026-04-30 finding: _rollback_pad must be zero. A producer
     * publishing nonzero policy bits in the reserved field is forging
     * future-policy data the validator does not interpret. */
    TEST_KLOG_SUPPRESS("boot");
    rb_zero();
    s_rb_buf.os_loader_security_version  = 1u;
    s_rb_buf.required_security_version   = 0u;
    s_rb_buf._rollback_pad               = 0xCAFEBABEu;

    enum boot_rollback_error err = BOOT_ROLLBACK_ERR_OK;
    boot_result_t r = boot_rollback_validate(&s_rb_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_FATAL,                          "_rollback_pad nonzero -> FATAL");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_ROLLBACK_ERR_RESERVED_NONZERO,  "err=RESERVED_NONZERO");
}

static void test_boot_rollback_refusal_flag_warning(void)
{
    TEST_KLOG_SUPPRESS("boot");
    rb_zero();
    /* Stale REFUSAL flag in boot_info -- the kernel shouldn't see
     * this because the bootloader should have halted, but if it
     * does, the validator logs a WARN and keeps booting (the kernel
     * running IS proof the downgrade check passed). */
    s_rb_buf.flags                       = BOOT_FLAG_ROLLBACK_REFUSAL;
    s_rb_buf.os_loader_security_version  = 5u;
    s_rb_buf.required_security_version   = 3u;

    enum boot_rollback_error err = BOOT_ROLLBACK_ERR_OK;
    boot_result_t r = boot_rollback_validate(&s_rb_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "stale REFUSAL flag -> OK + WARN");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_ROLLBACK_ERR_OK, "err=OK");
}

static void test_boot_rollback_read_failed_flag_accepted(void)
{
    TEST_KLOG_SUPPRESS("boot");
    rb_zero();
    /* READ_FAILED flag alone (without REFUSAL) is legitimate telemetry
     * from the bootloader -- validator accepts with WARN. */
    s_rb_buf.flags                       = BOOT_FLAG_ROLLBACK_READ_FAILED;
    s_rb_buf.os_loader_security_version  = 1u;
    s_rb_buf.required_security_version   = 0u;

    enum boot_rollback_error err = BOOT_ROLLBACK_ERR_OK;
    boot_result_t r = boot_rollback_validate(&s_rb_buf, &err);
    TEST_ASSERT_EQ((uint64_t)r,   (uint64_t)BOOT_OK,              "READ_FAILED flag stale -> OK + WARN");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_ROLLBACK_ERR_OK, "err=OK");
}

static void test_boot_rollback_should_raise_opt_out(void)
{
    rb_zero();
    /* shipped=7, required=5, opt_in=0: MUST NOT raise even though
     * shipped > required. Policy opt-out keeps the counter frozen. */
    s_rb_buf.os_loader_security_version  = 7u;
    s_rb_buf.required_security_version   = 5u;

    uint32_t new_value = 0xBEEF;
    int raise = boot_rollback_should_raise(&s_rb_buf, /*opt_in*/ 0, &new_value);
    TEST_ASSERT_EQ((uint64_t)raise, 0u, "opt-out -> no raise");
    TEST_ASSERT_EQ((uint64_t)new_value, 0xBEEFu, "new_value untouched on opt-out");
}

static void test_boot_rollback_should_raise_opt_in(void)
{
    rb_zero();
    /* shipped=7, required=5, opt_in=1: raise to 7. */
    s_rb_buf.os_loader_security_version  = 7u;
    s_rb_buf.required_security_version   = 5u;

    uint32_t new_value = 0;
    int raise = boot_rollback_should_raise(&s_rb_buf, /*opt_in*/ 1, &new_value);
    TEST_ASSERT_EQ((uint64_t)raise, 1u, "opt-in + newer -> raise");
    TEST_ASSERT_EQ((uint64_t)new_value, 7u, "new_value == shipped");
}

static void test_boot_rollback_should_raise_downgrade_impossible(void)
{
    rb_zero();
    /* shipped=3, required=5: would be rejected pre-jump by bootloader;
     * if we somehow reach the raise path, never decrease. */
    s_rb_buf.os_loader_security_version  = 3u;
    s_rb_buf.required_security_version   = 5u;

    uint32_t new_value = 0xBEEF;
    int raise = boot_rollback_should_raise(&s_rb_buf, /*opt_in*/ 1, &new_value);
    TEST_ASSERT_EQ((uint64_t)raise, 0u, "shipped < required -> no raise (never decrease)");
    TEST_ASSERT_EQ((uint64_t)new_value, 0xBEEFu, "new_value untouched on downgrade");
}

static void test_boot_rollback_should_raise_null_safe(void)
{
    uint32_t new_value = 0xBEEF;
    int raise = boot_rollback_should_raise((struct boot_info *)0, /*opt_in*/ 1, &new_value);
    TEST_ASSERT_EQ((uint64_t)raise, 0u, "NULL info -> no raise");
    /* Also null new_value pointer */
    raise = boot_rollback_should_raise(&s_rb_buf, /*opt_in*/ 1, (uint32_t *)0);
    TEST_ASSERT_EQ((uint64_t)raise, 0u, "NULL new_value safe");
}

/* Compositor-steady gate tests. Exercise mark_steady / raise_if_steady
 * without triggering uefi_set_variable: tests stage g_boot_info so
 * should_raise returns 0 (opt-out), keeping them host-safe. */
extern struct boot_info g_boot_info;

static void test_boot_rollback_raise_withheld_without_steady(void)
{
    TEST_KLOG_SUPPRESS("boot");
    boot_rollback_reset_for_test();

    uint8_t  saved_opt   = g_boot_info.config.anti_rollback_raise;
    uint32_t saved_ship  = g_boot_info.os_loader_security_version;
    uint32_t saved_req   = g_boot_info.required_security_version;
    /* Opt-out so even if mark_steady somehow fired we'd skip the live
     * NVRAM write. This test does NOT mark_steady -- the gate alone
     * must withhold the raise. */
    g_boot_info.config.anti_rollback_raise    = 0;
    g_boot_info.os_loader_security_version    = 7u;
    g_boot_info.required_security_version     = 5u;

    int raised = boot_rollback_raise_if_steady();
    TEST_ASSERT_EQ((uint64_t)raised, 0u, "no mark_steady -> no raise");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_is_steady(), 0u, "steady stays clear");

    g_boot_info.config.anti_rollback_raise    = saved_opt;
    g_boot_info.os_loader_security_version    = saved_ship;
    g_boot_info.required_security_version     = saved_req;
    boot_rollback_reset_for_test();
}

static void test_boot_rollback_mark_steady_latches(void)
{
    TEST_KLOG_SUPPRESS("boot");
    boot_rollback_reset_for_test();

    TEST_ASSERT_EQ((uint64_t)boot_rollback_is_steady(), 0u, "starts clear");
    boot_rollback_mark_steady();
    TEST_ASSERT_EQ((uint64_t)boot_rollback_is_steady(), 1u, "latches on mark");
    boot_rollback_mark_steady();
    TEST_ASSERT_EQ((uint64_t)boot_rollback_is_steady(), 1u, "idempotent second mark");

    boot_rollback_reset_for_test();
    TEST_ASSERT_EQ((uint64_t)boot_rollback_is_steady(), 0u, "reset clears");
}

static void test_boot_rollback_raise_one_shot_opt_out(void)
{
    TEST_KLOG_SUPPRESS("boot");
    boot_rollback_reset_for_test();

    /* Stage opt-out so should_raise returns 0 even after mark_steady.
     * The helper still latches s_attempted so subsequent calls are
     * no-ops -- prevents re-evaluation on every compositor wake. */
    uint8_t saved_opt = g_boot_info.config.anti_rollback_raise;
    g_boot_info.config.anti_rollback_raise = 0;

    boot_rollback_mark_steady();
    int r1 = boot_rollback_raise_if_steady();
    int r2 = boot_rollback_raise_if_steady();
    TEST_ASSERT_EQ((uint64_t)r1, 0u, "opt-out -> no raise (first call)");
    TEST_ASSERT_EQ((uint64_t)r2, 0u, "opt-out -> no raise (second call)");
    /* Split-latch semantic: opt-out latches s_attempted (so second
     * call early-outs) but must NOT set s_raised -- was_raised()
     * means NVRAM was actually advanced, which did not happen. */
    TEST_ASSERT_EQ((uint64_t)boot_rollback_was_raised(), 0u,
                   "opt-out never advances NVRAM");

    g_boot_info.config.anti_rollback_raise = saved_opt;
    boot_rollback_reset_for_test();
}

static void test_boot_rollback_was_raised_reset_clears(void)
{
    TEST_KLOG_SUPPRESS("boot");
    boot_rollback_reset_for_test();
    TEST_ASSERT_EQ((uint64_t)boot_rollback_was_raised(), 0u,
                   "was_raised starts 0");
    /* We cannot exercise the success branch here (would call
     * uefi_set_variable which forbidden in tests). Verify the
     * reset helper clears the flag if a prior real boot had set
     * it. Use the reset_for_test + is_steady round trip as proof
     * that all three latches wire to the same reset. */
    boot_rollback_mark_steady();
    TEST_ASSERT_EQ((uint64_t)boot_rollback_is_steady(), 1u,
                   "steady latched");
    boot_rollback_reset_for_test();
    TEST_ASSERT_EQ((uint64_t)boot_rollback_is_steady(), 0u,
                   "reset clears steady");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_was_raised(), 0u,
                   "reset keeps raised clear");
}

/* Codex 2026-04-30 anti-rollback-deferred-raise perf finding: boot_rollback_request_raise
 * defers the NVRAM SetVariable to sys_wq; the synchronous fallback
 * (sys_wq null OR pool exhausted) keeps the one-shot compositor
 * first-frame contract honest. Test the fallback path: with sys_wq
 * deliberately null and opt-out policy, request_raise must run the
 * sync raise (which short-circuits as a no-op due to opt-out, but
 * exercises the fallback dispatch). The deferred path itself is
 * exercised on every real boot; here we focus on the contract that
 * a missing sys_wq does not silently drop the only attempt. */
extern workqueue_t *sys_wq;
static void test_boot_rollback_request_raise_sync_fallback(void)
{
    TEST_KLOG_SUPPRESS("boot");
    boot_rollback_reset_for_test();

    workqueue_t *saved_wq = sys_wq;
    uint8_t saved_opt = g_boot_info.config.anti_rollback_raise;

    sys_wq = (workqueue_t *)0;            /* force fallback */
    g_boot_info.config.anti_rollback_raise = 0; /* opt-out: sync raise no-ops */
    boot_rollback_mark_steady();

    int r1 = boot_rollback_request_raise();
    TEST_ASSERT_EQ((uint64_t)r1, 1u,
                   "fallback path takes ownership of the request");
    /* Idempotent: second call must NOT re-enqueue / re-run; s_enqueued
     * is latched. */
    int r2 = boot_rollback_request_raise();
    TEST_ASSERT_EQ((uint64_t)r2, 0u,
                   "second request is idempotent no-op");

    /* Restore + clean up so other tests see normal state. */
    sys_wq = saved_wq;
    g_boot_info.config.anti_rollback_raise = saved_opt;
    boot_rollback_reset_for_test();
}

/* Regression: pre-steady request_raise must NOT latch s_enqueued, or
 * a later post-steady caller would observe the stale latch and never
 * advance the rollback floor. (Codex 2026-04-30 full-section review
 * H1 / consistency H1: caller-ordering safety in the public helper.) */
static void test_boot_rollback_request_raise_pre_steady_no_latch(void)
{
    TEST_KLOG_SUPPRESS("boot");
    boot_rollback_reset_for_test();

    workqueue_t *saved_wq = sys_wq;
    uint8_t saved_opt = g_boot_info.config.anti_rollback_raise;

    sys_wq = (workqueue_t *)0;
    g_boot_info.config.anti_rollback_raise = 0; /* opt-out keeps NVRAM untouched */

    /* Pre-steady: request must short-circuit to 0 without latching. */
    int r0 = boot_rollback_request_raise();
    TEST_ASSERT_EQ((uint64_t)r0, 0u,
                   "pre-steady request returns 0");

    /* Now mark steady and request again -- this call must take
     * ownership (return 1). If the earlier call had wrongly latched
     * s_enqueued, this would see the stale latch and return 0. */
    boot_rollback_mark_steady();
    int r1 = boot_rollback_request_raise();
    TEST_ASSERT_EQ((uint64_t)r1, 1u,
                   "post-steady request still owns the raise");

    sys_wq = saved_wq;
    g_boot_info.config.anti_rollback_raise = saved_opt;
    boot_rollback_reset_for_test();
}

/* ---- Bounded-retry state machine (section 25) ----
 *
 * The retry path is unreachable without fault injection: no harness
 * can make real firmware fail one SetVariable and pass the next,
 * which is exactly the case the retry exists for. These cases install
 * a fake writer, run the chain inline on the test thread, and zero
 * the backoff, so the whole state machine is observed
 * deterministically and off the wall clock.
 *
 * Every case forces sys_wq NULL so the raise takes the synchronous
 * path on this thread rather than racing the workqueue worker. */

#define RB_FI_MAX_SCRIPT 8u

static uint32_t s_fi_calls        = 0;   /* writer invocations */
static uint64_t s_fi_script[RB_FI_MAX_SCRIPT];  /* status per call, in order */
static uint32_t s_fi_script_len   = 0;   /* calls past the end return success */
static uint32_t s_fi_value_seen   = 0;   /* value of the LAST write attempt */
static int      s_fi_nested_rc    = -1;  /* rc of a request issued mid-chain */
static int      s_fi_nested_probe = 0;   /* issue that nested request? */

static uint64_t rb_fault_writer(uint32_t value)
{
    uint64_t status;

    s_fi_calls++;
    s_fi_value_seen = value;

    /* Concurrency probe: a second requester arriving while the chain
     * still owns its claim must be refused, and must not produce a
     * second writer call. Issued from inside the first write so the
     * chain is provably mid-flight. */
    if (s_fi_nested_probe && s_fi_calls == 1u)
        s_fi_nested_rc = boot_rollback_request_raise();

    /* Scripted status sequence: the Nth call returns the Nth entry,
     * and anything past the end succeeds. This is what makes
     * transient-then-terminal and mixed-status orders expressible. */
    status = 0u;
    if (s_fi_calls <= s_fi_script_len)
        status = s_fi_script[s_fi_calls - 1u];
    return status;
}

/* Stage opt-in policy + injected writer. The script is copied so the
 * caller can pass a literal array. Returns via out-params so the
 * caller can restore the globals it borrowed. */
static void rb_fault_begin(const uint64_t *script, uint32_t script_len,
                           workqueue_t **saved_wq, uint8_t *saved_opt,
                           uint32_t *saved_ship, uint32_t *saved_req)
{
    uint32_t i;

    boot_rollback_reset_for_test();

    s_fi_calls        = 0u;
    s_fi_value_seen   = 0u;
    s_fi_nested_rc    = -1;
    s_fi_nested_probe = 0;
    s_fi_script_len   = (script_len > RB_FI_MAX_SCRIPT) ? RB_FI_MAX_SCRIPT
                                                        : script_len;
    for (i = 0u; i < s_fi_script_len; i++)
        s_fi_script[i] = script[i];

    *saved_wq   = sys_wq;
    *saved_opt  = g_boot_info.config.anti_rollback_raise;
    *saved_ship = g_boot_info.os_loader_security_version;
    *saved_req  = g_boot_info.required_security_version;

    sys_wq = (workqueue_t *)0;                  /* synchronous, deterministic */
    g_boot_info.config.anti_rollback_raise = 1; /* opt in so the write happens */
    g_boot_info.os_loader_security_version = 7u;
    g_boot_info.required_security_version  = 5u;

    boot_rollback_set_writer_for_test(rb_fault_writer);
    boot_rollback_set_inline_retry_for_test(1); /* chain runs on this thread */
}

static void rb_fault_end(workqueue_t *saved_wq, uint8_t saved_opt,
                         uint32_t saved_ship, uint32_t saved_req)
{
    sys_wq = saved_wq;
    g_boot_info.config.anti_rollback_raise = saved_opt;
    g_boot_info.os_loader_security_version = saved_ship;
    g_boot_info.required_security_version  = saved_req;
    s_fi_nested_probe = 0;
    boot_rollback_reset_for_test();   /* also restores the production writer */
}

static void test_boot_rollback_retry_transient_then_success(void)
{
    TEST_KLOG_SUPPRESS("boot");
    workqueue_t *wq; uint8_t opt; uint32_t ship, req;
    static const uint64_t script[] = { UEFI_DEVICE_ERROR };
    rb_fault_begin(script, 1u, &wq, &opt, &ship, &req);

    boot_rollback_mark_steady();
    int rc = boot_rollback_request_raise();

    TEST_ASSERT_EQ((uint64_t)rc, 1u, "request owns the raise");
    TEST_ASSERT_EQ((uint64_t)s_fi_calls, 2u,
                   "one failure then one success: exactly 2 writes");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_was_raised(), 1u,
                   "floor advanced after the retry");
    TEST_ASSERT_EQ((uint64_t)s_fi_value_seen, 7u,
                   "retry writes the shipped version, not a stale value");

    /* No second compositor first-frame request exists on a real boot;
     * assert that even if one arrived it cannot double-write. */
    int rc2 = boot_rollback_request_raise();
    TEST_ASSERT_EQ((uint64_t)rc2, 0u, "post-success request is a no-op");
    TEST_ASSERT_EQ((uint64_t)s_fi_calls, 2u, "no third write");

    rb_fault_end(wq, opt, ship, req);
}

static void test_boot_rollback_retry_exhaustion_caps_attempts(void)
{
    TEST_KLOG_SUPPRESS("boot");
    workqueue_t *wq; uint8_t opt; uint32_t ship, req;
    /* Fail every call: the cap, not the firmware, must end the chain.
     * Five entries against a cap of four also proves the cap binds
     * before the script runs out. */
    static const uint64_t script[] = {
        UEFI_DEVICE_ERROR, UEFI_DEVICE_ERROR, UEFI_DEVICE_ERROR,
        UEFI_DEVICE_ERROR, UEFI_DEVICE_ERROR
    };
    rb_fault_begin(script, 5u, &wq, &opt, &ship, &req);

    boot_rollback_mark_steady();
    (void)boot_rollback_request_raise();

    TEST_ASSERT_EQ((uint64_t)s_fi_calls, (uint64_t)BOOT_ROLLBACK_MAX_ATTEMPTS,
                   "attempts stop at the cap");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_attempts_for_test(),
                   (uint64_t)BOOT_ROLLBACK_MAX_ATTEMPTS,
                   "counter agrees with the write count");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_was_raised(), 0u,
                   "floor never advanced");
    /* The give-up telemetry is a completion requirement, so assert it
     * rather than only suppressing it. */
    TEST_ASSERT_EQ((uint64_t)boot_rollback_terminal_reports_for_test(), 1u,
                   "exactly one exhaustion report");
    TEST_ASSERT_EQ(boot_rollback_terminal_status_for_test(),
                   (uint64_t)UEFI_DEVICE_ERROR,
                   "report carries the final firmware status");

    /* Terminal for the boot: nothing re-arms the chain. */
    int rc2 = boot_rollback_request_raise();
    TEST_ASSERT_EQ((uint64_t)rc2, 0u, "post-exhaustion request refused");
    TEST_ASSERT_EQ((uint64_t)s_fi_calls, (uint64_t)BOOT_ROLLBACK_MAX_ATTEMPTS,
                   "no attempt past the cap");

    rb_fault_end(wq, opt, ship, req);
}

static void test_boot_rollback_terminal_status_not_retried(void)
{
    TEST_KLOG_SUPPRESS("boot");
    workqueue_t *wq; uint8_t opt; uint32_t ship, req;
    /* A write-protected variable store is a decision the firmware
     * makes identically every time. Retrying spends three more
     * blocking calls and mislabels a refusal as a timeout. */
    static const uint64_t script[] = {
        UEFI_WRITE_PROTECTED, UEFI_WRITE_PROTECTED
    };
    rb_fault_begin(script, 2u, &wq, &opt, &ship, &req);

    boot_rollback_mark_steady();
    (void)boot_rollback_request_raise();

    TEST_ASSERT_EQ((uint64_t)s_fi_calls, 1u,
                   "terminal status stops at the first attempt");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_was_raised(), 0u,
                   "floor never advanced");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_terminal_reports_for_test(), 1u,
                   "exactly one refusal report");
    TEST_ASSERT_EQ(boot_rollback_terminal_status_for_test(),
                   (uint64_t)UEFI_WRITE_PROTECTED,
                   "report names the refusing status, not a timeout");

    int rc2 = boot_rollback_request_raise();
    TEST_ASSERT_EQ((uint64_t)rc2, 0u, "post-terminal request refused");
    TEST_ASSERT_EQ((uint64_t)s_fi_calls, 1u, "still exactly one write");

    rb_fault_end(wq, opt, ship, req);
}

/* The second allowlist entry must recover exactly like the first: a
 * variable store briefly out of working memory is the other status
 * this section exists to survive. */
static void test_boot_rollback_out_of_resources_is_retried(void)
{
    TEST_KLOG_SUPPRESS("boot");
    workqueue_t *wq; uint8_t opt; uint32_t ship, req;
    static const uint64_t script[] = { UEFI_OUT_OF_RESOURCES };
    rb_fault_begin(script, 1u, &wq, &opt, &ship, &req);

    boot_rollback_mark_steady();
    (void)boot_rollback_request_raise();

    TEST_ASSERT_EQ((uint64_t)s_fi_calls, 2u, "resource shortage is retried");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_was_raised(), 1u,
                   "floor advanced on the retry");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_terminal_reports_for_test(), 0u,
                   "a recovered chain reports nothing");

    rb_fault_end(wq, opt, ship, req);
}

/* A chain that starts transient and then hits a refusal must stop at
 * the refusal, not run out the remaining attempts. */
static void test_boot_rollback_transient_then_terminal_stops(void)
{
    TEST_KLOG_SUPPRESS("boot");
    workqueue_t *wq; uint8_t opt; uint32_t ship, req;
    static const uint64_t script[] = {
        UEFI_DEVICE_ERROR, UEFI_SECURITY_VIOLATION, UEFI_DEVICE_ERROR
    };
    rb_fault_begin(script, 3u, &wq, &opt, &ship, &req);

    boot_rollback_mark_steady();
    (void)boot_rollback_request_raise();

    TEST_ASSERT_EQ((uint64_t)s_fi_calls, 2u,
                   "chain stops at the refusal, short of the cap");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_was_raised(), 0u,
                   "floor never advanced");
    TEST_ASSERT_EQ(boot_rollback_terminal_status_for_test(),
                   (uint64_t)UEFI_SECURITY_VIOLATION,
                   "report names the refusal, not the earlier hiccup");

    rb_fault_end(wq, opt, ship, req);
}

/* Fail-closed: a status outside the allowlist is not demonstrably
 * transient, so it ends the chain at the first attempt. */
static void test_boot_rollback_unknown_status_is_terminal(void)
{
    TEST_KLOG_SUPPRESS("boot");
    workqueue_t *wq; uint8_t opt; uint32_t ship, req;
    /* High bit set marks an EFI error; the code itself is one this
     * kernel does not classify. */
    static const uint64_t script[] = { (0x2AULL | (1ULL << 63)) };
    rb_fault_begin(script, 1u, &wq, &opt, &ship, &req);

    boot_rollback_mark_steady();
    (void)boot_rollback_request_raise();

    TEST_ASSERT_EQ((uint64_t)s_fi_calls, 1u,
                   "unclassified status is not retried");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_terminal_reports_for_test(), 1u,
                   "exactly one report");

    rb_fault_end(wq, opt, ship, req);
}

/* The last attempt inside the cap must still be able to succeed --
 * an off-by-one in the give-up test would burn it. */
static void test_boot_rollback_succeeds_on_final_attempt(void)
{
    TEST_KLOG_SUPPRESS("boot");
    workqueue_t *wq; uint8_t opt; uint32_t ship, req;
    static const uint64_t script[] = {
        UEFI_DEVICE_ERROR, UEFI_DEVICE_ERROR, UEFI_DEVICE_ERROR
    };
    rb_fault_begin(script, 3u, &wq, &opt, &ship, &req);

    boot_rollback_mark_steady();
    (void)boot_rollback_request_raise();

    TEST_ASSERT_EQ((uint64_t)s_fi_calls, (uint64_t)BOOT_ROLLBACK_MAX_ATTEMPTS,
                   "the fourth write is still made");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_was_raised(), 1u,
                   "success on the final allowed attempt still raises");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_terminal_reports_for_test(), 0u,
                   "no give-up report when the last attempt lands");

    rb_fault_end(wq, opt, ship, req);
}

/* No carrier for the retry: the raise must fail CLOSED with the one
 * report, never sit latched behind a chain that will never run. */
static void test_boot_rollback_retry_scheduling_failure_is_terminal(void)
{
    TEST_KLOG_SUPPRESS("boot");
    workqueue_t *wq; uint8_t opt; uint32_t ship, req;
    static const uint64_t script[] = { UEFI_DEVICE_ERROR };
    rb_fault_begin(script, 1u, &wq, &opt, &ship, &req);
    boot_rollback_set_inline_retry_for_test(0);  /* take the real carrier path */
    boot_rollback_set_sched_fail_for_test(1);    /* which cannot be armed */

    boot_rollback_mark_steady();
    (void)boot_rollback_request_raise();

    TEST_ASSERT_EQ((uint64_t)s_fi_calls, 1u,
                   "one attempt, then nothing to carry the retry");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_was_raised(), 0u,
                   "floor never advanced");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_terminal_reports_for_test(), 1u,
                   "the unschedulable retry is reported once");

    int rc2 = boot_rollback_request_raise();
    TEST_ASSERT_EQ((uint64_t)rc2, 0u, "state is terminal, not retryable");
    TEST_ASSERT_EQ((uint64_t)s_fi_calls, 1u, "still exactly one write");

    rb_fault_end(wq, opt, ship, req);
}

/* The backoff ladder is a pure function, so assert it directly rather
 * than inferring it from timing. */
static void test_boot_rollback_backoff_progression(void)
{
    TEST_ASSERT_EQ((uint64_t)boot_rollback_next_backoff_for_test(0u), 25u,
                   "first backoff is 25 ms");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_next_backoff_for_test(25u), 50u,
                   "then 50 ms");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_next_backoff_for_test(50u), 100u,
                   "then 100 ms");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_next_backoff_for_test(100u), 100u,
                   "and clamps at 100 ms");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_next_backoff_for_test(0x80000000u),
                   100u, "a doubling that would wrap clamps instead");
}

static void test_boot_rollback_retry_excludes_concurrent_request(void)
{
    TEST_KLOG_SUPPRESS("boot");
    workqueue_t *wq; uint8_t opt; uint32_t ship, req;
    static const uint64_t script[] = { UEFI_DEVICE_ERROR };
    rb_fault_begin(script, 1u, &wq, &opt, &ship, &req);
    s_fi_nested_probe = 1;   /* second requester arrives mid-write */

    boot_rollback_mark_steady();
    (void)boot_rollback_request_raise();

    TEST_ASSERT_EQ((uint64_t)s_fi_nested_rc, 0u,
                   "a request during the live chain is refused");
    TEST_ASSERT_EQ((uint64_t)s_fi_calls, 2u,
                   "the chain owns both writes; the intruder added none");
    TEST_ASSERT_EQ((uint64_t)boot_rollback_was_raised(), 1u,
                   "chain still completes the raise exactly once");

    rb_fault_end(wq, opt, ship, req);
}

void test_register_boot_rollback(void)
{
    test_suite_register_cat("boot_rollback: NULL info rejected",
                            test_boot_rollback_null_info, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: first-ever boot (required=0)",
                            test_boot_rollback_first_ever_boot, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: equal versions accepted + no raise",
                            test_boot_rollback_equal_versions, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: unknown flag bit rejected",
                            test_boot_rollback_unknown_flag_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: version out-of-range rejected",
                            test_boot_rollback_version_oor_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: _rollback_pad nonzero rejected (Rule 3)",
                            test_boot_rollback_reserved_pad_nonzero_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: stale REFUSAL flag warns",
                            test_boot_rollback_refusal_flag_warning, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: stale READ_FAILED flag accepted + WARN",
                            test_boot_rollback_read_failed_flag_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: opt-out never raises",
                            test_boot_rollback_should_raise_opt_out, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: opt-in raises to shipped",
                            test_boot_rollback_should_raise_opt_in, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: never decrease on downgrade input",
                            test_boot_rollback_should_raise_downgrade_impossible, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: NULL-safe decision helper",
                            test_boot_rollback_should_raise_null_safe, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: raise withheld without mark_steady",
                            test_boot_rollback_raise_withheld_without_steady, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: mark_steady latches + reset clears",
                            test_boot_rollback_mark_steady_latches, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: raise is one-shot even on opt-out",
                            test_boot_rollback_raise_one_shot_opt_out, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: request_raise sync fallback when sys_wq null",
                            test_boot_rollback_request_raise_sync_fallback, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: request_raise pre-steady is no-latch no-op",
                            test_boot_rollback_request_raise_pre_steady_no_latch, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: was_raised split stays 0 on opt-out; reset clears",
                            test_boot_rollback_was_raised_reset_clears, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: transient failure then success raises exactly once",
                            test_boot_rollback_retry_transient_then_success, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: all-fail chain stops at the attempt cap",
                            test_boot_rollback_retry_exhaustion_caps_attempts, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: terminal firmware status is not retried",
                            test_boot_rollback_terminal_status_not_retried, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: live retry chain excludes a concurrent request",
                            test_boot_rollback_retry_excludes_concurrent_request, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: out-of-resources is retried like device-error",
                            test_boot_rollback_out_of_resources_is_retried, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: transient then terminal stops at the refusal",
                            test_boot_rollback_transient_then_terminal_stops, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: unclassified firmware status is terminal",
                            test_boot_rollback_unknown_status_is_terminal, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: success on the final allowed attempt still raises",
                            test_boot_rollback_succeeds_on_final_attempt, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: unschedulable retry fails closed with one report",
                            test_boot_rollback_retry_scheduling_failure_is_terminal, TEST_CAT_BOOT);
    test_suite_register_cat("boot_rollback: backoff ladder 25/50/100 with clamp",
                            test_boot_rollback_backoff_progression, TEST_CAT_BOOT);
}
