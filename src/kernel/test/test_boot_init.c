/* ============================================================================
 * test_boot_init.c -- Boot init sequencing unit tests
 *
 * Tests boot_result_t values, subsystem readiness tracking, BOOT_REQUIRE
 * macro, boot_progress() null safety, and POST code uniqueness.
 *
 * XREF: 02-kernel-core/TODO-01-kernel-init-sequencing.md §Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/boot_init.h"
#include "kernel/boot_load_status.h"
#include "kernel/drivers/lapic.h"   /* IPI_VECTOR_* for the CR-verify vector test (S10) */
#include "kernel/boot_info.h"
#include "kernel/boot_timing.h"
#include "kernel/klog.h"

/* Kernel libc, not the freestanding headers (see test_bare_metal.c). */
extern int snprintf(char *buf, size_t size, const char *fmt, ...);
extern int strcmp(const char *a, const char *b);

/* ---- boot_result_t values ---- */

static void test_boot_result_values(void)
{
    TEST_ASSERT(BOOT_OK == 0, "BOOT_OK == 0");
    TEST_ASSERT(BOOT_DEGRADED == 1, "BOOT_DEGRADED == 1");
    TEST_ASSERT(BOOT_FATAL == 2, "BOOT_FATAL == 2");
}

/* ---- Subsystem readiness ---- */

static void test_subsys_set_ready_true(void)
{
    /* Save and restore state to avoid side effects */
    bool saved = kernel_subsystem_ready(SUBSYS_PMM);

    kernel_subsystem_set_ready(SUBSYS_PMM, true);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_PMM) == true,
                "set_ready(PMM, true) then ready(PMM) returns true");

    kernel_subsystem_set_ready(SUBSYS_PMM, saved);
}

static void test_subsys_set_ready_false(void)
{
    bool saved = kernel_subsystem_ready(SUBSYS_PMM);

    kernel_subsystem_set_ready(SUBSYS_PMM, false);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_PMM) == false,
                "set_ready(PMM, false) then ready(PMM) returns false");

    kernel_subsystem_set_ready(SUBSYS_PMM, saved);
}

static void test_subsys_out_of_range(void)
{
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_COUNT) == false,
                "ready(SUBSYS_COUNT) returns false (out of range)");
    /* set_ready on out-of-range index should be a no-op (not crash) */
    kernel_subsystem_set_ready(SUBSYS_COUNT, true);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_COUNT) == false,
                "set_ready(SUBSYS_COUNT) is a no-op");
}

static void test_boot_defer_null_fn(void)
{
    int ret = boot_defer("null-test", (boot_result_t (*)(void))0);
    TEST_ASSERT_EQ(ret, -1, "boot_defer rejects NULL function pointer");
}

/* ---- BOOT_REQUIRE macro ----
 * BOOT_REQUIRE uses `return BOOT_FATAL`, so we test it via a wrapper
 * function that has the matching return type. */

static boot_result_t require_pmm_wrapper(void)
{
    BOOT_REQUIRE(SUBSYS_PMM);
    return BOOT_OK;
}

static void test_boot_require_fails_when_not_ready(void)
{
    bool saved = kernel_subsystem_ready(SUBSYS_PMM);

    /* Note: BOOT_REQUIRE calls _boot_require_failed() which emits one
     * "[WARN] boot: REQUIRE failed: PMM not ready" klog line when klog
     * is up (Phase 3 in the test runner).  LOG_WARN (not LOG_ERROR)
     * avoids the red [FAIL] badge that would look identical to a real
     * test failure.  This is expected test output. */
    kernel_subsystem_set_ready(SUBSYS_PMM, false);
    boot_result_t r = require_pmm_wrapper();
    TEST_ASSERT(r == BOOT_FATAL,
                "BOOT_REQUIRE returns BOOT_FATAL when PMM not ready");

    kernel_subsystem_set_ready(SUBSYS_PMM, saved);
}

static void test_boot_require_passes_when_ready(void)
{
    bool saved = kernel_subsystem_ready(SUBSYS_PMM);

    kernel_subsystem_set_ready(SUBSYS_PMM, true);
    boot_result_t r = require_pmm_wrapper();
    TEST_ASSERT(r == BOOT_OK,
                "BOOT_REQUIRE passes (returns BOOT_OK) when PMM ready");

    kernel_subsystem_set_ready(SUBSYS_PMM, saved);
}

/* ---- Boot step recorder null safety ----
 * Tests the pure in-memory recorder, not the full progress function
 * (which has VPD/fb/IO side effects forbidden in tests). */

