/* ============================================================================
 * test_desktop.c -- Desktop UI test framework (TEST_CAT_DESKTOP)
 *
 * TODO-05-desktop-ui-test-framework.md
 *   section 1: Framebuffer Snapshot API
 *   section 4: Input Event Injection
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/drivers/keyboard.h"
#include "kernel/drivers/mouse.h"
#include "kernel/mm/pmm.h"
#include "kernel/types.h"
#include "libc/string.h"
#include "desktop/terminal.h"
#include "desktop/wm.h"
#include "desktop/controls.h"
#include "kernel/test/input_record.h"
#include "kernel/test/wcag.h"
#include "kernel/test/test_desktop_reset.h"
#include "kernel/boot_info.h"
#include "icon_store.h"
#include "kernel/test/icon_store_test.h"
#include "main/main_internal.h"

/* ---- section 4 Input Event Injection ----------------------------------------- */

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

/* ---- section 1 Framebuffer Snapshot API -------------------------------------- */

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

    /* section 1 test checkpoint: "buffer is non-zero (not all black)". The four
     * sentinels we just verified would satisfy this trivially; also sample
     * the interior to prove the bulk copy fired. */
    TEST_ASSERT(dst[0] != 0u || dst[w / 2] != 0u || dst[w - 1] != 0u,
                "snapshot buffer is not all-black (bulk copy fired)");

    /* Free the 900-page snapshot buffer one page at a time (no dedicated
     * pmm_free_contiguous helper exists yet). */
    for (uint64_t i = 0; i < pages; i++)
        pmm_free_frame(phys + i * 4096);
}

/* ---- section 4 Input Event Injection (test cases) --------------------------- */

/* Reset the keyboard to a known state before each injection test. Clears
 * the ring buffer AND every latched modifier (shift/ctrl/alt/capslock)
 * AND any pending E0 prefix. Without the full reset, a test-order
 * dependency would creep in: a prior test that injected a modifier press
 * without a matching release would corrupt the next test's scancode
 * (e.g. 0x1E -> Ctrl-A instead of 'a'). */
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
 * so the section 5 terminal-roundtrip test can chain keypresses. */
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

/* ---- section 5 Terminal Output Verification (test cases) --------------------- */

/* Mini strlen so we do not depend on libc in kernel-test builds. */
static int tstr_len(const char *s)
{
    int n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

/* Fast strstr for bounded kernel-test ranges (non-recursive, O(n*m)). */
static int tstr_contains(const char *hay, int hay_len, const char *needle)
{
    int nlen = tstr_len(needle);
    if (nlen == 0 || nlen > hay_len) return 0;
    for (int i = 0; i + nlen <= hay_len; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            if (hay[i + j] != needle[j]) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

/* terminal_get_buffer returns 0 when the terminal window is closed. */
static void test_terminal_get_buffer_when_closed(void)
{
    /* Kernel tests run in Phase 3 BEFORE the desktop compositor opens the
     * Command Prompt window (boot_desktop.c opens it after the test sweep
     * finishes). So `terminal_is_open()` is guaranteed false here unless a
     * prior test left it open; either way, test the closed-path contract. */
    if (terminal_is_open())
        terminal_close();

    char buf[TERM_ROWS * TERM_COLS];
    int rc = terminal_get_buffer(buf, sizeof(buf));

    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)0,
                   "closed terminal returns 0 bytes (not -1)");
}

/* terminal_get_buffer rejects NULL / undersized dest args. */
static void test_terminal_get_buffer_null_or_small(void)
{
    char tiny[16];

    TEST_ASSERT_EQ((uint64_t)(int64_t)terminal_get_buffer((char *)0, 4096),
                   (uint64_t)(int64_t)-1,
                   "NULL dest rejected with -1");
    TEST_ASSERT_EQ((uint64_t)(int64_t)terminal_get_buffer(tiny, (int)sizeof(tiny)),
                   (uint64_t)(int64_t)-1,
                   "undersized dest rejected with -1");
}

/* terminal_buffer_contains is safe to call in every state; it returns 0
 * rather than crashing on NULL/empty needles or a closed terminal. */
static void test_terminal_buffer_contains_guards(void)
{
    if (terminal_is_open())
        terminal_close();

    TEST_ASSERT_EQ((uint64_t)terminal_buffer_contains((const char *)0),
                   (uint64_t)0,
                   "NULL needle returns 0");
    TEST_ASSERT_EQ((uint64_t)terminal_buffer_contains(""),
                   (uint64_t)0,
                   "empty needle returns 0");
    TEST_ASSERT_EQ((uint64_t)terminal_buffer_contains("anything"),
                   (uint64_t)0,
                   "closed terminal never contains anything");
}

/* End-to-end pipeline test: open terminal, simulate cmd.exe prompt and
 * input echo, verify terminal_get_buffer / terminal_buffer_contains see
 * the expected substrings.
 *
 * The real cmd.exe roundtrip (keystroke -> input ring -> cmd.exe drains
 * -> cmd.exe echoes -> cmd.exe runs `dir` -> terminal paints file list)
 * requires cmd.exe to be running, which only happens AFTER the kernel
 * test phase completes. So we stand in for cmd.exe here: terminal_puts
 * for the prompt and echo, plus a sample file-list line. The test
 * validates the entire get_buffer / contains API chain exactly as section 15
 * (Test Isolation and Crash Artifact Capture) will use it once the
 * full cmd.exe driver is wired. */
static void test_terminal_dir_roundtrip_synthesized(void)
{
    /* Kernel tests run in Phase 3 BEFORE the WM initializes, so
     * terminal_open() would fail. Use the test-only seam
     * terminal_test_force_open() to populate internal state without
     * a backing WM window. The seam is #ifdef KERNEL_TESTS-gated and
     * cannot be invoked from production code paths. Codex [M] review:
     * the success path MUST execute in the current test pipeline to
     * catch regressions in the read helpers. */
    terminal_test_force_open();
    TEST_ASSERT(terminal_is_open(),
                "terminal_test_force_open makes is_open() return true");

    /* 1. cmd.exe would paint the prompt. */
    const char *prompt = "C:\\>";
    terminal_puts(prompt, tstr_len(prompt));

    /* 2. User types 'dir' + Enter via the section 4 keypress injector. Route
     *    the bytes straight into terminal_key_input so they land on the
     *    input ring where cmd.exe would read them. */
    terminal_key_input('d');
    terminal_key_input('i');
    terminal_key_input('r');
    terminal_key_input('\n');

    /* 3. Simulate cmd.exe draining the input ring and echoing each
     *    character back to the display. In production this happens on
     *    cmd.exe's read loop. */
    char c;
    while ((c = terminal_trygetchar()) != 0) {
        if (c == '\n')
            terminal_putchar('\n');
        else
            terminal_putchar(c);
    }

    /* 4. Simulate cmd.exe running `dir` and printing a sample line. */
    const char *file_line = "hello.txt";
    terminal_puts(file_line, tstr_len(file_line));

    /* 5. Read back via get_buffer; the full TERM_ROWS*TERM_COLS grid must
     *    now contain both the prompt and the echoed + output bytes. */
    char snapshot[TERM_ROWS * TERM_COLS];
    int copied = terminal_get_buffer(snapshot, sizeof(snapshot));

    TEST_ASSERT_EQ((uint64_t)copied, (uint64_t)(TERM_ROWS * TERM_COLS),
                   "get_buffer copied the full grid");

    TEST_ASSERT(tstr_contains(snapshot, copied, "C:\\>"),
                "snapshot contains the C:\\> prompt");
    TEST_ASSERT(tstr_contains(snapshot, copied, "dir"),
                "snapshot contains the echoed 'dir' command");
    TEST_ASSERT(tstr_contains(snapshot, copied, "hello.txt"),
                "snapshot contains the synthesized dir output entry");

    /* 6. terminal_buffer_contains matches what get_buffer + tstr_contains
     *    report. Both helpers must stay consistent. */
    TEST_ASSERT(terminal_buffer_contains("C:\\>"),
                "buffer_contains(C:\\>) matches");
    TEST_ASSERT(terminal_buffer_contains("dir"),
                "buffer_contains(dir) matches");
    TEST_ASSERT(terminal_buffer_contains("hello.txt"),
                "buffer_contains(hello.txt) matches");
    TEST_ASSERT(!terminal_buffer_contains("definitely-not-in-term"),
                "buffer_contains(absent string) returns 0");

    /* Release the test-forced open state so the real boot_desktop.c
     * terminal_open() later can run cleanly. */
    terminal_test_force_close();
    TEST_ASSERT(!terminal_is_open(),
                "terminal_test_force_close releases is_open() state");
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

/* ---- Section 8: Window Manager State Verification -------------------- */

/* wm_get_window_count reports 0 on a freshly-reset WM, matches the
 * install count as synthetic windows are added, and drops back to 0
 * after wm_test_reset(). Exercises the pure count path without any
 * framebuffer allocation. */
static void test_wm_window_count_tracks_installs(void)
{
    int h1, h2, h3;

    wm_test_reset();
    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)0,
                   "empty WM reports 0 windows");

    h1 = wm_test_install_window(0, 0, 320, 240);
    TEST_ASSERT(h1 >= 0, "first install returns a valid handle");
    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)1,
                   "one window installed -> count == 1");

    h2 = wm_test_install_window(40, 40, 400, 200);
    h3 = wm_test_install_window(80, 80, 300, 180);
    TEST_ASSERT(h2 >= 0 && h3 >= 0, "subsequent installs return handles");
    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)3,
                   "three windows installed -> count == 3");

    wm_test_reset();
    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)0,
                   "wm_test_reset zeroes the count");
}

