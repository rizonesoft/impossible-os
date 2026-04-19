/* ============================================================================
 * klog_suppress.h -- Block-scoped klog level demotion for tests.
 *
 * TEST_KLOG_SUPPRESS(subsystem) raises the subsystem's minimum log level
 * to LOG_FATAL (effectively silencing everything below) for the
 * remainder of the current test suite, then restores the prior level
 * when the §7 action drain fires at suite exit.
 *
 * Purpose: tests that exercise error paths (validators, allocator
 * failure rollback, bad-input rejection) otherwise emit `[FAIL]
 * subsys: ...` lines onto serial. Diagnose-serial-log then has to
 * hand-classify each line as NOISE per-test, which is brittle. With
 * this macro the [FAIL] lines never reach the ring buffer; the
 * validator remains loud on real boot (where the level is LOG_ERROR
 * or LOG_INFO) because the test-time demotion only lasts the suite.
 *
 * Usage:
 *   static void test_xyz_negative(void) {
 *       TEST_KLOG_SUPPRESS("boot");
 *       TEST_ASSERT(validator_rejects_bad_input(&bad), "rejected");
 *       // no [FAIL] boot: ... line on the serial log; at suite exit
 *       // the §7 action drain restores the prior level.
 *   }
 *
 * Implementation:
 *   - On entry, the macro kmalloc's a small record holding the
 *     subsystem name + its current level (via klog_get_level).
 *   - Calls klog_set_level(subsystem, LOG_FATAL) so every subsequent
 *     klog call at LOG_ERROR/WARN/INFO/DEBUG is dropped before hitting
 *     the ring or serial.
 *   - Registers test_add_action(klog_suppress_restore, rec) so the
 *     level is restored + the record freed on suite-exit action drain,
 *     even if the test hit TEST_ASSERT and returned early.
 *   - If kmalloc fails (OOM or fault-injected) the macro is a silent
 *     no-op: the test still runs, but the error-path klog lines leak
 *     through. Acceptable failure mode for a noise-reduction aid.
 *
 * KERNEL_TESTS-gated -- release builds drop the translation unit
 * entirely. Must NOT be called during a drain (test_add_action rejects
 * re-entry in that window -- see §7 draining flag contract).
 * ============================================================================ */

#pragma once

#ifdef KERNEL_TESTS

#include "kernel/types.h"
#include "kernel/klog.h"           /* klog_set_level, klog_get_level, LOG_FATAL */
#include "kernel/mm/heap.h"        /* kmalloc */
#include "kernel/test/test.h"      /* test_add_action */

/* Internal: the record that TEST_KLOG_SUPPRESS kmalloc's and passes
 * as ctx to klog_suppress_restore. Exposed so the struct layout is
 * visible to the macro expansion but callers should treat it as
 * opaque. */
struct klog_suppress_record {
    const char *subsystem;
    log_level_t prev_level;
    uint8_t     had_override;  /* 1 = restore via klog_set_level; 0 = via klog_remove_override */
    uint8_t     _pad[3];
};

/* Restore the saved level + free the record. Registered via
 * test_add_action; fires on suite-exit action drain. NULL ctx is a
 * safe no-op (consistent with test_scratch_free). */
void klog_suppress_restore(void *ctx);

/* Internal: allocate a record, snapshot the current level, demote to
 * LOG_FATAL, and register the restore action. Returns 1 on success,
 * 0 if kmalloc or test_add_action failed (silent no-op -- the caller
 * continues with the subsystem un-suppressed). */
int klog_suppress_begin(const char *subsystem);

/* TEST_KLOG_SUPPRESS -- declare the demotion at any point inside a
 * test body. Takes no lock, allocates a small record (<32 bytes) via
 * kmalloc, and returns. Restoration is automatic at suite exit. */
#define TEST_KLOG_SUPPRESS(subsystem) \
    ((void)klog_suppress_begin((subsystem)))

#endif /* KERNEL_TESTS */
