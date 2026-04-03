/* ============================================================================
 * system_state.h -- Global kernel system state flags
 *
 * Lightweight flags set by subsystems at init time and read by the desktop
 * shell, tray renderer, and diagnostic tools.  Each field is set once during
 * boot by the owning subsystem; no synchronization needed after that.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

struct system_state {
    uint8_t secure_boot;    /* 1 = Secure Boot active (set by uefi_secureboot_init) */
    uint8_t pad[7];
};

extern struct system_state g_system_state;
