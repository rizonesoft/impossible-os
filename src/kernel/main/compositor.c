/* ============================================================================
 * compositor.c -- Main desktop compositor event loop
 *
 * Never returns.  Runs as the idle/main kernel thread after all boot
 * phases complete.  Handles mouse input, composites windows, flips the
 * framebuffer, and manages cursor drawing.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/drivers/mouse.h"
#include "kernel/drivers/virtio_input.h"
#include "kernel/drivers/vbox_mouse.h"
#include "kernel/drivers/rtc.h"
#include "kernel/time/mono_clock.h"
#include "kernel/timer.h"
#include "kernel/sched/task.h"
#include "kernel/klog.h"
#include "cursor.h"
#include "desktop/wm.h"
#include "desktop/terminal.h"
#include "desktop/gallery.h"
#include "desktop/desktop.h"
#include "registry.h"
#include "kernel/drivers/ahci.h"
#include "main/main_internal.h"
#include "kernel/cpuid_platform.h"

/* Headless state + test seed. All reads/writes are plain volatile
 * because tests + compositor thread never race on them today:
 * `compositor_set_headless` is called from boot_desktop.c BEFORE the
 * compositor loop starts, and `compositor_step_frames` is called
 * either from the test phase (pre-compositor) or from a thread while
 * the compositor is HLT-idling in headless mode. */
static volatile int      s_compositor_headless = 0;
static volatile uint64_t s_compositor_test_seed = 0;

void compositor_set_headless(int headless)
{
    s_compositor_headless = headless ? 1 : 0;
}

int compositor_is_headless(void)
{
    return s_compositor_headless;
}

void compositor_set_test_seed(uint64_t seed)
{
    s_compositor_test_seed = seed;
}

uint64_t compositor_get_test_seed(void)
{
    return s_compositor_test_seed;
}

uint32_t compositor_step_frames(uint32_t n)
{
    /* Hard reject when the normal compositor loop might be running:
     * step_frames walks `windows[]` and dereferences per-window
     * framebuffer pointers, which `wm_destroy_window()` frees under
     * the main compositor's `scheduler_disable()` guard. Running a
     * second presenter concurrently with compositor_run() can
     * produce use-after-free, torn composites, double swaps, and
     * corrupt frame-stats. Headless mode suppresses compositor_run
     * so step_frames is the only presenter -- that is the only safe
     * configuration for this entry point. Codex [H] adversarial
     * review of §12 required this refusal. */
    if (!s_compositor_headless)
        return 0;

    /* Same scheduler_disable pattern the main compositor uses around
     * wm_process_pending_closes + wm_composite so even headless-mode
     * callers that race a concurrent wm_mark_dirty() / destroy on
     * another CPU never observe a torn windows[] traversal. */
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint64_t vsync_ns = mono_ns();

        scheduler_disable();
        wm_process_pending_closes();
        wm_mark_dirty_internal();
        wm_composite();
        /* Skip the real framebuffer swap when running headless. The
         * composite still touches the WM back buffer so tests can
         * hash the output via fb_snapshot(), but no VRAM flip
         * happens -- that is exactly the speed win headless mode
         * buys. Under headless we always skip; the branch here is
         * belt + suspenders for the future case where step_frames
         * is wired into a live-display debug path. */
        if (!s_compositor_headless)
            fb_swap();
        scheduler_enable();

        /* Advance the §10 frame-timing oracle. Present timestamp is
         * sampled AFTER the optional swap so `last_present_qpc`
         * reflects the point the work is done, matching the main
         * compositor loop's call-site. */
        wm_frame_stats_on_present(vsync_ns, mono_ns());
    }
    return i;
}