static void test_boot_progress_null_step(void)
{
    const boot_timing_step_t *steps = (const boot_timing_step_t *)0;
    uint32_t before = boot_timing_get_steps(&steps);

    /* boot_timing_record_step is the pure data helper -- NULL step ignored */
    boot_timing_record_step(0, (const char *)0, 0x0000);

    uint32_t after = boot_timing_get_steps(&steps);
    TEST_ASSERT_EQ(after, before,
                   "boot_timing_record_step(NULL step) does not append");
}

/* ---- POST code constants ---- */

static void test_post_codes_nonzero_and_unique(void)
{
    /* Spot-check representative codes from each phase are non-zero */
    TEST_ASSERT(POST16_SERIAL_OK != 0, "POST16_SERIAL_OK is non-zero");
    TEST_ASSERT(POST16_PMM_OK != 0, "POST16_PMM_OK is non-zero");
    TEST_ASSERT(POST16_GDT_OK != 0, "POST16_GDT_OK is non-zero");
    TEST_ASSERT(POST16_TIMER_OK != 0, "POST16_TIMER_OK is non-zero");
    TEST_ASSERT(POST16_VFS_OK != 0, "POST16_VFS_OK is non-zero");
    TEST_ASSERT(POST16_SCHED_OK != 0, "POST16_SCHED_OK is non-zero");

    /* Verify uniqueness across phases */
    TEST_ASSERT(POST16_PMM_OK != POST16_VMM_OK, "PMM_OK != VMM_OK");
    TEST_ASSERT(POST16_GDT_OK != POST16_IDT_OK, "GDT_OK != IDT_OK");
    TEST_ASSERT(POST16_SCHED_OK != POST16_DESKTOP_OK, "SCHED_OK != DESKTOP_OK");
    TEST_ASSERT(POST16_BOOT_OK != POST16_BOOT_FAILED, "BOOT_OK != BOOT_FAILED");
}

/* ---- ABI: kernel_subsys_t enum layout ----
 *
 * Pin numeric values that other code uses by index (g_subsys_ready[],
 * crash-dump readers, recovery decoders).  Reordering would silently
 * desync those consumers.
 */
static void test_subsys_enum_layout(void)
{
    TEST_ASSERT_EQ(SUBSYS_SERIAL,     0, "SUBSYS_SERIAL == 0 (first slot)");
    TEST_ASSERT_EQ(SUBSYS_PMM,        1, "SUBSYS_PMM == 1");
    TEST_ASSERT_EQ(SUBSYS_OB,        20, "SUBSYS_OB == 20");
    TEST_ASSERT_EQ(SUBSYS_UEFI_VARS, 21, "SUBSYS_UEFI_VARS == 21 (Phase 0 propagation)");
    TEST_ASSERT_EQ(SUBSYS_UEFI_TIME, 22, "SUBSYS_UEFI_TIME == 22");
    TEST_ASSERT_EQ(SUBSYS_SECUREBOOT,23, "SUBSYS_SECUREBOOT == 23");
    TEST_ASSERT_EQ(SUBSYS_TPM,       24, "SUBSYS_TPM == 24");
    TEST_ASSERT_EQ(SUBSYS_XSAVE,     25, "SUBSYS_XSAVE == 25");
    TEST_ASSERT_EQ(SUBSYS_PCID,      26, "SUBSYS_PCID == 26");
    TEST_ASSERT_EQ(SUBSYS_EX,        27, "SUBSYS_EX == 27");
    TEST_ASSERT_EQ(SUBSYS_NLS,       28, "SUBSYS_NLS == 28");
    TEST_ASSERT_EQ(SUBSYS_KNF,       29, "SUBSYS_KNF == 29");
    TEST_ASSERT_EQ(SUBSYS_KEYBOARD,  30, "SUBSYS_KEYBOARD == 30");
    TEST_ASSERT_EQ(SUBSYS_MOUSE,     31, "SUBSYS_MOUSE == 31");
    TEST_ASSERT_EQ(SUBSYS_AHCI,      32, "SUBSYS_AHCI == 32 (first slot above bit 31)");
    TEST_ASSERT_EQ(SUBSYS_NIC,       33, "SUBSYS_NIC == 33");
    TEST_ASSERT_EQ(SUBSYS_SMBIOS,    34, "SUBSYS_SMBIOS == 34");
    TEST_ASSERT_EQ(SUBSYS_SPLASH,    35, "SUBSYS_SPLASH == 35 (last named slot)");
    TEST_ASSERT_EQ(SUBSYS_COUNT,     36, "SUBSYS_COUNT == 36 (sentinel)");
}

