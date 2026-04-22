/* ============================================================================
 * test_desktop_reset.c -- Per-test desktop-state reset for
 * TEST_CAT_DESKTOP suites.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test_desktop_reset.h"
#include "kernel/drivers/keyboard.h"
#include "desktop/terminal.h"
#include "desktop/wm.h"
#include "main/main_internal.h"

void test_desktop_reset(void)
{
    /* 1. Window Manager state: synthetic windows, focus, deferred
     *    close queue (wm_test_reset also clears pending_close_handle
     *    per the Codex-fixed regression in that seam). */
    wm_test_reset();

    /* 2. Keyboard driver state: modifier latches + input ring. */
    keyboard_reset_state();

    /* 3. Terminal state: grid + handle + input ring.
     *    Prior suite may have force-opened the terminal, written to
     *    term_cells via terminal_puts, and left characters in the
     *    input ring. A bare trygetchar drain is NOT enough -- an
     *    open terminal leaks into later keyboard_inject_scancode()
     *    calls (printable keys route to terminal_key_input when
     *    terminal_is_open() is true), and stale term_cells content
     *    can leak into future fb_snapshot / buffer-contains checks.
     *    Codex [H] section 15 review required closing + grid-clear
     *    in addition to the ring drain.
     *
     *    Cycle force_open -> force_close to reset the grid (force_open
     *    clears term_cells to spaces) and leave the terminal closed
     *    so later keyboard_inject keys land in the keyboard buffer. */
    terminal_test_force_open();
    terminal_test_force_close();
    while (terminal_trygetchar() != 0) {
        /* drain any residual input-ring bytes */
    }

    /* 4. Compositor test seed + headless flag. Tests that toggle
     *    headless on for step_frames should toggle it back, but an
     *    explicit reset here guards against leaks when a test exits
     *    early via a failed assertion. */
    compositor_set_test_seed(0);
    compositor_set_headless(0);
}

void test_desktop_reset_action(void *ctx)
{
    (void)ctx;
    test_desktop_reset();
}

#endif /* KERNEL_TESTS */
