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
 * these). Values are in bytes of UTF-8, excluding the NUL terminator.
 *
 * BYTE-CAP CONTRACT (TODO-22 s10): ENV_NAME_MAX / ENV_VALUE_MAX are UTF-8 BYTE
 * caps, matching the UTF-8 storage model -- NOT the Windows UTF-16 CHARACTER
 * limits (name ~255 chars, value 32767 chars). The NT env syscalls
 * (NtSetEnvironmentVariable) convert the caller's UTF-16 to UTF-8 and THEN apply
 * these byte caps, so a UTF-16 value within the 32767-character limit but whose
 * UTF-8 encoding exceeds ENV_VALUE_MAX bytes is rejected (ENV_ERR_TOOLONG ->
 * STATUS_NAME_TOO_LONG). This is deliberate: it keeps one authoritative byte cap
 * across the storage, block-builder, and DoS-guard layers rather than tracking a
 * separate char count. Names/values are ASCII in every practical case, where
 * byte length == char length and the distinction is moot. */
#define ENV_NAME_MAX        256u   /* max variable name length (Windows practical limit) */
#define ENV_VALUE_MAX       32767u /* max variable value length (Windows 32K-1 limit) */
#define ENV_MAX_ENTRIES     511u   /* (n+1)*sizeof(char*) stays <= 4 KiB kmalloc array */
#define ENV_STR_KMALLOC_MAX 4096u  /* "KEY=VALUE" strings up to this via kmalloc; larger via PMM */

/* Per-process environment block sanity cap (TODO-22 s10). Windows Vista+ imposes
 * no hard block-size limit, but an unbounded block is a DoS vector now that
 * env_set is user-reachable via NtSetEnvironmentVariable (s5). env_set / the
 * exec-adoption / block-parse paths reject an operation that would push the total
 * block ("name=value\0"... + final NUL) past ENV_BLOCK_MAX (ENV_ERR_NOSPACE), and
 * a klog warning fires the first time the block crosses ENV_BLOCK_WARN. */
#define ENV_BLOCK_MAX       (1u * 1024u * 1024u)   /* 1 MiB hard cap */
#define ENV_BLOCK_WARN      (256u * 1024u)         /* warn threshold */

/* Immutable kernel install root and system directory. SINGLE SOURCE OF TRUTH:
 * env_synth_base seeds SYSTEMROOT/WINDIR from ENV_SYSTEMROOT_DIR, and SearchPathW
 * (env_searchpath.c) derives its trusted system/Windows search legs from these
 * compile-time constants -- NEVER from the caller-mutable %SYSTEMROOT% env var,
 * which NtSetEnvironmentVariable lets a process rewrite (a DLL-hijack vector if
 * it steered a trusted search leg). Impossible OS roots at C:\Impossible, not
 * C:\Windows (deliberate branding divergence). */
#define ENV_SYSTEMROOT_DIR  "C:\\Impossible"
#define ENV_SYSTEM32_DIR    ENV_SYSTEMROOT_DIR "\\System32"

/* Return codes: 0 on success, negative on error. */
#define ENV_OK             0
#define ENV_ERR_INVAL     (-1)     /* bad args / invalid name */
#define ENV_ERR_NOMEM     (-2)     /* allocation failed */
#define ENV_ERR_TOOLONG   (-3)     /* name or value exceeds a limit */
#define ENV_ERR_NOSPACE   (-4)     /* environ array is full (ENV_MAX_ENTRIES) */
#define ENV_ERR_NOTFOUND  (-5)     /* variable does not exist (env_unset) */
#define ENV_ERR_UNSUPPORTED (-6)   /* operation needs infrastructure not present yet
                                    * (per-user token / loaded-profile / SMP-safe
                                    * runtime Registry snapshot) -- see env_create_block */