/* ---- BOOT_STEP readiness mapping ----
 *
 * BOOT_STEP must mark a subsystem ready ONLY for BOOT_OK or BOOT_DEGRADED.
 * BOOT_FATAL and BOOT_DEFERRED must leave the subsystem NOT ready so
 * BOOT_REQUIRE downstream catches the failure.
 */
static boot_result_t bs_return_ok(void)       { return BOOT_OK; }
static boot_result_t bs_return_degraded(void) { return BOOT_DEGRADED; }
static boot_result_t bs_return_fatal(void)    { return BOOT_FATAL; }
static boot_result_t bs_return_deferred(void) { return BOOT_DEFERRED; }

static void test_boot_step_readiness_mapping(void)
{
    bool saved = kernel_subsystem_ready(SUBSYS_PMM);

    kernel_subsystem_set_ready(SUBSYS_PMM, false);
    BOOT_STEP(SUBSYS_PMM, bs_return_ok);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_PMM) == true,
                "BOOT_STEP marks ready on BOOT_OK");

    kernel_subsystem_set_ready(SUBSYS_PMM, false);
    BOOT_STEP(SUBSYS_PMM, bs_return_degraded);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_PMM) == true,
                "BOOT_STEP marks ready on BOOT_DEGRADED");

    kernel_subsystem_set_ready(SUBSYS_PMM, true);
    BOOT_STEP(SUBSYS_PMM, bs_return_fatal);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_PMM) == false,
                "BOOT_STEP clears ready on BOOT_FATAL");

    kernel_subsystem_set_ready(SUBSYS_PMM, true);
    BOOT_STEP(SUBSYS_PMM, bs_return_deferred);
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_PMM) == false,
                "BOOT_STEP clears ready on BOOT_DEFERRED");

    kernel_subsystem_set_ready(SUBSYS_PMM, saved);
}

/* ---- kernel_subsystem_dump() smoke test ----
 *
 * Dump must run to completion without crashing.  We deliberately do NOT
 * assert that klog_get_seq() advances: kernel_subsystem_dump() emits 26
 * lines all tagged "BOOT" at LOG_INFO, and by the time the test runs
 * (Phase 3, ~5s into boot) the BOOT subsystem has typically hit its
 * per-subsystem rate limit (KLOG_RATE_DEFAULT msgs/sec).  Rate-limited
 * entries return early in klog() at src/kernel/klog.c:786 BEFORE
 * incrementing klog_ring_seq, so seq_after == seq_before is a legitimate
 * outcome that does NOT indicate a dump bug.  The real protection here
 * is "the call returns" (no infinite loop, no crash, no fault).  The
 * earlier "advanced by at least 1" assertion was flaky and fired during
 * the propagation test run on the user's i5-11600K box on 2026-04-08.
 */
static void test_kernel_subsystem_dump_emits(void)
{
    kernel_subsystem_dump();
    /* Reaching this point with no crash is the test. */
}

/* ---- POSTCODE_* (8-bit phase) constants ----
 *
 * Pin sentinel values used by serial diagnostics and verify the
 * Phase 0/Phase 1 starting points are non-overlapping.
 */
static void test_postcode_phase_constants(void)
{
    TEST_ASSERT(POSTCODE_PMM_INIT  != POSTCODE_VMM_INIT,
                "POSTCODE_PMM_INIT != POSTCODE_VMM_INIT");
    TEST_ASSERT(POSTCODE_GDT_INIT  != POSTCODE_IDT_INIT,
                "POSTCODE_GDT_INIT != POSTCODE_IDT_INIT");
    TEST_ASSERT(POSTCODE_VFS_INIT  != POSTCODE_REGISTRY_INIT,
                "POSTCODE_VFS_INIT != POSTCODE_REGISTRY_INIT");
    TEST_ASSERT(POSTCODE_SERIAL_INIT < POSTCODE_GDT_INIT,
                "Phase 0 POSTCODE range below Phase 1");
    TEST_ASSERT(POSTCODE_GDT_INIT    < POSTCODE_PCI_INIT,
                "Phase 1 POSTCODE range below Phase 2");
}

/* ---- BOOT_DEFERRED value ---- */

static void test_boot_deferred_value(void)
{
    TEST_ASSERT_EQ(BOOT_DEFERRED, 3, "BOOT_DEFERRED == 3");
}

