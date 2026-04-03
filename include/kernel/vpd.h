/* ============================================================================
 * vpd.h -- Visual POST Display (VPD)
 *
 * Two-tier boot progress visualization:
 *   Tier 1 (pre-splash): direct VRAM writes using vpd_font.h micro-font
 *   Tier 2 (post-splash): integrates with splash via fb driver (TODO §7-§8)
 *
 * Tier 1 is active from g_boot_info parse until boot_splash_init() takes over.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Initialize VPD Tier 1 -- call right after g_boot_info is populated.
 * Stores framebuffer pointer and clears the VPD display area. */
void vpd_init(void);

/* Begin a new boot stage -- marks previous stage as done (green),
 * draws new stage row as in-progress (yellow). */
void vpd_stage_begin(uint8_t phase, const char *name, uint16_t postcode);

/* Mark the current stage as done (green) without starting a new one.
 * Called at the end of boot when compositor takes over. */
void vpd_stage_done(void);

/* Mark the current stage as failed (red).
 * Called from kernel_panic() or boot_halt(). */
void vpd_stage_fail(void);

/* Returns 1 if VPD Tier 1 is active and rendering. */
int vpd_is_active(void);

/* Update progress bar (0-100%). Called automatically by vpd_stage_begin. */
void vpd_update_progress(uint8_t percent);

/* Render "Last boot failed at: NAME (0xNNNN)" crash banner at top of screen.
 * Always renders regardless of postbars setting -- safety feature.
 * Call before vpd_init() if needed (uses g_boot_info.fb directly). */
void vpd_crash_banner(uint16_t last_postcode);

/* Look up a POST16 code and return a human-readable stage name.
 * Returns "UNKNOWN" if code is not in the lookup table. */
const char *vpd_post16_name(uint16_t code);

/* Signal that splash is taking over -- stop Tier 1 rendering. */
void vpd_stop_tier1(void);