/* --- Shared environment buffer allocator (TODO-22 s21) -------------------
 * ONE implementation of the environment subsystem's "kind by size" allocation
 * rule: kmalloc for <= ENV_STR_KMALLOC_MAX, else identity-mapped contiguous PMM
 * frames. It existed as three near-identical private copies (env.c env_str_alloc,
 * nt_env.c nt_env_alloc, env_searchpath.c sp_alloc), which is the third-occurrence
 * trigger for kernel-code-quality Gate 10; the copies are now thin adapters over
 * this pair so the size-class rule -- and any future change to it -- lives in one
 * place.
 *
 * CALLER-SUPPLIED SIZE IS LOAD-BEARING: `n` passed to env_buf_free MUST be the
 * exact byte count passed to env_buf_alloc. The size selects the DEALLOCATOR, so a
 * mismatch that crosses ENV_STR_KMALLOC_MAX calls kfree() on PMM frames or
 * pmm_free_frame() on heap memory. A self-describing {magic,total_bytes} header
 * would remove that caller obligation entirely; it is a layout change to every
 * allocation (and nests under env_create_block's existing env_block_hdr), so it is
 * owned by TODO-22 s22 (item: "Fold the caller-supplied size into a
 * self-describing env_buf header") rather than done here.
 *
 * n == 0 returns NULL (there is no zero-byte allocation to free); env_buf_free
 * ignores a NULL pointer. Callers that need a minimum 1-byte buffer for an empty
 * string normalize the count themselves before calling. */
void *env_buf_alloc(uint32_t n);
void  env_buf_free(void *p, uint32_t n);

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
 * failure leaves the prior value intact. Maintains the environ[] SORTED
 * invariant (case-insensitive by name, ASCII fold): a new name is inserted at
 * its ordered position; a replace keeps the (unchanged) position. Rejects a set
 * that would push the total block past ENV_BLOCK_MAX (ENV_ERR_NOSPACE) and warns
 * once at ENV_BLOCK_WARN. Returns ENV_OK or a negative code. */
int env_set(struct task *t, const char *name, const char *value);

/* Remove `name` (validated as in env_set). Returns ENV_OK, ENV_ERR_NOTFOUND,
 * ENV_ERR_INVAL (NULL/empty/'='-containing), or ENV_ERR_TOOLONG. */
int env_unset(struct task *t, const char *name);

/* --- s16: elevation security (AT_SECURE parallel) ------------------------
 * A privilege-sensitive variable is one an elevated (High/System integrity)
 * process must never observe or export: LD_PRELOAD, LD_LIBRARY_PATH (loader
 * hijack vectors honored only for NORMAL processes by the Linux compat layer),
 * and any name beginning "_IMPOSSIBLE_DEBUG_" (debug knobs). The blocklist match
 * is case-INSENSITIVE (the env store is), so a mixed-case spelling cannot bypass.
 *
 * The gate has two layers. (1) READ gate, active now: env_get_copy and
 * env_peek_locked report a blocklisted name as ABSENT, and the block builders
 * (env_build_block_utf16/env_build_block -> CreateEnvironmentBlock + Rtl NULL
 * expansion) omit it, whenever env_is_secure_context(owner) holds. (2) PHYSICAL
 * strip: env_sanitize_for_elevation removes the names outright at an elevation
 * transition; that INVOCATION site (UAC in-place token replacement; elevated
 * child env inheritance) is owned downstream and deferred (TODO-15 SRM,
 * 02-kernel-core/TODO-12 s7). */

/* True iff `name` (namelen bytes, no NUL required) is on the elevation blocklist.
 * Exposed for the s16 unit tests; the getters/builders call it internally. */
int env_name_is_privilege_sensitive(const char *name, uint32_t namelen);

/* True iff `t`'s primary token integrity level is above Medium (elevated admin
 * or System service), so the blocklist applies. FAIL-CLOSED: a task with no
 * token, or one whose integrity SID is malformed, is treated as secure (a
 * corrupt token cannot masquerade as benign Medium). Only a token with a valid
 * IL that resolves to <= Medium bypasses the gate -- so a normal-integrity
 * process keeps its legitimate LD_PRELOAD for the Linux compat layer. Caller
 * must keep `t` alive across the call (all callers pass current/a live parent). */
int env_is_secure_context(struct task *t);

