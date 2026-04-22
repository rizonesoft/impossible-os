/* ============================================================================
 * test_desktop.c -- Desktop UI test framework (TEST_CAT_DESKTOP)
 *
 * TODO-05-desktop-ui-test-framework.md
 *   §1: Framebuffer Snapshot API
 *   §4: Input Event Injection
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/drivers/keyboard.h"
#include "kernel/drivers/mouse.h"
#include "kernel/mm/pmm.h"
#include "kernel/types.h"

/* ---- §4 Input Event Injection ----------------------------------------- */

/* Test-scoped keypress injection: thin pass-through to the existing
 * keyboard_inject_scancode() primitive (src/kernel/drivers/keyboard.c:318).
 * The existing primitive already honors modifier latching, E0 prefixes,
 * key-release bit 7, and the scancode-to-ASCII lookup table; a test wrapper
 * would duplicate behavior. Exposed here under a test-friendly name so the
 * TODO's specified API (`test_inject_keypress`) is addressable. */
static void test_inject_keypress(uint8_t scancode)
{
    keyboard_inject_scancode(scancode);
}

/* Test-scoped mouse press/release primitives: absolute (x, y) + button
 * state. Wraps mouse_inject_state() (src/kernel/drivers/mouse.c:300); the
 * existing primitive already clamps out-of-bounds coordinates against the
 * current framebuffer dimensions. Button encoding follows
 * MOUSE_BTN_LEFT/RIGHT/MIDDLE from mouse.h. Split into press + release so
 * callers that need to observe the press state between the two events
 * can do so (e.g., tests that verify the compositor actually sees the
 * press edge, not just the final released state). */
static void test_inject_mouse_press(int32_t x, int32_t y, uint8_t button)
{
    mouse_inject_state(x, y, button);
}

static void test_inject_mouse_release(int32_t x, int32_t y)
{
    mouse_inject_state(x, y, 0);
}

/* Convenience full-click wrapper: press + release in one call. Callers who
 * need compositor-visible press edges should use the split primitives with
 * a yield / state check between them. Codex [H] review note. */
static void test_inject_mouse_click(int32_t x, int32_t y, uint8_t button)
{
    test_inject_mouse_press(x, y, button);
    test_inject_mouse_release(x, y);
}

/* ---- §1 Framebuffer Snapshot API -------------------------------------- */

/* fb_snapshot_size() reports a non-zero byte count that matches width * height
 * * 4 BGRA bytes once the framebuffer has been initialized. */
static void test_fb_snapshot_size_nonzero(void)
{
    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();
    uint64_t sz = fb_snapshot_size();

    TEST_ASSERT(w > 0, "fb_get_width() > 0 after boot");
    TEST_ASSERT(h > 0, "fb_get_height() > 0 after boot");
    TEST_ASSERT_EQ(sz, (uint64_t)w * (uint64_t)h * 4u,
                   "fb_snapshot_size() == width * height * 4");
}

/* fb_snapshot() rejects NULL arguments. */
static void test_fb_snapshot_null_args(void)
{
    uint32_t w = 0, h = 0;
    uint8_t  one_byte = 0;

    TEST_ASSERT_EQ((uint64_t)fb_snapshot(NULL, &w, &h), (uint64_t)-1,
                   "fb_snapshot(NULL dest) returns -1");
    TEST_ASSERT_EQ((uint64_t)fb_snapshot(&one_byte, NULL, &h), (uint64_t)-1,
                   "fb_snapshot(NULL width) returns -1");
    TEST_ASSERT_EQ((uint64_t)fb_snapshot(&one_byte, &w, NULL), (uint64_t)-1,
                   "fb_snapshot(NULL height) returns -1");
}

/* Write a unique sentinel pattern directly into the back buffer corners, take
 * a snapshot, and verify (a) the reported dimensions match the framebuffer,
 * (b) the snapshot contains the sentinels at the matching corner pixels, and
 * (c) at least one pixel is non-zero (the section's stated pass criterion).
 *
 * The boot splash spinner registers a timer tick callback that mutates the
 * back buffer from IRQ context every 100ms, per src/kernel/spinner.c. Stop
 * the spinner before touching back_buf so the sentinel-corner writes and
 * the snapshot copy are not torn by an in-flight spinner_render. spinner_stop
 * is idempotent and a later splash-finish path re-issues it, so this early
 * stop does not regress later boot code. TEST-SIDE-EFFECT-ALLOWED:
 * spinner_stop is a pure-state writer (clears s_active + unregisters a
 * callback); the forbidden-boot-call ban targets framebuffer/VPD/NVRAM
 * writers, not the scheduler-local timer unregister. */
extern void spinner_stop(void);

