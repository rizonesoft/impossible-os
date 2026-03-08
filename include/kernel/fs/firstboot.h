/* ============================================================================
 * firstboot.h — First-Boot Setup Module
 *
 * Isolated module that populates the system partition (C:\) on first boot.
 * Creates the default directory hierarchy and copies essential files from
 * the initrd (B:\) to the IXFS system drive.
 *
 * This module is designed to be easily removed once an installer exists.
 * Simply delete firstboot.c/firstboot.h and remove the call from main.c.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Run first-boot setup if C:\ is mounted and empty.
 * Creates default folder hierarchy and copies initrd files.
 * Safe to call on every boot — only acts if C:\ has no entries.
 * Returns 0 on success or if setup was skipped (not first boot). */
int firstboot_setup(void);