/* True iff `t` is PROVEN elevated: non-NULL task + non-NULL token whose authoritative
 * IsElevated flag is set AND whose valid integrity SID is at or above High. The
 * authorization inverse of env_is_secure_context: a NULL / identity-less /
 * malformed-token / IsElevated-clear / below-High caller returns 0 (fail closed to
 * NOT-elevated), so an inconsistent token cannot be authorized to create machine-wide
 * state (e.g. an HKLM App Paths registration). Use this for a WRITE/authorization gate;
 * use env_is_secure_context for a READ restriction gate. */
int env_is_proven_elevated(struct task *t);

/* Physically strip every blocklisted variable from `t`'s environment (the
 * elevation-transition primitive). Returns the count removed. Detaches each
 * entry under environ_lock, then audit-logs the NAME (never the value) and frees
 * AFTER unlocking -- klog under environ_lock is forbidden. Idempotent; a clean
 * environment returns 0. */
int env_sanitize_for_elevation(struct task *t);

/* Deep-copy src's environ into dst (dst must be an unpublished child: its lock
 * is NOT taken). Snapshots src under src->environ_lock. On any allocation
 * failure the partial dst copy is unwound and dst->environ stays NULL. Intended
 * for every constructor (task_fork; the shared task_create that NtCreateProcess
 * and the boot/desktop launchers use; and task_create_user) to give a
 * NULL-lpEnvironment child the parent's block; it has NO live caller yet (wiring
 * owned by TODO-12 s7, fail-closed before the num_tasks++ publish), so children do
 * NOT inherit today. Returns ENV_OK or a negative code. */
int env_copy(struct task *dst, const struct task *src);

/* Free task->environ and task->argv and their strings; NULLs the fields. Call
 * ONLY at the task_cleanup reap barrier (task is TASK_DEAD, no thread of it
 * runs), so no lock is taken. Safe to call on an already-empty task. */
void env_free(struct task *t);

/* --- Argument (argv) array + exec argument handoff ------------------------- */

/* Deep-copy an argv vector into t->argv/t->argc, serializing on t->environ_lock.
 * Strings are allocated via the SAME env string allocator as environ, so
 * env_free() reclaims them with env_str_free(str, env_strlen(str)+1). Allocates
 * the whole new array BEFORE freeing any prior argv, unwinding on any failure so
 * a partial allocation never publishes. `argv` is a NULL-terminated-or-argc-
 * bounded kernel-side array of NUL-terminated strings (SYS_EXEC copies the
 * user vector into a kernel snapshot first -- task_set_argv never touches raw
 * user pointers). argc<=0 or argv==NULL clears argv to empty. Caps at
 * ARG_ARGC_MAX. Returns ENV_OK or a negative code. */
int task_set_argv(struct task *t, int argc, const char *const *argv);

/* Replace t->environ from a kernel-side array of `count` "KEY=VALUE" UTF-8
 * strings (used for SYS_EXEC envp adoption; entries come from a kernel snapshot,
 * never raw user pointers). Serializes on t->environ_lock; builds the whole new
 * array before freeing the old one (unwind on failure leaves the prior environ
 * intact). Malformed entries (no '=', over-long, empty name) are skipped.
 * `count` above ENV_MAX_ENTRIES is a hard error (ENV_ERR_NOSPACE), never a silent
 * truncation. Returns ENV_OK or a negative code. */
int env_adopt_block(struct task *t, const char *const *entries, uint32_t count);

/* Encode an argv vector into a Windows command-line string in `out` (CommandLine
 * / GetCommandLineW format; exact inverse of CommandLineToArgvW decode): quote
 * any arg containing space/tab/quote or an empty arg; emit 2n backslashes before
 * an interior quote and 2n+1 for a literal '"'; args separated by a single
 * space. Returns the length written (excluding NUL); if the result would exceed
 * `max`, `out` gets the truncated result + NUL and the return value is `max`
 * (truncation sentinel). `out` is always NUL-terminated when max>0. */
uint32_t argv_to_cmdline(int argc, const char *const *argv, char *out, uint32_t max);

