/* ============================================================================
 * boot_recovery.h -- Degraded-boot recovery screen
 *
 * In-kernel graphical recovery UI shown when a Phase 2/3 subsystem fails.
 * Renders directly to the GOP framebuffer -- no compositor, no WM required.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_init.h"

/* Recovery info passed to the recovery screen */
typedef struct {
    kernel_subsys_t subsystem;   /* which subsystem failed */
    uint16_t        postcode;    /* POST code at failure */
    boot_result_t   result;      /* BOOT_FATAL or BOOT_DEGRADED */
    uint8_t         phase;       /* phase number (2 or 3) */
} boot_recovery_info_t;

/* Recovery action returned by the screen */
typedef enum {
    RECOVERY_RETRY    = 0,  /* user pressed [R] -- retry boot */
    RECOVERY_CONSOLE  = 1,  /* user pressed [C] -- drop to serial console */
    RECOVERY_POWEROFF = 2,  /* user pressed [P] -- power off */
} boot_recovery_action_t;

/* Show the recovery screen and wait for user input.
 * Returns the chosen action. Falls through to boot_halt() if FB is not ready.
 * Safe to call from Phase 2 or Phase 3 -- no heap allocation. */
boot_recovery_action_t boot_recovery_show(const boot_recovery_info_t *info);