void compositor_run(void)
{
    if (s_compositor_headless) {
        /* Headless mode: the BSP's real compositor loop is
         * suppressed. Tests / external drivers own frame
         * advancement via compositor_step_frames(). The function
         * must NOT return because boot_desktop.c calls it as a
         * never-returns tail; HLT-idle until a panic or shutdown. */
        klog(LOG_INFO, "compositor",
             "headless mode: BSP idles -- tests drive "
             "compositor_step_frames() for rendering");
        for (;;)
            __asm__ volatile ("sti; hlt");
    }

    fb_fill_rect(0, 0, fb_get_width(), fb_get_height(), 0x00000000);

    int32_t prev_mx = -1, prev_my = -1;
    uint8_t prev_mb = 0;
    uint8_t first_frame = 1;
    uint64_t last_clock_sec = 0;
    uint8_t last_clock_min = 0xFF;  /* force first draw */

    for (;;) {
        int32_t mx, my;
        uint8_t mb;

        /* Get mouse state from the best available source:
         *   1. VirtIO tablet (QEMU) -- absolute coordinates
         *   2. VBox VMMDev mouse (VirtualBox) -- absolute coordinates
         *   3. PS/2 mouse (fallback) -- relative deltas */
        if (virtio_input_available()) {
            struct virtio_input_state vis = virtio_input_get_state();
            mx = vis.x;
            my = vis.y;
            mb = vis.buttons;
            /* Update PS/2 mouse state for cursor position tracking */
            mouse_set_position(mx, my);
        } else if (vbox_mouse_available()) {
            struct mouse_state vb = vbox_mouse_get_state();
            mx = vb.x;
            my = vb.y;
            mb = vb.buttons;
            mouse_set_position(mx, my);
        } else {
            struct mouse_state ms = mouse_get_state();
            mx = ms.x;
            my = ms.y;
            mb = ms.buttons;
        }

        /* Batch mouse events: after reading, briefly yield to let
         * any additional IRQ deltas arrive, then re-read.
         * This turns 5x1px moves into 1x5px = 1 composite.
         * TCG is 10-50x slower -- batch more aggressively to avoid
         * wasting cycles on per-pixel composites. */
        {
            int batch;
            int max_batch = platform_is_tcg() ? 12 : 4;
            for (batch = 0; batch < max_batch; batch++) {
                int32_t nx, ny;
                uint8_t nb;
                /* Allow IRQs to fire */
                __asm__ volatile ("sti; hlt");
                /* Re-read */
                if (virtio_input_available()) {
                    struct virtio_input_state vis =
                        virtio_input_get_state();
                    nx = vis.x; ny = vis.y; nb = vis.buttons;
                    mouse_set_position(nx, ny);
                } else if (vbox_mouse_available()) {
                    struct mouse_state vb = vbox_mouse_get_state();
                    nx = vb.x; ny = vb.y; nb = vb.buttons;
                    mouse_set_position(nx, ny);
                } else {
                    struct mouse_state ms = mouse_get_state();
                    nx = ms.x; ny = ms.y; nb = ms.buttons;
                }
                /* If no new movement, stop draining */
                if (nx == mx && ny == my && nb == mb)
                    break;
                mx = nx; my = ny; mb = nb;
            }
        }

        uint8_t cursor_moved = (mx != prev_mx || my != prev_my);
        uint8_t btn_changed  = (mb != prev_mb);

        /* Check if clock needs update (only when minute changes) */
        uint64_t cur_sec = uptime();
        uint8_t clock_tick = 0;
        if (cur_sec != last_clock_sec) {
            last_clock_sec = cur_sec;
            struct rtc_time rtc_now;
            rtc_read(&rtc_now);
            if (rtc_now.minute != last_clock_min) {
                last_clock_min = rtc_now.minute;
                clock_tick = 1;
            }
        }

        /* Check if any window content changed (e.g., terminal output) */
        uint8_t wm_dirty = wm_needs_redraw();

        /* Always dispatch mouse events when cursor moves
         * (hover tracking needs this even without buttons) */
        if (cursor_moved || btn_changed) {
            if (!desktop_handle_click(mx, my, mb)) {
                wm_handle_mouse(mx, my, mb);
            }
        }

        /* Check if any window content changed (includes hover) */
        uint8_t wm_dirty2 = wm_needs_redraw();

        /* Only do work if something actually changed */
        uint8_t need_full = first_frame || btn_changed
                         || (cursor_moved && mb != 0)
                         || wm_dirty || wm_dirty2 || clock_tick;

        if (need_full) {
            /* Full composite needed */

            /* Frame-timing oracle: stamp vsync at the start of frame
             * work so `wm_frame_stats_on_present()` below can measure
             * the full composite + swap budget. mono_ns() is the
             * highest-resolution monotonic clock the kernel has
             * (invariant TSC when present, HPET / LAPIC fallback),
             * matching what Win11 QueryPerformanceCounter exposes. */
            uint64_t vsync_ns = mono_ns();

            /* Skip terminal re-render during drag */
            if (!wm_is_dragging()) {
                terminal_render();
                gallery_render();
            }

            /* Drain any deferred window-close requests before painting
             * so Alt+F4 (keyboard IRQ posts; see wm_close_focused_window)
             * takes effect within one frame. Done under scheduler_disable
             * below to keep wm_destroy_window out of compositor-traversal
             * races. */
            scheduler_disable();
            wm_process_pending_closes();
            wm_mark_dirty_internal();

            /* Restore cursor, full composite, draw cursor, flip */
            cursor_restore();
            wm_composite();

            /* On the very first frame: unlock compositor so it can
             * flip the back-buffer to VRAM. Do NOT re-enable klog screen
             * output -- the compositor owns the framebuffer now. klog
             * messages go to serial only; only LOG_FATAL (panic) should
             * ever render over the desktop. The boot_splash_finish()
             * already set screen_min_level to LOG_FATAL. */
            if (first_frame) {
                fb_unlock_compositor();
            }

            /* Determine cursor shape from context */
            cursor_shape_t ctx = desktop_get_cursor_context(mx, my);
            if (ctx == CURSOR_ARROW)
                ctx = wm_get_cursor_context(mx, my);
            cursor_set_shape(ctx);

            cursor_draw(mx, my);

            /* During drag: partial swap of only the dirty region.
             * Otherwise: full screen swap. */
            {
                int32_t  drx, dry;
                uint32_t drw, drh;
                if (wm_get_drag_dirty_rect(&drx, &dry, &drw, &drh)) {
                    fb_swap_rect((uint32_t)drx, (uint32_t)dry,
                                 drw, drh);
                    /* Also swap cursor area */
                    {
                        int32_t  crx, cry;
                        uint32_t crw, crh;
                        if (cursor_get_rect(&crx, &cry, &crw, &crh))
                            fb_swap_rect((uint32_t)crx, (uint32_t)cry,
                                         crw, crh);
                    }
                } else {
                    fb_swap();
                }
            }

            /* Frame-timing oracle: take the present timestamp and
             * snapshot the counters BEFORE reopening preemption so
             * the WM_FRAME_PRESENTED payload cannot absorb
             * wm_mark_dirty() calls that arrive on another CPU for
             * the NEXT frame. Codex [M] review of §10 required this
             * ordering: scheduler_enable() used to run first,
             * leaving a window where other writers updated
             * s_frame_stats before the present snapshot copy. */
            wm_frame_stats_on_present(vsync_ns, mono_ns());

            scheduler_enable();

            prev_mx = mx;
            prev_my = my;
            prev_mb = mb;
            first_frame = 0;
        } else if (cursor_moved) {
            /* Cursor-only move (no buttons held) --
             * swap just the union of old + new cursor rects */
            scheduler_disable();

            int32_t  old_rx, old_ry;
            uint32_t old_rw, old_rh;
            int had_old = cursor_get_rect(&old_rx, &old_ry,
                                           &old_rw, &old_rh);

            cursor_restore();

            /* Update cursor shape even on cursor-only moves */
            cursor_shape_t ctx = desktop_get_cursor_context(mx, my);
            if (ctx == CURSOR_ARROW)
                ctx = wm_get_cursor_context(mx, my);
            cursor_set_shape(ctx);

            cursor_draw(mx, my);

            int32_t  new_rx, new_ry;
            uint32_t new_rw, new_rh;
            cursor_get_rect(&new_rx, &new_ry, &new_rw, &new_rh);

            /* Single swap of the union bounding box */
            if (had_old) {
                int32_t ux = (old_rx < new_rx) ? old_rx : new_rx;
                int32_t uy = (old_ry < new_ry) ? old_ry : new_ry;
                int32_t ur = old_rx + (int32_t)old_rw;
                int32_t nr = new_rx + (int32_t)new_rw;
                int32_t ub = old_ry + (int32_t)old_rh;
                int32_t nb = new_ry + (int32_t)new_rh;
                if (nr > ur) ur = nr;
                if (nb > ub) ub = nb;
                if (ux < 0) ux = 0;
                if (uy < 0) uy = 0;
                fb_swap_rect((uint32_t)ux, (uint32_t)uy,
                             (uint32_t)(ur - ux),
                             (uint32_t)(ub - uy));
            } else {
                uint32_t sx = (new_rx >= 0) ? (uint32_t)new_rx : 0;
                uint32_t sy = (new_ry >= 0) ? (uint32_t)new_ry : 0;
                fb_swap_rect(sx, sy, new_rw, new_rh);
            }

            scheduler_enable();

            prev_mx = mx;
            prev_my = my;
        }

        /* Periodically flush dirty registry hives to disk */
        registry_flush();

        /* Periodically write AHCI error counters to registry */
        ahci_flush_error_counters();

        /* Sleep until next IRQ. */
        __asm__ volatile ("sti; hlt");
    }
}
