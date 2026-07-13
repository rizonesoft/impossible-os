/* ============================================================================
 * nt_env.h -- NT per-process environment-variable syscalls (TODO-22 s5)
 *
 * NtQueryEnvironmentVariable / NtSetEnvironmentVariable are the NT-boundary
 * syscalls (SSDT 0x03DD / 0x03DE) over the calling process's kernel-resident
 * UTF-8 environment (kernel/env.h -- the AUTHORITATIVE store). They are the
 * counterparts of the firmware env syscalls 0x00D2-0x00D6 (which read UEFI/
 * SMBIOS variables), not a replacement for them.
 *
 * There is intentionally NO separate Zw* alias symbol: this kernel resolves
 * UserMode vs KernelMode at dispatch time via ssdt_previous_mode() (see zw.h),
 * so a single SSDT number serves both the Nt (probed) and Zw (kernel-trusted)
 * roles -- the whole tree carries zero `Zw*` alias defines by design.
 *
 * The PEB block PEB->ProcessParameters->Environment is a WRITE-ONCE process-
 * startup snapshot; every documented reader (GetEnvironmentVariableW,
 * GetEnvironmentStringsW, RtlExpandEnvironmentStrings_U) resolves against
 * task->environ, not that block, so NtSetEnvironmentVariable updates only the
 * kernel store. Optional post-startup PEB-block re-sync is a compatibility
 * nicety tracked in TODO-22 s6 (see the section notes).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Register the two environment syscalls in the main SSDT. Returns the number
 * of ssdt_register() failures (0 on success) so the boot-path registration
 * gate (boot_desktop.c) can fold it into its boot-halt tally, matching the
 * int-returning csprng/nt_lpc/nt_alpc/nt_audit registration convention. */
int nt_env_register_ssdt(void);