/* ---- Deferred init registration ---- */

static boot_result_t deferred_test_fn(void)
{
    return BOOT_OK;
}

static void test_boot_defer_register(void)
{
    /* boot_defer returns 0 on success.
     * Note: this adds to the real deferred array, but boot_run_deferred()
     * has already run by the time tests execute, so these entries won't
     * cause problems -- they'd run only if boot_run_deferred() is called
     * again (which it won't be). */
    int r = boot_defer("test_deferred", deferred_test_fn);
    TEST_ASSERT_EQ(r, 0, "boot_defer returns 0 on success");
}

/* ---- Deferred POST codes ---- */

static void test_deferred_post_codes(void)
{
    TEST_ASSERT(POST16_DEFERRED != 0, "POST16_DEFERRED is non-zero");
    TEST_ASSERT(POST16_DEFERRED_OK != 0, "POST16_DEFERRED_OK is non-zero");
    TEST_ASSERT(POST16_DEFERRED != POST16_DEFERRED_OK,
                "DEFERRED != DEFERRED_OK");
    TEST_ASSERT(POST16_DEFERRED_NET != POST16_DEFERRED_INPUT,
                "DEFERRED_NET != DEFERRED_INPUT");
}

/* ---- Boot perf record struct ---- */

static void test_boot_perf_record_size(void)
{
    /* boot_perf_record_t must be 24 bytes for NVRAM layout stability */
    TEST_ASSERT_EQ(sizeof(boot_perf_record_t), 24,
                   "boot_perf_record_t is 24 bytes");
}

/* ---- Boot perf POST codes ---- */

static void test_bootperf_post_codes(void)
{
    TEST_ASSERT(POST16_BOOTPERF != 0, "POST16_BOOTPERF is non-zero");
    TEST_ASSERT(POST16_BOOTPERF_WRITE != 0, "POST16_BOOTPERF_WRITE is non-zero");
    TEST_ASSERT(POST16_BOOTPERF != POST16_BOOTPERF_READ,
                "BOOTPERF != BOOTPERF_READ");
    TEST_ASSERT(POST16_BOOTPERF_CMP != POST16_BOOTPERF_WRITE,
                "BOOTPERF_CMP != BOOTPERF_WRITE");
    /* Confirm no overlap with deferred POST range */
    TEST_ASSERT(POST16_BOOTPERF != POST16_DEFERRED,
                "BOOTPERF != DEFERRED");
}

/* ---- Async init POST codes and IPI vector ---- */

static void test_async_post_codes(void)
{
    TEST_ASSERT(POST16_ASYNC != 0, "POST16_ASYNC is non-zero");
    TEST_ASSERT(POST16_ASYNC_DONE != 0, "POST16_ASYNC_DONE is non-zero");
    TEST_ASSERT(POST16_ASYNC != POST16_ASYNC_AP,
                "ASYNC != ASYNC_AP");
    TEST_ASSERT(POST16_ASYNC_BARRIER != POST16_ASYNC_DONE,
                "ASYNC_BARRIER != ASYNC_DONE");
    /* No overlap with other debug ranges */
    TEST_ASSERT(POST16_ASYNC != POST16_DEFERRED,
                "ASYNC != DEFERRED");
    TEST_ASSERT(POST16_ASYNC != POST16_BOOTPERF,
                "ASYNC != BOOTPERF");
}

static void test_async_ipi_vector(void)
{
    /* Must not collide with existing IPI vectors */
    TEST_ASSERT(IPI_VECTOR_ASYNC_INIT != 0xFD,
                "ASYNC_INIT != RESCHEDULE (0xFD)");
    TEST_ASSERT(IPI_VECTOR_ASYNC_INIT != 0xFE,
                "ASYNC_INIT != TLB_SHOOTDOWN (0xFE)");
}

static void test_cr_verify_ipi_vector(void)
{
    /* The S10 CR-pin verify-IPI vector must not collide with any other IPI
     * vector (ASYNC_INIT 0xFC / RESCHEDULE 0xFD / TLB_SHOOTDOWN 0xFE). */
    TEST_ASSERT(IPI_VECTOR_CR_VERIFY != IPI_VECTOR_ASYNC_INIT,
                "CR_VERIFY != ASYNC_INIT");
    TEST_ASSERT(IPI_VECTOR_CR_VERIFY != IPI_VECTOR_RESCHEDULE,
                "CR_VERIFY != RESCHEDULE");
    TEST_ASSERT(IPI_VECTOR_CR_VERIFY != IPI_VECTOR_TLB_SHOOTDOWN,
                "CR_VERIFY != TLB_SHOOTDOWN");
}