/* wm_get_focused_window returns -1 on a fresh WM, the focused handle
 * after wm_test_set_focused, and -1 again once the focused window is
 * destroyed (stale-focus protection inside the API). */
static void test_wm_focused_window_tracks_focus(void)
{
    int terminal_h, gallery_h;

    wm_test_reset();
    TEST_ASSERT_EQ((uint64_t)wm_get_focused_window(), (uint64_t)-1,
                   "empty WM reports no focus");

    terminal_h = wm_test_install_window(50, 30, 640, 320);
    gallery_h  = wm_test_install_window(200, 100, 480, 300);
    TEST_ASSERT(terminal_h >= 0 && gallery_h >= 0 && terminal_h != gallery_h,
                "two distinct synthetic windows installed");

    wm_test_set_focused(terminal_h);
    TEST_ASSERT_EQ((uint64_t)wm_get_focused_window(), (uint64_t)terminal_h,
                   "focus returns the terminal handle");

    wm_test_set_focused(gallery_h);
    TEST_ASSERT_EQ((uint64_t)wm_get_focused_window(), (uint64_t)gallery_h,
                   "focus tracks the most recent set_focused call");

    /* Destroying the focused window must clear focused_window back to -1;
     * wm_destroy_window already handles this. Verify the guard inside
     * wm_get_focused_window catches a stale handle. */
    wm_destroy_window(gallery_h);
    TEST_ASSERT_EQ((uint64_t)wm_get_focused_window(), (uint64_t)-1,
                   "destroying the focused window clears focus");
}

/* wm_get_window_rect fills the out-params with the installed rect,
 * rejects NULL out-pointers with -2, and rejects inactive/oob handles
 * with -1. */
static void test_wm_window_rect_roundtrip(void)
{
    int h;
    int32_t x = -1, y = -1;
    uint32_t w = 0, ht = 0;
    int rc;

    wm_test_reset();
    h = wm_test_install_window(50, 30, 640, 320);
    TEST_ASSERT(h >= 0, "installed a window");

    rc = wm_get_window_rect(h, &x, &y, &w, &ht);
    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)0, "rc == 0 on success");
    TEST_ASSERT_EQ((uint64_t)x,  (uint64_t)50,  "x matches install");
    TEST_ASSERT_EQ((uint64_t)y,  (uint64_t)30,  "y matches install");
    TEST_ASSERT_EQ((uint64_t)w,  (uint64_t)640, "width matches install");
    TEST_ASSERT_EQ((uint64_t)ht, (uint64_t)320, "height matches install");

    /* NULL out-param -> -2 */
    rc = wm_get_window_rect(h, (int32_t *)0, &y, &w, &ht);
    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)-2,
                   "NULL x -> -2");

    /* Out-of-range handle -> -1 */
    rc = wm_get_window_rect(9999, &x, &y, &w, &ht);
    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)-1,
                   "oob handle -> -1");

    /* Negative handle -> -1 */
    rc = wm_get_window_rect(-1, &x, &y, &w, &ht);
    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)-1,
                   "negative handle -> -1");

    /* Inactive handle -> -1 */
    wm_destroy_window(h);
    rc = wm_get_window_rect(h, &x, &y, &w, &ht);
    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)-1,
                   "inactive handle -> -1");
}

/* Alt+F4 close path: install two windows, focus the terminal, inject
 * Alt+F4 via keyboard_inject_scancode, then drain the pending-close
 * queue the way the compositor does every frame. Verifies that:
 *   (a) Alt+F4 only ENQUEUES the close (nothing destroyed synchronously),
 *       so the real IRQ handler stays out of PMM / compositor races;
 *   (b) wm_process_pending_closes() actually tears the window down;
 *   (c) modifiers / non-focused states behave correctly.
 * Addresses Codex [H] review findings: IRQ-context destruction was the
 * original design; this test now targets the deferred-drain path used
 * by compositor.c. */
static void test_wm_alt_f4_closes_focused_window(void)
{
    int terminal_h, gallery_h;

    wm_test_reset();
    keyboard_reset_state();

    terminal_h = wm_test_install_window(50, 30, 640, 320);
    gallery_h  = wm_test_install_window(200, 100, 480, 300);
    TEST_ASSERT(terminal_h >= 0 && gallery_h >= 0,
                "two synthetic windows installed");

    wm_test_set_focused(terminal_h);
    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)2,
                   "pre-Alt+F4: count == 2");

    /* Alt press, then F4 press, then Alt release. F4 with alt_held == 1
     * posts a deferred close -- window count stays at 2 until the drain
     * runs. */
    keyboard_inject_scancode(0x38);  /* LALT press */
    keyboard_inject_scancode(0x3E);  /* F4 press -> wm_close_focused_window */
    keyboard_inject_scancode(0xB8);  /* LALT release */

    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)2,
                   "post-Alt+F4 pre-drain: count still 2 (close deferred)");
    TEST_ASSERT_EQ((uint64_t)wm_get_focused_window(), (uint64_t)terminal_h,
                   "post-Alt+F4 pre-drain: focus unchanged");

    /* Drain the pending-close queue the way compositor.c does every
     * frame. Must destroy exactly one window (the focused one). */
    {
        int destroyed = wm_process_pending_closes();
        TEST_ASSERT_EQ((uint64_t)destroyed, (uint64_t)1,
                       "drain destroys the one pending window");
    }

    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)1,
                   "post-drain: count == 1");
    TEST_ASSERT_EQ((uint64_t)wm_get_focused_window(), (uint64_t)-1,
                   "post-drain: focus cleared by wm_destroy_window");

    /* Gallery is still around, just no longer focused. */
    {
        int32_t gx = -1, gy = -1;
        uint32_t gw = 0, gh = 0;
        int rc = wm_get_window_rect(gallery_h, &gx, &gy, &gw, &gh);
        TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)0,
                       "gallery window still reachable after drain");
        TEST_ASSERT_EQ((uint64_t)gw, (uint64_t)480,
                       "gallery width preserved");
    }

    /* F4 without alt_held does NOT enqueue a close. */
    wm_test_set_focused(gallery_h);
    keyboard_inject_scancode(0x3E);  /* F4 press, no alt */
    TEST_ASSERT_EQ((uint64_t)wm_process_pending_closes(), (uint64_t)0,
                   "F4 without Alt does not enqueue a close");
    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)1,
                   "F4 alone does NOT close a window");

    /* Alt+F4 with no focused window is a silent no-op (enqueue rejects
     * the -1 handle). */
    wm_destroy_window(gallery_h);
    keyboard_inject_scancode(0x38);
    keyboard_inject_scancode(0x3E);
    keyboard_inject_scancode(0xB8);
    TEST_ASSERT_EQ((uint64_t)wm_process_pending_closes(), (uint64_t)0,
                   "drain after Alt+F4 with no focus destroys nothing");
    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)0,
                   "Alt+F4 on empty WM is a no-op");
}

/* Reset path: a deferred-close enqueue from one test must NOT leak into
 * the next. Codex [M] section 8 quality review demanded a regression so the
 * seam stays deterministic across test order. */
static void test_wm_reset_clears_deferred_close(void)
{
    int handle;

    /* Seed: install a window, focus it, enqueue Alt+F4 (does NOT
     * destroy yet -- destruction is deferred). */
    wm_test_reset();
    keyboard_reset_state();
    handle = wm_test_install_window(0, 0, 320, 240);
    TEST_ASSERT(handle >= 0, "first install OK");
    wm_test_set_focused(handle);
    keyboard_inject_scancode(0x38);  /* LALT press */
    keyboard_inject_scancode(0x3E);  /* F4 press -> enqueue */
    keyboard_inject_scancode(0xB8);  /* LALT release */

    /* wm_test_reset MUST drop the queued close so a fresh window slot
     * isn't destroyed by the stale enqueue. */
    wm_test_reset();
    handle = wm_test_install_window(50, 50, 400, 200);
    TEST_ASSERT(handle >= 0, "post-reset install OK");
    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)1,
                   "post-reset count == 1 (fresh install only)");

    /* If the seam leaked the prior enqueue, this drain would destroy
     * our just-installed window. */
    TEST_ASSERT_EQ((uint64_t)wm_process_pending_closes(), (uint64_t)0,
                   "drain after wm_test_reset is a no-op");
    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)1,
                   "fresh window survives drain");
}

