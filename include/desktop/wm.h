/* ============================================================================
 * wm.h -- Stacking window manager
 *
 * Manages overlapping windows with per-window framebuffers, title bars,
 * decorations, z-order stacking, dragging, and painter's-algorithm
 * compositing onto the screen framebuffer.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Constants ---- */

#define WM_MAX_WINDOWS     32
#define WM_TITLE_MAX       64
#define WM_TITLEBAR_HEIGHT 32
#define WM_BORDER_WIDTH    1
#define WM_CORNER_RADIUS   6
#define WM_BTN_WIDTH       46     /* Windows 11 caption button width */
#define WM_BTN_HEIGHT      32     /* same as title bar */
#define WM_RESIZE_MARGIN   5      /* grab zone for edge/corner resize */

/* Window flags */
#define WM_FLAG_VISIBLE    0x01
#define WM_FLAG_DECORATED  0x02    /* has title bar + border */
#define WM_FLAG_MOVABLE    0x04
#define WM_FLAG_RESIZABLE  0x08
#define WM_FLAG_FOCUSED    0x10
#define WM_FLAG_DIALOG     0x20    /* dialog: close-only, cached acrylic bg */

#define WM_DEFAULT_FLAGS   (WM_FLAG_VISIBLE | WM_FLAG_DECORATED | \
                            WM_FLAG_MOVABLE | WM_FLAG_RESIZABLE)

#define WM_DIALOG_FLAGS    (WM_FLAG_VISIBLE | WM_FLAG_DECORATED | \
                            WM_FLAG_MOVABLE | WM_FLAG_DIALOG)

/* ---- Color palette for decorations ---- */

/* Windows 11 Dark Theme -- neutral grays */
#define WM_COLOR_TITLEBAR_ACTIVE   0xFF202020   /* active fallback (Mica overrides) */
#define WM_COLOR_TITLEBAR_INACTIVE 0xFF383838   /* inactive: flat gray, no Mica    */
#define WM_COLOR_TITLE_TEXT        0xFFFFFFFF   /* active title text         */
#define WM_COLOR_TITLE_INACTIVE    0xFF999999   /* inactive title text       */
#define WM_COLOR_BORDER_ACTIVE     0xFF3A3A3A   /* subtle dark outline       */
#define WM_COLOR_BORDER_INACTIVE   0xFF333333   /* inactive border           */
#define WM_COLOR_CLOSE_HOVER_BG    0xFFC42B1C   /* Win11 close hover red     */
#define WM_COLOR_BTN_HOVER_BG      0x30FFFFFF   /* subtle white glow         */
#define WM_COLOR_BTN_GLYPH         0xFFFFFFFF   /* button glyph color        */
#define WM_COLOR_BTN_GLYPH_DIM     0xFF999999   /* inactive glyph            */
#define WM_COLOR_CLIENT_BG         0xFF202020   /* client area background    */

/* ---- Window struct ---- */

struct wm_window {
    int32_t  x, y;                  /* position (top-left, outer) */
    uint32_t width, height;         /* client area size (pixels) */
    uint32_t flags;
    char     title[WM_TITLE_MAX];
    uint32_t *framebuffer;          /* per-window pixel buffer (client area) */
    uint32_t fb_pitch;              /* pixels per row in framebuffer */
    int32_t  z_order;               /* higher = on top */
    uint8_t  active;                /* 1 = in use */

    /* Internal drag state */
    uint8_t  dragging;
    int32_t  drag_offset_x;
    int32_t  drag_offset_y;

    /* Button hover state (for Windows 11 highlight-on-hover) */
    uint8_t  close_hover;
    uint8_t  min_hover;
    uint8_t  max_hover;

    /* Dialog acrylic cache (pre-blurred wallpaper snapshot) */
    uint32_t *acrylic_cache;        /* PMM buffer: blurred wallpaper */
    uint32_t  acrylic_w, acrylic_h; /* cached texture dimensions */
    uint8_t   acrylic_dirty;        /* 1 = needs re-blur */

    /* Mica title bar: cached flat tint color from wallpaper */
    uint32_t  mica_color;           /* 0 = not computed yet */
};

/* ---- Lifecycle ---- */