/* --- CommandLineToArgvW command-line decode (TODO-22 s15) ------------------
 * Parse a Windows command line into an argv vector -- the inverse of
 * argv_to_cmdline for argv[1+] (see the argv[0] NOTE below). Two parallel forms
 * share ONE macro-generated parser body; the
 * W form parses UTF-16 code units DIRECTLY (all syntax characters -- space,
 * tab, '"', '\\' -- are ASCII, so every other WCHAR is preserved verbatim,
 * unlike a UTF-16<->UTF-8 transcode that would mutate lone surrogates). argv[1+]
 * follow the Windows backslash/quote rules: 2n backslashes + '"' -> n backslashes
 * + toggle "in quotes"; 2n+1 + '"' -> n backslashes + a literal '"'; a '"' inside
 * quotes immediately followed by another '"' is a single literal '"' that CLOSES
 * the quoted region (the modulo-3 consecutive-quote rule); backslashes not before
 * a '"' are literal; unquoted whitespace ends the argument. argv[0] uses the
 * Windows program-name rule (quote-delimited if it opens with '"', else
 * whitespace-delimited, backslashes literal). NOTE: this is the exact inverse of
 * argv_to_cmdline for argv[1+]; argv[0] is NOT a general inverse because Windows
 * parses the program name specially and the generic encoder does not (an argv[0]
 * carrying quotes/backslashes round-trips imperfectly -- see s15 follow-up / s4).
 *
 * Each returns ONE self-describing block: a hidden {magic,total_bytes} header
 * precedes a (argc+1) NUL-terminated element-pointer array (last entry NULL),
 * followed by the argument strings. The RETURNED pointer is the pointer array
 * (so it indexes like argv[]); free the whole block with cmdline_free_argv()
 * (Win32 callers LocalFree the CommandLineToArgvW result -- this kernel has no
 * LocalAlloc bookkeeping, hence the header). The command line is SNAPSHOTTED into
 * kernel memory so BOTH parse passes consume the same bytes (a concurrent write
 * cannot make pass 2 diverge from pass 1 and overrun the block); the caller must
 * still keep the command line valid and NUL-terminated for the duration of the
 * call, exactly as Win32 requires. Reentrant for the kmalloc path (blocks <= 4 KiB); a
 * block over 4 KiB rides pmm_alloc_contiguous, which shares the pre-existing
 * unlocked-PMM-bitmap exposure (owner: 03-memory-concurrency/TODO-03), not a new
 * hazard. No shared mutable state beyond that; caller->name is read lock-free. */

/* Caps on total argument-string units (bounds the single-block allocation and
 * keeps the sizing arithmetic well under UINT32_MAX). The W cap is in WCHARs
 * (the Windows command-line ceiling); the UTF-8 core cap is in bytes, sized so
 * any input under the WCHAR ceiling fits without a transcode penalty. */
#define CMDL_ARGV_MAX_WCHARS  32767u
#define CMDL_ARGV_MAX_BYTES   131072u

/* UTF-8 core: decode `cmdline` into a single-block char** argv; `*out_argc` gets
 * the count. cmdline==NULL -> NULL. An empty cmdline ("") yields argc==1 with
 * argv[0] = the caller's module identity (caller->name, or "" if unavailable) --
 * matching Win32 CommandLineToArgvW(L"") returning the executable path. A cmdline
 * whose decoded strings exceed CMDL_ARGV_MAX_BYTES returns NULL. Free with
 * cmdline_free_argv(). `caller` may be NULL (module-path case falls back to ""). */
char **cmdline_to_argv(struct task *caller, const char *cmdline, int *out_argc);

/* Win32 shell32 CommandLineToArgvW: wide form. Parses lpCmdLine UTF-16 code units
 * directly into a single-block uint16_t** (LPWSTR*), sets *pNumArgs. lpCmdLine==
 * NULL -> NULL. Empty -> *pNumArgs==1, argv[0] = caller module identity widened to
 * UTF-16. Over CMDL_ARGV_MAX_WCHARS or on allocation failure -> NULL (and the
 * executing thread's TEB LastErrorValue is set when a live TEB exists). A plain
 * kernel function taking an explicit caller (like SearchPathW), NOT an SSDT
 * syscall and NOT a pe.c export. Free with cmdline_free_argv(). */
