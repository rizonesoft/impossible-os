/* ============================================================================
 * test_desktop_reset.h -- Per-test desktop-state isolation for
 * TEST_CAT_DESKTOP suites (the desktop UI test framework's isolation
 * section).
 *
 * Every TEST_CAT_DESKTOP suite exercises shared global state:
 *   * the WM `windows[]` array + `focused_window` + pending-close queue
 *   * the keyboard input ring + modifier latches
 *   * the terminal input ring + `term_cells[]` grid
 *   * the compositor test seed + headless flag + frame-stats seqlock
 *
 * A suite that forgets to reset any of those leaks state into the next
 * suite and produces order-dependent flakes. `test_desktop_reset()`
 * zeroes ALL of the above in one call so a suite registers exactly
 * one action and stays isolated.
 *
 * Typical use:
 *
 *   static void test_my_desktop_thing(void)
 *   {
 *       test_add_action(test_desktop_reset_action, NULL);
 *       wm_test_install_window(...);
 *       ...
 *   }
 *
 * The `action` wrapper matches the signature `test_add_action()` expects
 * (void *ctx); the plain `test_desktop_reset()` is the direct call.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

#ifdef KERNEL_TESTS

/* Zero every piece of shared desktop state a TEST_CAT_DESKTOP suite
 * might have touched. Idempotent; safe to call multiple times. */
void test_desktop_reset(void);

/* `test_add_action`-compatible wrapper around `test_desktop_reset()`.
 * `ctx` is ignored. Use this when registering the reset via
 * test_add_action so it fires after the suite body completes:
 *
 *   test_add_action(test_desktop_reset_action, (void *)0);
 */
void test_desktop_reset_action(void *ctx);

#endif /* KERNEL_TESTS */