/* Initialize the window manager (call after fb_init + heap_init) */
void wm_init(void);

/* ---- Window management ---- */

/* Create a window. Returns a handle (index) or -1 on failure. */
int wm_create_window(const char *title, int32_t x, int32_t y,
                     uint32_t width, uint32_t height, uint32_t flags);

/* Destroy a window and free its buffer. */
void wm_destroy_window(int handle);

/* ---- Manipulation ---- */

/* Move a window to a new position. */
void wm_move_window(int handle, int32_t x, int32_t y);

/* Resize a window (reallocates the framebuffer). */
void wm_resize_window(int handle, uint32_t width, uint32_t height);

/* Bring a window to the front. */
void wm_raise_window(int handle);

/* Set focus to a window. */
void wm_focus_window(int handle);

/* ---- Rendering ---- */

/* Composite all windows onto the screen framebuffer (painter's algorithm).
 * Call this once per frame. The caller should call fb_swap() afterward. */
void wm_composite(void);

/* Force a redraw on the next wm_composite() call. Counts as a queued
 * frame event for the frame-stats oracle; use wm_mark_dirty_internal()
 * for compositor-internal reinvalidation that should NOT bump the
 * external queued-frame counter. */
void wm_mark_dirty(void);

/* Compositor-internal dirty mark: sets needs_redraw without advancing
 * the frame-stats oracle. Used by the compositor loop when pending
 * close drains or similar self-initiated invalidations need a full
 * repaint without masquerading as a new external frame request. */
void wm_mark_dirty_internal(void);

/* Check if a redraw is pending (content changed). */
int wm_needs_redraw(void);

/* Get a window's client-area framebuffer for drawing into. */
uint32_t *wm_get_framebuffer(int handle);

/* Get the client area dimensions. */
uint32_t wm_get_client_width(int handle);
uint32_t wm_get_client_height(int handle);

/* ---- Frame timing + drop oracle (desktop UI test section 10) ---- */

/* Monotonic frame counters. Parity with Win11
 * DwmGetCompositionTimingInfo and Linux Wayland presentation-time
 * protocol. All values are monotonic since boot (or since the last
 * wm_frame_stats_reset_for_test call under KERNEL_TESTS).
 *
 * `last_vsync_qpc` / `last_present_qpc` are mono_ns() timestamps
 * ("QPC" in the field name is a Win32-terminology nod; the unit is
 * nanoseconds, not the raw QueryPerformanceCounter tick count, because
 * our monotonic clock normalizes away TSC/HPET source differences). */
struct wm_frame_stats {
    uint64_t frames_presented;
    uint64_t frames_queued;
    uint64_t frames_late;
    uint64_t frames_dropped;
    uint64_t last_vsync_qpc;
    uint64_t last_present_qpc;
};

/* SMP-safe snapshot of the frame stats. Readers loop over a seqlock
 * so a concurrent compositor thread updating the counters mid-read
 * does not produce a torn mix of old + new field values. The out
 * pointer must not be NULL. */
void wm_get_frame_stats(struct wm_frame_stats *out);

/* Writer-side: called by wm_mark_dirty() whenever the compositor is
 * notified of a render-needing change. `was_already_dirty` is 1 when
 * the previous mark has not yet been composited (the new mark is
 * coalesced and counts as a drop). */
void wm_frame_stats_on_mark_dirty(int was_already_dirty);

/* Writer-side: called by the compositor once per successful present
 * (post fb_swap()). `vsync_ns` is the monotonic timestamp at frame
 * start; `present_ns` is the timestamp right after the back-buffer
 * flip. If `present_ns - vsync_ns` exceeds the 16.67 ms 60 Hz budget,
 * frames_late is bumped. This is the single place frames_presented
 * advances. */
void wm_frame_stats_on_present(uint64_t vsync_ns, uint64_t present_ns);

#ifdef KERNEL_TESTS
/* Test-only: zero every counter so a TEST_CAT_DESKTOP case can
 * exercise monotonic progress from a known baseline. Paired with
 * wm_frame_stats_on_mark_dirty / wm_frame_stats_on_present which
 * stay callable from tests to synthesize deterministic frame history
 * without waiting for the real compositor loop. */