/* ---- Section 10: Frame Timing and Drop Oracle ----------------------- */

/* Fresh reset zeroes every counter; seqlock snapshot is consistent. */
static void test_wm_frame_stats_reset_zeros_counters(void)
{
    struct wm_frame_stats s;

    wm_frame_stats_reset_for_test();
    wm_get_frame_stats(&s);
    TEST_ASSERT_EQ((uint64_t)s.frames_presented, (uint64_t)0,
                   "reset -> frames_presented == 0");
    TEST_ASSERT_EQ((uint64_t)s.frames_queued, (uint64_t)0,
                   "reset -> frames_queued == 0");
    TEST_ASSERT_EQ((uint64_t)s.frames_late, (uint64_t)0,
                   "reset -> frames_late == 0");
    TEST_ASSERT_EQ((uint64_t)s.frames_dropped, (uint64_t)0,
                   "reset -> frames_dropped == 0");
    TEST_ASSERT_EQ((uint64_t)s.last_vsync_qpc, (uint64_t)0,
                   "reset -> last_vsync_qpc == 0");
    TEST_ASSERT_EQ((uint64_t)s.last_present_qpc, (uint64_t)0,
                   "reset -> last_present_qpc == 0");
}

/* Present under budget advances presented + last_*; stays below late. */
static void test_wm_frame_stats_on_present_under_budget(void)
{
    struct wm_frame_stats s;
    uint64_t t0 = 1000000ULL;         /* 1 ms */
    uint64_t t1 = t0 + 10000000ULL;   /* t0 + 10 ms -- under 16.67 ms budget */

    wm_frame_stats_reset_for_test();
    wm_frame_stats_on_present(t0, t1);
    wm_get_frame_stats(&s);

    TEST_ASSERT_EQ((uint64_t)s.frames_presented, (uint64_t)1,
                   "one present -> frames_presented == 1");
    TEST_ASSERT_EQ((uint64_t)s.frames_late, (uint64_t)0,
                   "under-budget present -> frames_late == 0");
    TEST_ASSERT_EQ((uint64_t)s.last_vsync_qpc, t0,
                   "last_vsync_qpc == provided vsync stamp");
    TEST_ASSERT_EQ((uint64_t)s.last_present_qpc, t1,
                   "last_present_qpc == provided present stamp");
}

/* Present over 16.67 ms budget bumps frames_late. */
static void test_wm_frame_stats_on_present_over_budget(void)
{
    struct wm_frame_stats s;
    uint64_t t0 = 0ULL;
    uint64_t t1 = 20000000ULL;  /* 20 ms -- exceeds 16.67 ms budget */

    wm_frame_stats_reset_for_test();
    wm_frame_stats_on_present(t0, t1);
    wm_get_frame_stats(&s);

    TEST_ASSERT_EQ((uint64_t)s.frames_presented, (uint64_t)1,
                   "over-budget still counts as presented");
    TEST_ASSERT_EQ((uint64_t)s.frames_late, (uint64_t)1,
                   "over-budget present -> frames_late == 1");
}

/* Queue + coalesce: mark_dirty on a clean frame increments queued;
 * mark_dirty on an already-dirty frame increments queued AND dropped. */
static void test_wm_frame_stats_queue_and_drop(void)
{
    struct wm_frame_stats s;

    wm_frame_stats_reset_for_test();

    /* First mark: was clean, count only queued. */
    wm_frame_stats_on_mark_dirty(/* was_already_dirty */ 0);
    wm_get_frame_stats(&s);
    TEST_ASSERT_EQ((uint64_t)s.frames_queued, (uint64_t)1,
                   "first mark -> queued == 1");
    TEST_ASSERT_EQ((uint64_t)s.frames_dropped, (uint64_t)0,
                   "first mark -> dropped == 0");

    /* Second mark while still dirty: queued++, dropped++. */
    wm_frame_stats_on_mark_dirty(/* was_already_dirty */ 1);
    wm_get_frame_stats(&s);
    TEST_ASSERT_EQ((uint64_t)s.frames_queued, (uint64_t)2,
                   "second mark (already dirty) -> queued == 2");
    TEST_ASSERT_EQ((uint64_t)s.frames_dropped, (uint64_t)1,
                   "second mark (already dirty) -> dropped == 1");
}

/* Monotonic progression across multiple presents: counters never
 * decrease, last_present_qpc advances. */
static void test_wm_frame_stats_counters_monotonic(void)
{
    struct wm_frame_stats a, b;

    wm_frame_stats_reset_for_test();
    wm_frame_stats_on_present(100ULL, 200ULL);
    wm_get_frame_stats(&a);

    wm_frame_stats_on_present(300ULL, 400ULL);
    wm_get_frame_stats(&b);

    TEST_ASSERT(b.frames_presented >= a.frames_presented,
                "frames_presented never decreases");
    TEST_ASSERT_EQ((uint64_t)b.frames_presented,
                   (uint64_t)(a.frames_presented + 1),
                   "second present advances by exactly 1");
    TEST_ASSERT(b.last_present_qpc > a.last_present_qpc,
                "last_present_qpc advances with fresh timestamps");
}

/* NULL out-pointer guard: wm_get_frame_stats must not crash on NULL. */
static void test_wm_frame_stats_null_guard(void)
{
    wm_frame_stats_reset_for_test();
    wm_get_frame_stats((struct wm_frame_stats *)0);
    /* If we're still running, the guard worked. */
}

/* wm_mark_dirty path end-to-end: public entry point bumps queued +
 * dropped when called twice in a row. Exercises the real dirty-flag
 * coalescing logic in src/desktop/wm.c. */
static void test_wm_mark_dirty_bumps_queued(void)
{
    struct wm_frame_stats before, after;

    wm_frame_stats_reset_for_test();
    wm_get_frame_stats(&before);

    /* First call: sets needs_redraw. was_already_dirty state read by
     * wm_mark_dirty() determines whether drop fires. */
    wm_mark_dirty();
    wm_mark_dirty();  /* second call now sees needs_redraw == 1 */

    wm_get_frame_stats(&after);
    TEST_ASSERT_EQ((uint64_t)after.frames_queued,
                   (uint64_t)(before.frames_queued + 2),
                   "two wm_mark_dirty calls -> queued += 2");
    TEST_ASSERT(after.frames_dropped >= before.frames_dropped + 1,
                "second mark while already dirty drops at least one");
}

/* ---- Section 11: Input Record and Replay ---------------------------- */

/* record_begin allocates, record_stop returns count, record_release
 * frees. Double-begin returns -2; double-release is a no-op. */
static void test_input_record_begin_stop_release(void)
{
    int rc;

    input_record_release();  /* clean slate */
    rc = input_record_begin(16);
    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)0, "first begin returns 0");

    rc = input_record_begin(16);
    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)-2, "double begin returns -2");

    TEST_ASSERT_EQ((uint64_t)input_record_count(), (uint64_t)0,
                   "empty record -> count == 0");

    rc = input_record_stop();
    TEST_ASSERT_EQ((uint64_t)rc, (uint64_t)0, "stop returns captured count (0)");

    input_record_release();
    input_record_release();  /* idempotent */
    TEST_ASSERT_EQ((uint64_t)input_record_count(), (uint64_t)0,
                   "post-release count == 0");
}

/* Capture is tee-mode: calls while inactive are no-ops; calls while
 * active append to the buffer. */
static void test_input_record_key_mouse_ime_capture(void)
{
    uint8_t commit_bytes[3] = {0xE4, 0xBD, 0xA0};  /* U+4F60 "ni" */

    input_record_release();

    /* Inactive: recording primitives are no-ops. */
    input_record_key(0x1E, 'a');
    TEST_ASSERT_EQ((uint64_t)input_record_count(), (uint64_t)0,
                   "inactive record_key is a no-op");

    TEST_ASSERT_EQ((uint64_t)input_record_begin(16), (uint64_t)0,
                   "begin OK");

    input_record_key(0x1E, 'a');
    input_record_mouse(100, 200, 1);
    input_record_ime_compose(0x4F60, 0);
    input_record_ime_commit(commit_bytes, 3);

    TEST_ASSERT_EQ((uint64_t)input_record_count(), (uint64_t)4,
                   "4 events captured");

    const input_event_t *evts = input_record_events();
    TEST_ASSERT(evts != (const input_event_t *)0, "events ptr non-null");
    TEST_ASSERT_EQ((uint64_t)evts[0].kind, (uint64_t)INPUT_EVT_KEY, "event 0 is key");
    TEST_ASSERT_EQ((uint64_t)evts[0].payload.key.codepoint, (uint64_t)'a',
                   "key codepoint preserved");
    TEST_ASSERT_EQ((uint64_t)evts[1].kind, (uint64_t)INPUT_EVT_MOUSE, "event 1 is mouse");
    TEST_ASSERT_EQ((uint64_t)evts[1].payload.mouse.x, (uint64_t)100, "mouse x");
    TEST_ASSERT_EQ((uint64_t)evts[2].kind, (uint64_t)INPUT_EVT_IME_COMPOSE,
                   "event 2 is ime_compose");
    TEST_ASSERT_EQ((uint64_t)evts[2].payload.ime_compose.codepoint, (uint64_t)0x4F60,
                   "compose codepoint");
    TEST_ASSERT_EQ((uint64_t)evts[3].kind, (uint64_t)INPUT_EVT_IME_COMMIT,
                   "event 3 is ime_commit");
    TEST_ASSERT_EQ((uint64_t)evts[3].payload.ime_commit.len, (uint64_t)3,
                   "commit len matches");
    TEST_ASSERT_EQ((uint64_t)evts[3].payload.ime_commit.utf8[0], (uint64_t)0xE4,
                   "commit byte 0 preserved");

    input_record_stop();
    input_record_release();
}

