/* ============================================================================
 * env_searchpath.h -- SearchPathW / SearchPathA kernel32 API (TODO-22 s14)
 *
 * Win32 executable/DLL path search. These are plain kernel C functions taking an
 * explicit `struct task *caller` (matching ExpandEnvironmentStringsForUser in
 * nt_rtlenv.c) so they are unit-testable against a fixture task -- they are NOT
 * SSDT syscalls and NOT entries in pe.c's export tables. The shared UTF-8 core
 * env_search_path() holds all search logic; SearchPathW/SearchPathA are thin
 * encoding wrappers.
 *
 * Security posture:
 *  - Trusted search legs (system dir, Windows dir) derive from the IMMUTABLE
 *    ENV_SYSTEMROOT_DIR / ENV_SYSTEM32_DIR compile-time constants, never the
 *    caller-mutable %SYSTEMROOT% env var (a DLL-hijack vector).
 *  - Every candidate probe is caller-aware: it resolves through
 *    task_resolve_path_for() and is gated by unveil_check(UNVEIL_R) under
 *    pledge_user_mode(), so SearchPath cannot become a sandbox-bypass oracle.
 *  - The application-load directory (Win32 search leg 1) is deliberately NOT
 *    implemented: it would require trusting the user-writable PEB ImagePathName /
 *    task->name. Deferred to a kernel-owned canonical image path
 *    (TODO-21 s2 -> TODO-22 s14).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

struct task;

/* SetSearchPathMode() BaseSearchPathMode bits (Win32 winbase.h values). Safe
 * search mode moves the current directory to the END of the SearchPathW order
 * (after PATH); disabling it puts the current directory early (before the system
 * directories) -- the classic DLL-hijack-prone ordering. PERMANENT locks the
 * choice for the process lifetime and is valid only combined with ENABLE. */
#define BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE   0x00000001u
#define BASE_SEARCH_PATH_DISABLE_SAFE_SEARCHMODE  0x00010000u
#define BASE_SEARCH_PATH_PERMANENT                0x00008000u

/* All bits SetSearchPathMode accepts; anything outside is ERROR_INVALID_PARAMETER. */
#define BASE_SEARCH_PATH_VALID_MASK \
    (BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | \
     BASE_SEARCH_PATH_DISABLE_SAFE_SEARCHMODE | \
     BASE_SEARCH_PATH_PERMANENT)

/* Longest single path component / candidate SearchPath assembles internally. */
#define SP_PATH_MAX  1024u

/* ---- Shared UTF-8 core ----------------------------------------------------
 * Search order when lpPath == NULL: current directory, system dir
 * (ENV_SYSTEM32_DIR), Windows dir (ENV_SYSTEMROOT_DIR), then %PATH% entries --
 * with the current directory REORDERED (not removed) per the caller's
 * search_path_mode: unsafe = current directory first; safe/unset = after PATH.
 * When lpPath != NULL only its ';'-delimited directories are searched. A
 * qualified lpFileName (containing a separator or "X:" drive spec) bypasses
 * directory iteration and is probed directly.
 *
 * lpExtension (first char must be '.') is appended only when lpFileName's final
 * component has no extension. Every probe is resolved via task_resolve_path_for()
 * and gated by unveil_check(UNVEIL_R)/pledge_user_mode(); only a regular file
 * matches.
 *
 * Returns, on the found UTF-8 absolute path:
 *   - length in bytes excluding NUL when it fits (out filled, NUL-terminated,
 *     *out_filepart_off = byte offset of the file-name component, 0 if none);
 *   - the required size INCLUDING NUL when it will not fit (out left UNTOUCHED);
 *   - 0 when not found (*out_err = ERROR_FILE_NOT_FOUND) or on a bad argument
 *     (*out_err = ERROR_INVALID_PARAMETER) / allocation failure
 *     (*out_err = ERROR_OUTOFMEMORY). *out_err is 0 on success.
 * out_filepart_off and out_err may be NULL. */
uint32_t env_search_path(struct task *caller, const char *lpPath,
                         const char *lpFileName, const char *lpExtension,
                         char *out, uint32_t out_size,
                         uint32_t *out_filepart_off, uint32_t *out_err);

/* TRUE (1) if the current directory should participate in an executable search
 * for exe_name: TRUE when exe_name contains a backslash; otherwise FALSE when the
 * NoDefaultCurrentDirectoryInExePath env var is present (even empty), else TRUE.
 * UTF-8 core behind NeedCurrentDirectoryForExePathW. */
int env_need_current_dir_for_exe(struct task *caller, const char *exe_name);

/* ---- Win32 ABI wrappers ---------------------------------------------------
 * Lengths and lpFilePart are reported in the target ABI's units: UTF-16 code
 * units for the W forms, ACP (NLS_CP_ACP) bytes for the A forms. On not-found /
 * error the caller thread's TEB LastErrorValue is set (when a live TEB exists). */
uint32_t SearchPathW(struct task *caller, const uint16_t *lpPath,
                     const uint16_t *lpFileName, const uint16_t *lpExtension,
                     uint32_t nBufferLength, uint16_t *lpBuffer,
                     uint16_t **lpFilePart);
uint32_t SearchPathA(struct task *caller, const char *lpPath,
                     const char *lpFileName, const char *lpExtension,
                     uint32_t nBufferLength, char *lpBuffer, char **lpFilePart);

int NeedCurrentDirectoryForExePathW(struct task *caller, const uint16_t *ExeName);

/* Set the per-task SearchPathW current-directory ordering. Returns TRUE (1) on
 * success, FALSE (0) on an invalid flag combination (ERROR_INVALID_PARAMETER) or
 * when a prior PERMANENT setting locks further change (ERROR_ACCESS_DENIED). The
 * store is an __atomic CAS so a concurrent set cannot downgrade a PERMANENT
 * enable on SMP. */
int SetSearchPathMode(struct task *caller, uint32_t BaseSearchPathMode);
