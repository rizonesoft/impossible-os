/* ============================================================================
 * compositor.c -- Main desktop compositor event loop
 *
 * Never returns.  Runs as the idle/main kernel thread after all boot
 * phases complete.  Handles mouse input, composites windows, flips the
 * framebuffer, and manages cursor drawing.
 * ============================================================================ */

#include "kernel/mm/boot_stack.h"
#include "kernel/types.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/drivers/mouse.h"
#include "kernel/drivers/virtio_input.h"
#include "kernel/drivers/vbox_mouse.h"
#include "kernel/time/mono_clock.h"
#include "kernel/time/wall_clock.h"
#include "kernel/nt/filetime.h"
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
#include "kernel/boot_info.h"
#include "kernel/fs/partition.h"   /* ab_boot_mark_slot_successful (TODO-21 A/B mark-good) */
#include "kernel/boot_status.h"    /* boot_status_accept_advance (single bless authority) */
#include "kernel/panic.h"        /* panic_screen -- declared, not re-externed locally */

/* Headless state + test seed. All reads/writes are plain volatile
 * because tests + compositor thread never race on them today:
 * `compositor_set_headless` is called from boot_desktop.c BEFORE the
 * compositor loop starts, and `compositor_step_frames` is called
 * either from the test phase (pre-compositor) or from a thread while
 * the compositor is HLT-idling in headless mode. */
static volatile int      s_compositor_headless = 0;
#ifdef KERNEL_TESTS
/* Test-only seed slot: no production compositor feature consumes it (see the
 * accessor contract in main_internal.h). Guarded out of release images so the
 * storage + accessors carry no test surface (release test-surface exclusion). */
static volatile uint64_t s_compositor_test_seed = 0;
#endif

void compositor_set_headless(int headless)
{
    s_compositor_headless = headless ? 1 : 0;
}

int compositor_is_headless(void)
{
    return s_compositor_headless;
}

#ifdef KERNEL_TESTS
void compositor_set_test_seed(uint64_t seed)
{
    s_compositor_test_seed = seed;
}

uint64_t compositor_get_test_seed(void)
{
    return s_compositor_test_seed;
}
#endif

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
     * review of required this refusal. */
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

        /* Advance the frame-timing oracle BEFORE scheduler_enable
         * so the present snapshot cannot absorb mark_dirty bumps from
         * the next frame arriving on another CPU. Matches the
         * compositor_run ordering (see quality review that moved
         * on_present inside the scheduler_disable window). Codex [H]
         * quality review required this alignment. */
        wm_frame_stats_on_present(vsync_ns, mono_ns());
        scheduler_enable();
    }
    return i;
}

/* Steady-state stack-sample probe state (TODO-10 sec32). File scope on
 * purpose: as locals these enlarged compositor_run's stack frame, which is
 * part of the very watermark the sample reports. Single-writer -- only PID 0
 * runs this loop -- so no synchronization is needed or implied.
 *
 * Sample on the FIRST presentation, then re-check every
 * COMPOSITOR_STACK_SAMPLE_PERIOD presentations and log only when the peak has
 * GROWN. Both halves are there for a measured reason:
 *
 *   - First presentation, because depth here is set by the deepest per-frame
 *     call chain (input merge, damage accumulation, blit, present) and any
 *     full composite exercises it. Measured: the reading after ONE
 *     presentation is 38,368 bytes, byte-identical to the reading taken after
 *     120. Extra frames add repetition, not depth.
 *   - A fixed larger count does not work at all. Presentations happen on
 *     damage, so an idle desktop can sit at a handful indefinitely; 120 and
 *     then 16 were both measured NEVER to be reached in a full boot-to-shell
 *     run, so the sample never fired. A missing number reads as no problem,
 *     which is worse than a modest one.
 *   - Periodic re-checks, because one composite is not proof that nothing
 *     deeper ever runs. This is PID 0's permanent stack, so the honest
 *     instrument keeps watching rather than declaring a final answer at
 *     second three.
 *
 * Cost is one poison scan per period, and a log line only on growth. */