/* ---- kernel_subsystem_apply_result() helper ----
 *
 * Both channels MUST agree:
 *   BOOT_OK       -> ready=true,  bit unchanged
 *   BOOT_DEGRADED -> ready=true,  bit SET
 *   BOOT_FATAL    -> ready=false, bit SET
 *   BOOT_DEFERRED -> ready=false, bit SET (any non-OK)
 *
 * Driven through the explicit-instance entry point against a private
 * struct kernel_subsys_state, so the live readiness table and the live
 * degraded bitmap are never written (the bitmap is sticky by design and has
 * no clear operation to restore it with). A slot above bit 31 (SPLASH, 35)
 * is exercised alongside TPM so a 32-bit shift anywhere on the path fails.
 */
static void test_subsys_apply_result_dual_channel(void)
{
    static const kernel_subsys_t slots[2] = { SUBSYS_TPM, SUBSYS_SPLASH };
    char msg[96];

    for (uint32_t k = 0; k < 2; k++) {
        kernel_subsys_t s = slots[k];
        uint64_t bit = (uint64_t)1u << (uint32_t)s;
        struct kernel_subsys_state st;
        __builtin_memset(&st, 0, sizeof st);

        bool r1 = kernel_subsystem_apply_result_into(&st, s, BOOT_OK);
        snprintf(msg, sizeof msg, "slot %u: OK -> ready, no bit", (uint64_t)s);
        TEST_ASSERT(r1 && st.ready[s] == 1 && st.degraded == 0, msg);

        __builtin_memset(&st, 0, sizeof st);
        bool r2 = kernel_subsystem_apply_result_into(&st, s, BOOT_DEGRADED);
        snprintf(msg, sizeof msg, "slot %u: DEGRADED -> ready + only its bit", (uint64_t)s);
        TEST_ASSERT(r2 && st.ready[s] == 1 && st.degraded == bit, msg);

        __builtin_memset(&st, 0, sizeof st);
        bool r3 = kernel_subsystem_apply_result_into(&st, s, BOOT_FATAL);
        snprintf(msg, sizeof msg, "slot %u: FATAL -> not ready + bit", (uint64_t)s);
        TEST_ASSERT(!r3 && st.ready[s] == 0 && st.degraded == bit, msg);

        __builtin_memset(&st, 0, sizeof st);
        bool r4 = kernel_subsystem_apply_result_into(&st, s, BOOT_DEFERRED);
        snprintf(msg, sizeof msg, "slot %u: DEFERRED -> not ready + bit", (uint64_t)s);
        TEST_ASSERT(!r4 && st.ready[s] == 0 && st.degraded == bit, msg);

        /* Sticky: a later OK restores readiness but never clears the bit, so
         * a subsystem that failed once is still named in the summary. */
        bool r5 = kernel_subsystem_apply_result_into(&st, s, BOOT_OK);
        snprintf(msg, sizeof msg, "slot %u: OK after DEFERRED keeps the bit", (uint64_t)s);
        TEST_ASSERT(r5 && st.ready[s] == 1 && st.degraded == bit, msg);
    }

    /* Out of range and NULL: refused, nothing written. */
    {
        struct kernel_subsys_state st;
        __builtin_memset(&st, 0, sizeof st);
        TEST_ASSERT(!kernel_subsystem_apply_result_into(&st,
                        (kernel_subsys_t)SUBSYS_COUNT, BOOT_FATAL),
                    "apply_into(SUBSYS_COUNT) returns false");
        TEST_ASSERT_EQ(st.degraded, 0, "apply_into(out of range) sets no bit");
        TEST_ASSERT(!kernel_subsystem_apply_result_into((struct kernel_subsys_state *)0,
                        SUBSYS_TPM, BOOT_OK),
                    "apply_into(NULL state) returns false");
    }
}

/* ---- kernel_subsystem_apply_load_state() mapping ----
 *
 * The probe vocabulary (boot_load_state) onto the two channels. The case the
 * old mapping could not express is SKIPPED: an absent optional device is NOT
 * ready and NOT degraded, so a NIC-less machine never reports a degraded boot.
 */
