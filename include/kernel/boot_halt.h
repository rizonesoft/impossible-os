/* ============================================================================
 * boot_halt.h -- Pre-framebuffer fatal halt with diagnostic error screen
 * ============================================================================ */

#pragma once

/* Halt the system with a diagnostic error screen.
 *
 * - Writes reason to serial unconditionally.
 * - If the framebuffer is available (g_boot_info.fb_available), draws:
 *     · Solid red rectangle across the top 40 px of the screen
 *     · White error text in 8×8 pixels below the red banner
 * - Does not return.
 *
 * Safe to call from any kernel phase, including before fb_init().
 * XREF: 02-kernel-core/TODO-01-kernel-init-sequencing.md */
void boot_halt(const char *reason);
