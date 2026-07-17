/* ============================================================================
 * shell_loader.h -- cmd.exe task-entry loader
 *
 * PRODUCTION code: the boot->desktop handoff (boot_desktop.c) and the
 * desktop Start Menu Terminal launcher (desktop.c) both task_create() this
 * as the entry point that loads and execs cmd.exe from C:\. It is NOT
 * test-only despite formerly living in test_threads.c (moved 2026-07-17,
 * TODO-10 kernel-security-hardening.md section 28) -- that file is pruned
 * entirely under KERNEL_TESTS=off, which would have deleted the release
 * flavor's only path to a shell.
 * ============================================================================ */

#pragma once

/* Task entry point: loads C:\cmd.exe via vfs_open/vfs_read, execs it via
 * task_exec(), then parks the loader thread in a halt loop. Logs and
 * returns (no-op) if C:\ is not mounted, cmd.exe is missing, allocation
 * fails, or exec fails -- never called directly, only via task_create(). */
void shell_loader_func(void);