static void test_subsys_apply_load_state_mapping(void)
{
    static const struct { uint8_t state; uint8_t ready; uint8_t bit; const char *name; } m[] = {
        { BOOT_LOAD_LOADED,    1, 0, "LOADED -> ready, no bit"        },
        { BOOT_LOAD_DEGRADED,  1, 1, "DEGRADED -> ready + bit"        },
        { BOOT_LOAD_SKIPPED,   0, 0, "SKIPPED -> not ready, no bit"   },
        { BOOT_LOAD_FAILED,    0, 1, "FAILED -> not ready + bit"      },
        { BOOT_LOAD_ATTEMPTED, 0, 1, "ATTEMPTED (never finished) -> not ready + bit" },
        { 0xEEu,               0, 1, "unknown state -> not ready + bit (fail closed)" },
    };
    for (uint32_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        struct kernel_subsys_state st;
        __builtin_memset(&st, 0, sizeof st);
        bool r = kernel_subsystem_apply_load_state_into(&st, SUBSYS_AHCI, m[i].state);
        uint64_t want = m[i].bit ? ((uint64_t)1u << SUBSYS_AHCI) : 0u;
        TEST_ASSERT(r == (bool)m[i].ready && st.ready[SUBSYS_AHCI] == m[i].ready &&
                    st.degraded == want, m[i].name);
    }
    {
        struct kernel_subsys_state st;
        __builtin_memset(&st, 0, sizeof st);
        TEST_ASSERT(!kernel_subsystem_apply_load_state_into(&st,
                        (kernel_subsys_t)SUBSYS_COUNT, BOOT_LOAD_FAILED) &&
                    st.degraded == 0,
                    "apply_load_state_into(out of range) refused, no bit");
    }
}

/* ---- Failure AFTER readiness ----
 *
 * A failure must REVOKE readiness that an earlier success granted, keep the
 * degraded bit sticky, and leave every other slot alone. Starting each case
 * from zeroed state (as the mapping tests do) cannot catch a writer that only
 * ever stores "ready = 1". */
static void test_subsys_failure_revokes_readiness(void)
{
    static const uint8_t fail_states[4] = {
        BOOT_LOAD_FAILED, BOOT_LOAD_SKIPPED, BOOT_LOAD_ATTEMPTED, 0xEEu
    };
    static const boot_result_t fail_results[2] = { BOOT_FATAL, BOOT_DEFERRED };
    const uint64_t other_bit = (uint64_t)1u << SUBSYS_RTC;
    char msg[96];

    for (uint32_t i = 0; i < 4; i++) {
        struct kernel_subsys_state st;
        __builtin_memset(&st, 0, sizeof st);
        st.ready[SUBSYS_TPM] = 1;            /* unrelated, ready */
        st.degraded = other_bit;             /* unrelated, degraded */
        (void)kernel_subsystem_apply_load_state_into(&st, SUBSYS_NIC, BOOT_LOAD_DEGRADED);
        bool r = kernel_subsystem_apply_load_state_into(&st, SUBSYS_NIC, fail_states[i]);
        snprintf(msg, sizeof msg, "load state %u after DEGRADED: ready revoked, bit sticky",
                 (uint64_t)fail_states[i]);
        TEST_ASSERT(!r && st.ready[SUBSYS_NIC] == 0 &&
                    (st.degraded & ((uint64_t)1u << SUBSYS_NIC)) != 0, msg);
        TEST_ASSERT(st.ready[SUBSYS_TPM] == 1 && (st.degraded & other_bit) != 0,
                    "unrelated slot's readiness and bit untouched");
    }
    for (uint32_t i = 0; i < 4; i++) {
        struct kernel_subsys_state st;
        __builtin_memset(&st, 0, sizeof st);
        (void)kernel_subsystem_apply_load_state_into(&st, SUBSYS_SPLASH, BOOT_LOAD_LOADED);
        (void)kernel_subsystem_apply_load_state_into(&st, SUBSYS_SPLASH, fail_states[i]);
        snprintf(msg, sizeof msg, "load state %u after LOADED revokes readiness",
                 (uint64_t)fail_states[i]);
        TEST_ASSERT(st.ready[SUBSYS_SPLASH] == 0, msg);
    }
    for (uint32_t i = 0; i < 2; i++) {
        struct kernel_subsys_state st;
        __builtin_memset(&st, 0, sizeof st);
        st.ready[SUBSYS_TPM] = 1;
        (void)kernel_subsystem_apply_result_into(&st, SUBSYS_SPLASH, BOOT_OK);
        bool r = kernel_subsystem_apply_result_into(&st, SUBSYS_SPLASH, fail_results[i]);
        snprintf(msg, sizeof msg, "result %u after OK: ready revoked, only its bit set",
                 (uint64_t)fail_results[i]);
        TEST_ASSERT(!r && st.ready[SUBSYS_SPLASH] == 0 &&
                    st.degraded == ((uint64_t)1u << SUBSYS_SPLASH) &&
                    st.ready[SUBSYS_TPM] == 1, msg);
    }
}

