/* ============================================================================
 * env.h -- Per-process environment variable storage & kernel API (TODO-22)
 *
 * Every task carries a NULL-terminated array of "KEY=VALUE" UTF-8 strings
 * (task->environ) guarded by a per-task sleeping lock (task->environ_lock).
 * The lock is a mutex, NOT a spinlock: env_set/env_unset/env_copy allocate and
 * free heap/PMM memory and env_get_copy may copy a value up to ENV_VALUE_MAX
 * bytes -- all forbidden under a spinlock (see include/kernel/sched/spinlock.h:
 * "NEVER call kmalloc() while holding a spinlock"). env is only ever touched
 * from thread context (syscall + exec paths), never from an ISR, so a mutex is
 * the correct primitive.
 *
 * Reader lifetime contract
 * ------------------------
 * There is deliberately NO public "const char *env_get()" that returns a raw
 * borrowed pointer after dropping the lock -- a sibling thread's env_set/
 * env_unset frees that entry, so the borrow is a use-after-free the instant the
 * lock is released. Two safe read shapes instead:
 *   - env_get_copy(): snapshots the value into a caller buffer UNDER the lock.
 *     Use this everywhere a value crosses to user space or is retained.
 *   - env_lock()/env_peek_locked()/env_unlock(): a batch fast path for callers
 *     doing many lookups (e.g. the %VAR% expansion pass). The pointer from
 *     env_peek_locked() is valid ONLY between env_lock() and env_unlock();
 *     copy out before unlocking.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

struct task;

/* Size limits (the env-syscall, block-builder, and sanitization layers reuse
 * these). Values are in bytes of UTF-8, excluding the NUL terminator. */
#define ENV_NAME_MAX        256u   /* max variable name length (Windows practical limit) */
#define ENV_VALUE_MAX       32767u /* max variable value length (Windows 32K-1 limit) */
#define ENV_MAX_ENTRIES     511u   /* (n+1)*sizeof(char*) stays <= 4 KiB kmalloc array */
#define ENV_STR_KMALLOC_MAX 4096u  /* "KEY=VALUE" strings up to this via kmalloc; larger via PMM */

/* Return codes: 0 on success, negative on error. */
#define ENV_OK             0
#define ENV_ERR_INVAL     (-1)     /* bad args / invalid name */
#define ENV_ERR_NOMEM     (-2)     /* allocation failed */
#define ENV_ERR_TOOLONG   (-3)     /* name or value exceeds a limit */
#define ENV_ERR_NOSPACE   (-4)     /* environ array is full (ENV_MAX_ENTRIES) */
#define ENV_ERR_NOTFOUND  (-5)     /* variable does not exist (env_unset) */

/* Copy the value of `name` into `out` (NUL-terminated) under the env lock.
 * Case-insensitive name match (Windows semantics). Returns the value length in
 * bytes (excluding NUL) on success. If `out` is too small the value is
 * truncated to fit + NUL and the FULL required length (>= out_size) is still
 * returned, so callers can detect truncation and resize. The name is validated
 * exactly as env_set validates it: returns ENV_ERR_INVAL on bad args (NULL/
 * empty/'='-containing name), ENV_ERR_TOOLONG when the name exceeds
 * ENV_NAME_MAX, ENV_ERR_NOTFOUND if the variable is absent. */
int env_get_copy(struct task *t, const char *name, char *out, uint32_t out_size);

/* --- Borrowed-read batch fast path (pointer valid only while locked) --- */
void        env_lock(struct task *t);
void        env_unlock(struct task *t);
/* Requires the caller to already hold env_lock(t). Returns a borrowed pointer
 * to the value portion of the matching entry, or NULL if absent. The pointer is
 * invalidated by the next env_set/env_unset and by env_unlock -- copy out
 * before releasing the lock. */
const char *env_peek_locked(struct task *t, const char *name);

/* Set (create or replace) `name`=`value`. Case-insensitive replace. Allocates
 * the new "KEY=VALUE" string BEFORE freeing any old one, so an allocation
 * failure leaves the prior value intact. Returns ENV_OK or a negative code. */
int env_set(struct task *t, const char *name, const char *value);

/* Remove `name` (validated as in env_set). Returns ENV_OK, ENV_ERR_NOTFOUND,
 * ENV_ERR_INVAL (NULL/empty/'='-containing), or ENV_ERR_TOOLONG. */
int env_unset(struct task *t, const char *name);

/* Deep-copy src's environ into dst (dst must be an unpublished child: its lock
 * is NOT taken). Snapshots src under src->environ_lock. On any allocation
 * failure the partial dst copy is unwound and dst->environ stays NULL. Called
 * from the process-creation path (NtCreateProcess child-env inheritance).
 * Returns ENV_OK or a negative code. */
int env_copy(struct task *dst, const struct task *src);

/* Free task->environ and task->argv and their strings; NULLs the fields. Call
 * ONLY at the task_cleanup reap barrier (task is TASK_DEAD, no thread of it
 * runs), so no lock is taken. Safe to call on an already-empty task. */
void env_free(struct task *t);

/* --- System default environment (system-default-variables feature) -------- */

/* Populate `t`'s environment with the system default variable set: a synthesised
 * base layer (COMPUTERNAME, USERNAME, USERPROFILE, APPDATA, TEMP, PATH, ...) that
 * is then OVERLAID by the machine-wide Registry key
 * `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment` and finally
 * by the per-user key `HKCU\Environment` (user overrides system overrides synth).
 * PATH is special-cased: a user `HKCU\Environment\PATH` is APPENDED to the base
 * PATH with ';', not replaced. Missing/unreadable Registry keys are skipped
 * (the synth base still lands), so this is safe to call before the Registry is
 * fully populated. Registry values that are not REG_SZ/REG_EXPAND_SZ are ignored.
 *
 * This is the seed used for the initial system process (PID 0) via
 * env_init_kernel_task(); ordinary child processes inherit their parent's block
 * through env_copy() (child-creation wiring owned by the native-API process/
 * thread lifecycle work), NOT by re-deriving Registry defaults. Callable only
 * from thread/boot context (takes the env mutex + reads the Registry); never
 * from an ISR. Returns ENV_OK, or the first negative env_set error code
 * encountered (best-effort: earlier successful sets are retained). */
int env_init_defaults(struct task *t);

/* Seed the initial system process (PID 0, task_get_by_pid(0)) with the boot
 * environment. When the Registry subsystem is ready it applies the full
 * env_init_defaults() set; otherwise (Registry-population failure) it falls back
 * to a minimal hardcoded bootstrap table (PATH/SYSTEMROOT/TEMP). Idempotent
 * enough to be called once from boot_phase3 right after task_init(). No-op if
 * PID 0 does not yet exist. Returns ENV_OK or a negative env_set error. */
int env_init_kernel_task(void);
