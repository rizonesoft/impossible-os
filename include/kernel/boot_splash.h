/* ============================================================================
 * boot_splash.h — Persistent boot splash screen (Windows 11-style)
 *
 * Shows centered icon + animated horizontal dots + status text
 * on a black background. Persists from fb_init() until desktop_init().
 * ============================================================================ */

#pragma once

/* Initialize and draw the boot splash screen.
 * Call immediately after fb_init(). */
void boot_splash_init(void);

/* Start the timer-driven dot animation.
 * Call after pit_init() + sti (needs PIT interrupts running). */
void boot_splash_start_animation(void);

/* Update the status message below the dots.
 * The previous message is cleared and the new one is drawn. */
void boot_splash_status(const char *msg);

/* Advance the dot animation by one frame and redraw.
 * Call periodically during kernel init (e.g., every subsystem init). */
void boot_splash_tick(void);

/* Finish the splash: clear screen and hand off to the desktop.
 * After this call, the framebuffer is fully available for wm/desktop. */
void boot_splash_finish(void);

/* Returns 1 if the boot splash is currently active (screen owned by splash). */
int boot_splash_active(void);