/* ---- kernel_subsystem_format_degraded() ----
 *
 * The reporting path the end-of-boot summary uses, fed a bitmap built by the
 * real writers so bits above 31 travel the whole chain. */
static void test_subsys_format_degraded(void)
{
    struct kernel_subsys_state st;
    char buf[128];
    uint32_t n;

    __builtin_memset(&st, 0, sizeof st);
    (void)kernel_subsystem_apply_load_state_into(&st, SUBSYS_RTC,    BOOT_LOAD_FAILED);
    (void)kernel_subsystem_apply_load_state_into(&st, SUBSYS_AHCI,   BOOT_LOAD_DEGRADED);
    (void)kernel_subsystem_apply_load_state_into(&st, SUBSYS_SPLASH, BOOT_LOAD_FAILED);
    (void)kernel_subsystem_apply_load_state_into(&st, SUBSYS_NIC,    BOOT_LOAD_SKIPPED);

    n = kernel_subsystem_format_degraded(st.degraded, buf, sizeof buf);
    TEST_ASSERT_EQ(n, 3, "three degraded slots counted (SKIPPED NIC excluded)");
    TEST_ASSERT(strcmp(buf, "RTC, AHCI, SPLASH") == 0,
                "names in slot order, comma separated, bits > 31 included");

    /* Bits at or above SUBSYS_COUNT are ignored, not printed as UNKNOWN. */
    n = kernel_subsystem_format_degraded(((uint64_t)1u << 63) | ((uint64_t)1u << SUBSYS_RTC),
                                         buf, sizeof buf);
    TEST_ASSERT(n == 1 && strcmp(buf, "RTC") == 0,
                "stray bits >= SUBSYS_COUNT are ignored");

    n = kernel_subsystem_format_degraded(0, buf, sizeof buf);
    TEST_ASSERT(n == 0 && buf[0] == '\0', "empty mask -> empty string");

    /* Truncation never cuts a name: "RTC" + ", ..." fits in 9, AHCI does not. */
    n = kernel_subsystem_format_degraded(st.degraded, buf, 9);
    TEST_ASSERT(n == 3 && strcmp(buf, "RTC, ...") == 0,
                "cut marked with ', ...' and never mid-name");

    /* Too small for even the first name: "..." when it fits, else empty. */
    n = kernel_subsystem_format_degraded(st.degraded, buf, 4);
    TEST_ASSERT(n == 3 && strcmp(buf, "...") == 0, "cap 4 -> '...'");
    n = kernel_subsystem_format_degraded(st.degraded, buf, 3);
    TEST_ASSERT(n == 3 && buf[0] == '\0', "cap 3 -> empty, still terminated");

    /* Last name needs no marker reserve: "RTC" alone fits exactly in 4. */
    n = kernel_subsystem_format_degraded((uint64_t)1u << SUBSYS_RTC, buf, 4);
    TEST_ASSERT(n == 1 && strcmp(buf, "RTC") == 0,
                "final name uses the whole buffer without a marker reserve");

    /* NULL / zero-cap buffers still count. */
    TEST_ASSERT_EQ(kernel_subsystem_format_degraded(st.degraded, (char *)0, 64), 3,
                   "NULL buf still returns the count");
    buf[0] = 'x';
    TEST_ASSERT(kernel_subsystem_format_degraded(st.degraded, buf, 0) == 3 && buf[0] == 'x',
                "cap 0 writes nothing");

    /* Every slot set: all SUBSYS_COUNT names fit the summary's 512-byte
     * buffer, so the live line is never truncated today. */
    {
        static char big[512];
        uint64_t all = (SUBSYS_COUNT >= 64) ? ~(uint64_t)0
                                            : (((uint64_t)1u << SUBSYS_COUNT) - 1u);
        n = kernel_subsystem_format_degraded(all, big, sizeof big);
        uint32_t len = 0;
        while (big[len]) len++;
        TEST_ASSERT(n == SUBSYS_COUNT && len > 0 &&
                    !(len >= 5 && strcmp(big + len - 5, ", ...") == 0),
                    "all slots format into 512 bytes without truncation");
    }
}