uint16_t **CommandLineToArgvW(struct task *caller, const uint16_t *lpCmdLine,
                              int *pNumArgs);

/* Free a block returned by cmdline_to_argv or CommandLineToArgvW (or NULL). The
 * argument MUST be a value one of those returned, freed EXACTLY once; the hidden
 * header is magic-checked and poisoned so an immediate double free is a no-op
 * (the single-free contract is the real guarantee). Works for both widths -- the
 * header layout is width-independent. */
void cmdline_free_argv(void *argv);

/* Pure sizing helper: EXACT number of user-stack bytes the initial argv frame
 * consumes, matching the task_exec builder byte-for-byte -- qword-rounded string
 * bytes + string-area parity pad + argc parity pad + the argv[] pointer array
 * (argc + NULL) + the argc slot; it does NOT count the fixed auxv/AT_RANDOM/envp
 * block (that is ARGV_FRAME_RESERVE). The SYS_EXEC path and the builder both add
 * ARGV_FRAME_RESERVE and reject when the total would exceed USER_STACK_SIZE, so
 * an undercount here (which would permit a stack overwrite) must never happen.
 * Returns 0 for argc<=0/argv==NULL. */
uint32_t argv_frame_bytes(int argc, const char *const *argv);

/* --- %VAR% expansion (single-pass, Win32 ExpandEnvironmentStrings) --------- */

/* Expand `%VAR%` references in `input` into `output` using `t`'s environment.
 * Single-pass substitution (Win32 ExpandEnvironmentStrings semantics): each
 * `%NAME%` is replaced exactly once with env_get(t,NAME); a value that itself
 * contains `%OTHER%` is NOT re-expanded (delayed `!VAR!` re-expansion is a
 * distinct cmd.exe mode owned by the pseudo-variable section). `%%` is NOT a
 * cmd-style escape: it is an empty (unresolved) variable name, so both percent
 * signs are PRESERVED verbatim -- matching Win32/ntdll, where cmd.exe's `%%`->`%`
 * is a distinct shell mode. An unknown `%NAME%` and an unmatched trailing `%`
 * are likewise copied verbatim. Name matching is case-insensitive (ASCII fold,
 * matching the storage layer). Returns the number of bytes written (excluding
 * the NUL). If
 * the result would overflow `max_len`, `output` gets the truncated result + NUL
 * and the return value is `max_len` (a truncation sentinel). `output` is always
 * NUL-terminated when `max_len > 0`, EXCEPT the overlap-rejection case below.
 * `input` and `output` MUST NOT overlap (as with Win32 ExpandEnvironmentStrings);
 * an overlapping alias returns 0 and leaves BOTH buffers unchanged (it does not
 * even write output[0], since output may alias input). Caller contract: must NOT
 * already hold t->environ_lock (env_expand takes it for the whole walk). For
 * kernel-internal expansion pass the initial system process (task_get_by_pid(0))
 * as `t`. */
int env_expand(struct task *t, const char *input, char *output, uint32_t max_len);

/* Build a UTF-16 NT environment block ("NAME=VALUE\0"... double-NUL terminated)
 * from `t`'s UTF-8 environ, snapshotting under t->environ_lock. Entries are
 * converted UTF-8 -> UTF-16 via nls_cp_utf8_to_utf16 (NLS_CP_REPLACE, so a
 * malformed stored value becomes U+FFFD rather than failing the block). On
 * success `*out_block` is an allocated block and `*out_wchars` its total wchar
 * length (including the inter-entry NULs and the final terminator); free it with
 * env_free_block_utf16(*out_block, *out_wchars). Returns ENV_ERR_NOSPACE if the
 * block would exceed `max_wchars`, ENV_ERR_NOMEM on allocation failure,
 * ENV_ERR_INVAL on bad args. Used by RtlExpandEnvironmentStrings_U to honor the
 * NULL-Environment "calling process's own block" contract from kernel-resident
 * memory (the authoritative store), not the stale user-mapped PEB block. */
