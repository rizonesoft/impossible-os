/* ============================================================================
 * wx.h -- Kernel-image W^X (STRICT_KERNEL_RWX)
 *
 * Clears the WRITABLE bit on the static kernel `.text` and `.rodata` so a
 * kernel write primitive cannot patch executable code or constant data
 * (Linux STRICT_KERNEL_RWX / mark_rodata_ro, Win11 HVCI). `.text` stays
 * executable (NX clear); `.rodata` is already NX via vmm_apply_nx_policy.
 *
 * MUST run single-CPU, before APs launch: vmm_set_ro uses a local invlpg
 * with no TLB shootdown, so it is only safe while one CPU is running.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Clear WRITABLE on kernel `.text` (stays executable). Single-CPU only.
 * Returns 0 on success, -1 if the range could not be fully protected. */
int kernel_wx_protect(void);

/* Clear WRITABLE on kernel `.rodata` (already NX). Single-CPU only.
 * Returns 0 on success, -1 if the range could not be fully protected. */
int kernel_rodata_protect(void);