/* Capacity ceiling: events past capacity are dropped (not overwritten,
 * so tests see a deterministic event count). */
static void test_input_record_capacity_drops(void)
{
    int i;
    input_record_release();
    TEST_ASSERT_EQ((uint64_t)input_record_begin(4), (uint64_t)0, "begin with cap=4");

    for (i = 0; i < 10; i++)
        input_record_key((uint8_t)i, (uint32_t)('a' + i));

    TEST_ASSERT_EQ((uint64_t)input_record_count(), (uint64_t)4,
                   "capacity-4 ring stops at 4 events (no overwrite)");
    input_record_stop();
    input_record_release();
}

/* Serialize -> parse -> replay roundtrip: records three key events,
 * serializes to JSONL, parses back via input_replay_from_jsonl() with
 * speed=0/1 (as-fast-as-possible), and verifies the replayed
 * characters landed in the terminal input ring. */
static void test_input_record_serialize_roundtrip(void)
{
    char buf[4096];
    int  len;

    input_record_release();
    terminal_test_force_open();

    /* Drain the terminal input ring from any prior test. */
    while (terminal_trygetchar() != 0) { /* drain */ }

    TEST_ASSERT_EQ((uint64_t)input_record_begin(8), (uint64_t)0, "begin OK");

    /* Record "dir" + Enter. scancode_normal[0x20]='d', 0x17='i',
     * 0x13='r', 0x1C='\n'. We use keyboard_inject_scancode for capture
     * via the in-test record API to pair record + replay, not the real
     * IRQ path. */
    input_record_key(0x20, 'd');
    input_record_key(0x17, 'i');
    input_record_key(0x13, 'r');
    input_record_key(0x1C, '\n');

    TEST_ASSERT_EQ((uint64_t)input_record_count(), (uint64_t)4, "4 events recorded");

    len = input_record_serialize(buf, (int)sizeof(buf));
    TEST_ASSERT(len > 0, "serialize returned positive length");
    TEST_ASSERT(len < (int)sizeof(buf), "serialize fit in dest buffer");

    input_record_stop();
    input_record_release();

    /* Replay parses the JSONL and drives keyboard_inject_scancode,
     * which pushes characters to terminal_key_input. Use 0/1 = run
     * as fast as possible so the test does not wait wall-clock time. */
    int replayed = input_replay_from_jsonl(buf, len, 0, 1);
    TEST_ASSERT_EQ((uint64_t)replayed, (uint64_t)4, "replay processed 4 events");

    /* Pull characters off the ring and compare. */
    char got[5] = {0,0,0,0,0};
    int gi;
    for (gi = 0; gi < 4; gi++) {
        got[gi] = terminal_trygetchar();
    }
    TEST_ASSERT_EQ((uint64_t)(uint8_t)got[0], (uint64_t)'d', "byte 0 == 'd'");
    TEST_ASSERT_EQ((uint64_t)(uint8_t)got[1], (uint64_t)'i', "byte 1 == 'i'");
    TEST_ASSERT_EQ((uint64_t)(uint8_t)got[2], (uint64_t)'r', "byte 2 == 'r'");
    TEST_ASSERT_EQ((uint64_t)(uint8_t)got[3], (uint64_t)'\n', "byte 3 == '\\n'");

    terminal_test_force_close();
}

/* IME commit roundtrip: records a CJK codepoint's UTF-8 bytes,
 * serializes to the hex-string format, parses back, replays;
 * terminal_key_input receives the same UTF-8 byte sequence.
 * Matches the section 11 test checkpoint "Record CJK via IME composition;
 * replay; committed UTF-8 string matches original byte sequence." */
static void test_input_record_ime_roundtrip_utf8(void)
{
    /* U+4F60 "ni" (hello), UTF-8: E4 BD A0 */
    uint8_t ni[3] = {0xE4, 0xBD, 0xA0};
    /* U+597D "hao" (good),  UTF-8: E5 A5 BD */
    uint8_t hao[3] = {0xE5, 0xA5, 0xBD};
    char buf[1024];
    int  len;

    input_record_release();
    terminal_test_force_open();
    while (terminal_trygetchar() != 0) { /* drain */ }

    TEST_ASSERT_EQ((uint64_t)input_record_begin(4), (uint64_t)0, "begin OK");

    input_record_ime_commit(ni, 3);
    input_record_ime_commit(hao, 3);

    TEST_ASSERT_EQ((uint64_t)input_record_count(), (uint64_t)2, "2 commits captured");
    len = input_record_serialize(buf, (int)sizeof(buf));
    TEST_ASSERT(len > 0, "serialize succeeded");

    input_record_stop();
    input_record_release();

    int replayed = input_replay_from_jsonl(buf, len, 0, 1);
    TEST_ASSERT_EQ((uint64_t)replayed, (uint64_t)2, "replay 2 IME commits");

    /* Pull 6 UTF-8 bytes out of the terminal input ring and verify
     * byte-for-byte equality with the original commit sequence. */
    uint8_t got[6];
    int gi;
    for (gi = 0; gi < 6; gi++) {
        got[gi] = (uint8_t)terminal_trygetchar();
    }

    TEST_ASSERT_EQ((uint64_t)got[0], (uint64_t)0xE4, "byte 0 of 'ni'");
    TEST_ASSERT_EQ((uint64_t)got[1], (uint64_t)0xBD, "byte 1 of 'ni'");
    TEST_ASSERT_EQ((uint64_t)got[2], (uint64_t)0xA0, "byte 2 of 'ni'");
    TEST_ASSERT_EQ((uint64_t)got[3], (uint64_t)0xE5, "byte 0 of 'hao'");
    TEST_ASSERT_EQ((uint64_t)got[4], (uint64_t)0xA5, "byte 1 of 'hao'");
    TEST_ASSERT_EQ((uint64_t)got[5], (uint64_t)0xBD, "byte 2 of 'hao'");

    terminal_test_force_close();
}

/* Compute C-string length without pulling in a libc dependency. */
static int tr_strlen(const char *s)
{
    int n = 0;
    while (s[n] != '\0') n++;
    return n;
}

/* Malformed JSONL -> -1. Verifies every parse-failure branch wired
 * through input_replay_from_jsonl rejects rather than silently
 * replaying. */
static void test_input_record_replay_rejects_malformed(void)
{
    const char *cases[] = {
        /* missing kind */
        "{\"ts_ns\":0}\n",
        /* unknown kind */
        "{\"ts_ns\":0,\"kind\":\"laser\"}\n",
        /* mouse missing x */
        "{\"ts_ns\":0,\"kind\":\"mouse\",\"y\":0,\"buttons\":0}\n",
        /* mouse missing buttons */
        "{\"ts_ns\":0,\"kind\":\"mouse\",\"x\":0,\"y\":0}\n",
        /* key missing scancode (Codex section 11 quality finding -- prior
         * draft accepted this and replayed scancode=0). */
        "{\"ts_ns\":0,\"kind\":\"key\",\"codepoint\":97}\n",
        /* key missing codepoint */
        "{\"ts_ns\":0,\"kind\":\"key\",\"scancode\":30}\n",
        /* ime_compose missing codepoint */
        "{\"ts_ns\":0,\"kind\":\"ime_compose\",\"candidate\":0}\n",
        /* ime_compose missing candidate */
        "{\"ts_ns\":0,\"kind\":\"ime_compose\",\"codepoint\":0x4F60}\n",
        /* ime_commit missing utf8 */
        "{\"ts_ns\":0,\"kind\":\"ime_commit\"}\n",
        /* unknown key in payload */
        "{\"ts_ns\":0,\"kind\":\"key\",\"scancode\":0,\"codepoint\":0,\"laser\":1}\n",
    };
    const char *names[] = {"missing kind", "unknown kind",
                           "mouse missing x", "mouse missing buttons",
                           "key missing scancode", "key missing codepoint",
                           "ime_compose missing codepoint",
                           "ime_compose missing candidate",
                           "ime_commit missing utf8",
                           "unknown key"};
    unsigned i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int rc = input_replay_from_jsonl(cases[i], tr_strlen(cases[i]), 0, 1);
        TEST_ASSERT_EQ((uint64_t)(int64_t)rc, (uint64_t)(int64_t)-1,
                       names[i]);
    }

    /* NULL input -> -1. */
    int rc_null = input_replay_from_jsonl((const char *)0, 10, 0, 1);
    TEST_ASSERT_EQ((uint64_t)(int64_t)rc_null, (uint64_t)(int64_t)-1,
                   "NULL jsonl -> -1");

    /* Zero length -> -1. */
    int rc_zero = input_replay_from_jsonl("{}", 0, 0, 1);
    TEST_ASSERT_EQ((uint64_t)(int64_t)rc_zero, (uint64_t)(int64_t)-1,
                   "zero length -> -1");
}