int env_build_block_utf16(struct task *t, uint16_t **out_block,
                          uint32_t *out_wchars, uint32_t max_wchars);

/* Free a block returned by env_build_block_utf16. `wchars` MUST be the same
 * `*out_wchars` that build returned. */
void env_free_block_utf16(uint16_t *block, uint32_t wchars);

/* --- Contiguous CreateProcess environment blocks (TODO-22 s10) ------------- */

/* Build a contiguous NT environment block into the CALLER's buffer, snapshotting
 * `t`'s environ under t->environ_lock. Entries are emitted in the sorted storage
 * order (case-insensitive by name), so the block satisfies the CreateProcess /
 * GetEnvironmentStrings "sorted" invariant that the storage layer maintains.
 *
 *   is_unicode == 0 (ANSI):    bytes,  "name=value\0"...  + one trailing '\0'
 *                              (an empty environment is "\0\0").
 *   is_unicode != 0 (UNICODE): UTF-16, "name=value\0"...  + one trailing 0x0000
 *                              (an empty environment is 0x0000 0x0000). Entries
 *                              convert UTF-8 -> UTF-16 via nls_cp (NLS_CP_REPLACE).
 *
 * `max_len` is the caller buffer size in BYTES. `*out_len` is set to the required
 * byte count in ALL cases (including too-small and empty), so a caller can size a
 * buffer with a first NULL/0 sizing call. Returns ENV_OK on a full write,
 * ENV_ERR_NOSPACE (no partial write) when `max_len` is too small, ENV_ERR_INVAL
 * on bad args. "ANSI" here means the raw stored UTF-8 bytes (Impossible stores
 * UTF-8, not a legacy system code page). Callable from thread context only. */
int env_build_block(struct task *t, void *out_buf, uint32_t max_len,
                    int is_unicode, uint32_t *out_len);

/* Parse a contiguous CreateProcess-style environment block (from a caller-built
 * `lpEnvironment`) and REPLACE `t`'s environ with it, atomically (build the whole
 * new array, then swap under t->environ_lock -- a malformed block or allocation
 * failure leaves the prior environ intact). `len` is the block length in BYTES.
 *   is_unicode == 0: ANSI/UTF-8 "name=value\0"... block.
 *   is_unicode != 0: UTF-16 block; each entry converts UTF-16 -> UTF-8 first.
 * Validation mirrors env_adopt_block (which does the atomic swap): an entry with
 * an empty name, a '=' inside the name, or over the byte caps is SKIPPED; the
 * resulting environ is sorted and de-duplicated (last occurrence wins). A block
 * whose total exceeds ENV_BLOCK_MAX, or more than ENV_MAX_ENTRIES entries, is a
 * hard error (ENV_ERR_NOSPACE). Returns ENV_OK or a negative code. */
int env_parse_block(struct task *t, const void *block, uint32_t len,
                    int is_unicode);

/* --- userenv.dll CreateEnvironmentBlock / DestroyEnvironmentBlock (s13) ---- */

/* Upper bound on a created block, in WCHARs. MUST stay <= the
 * RtlExpandEnvironmentStrings_U explicit-block scan bound (RTL_ENV_BLOCK_MAX_WCHARS)
 * so a block produced here is always consumable by the Rtl expansion path
 * (ExpandEnvironmentStringsForUser); a _Static_assert in nt_rtlenv.c pins that.
 * The two were EQUAL until TODO-22 s19 raised the scan bound to 1 MiWCHAR; only
 * `<=` is required, so this stayed at 64 KiWCHAR.
 *
 * KNOWN LIMITATION (not merely a reworded invariant): the store accepts an environ
 * up to ENV_BLOCK_MAX (1 MiB), and s19 made the expansion path consume all of it,
 * but env_create_block still returns ENV_ERR_NOSPACE past 64 KiWCHAR -- so a large
 * but valid environ is expandable yet not create-block-able. Raising this to match
 * means a ~2 MiB contiguous-PMM allocation, which needs the PMM bitmap SMP-locking
 * work verified under load first (03-memory-concurrency/TODO-03); tracked as a
 * concrete item in TODO-22 s22. */
