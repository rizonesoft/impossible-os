/* ============================================================================
 * nt_alpc.h -- Modern ALPC port SSDT (TODO-05 §31)
 *
 * Reserves SSDT indices 0x010F-0x011E for Vista+ ALPC syscalls. Each
 * handler returns the deferred-status sentinel until the ALPC engine
 * in 02-kernel-core/TODO-12 §8-§9 is implemented. Registering these slots
 * ensures user-mode callers get a deterministic NTSTATUS rather than
 * a dispatch-miss path, and makes ownership visible in /audit-ssdt.
 *
 * SCOPE-GAP-ALLOWED: 16 stub handlers pending TODO-12 §8 SSDT retrofit
 *                    and §9 NtAlpc Query/Set/Cancel. Retrofit list is in
 *                    TODO-12 §8 (item:
 *                    "Retrofit the 16 ALPC SSDT stubs in
 *                    src/kernel/nt/nt_alpc.c ..." -- enumerated by
 *                    SSDT index with per-syscall semantics).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Returns 0 on success, or the number of handlers that failed to
 * register. Callers SHOULD treat non-zero as boot-fatal -- a partial
 * ALPC surface with missing handlers is an ABI hazard. */
int nt_alpc_register_ssdt(void);
