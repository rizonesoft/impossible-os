/* ============================================================================
 * boot_splash.h -- Persistent boot splash screen (Windows 11-style)
 *
 * Shows centered icon + progressive arc spinner + status text
 * on a black background. Persists from fb_init() until desktop_init().
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Initialize and draw the boot splash screen.
 * Call immediately after fb_init(). */
void boot_splash_init(void);

/* Start the timer-driven spinner animation.
 * Call after pit_init() + sti (needs PIT interrupts running). */
void boot_splash_start_animation(void);

/* Update the status message below the spinner.
 * The previous message is cleared and the new one is drawn. */
void boot_splash_status(const char *msg);

/* Update the diagnostic line below the status text (debug=1 only).
 * Persists until explicitly cleared -- not overwritten by boot_splash_status(). */
void boot_splash_diag(const char *msg);

/* Pause for N seconds while keeping the spinner animated. */
void boot_splash_delay(uint32_t seconds);

/* Advance the animation by one frame and redraw.
 * Call periodically during kernel init (e.g., every subsystem init). */
void boot_splash_tick(void);

/* Finish the splash: clear screen and hand off to the desktop.
 * After this call, the framebuffer is fully available for wm/desktop. */
void boot_splash_finish(void);

/* Returns 1 if the boot splash is currently active (screen owned by splash). */
int boot_splash_active(void);

/* 1 only when the splash is on AND a usable text renderer exists, so a
 * caller can tell "I called status()" apart from "the operator saw it".
 * splash_on is set before the font is initialized, so the two differ on a
 * font/PMM failure -- use THIS one whenever the message being seen is the
 * point (a confirmation prompt), not merely nice to have. */
int boot_splash_text_ready(void);

/* Abort the splash immediately (no fade) and unlock framebuffer for printk.
 * Used by debug boot mode to show text output on screen. */
void boot_splash_abort(void);