#define ENV_CREATE_BLOCK_MAX_WCHARS 65536u

/* Win32 userenv.dll CreateEnvironmentBlock(LPVOID *lpEnvironment, HANDLE hToken,
 * BOOL bInherit). Produces a SELF-DESCRIBING contiguous UTF-16 environment block
 * (a hidden {magic,wchar-count} header precedes the returned pointer, so the
 * pointer-only env_destroy_block can recover the allocation size -- the kernel
 * has no LocalAlloc/LocalFree size bookkeeping). `*out_block` is the block BODY
 * (sorted "NAME=VALUE\0"... double-NUL terminated), which callers read exactly
 * like any NT env block; free it with env_destroy_block().
 *
 * Supported today (no runtime Registry walk): htoken == NULL && inherit != 0
 * snapshots `caller`'s current environment (already the assembled system+user set
 * from process creation). The environ snapshot serializes under
 * caller->environ_lock; the block allocation itself uses env_str_alloc, so a block
 * over ENV_STR_KMALLOC_MAX (4 KiB) rides the SAME pre-existing unlocked-PMM
 * exposure that env_build_block_utf16 / RtlExpandEnvironmentStrings_U already carry
 * (owned by the PMM bitmap SMP-locking work in 03-memory-concurrency/TODO-03), NOT
 * a new hazard. The other Win32 branches need
 * infrastructure that is not present yet and return ENV_ERR_UNSUPPORTED without
 * fabricating data:
 *   - htoken != NULL  -> a per-user block needs a token-SID -> HKU-hive map and
 *     LoadUserProfile, neither of which exists (TODO-22 s13 defers this). Returning
 *     the process-global user's data for an arbitrary token would leak another
 *     identity's PATH/TEMP/profile, so it is refused.
 *   - inherit == 0    -> a fresh Registry-only block needs an SMP-safe runtime
 *     Registry snapshot (env_init_defaults is documented boot-context-only because
 *     the Registry has no SMP lock); deferred, so refused rather than raced.
 * Returns ENV_OK; ENV_ERR_INVAL (NULL out/caller); ENV_ERR_UNSUPPORTED (deferred
 * branch above); ENV_ERR_NOSPACE (environ exceeds ENV_CREATE_BLOCK_MAX_WCHARS);
 * ENV_ERR_NOMEM. `htoken` is an ACCESS_TOKEN* (typed void* to keep env.h free of
 * the security headers); only its NULL-ness is consulted today. Thread context
 * only (takes caller->environ_lock). */
int env_create_block(struct task *caller, const void *htoken, int inherit,
                     void **out_block);

/* Produce an EMPTY environment block in the same self-describing format
 * env_create_block returns (hidden {magic,wchar-count} header, body = the two-WCHAR
 * "\0\0" empty-block form), freed by the same env_destroy_block. Takes no task and
 * no lock: an empty block reads nothing from any environ.
 *
 * WHY THIS IS NOT env_create_block(inherit == 0): the two zeroes mean different
 * things. `inherit == 0` is the Win32 userenv CreateEnvironmentBlock(bInherit =
 * FALSE) form, which must produce a fresh REGISTRY-DERIVED block for the token's
 * identity -- it is refused above precisely because that needs an SMP-safe runtime
 * Registry snapshot, and quietly returning an EMPTY block instead would be a wrong
 * answer, not a deferral. The ntdll RtlCreateEnvironment(CloneCurrent = FALSE) form
 * genuinely means EMPTY (ntdll walks no Registry). Separate entries keep each
 * contract honest while sharing one block format and one free path.
 *
 * Returns ENV_OK or ENV_ERR_INVAL (NULL out_block) / ENV_ERR_NOMEM. */
int env_create_empty_block(void **out_block);