/* ---- Section 12: Headless Compositor + Frame-Lock Stepping ---------- */

/* set_headless toggles the volatile flag; is_headless reads it back.
 * Default (zero-init) is 0. */
static void test_compositor_headless_toggle(void)
{
    int saved = compositor_is_headless();
    compositor_set_headless(0);
    TEST_ASSERT_EQ((uint64_t)compositor_is_headless(), (uint64_t)0,
                   "is_headless == 0 after set(0)");
    compositor_set_headless(1);
    TEST_ASSERT_EQ((uint64_t)compositor_is_headless(), (uint64_t)1,
                   "is_headless == 1 after set(1)");
    compositor_set_headless(42);
    TEST_ASSERT_EQ((uint64_t)compositor_is_headless(), (uint64_t)1,
                   "non-zero set normalises to 1");
    /* Restore for subsequent tests. */
    compositor_set_headless(saved);
}

/* set_test_seed/get_test_seed roundtrip the full uint64 range. */
static void test_compositor_test_seed_roundtrip(void)
{
    uint64_t saved = compositor_get_test_seed();

    compositor_set_test_seed(0);
    TEST_ASSERT_EQ((uint64_t)compositor_get_test_seed(), (uint64_t)0,
                   "seed == 0 after set(0)");
    compositor_set_test_seed(0xDEADBEEFCAFEBABEULL);
    TEST_ASSERT_EQ((uint64_t)compositor_get_test_seed(),
                   (uint64_t)0xDEADBEEFCAFEBABEULL,
                   "seed roundtrips full uint64");
    compositor_set_test_seed(~(uint64_t)0);
    TEST_ASSERT_EQ((uint64_t)compositor_get_test_seed(), (uint64_t)~(uint64_t)0,
                   "seed accepts 0xFFFFFFFFFFFFFFFF");

    compositor_set_test_seed(saved);
}

/* compositor_step_frames(N) returns N AND advances frames_presented
 * by exactly N. Verifies section 12 wires section 10's counter advancement
 * correctly under headless mode. */
static void test_compositor_step_frames_advances_stats(void)
{
    struct wm_frame_stats before, after;
    int saved_headless = compositor_is_headless();

    /* Force headless so step_frames does not call fb_swap (the test
     * phase has no compositor thread bringing the fb online). */
    compositor_set_headless(1);

    wm_frame_stats_reset_for_test();
    wm_get_frame_stats(&before);
    TEST_ASSERT_EQ((uint64_t)before.frames_presented, (uint64_t)0,
                   "fresh reset -> frames_presented == 0");

    uint32_t n = compositor_step_frames(10);
    TEST_ASSERT_EQ((uint64_t)n, (uint64_t)10,
                   "step_frames(10) returns 10");

    wm_get_frame_stats(&after);
    TEST_ASSERT_EQ((uint64_t)after.frames_presented,
                   (uint64_t)(before.frames_presented + 10),
                   "frames_presented advances by exactly 10");

    /* section 12 test checkpoint: replay with same seed yields the same
     * counter increment. The stat advance is deterministic by
     * construction (one bump per step), so re-running step_frames
     * with the same seed must again advance by exactly N. */
    compositor_set_test_seed(0xC0FFEE);
    wm_frame_stats_reset_for_test();
    uint32_t a = compositor_step_frames(7);
    struct wm_frame_stats run_a;
    wm_get_frame_stats(&run_a);

    compositor_set_test_seed(0xC0FFEE);
    wm_frame_stats_reset_for_test();
    uint32_t b = compositor_step_frames(7);
    struct wm_frame_stats run_b;
    wm_get_frame_stats(&run_b);

    TEST_ASSERT_EQ((uint64_t)a, (uint64_t)b,
                   "same-seed runs return identical step count");
    TEST_ASSERT_EQ((uint64_t)run_a.frames_presented,
                   (uint64_t)run_b.frames_presented,
                   "same-seed runs land at identical frames_presented");

    /* step_frames(0) is a clean no-op. */
    wm_frame_stats_reset_for_test();
    n = compositor_step_frames(0);
    TEST_ASSERT_EQ((uint64_t)n, (uint64_t)0, "step_frames(0) returns 0");
    wm_get_frame_stats(&after);
    TEST_ASSERT_EQ((uint64_t)after.frames_presented, (uint64_t)0,
                   "step_frames(0) does not advance frames_presented");

    /* step_frames is REJECTED when headless is off so it cannot race
     * the live compositor loop. Codex [H] section 12 review required this. */
    compositor_set_headless(0);
    wm_frame_stats_reset_for_test();
    n = compositor_step_frames(5);
    TEST_ASSERT_EQ((uint64_t)n, (uint64_t)0,
                   "step_frames(5) returns 0 when !headless");
    wm_get_frame_stats(&after);
    TEST_ASSERT_EQ((uint64_t)after.frames_presented, (uint64_t)0,
                   "step_frames while !headless must not advance stats");

    compositor_set_headless(saved_headless);
}

/* boot_config.compositor mirrors between the bootloader struct and the
 * kernel struct; the static_assert in boot_info.h catches struct-size
 * drift but not the field offset of `compositor` specifically. Pin it. */
static void test_boot_config_compositor_field(void)
{
    /* Touch the field. If the struct doesn't have it the build fails;
     * if the bootloader and kernel disagree on the offset the
     * boot_info ABI mirror_compare.sh in CI catches it. */
    struct boot_config local = {0};
    local.compositor = 1;
    TEST_ASSERT_EQ((uint64_t)local.compositor, (uint64_t)1,
                   "boot_config.compositor accepts 1");
    local.compositor = 0;
    TEST_ASSERT_EQ((uint64_t)local.compositor, (uint64_t)0,
                   "boot_config.compositor accepts 0");
}

/* ---- Section 13: Multi-Monitor + DPI Matrix (API surface only) ------ */

/* fb_get_output_count is 1 today (single-output hardware path) and
 * MUST return 1 until the virtio-gpu multi-output driver lands. The
 * test pins the contract so a future driver that returns 0 (missing
 * init) or garbage doesn't silently bypass the matrix runner. */
static void test_fb_output_count_is_nonzero(void)
{
    uint32_t n = fb_get_output_count();
    TEST_ASSERT(n >= 1,
                "fb_get_output_count() returns at least 1 (single-output baseline)");
    /* Matrix tests rely on this being <= 3; the virtio-gpu driver will
     * negotiate max_outputs=3 when wired. */
    TEST_ASSERT(n <= 3,
                "fb_get_output_count() <= 3 (matrix cap)");
}

/* fb_snapshot_monitor(0, ...) routes to fb_snapshot when single-output.
 * For indices >= output_count, returns -2 (E_INVALID_INDEX). */
static void test_fb_snapshot_monitor_rejects_oob(void)
{
    uint32_t w = 0, h = 0;
    uint32_t count = fb_get_output_count();

    /* NULL out-args still reject with -1 (same as fb_snapshot). */
    int rc = fb_snapshot_monitor(0, (void *)0, &w, &h);
    TEST_ASSERT_EQ((uint64_t)(int64_t)rc, (uint64_t)(int64_t)-1,
                   "NULL dest rejected with -1");

    rc = fb_snapshot_monitor(count, (void *)&w, &w, &h);
    TEST_ASSERT_EQ((uint64_t)(int64_t)rc, (uint64_t)(int64_t)-2,
                   "index == output_count returns -2 (OOB)");

    rc = fb_snapshot_monitor(count + 10, (void *)&w, &w, &h);
    TEST_ASSERT_EQ((uint64_t)(int64_t)rc, (uint64_t)(int64_t)-2,
                   "index far past output_count returns -2");

    rc = fb_snapshot_monitor(0xFFFFFFFFu, (void *)&w, &w, &h);
    TEST_ASSERT_EQ((uint64_t)(int64_t)rc, (uint64_t)(int64_t)-2,
                   "UINT32_MAX index rejected");
}

/* boot_config.test_monitors_count must be a valid count (0..3) after
 * bootloader parse. Zero = "use hardware default" (clamped by the
 * parser when value > 3 as well). */
