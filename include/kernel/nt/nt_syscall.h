/* ============================================================================
 * nt_syscall.h -- NT native syscall wrappers (SSDT migration layer)
 *
 * NtXxx functions that wrap existing SYS_* implementations with proper
 * NTSTATUS returns and SSDT-compatible signatures. These are the user-mode
 * callable entry points via INT 0x2E / SYSCALL; the old INT 0x80 path
 * continues to work for backward compatibility.
 *
 * Each NtXxx function is registered in the SSDT at the index defined in
 * service_numbers.h. Call nt_syscall_register_ssdt() after ssdt_init().
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/ssdt.h"

/* Register all NtXxx syscall wrappers with the SSDT.
 * Call once during Phase 3, after ssdt_init(). */
void nt_syscall_register_ssdt(void);