void wm_frame_stats_reset_for_test(void);
#endif

/* ---- Introspection (test framework -- desktop UI test section 8) ---- */

/* Count currently-active windows (0 when WM is uninitialized or all
 * slots are free). Used by tests and diagnostics to assert WM state
 * without snapshotting the framebuffer. */
int wm_get_window_count(void);

/* Return the handle of the currently-focused window, or -1 if no window
 * has focus (no windows exist, or focus was just cleared). */
int wm_get_focused_window(void);

/* Fill `*x`, `*y`, `*w`, `*h` with the outer (decoration-inclusive)
 * position and client-area size of `handle`. Returns 0 on success,
 * -1 when handle is out-of-range or inactive, -2 when any out-pointer
 * is NULL. On failure the out-pointers are not written. */
int wm_get_window_rect(int handle, int32_t *x, int32_t *y,
                       uint32_t *w, uint32_t *h);

/* Post a deferred close request for whichever window is currently
 * focused. IRQ-safe: only records the focused handle into a single-slot
 * pending-close queue. Actual destruction happens on the next call to
 * wm_process_pending_closes() from thread context, NOT from the IRQ
 * that posted the request.
 *
 * Why deferred: wm_destroy_window() calls pmm_free_pages(), and the
 * compositor walks the windows[] array without IRQ masking. Running
 * destruction directly from the keyboard IRQ races both the PMM bitmap
 * and the compositor's traversal; Codex [H] adversarial review of
 * the desktop UI test framework WM-state-verification section flagged
 * the earlier direct-destroy draft. */
void wm_close_focused_window(void);

/* Drain the pending-close queue in thread context and destroy any
 * window whose close was requested since the last drain. Called once
 * per compositor frame (before wm_composite) so Alt+F4 user-visible
 * latency stays within one frame. Returns the number of windows
 * actually destroyed (0 or 1 today; coalesced multi-close can land
 * once the queue is widened). */
int wm_process_pending_closes(void);

#ifdef KERNEL_TESTS
/* Test-only seam: initialize WM state without allocating framebuffers or
 * calling wm_init(). Lets kernel tests exercise wm_get_* on a synthetic
 * window set in Phase 3, before boot_desktop.c brings the compositor up.
 * Paired with wm_test_install_window / wm_test_set_focused. */
void wm_test_reset(void);

/* Install a synthetic window at the next free slot (or `-1` if full).
 * No framebuffer is allocated; only `x/y/width/height/active/flags` are
 * set. The returned handle is stable across subsequent installs in the
 * same run. Returns -1 on overflow. */
int wm_test_install_window(int32_t x, int32_t y, uint32_t w, uint32_t h);

/* Force `focused_window` to the given handle without dispatching any
 * side-effects (no mark_dirty, no flag toggles on other slots beyond
 * the focus bit). The handle must refer to a slot marked active by
 * wm_test_install_window. */
void wm_test_set_focused(int handle);
#endif

/* Write a pixel to a window's client-area framebuffer. */
void wm_put_pixel(int handle, uint32_t x, uint32_t y, uint32_t color);

/* Fill a rectangle in a window's client area. */
void wm_fill_rect(int handle, uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h, uint32_t color);

/* ---- Input dispatching ---- */

/* Process mouse input -- handles dragging, focus, button clicks.
 * Call once per frame with current mouse state. */
void wm_handle_mouse(int32_t mx, int32_t my, uint8_t buttons);

/* Find which window is under the given screen coordinate.
 * Returns handle or -1 if none. */
int wm_window_at(int32_t x, int32_t y);

/* Determine cursor shape based on what's under the pointer.
 * Returns the appropriate cursor_shape_t for resize edges, close button, etc. */
#include "cursor.h"
cursor_shape_t wm_get_cursor_context(int32_t mx, int32_t my);

/* ---- Drag dirty-rect API ---- */

/* Returns 1 if a window is currently being dragged. */
int wm_is_dragging(void);

/* Get the dirty rect (union of old + new window positions) for the current drag.
 * Returns 1 and fills out params if drag is active, 0 otherwise. */
int wm_get_drag_dirty_rect(int32_t *out_x, int32_t *out_y,
                            uint32_t *out_w, uint32_t *out_h);