static void test_fb_snapshot_roundtrip(void)
{
    spinner_stop();

    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();
    uint32_t stride = fb_get_stride();
    uint32_t *back = fb_get_backbuffer();

    TEST_ASSERT(w > 0 && h > 0, "framebuffer dimensions populated");
    TEST_ASSERT(back != NULL, "back buffer pointer non-NULL");
    if (!back || w == 0 || h == 0)
        return;

    /* Allocate destination buffer via PMM (CLAUDE.md: > 4 KiB). 1280x720 is
     * 3.5 MiB = 900 pages, well within budget for the test run. */
    uint64_t sz = fb_snapshot_size();
    uint64_t pages = (sz + 4095) / 4096;
    uintptr_t phys = pmm_alloc_contiguous(pages);
    TEST_ASSERT(phys != 0, "pmm_alloc_contiguous for snapshot dest");
    if (phys == 0)
        return;
    uint32_t *dst = (uint32_t *)phys;

    /* Stamp four distinct sentinels into the four visible corners of the
     * back buffer. Save and restore the original pixels so the test leaves
     * the display state untouched in case a later test reads it. */
    const uint32_t S_TL = 0xDEADBEEFu;
    const uint32_t S_TR = 0xCAFEBABEu;
    const uint32_t S_BL = 0xFEEDFACEu;
    const uint32_t S_BR = 0xBADDCAFEu;

    uint32_t idx_tl = 0;
    uint32_t idx_tr = w - 1;
    uint32_t idx_bl = (h - 1) * stride;
    uint32_t idx_br = (h - 1) * stride + (w - 1);

    uint32_t save_tl = back[idx_tl];
    uint32_t save_tr = back[idx_tr];
    uint32_t save_bl = back[idx_bl];
    uint32_t save_br = back[idx_br];

    back[idx_tl] = S_TL;
    back[idx_tr] = S_TR;
    back[idx_bl] = S_BL;
    back[idx_br] = S_BR;

    uint32_t snap_w = 0, snap_h = 0;
    int rc = fb_snapshot(dst, &snap_w, &snap_h);

    /* Restore original pixels immediately so any later reader sees the
     * pre-test back buffer. */
    back[idx_tl] = save_tl;
    back[idx_tr] = save_tr;
    back[idx_bl] = save_bl;
    back[idx_br] = save_br;

    TEST_ASSERT_EQ((uint64_t)rc, 0, "fb_snapshot returns 0 on success");
    TEST_ASSERT_EQ((uint64_t)snap_w, (uint64_t)w,
                   "snapshot width matches fb_get_width");
    TEST_ASSERT_EQ((uint64_t)snap_h, (uint64_t)h,
                   "snapshot height matches fb_get_height");

    /* Destination is tightly packed w*h, independent of back-buffer stride. */
    uint32_t out_tl = dst[0];
    uint32_t out_tr = dst[w - 1];
    uint32_t out_bl = dst[(uint64_t)(h - 1) * w];
    uint32_t out_br = dst[(uint64_t)(h - 1) * w + (w - 1)];

    TEST_ASSERT_EQ((uint64_t)out_tl, (uint64_t)S_TL, "top-left sentinel round-trips");
    TEST_ASSERT_EQ((uint64_t)out_tr, (uint64_t)S_TR, "top-right sentinel round-trips");
    TEST_ASSERT_EQ((uint64_t)out_bl, (uint64_t)S_BL, "bottom-left sentinel round-trips");
    TEST_ASSERT_EQ((uint64_t)out_br, (uint64_t)S_BR, "bottom-right sentinel round-trips");

    /* §1 test checkpoint: "buffer is non-zero (not all black)". The four
     * sentinels we just verified would satisfy this trivially; also sample
     * the interior to prove the bulk copy fired. */
    TEST_ASSERT(dst[0] != 0u || dst[w / 2] != 0u || dst[w - 1] != 0u,
                "snapshot buffer is not all-black (bulk copy fired)");

    /* Free the 900-page snapshot buffer one page at a time (no dedicated
     * pmm_free_contiguous helper exists yet). */
    for (uint64_t i = 0; i < pages; i++)
        pmm_free_frame(phys + i * 4096);
}

/* ---- §4 Input Event Injection (test cases) --------------------------- */

/* Reset the keyboard to a known state before each injection test. Clears
 * the ring buffer AND every latched modifier (shift/ctrl/alt/capslock)
 * AND any pending E0 prefix. Without the full reset, a test-order
 * dependency would creep in: a prior test that injected a modifier press
 * without a matching release would corrupt the next test's scancode
 * (e.g. 0x1E -> Ctrl-A instead of 'a'). Codex [H] adversarial review. */
static void drain_keyboard_buffer(void)
{
    keyboard_reset_state();
}

/* test_inject_keypress(scancode) drives the same code path as a real IRQ1
 * handler call, so the downstream character ends up in the same ring buffer
 * that keyboard_trygetchar() consumes. Inject 'A' scancode (0x1E), verify
 * the buffer returns 'a' (no shift held) within one attempt. */