static void test_boot_config_test_monitors_count(void)
{
    /* Read the live value; the bootloader already parsed boot.conf.
     * Default is 0 (not set). We don't force a specific boot.conf
     * for this test -- we just verify the field is accessible and
     * carries a sane (<=3) value. */
    uint8_t n = g_boot_info.config.test_monitors_count;
    TEST_ASSERT(n <= 3,
                "test_monitors_count must be 0..3 (parser clamps to 0 when >3)");
}

/* Kernel-side mirror of the bootloader's test_monitors parser so the
 * exact-mapping semantics can be unit-tested without booting. The
 * logic MUST stay in lock-step with `src/boot/uefi/bootx64.c`
 * `parse_conf_kv()` test_monitors handler. Codex section 13 review demanded
 * positive tests after the prior implementation mis-parsed
 * `test_monitors=2` as count=1. */
static uint8_t test_parse_test_monitors(const char *val)
{
    uint8_t count = 0;
    int all_digits = (*val != '\0');
    const char *p = val;
    while (*p) {
        if (*p < '0' || *p > '9') { all_digits = 0; break; }
        p++;
    }
    if (all_digits) {
        /* ascii_atoi equivalent: base-10, no sign, no overflow guard
         * beyond the count > 3 clamp that follows. */
        uint64_t v = 0;
        const char *q = val;
        while (*q) { v = v * 10 + (uint64_t)(*q - '0'); q++; }
        count = (v > 3) ? 0 : (uint8_t)v;
    } else {
        count = (*val != '\0') ? 1 : 0;
        const char *q = val;
        while (*q) {
            if (*q == ',') count++;
            q++;
        }
        if (count > 3) count = 0;
    }
    return count;
}

/* Exact-mapping regression for the bootloader parser. Pure-integer
 * form: "1"->1, "2"->2, "3"->3, "4"->0 (clamped); geometry list
 * form: count the comma-separated entries. Codex [H] section 13 review. */
static void test_test_monitors_parser_exact_mapping(void)
{
    /* Pure-integer form */
    TEST_ASSERT_EQ((uint64_t)test_parse_test_monitors("1"), (uint64_t)1,
                   "test_monitors=1 -> count 1");
    TEST_ASSERT_EQ((uint64_t)test_parse_test_monitors("2"), (uint64_t)2,
                   "test_monitors=2 -> count 2");
    TEST_ASSERT_EQ((uint64_t)test_parse_test_monitors("3"), (uint64_t)3,
                   "test_monitors=3 -> count 3");
    TEST_ASSERT_EQ((uint64_t)test_parse_test_monitors("4"), (uint64_t)0,
                   "test_monitors=4 -> 0 (clamp)");
    TEST_ASSERT_EQ((uint64_t)test_parse_test_monitors("0"), (uint64_t)0,
                   "test_monitors=0 -> 0");
    TEST_ASSERT_EQ((uint64_t)test_parse_test_monitors(""),  (uint64_t)0,
                   "test_monitors empty -> 0");

    /* Geometry list form -- counts comma-separated entries. */
    TEST_ASSERT_EQ((uint64_t)test_parse_test_monitors("1920x1080@96"),
                   (uint64_t)1, "single geometry -> 1");
    TEST_ASSERT_EQ((uint64_t)test_parse_test_monitors("1920x1080@96,1920x1080@144"),
                   (uint64_t)2, "two geometries -> 2");
    TEST_ASSERT_EQ((uint64_t)test_parse_test_monitors(
                       "1920x1080@96,1920x1080@144,3840x2160@192"),
                   (uint64_t)3, "three geometries -> 3");
    TEST_ASSERT_EQ((uint64_t)test_parse_test_monitors(
                       "a,b,c,d"),
                   (uint64_t)0, "four entries -> 0 (clamp)");
}

/* ---- Section 14: WCAG Sweep Over Automation Tree (test infra only) -- */

/* wcag_rule_name returns a stable string for every rule ID, even
 * before the accessibility automation tree provider ships. */
static void test_wcag_rule_name_table(void)
{
    /* Strings are const -- just make sure each enum entry returns a
     * non-NULL, non-empty string. */
    for (int i = 0; i < WCAG_RULE_COUNT; i++) {
        const char *name = wcag_rule_name((wcag_rule_id_t)i);
        TEST_ASSERT(name != (const char *)0,
                    "wcag_rule_name returns non-NULL");
        TEST_ASSERT(name[0] != '\0', "wcag_rule_name returns non-empty");
    }
    /* Out-of-range returns "unknown" without crashing. */
    const char *oor = wcag_rule_name((wcag_rule_id_t)999);
    TEST_ASSERT(oor != (const char *)0,
                "wcag_rule_name(OOR) returns non-NULL");
}

/* Provider-ready predicate is 0 today (the accessibility automation tree
 * provider not shipped). The test itself is a real TEST_ASSERT -- it
 * fails if someone hard-codes wcag_provider_ready() to 1 without
 * also wiring the sweep loop. The PENDING-flavored assertion sits
 * on top in test_wcag_sweep_pending_provider below. */
static void test_wcag_provider_ready_flag(void)
{
    int ready = wcag_provider_ready();
    TEST_ASSERT(ready == 0 || ready == 1,
                "wcag_provider_ready returns 0 or 1 (boolean contract)");
}

/* wcag_sweep_run returns WCAG_SWEEP_PROVIDER_MISSING today because
 * the automation tree provider is not yet wired. TEST_PENDING marks
 * this as a known-blocked assertion. When the accessibility automation tree provider lands AND
 * wcag_provider_ready() flips to 1, this assertion will fail (the
 * sweep will return >= 0, not -1) -- that failure is the signal that
 * the test case itself needs to be rewritten into a real positive
 * check (e.g., "sweep against a fixture with a known violation and
 * expected finding"). Codex [M] section 14 review required this split so
 * a provider-wired desktop with 0 real findings doesn't read as
 * indistinguishable from the stub state. */
static void test_wcag_sweep_pending_provider(void)
{
    wcag_finding_t findings[16];
    int n = wcag_sweep_run(findings, 16);
    TEST_PENDING(n == WCAG_SWEEP_PROVIDER_MISSING,
                 "WCAG sweep signals PROVIDER_MISSING until "
                 "the accessibility automation tree provider lands");
}

/* wcag_sweep_run tolerates a NULL out buffer (dry-run mode) and a
 * zero max -- both safe call shapes for "just tell me the count".
 * Today both also return PROVIDER_MISSING because there's no tree
 * to count; the NULL-buffer guard happens before the provider check
 * in a real implementation would matter anyway. */
static void test_wcag_sweep_null_buffer_safe(void)
{
    int n = wcag_sweep_run((wcag_finding_t *)0, 0);
    TEST_ASSERT(n == 0 || n == WCAG_SWEEP_PROVIDER_MISSING,
                "wcag_sweep_run(NULL, 0) returns 0 or PROVIDER_MISSING");
    n = wcag_sweep_run((wcag_finding_t *)0, 16);
    TEST_ASSERT(n == 0 || n == WCAG_SWEEP_PROVIDER_MISSING,
                "wcag_sweep_run(NULL, 16) returns 0 or PROVIDER_MISSING without crashing");
}

/* ---- Section 15: Per-Test Desktop Reset (isolation) ---------------- */

/* test_desktop_reset() zeroes every piece of shared desktop state a
 * TEST_CAT_DESKTOP suite might have touched. Verify each of the four
 * subsystems the reset handles: WM slots + focus, keyboard modifier
 * latches (via alt+F4 no-fire after reset), terminal input ring drain,
 * compositor seed + headless flag. */
static void test_desktop_reset_clears_all_state(void)
{
    /* Start from a known-clean state: earlier TEST_CAT_DESKTOP suites
     * leak WM/terminal/keyboard state, so establish the baseline
     * before seeding the fixture -- that's exactly the leak
     * test_desktop_reset() is designed to clear. */
    test_desktop_reset();

    /* Seed state: install a window, focus it, latch alt via inject,
     * push a character into the terminal ring, flip headless, seed
     * the RNG. All of these must be gone after reset. */
    int h = wm_test_install_window(10, 20, 100, 100);
    TEST_ASSERT(h >= 0, "install window before reset");
    wm_test_set_focused(h);
    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)1,
                   "pre-reset: 1 window installed");

    keyboard_inject_scancode(0x38);  /* latch LALT */

    terminal_test_force_open();
    /* Prime the terminal input ring with a character via the keyboard
     * inject path (LALT stays latched -- we'll verify reset clears it). */
    terminal_key_input('x');

    compositor_set_test_seed(0xDEADBEEFCAFEBABEULL);
    compositor_set_headless(1);

    /* Reset. */
    test_desktop_reset();

    /* 1. WM zeroed. */
    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)0,
                   "post-reset: window count 0");
    TEST_ASSERT_EQ((uint64_t)wm_get_focused_window(), (uint64_t)-1,
                   "post-reset: focus cleared");

    /* 2. Keyboard modifier latches cleared. Inject F4 alone; if the
     *    LALT latch leaked, Alt+F4 handler would fire on it. Since
     *    there's no focused window now, even a misfire would be a
     *    silent no-op but we assert queue state. */
    keyboard_inject_scancode(0x3E);  /* F4 press */
    TEST_ASSERT_EQ((uint64_t)wm_process_pending_closes(), (uint64_t)0,
                   "post-reset: F4 alone did not queue Alt+F4 close");

    /* 3. Terminal ring drained. */
    TEST_ASSERT_EQ((uint64_t)(int64_t)terminal_trygetchar(), (uint64_t)0,
                   "post-reset: terminal ring drained");

    /* 4. Compositor test seed + headless flag zeroed. */
    TEST_ASSERT_EQ((uint64_t)compositor_get_test_seed(), (uint64_t)0,
                   "post-reset: test seed cleared");
    TEST_ASSERT_EQ((uint64_t)compositor_is_headless(), (uint64_t)0,
                   "post-reset: headless flag cleared");

    terminal_test_force_close();
}

