/* ============================================================================
 * env_apppaths.h -- App Paths registry-based executable lookup (TODO-22 s17)
 *
 * The Windows App Paths convention: an executable can be resolved by bare name
 * without a %PATH% entry via
 *   HKLM|HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\{name}
 *     default (unnamed) value -> the full executable path
 *     "Path" value (optional) -> a directory prepended to the child's PATH
 * ShellExecute consults it as a FALLBACK after the normal PATH search fails.
 *
 * These are plain kernel-C helpers over the registry API (registry.h); they are
 * NOT SSDT syscalls and NOT pe.c export entries. Post-Phase-3 code (no POST16).
 * The shell-side integration (s7 shell_find_command consulting App Paths after
 * PATH) is DEFERRED with s7 -- no kernel shell exists yet -- so these ship as the
 * standalone, unit-testable lookup/register primitives a future resolver calls.
 *
 * Security posture (s16 elevation gate):
 *  - Lookup root precedence is bound to caller elevation, never applied blindly:
 *    a user-writable HKCU App Paths entry must never redirect a privileged
 *    resolution. Elevated/System (and identity-less NULL) callers see HKLM ONLY;
 *    normal callers get the Windows HKCU-first, HKLM-fallback order.
 *  - Registration into HKLM (machine-wide) requires a proven elevated caller --
 *    an API-level authorization boundary standing in for registry DACLs until
 *    TODO-15 s5 SeAccessCheck lands (today reg_check_access is always-grant).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "registry.h"          /* HKEY, ERROR_* / KEY_* codes */

struct task;

/* The App Paths container key, relative to the HKLM/HKCU root sentinel. Exposed
 * so consumers and tests reference the one canonical literal (no hand-copies). */
#define APP_PATHS_BASE  "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths"

/* Longest executable subkey name (a single filename component) accepted; matches
 * the Windows / REG_MAX_KEY_NAME 255-char registry key-name limit. A longer or
 * empty name, or one containing a path separator, is ERROR_INVALID_PARAMETER.
 * Note the EFFECTIVE limit for a name WITHOUT an extension is 4 shorter (251):
 * ".exe" is appended and the resulting component must still fit REG_MAX_KEY_NAME,
 * so a 252-255-char dotless name is rejected (would build a 256-259-char key). */
#define APP_PATHS_NAME_MAX  255u

/* Resolve an executable's registered full path via App Paths.
 *
 * `name` is a single filename component; when it has no '.' the lookup appends
 * ".exe" (matching ShellExecute). A `name` containing '\' or '/' is rejected --
 * it would traverse out of the App Paths container.
 *
 * Root precedence is caller-elevation-bound:
 *   - caller is NULL, or env_is_secure_context(caller) (elevated/System, or a
 *     task whose token is absent/malformed -- s16 fail-closed): HKLM ONLY.
 *   - otherwise (a normal <= Medium caller): HKCU first, then HKLM.
 *
 * Return value reflects the DEFAULT (executable) value only:
 *   ERROR_SUCCESS           found + copied, NUL-terminated; *out_path_needed set
 *                           to bytes-incl-NUL written.
 *   ERROR_MORE_DATA         entry exists but out_path too small; *out_path_needed
 *                           = required bytes incl NUL; out_path holds no valid
 *                           result (untouched, except a malformed non-terminated
 *                           exact-fit value may leave partial bytes). Never
 *                           silently falls through to a lower-precedence root or
 *                           reports not-found -- a too-small buffer must not
 *                           masquerade as absence and resolve a different binary.
 *   ERROR_FILE_NOT_FOUND    no App Paths entry for name in the consulted root(s).
 *   ERROR_INVALID_PARAMETER bad argument (NULL name, separator, over-long).
 *   (other Win32)           underlying registry error (e.g. ERROR_ACCESS_DENIED),
 *                           returned without downgrading to a lower-precedence
 *                           root.
 *
 * The optional "Path" value is INDEPENDENT of the executable result and never
 * changes the return code: when out_additional is non-NULL it receives the Path
 * value if present and it fits (empty string if absent or it does not fit).
 * *out_add_needed (if non-NULL) is set to the required size incl NUL (0 when no
 * Path value), so a too-small out_additional is discoverable without erasing an
 * otherwise valid executable result. out_path/out_additional and their size /
 * needed pointers may be NULL / 0 independently. */
long app_paths_lookup(struct task *caller, const char *name,
                      char *out_path, uint32_t out_path_size,
                      uint32_t *out_path_needed,
                      char *out_additional, uint32_t out_add_size,
                      uint32_t *out_add_needed);

/* Register an executable under App Paths so it resolves without a PATH change
 * (installer API; -> XREF 10-platform-services/TODO-03-updates-packages.md).
 *
 * `root` MUST be HKEY_LOCAL_MACHINE (machine-wide) or HKEY_CURRENT_USER
 * (per-user); anything else is ERROR_INVALID_PARAMETER. Writing HKLM requires a
 * proven elevated caller (env_is_proven_elevated(caller) -- the authorization
 * predicate requiring BOTH the token's IsElevated flag AND a valid integrity
 * level at/above High, NOT the fail-closed-to-secure env_is_secure_context used
 * for lookup precedence); a NULL, identity-less, malformed-token, IsElevated-clear,
 * or below-High caller gets ERROR_ACCESS_DENIED. HKCU (per-user) is unrestricted.
 *
 * Replacement is deterministic: the default value is set to full_path; when
 * additional_path is non-NULL the "Path" value is set to it, and when
 * additional_path is NULL any existing "Path" value is DELETED -- a repair
 * re-registration cannot leave a stale, possibly attacker-controlled search
 * directory behind. The first underlying registry error is propagated. The
 * default and "Path" writes are two SEPARATE registry operations, NOT an atomic
 * pair: on the lock-free registry a concurrent registration can interleave, and a
 * failure after the default write leaves the new executable with the prior Path.
 * Batched atomic multi-value writes are owned by TODO-14 s14. `name` follows the
 * same .exe-append and no-separator rules as app_paths_lookup; full_path must be
 * non-NULL. */
long app_paths_register(struct task *caller, HKEY root, const char *name,
                        const char *full_path, const char *additional_path);