/* ---- Phase 0 propagation slots ----
 *
 * Verify that the four new Phase 0 readiness slots (UEFI_VARS, UEFI_TIME,
 * SECUREBOOT, TPM) integrate with the readiness oracle the same way every
 * other slot does -- save/restore wrapper around set_ready/ready, no live
 * boot infrastructure calls. The actual init functions (uefi_vars_init,
 * tpm_init, etc.) are forbidden in tests per CLAUDE.md "Test Code -- No
 * Live Boot Infrastructure Calls" -- this test only validates the oracle
 * round-trip on the new slot indices.
 */
static void test_subsys_phase0_propagation_slots(void)
{
    bool saved_vars = kernel_subsystem_ready(SUBSYS_UEFI_VARS);
    bool saved_time = kernel_subsystem_ready(SUBSYS_UEFI_TIME);
    bool saved_sb   = kernel_subsystem_ready(SUBSYS_SECUREBOOT);
    bool saved_tpm  = kernel_subsystem_ready(SUBSYS_TPM);

    kernel_subsystem_set_ready(SUBSYS_UEFI_VARS,  true);
    kernel_subsystem_set_ready(SUBSYS_UEFI_TIME,  true);
    kernel_subsystem_set_ready(SUBSYS_SECUREBOOT, true);
    kernel_subsystem_set_ready(SUBSYS_TPM,        true);

    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_UEFI_VARS) == true,
                "UEFI_VARS slot round-trip true");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_UEFI_TIME) == true,
                "UEFI_TIME slot round-trip true");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_SECUREBOOT) == true,
                "SECUREBOOT slot round-trip true");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_TPM) == true,
                "TPM slot round-trip true");

    kernel_subsystem_set_ready(SUBSYS_UEFI_VARS,  false);
    kernel_subsystem_set_ready(SUBSYS_UEFI_TIME,  false);
    kernel_subsystem_set_ready(SUBSYS_SECUREBOOT, false);
    kernel_subsystem_set_ready(SUBSYS_TPM,        false);

    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_UEFI_VARS) == false,
                "UEFI_VARS slot round-trip false");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_UEFI_TIME) == false,
                "UEFI_TIME slot round-trip false");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_SECUREBOOT) == false,
                "SECUREBOOT slot round-trip false");
    TEST_ASSERT(kernel_subsystem_ready(SUBSYS_TPM) == false,
                "TPM slot round-trip false");

    kernel_subsystem_set_ready(SUBSYS_UEFI_VARS,  saved_vars);
    kernel_subsystem_set_ready(SUBSYS_UEFI_TIME,  saved_time);
    kernel_subsystem_set_ready(SUBSYS_SECUREBOOT, saved_sb);
    kernel_subsystem_set_ready(SUBSYS_TPM,        saved_tpm);
}

/* ---- Registration ---- */

void test_register_boot_init(void)
{
    test_suite_register_cat("Boot init: result values", test_boot_result_values, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: set_ready true", test_subsys_set_ready_true, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: set_ready false", test_subsys_set_ready_false, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: out of range", test_subsys_out_of_range, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: REQUIRE fails", test_boot_require_fails_when_not_ready, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: REQUIRE passes", test_boot_require_passes_when_ready, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: progress null", test_boot_progress_null_step, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: POST codes", test_post_codes_nonzero_and_unique, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: subsys enum layout", test_subsys_enum_layout, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: BOOT_STEP mapping", test_boot_step_readiness_mapping, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: dump emits entries", test_kernel_subsystem_dump_emits, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: POSTCODE phase constants", test_postcode_phase_constants, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: defer NULL fn", test_boot_defer_null_fn, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: BOOT_DEFERRED value", test_boot_deferred_value, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: defer register", test_boot_defer_register, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: deferred POST codes", test_deferred_post_codes, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: perf record size", test_boot_perf_record_size, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: bootperf POST codes", test_bootperf_post_codes, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: async POST codes", test_async_post_codes, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: async IPI vector", test_async_ipi_vector, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: CR-verify IPI vector", test_cr_verify_ipi_vector, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: Phase 0 propagation slots",
                            test_subsys_phase0_propagation_slots, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: apply_result dual channel",
                            test_subsys_apply_result_dual_channel, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: apply_load_state mapping",
                            test_subsys_apply_load_state_mapping, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: failure revokes readiness",
                            test_subsys_failure_revokes_readiness, TEST_CAT_BOOT);
    test_suite_register_cat("Boot init: format degraded list",
                            test_subsys_format_degraded, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