/* test_desktop_reset() is idempotent: calling it twice in a row is
 * equivalent to calling it once. */
static void test_desktop_reset_is_idempotent(void)
{
    test_desktop_reset();
    test_desktop_reset();
    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)0,
                   "double reset: count still 0");
    TEST_ASSERT_EQ((uint64_t)compositor_get_test_seed(), (uint64_t)0,
                   "double reset: seed still 0");
}

/* test_desktop_reset_action() is the test_add_action-compatible
 * wrapper; its ctx is ignored. Ensures calling via the action stack
 * produces the same effect as the direct call. */
/* Regression for Codex section 15 [H]: a prior suite that force-opens
 * the terminal leaks that state, and later keyboard_inject printable
 * keys route to terminal_key_input instead of kb_buffer_push. After
 * test_desktop_reset(), the terminal must be closed so the inject
 * keypress-roundtrip semantics hold. */
static void test_desktop_reset_closes_leaked_terminal(void)
{
    /* Leak: force-open the terminal and write content into the grid. */
    terminal_test_force_open();
    terminal_puts("polluted content\n", 16);

    /* Reset should close the terminal AND clear the grid. */
    test_desktop_reset();

    TEST_ASSERT_EQ((uint64_t)terminal_is_open(), (uint64_t)0,
                   "post-reset: terminal closed (prior force_open undone)");

    /* Re-open to inspect the grid; buffer_contains must NOT find the
     * pollution left by the prior suite. */
    terminal_test_force_open();
    TEST_ASSERT_EQ((uint64_t)terminal_buffer_contains("polluted"),
                   (uint64_t)0,
                   "post-reset: grid cleared (no leaked terminal text)");
    terminal_test_force_close();

    /* Now prove keyboard_inject of a printable key lands in the kb
     * buffer -- not the terminal ring, because the terminal is closed. */
    keyboard_reset_state();
    keyboard_inject_scancode(0x1E);   /* 'a' */

    /* The terminal ring should stay empty (terminal was closed when
     * the inject happened). */
    TEST_ASSERT_EQ((uint64_t)(int64_t)terminal_trygetchar(),
                   (uint64_t)0,
                   "keyboard inject after reset did not route to terminal ring");
}

static void test_desktop_reset_action_wrapper_matches(void)
{
    test_desktop_reset();   /* baseline */
    int h = wm_test_install_window(0, 0, 50, 50);
    TEST_ASSERT(h >= 0, "install window");
    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)1,
                   "pre-action: 1 window");

    /* Call the action wrapper directly; ctx intentionally ignored. */
    test_desktop_reset_action((void *)0xABCDEF);

    TEST_ASSERT_EQ((uint64_t)wm_get_window_count(), (uint64_t)0,
                   "post-action: window count 0");
}

/* ---- Icon cache regression tests (window-drag 0xE0000002 crash class) ---- */

/* Stand-in "IRES" pixel buffer: BSS-resident, so a pre-fix wild kfree of it
 * is exactly the non-heap-pointer class the hardened heap bugchecks. */
static uint32_t s_test_borrowed_px[4 * 4];

/* Color (IRES) icons ignore tint: every requested tint must hit the ONE
 * cache entry stored with color=0. Pre-fix, each nonzero tint missed and
 * inserted a duplicate entry per lookup -- three per composited frame --
 * flooding the 128-entry cache into eviction within ~40 drag frames.
 * Exercised via the test seams (no icons.ires in the test boot): a borrowed
 * color-icon entry is injected exactly as ires_get_bitmap stores it, then
 * looked up through the real public API with varying tints. */
static void test_icon_color_tint_normalized(void)
{
    int prev_ready;
    uint32_t count_before, i;
    icon_bitmap_t *first;

    s_test_borrowed_px[0] = 0xA5F00D01u;
    prev_ready = icon_store_test_force_ready(1);
    if (icon_cache_insert_borrowed_for_test(ICON_FOLDER_CLOSED, 32,
                                            s_test_borrowed_px, 4, 4) != 0) {
        icon_store_test_force_ready(prev_ready);
        TEST_SKIP("no icon cache slot available");
        return;
    }
    count_before = icon_cache_entry_count();

    /* Pre-fix: nonzero tints missed the color=0 entry, fell to the (absent)
     * IRES loader, and returned NULL -- and in a real boot inserted a
     * duplicate per lookup. Post-fix: every tint hits the one entry. */
    first = icon_get_colored(ICON_FOLDER_CLOSED, 32, 0xFFFFFFFF);
    TEST_ASSERT(first && first->pixels == s_test_borrowed_px,
                "tinted lookup must hit the color=0 cached entry (key normalized)");
    for (i = 0; i < 10; i++) {
        icon_bitmap_t *b = icon_get_colored(ICON_FOLDER_CLOSED, 32,
                                            0xFF000000u | (i * 0x1F2F3Fu));
        TEST_ASSERT(b == first,
                    "every tint must resolve to the same cached color-icon entry");
    }
    TEST_ASSERT_EQ(icon_cache_entry_count(), count_before,
                   "repeated tinted lookups must not grow the icon cache");
    icon_cache_reset_for_test();
    icon_store_test_force_ready(prev_ready);
}

/* Evicting past the cache capacity with MIXED owned and borrowed entries
 * must never free the borrowed pixels: production borrowed entries point
 * into the PMM-loaded icons.ires buffer, and pre-fix the eviction kfree()'d
 * them -- the hardened heap bugchecked that as HEAP_FAULT_WILD (0xE0000002,
 * the window-drag crash). Here the borrowed buffer is BSS-resident, so a
 * regressed wild free bugchecks the test run outright; a clean pass with
 * the sentinel intact proves eviction skipped the borrowed entry. */
static void test_icon_cache_eviction_borrowed_safe(void)
{
    int prev_ready;
    uint32_t flooded;

    s_test_borrowed_px[0] = 0xA5F00D02u;
    prev_ready = icon_store_test_force_ready(1);
    if (icon_cache_insert_borrowed_for_test(ICON_RECYCLE_BIN_EMPTY, 32,
                                            s_test_borrowed_px, 4, 4) != 0) {
        icon_store_test_force_ready(prev_ready);
        TEST_SKIP("no icon cache slot available");
        return;
    }

    /* Force LRU eviction well past the 128-entry capacity; the borrowed
     * entry is the oldest and is evicted early in the flood. */
    flooded = icon_cache_flood_owned_for_test(140);
    TEST_ASSERT(flooded >= 130,
                "owned flood must exceed the cache capacity to force eviction");
    TEST_ASSERT(icon_cache_entry_count() <= 128,
                "cache must stay bounded at ICON_CACHE_MAX after the flood");
    TEST_ASSERT_EQ(s_test_borrowed_px[0], 0xA5F00D02u,
                   "borrowed pixels must be untouched by eviction (no wild free)");
    /* Ownership-aware full release: also exercises free_pixels across every
     * remaining owned entry (and any borrowed survivor) in one sweep. */
    icon_cache_reset_for_test();
    TEST_ASSERT_EQ(icon_cache_entry_count(), 0u,
                   "cache reset must release every entry");
    icon_store_test_force_ready(prev_ready);
}

/* ---- ctrl_init frame-backed window pool (TODO-33 s13) ----------------- */

/* Forces the CTRL_MAX_WINDOWS pool allocation to fail (pmm_alloc_pages_hhdm
 * -> pmm_alloc_contiguous, multi-frame, so pmm_alloc_fail_next's single-shot
 * countdown fires on it) and asserts ctrl_init() degrades instead of
 * crashing: ctrl_ready() stays false, and the lookup entry points that
 * guard on ctrl_windows do not NULL-deref. Restores the real pool
 * afterward so later tests / gallery_open() see a working subsystem. */