#define COMPOSITOR_STACK_SAMPLE_PERIOD 512u
static uint64_t s_stack_sample_presents;
static uint64_t s_stack_peak_reported;

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
        /* Anti-rollback policy in headless mode: there is no
         * user-visible steady state to gate on, so the raise is
         * intentionally SKIPPED. Loudly log the bypass so a
         * shipped boot.conf that enables anti_rollback_raise=1
         * under compositor=headless surfaces the mismatch
         * instead of silently leaving the counter stuck. */
        uint32_t shipped  = g_boot_info.os_loader_security_version;
        uint32_t required = g_boot_info.required_security_version;
        if (g_boot_info.config.anti_rollback_raise &&
            shipped > required) {
            klog(LOG_WARN, "boot",
                 "anti-rollback: raise skipped in headless mode "
                 "(shipped=%u required=%u); opt-in has no effect",
                 (uint64_t)shipped, (uint64_t)required);
        }
        /* Headless has no first composited frame, but reaching the idle loop
         * after the Phase-3 health gate IS the boot-acceptance signal for this
         * mode. Drive the ledger to UI_READY so the accepted transition still
         * blesses the boot (A/B mark-good + per-entry MarkGood + durable record);
         * otherwise a healthy headless boot would leave rollback state pending. */
        (void)boot_status_accept_advance(BOOT_ACCEPT_UI_READY);
        /* Steady state for headless: PID 0 does no further work after this, so
         * this IS the terminal peak for this mode (TODO-10 sec32). */
        (void)boot_stack_measure("steady state (headless)");
        for (;;)
            __asm__ volatile ("sti; hlt");
    }

    fb_fill_rect(0, 0, fb_get_width(), fb_get_height(), 0x00000000);

    int32_t prev_mx = -1, prev_my = -1;
    uint8_t prev_mb = 0;
    uint8_t first_frame = 1;
    uint64_t last_clock_sec = 0;
    uint64_t last_clock_min = (uint64_t)-1;  /* force first draw */

    for (;;) {
        int32_t mx, my;
        uint8_t mb;

        /* All pointing sources merge into ONE cursor (the shared mouse state)
         * for input source coexistence:
         *   - VirtIO tablet (QEMU) / VBox VMMDev -- absolute, edge-merged so a
         *     simultaneous relative USB/PS2 mouse is not shadowed.
         *   - USB HID + PS/2 mice -- relative deltas already in the shared state.
         * Feed any active absolute source, then read the merged cursor once. */
        if (virtio_input_available()) {
            struct virtio_input_state vis = virtio_input_get_state();
            mouse_merge_absolute(vis.x, vis.y, vis.buttons);
        } else if (vbox_mouse_available()) {
            struct mouse_state vb = vbox_mouse_get_state();
            mouse_merge_absolute_position(vb.x, vb.y);
        }
        {
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
                /* Re-read the merged cursor (same source-merge as above) */
                if (virtio_input_available()) {
                    struct virtio_input_state vis =
                        virtio_input_get_state();
                    mouse_merge_absolute(vis.x, vis.y, vis.buttons);
                } else if (vbox_mouse_available()) {
                    struct mouse_state vb = vbox_mouse_get_state();
                    mouse_merge_absolute_position(vb.x, vb.y);
                }
                {
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
            /* Detect a wall-clock minute change. RTC is a boot-time seed only,
             * not a runtime clock (and returns a frozen year-0 sentinel on
             * no-CMOS hardware); the wall clock is the runtime source and is
             * driven by UEFI GetTime when there is no RTC. */
            uint64_t cur_min = KeQuerySystemTime() /
                               (FILETIME_TICKS_PER_SECOND * 60);
            if (cur_min != last_clock_min) {
                last_clock_min = cur_min;
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

            /* Late crash test -- crash_test=2 dies HERE, inside the
             * compositor loop, after everything Phase 3 does AND after
             * entering compositor_run(), but before the first composite
             * and flip. This is the deepest pre-presentation point in
             * the boot, and it is deliberately inside the loop rather
             * than ahead of the call: a raise placed at the top of
             * compositor_run() would execute on a boot stopped earlier,
             * and the anti-rollback fixture harness would then certify a
             * floor that advanced before anything was ever shown. Fires
             * only on the first iteration -- the boot cannot reach a
             * second one. */
            if (first_frame && g_boot_info.config.crash_test == 2) {
                /* Unrated: the fixture harness greps this exact line as
                 * proof the boot really died here, and the shared
                 * per-subsystem rate cap would otherwise let an unrelated
                 * chatty "boot" caller drop it -- which reads downstream
                 * as a rollback-policy regression rather than a lost log.
                 * No fb_unlock_compositor() call here: panic_screen does
                 * that itself before its first draw, so the BSOD reaches
                 * VRAM regardless of the lock state at entry, and
                 * unlocking early would only widen the window before its
                 * interrupt mask. */
                klog_unrated(LOG_WARN, "boot",
                             "crash_test=2 -- triggering deliberate BSOD before first composite");
                panic_screen((struct interrupt_frame *)0, 0xDEAD,
                             "CRASH_TEST: Deliberate pre-presentation panic",
                             __FILE__, __LINE__);
            }

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
             * the NEXT frame. Codex [M] review of required this
             * ordering: scheduler_enable() used to run first,
             * leaving a window where other writers updated
             * s_frame_stats before the present snapshot copy. */
            wm_frame_stats_on_present(vsync_ns, mono_ns());

            scheduler_enable();

            /* One-shot steady-state stack sample (TODO-10 sec32). PID 0 never
             * leaves this loop and, per task.c:596, keeps the loader-owned run
             * as its PERMANENT kernel stack -- so a peak taken at the end of
             * Phase 1, or even after boot_tests_run(), describes a prefix of
             * this stack's life rather than its workload.
             *
             * Counted in PRESENTATIONS, not loop iterations. An idle desktop
             * spins this loop without compositing, so an iteration counter
             * would fire after one real frame plus N no-ops and prove nothing
             * about the deep per-frame paths (input merge, damage, blit) the
             * number is supposed to cover. This site is past the present, so
             * a sample always follows a completed frame.
             *
             * State is file-scope, not a local, and the call sits AFTER
             * scheduler_enable() rather than inside the timed region: a probe
             * that enlarges compositor_run's own frame moves the watermark it
             * is measuring. Measured: as two locals at the loop head it grew
             * that frame from 88 to 104 bytes. */
            s_stack_sample_presents++;
            if (s_stack_sample_presents == 1u ||
                (s_stack_sample_presents % COMPOSITOR_STACK_SAMPLE_PERIOD) == 0u) {
                uint64_t peak = boot_stack_peak();
                if (peak > s_stack_peak_reported) {
                    s_stack_peak_reported = peak;
                    (void)boot_stack_measure("steady state (compositor)");
                }
            }

            prev_mx = mx;
            prev_my = my;
            prev_mb = mb;
            if (first_frame) {
                /* Compositor-steady signal: the first full-desktop
                 * composite + fb_swap has completed. Latch the signal
                 * and evaluate the anti-rollback raise. If the opt-in
                 * policy is clear or shipped <= required the raise is
                 * a no-op; the point of this gate is to withhold the
                 * raise when the boot dies before reaching the first
                 * frame, not to force it on every boot. */
                boot_rollback_mark_steady();
                /* Defer the actual NVRAM write to sys_wq so the
                 * first-frame presentation thread doesn t block on
                 * the UEFI Runtime Services SetVariable call (10-100
                 * ms on real firmware). request_raise falls back to
                 * synchronous if sys_wq is unavailable so the raise
                 * still happens on every steady boot. */
                (void)boot_rollback_request_raise();
                /* The desktop reaching its first composited frame IS the
                 * boot-acceptance signal (UI_READY). Drive the boot-status
                 * ledger: when this reaches the configured accept stage, the
                 * single accepted transition performs the A/B mark-good (tries
                 * reset + successful), the per-entry MarkGood, and the durable
                 * record -- exactly once, instead of marking good here directly
                 * (which let the compositor and the health gate bless from two
                 * independent authorities). */
                (void)boot_status_accept_advance(BOOT_ACCEPT_UI_READY);
            }
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