/* Win32 userenv.dll DestroyEnvironmentBlock(LPVOID lpEnvironment). Frees a block
 * returned by env_create_block OR env_create_empty_block using only the pointer
 * (recovers the size from the hidden header) -- both go through the same single
 * constructor, so both free identically here.
 * NULL is a no-op. `block` MUST be a pointer previously returned by one of those two
 * entries (or NULL) -- exactly as Win32 DestroyEnvironmentBlock requires a
 * CreateEnvironmentBlock pointer; passing an arbitrary pointer is a caller error
 * (it reads the predecessor header). The header magic + wchar-cap check is a
 * BEST-EFFORT reject of an obviously-malformed header on an otherwise-valid pointer
 * (never a foreign-pointer validator, and NOT a use-after-free / double-free
 * guarantee -- once freed, the header memory may be reused). Call exactly once. */
void env_destroy_block(void *block);

/* --- Hidden drive-letter current-directory variables (=C:, =D:) ----------- */

/* Windows tracks the current directory of each drive letter in a HIDDEN
 * environment variable whose NAME literally begins with '=' -- "=C:", "=D:",
 * etc. (entry form "=C:=C:\Users\Default": the first '=' is part of the name,
 * the second separates name from value). These are stored in the SAME sorted
 * task->environ array as ordinary variables; because '=' (0x3D) sorts before any
 * letter, they naturally appear at the FRONT of a built environment block, and
 * env_copy would inherit them for free once inheritance is wired (TODO-12 s7 --
 * env_copy has no live caller today, so children do NOT yet receive them). They
 * are NOT enumerated by SET / user-facing listings (a display filter drops
 * leading-'=' names). The '=X:' name is the ONLY name form permitted to contain
 * '=' (env_name_classify accepts exactly "=<A-Z>:" and rejects any other
 * '='-containing name). */

/* Set the hidden "=X:" variable (X = uppercased `drive`) to `path` (the full
 * current directory for that drive, e.g. "C:\Users\Default"). `drive` must be an
 * ASCII letter; a non-letter or NULL `path` returns ENV_ERR_INVAL. Serializes on
 * t->environ_lock via env_set (same block-cap / allocation-atomicity contract).
 * Returns ENV_OK or a negative env_set error. */
int env_set_drive_cwd(struct task *t, char drive, const char *path);

/* Copy the current directory remembered for drive `drive` (its hidden "=X:"
 * variable) into `out` (NUL-terminated), under t->environ_lock. When the drive
 * has no remembered directory, `out` receives the drive root "X:\" and the call
 * still succeeds. A PRESENT value that is not a canonical absolute path ON drive
 * X (a foreign-drive or non-absolute value -- storable because env_set validates
 * the "=X:" NAME but not the VALUE) is REJECTED with ENV_ERR_INVAL, so a consumer
 * never resolves a drive-relative path against a wrong-volume base. `drive` must
 * be an ASCII letter; a non-letter, NULL out, or out_size < 4 returns
 * ENV_ERR_INVAL. Returns the value length in bytes (excluding NUL) on success --
 * like env_get_copy, an oversize value is truncated to fit and the FULL required
 * length (>= out_size) is returned so a caller can detect truncation. This is a
 * COPY-OUT (never a borrowed pointer): the env reader-lifetime contract forbids
 * returning a raw pointer that a sibling env_set/env_unset would free after the
 * lock drops. */
int env_get_drive_cwd(struct task *t, char drive, char *out, uint32_t out_size);

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
 * env_init_kernel_task(). Ordinary child processes are DESIGNED to inherit their
 * parent's block through env_copy() (NOT by re-deriving Registry defaults), but
 * that inheritance is not yet wired: env_copy() has no live caller -- every
 * constructor (task_fork, the shared task_create that NtCreateProcess + the
 * boot/desktop launchers use, and task_create_user) must call it fail-closed
 * before the num_tasks++ publish (owned by the native-API process/thread
 * lifecycle work, TODO-12 s7). Callable only from thread/boot context (takes the
 * env mutex + reads the Registry); never
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
