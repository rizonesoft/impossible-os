/* ============================================================================
 * nt_lpc.h -- Legacy LPC port SSDT
 *
 * Reserves SSDT indices 0x0100-0x010E for NT 3.x-5.x LPC compatibility
 * syscalls. Each handler returns the deferred-status sentinel until the
 * LPC engine in 03-memory-concurrency/TODO-09-win32-ipc-extensions.md
 * and 02-kernel-core/TODO-24-alpc-message-ports.md ship. Registering
 * these slots ensures user-mode callers get a deterministic NTSTATUS
 * rather than a dispatch-miss path.
 *
 * SCOPE-GAP-ALLOWED: 15 stub handlers pending the legacy-IPC LPC engine.
 *                    Retrofit list is tracked on that roadmap under an
 *                    item ("Retrofit the 15 LPC SSDT stubs in
 *                    src/kernel/nt/nt_lpc.c ...") enumerated by SSDT
 *                    index with per-syscall semantics.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Returns 0 on success, or the number of handlers that failed to
 * register. Callers SHOULD treat non-zero as boot-fatal -- a partial
 * LPC surface with missing handlers is an ABI hazard. */
int nt_lpc_register_ssdt(void);
