/* ============================================================================
 * nt_lpc.h -- Legacy LPC port SSDT (TODO-05 §20)
 *
 * Reserves SSDT indices 0x0100-0x010E for NT 3.x-5.x LPC compatibility
 * syscalls. Each handler returns the deferred-status sentinel until the
 * LPC engine in 03-memory-concurrency/TODO-08 §7 + 02-kernel-core/
 * TODO-12 §8 is implemented. Registering these slots ensures user-mode
 * callers get a deterministic NTSTATUS rather than a dispatch-miss path.
 *
 * SCOPE-GAP-ALLOWED: 15 stub handlers pending TODO-08 §7 ALPC engine.
 *                    Retrofit list is tracked in TODO-08 §7 (item:
 *                    "Retrofit the 15 LPC SSDT stubs in
 *                    src/kernel/nt/nt_lpc.c ..." -- enumerated by SSDT
 *                    index with per-syscall semantics).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

void nt_lpc_register_ssdt(void);