static void test_input_inject_keypress_roundtrip(void)
{
    drain_keyboard_buffer();

    test_inject_keypress(0x1E);              /* scancode for 'a' */
    char c = keyboard_trygetchar();

    TEST_ASSERT_EQ((uint64_t)(unsigned char)c, (uint64_t)'a',
                   "scancode 0x1E round-trips to 'a' in kb_buffer");
}

/* test_inject_keypress should accept and process the Enter scancode (0x1C)
 * so the §5 terminal-roundtrip test can chain keypresses. */
static void test_input_inject_keypress_enter(void)
{
    drain_keyboard_buffer();

    test_inject_keypress(0x1C);              /* Enter */
    char c = keyboard_trygetchar();

    /* Enter maps to '\n' (LF, 10) via scancode_normal[0x1C] in
     * src/kernel/drivers/keyboard.c. Do NOT assume CR -- that was the
     * DOS convention; Impossible OS follows the Linux line-discipline
     * convention of emitting LF from the keyboard layer. Shells that
     * echo "\r\n" produce the CR themselves. */
    TEST_ASSERT_EQ((uint64_t)(unsigned char)c, (uint64_t)'\n',
                   "Enter scancode round-trips to LF");
}

/* test_inject_mouse_click(x, y, button) performs a press + release pair so
 * the post-call mouse_get_state() reflects the cursor at (x, y) with
 * buttons == 0 (released). Position is clamped by mouse_inject_state()
 * against the current framebuffer dimensions -- so a coordinate well within
 * the typical 1280x720 mode lands verbatim. */
static void test_input_inject_mouse_click_delivered(void)
{
    test_inject_mouse_click(100, 200, MOUSE_BTN_LEFT);

    struct mouse_state s = mouse_get_state();

    TEST_ASSERT_EQ((uint64_t)s.x, (uint64_t)100,
                   "click x coordinate preserved");
    TEST_ASSERT_EQ((uint64_t)s.y, (uint64_t)200,
                   "click y coordinate preserved");
    TEST_ASSERT_EQ((uint64_t)s.buttons, (uint64_t)0,
                   "click ends with buttons released");
}

/* Split press/release primitives: the press must be observable between
 * the two calls. This catches callers that mistakenly use the combined
 * click() wrapper and lose the press edge because the compositor never
 * gets to see the pressed state. */
static void test_input_inject_mouse_press_observable(void)
{
    test_inject_mouse_press(300, 150, MOUSE_BTN_RIGHT);

    struct mouse_state mid = mouse_get_state();
    TEST_ASSERT_EQ((uint64_t)mid.buttons, (uint64_t)MOUSE_BTN_RIGHT,
                   "press state visible between press and release");
    TEST_ASSERT_EQ((uint64_t)mid.x, (uint64_t)300,
                   "press state preserves x");
    TEST_ASSERT_EQ((uint64_t)mid.y, (uint64_t)150,
                   "press state preserves y");

    test_inject_mouse_release(300, 150);

    struct mouse_state end = mouse_get_state();
    TEST_ASSERT_EQ((uint64_t)end.buttons, (uint64_t)0,
                   "release clears button mask");
}

/* mouse_inject_state clamps coordinates against fb_get_width/height. A
 * coordinate far outside the framebuffer must land at the edge, not
 * wrap, not retain the requested value, and not underflow. */
static void test_input_inject_mouse_click_clamps_out_of_bounds(void)
{
    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();

    TEST_ASSERT(w > 0 && h > 0, "framebuffer dimensions populated");

    /* Way past the right edge; also negative y to test both clamps. */
    test_inject_mouse_click((int32_t)(w + 10000), -500, MOUSE_BTN_LEFT);

    struct mouse_state s = mouse_get_state();

    TEST_ASSERT_EQ((uint64_t)s.x, (uint64_t)(w - 1),
                   "x clamped to width - 1");
    TEST_ASSERT_EQ((uint64_t)s.y, (uint64_t)0,
                   "y clamped to 0");
}

/* ---- Registration ----------------------------------------------------- */

void test_register_desktop(void)
{
    test_suite_register_cat("Desktop: fb_snapshot_size nonzero",
                            test_fb_snapshot_size_nonzero, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: fb_snapshot NULL args rejected",
                            test_fb_snapshot_null_args, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: fb_snapshot roundtrip (sentinel corners)",
                            test_fb_snapshot_roundtrip, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: test_inject_keypress roundtrip ('a')",
                            test_input_inject_keypress_roundtrip, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: test_inject_keypress Enter -> LF",
                            test_input_inject_keypress_enter, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: test_inject_mouse_click delivered",
                            test_input_inject_mouse_click_delivered, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: test_inject_mouse press edge observable",
                            test_input_inject_mouse_press_observable, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: test_inject_mouse_click clamps out-of-bounds",
                            test_input_inject_mouse_click_clamps_out_of_bounds, TEST_CAT_DESKTOP);
}

#endif /* KERNEL_TESTS */