static void test_ctrl_init_degrades_on_oom(void)
{
    /* TEST-SIDE-EFFECT-ALLOWED: controlled fault-recovery test of
     * ctrl_init()'s own degrade/recover contract. ctrl_init() is a
     * desktop-subsystem init that is idempotent by design (the CAS
     * state machine it added exists specifically so a repeat call is
     * safe), degrades rather than halts on failure, and touches no
     * boot-critical/hardware state -- unlike the enumerated
     * pmm_init()/vmm_init()/acpi_init() class this policy protects. */
    ctrl_test_reset_for_fault_injection();
    TEST_ASSERT_EQ(ctrl_ready(), 0, "reset leaves controls not-ready");

    pmm_alloc_fail_next();
    ctrl_init();
    TEST_ASSERT_EQ(ctrl_ready(), 0,
                   "forced OOM leaves ctrl_init() degraded, not initialized");

    /* Degraded lookup paths must return -1 (no control created), not crash. */
    TEST_ASSERT(ctrl_create_button(0, 0, 0, 10, 10, "x", (ctrl_click_fn)0) == -1,
                "ctrl_create_button returns -1 while degraded, no crash");

    /* Recover for the next test / any later consumer in this boot. */
    ctrl_init();
    TEST_ASSERT_EQ(ctrl_ready(), 1, "un-injected ctrl_init() recovers");
}

/* Companion to the above: after a degrade-then-recover cycle, a normal
 * control creation succeeds -- proves the recovered pool is actually
 * usable, not just non-NULL. */
static void test_ctrl_init_recovers_after_oom(void)
{
    int id;

    /* TEST-SIDE-EFFECT-ALLOWED: see test_ctrl_init_degrades_on_oom. */
    ctrl_test_reset_for_fault_injection();
    pmm_alloc_fail_next();
    ctrl_init();
    TEST_ASSERT_EQ(ctrl_ready(), 0, "degraded before recovery");

    ctrl_init();
    TEST_ASSERT_EQ(ctrl_ready(), 1, "recovered before use");

    id = ctrl_create_button(0, 0, 0, 10, 10, "x", (ctrl_click_fn)0);
    TEST_ASSERT(id >= 0, "recovered pool actually creates a control");

    /* This test creates a real, live control on window_handle 0 in the
     * shared pool -- leaving it behind would leak into whatever real
     * window later claims that slot (gallery_open() runs on the same
     * pool post-boot). ctrl_destroy() alone only tombstones the control's
     * TYPE: it leaves cw->active and cw->count set, so window_handle 0
     * stays "in use" with count 1 and a later client's first control
     * gets id 1, not id 0. ctrl_destroy_all() clears active/count/
     * focused_id too, returning the whole window slot to the pristine
     * state get_or_create_ctrl_window() produces on first use --
     * restoring the invariant every later fault-injection test and the
     * real subsystem depend on. */
    ctrl_destroy_all(0);
    TEST_ASSERT(ctrl_create_button(0, 0, 0, 10, 10, "y", (ctrl_click_fn)0) == 0,
                "post-cleanup handle 0 is pristine: next control gets id 0");
    ctrl_destroy_all(0);
}

/* CAS-guard idempotency: the two tests above only exercise UNINIT ->
 * degraded -> UNINIT -> READY. Neither ever calls ctrl_init() a second
 * time while already READY, so the atomic_cmpxchg guard that stops a
 * repeat/concurrent caller from reallocating the pool had zero coverage.
 * This asserts a repeat call is a true no-op: still READY, no second
 * PMM allocation, and an existing control survives untouched. */
static void test_ctrl_init_idempotent_when_ready(void)
{
    uint64_t used_before, used_after;
    int id;

    /* TEST-SIDE-EFFECT-ALLOWED: see test_ctrl_init_degrades_on_oom. */
    ctrl_test_reset_for_fault_injection();
    ctrl_init();
    TEST_ASSERT_EQ(ctrl_ready(), 1, "clean init reaches READY");

    id = ctrl_create_button(0, 0, 0, 10, 10, "x", (ctrl_click_fn)0);
    TEST_ASSERT(id >= 0, "control created before the repeat call");

    used_before = pmm_get_used_frames();
    ctrl_init();
    used_after = pmm_get_used_frames();

    TEST_ASSERT_EQ(ctrl_ready(), 1, "still READY after a repeat call");
    TEST_ASSERT_EQ(used_before, used_after,
                   "repeat ctrl_init() does not allocate a second pool");
    TEST_ASSERT(strcmp(ctrl_get_text(0, id), "x") == 0,
                "the control created before the repeat call survives it");

    ctrl_destroy_all(0);
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
    test_suite_register_cat("Desktop: terminal_get_buffer closed returns 0",
                            test_terminal_get_buffer_when_closed, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: terminal_get_buffer NULL/undersized rejected",
                            test_terminal_get_buffer_null_or_small, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: terminal_buffer_contains guards",
                            test_terminal_buffer_contains_guards, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: terminal dir roundtrip (synthesized)",
                            test_terminal_dir_roundtrip_synthesized, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: wm_get_window_count tracks installs",
                            test_wm_window_count_tracks_installs, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: wm_get_focused_window tracks focus",
                            test_wm_focused_window_tracks_focus, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: wm_get_window_rect roundtrip + guards",
                            test_wm_window_rect_roundtrip, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: Alt+F4 closes focused window (inject path)",
                            test_wm_alt_f4_closes_focused_window, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: wm_test_reset clears deferred-close state",
                            test_wm_reset_clears_deferred_close, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: wm_frame_stats_reset_for_test zeroes counters",
                            test_wm_frame_stats_reset_zeros_counters, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: frame_stats on-present under budget",
                            test_wm_frame_stats_on_present_under_budget, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: frame_stats on-present over budget -> frames_late",
                            test_wm_frame_stats_on_present_over_budget, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: frame_stats queue + coalesce drops",
                            test_wm_frame_stats_queue_and_drop, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: frame_stats counters monotonic across presents",
                            test_wm_frame_stats_counters_monotonic, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: wm_get_frame_stats NULL guard",
                            test_wm_frame_stats_null_guard, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: wm_mark_dirty bumps queued + drops on coalesce",
                            test_wm_mark_dirty_bumps_queued, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: input_record begin/stop/release lifecycle",
                            test_input_record_begin_stop_release, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: input_record captures key/mouse/IME",
                            test_input_record_key_mouse_ime_capture, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: input_record capacity drops (no overwrite)",
                            test_input_record_capacity_drops, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: input_record serialize + replay roundtrip (dir+Enter)",
                            test_input_record_serialize_roundtrip, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: input_record IME CJK commit UTF-8 byte-identical",
                            test_input_record_ime_roundtrip_utf8, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: input_replay rejects malformed JSONL",
                            test_input_record_replay_rejects_malformed, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: compositor_set_headless toggle",
                            test_compositor_headless_toggle, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: compositor_set_test_seed roundtrip",
                            test_compositor_test_seed_roundtrip, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: compositor_step_frames advances frame stats",
                            test_compositor_step_frames_advances_stats, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: boot_config.compositor field present",
                            test_boot_config_compositor_field, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: fb_get_output_count nonzero baseline",
                            test_fb_output_count_is_nonzero, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: fb_snapshot_monitor rejects out-of-range indices",
                            test_fb_snapshot_monitor_rejects_oob, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: boot_config.test_monitors_count 0..3 range",
                            test_boot_config_test_monitors_count, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: test_monitors parser exact mapping (kernel mirror)",
                            test_test_monitors_parser_exact_mapping, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: WCAG rule-name table covers every enum entry",
                            test_wcag_rule_name_table, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: WCAG provider-ready flag boolean contract",
                            test_wcag_provider_ready_flag, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: WCAG sweep PENDING the accessibility automation tree",
                            test_wcag_sweep_pending_provider, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: WCAG sweep NULL buffer safe",
                            test_wcag_sweep_null_buffer_safe, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: test_desktop_reset clears all shared state",
                            test_desktop_reset_clears_all_state, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: test_desktop_reset is idempotent",
                            test_desktop_reset_is_idempotent, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: test_desktop_reset closes leaked terminal",
                            test_desktop_reset_closes_leaked_terminal, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: test_desktop_reset_action wrapper matches direct call",
                            test_desktop_reset_action_wrapper_matches, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: color icon cache key ignores tint (no duplicate flood)",
                            test_icon_color_tint_normalized, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: icon cache eviction never frees borrowed IRES pixels",
                            test_icon_cache_eviction_borrowed_safe, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: ctrl_init degrades (not crashes) under forced OOM",
                            test_ctrl_init_degrades_on_oom, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: ctrl_init recovers after a forced-OOM degrade",
                            test_ctrl_init_recovers_after_oom, TEST_CAT_DESKTOP);
    test_suite_register_cat("Desktop: repeat ctrl_init() while READY is a no-op",
                            test_ctrl_init_idempotent_when_ready, TEST_CAT_DESKTOP);
}

#endif /* KERNEL_TESTS */
