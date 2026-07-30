/* ============================================================================
 * test_usermode.c -- Kernel-side launcher for user-mode test binaries
 *
 * Ships (baseline spawn-and-wait) and (manifest, timeouts, TAP,
 * SKIP, filter) of TODO-04. See include/kernel/test/test_usermode.h
 * for the public API contract.
 *
 * Sequencing: the launcher is single-threaded by design. Binaries run
 * one at a time so a leaked file handle, dirty Registry key, or stuck
 * process from binary N cannot perturb binary N+1's run. Per-test
 * isolation hardening (scratch dir + handle-leak detection) is owned
 * by; this file gets the basic sequence + watchdog right.
 *
 * Preemptive scheduling: test_usermode_run() enables the scheduler
 * for the duration of the launcher run and disables it on return.
 * Without this, a test_*.exe that spins in user mode without making
 * any syscall would block CPU 0 forever -- cooperative yield() cannot
 * wrest control back from a spinning user task, and the timeout
 * watchdog below depends on the launcher periodically regaining the
 * CPU to check uptime_ms(). Wrapped in enable/disable so the rest of
 * boot_phase3 (which assumes single-threaded init order) is
 * unaffected. Mirrors the sys_wq creation wrapper at
 * boot_desktop.c:86.
 *
 * The path-passing trick:
 *   task_create(loader_func, name) launches a kernel task that runs
 *   loader_func once. The loader function reads `s_pending_test_path`
 *   (a file-scope volatile pointer the launcher set just before the
 *   task_create call), opens the file, kmalloc's a buffer, and then
 *   calls task_exec(buf, size) to morph the kernel task into a user
 *   task running the binary. Same pattern as exec_loader_func in
 *   src/kernel/main/test_threads.c. Single-threaded launch + polled
 *   wait means there is no race on s_pending_test_path.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/klog.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/fs/vfs.h"
#include "kernel/sched/task.h"
#include "kernel/ipc/signal.h"
#include "kernel/ob/ob_process.h"
#include "kernel/timer.h"
#include "kernel/test/test_usermode.h"
#include "kernel/csprng.h"
#include "kernel/time/mono_clock.h"
#include "registry.h"

/* ---- Internal state -------------------------------------------------- */

/* Path of the binary the next utest_loader_func() invocation will load.
 * Set by test_usermode_run() immediately before each task_create call;
 * read by utest_loader_func() once the new task is scheduled in.
 * Volatile because the loader runs in a different scheduling slot. */
static volatile const char *s_pending_test_path;

/* Filter set by -- NULL means "run every test_*.exe". */
static const char *s_filter;

/* Per-binary wall-clock timeout in ms. 0 = default. */
static uint32_t s_timeout_ms;

/* TAP mode: 1 = emit `ok N - name` / `not ok N - name` / `1..N` plan. */
static int s_tap_mode;

/* per-test isolation: 1 = scratch dir + Registry wipe + handle-leak
 * detection around each binary. Default 1 (ON); boot.conf
 * utest_isolation=0 flips it off for debugging broken cleanup hooks. */
static int s_isolation_enabled = 1;

/* CI-friendly output formats. Orthogonal to TAP and to each
 * other: any subset can be enabled and all enabled formats emit
 * interleaved on serial, tagged with distinct `[UTEST-XML]` /
 * `[UTEST-JSON]` prefixes so scripts/test.sh can split them by grep. */
static int s_xml_mode;
static int s_json_mode;

/* test-type taxonomy: stress iteration count (0 = built-in default).
 * Set from boot.conf `stress_iters=<N>` via test_usermode_set_stress_iters. */
static uint32_t s_stress_iters;
#define UTEST_STRESS_DEFAULT_ITERS 100u

/* UTEST color-scope flag. Set by the launcher around each spawn
 * (task_create -> task_cleanup); read by klog's color picker so every
 * kernel subsystem line emitted WHILE a user-mode test binary is the
 * live task (sched/exec/elf/signal/etc.) renders in the UTEST color
 * instead of the default per-level color. The launcher is
 * single-threaded on a single CPU at boot_tests_run time, so one
 * global suffices -- no per-CPU ABI churn. Exposed to klog via
 * test_usermode_color_active() below. */
static volatile int s_utest_color_active;

int test_usermode_color_active(void)
{
    return s_utest_color_active;
}

/* Defaults matching the test checkpoint: 10s wall clock is long
 * enough for a trivial test_*.exe on WHPX TCG (launch overhead plus
 * ELF load plus a few syscalls is <2s), short enough that a genuine
 * hang is caught in one boot cycle. */
#define UTEST_DEFAULT_TIMEOUT_MS 10000u
/* Grace period after SIGKILL before we force state=DEAD. */
#define UTEST_KILL_GRACE_MS       500u
/* Max binaries a single manifest can list; beyond this, extras fall
 * through to the directory-glob fallback. 128 is ~2x the -
 * planned binary set and matches what a future stress-test batch
 * would realistically enumerate. */
#define UTEST_MANIFEST_MAX        128u

void test_usermode_set_filter(const char *filter)
{
    s_filter = (filter && filter[0]) ? filter : (const char *)0;
}

void test_usermode_set_timeout_ms(uint32_t ms)
{
    s_timeout_ms = ms;
}

void test_usermode_set_tap(int enable)
{
    s_tap_mode = enable ? 1 : 0;
}

void test_usermode_set_isolation(int enable)
{
    s_isolation_enabled = enable ? 1 : 0;
}

void test_usermode_set_xml(int enable)
{
    s_xml_mode = enable ? 1 : 0;
}

void test_usermode_set_json(int enable)
{
    s_json_mode = enable ? 1 : 0;
}

void test_usermode_set_stress_iters(uint32_t n)
{
    s_stress_iters = n;
}

/* ---- Tiny string helpers (no libc deps in kernel) -------------------- */

static int u_strncmp(const char *a, const char *b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb)
            return (int)ca - (int)cb;
        if (ca == 0)
            return 0;
    }
    return 0;
}

static int u_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s != *prefix)
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

static int u_ends_with(const char *s, const char *suffix)
{
    uint32_t sl = 0, fl = 0;
    while (s[sl]) sl++;
    while (suffix[fl]) fl++;
    if (fl > sl)
        return 0;
    return u_strncmp(s + (sl - fl), suffix, fl + 1) == 0;
}

/* fnmatch-style `*` glob (single wildcard supported, anywhere). The
 * §4 spec calls for "literal name or `*`-glob" and that's exactly
 * what tests need (`test_smoke_*.exe` etc.). NULL pattern matches
 * everything. Exposed for unit tests (u_glob_match_public). */
int test_usermode_glob_match(const char *pattern, const char *name);
int test_usermode_glob_match(const char *pattern, const char *name)
{
    const char *star;
    uint32_t prefix_len, suffix_len, name_len;
    const char *suffix;

    if (!pattern)
        return 1;
    star = pattern;
    while (*star && *star != '*') star++;
    if (!*star) {
        const char *p = pattern, *n = name;
        while (*p && *n && *p == *n) { p++; n++; }
        return *p == 0 && *n == 0;
    }
    prefix_len = (uint32_t)(star - pattern);
    suffix = star + 1;
    name_len = 0;
    while (name[name_len]) name_len++;
    if (name_len < prefix_len)
        return 0;
    if (u_strncmp(pattern, name, prefix_len) != 0)
        return 0;
    suffix_len = 0;
    while (suffix[suffix_len]) suffix_len++;
    if (suffix_len > name_len - prefix_len)
        return 0;
    return u_strncmp(name + (name_len - suffix_len), suffix, suffix_len + 1) == 0;
}

static int u_is_test_binary(const char *name)
{
    if (!u_starts_with(name, "test_"))
        return 0;
    if (!u_ends_with(name, ".exe"))
        return 0;
    return 1;
}

/* Why a name gets refused, kept SEPARATE from "this is not a test file".
 *
 * A readdir entry that is not `test_*.exe` is not addressed to this
 * framework at all and stays silently ignored, exactly as before. A
 * manifest entry, or a discovered entry that IS test-shaped, is a binary
 * somebody intended to run -- so refusing it is a result, not a filter,
 * and it has to reach the artifacts as a counted infrastructure failure.
 * Dropping one silently is the false-green this taxonomy exists to close:
 * before it, a rejected entry never reached total_planned, so it vanished
 * from every artifact while the suite still reported success. */
typedef enum {
    UTEST_NAME_ACCEPT = 0,
    UTEST_NAME_NOT_TEST_SHAPED, /* not test_*.exe -- ignore, do not count */
    UTEST_NAME_REFUSE_LENGTH,   /* longer than the derived record bound   */
    UTEST_NAME_REFUSE_CHARSET,  /* byte outside [A-Za-z0-9._-]            */
    UTEST_NAME_REFUSE_PATH,     /* `..` traversal component               */
    UTEST_NAME_REFUSE_NUL       /* NUL inside the manifest line's span    */
} utest_name_verdict_t;

/* Defined after the record-bound derivation, which is expressed in terms
 * of UTEST_RECORD_LINE_MAX and so cannot precede the parser that calls
 * these. `span_len` is the length of the raw bytes the caller holds: the
 * manifest passes its line span so an embedded NUL is caught rather than
 * silently truncating the name, and the glob path passes the dirent's own
 * string length. */
static utest_name_verdict_t u_classify_name_span(const char *name,
                                                 uint32_t span_len);
static utest_name_verdict_t u_classify_name(const char *name);
static uint32_t u_name_digest(const char *p, uint32_t len);

/* Stricter gate used for manifest entries: the file is user-provided
 * text and u_run_one concatenates `C:\<name>` before vfs_open+task_exec,
 * so entries must stay in the C:\ root and must not contain path
 * separators, drive-letter colons, or upwards traversal components.
 * The glob-discovery path is already implicitly safe because vfs_readdir
 * returns one directory entry at a time, but the manifest path has no
 * such guard.
 *
 * Now a thin boolean view of the taxonomy above, kept because both the
 * unit tests and the cleanup-manifest path want the yes/no answer. Every
 * ENUMERATION call site takes the verdict instead, because "why" is what
 * decides between ignoring an entry and counting a failure. */
static int u_is_valid_manifest_name(const char *name)
{
    return u_classify_name(name) == UTEST_NAME_ACCEPT;
}

static uint64_t u_uptime_ms(void)
{
    return uptime_ns() / 1000000ULL;
}

/* ---- test-type taxonomy ----------------------------------------- *
 *
 * The launcher classifies each binary by filename prefix so it can
 * apply per-type policy: smoke runs FIRST with abort-on-FAIL, stress
 * loops N times, perf gets `classname="perf"` in the XML/JSON output.
 * An optional manifest `type=<value>` attribute overrides the
 * filename-derived default (for binaries that want to declare a
 * different policy or migrate without renaming).
 *
 * Prefix matching: `test_smoke_` beats `test_stress_` beats
 * `test_perf_` beats bare `test_`. All four prefixes are
 * case-sensitive (test_*.exe is lowercase by convention). Any
 * name that matches u_is_test_binary but none of the typed
 * prefixes defaults to UTEST_TYPE_CORRECTNESS.
 * --------------------------------------------------------------------- */

static utest_type_t u_type_for_name(const char *name)
{
    if (u_starts_with(name, "test_smoke_"))  return UTEST_TYPE_SMOKE;
    if (u_starts_with(name, "test_stress_")) return UTEST_TYPE_STRESS;
    if (u_starts_with(name, "test_perf_"))   return UTEST_TYPE_PERF;
    return UTEST_TYPE_CORRECTNESS;
}

/* Human-readable type label for the `classname` attribute in XML and
 * the `"type"` field in JSON. Kept short and hyphen-free so CI tools
 * can group by it without escaping. */
static const char *u_type_label(utest_type_t t)
{
    switch (t) {
    case UTEST_TYPE_SMOKE:       return "smoke";
    case UTEST_TYPE_STRESS:      return "stress";
    case UTEST_TYPE_PERF:        return "perf";
    case UTEST_TYPE_CORRECTNESS:
    default:                     return "correctness";
    }
}

/* Exact case-sensitive string equality. */
static int u_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Parse a manifest `type=<value>` attribute. Returns the enum for
 * known names, or UTEST_TYPE_CORRECTNESS for unrecognized inputs
 * (warn-and-continue). */
static utest_type_t u_type_from_attr(const char *val)
{
    if (u_streq(val, "smoke"))       return UTEST_TYPE_SMOKE;
    if (u_streq(val, "stress"))      return UTEST_TYPE_STRESS;
    if (u_streq(val, "perf"))        return UTEST_TYPE_PERF;
    if (u_streq(val, "correctness")) return UTEST_TYPE_CORRECTNESS;
    return UTEST_TYPE_CORRECTNESS;
}

/* ---- per-test isolation helpers --------------------------------- *
 *
 * For each binary the launcher creates a fresh scratch directory and
 * wipes a Registry subkey so leftover state cannot cross-pollute the
 * next binary. After the binary exits (PASS, FAIL, or timeout) the
 * launcher tears both down and records the task's final handle-table
 * count as a handle-leak signal. Opt out via boot.conf
 * utest_isolation=0 (consumed via test_usermode_set_isolation).
 * --------------------------------------------------------------------- */

/* Scratch root paths. `<name>` is the test binary filename minus the
 * ".exe" suffix (derived in u_derive_test_name below). Both roots use
 * backslash separators to match the Win32-native path convention. */
#define UTEST_SCRATCH_ROOT_L1 "C:\\Temp"
#define UTEST_SCRATCH_ROOT_L2 "C:\\Temp\\utest"
#define UTEST_REG_ROOT_L1     "SOFTWARE"
#define UTEST_REG_ROOT_L2     "SOFTWARE\\ImpossibleOS"
#define UTEST_REG_ROOT_L3     "SOFTWARE\\ImpossibleOS\\Test"

/* Hard caps on the iterative recursive-delete loop. The scratch dir is
 * launcher-owned and tests should not create deep hierarchies in it;
 * these caps exist as a paranoia floor against pathological FS state
 * that would otherwise infinite-loop the cleanup. */
#define UTEST_RMTREE_MAX_ENTRIES 512u
#define UTEST_RMTREE_MAX_DEPTH     8u

/* Strip a trailing ".exe" (case-insensitive) from `name_in` into
 * `out[out_cap]`. Returns 1 on success, 0 if the suffix was absent or
 * the resulting name would be empty / won't fit. `out` is always
 * NUL-terminated on success. */
static int u_derive_test_name(const char *name_in, char *out, uint32_t out_cap)
{
    uint32_t n = 0;
    uint32_t i;

    if (!name_in || !out || out_cap < 2)
        return 0;
    while (name_in[n]) n++;
    if (n < 5)   /* minimum "a.exe" is 5 chars; below that no stem remains */
        return 0;
    /* Match ".exe" / ".EXE" / mixed case at the tail. */
    if (name_in[n - 4] != '.' ||
        (name_in[n - 3] != 'e' && name_in[n - 3] != 'E') ||
        (name_in[n - 2] != 'x' && name_in[n - 2] != 'X') ||
        (name_in[n - 1] != 'e' && name_in[n - 1] != 'E'))
        return 0;
    if (n - 4 >= out_cap)
        return 0;
    for (i = 0; i < n - 4; i++)
        out[i] = name_in[i];
    out[i] = '\0';
    return 1;
}

/* Concat `parent\name` into `out[out_cap]`. Returns 1 on success, 0 on
 * overflow or empty input. */
static int u_path_join(const char *parent, const char *name,
                       char *out, uint32_t out_cap)
{
    uint32_t pi = 0;
    uint32_t ni = 0;

    if (!parent || !name || !out || out_cap < 3)
        return 0;
    while (parent[pi] && pi < out_cap - 2) {
        out[pi] = parent[pi];
        pi++;
    }
    if (parent[pi] != '\0' || pi == 0)
        return 0;  /* parent truncated or empty */
    if (out[pi - 1] != '\\' && out[pi - 1] != '/') {
        if (pi >= out_cap - 2)
            return 0;
        out[pi++] = '\\';
    }
    while (name[ni] && pi < out_cap - 1)
        out[pi++] = name[ni++];
    if (name[ni] != '\0')
        return 0;  /* name truncated */
    out[pi] = '\0';
    return 1;
}

/* Iterative recursive delete of a VFS path (file or directory). Safe
 * to call on a path that doesn't exist. Returns 0 on full cleanup, -1
 * if anything failed. Callers MUST check the return value and WARN on
 * failure so partial deletion does not silently let the next binary
 * reuse stale scratch state (Codex quality M 2026-04-20). */
static int u_rmtree(const char *path, uint32_t depth)
{
    struct vfs_node *n;
    uint8_t is_dir;

    if (depth > UTEST_RMTREE_MAX_DEPTH) {
        klog(LOG_WARN, "UTEST", "rmtree: depth > %u at '%s' -- aborting",
             (uint64_t)UTEST_RMTREE_MAX_DEPTH, path);
        return -1;
    }

    n = vfs_open(path, VFS_O_READ);
    if (!n)
        return 0;  /* absent -- nothing to do */
    is_dir = (uint8_t)(n->type & VFS_DIRECTORY);
    vfs_close(n);

    if (is_dir) {
        uint32_t pass;
        /* Loop: readdir[0] -> skip "."/".." -> recurse + unlink the
         * first real child -> restart readdir. Bounded by
         * UTEST_RMTREE_MAX_ENTRIES so a pathological FS state cannot
         * infinite-loop cleanup. */
        for (pass = 0; pass <= UTEST_RMTREE_MAX_ENTRIES; pass++) {
            struct vfs_node *dir;
            struct vfs_dirent *de;
            uint32_t idx;
            int found_real;
            char child_path[VFS_MAX_NAME + 64];
            char child_name[VFS_MAX_NAME];
            uint32_t ci;

            if (pass == UTEST_RMTREE_MAX_ENTRIES) {
                /* Cap exhausted -- directory still populated. Do NOT
                 * fall through to vfs_unlink: an attempt to delete a
                 * non-empty directory will fail, and more importantly
                 * we'd hide the cap-exhaustion in an ambiguous -1.
                 * Surface the cap hit explicitly so the caller can
                 * mark the run FAIL instead of letting stale state
                 * bleed through. */
                klog(LOG_WARN, "UTEST",
                     "rmtree: cap %u entries reached at '%s' -- partial cleanup",
                     (uint64_t)UTEST_RMTREE_MAX_ENTRIES, path);
                return -1;
            }

            dir = vfs_open(path, VFS_O_READ);
            if (!dir || !dir->ops || !dir->ops->readdir) {
                if (dir) vfs_close(dir);
                break;
            }

            found_real = 0;
            for (idx = 0; (de = dir->ops->readdir(dir, idx)) != (struct vfs_dirent *)0;
                 idx++) {
                /* Skip "." and ".." which some VFS backends include. */
                if (de->name[0] == '.' &&
                    (de->name[1] == '\0' ||
                     (de->name[1] == '.' && de->name[2] == '\0')))
                    continue;
                /* Snapshot the name (shared dirent storage). */
                for (ci = 0; de->name[ci] && ci < sizeof(child_name) - 1; ci++)
                    child_name[ci] = de->name[ci];
                child_name[ci] = '\0';
                found_real = 1;
                break;
            }
            vfs_close(dir);
            if (!found_real)
                break;

            if (!u_path_join(path, child_name, child_path, sizeof(child_path))) {
                klog(LOG_WARN, "UTEST",
                     "rmtree: path join overflowed for '%s\\%s' -- stopping",
                     path, child_name);
                return -1;
            }
            if (u_rmtree(child_path, depth + 1) != 0)
                return -1;  /* propagate cap / depth failures */
        }
    }

    return vfs_unlink(path);
}

/* Ensure a VFS directory exists. `parent` is the enclosing path, `full`
 * is the target. Tries to create; ignores errors (the target may
 * already exist). Returns 1 if the target is a directory after the
 * call, 0 otherwise. */
static int u_ensure_dir(const char *full)
{
    struct vfs_node *n = vfs_open(full, VFS_O_READ);
    if (n) {
        int is_dir = (n->type & VFS_DIRECTORY) != 0;
        vfs_close(n);
        return is_dir;
    }
    if (vfs_create(full, VFS_DIRECTORY) != 0)
        return 0;
    n = vfs_open(full, VFS_O_READ);
    if (!n)
        return 0;
    {
        int is_dir = (n->type & VFS_DIRECTORY) != 0;
        vfs_close(n);
        return is_dir;
    }
}

/* Pre-exec isolation: wipe stale state, create fresh scratch + Registry
 * subkey for `<name>`. Returns 0 on clean setup, -1 if anything failed
 * in a way that leaves stale state behind (cap-exhausted rmtree,
 * failed vfs_create). Caller treats -1 as a signal that the binary's
 * verdict should escalate to FAIL because the isolation contract
 * was violated. */
static int u_isolation_setup(const char *test_name)
{
    char path[VFS_MAX_NAME + 64];
    char reg_key[VFS_MAX_NAME + 64];
    int rc = 0;

    if (!vfs_is_mounted('C'))
        return 0;  /* no C:\ mount = nothing to isolate = not an error */

    /* L1 + L2 roots are shared across all binaries; create once, ignore
     * if they already exist. */
    (void)u_ensure_dir(UTEST_SCRATCH_ROOT_L1);
    (void)u_ensure_dir(UTEST_SCRATCH_ROOT_L2);

    /* Per-test scratch: wipe any leftover from a prior run, recreate
     * fresh. u_rmtree on a missing path is a no-op. If the pre-clear
     * rmtree fails (cap exhaustion, vfs error), the next vfs_create
     * may succeed at the existing-dir level but the contents are
     * still stale -- flag that so the caller can fail the run. */
    if (!u_path_join(UTEST_SCRATCH_ROOT_L2, test_name, path, sizeof(path))) {
        klog(LOG_WARN, "UTEST",
             "isolation: path join overflow for scratch '%s'", test_name);
        return -1;
    }
    if (u_rmtree(path, 0) != 0) {
        klog(LOG_WARN, "UTEST",
             "isolation: pre-clear of '%s' failed -- stale state may remain",
             path);
        rc = -1;
    }
    if (vfs_create(path, VFS_DIRECTORY) != 0) {
        klog(LOG_WARN, "UTEST",
             "isolation: failed to create scratch dir '%s'", path);
        rc = -1;
    }

    /* Registry subkey: HKLM\SOFTWARE\ImpossibleOS\Test\<name>.
     * RegCreateKeyEx is idempotent (open-or-create); RegDeleteTree
     * scrubs any prior content first so a previous run's dirty state
     * cannot bleed through. */
    {
        uint32_t pi = 0;
        uint32_t ni = 0;
        while (UTEST_REG_ROOT_L3[pi] && pi < sizeof(reg_key) - 2)
            reg_key[pi] = UTEST_REG_ROOT_L3[pi], pi++;
        if (pi < sizeof(reg_key) - 2)
            reg_key[pi++] = '\\';
        while (test_name[ni] && pi < sizeof(reg_key) - 1)
            reg_key[pi++] = test_name[ni++];
        reg_key[pi] = '\0';
        if (ni != 0 && test_name[ni] == '\0') {
            /* Clear any stale state, then ensure a fresh key exists. */
            (void)RegDeleteTree(HKEY_LOCAL_MACHINE, reg_key);
            {
                HKEY scratch = (HKEY)(uintptr_t)0;
                uint32_t disp = 0;
                if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, reg_key, 0,
                                   (char *)0, 0, 0, (void *)0,
                                   &scratch, &disp) == 0 && scratch)
                    (void)RegCloseKey(scratch);
            }
        }
    }

    return rc;
}

/* Post-exec isolation: read handle leak count (MUST be called BEFORE
 * task_cleanup -- once cleanup runs the handle table is gone). The
 * physical scratch + Registry tear-down has to wait until AFTER
 * task_cleanup closes the child's handles, because vfs_unlink rejects
 * targets with ref_count > 0: a leaked open handle on a file in the
 * scratch dir would otherwise block its deletion and leave stale state
 * for the next run (Codex quality H1, 2026-04-20). Split into two
 * phases accordingly: u_isolation_snapshot_leaks for the count-before-
 * teardown, u_isolation_reap for the delete-after-cleanup. */
static uint32_t u_isolation_snapshot_leaks(uint32_t child_pid)
{
    struct task *child = task_get_by_pid(child_pid);
    return child ? child->handle_table.count : 0u;
}

static int u_isolation_reap(const char *test_name)
{
    char path[VFS_MAX_NAME + 64];
    char reg_key[VFS_MAX_NAME + 64];
    int rc = 0;

    if (!vfs_is_mounted('C'))
        return 0;

    if (u_path_join(UTEST_SCRATCH_ROOT_L2, test_name, path, sizeof(path))) {
        if (u_rmtree(path, 0) != 0) {
            klog(LOG_WARN, "UTEST",
                 "isolation: post-run rmtree of '%s' failed -- stale scratch remains",
                 path);
            rc = -1;
        }
    }

    {
        uint32_t pi = 0;
        uint32_t ni = 0;
        while (UTEST_REG_ROOT_L3[pi] && pi < sizeof(reg_key) - 2)
            reg_key[pi] = UTEST_REG_ROOT_L3[pi], pi++;
        if (pi < sizeof(reg_key) - 2)
            reg_key[pi++] = '\\';
        while (test_name[ni] && pi < sizeof(reg_key) - 1)
            reg_key[pi++] = test_name[ni++];
        reg_key[pi] = '\0';
        if (ni != 0 && test_name[ni] == '\0')
            (void)RegDeleteTree(HKEY_LOCAL_MACHINE, reg_key);
    }

    return rc;
}

/* ---- Optional `tests/usermode-cleanup.manifest` ------------------- *
 *
 * Format: one path or Registry key per line, `#` starts a comment.
 * A line starting with `C:\` is deleted via vfs-rmtree (file or
 * directory). A line starting with `HKLM\` is deleted via
 * RegDeleteTree. Any other prefix is logged and skipped. Called once
 * per binary AFTER the per-test scratch + Registry teardown.
 *
 * Scope: tests that legitimately touch global state (e.g. DLL cache,
 * network sockets) can enumerate the paths/keys to scrub here.
 * --------------------------------------------------------------------- */

#define UTEST_CLEANUP_ARENA_BYTES 4096u
#define UTEST_CLEANUP_ARENA_PAGES 1u

/* Reject path-traversal components in a cleanup-manifest entry.
 * Returns 1 if the path contains any `..` component (after a `\`, `/`,
 * or at the start of the string), 0 if it is clean. Called for both
 * the `C:\...` and `HKLM\...` lines -- a `..` there is either an
 * attacker or a typo; either way reject.
 *
 * Without this guard the raw prefix check authorizes
 * `C:\Impossible\..\hello.txt` because the VFS walker resolves real
 * `..` entries in IXFS directories, so the delete escapes outside the
 * allowed subtree. The prefix match alone is not a containment
 * primitive; we have to ban the characters that let the walker leave
 * the subtree. */
static int u_path_has_traversal(const char *p)
{
    int at_component_start = 1;
    while (*p) {
        if (at_component_start && p[0] == '.' && p[1] == '.' &&
            (p[2] == '\0' || p[2] == '\\' || p[2] == '/'))
            return 1;
        at_component_start = (*p == '\\' || *p == '/');
        p++;
    }
    return 0;
}

static void u_cleanup_manifest_apply(void)
{
    struct vfs_node *f;
    uintptr_t arena_phys;
    char *arena;
    uint32_t size;
    int n;
    char *p;
    char *end;

    if (!vfs_is_mounted('C'))
        return;

    f = vfs_open("C:\\tests\\usermode-cleanup.manifest", VFS_O_READ);
    if (!f)
        return;  /* absent = normal */

    size = f->size;
    if (size == 0 || size >= UTEST_CLEANUP_ARENA_BYTES) {
        vfs_close(f);
        if (size >= UTEST_CLEANUP_ARENA_BYTES)
            klog(LOG_WARN, "UTEST",
                 "cleanup manifest %u bytes >= %u -- skipping",
                 (uint64_t)size, (uint64_t)UTEST_CLEANUP_ARENA_BYTES);
        return;
    }

    arena_phys = pmm_alloc_contiguous(UTEST_CLEANUP_ARENA_PAGES);
    if (!arena_phys) {
        vfs_close(f);
        klog(LOG_WARN, "UTEST", "cleanup manifest: pmm alloc failed");
        return;
    }
    arena = (char *)arena_phys;

    n = vfs_read(f, 0, size, (uint8_t *)arena);
    vfs_close(f);
    if (n <= 0 || (uint32_t)n != size) {
        pmm_free_frame(arena_phys);
        return;
    }

    /* In-place tokenize by newline and process each entry. */
    p = arena;
    end = arena + n;
    while (p < end) {
        char *line_start;
        char *line_end;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'))
            p++;
        if (p >= end) break;
        if (*p == '\n') { p++; continue; }
        if (*p == '#') {
            while (p < end && *p != '\n') p++;
            continue;
        }
        line_start = p;
        while (p < end && *p != '\n' && *p != '\r' && *p != '#')
            p++;
        line_end = p;
        while (line_end > line_start &&
               (line_end[-1] == ' ' || line_end[-1] == '\t'))
            line_end--;
        while (p < end && *p != '\n') p++;
        if (p < end) { *p = '\0'; p++; }
        if (line_end == line_start) continue;
        *line_end = '\0';

        if (line_start[0] == 'C' && line_start[1] == ':' &&
            line_start[2] == '\\') {
            /* Guard: cleanup manifest C:\ entries must live under
             * `C:\Impossible\` or the per-test scratch root so a typo
             * / malicious manifest cannot wipe hello.txt or cmd.exe
             * during a test boot. The per-test scratch at
             * `C:\Temp\utest\<stem>` is already scrubbed automatically
             * -- the cleanup manifest is for state OUTSIDE that root
             * (docs say DLL cache, etc.), which lives in
             * `C:\Impossible\` on this OS. */
            if (!(line_start[3] == 'I' && line_start[4] == 'm' &&
                  line_start[5] == 'p' && line_start[6] == 'o' &&
                  line_start[7] == 's' && line_start[8] == 's' &&
                  line_start[9] == 'i' && line_start[10] == 'b' &&
                  line_start[11] == 'l' && line_start[12] == 'e' &&
                  line_start[13] == '\\')) {
                klog(LOG_WARN, "UTEST",
                     "cleanup manifest: rejecting C:\\ entry outside C:\\Impossible\\ -- '%s'",
                     line_start);
                continue;
            }
            /* Reject `..` components -- a path like
             * `C:\Impossible\..\hello.txt` passes the prefix gate but
             * the VFS walker would escape outside the subtree. Codex
             * H3, 2026-04-20. */
            if (u_path_has_traversal(line_start)) {
                klog(LOG_WARN, "UTEST",
                     "cleanup manifest: rejecting C:\\ entry with '..' traversal -- '%s'",
                     line_start);
                continue;
            }
            (void)u_rmtree(line_start, 0);
        } else if (line_start[0] == 'H' && line_start[1] == 'K' &&
                   line_start[2] == 'L' && line_start[3] == 'M' &&
                   line_start[4] == '\\') {
            const char *sub = line_start + 5;
            /* Guard: reject empty suffix (`HKLM\\` alone).  RegDeleteTree
             * with an empty lpSubKey wipes every child of
             * HKEY_LOCAL_MACHINE, i.e. the whole registry, which is
             * catastrophic even under test=1.  Also restrict to the
             * ImpossibleOS test subtree to bound the blast radius. */
            if (sub[0] == '\0') {
                klog(LOG_WARN, "UTEST",
                     "cleanup manifest: rejecting bare 'HKLM\\' (would wipe root) -- '%s'",
                     line_start);
                continue;
            }
            if (!(sub[0] == 'S' && sub[1] == 'O' && sub[2] == 'F' &&
                  sub[3] == 'T' && sub[4] == 'W' && sub[5] == 'A' &&
                  sub[6] == 'R' && sub[7] == 'E' && sub[8] == '\\' &&
                  sub[9] == 'I' && sub[10] == 'm' && sub[11] == 'p' &&
                  sub[12] == 'o' && sub[13] == 's' && sub[14] == 's' &&
                  sub[15] == 'i' && sub[16] == 'b' && sub[17] == 'l' &&
                  sub[18] == 'e')) {
                klog(LOG_WARN, "UTEST",
                     "cleanup manifest: rejecting HKLM entry outside SOFTWARE\\Impossible -- '%s'",
                     line_start);
                continue;
            }
            /* Same `..` defense applies to Registry paths even though
             * reg_walk_path is stricter -- defense in depth. */
            if (u_path_has_traversal(sub)) {
                klog(LOG_WARN, "UTEST",
                     "cleanup manifest: rejecting HKLM entry with '..' traversal -- '%s'",
                     line_start);
                continue;
            }
            (void)RegDeleteTree(HKEY_LOCAL_MACHINE, sub);
        } else {
            klog(LOG_WARN, "UTEST",
                 "cleanup manifest: unknown prefix '%s' -- skipped",
                 line_start);
        }
    }

    pmm_free_frame(arena_phys);
}

/* ---- Manifest parser ------------------------------------------------ *
 *
 * Manifest format: one `test_*.exe` filename per line; `#` starts a
 * line comment; leading/trailing whitespace ignored; blank lines
 * skipped. First line that is not blank/comment is the first binary
 * to run, in file order. Maximum UTEST_MANIFEST_MAX binaries per
 * manifest -- a tripped cap logs a warning and falls back to the
 * directory glob for the remainder.
 *
 * Location: `C:\tests\usermode.manifest`. Absent = launcher falls
 * back to scanning C:\ root (legacy behavior). Makefile userland
 * target deploys the manifest file if `tests/usermode.manifest`
 * exists in the source tree.
 *
 * We store parsed entries in a file-scope static array of pointers
 * plus a single arena buffer for the filename text. The arena is
 * allocated once per test_usermode_run() invocation from PMM (8 KiB
 * exceeds kmalloc's 4 KiB ceiling per CLAUDE.md Freestanding Kernel
 * rules) and freed before return, so no permanent allocation survives.
 * ------------------------------------------------------------------ */

/* Refused manifest entries a run can publish individually. Separate from
 * UTEST_MANIFEST_MAX because a refusal costs no task slot and must never
 * be crowded out by runnable entries; smaller because a manifest with
 * dozens of malformed lines is already a broken manifest, and exhausting
 * this array is itself published as a fail-closed marker rather than
 * silently truncated. */
#define UTEST_MANIFEST_REFUSAL_MAX 64u
#define UTEST_MANIFEST_ARENA_BYTES 8192u  /* 128 entries * avg 64 bytes */
#define UTEST_MANIFEST_ARENA_PAGES 2u     /* 2 x 4 KiB */

struct manifest_state {
    const char  *names[UTEST_MANIFEST_MAX];
    utest_type_t types[UTEST_MANIFEST_MAX]; /*: type per entry */
    /* follow-up: expected total task_create cost for this binary,
     * including nested sys_fork calls. Default 1 (the launcher-spawned
     * task itself). Manifest entries tag fork-heavy binaries with
     * `expects_tasks=<N>` so the pre-flight budget check sums actual
     * slot consumption instead of counting binaries. Clamped to
     * 1..255 -- a binary claiming more than 255 task slots almost
     * certainly has a bug. */
    uint8_t      expects_tasks[UTEST_MANIFEST_MAX];
    uint32_t     count;
    /* Refused entries, in their OWN array rather than parallel to
     * names[]. Keeping them separate is load-bearing, not tidiness: when
     * refusals shared the runnable array they competed for the same
     * UTEST_MANIFEST_MAX slots, and the cap check ran BEFORE
     * classification and stopped parsing -- so a malformed entry in the
     * tail of an oversized manifest was never classified at all. Its
     * documented "tail runs via glob" fallback cannot recover it either,
     * because a name refused for an embedded NUL or an illegal byte may
     * not exist as a directory entry in the first place. Split apart, a
     * full runnable array can no longer hide a refusal.
     *
     * `refused_digest[]` is the FNV-1a of the entry's EXACT line span,
     * taken at parse time because that is the only point where the span
     * length is still known -- an entry refused for an embedded NUL has
     * a C string shorter than the bytes it came from, so hashing it
     * later would hash the truncation. */
    const char  *refused_names[UTEST_MANIFEST_REFUSAL_MAX];
    uint32_t     refused_digest[UTEST_MANIFEST_REFUSAL_MAX];
    uint8_t      refused_verdict[UTEST_MANIFEST_REFUSAL_MAX];
    uint32_t     refused_count;
    int          refused_overflowed; /* 1 = more refusals than we can hold */
    char        *arena;         /* pmm_alloc_contiguous()'d; NULL if not loaded */
    uintptr_t    arena_phys;    /* matching physical base for pmm_free_frame loop */
    uint32_t     arena_used;
    uint32_t     arena_cap;
    int          overflowed;    /* 1 = hit UTEST_MANIFEST_MAX cap */
};

/* This struct is a LOCAL in test_usermode_run, so every array added to it
 * comes off the stack the launcher runs on. Pinning the size makes a
 * future per-entry field an explicit decision instead of a stack overrun
 * discovered on hardware. */
_Static_assert(sizeof(struct manifest_state) <= 4096,
               "manifest_state is stack-allocated by test_usermode_run -- "
               "keep it well inside a single page");

static int u_manifest_load(struct manifest_state *ms)
{
    struct vfs_node *f;
    uint8_t *buf;
    int n;
    uint32_t size;

    ms->count       = 0;
    ms->refused_count      = 0;
    ms->refused_overflowed = 0;
    ms->arena       = (char *)0;
    ms->arena_phys  = 0;
    ms->arena_used  = 0;
    ms->arena_cap   = 0;
    ms->overflowed  = 0;

    f = vfs_open("C:\\tests\\usermode.manifest", VFS_O_READ);
    if (!f)
        return 0;  /* absent is normal */

    size = f->size;
    /* Cap at ARENA - 1 to guarantee room for a sentinel NUL at
     * arena[size].  Otherwise a file of exactly ARENA bytes whose last
     * line lacks a trailing newline lets the in-place tokenizer's
     * `*line_end = '\0'` write at arena[size], which is past the end
     * of the allocation -- a kernel-heap OOB write triggered by a
     * user-provided file. */
    if (size == 0 || size >= UTEST_MANIFEST_ARENA_BYTES) {
        vfs_close(f);
        if (size >= UTEST_MANIFEST_ARENA_BYTES)
            klog(LOG_WARN, "UTEST",
                 "manifest size %u >= %u -- falling back to glob",
                 (uint64_t)size, (uint64_t)UTEST_MANIFEST_ARENA_BYTES);
        return 0;
    }

    /* 8 KiB arena uses pmm_alloc_contiguous (CLAUDE.md Freestanding
     * Kernel rules: kmalloc is for <=4 KiB, larger buffers go through
     * PMM). Physical pages are identity-mapped in the kernel VA, so
     * the physical base doubles as a valid kernel virtual pointer. */
    ms->arena_cap  = UTEST_MANIFEST_ARENA_BYTES;
    ms->arena_phys = pmm_alloc_contiguous(UTEST_MANIFEST_ARENA_PAGES);
    if (!ms->arena_phys) {
        vfs_close(f);
        klog(LOG_WARN, "UTEST",
             "manifest pmm_alloc_contiguous(%u pages) failed -- falling back to glob",
             (uint64_t)UTEST_MANIFEST_ARENA_PAGES);
        return 0;
    }
    ms->arena = (char *)ms->arena_phys;
    buf = (uint8_t *)ms->arena;

    n = vfs_read(f, 0, size, buf);
    vfs_close(f);
    if (n <= 0 || (uint32_t)n != size) {
        klog(LOG_WARN, "UTEST", "manifest short read -- falling back to glob");
        for (uint32_t p = 0; p < UTEST_MANIFEST_ARENA_PAGES; p++)
            pmm_free_frame(ms->arena_phys + (uintptr_t)p * 4096u);
        ms->arena      = (char *)0;
        ms->arena_phys = 0;
        return 0;
    }

    /* In-place tokenize: walk lines, trim ws + comments, NUL-terminate,
     * append pointer to names[]. The arena buffer's raw content is
     * safe to overwrite -- we're done reading the source file. */
    {
        char *p = ms->arena;
        char *end = ms->arena + n;
        while (p < end) {
            char *line_start;
            char *line_end;
            /* Skip leading ws on the line. */
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'))
                p++;
            if (p >= end) break;
            if (*p == '\n') { p++; continue; }       /* blank line */
            if (*p == '#') {                         /* comment */
                while (p < end && *p != '\n') p++;
                continue;
            }
            line_start = p;
            while (p < end && *p != '\n' && *p != '\r' && *p != '#')
                p++;
            line_end = p;
            /* Strip trailing ws. */
            while (line_end > line_start &&
                   (line_end[-1] == ' ' || line_end[-1] == '\t'))
                line_end--;
            /* Skip the rest of the line (comment tail or EOL). */
            while (p < end && *p != '\n') p++;
            if (p < end) { *p = '\0'; p++; }
            /* Everything from line_start..line_end is the raw line
             * payload: a whitespace-delimited list of tokens where
             * the first is the binary name and subsequent tokens are
             * `key=value` attributes. Today only `type=<smoke|stress|
             * perf|correctness>` is recognized (of TODO-04); other
             * key/value pairs are silently ignored so a future schema
             * addition doesn't break older kernels. */
            if (line_end > line_start) {
                char *name_start;
                char *tok = line_start;
                utest_type_t entry_type;
                utest_name_verdict_t verdict;
                uint32_t name_span;
                /* Terminate the line in-place; safe by the ARENA-1
                 * cap above. */
                *line_end = '\0';

                /* Extract the first token (binary name). Stops at
                 * first whitespace inside the line. */
                name_start = tok;
                /* True span of the name token, measured against the
                 * LINE's end rather than by C-string scanning, and
                 * captured BEFORE the tokenizer writes its terminators.
                 * The scan below stops at an embedded NUL, so without
                 * this the parser -- and every consumer after it -- would
                 * see only the bytes preceding that NUL and never know
                 * the entry was longer. Comparing this span against the
                 * C length is what makes such an entry a counted refusal
                 * instead of a silently truncated name. */
                {
                    const char *e = name_start;
                    while (e < line_end && *e != ' ' && *e != '\t')
                        e++;
                    name_span = (uint32_t)(e - name_start);
                }
                while (*tok && *tok != ' ' && *tok != '\t')
                    tok++;
                if (*tok) {
                    *tok++ = '\0';
                    /* Skip over any run of whitespace between tokens. */
                    while (*tok == ' ' || *tok == '\t') tok++;
                }
                /* Default: infer type from filename prefix. Default
                 * expects_tasks to 1 (launcher task only). */
                entry_type = u_type_for_name(name_start);
                uint32_t entry_expects = 1u;
                /* Walk remaining tokens, honouring `type=<value>` and
                 * `expects_tasks=<N>`. */
                while (*tok) {
                    char *kv_end = tok;
                    while (*kv_end && *kv_end != ' ' && *kv_end != '\t')
                        kv_end++;
                    if (*kv_end) { *kv_end = '\0'; kv_end++; }
                    if (tok[0] == 't' && tok[1] == 'y' && tok[2] == 'p' &&
                        tok[3] == 'e' && tok[4] == '=') {
                        entry_type = u_type_from_attr(tok + 5);
                    } else if (tok[0] == 'e' && tok[1] == 'x' &&
                               tok[2] == 'p' && tok[3] == 'e' &&
                               tok[4] == 'c' && tok[5] == 't' &&
                               tok[6] == 's' && tok[7] == '_' &&
                               tok[8] == 't' && tok[9] == 'a' &&
                               tok[10] == 's' && tok[11] == 'k' &&
                               tok[12] == 's' && tok[13] == '=') {
                        /* Inline decimal parse -- up to 3 digits fit in
                         * the 1..255 uint8 slot without overflow. */
                        const char *p = tok + 14;
                        uint32_t n = 0;
                        while (*p >= '0' && *p <= '9' && n < 10000u)
                            n = n * 10u + (uint32_t)(*p++ - '0');
                        if (n == 0) n = 1;        /* 0 makes no sense */
                        if (n > 255u) n = 255u;   /* uint8 ceiling */
                        entry_expects = n;
                    }
                    tok = kv_end;
                    while (*tok == ' ' || *tok == '\t') tok++;
                }
                /* Enforce the manifest trust boundary on the bare name:
                 * test_*.exe, inside the accepted charset, within the
                 * derived record bound, no `..`, no embedded NUL.
                 *
                 * A refusal is KEPT, not dropped. Dropping it here is
                 * what used to make a rejected entry invisible: it never
                 * reached total_planned, so the artifacts and the host
                 * recount described a run that silently skipped a binary
                 * somebody asked for. The entry stays in the array with
                 * its verdict and its span digest so both walks can count
                 * it and the run walk can publish it as a failure.
                 *
                 * The raw name is NOT logged. It is untrusted bytes, and
                 * a control byte in it would split this very WARN across
                 * physical serial lines -- the sanitized identity the
                 * record carries is emitted by the run walk instead. */
                verdict = u_classify_name_span(name_start, name_span);
                if (verdict == UTEST_NAME_NOT_TEST_SHAPED) {
                    klog(LOG_WARN, "UTEST",
                         "manifest: ignoring entry %u -- not test_*.exe",
                         (uint64_t)(ms->count + ms->refused_count));
                    continue;
                }
                if (verdict != UTEST_NAME_ACCEPT) {
                    /* Refusals are classified and stored FIRST, before
                     * the runnable cap is consulted, so an oversized
                     * manifest can never stop the parser short of a
                     * malformed entry it has not yet examined. */
                    if (ms->refused_count >= UTEST_MANIFEST_REFUSAL_MAX) {
                        ms->refused_overflowed = 1;
                        continue;
                    }
                    ms->refused_names[ms->refused_count]   = name_start;
                    ms->refused_digest[ms->refused_count]  =
                        u_name_digest(name_start, name_span);
                    ms->refused_verdict[ms->refused_count] = (uint8_t)verdict;
                    ms->refused_count++;
                    continue;
                }
                /* Runnable entry: now the cap applies. `continue` rather
                 * than `break` because the tail still has to be scanned
                 * for refusals -- the glob fallback that picks up valid
                 * tail entries cannot see a name that is illegal as a
                 * filename. */
                if (ms->count >= UTEST_MANIFEST_MAX) {
                    ms->overflowed = 1;
                    continue;
                }
                ms->names[ms->count]         = name_start;
                ms->types[ms->count]         = entry_type;
                ms->expects_tasks[ms->count] = (uint8_t)entry_expects;
                ms->count++;
            }
        }
    }

    if (ms->overflowed)
        klog(LOG_WARN, "UTEST",
             "manifest entries truncated at %u -- tail runs via glob",
             (uint64_t)UTEST_MANIFEST_MAX);

    return ms->count > 0 ? 1 : 0;
}

static void u_manifest_free(struct manifest_state *ms)
{
    if (ms->arena) {
        uint32_t p;
        for (p = 0; p < UTEST_MANIFEST_ARENA_PAGES; p++)
            pmm_free_frame(ms->arena_phys + (uintptr_t)p * 4096u);
        ms->arena      = (char *)0;
        ms->arena_phys = 0;
    }
    ms->count = 0;
}

/* ---- Task entry: the loader that morphs into the test binary ---------
 *
 * Failure paths use task_exit(STATUS) -- a plain `return` from a kernel
 * task only sets TASK_DEAD but never wakes the parent's polling
 * watchdog.  task_exit is the canonical wakeup path.  Status codes:
 *   -1: NULL pending path (launcher bug)
 *   -2: vfs_open failed
 *   -3: pmm_alloc_contiguous failed
 *   -4: vfs_read short / size mismatch
 *   -5: task_exec failed
 * The launcher renders any negative exit code as `[UTEST] <name>: FAIL
 * (exit=N)` so loader failures surface even though the binary itself
 * never produced output.
 * --------------------------------------------------------------------- */

/* Staging release for the PMM shape of the ownership token: this loader stages
 * the binary in contiguous frames rather than on the kmalloc heap, so it cannot
 * share task_exec_staging_kfree. Clears the count first so a second call is a
 * no-op even independently of the token's own idempotence. */
static void utest_staging_free_frames(struct task_exec_staging *st)
{
    uint32_t n;

    if (!st || !st->pages)
        return;
    n = st->pages;
    st->pages = 0;
    /* pmm_free_contiguous is the declared symmetric counterpart of the
     * pmm_alloc_contiguous that staged these frames -- the per-frame loop this
     * file already open-codes twice does not need a third copy. */
    pmm_free_contiguous(st->phys, n);
}

static void utest_loader_func(void)
{
    const char *path = (const char *)s_pending_test_path;
    struct vfs_node *file;
    uint32_t size, pages, p;
    uintptr_t buf_phys;
    uint8_t *buf;
    int n, rc;

    if (!path || !path[0]) {
        klog(LOG_ERROR, "UTEST", "loader: NULL pending path");
        task_exit(-1);
    }

    file = vfs_open(path, VFS_O_READ);
    if (!file) {
        klog(LOG_ERROR, "UTEST", "%s: vfs_open failed", path);
        task_exit(-2);
    }

    size  = file->size;
    pages = (size + 4095u) / 4096u;
    buf_phys = pmm_alloc_contiguous(pages);
    if (!buf_phys) {
        klog(LOG_ERROR, "UTEST",
             "%s: pmm_alloc_contiguous(%u pages) failed",
             path, (uint64_t)pages);
        vfs_close(file);
        task_exit(-3);
    }
    buf = (uint8_t *)buf_phys;

    n = vfs_read(file, 0, size, buf);
    vfs_close(file);
    if (n <= 0 || (uint32_t)n != size) {
        klog(LOG_ERROR, "UTEST", "%s: vfs_read short (n=%d size=%u)",
             path, (int64_t)n, (uint64_t)size);
        for (p = 0; p < pages; p++)
            pmm_free_frame(buf_phys + (uintptr_t)p * 4096u);
        task_exit(-4);
    }

    /* task_exec stages an iretq frame for user mode (consumed on the
     * next scheduling switch). It DOES NOT take ownership of `buf` --
     * exec_load() inside copies the binary into user pages, so once
     * task_exec returns 0 the staging buffer is no longer needed.
     *
     * This loader runs with interrupts enabled, so the frame release must go
     * through the ownership token: on success task_exec drops these frames
     * before publication, because a tick can carry this task into the new image
     * before the call returns and the loop below would never run. */
    {
        struct task_exec_staging st = {
            .release = utest_staging_free_frames,
            .phys = buf_phys, .pages = pages
        };
        rc = task_exec(buf, size, &st);
        /* Covers BOTH failure outcomes: a pre-commit -1 (image intact) and
         * TASK_EXEC_IMAGE_DESTROYED (image gone). Either way the staging
         * frames are released here and the loader task exits below -- it must
         * never fall through to ring 3 with a destroyed image. On success this
         * is a no-op; task_exec already released. */
        task_exec_staging_release(&st);
    }
    if (rc < 0) {
        klog(LOG_ERROR, "UTEST", "%s: task_exec failed (rc=%d)",
             path, (int64_t)rc);
        /* Distinguish the two outcomes like the other three callers do. A
         * post-commit failure destroyed this task's image, and the launcher's
         * own -1..-5 codes sit inside the -(signum) range, so reporting one of
         * those here would be indistinguishable from a signal death to the same
         * oracle the exec lifecycle test relies on. */
        if (rc == TASK_EXEC_IMAGE_DESTROYED)
            task_exit(TASK_EXIT_EXEC_IMAGE_DESTROYED);   /* no return */
        task_exit(-5);
    }

    /* Force a cooperative reschedule so the prepared user-mode iretq
     * frame is consumed. yield() goes through schedule_now() which
     * switches regardless of the preemptive sched_enabled flag.
     * CRITICAL: do NOT replace this with `for(;;) hlt;` -- a user-mode
     * spinloop would block forever if we relied on HLT here. (
     * regression, 2026-04-20.) */
    for (;;)
        yield();
}

/* ---- Polled wait with timeout -------------------------------------- *
 *
 * Replaces task_waitpid in the path. Semantics:
 *  - Returns normally with the child's exit_status if the child
 *    reaches TASK_DEAD before the deadline.
 *  - On timeout: send SIGKILL, wait KILL_GRACE_MS for cooperative
 *    tear-down, then force state=TASK_DEAD + exit_status=TIMEOUT.
 *    The exit_status the caller sees is always UTEST_EXIT_TIMEOUT
 *    on the timeout path (overwrites SIGKILL's -9).
 *  - Caller still owns task_cleanup() for the child pid afterward.
 *
 * Needs preemptive scheduling to be enabled (see top-of-file comment
 * on scheduler_enable wrap) -- otherwise yield() gives control to a
 * spinning user task and never comes back. The launcher wraps the
 * whole run in scheduler_enable/disable so this function is safe.
 * ------------------------------------------------------------------ */

static int32_t u_wait_with_timeout(uint32_t pid, uint32_t timeout_ms,
                                   int *out_timed_out)
{
    struct task *t = task_get_by_pid(pid);
    uint64_t deadline, grace_deadline;

    *out_timed_out = 0;

    if (!t)
        return -1;

    if (timeout_ms == 0)
        timeout_ms = UTEST_DEFAULT_TIMEOUT_MS;

    deadline = u_uptime_ms() + (uint64_t)timeout_ms;

    while (t->state != TASK_DEAD) {
        if (u_uptime_ms() >= deadline) {
            *out_timed_out = 1;
            break;
        }
        yield();
    }

    if (*out_timed_out) {
        /* Cooperative kill first -- gives the task a chance to run
         * its signal_check on the next kernel entry, unwind cleanly,
         * and set its own exit_status. */
        signal_send(pid, SIGKILL);
        grace_deadline = u_uptime_ms() + (uint64_t)UTEST_KILL_GRACE_MS;
        while (t->state != TASK_DEAD && u_uptime_ms() < grace_deadline)
            yield();
        /* Forceful fallback: a user-mode spinloop with no syscall
         * never runs signal_check, so SIGKILL alone cannot land. We
         * have to mark the task DEAD ourselves.
         *
         * Safe in this kernel because task dispatch uses a single
         * global `current_task` ([src/kernel/sched/task.c]) -- APs do
         * not run scheduled tasks, so no other CPU can be dispatching
         * the child while the launcher (currently executing) decides
         * to force-kill. The existing signal_send() writes t->state
         * unlocked under the same assumption (ipc/signal.c:41).
         *
         * Route through task_terminate_remote() so the child gets the
         * SAME shared DEAD-transition teardown as every other death path
         * (OB process object mark-dead, syscall-filter count, Job Object
         * detach, timer-resolution reap). A direct `t->state = TASK_DEAD`
         * would leak Job membership + strand a timer-resolution request
         * into the next test suite.
         *
         * Defensive guard: refuse to force-kill ourselves; would
         * leave the running task DEAD and trip a cascading crash. */
        if (t->state != TASK_DEAD && t != task_current())
            task_terminate_remote(t, UTEST_EXIT_TIMEOUT);
        /* Either way, surface TIMEOUT so the launcher log / TAP / bat
         * output names the actual reason rather than SIGKILL's -9. */
        t->exit_status = UTEST_EXIT_TIMEOUT;
    }

    return t->exit_status;
}

/* ---- CI-friendly output format helpers ------------------------- *
 *
 * XML attribute escaping covers the five spec-required characters
 * (`&`, `<`, `>`, `"`, `'`) plus control bytes (< 0x20, except tab/LF
 * which XML 1.0 allows in attributes). JSON string escaping covers
 * `"`, `\`, and control bytes (< 0x20) per RFC 8259. Both writers use
 * a bounded output buffer with explicit length check on every append.
 *
 * Names today come from u_is_valid_manifest_name (no special chars),
 * so escape never triggers on the happy path; reason strings are
 * launcher-formatted (`exit=N`, `timeout`, `N handle(s) leaked`) and
 * equally safe. The escape is defense-in-depth for future consumers.
 * --------------------------------------------------------------------- */

/* Append `src` to `dst[*pos]`, bounded by `cap`. Returns 1 on
 * success, 0 if `src` would overflow. Leaves dst NUL-terminated. */
static int u_append(char *dst, uint32_t *pos, uint32_t cap, const char *src)
{
    uint32_t p = *pos;
    while (*src) {
        if (p + 1 >= cap) return 0;
        dst[p++] = *src++;
    }
    dst[p] = '\0';
    *pos = p;
    return 1;
}

/* Append a hex escape like `&#x1F;` (XML) or `\u001f` (JSON) for a
 * control byte c (< 0x20). */
static int u_append_hex2(char *dst, uint32_t *pos, uint32_t cap,
                         const char *prefix, const char *suffix, uint8_t c)
{
    static const char hex[] = "0123456789abcdef";
    char buf[8];
    uint32_t i = 0;
    while (prefix[i]) { buf[i] = prefix[i]; i++; }
    buf[i++] = hex[(c >> 4) & 0xF];
    buf[i++] = hex[c & 0xF];
    /* JSON wants 4-digit \u escape; we always pass "\u00" as prefix +
     * two hex digits, matching the JSON spec. For XML the prefix is
     * "&#x" and suffix is ";" -- we emit the suffix via the caller. */
    buf[i] = '\0';
    if (!u_append(dst, pos, cap, buf)) return 0;
    return u_append(dst, pos, cap, suffix);
}

/* Escape `src` into XML attribute-value text. Writes into
 * `dst[*pos..cap]`. Returns 1 on success, 0 on overflow. */
static int u_xml_escape(char *dst, uint32_t *pos, uint32_t cap, const char *src)
{
    while (*src) {
        unsigned char c = (unsigned char)*src++;
        const char *rep = (const char *)0;
        switch (c) {
        case '&':  rep = "&amp;"; break;
        case '<':  rep = "&lt;"; break;
        case '>':  rep = "&gt;"; break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&apos;"; break;
        default: break;
        }
        if (rep) {
            if (!u_append(dst, pos, cap, rep)) return 0;
            continue;
        }
        /* Control bytes other than tab/LF/CR are illegal in XML 1.0
         * even as entity references; skip them silently. (Our inputs
         * never contain them today.) */
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
            continue;
        if (*pos + 1 >= cap) return 0;
        dst[(*pos)++] = (char)c;
        dst[*pos] = '\0';
    }
    return 1;
}

/* Escape `src` into a JSON string-body (the bytes BETWEEN the two
 * double-quotes). Writes into `dst[*pos..cap]`. */
static int u_json_escape(char *dst, uint32_t *pos, uint32_t cap, const char *src)
{
    while (*src) {
        unsigned char c = (unsigned char)*src++;
        switch (c) {
        case '\"':
            if (!u_append(dst, pos, cap, "\\\"")) return 0;
            continue;
        case '\\':
            if (!u_append(dst, pos, cap, "\\\\")) return 0;
            continue;
        case '\n':
            if (!u_append(dst, pos, cap, "\\n")) return 0;
            continue;
        case '\r':
            if (!u_append(dst, pos, cap, "\\r")) return 0;
            continue;
        case '\t':
            if (!u_append(dst, pos, cap, "\\t")) return 0;
            continue;
        default: break;
        }
        if (c < 0x20) {
            if (!u_append_hex2(dst, pos, cap, "\\u00", "", c)) return 0;
            continue;
        }
        if (*pos + 1 >= cap) return 0;
        dst[(*pos)++] = (char)c;
        dst[*pos] = '\0';
    }
    return 1;
}

/* Append a decimal unsigned integer. */
static int u_append_uint(char *dst, uint32_t *pos, uint32_t cap, uint64_t v)
{
    char buf[24];
    uint32_t i = 0;
    if (v == 0) return u_append(dst, pos, cap, "0");
    while (v) {
        if (i >= sizeof(buf)) return 0;
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i--) {
        if (*pos + 1 >= cap) return 0;
        dst[(*pos)++] = buf[i];
    }
    dst[*pos] = '\0';
    return 1;
}

/* Render milliseconds as `S.MMM` (seconds with 3-digit millisecond
 * fraction) for the XML `time` attribute. */
static void u_format_seconds(char *dst, uint32_t cap, uint64_t ms)
{
    uint32_t pos = 0;
    uint64_t secs = ms / 1000ull;
    uint64_t frac = ms % 1000ull;
    u_append_uint(dst, &pos, cap, secs);
    if (pos + 4 < cap) {
        dst[pos++] = '.';
        dst[pos++] = (char)('0' + (frac / 100) % 10);
        dst[pos++] = (char)('0' + (frac / 10) % 10);
        dst[pos++] = (char)('0' + frac % 10);
        dst[pos] = '\0';
    }
}

/* ---- ring-3 self-report (SYS_TEST_REPORT) ----------------------- *
 *
 * A binary's exit code carries two outcomes; honest reporting needs
 * three. The harness submits its counters through syscall 48 and the
 * kernel parks them on the child's TCB (see TASK_UTEST_REPORT_* in
 * sched/task.h); the launcher lifts them here, BEFORE task_cleanup
 * destroys the TCB, exactly like the handle-leak snapshot above.
 *
 * Two dimensions, never summed: `asserts_*` count UTEST_ASSERT calls,
 * `skip_blocks` counts UTEST_SKIP sites taken, and one skip site
 * usually stands in for several assertions that never ran.
 * ------------------------------------------------------------------ */

/* Run-wide ceiling on synthetic skip records. The per-binary bound the
 * syscall enforces (TASK_UTEST_REPORT_SKIP_MAX) is multiplicative across
 * TASK_MAX binaries, so it cannot be the only stop; this is the aggregate
 * one.
 *
 * It is DERIVED from what a burst costs the transport, not picked. The
 * original 1024 was chosen while klog's per-subsystem rate limiter was the
 * real ceiling; the artifact records moved to klog_unrated (see above), so
 * this budget became the only bound on the burst and a picked number stopped
 * being defensible.
 *
 * The derivation is deliberately transport-INDEPENDENT -- a serialized-BYTE
 * allowance, never a time. Pinning it to a baud rate would be wrong on a
 * supported configuration: serial init accepts 9600/19200/38400/57600/115200,
 * preserves an unknown firmware-configured divisor when SPCR reports baud
 * code 0, and falls back to 38400 on an unrecognized rate
 * (src/kernel/drivers/serial.c:57-69), so a 115200-derived constant is off by
 * up to 12x. Completion TIME is the host deadline's job; the kernel's job is
 * to bound how many bytes it may add. */

/* Physical records per logical skip, counted on the WORST path rather than
 * the happy one. Three emitters fire per skip -- a TAP point, a JUnit
 * <testcase> and a JSON record (u_emit_skip_records below) -- and each of
 * them emits TWO records when its buffer overflows: an
 * [UTEST-RECORD-OVERFLOW] marker plus a verdict-preserving fallback record
 * (u_emit_tap_point, u_emit_xml_testcase and u_emit_json_testcase all take
 * that shape at their `overflow:` labels). A binary name long enough to
 * overflow every formatter therefore costs six framed lines per skip, not
 * three, and deriving from three would have understated the worst-case burst
 * by exactly 2x -- which is how a "derived" ceiling silently becomes as
 * wrong as a picked one. */
#define UTEST_SKIP_RECORD_FANOUT 6u

/* Worst-case SERIALIZED cost of one record: the message cap plus every
 * wrapper byte klog puts on the wire. Counting only UTEST_RECORD_LINE_MAX
 * would under-count the real burst by roughly a third, which is the error
 * that makes a "derived" number no better than a picked one. Components are
 * read off klog's serial emit path (src/kernel/klog.c:1240-1341). */
/* "[NNNNNNNN.mmm] " -- klog's serial renderer buffers the seconds field's
 * decimal digits in a local `char tmp[8]` (klog.c, same function) with no
 * bound on the digit COUNT it writes there beyond `sizeof(tmp)`; `sec` is a
 * uint32_t, so digit counts beyond 8 (sec >= 100,000,000, ~3.17 years of
 * continuous uptime) write past `tmp` -- a latent, practically-unreachable
 * stack overflow tracked as its own item (02-kernel-core/TODO-04-system-
 * logging.md item: "Bound the timestamp digit loop in klog.c's serial
 * renderer so it cannot write past its fixed stack buffer regardless of
 * uptime seconds"), out of THIS section's scope. What this macro must do is
 * bound the WIRE cost within klog's actual defined behavior, which caps at
 * 8 digits: '[' + 8 digits + '.' + 3 fractional digits + ']' + ' ' = 15. A
 * post-ship review (2026-07-29) found the prior value (12, assuming <= 5
 * digits) undercounts every timestamp past 99,999 seconds (~27.8h uptime),
 * which a long boot-test session can reach. */
#define KLOG_WIRE_TIMESTAMP_MAX 15u
#define KLOG_WIRE_CPUTAG_MAX     9u  /* "[cpu:NN] " on SMP                 */
#define KLOG_WIRE_LEVEL_ANSI_MAX 7u  /* longest level_ansi[] entry         */
#define KLOG_WIRE_LEVEL_PREFIX   7u  /* "[CRIT] " -- every badge is 7      */
#define KLOG_WIRE_ANSI_RESET     4u  /* the reset sequence                 */
#define KLOG_WIRE_TRUNC_MARK     1u  /* '~' appended on truncation         */
#define KLOG_WIRE_CRLF           2u  /* newline reaches the wire as CR LF  */
/* Subsystem tag plus its ": " separator. The tag is the framed
 * "UTEST-<8hex>" (UTEST_FRAME_TAG_MAX), bounded by klog's own field. */
#define KLOG_WIRE_SUBSYSTEM_MAX (KLOG_SUBSYSTEM_MAX + 2u)

/* Every record this macro bounds is one of the two overflow markers
 * u_emit_skip_records() logs -- [UTEST-SKIP-RECORD-BUDGET] and
 * [UTEST-RECORD-OVERFLOW], both `utest_record_log(LOG_ERROR, ...)` -- and
 * both fire AFTER task_cleanup() clears the launcher's color-scope flag
 * (s_utest_color_active = 0 at test_usermode.c:2895, well before
 * u_emit_skip_records() runs at test_usermode.c:3062). The explicit
 * "UTEST-<8hex>" subsystem match in klog's renderer also does not fire for
 * this tag: it requires subsystem[5] to be '\0' or ':' (klog.c:1279-1280),
 * but subsystem[5] here is '-'. So klog's renderer (klog.c:1274-1327) never
 * takes the TEST-color branch for these specific records -- it always takes
 * the plain LOG_ERROR path, where `level_full_line[LOG_ERROR]` is 1
 * (klog.c:276), so the badge's level-ANSI sequence is re-emitted after the
 * message. That path costs: the badge (1x LEVEL_ANSI + LEVEL_PREFIX +
 * reset), a second LEVEL_ANSI re-emission, the message's own reset, and the
 * reset klog appends to EVERY record unconditionally (klog.c:1332-1336) --
 * 2x LEVEL_ANSI, 3x RESET, and no truecolor sequence at all. Modeling the
 * TEST-color branch instead (as an earlier version of this macro did) would
 * both add a sequence these records never carry AND miss the third reset
 * they always do -- the wrong branch, not merely an imprecise one. */
#define UTEST_RECORD_WIRE_MAX                                            \
    (UTEST_RECORD_LINE_MAX + KLOG_WIRE_TIMESTAMP_MAX +                   \
     KLOG_WIRE_CPUTAG_MAX + (2u * KLOG_WIRE_LEVEL_ANSI_MAX) +            \
     KLOG_WIRE_LEVEL_PREFIX + (3u * KLOG_WIRE_ANSI_RESET) +              \
     KLOG_WIRE_SUBSYSTEM_MAX + KLOG_WIRE_TRUNC_MARK + KLOG_WIRE_CRLF)

/* The allowance itself: how many bytes a pathological skip burst may add to
 * the capture. A mebibyte is a rounding error against the multi-MB serial
 * logs this framework already produces, and it is a bound a reader can check
 * against a file size -- unlike a record count, which means nothing without
 * knowing the per-record cost.
 *
 * It is 1 MiB rather than 512 KiB because the floor below is what actually
 * constrains this: with the honest 6x overflow fanout, a 512 KiB allowance
 * divides out to 252 records even BEFORE the refusal reservation below is
 * subtracted -- under the 256 a single binary may legitimately report.
 * Rather than accept a budget that clips an honest binary, the allowance
 * doubles. The _Static_assert is what forced the choice into the open
 * instead of leaving it to arithmetic nobody re-derives. */
#define UTEST_SKIP_BURST_WIRE_MAX (1024u * 1024u)

/* Refusing costs wire too. u_emit_skip_records() emits a
 * [UTEST-SKIP-RECORD-BUDGET] marker on every request it clips, and a request
 * is per BINARY, so a run can pay one marker per task slot. Budgeting only
 * the permitted records left the diagnostics OUTSIDE the allowance the
 * derivation advertises: at the boundary the logical records already consume
 * 1,048,380 of 1,048,576 bytes, so a single marker overruns it. Reserving
 * the refusal traffic FIRST keeps the 1 MiB figure a real bound instead of
 * one that holds only until the budget is actually enforced.
 *
 * TASK_MAX bounds "binaries run" only because task slots are NOT reusable
 * today (test_usermode.c:3288 -- "task_create-failed until scheduler slot
 * reuse ships"); a run cannot dispatch more than TASK_MAX binaries because
 * num_tasks++ never resets. Once the scheduler's reusable-task-slot
 * free-list work ships (tracked in the scheduler enhancement TODO, item
 * "Add reusable slot/free-list logic for dead tasks"), a single run could
 * dispatch more binaries than TASK_MAX and this reservation would silently
 * undercount -- that item is this reservation's consumer too, not just the
 * launcher's stress-loop XREF already on file. */
#define UTEST_SKIP_REFUSAL_WIRE_MAX (TASK_MAX * UTEST_RECORD_WIRE_MAX)

#define UTEST_SKIP_RECORD_BUDGET                                         \
    ((UTEST_SKIP_BURST_WIRE_MAX - UTEST_SKIP_REFUSAL_WIRE_MAX) /         \
     (UTEST_SKIP_RECORD_FANOUT * UTEST_RECORD_WIRE_MAX))

/* The two asserts that pin this derivation live just below
 * UTEST_RECORD_LINE_MAX, which the wire cost is expressed in terms of: a
 * macro body is only expanded where it is USED, so asserting here would
 * evaluate UTEST_RECORD_WIRE_MAX before that cap exists. */

/* Transport cap for EVERY machine-artifact record.
 *
 * Records reach the host through klog, whose ring entry is `message[256]`
 * and which bounds the formatted message to that size. A record formatted
 * into a larger local buffer still gets cut there, silently -- and a cut
 * JSON object is unparseable rather than merely short, while a cut XML
 * element corrupts the assembled document. Formatting against this cap
 * instead means the bounded-append path refuses and the emitter publishes
 * an explicit overflow marker the host fails on, which is a diagnosis
 * rather than a corruption.
 *
 * It bites on real inputs, not theoretical ones: a binary name may be
 * VFS_MAX_NAME (256) bytes, so the JSON prefix plus a long name already
 * exceeds the wire capacity on its own. */
#define UTEST_RECORD_LINE_MAX 256
_Static_assert(UTEST_RECORD_LINE_MAX <= sizeof(((klog_entry_t *)0)->message),
               "UTEST record buffer must fit klog's message field or the "
               "wire copy is silently truncated");

/* --- pins for the derived skip-record budget above ------------------- *
 *
 * This ceiling bounds BYTES, not TIME, and the distinction is deliberate.
 * A ceiling-sized burst is roughly a megabyte of serial traffic, which no
 * supported UART carries inside the host's default 60-second deadline --
 * about 90 seconds at 115200 8N1, far longer at 9600. That is not a
 * contradiction to resolve by shrinking the number: the kernel cannot know
 * the wire rate (the divisor may be firmware-configured and never reported),
 * and a run that actually reaches this ceiling is PATHOLOGICAL by
 * construction. It is the abuse the budget exists to bound, not a run that
 * must be helped to finish; the host deadline failing it is the correct
 * outcome and the reason the deadline exists. Real suites report a handful of
 * skip blocks. The budget stops an unbounded burst from being EMITTED; TIME
 * stays the host's to enforce.
 *
 * The floor is load-bearing, not a sanity check on arithmetic. The syscall
 * side lets ONE binary report TASK_UTEST_REPORT_SKIP_MAX skip blocks, so a
 * run-wide budget below that would clip a single HONEST binary -- turning an
 * aggregate abuse stop into a per-binary truncation, which is the false-green
 * class this path exists to close. Any future retune of the byte allowance or
 * of the wire costs that would breach this fails the BUILD instead of
 * silently shrinking what a legitimate binary may report. */
_Static_assert(UTEST_SKIP_RECORD_BUDGET >= TASK_UTEST_REPORT_SKIP_MAX,
               "run-wide skip budget must cover the per-binary ceiling or a "
               "single legitimate binary's skip blocks get clipped");

/* UTEST_SKIP_RECORD_BUDGET's numerator is an UNSIGNED subtraction
 * (BURST_WIRE_MAX - REFUSAL_WIRE_MAX). If a future retune ever made the
 * reservation exceed the allowance, that subtraction would wrap to
 * approximately UINT32_MAX rather than go negative, and the floor assert
 * above would still pass on the resulting (wrongly huge) budget -- the
 * worst-case-fits assert below DOES catch it, but points at the wrong
 * relationship for whoever has to diagnose the failure. Asserting the
 * subtraction's precondition directly, by name, is what makes the eventual
 * build failure diagnose itself instead of requiring this comment to be
 * re-derived. */
_Static_assert(UTEST_SKIP_REFUSAL_WIRE_MAX < UTEST_SKIP_BURST_WIRE_MAX,
               "the refusal-marker reservation must not consume the whole "
               "serialized-byte allowance or the budget's unsigned "
               "subtraction wraps instead of going negative");

/* The whole point of the derivation: the worst-case burst must actually fit
 * the allowance it was divided out of -- INCLUDING the refusal markers the
 * budget's own enforcement emits, which is the boundary the first version of
 * this assert missed. Integer division guarantees the arithmetic, so the
 * assert exists to catch a future edit that replaces the division with a
 * hand-written number and quietly breaks the relationship. */
_Static_assert((uint64_t)UTEST_SKIP_RECORD_BUDGET *
                       UTEST_SKIP_RECORD_FANOUT * UTEST_RECORD_WIRE_MAX +
                   UTEST_SKIP_REFUSAL_WIRE_MAX <=
                   UTEST_SKIP_BURST_WIRE_MAX,
               "the worst-case skip burst plus its refusal markers must fit "
               "the serialized-byte allowance the budget is derived from");

/* ---- The binary-name bound, derived per formatter ------------------- *
 *
 * A binary name was bounded only by VFS_MAX_NAME (256) while every record
 * is bounded by UTEST_RECORD_LINE_MAX (256), so a long enough name pushed
 * each formatter onto its own overflow fallback: the XML testcase drops
 * the name, the JSON record substitutes "overflow", the TAP point becomes
 * "unrepresentable", and the verdict line loses its `: PASS` / `: FAIL`
 * token off the end -- which is the token the host's recount greps for, so
 * the binary silently left the fail-closed count entirely.
 *
 * The bound below is DERIVED rather than picked: each record kind's fixed
 * cost is summed from that kind's OWN format literals through UTEST_LIT,
 * so editing a format string moves the bound with it instead of leaving a
 * hand-counted number behind. The name's MULTIPLICITY per record matters
 * as much as the fixed cost -- the JSON skip_block carries it twice (as
 * `rec_name` and again as `parent`), so it costs 2N there -- and the
 * answer is the MINIMUM across every kind, not the skip_block's. Checking
 * that assumption is what this block is for: the skip_block leaves 37
 * bytes and the JSON binary record 39, so they are two bytes apart --
 * close enough that one edit to either format string reverses which one
 * binds, and far too close to settle by inspection. The unit test asserts
 * the bound equals the minimum rather than any particular kind's room.
 *
 * Scope: the kinds below are the ones a host consumer PARSES -- the
 * verdict line, the XML testcase, both JSON record kinds, the TAP point
 * and the report line. The `%s: format=%s` diagnostic is deliberately not
 * in the minimum: no consumer parses it, its tail truncating loses no
 * machine-read field, and its second operand is a kernel-owned loader
 * constant rather than part of the record contract -- pulling exec.c's
 * label width into this derivation would couple the bound to an unrelated
 * subsystem for a line that carries no accounting. */

/* Length of a string literal, NUL excluded. */
#define UTEST_LIT(s) ((uint32_t)(sizeof(s) - 1u))
#define UTEST_MAX2(a, b) ((a) > (b) ? (a) : (b))
#define UTEST_MIN2(a, b) ((a) < (b) ? (a) : (b))

/* Decimal widths. Each is pinned to the cap the VALUE carries, not to the
 * width of its C type, and each cap is asserted so a future retune that
 * outgrows its digit count fails the build instead of silently overrunning
 * a record whose bound was derived from it. */
#define UTEST_DIGITS_U32     10u   /* 4294967295                        */
#define UTEST_DIGITS_REPORT   7u   /* TASK_UTEST_REPORT_MAX = 1,000,000 */
_Static_assert(TASK_UTEST_REPORT_MAX <= 9999999u,
               "report counters must stay within the digit width the "
               "record bound reserves for them");
_Static_assert(TASK_UTEST_REPORT_SKIP_MAX <= 9999999u,
               "skip-block counts must stay within the reserved digit width");
_Static_assert(UTEST_SKIP_RECORD_BUDGET <= 9999999u,
               "the skip-record budget bounds the skip_index field's digits");

/* Elapsed milliseconds are the one record field with no natural cap:
 * uptime is a uint64 and `end_ms - start_ms` inherits that, so its digit
 * count could not be proven -- only assumed. It is CLAMPED at emit
 * (u_clamp_time_ms) so the reservation below is a fact about the code
 * rather than a bet on how long a run takes. */
#define UTEST_TIME_MS_MAX    0xFFFFFFFFull
#define UTEST_DIGITS_TIME    UTEST_DIGITS_U32
/* u_format_seconds renders `<secs>.<3 digits>`; secs = ms/1000, so it can
 * never be wider than the clamped millisecond field itself. */
#define UTEST_SECONDS_MAX    (UTEST_DIGITS_TIME + UTEST_LIT(".000"))

/* The reason string, derived from the launcher's own reason literals --
 * every site that writes `reason` is enumerated here, so the widest one is
 * a fact rather than an estimate. */
#define UTEST_REASON_BUF     96u
#define UTEST_REASON_TIMEOUT (UTEST_LIT("timeout after ") + UTEST_DIGITS_U32 \
                              + UTEST_LIT("ms"))
#define UTEST_REASON_EXIT    (UTEST_LIT("exit=-") + UTEST_DIGITS_U32)
#define UTEST_REASON_LEAK    (UTEST_DIGITS_U32 + UTEST_LIT(" handle(s) leaked"))
#define UTEST_REASON_ISOLATE UTEST_LIT("isolation failed")
#define UTEST_REASON_INVALID UTEST_LIT("invalid test report")
#define UTEST_REASON_REFUSAL UTEST_LIT("name refused: charset")
#define UTEST_REASON_MAX                                                   \
    UTEST_MAX2(UTEST_MAX2(UTEST_MAX2(UTEST_REASON_TIMEOUT,                 \
                                     UTEST_REASON_EXIT),                   \
                          UTEST_MAX2(UTEST_REASON_LEAK,                    \
                                     UTEST_REASON_ISOLATE)),               \
               UTEST_MAX2(UTEST_REASON_INVALID, UTEST_REASON_REFUSAL))
_Static_assert(UTEST_REASON_MAX < UTEST_REASON_BUF,
               "the widest reason the launcher composes must fit the buffer "
               "u_run_one writes it into");

/* The longest `classname` / `type` label either testcase emitter can put
 * on a record: u_type_label's widest return, itself wider than the
 * "skip-block" classname override. */
#define UTEST_LABEL_MAX      UTEST_LIT("correctness")

/* Per-kind fixed cost -- everything on the record that is NOT the name. */
#define UTEST_FIXED_VERDICT                                                \
    (UTEST_LIT(": FAIL (") + UTEST_DIGITS_U32 +                            \
     UTEST_LIT(" handle(s) leaked -- escalated from PASS)"))
#define UTEST_FIXED_XML                                                    \
    (UTEST_LIT("[UTEST-XML] <testcase name=\"") +                          \
     UTEST_LIT("\" classname=\"") + UTEST_LABEL_MAX +                      \
     UTEST_LIT("\" time=\"") + UTEST_SECONDS_MAX + UTEST_LIT("\">") +      \
     UTEST_LIT("<failure message=\"") + UTEST_REASON_MAX +                 \
     UTEST_LIT("\"/>") + UTEST_LIT("</testcase>"))
#define UTEST_FIXED_JSON_BINARY                                            \
    (UTEST_LIT("[UTEST-JSON] {\"record_kind\":\"binary\",\"name\":\"") +    \
     UTEST_LIT("\",\"type\":\"") + UTEST_LABEL_MAX +                       \
     UTEST_LIT("\",\"status\":\"") + UTEST_LIT("PASS") +                   \
     UTEST_LIT("\",\"time_ms\":") + UTEST_DIGITS_TIME +                    \
     UTEST_LIT(",\"reason\":\"") + UTEST_REASON_MAX + UTEST_LIT("\"") +    \
     UTEST_LIT(",\"asserts_passed\":") + UTEST_DIGITS_REPORT +             \
     UTEST_LIT(",\"asserts_failed\":") + UTEST_DIGITS_REPORT +             \
     UTEST_LIT(",\"skip_blocks\":") + UTEST_DIGITS_REPORT + UTEST_LIT("}"))
/* The synthetic skip-record name both kinds below carry in place of the
 * bare binary name: `<binary>::skipped-block-<k>`. */
#define UTEST_FIXED_SKIP_SUFFIX                                            \
    (UTEST_LIT("::skipped-block-") + UTEST_DIGITS_REPORT)
#define UTEST_FIXED_JSON_SKIP                                              \
    (UTEST_LIT("[UTEST-JSON] {\"record_kind\":\"skip_block\",\"name\":\"")  \
     + UTEST_FIXED_SKIP_SUFFIX + UTEST_LIT("\",\"parent\":\"") +           \
     UTEST_LIT("\",\"skip_index\":") + UTEST_DIGITS_REPORT +               \
     UTEST_LIT(",\"status\":\"SKIP\",\"reason\":\"sub-test block skipped "  \
               "(reason on serial log)\"}"))
#define UTEST_FIXED_TAP                                                    \
    (UTEST_LIT("not ok ") + UTEST_DIGITS_U32 + UTEST_LIT(" - ") +          \
     UTEST_FIXED_SKIP_SUFFIX + UTEST_LIT(" # ") +                          \
     UTEST_MAX2(UTEST_LIT("SKIP reported by binary"), UTEST_REASON_MAX))
#define UTEST_FIXED_REPORT                                                 \
    (UTEST_LIT("[UTEST-REPORT] ") + UTEST_LIT(" asserts_passed=") +        \
     UTEST_DIGITS_REPORT + UTEST_LIT(" asserts_failed=") +                 \
     UTEST_DIGITS_REPORT + UTEST_LIT(" skip_blocks=") +                    \
     UTEST_DIGITS_REPORT + UTEST_LIT(" state=") + UTEST_LIT("INVALID"))

/* Room a kind leaves for the name: the appenders refuse at
 * `pos + 1 >= cap` (u_append), so a record holds UTEST_RECORD_LINE_MAX - 1
 * content bytes. Divided by the multiplicity, because a kind that carries
 * the name twice pays for it twice. */
#define UTEST_NAME_ROOM(fixed, mult)                                       \
    (((UTEST_RECORD_LINE_MAX - 1u) - (fixed)) / (mult))

#define UTEST_MAX_BINARY_NAME                                              \
    UTEST_MIN2(                                                            \
        UTEST_MIN2(UTEST_MIN2(UTEST_NAME_ROOM(UTEST_FIXED_VERDICT, 1u),    \
                              UTEST_NAME_ROOM(UTEST_FIXED_XML, 1u)),       \
                   UTEST_MIN2(UTEST_NAME_ROOM(UTEST_FIXED_JSON_BINARY, 1u),\
                              UTEST_NAME_ROOM(UTEST_FIXED_JSON_SKIP, 2u))),\
        UTEST_MIN2(UTEST_NAME_ROOM(UTEST_FIXED_TAP, 1u),                   \
                   UTEST_NAME_ROOM(UTEST_FIXED_REPORT, 1u)))

/* Every kind must still fit at the derived bound. The division above makes
 * that arithmetically true; the assert exists to catch a future edit that
 * replaces the minimum with a literal and quietly breaks the relationship
 * -- the same failure mode the skip-budget asserts above guard against. */
_Static_assert(UTEST_FIXED_JSON_SKIP + 2u * UTEST_MAX_BINARY_NAME <=
                   UTEST_RECORD_LINE_MAX - 1u &&
               UTEST_FIXED_JSON_BINARY + UTEST_MAX_BINARY_NAME <=
                   UTEST_RECORD_LINE_MAX - 1u &&
               UTEST_FIXED_XML + UTEST_MAX_BINARY_NAME <=
                   UTEST_RECORD_LINE_MAX - 1u &&
               UTEST_FIXED_TAP + UTEST_MAX_BINARY_NAME <=
                   UTEST_RECORD_LINE_MAX - 1u &&
               UTEST_FIXED_VERDICT + UTEST_MAX_BINARY_NAME <=
                   UTEST_RECORD_LINE_MAX - 1u &&
               UTEST_FIXED_REPORT + UTEST_MAX_BINARY_NAME <=
                   UTEST_RECORD_LINE_MAX - 1u,
               "a name at the derived bound must fit EVERY record kind "
               "without any formatter reaching its overflow fallback");

/* Compatibility ratchet. The derivation is honest in one direction on
 * its own -- a name past the bound is refused rather than truncated --
 * but nothing stops a future format string from GROWING its fixed cost
 * and silently shrinking this bound, which turns filenames that ran
 * yesterday into counted failures today. That is a compatibility break
 * and belongs in a review, not in a build that stays green.
 *
 * So the floor is today's derived value, not a comfortable margin above
 * the longest name the repository currently builds (test_harness_smoke.exe,
 * 22 bytes). Shrinking the bound now requires deliberately lowering this
 * number, which is exactly the decision that should be explicit. It lives
 * here alone rather than being mirrored into the unit test, so there is
 * one ratchet to move rather than two that can disagree. */
#define UTEST_NAME_BOUND_RATCHET 37u
_Static_assert(UTEST_MAX_BINARY_NAME >= UTEST_NAME_BOUND_RATCHET,
               "a record format grew and shrank the derived name bound: "
               "names that were accepted before would now be refused, so "
               "lower UTEST_NAME_BOUND_RATCHET deliberately or don't");

/* Hex digits of the refused-name digest. The digest exists to correlate
 * the SAME bad name across runs; within a run, uniqueness comes from the
 * ordinal below rather than from hash strength, because no digest width
 * can guarantee distinctness and this identity has to. */
#define UTEST_NAME_DIGEST_HEX     8u
#define UTEST_REFUSAL_PREFIX_MIN  4u
/* The identity is assembled at runtime from the ordinal's ACTUAL digit
 * count, so this reserves the worst case (a 10-digit ordinal) and asserts
 * that a readable prefix still survives it. */
#define UTEST_REFUSAL_ID_FIXED                                             \
    (UTEST_LIT("refused_") + UTEST_DIGITS_U32 + UTEST_LIT("_") +           \
     UTEST_LIT("_") + UTEST_NAME_DIGEST_HEX + UTEST_LIT(".exe"))
_Static_assert(UTEST_REFUSAL_ID_FIXED + UTEST_REFUSAL_PREFIX_MIN <=
                   UTEST_MAX_BINARY_NAME,
               "a refusal identity must fit the bound it exists to prove, "
               "with room left for a prefix an operator can read");

/* The accepted charset. Restricting it is what makes the bound above
 * PROVABLE rather than probabilistic: u_xml_escape and u_json_escape are
 * the identity on every byte in this set, so N accepted bytes still cost
 * N bytes on the wire. Without it a single `"` expands to `&quot;` and a
 * name six times shorter than the bound could still overflow a record.
 * The set also subsumes the path-shape checks this validator used to make
 * one at a time -- `\`, `/`, `:`, spaces, `#` and every control byte are
 * outside it -- so `#` can no longer inject a TAP directive either. */
static int u_name_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

static utest_name_verdict_t u_classify_name_span(const char *name,
                                                 uint32_t span_len)
{
    uint32_t len = 0;
    const char *p;

    if (!name)
        return UTEST_NAME_NOT_TEST_SHAPED;
    while (name[len])
        len++;
    /* The span check comes FIRST, ahead of the shape check, and the
     * order is the whole point. An embedded NUL means the C string the
     * validator, the filter and the digest all see is SHORTER than the
     * bytes the caller actually holds. Classifying shape first would
     * read that truncation as the real name -- and for `test_bad\0.exe`
     * the truncation is `test_bad`, which is NOT test-shaped, so the
     * entry would be ignored as a stray file rather than counted as the
     * malformed request it is. A NUL anywhere in the span is a refusal
     * regardless of what the visible prefix happens to look like.
     *
     * Only the manifest can produce a mismatch here: dirent names are C
     * strings, so the glob path always passes span_len == len. */
    if (len != span_len)
        return UTEST_NAME_REFUSE_NUL;
    if (!name[0])
        return UTEST_NAME_NOT_TEST_SHAPED;
    /* Shape next: a file that is not test_*.exe is not addressed to this
     * framework, and classifying it as a refusal would turn every stray
     * file in C:\ into a suite failure. Everything below this line is a
     * binary somebody meant to run. */
    if (!u_is_test_binary(name))
        return UTEST_NAME_NOT_TEST_SHAPED;
    if (len > UTEST_MAX_BINARY_NAME)
        return UTEST_NAME_REFUSE_LENGTH;
    for (p = name; *p; p++) {
        if (!u_name_char_ok(*p))
            return UTEST_NAME_REFUSE_CHARSET;
        /* `.` and `..` cannot be test-shaped on their own, but
         * `test_.._foo.exe` is, and both dots are inside the charset --
         * so traversal stays an explicit check. */
        if (p[0] == '.' && p[1] == '.')
            return UTEST_NAME_REFUSE_PATH;
    }
    return UTEST_NAME_ACCEPT;
}

static utest_name_verdict_t u_classify_name(const char *name)
{
    uint32_t len = 0;

    if (!name)
        return UTEST_NAME_NOT_TEST_SHAPED;
    while (name[len])
        len++;
    return u_classify_name_span(name, len);
}

/* Short human label for a refusal, used as the record's `reason`. Every
 * string here is inside UTEST_REASON_REFUSAL's derived width. */
static const char *u_refusal_reason(utest_name_verdict_t v)
{
    switch (v) {
    case UTEST_NAME_REFUSE_LENGTH:  return "name refused: length";
    case UTEST_NAME_REFUSE_CHARSET: return "name refused: charset";
    case UTEST_NAME_REFUSE_PATH:    return "name refused: path";
    case UTEST_NAME_REFUSE_NUL:     return "name refused: nul";
    default:                        return "name refused";
    }
}

/* FNV-1a over the EXACT byte span, not over a C string: the whole point
 * of the digest is to distinguish two names the sanitized prefix renders
 * identically, and a name refused for an embedded NUL is precisely the
 * case where the C string stops early. Constants are the published FNV-1a
 * 32-bit basis and prime. */
#define UTEST_FNV1A_BASIS 2166136261u
#define UTEST_FNV1A_PRIME 16777619u
static uint32_t u_name_digest(const char *p, uint32_t len)
{
    uint32_t h = UTEST_FNV1A_BASIS;
    uint32_t i;

    for (i = 0; i < len; i++) {
        h ^= (uint32_t)(uint8_t)p[i];
        h *= UTEST_FNV1A_PRIME;
    }
    return h;
}

/* Build the identity a refused binary appears under:
 * `refused_<ordinal>_<sanitized prefix>_<8 hex>.exe`.
 *
 * Three properties, in the order they matter. It never echoes the raw
 * name, so a hostile name cannot reach the wire through the record that
 * reports it. Its ordinal is unique WITHIN the run by construction, which
 * is stronger than any digest width could be -- a 32-bit FNV-1a is not
 * collision-resistant, and an operator who cannot tell two refusals apart
 * cannot act on either. The digest is what still correlates the SAME bad
 * name across runs, which the ordinal alone cannot do. It ends in `.exe`
 * because the host's fail-closed recount greps for a `.exe` name followed
 * by a verdict token; an identity that missed it would be reported by the
 * launcher and dropped by the host.
 *
 * The prefix takes whatever the ordinal leaves, so an early refusal gets
 * a readable prefix while the static assert above still holds against a
 * worst-case 10-digit ordinal.
 *
 * `digest` is passed in rather than computed here: it must cover the
 * entry's exact byte span, which only the ENUMERATOR still knows -- an
 * entry refused for an embedded NUL has a C string shorter than the bytes
 * it came from, so hashing `raw` at this point would hash the truncation
 * instead of the name. `raw` is used only for the readable prefix, where
 * stopping at the NUL is harmless. Returns 1 on success, 0 if `cap`
 * cannot hold a bound-conforming identity. */
static int u_build_refusal_id(char *dst, uint32_t cap, uint32_t ordinal,
                              const char *raw, uint32_t digest)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t pos = 0;
    uint32_t tail = UTEST_LIT("_") + UTEST_NAME_DIGEST_HEX + UTEST_LIT(".exe");
    uint32_t budget, i;

    if (!dst || cap < UTEST_MAX_BINARY_NAME + 1u)
        return 0;
    dst[0] = '\0';
    if (!u_append(dst, &pos, cap, "refused_"))
        return 0;
    if (!u_append_uint(dst, &pos, cap, (uint64_t)ordinal))
        return 0;
    if (!u_append(dst, &pos, cap, "_"))
        return 0;
    /* Room left for the prefix once the fixed tail is reserved. */
    if (pos + tail >= UTEST_MAX_BINARY_NAME)
        return 0;
    budget = UTEST_MAX_BINARY_NAME - pos - tail;
    for (i = 0; i < budget && raw && raw[i]; i++)
        dst[pos++] = u_name_char_ok(raw[i]) ? raw[i] : '_';
    dst[pos] = '\0';
    if (!u_append(dst, &pos, cap, "_"))
        return 0;
    for (i = 0; i < UTEST_NAME_DIGEST_HEX; i++) {
        uint32_t shift = (UTEST_NAME_DIGEST_HEX - 1u - i) * 4u;
        if (pos + 1u >= cap)
            return 0;
        dst[pos++] = hex[(digest >> shift) & 0xFu];
    }
    dst[pos] = '\0';
    return u_append(dst, &pos, cap, ".exe");
}

/* Clamp elapsed milliseconds to the width the record bound reserves for
 * them. Uptime is a uint64 with no natural cap, so without this the
 * `time_ms` field's digit count is an assumption rather than a fact --
 * and a bound derived from an assumption is the picked number the whole
 * derivation exists to avoid. A run that reaches 49 days of uptime
 * reports a saturated duration; every other field stays exact. */
static uint64_t u_clamp_time_ms(uint64_t ms)
{
    return ms > UTEST_TIME_MS_MAX ? (uint64_t)UTEST_TIME_MS_MAX : ms;
}

/* Machine-artifact records bypass the per-subsystem rate limiter.
 *
 * klog drops messages past 100 per second per subsystem, and under
 * `xml=1 json=1` the UTEST subsystem emits several records per binary --
 * so a fast enough suite can push the mandatory tail (run_report, summary,
 * run_meta, and the XML closer) past the budget. The host then refuses a
 * perfectly valid run because its terminator never arrived, and the drop
 * gets worse on faster hardware. A rate limiter silently deleting the
 * records an artifact is assembled from is the same false-green class this
 * whole path exists to close. The volume is bounded by the binary count,
 * not by anything unbounded, so the limiter has nothing to protect here.
 *
 * Human-readable UTEST progress lines deliberately keep the rate limit. */

/* ---- Non-forgeable record framing ---------------------------------- *
 *
 * Ring-3 stdout and every launcher record share one serial stream:
 * `sys_write(fd=1, ...)` copies caller-controlled bytes straight to
 * `serial_putchar` (src/kernel/sched/syscall.c) with no annotation of any
 * kind. A test binary could therefore print a byte-identical
 * `UTEST: === 5 passed, 0 failed, 0 skipped of 5 total ===`, and the host's
 * boot-completion poll would accept it, stop QEMU, and report success
 * without ever seeing the launcher's real verdict. The same shared
 * namespace let a binary inject `[UTEST-XML]` / `[UTEST-JSON]` records
 * straight into the assembled artifact, where no budget or counter
 * accounted for them.
 *
 * The frame is a per-boot nonce carried in the klog SUBSYSTEM field, so a
 * launcher record renders as `[INFO] UTEST-1a2b3c4d: <payload>` where a
 * forgery can only manage `[INFO] UTEST: <payload>`. It rides the subsystem
 * rather than the message because every record is bounded by klog's
 * 256-byte message field and the JSON summary already measured 242 of 255
 * usable bytes: framing inside the message would be paid for by every
 * emitter and would push the tightest records over the wire. The subsystem
 * is a `const char *`, so the tag costs the record budget nothing.
 *
 * Ring 3 cannot observe the value. It is derived in kernel context, no
 * syscall reads it back, and it leaves the kernel on SERIAL ONLY: the live
 * disk log is openable from ring 3, so the frame tag is aliased back to the
 * plain "UTEST" on that sink (klog_set_disk_alias) and the announcement
 * body does not restate the nonce. Without both, a binary could read
 * X:\Logs\Serial_*.log -- which klog appends and flushes synchronously,
 * before the first test task runs -- and forge a correctly framed
 * terminator.
 *
 * The nonce is generated ONCE PER BOOT and never rewritten, for two
 * reasons. test_usermode_run() is documented safe to call repeatedly, and
 * klog's ring entries retain the subsystem as a POINTER -- rewriting the
 * buffer for a second run would silently re-attribute every retained
 * first-run entry, including the crash-region evidence the next boot reads
 * back. Each run instead gets its own ordinal and its own record count. */
#define UTEST_FRAME_NONCE_HEX 8u
#define UTEST_FRAME_TAG_MAX   (sizeof("UTEST-") + UTEST_FRAME_NONCE_HEX)
_Static_assert(UTEST_FRAME_TAG_MAX <= KLOG_SUBSYSTEM_MAX,
               "the framed subsystem tag must fit klog's serialized "
               "subsystem field or crash-region and debug-console evidence "
               "is truncated for every launcher record");

/* Written exactly once per boot, then read for the life of the boot: klog
 * keeps the pointer, so this storage must outlive every entry logged under
 * it. `s_frame_ready` publishes the buffer with release ordering. */
static char     s_frame_tag[UTEST_FRAME_TAG_MAX];
static uint32_t s_frame_nonce;    /* 0 = not generated yet (the sentinel) */
static uint32_t s_frame_ready;    /* 1 = s_frame_tag is filled and stable */
static uint32_t s_frame_records;  /* framed records emitted in THIS run */
static uint32_t s_frame_run;      /* run ordinal, 1-based */

/* Format "UTEST-<8 lowercase hex>" into dst. Pure: touches no globals and
 * logs nothing, so a unit test can exercise it directly. Returns 1 on
 * success, 0 if the buffer cannot hold the tag and its NUL. */
static int u_frame_tag_format(char *dst, uint32_t cap, uint32_t nonce)
{
    static const char hex[] = "0123456789abcdef";
    const char       *pre   = "UTEST-";
    uint32_t          i     = 0;
    uint32_t          k;

    if (!dst || cap < UTEST_FRAME_TAG_MAX)
        return 0;
    while (*pre)
        dst[i++] = *pre++;
    for (k = 0; k < UTEST_FRAME_NONCE_HEX; k++)
        dst[i++] = hex[(nonce >> (28u - 4u * k)) & 0xFu];
    dst[i] = '\0';
    return 1;
}

/* The subsystem every framed record is logged under. Falls back to the
 * unframed literal before the frame is published, which is unreachable by
 * construction (the launcher publishes before it emits anything) and fails
 * CLOSED if it ever happens: the record is still counted, so the host's
 * count reconciliation refuses the run rather than accepting a stream with
 * a silently unframed record in it. */
static const char *u_frame_tag(void)
{
    if (__atomic_load_n(&s_frame_ready, __ATOMIC_ACQUIRE))
        return s_frame_tag;
    return "UTEST";
}

/* Machine-artifact records bypass the per-subsystem rate limiter (see
 * above) and carry the frame. Every framed record is counted so the run's
 * terminator can state how many the host must have seen. */
#define utest_record_log(level, ...)                                   \
    do {                                                               \
        __atomic_fetch_add(&s_frame_records, 1u, __ATOMIC_RELAXED);    \
        klog_unrated((level), u_frame_tag(), __VA_ARGS__);             \
    } while (0)

/* Derive this boot's nonce. csprng is seeded in Phase 1, long before the
 * launcher runs in Phase 3; the TSC mix is a defence-in-depth salt, not a
 * substitute, since a monotonic counter is approximable from outside. 0 is
 * the "not generated" sentinel, so it is folded to 1. */
static uint32_t u_frame_nonce_fold(uint64_t mixed)
{
    uint32_t nonce = (uint32_t)(mixed ^ (mixed >> 32));

    return nonce ? nonce : 1u;
}

static uint32_t u_frame_nonce_new(void)
{
    /* The nonce authenticates records, so an uncredited entropy source is
     * worth saying out loud rather than assuming away. It is NOT fatal: the
     * threat model is a binary printing a launcher pattern by accident as
     * much as a hostile one, and a caller that cannot observe the value
     * cannot exploit a weak one. The TSC term is a salt, not a backstop --
     * rdtsc_ns() returns 0 outright when the active clocksource is not the
     * TSC. */
    if (!csprng_is_seeded())
        klog(LOG_WARN, "UTEST",
             "record framing nonce drawn before the CSPRNG was seeded -- "
             "the frame is still unguessable from ring 3 but is not "
             "cryptographically random");

    return u_frame_nonce_fold(csprng_u64() ^ rdtsc_ns());
}

/* Open a framed run: publish the per-boot tag on first use, take the next
 * run ordinal, reset the per-run record count, and announce the frame.
 *
 * The announcement is itself framed and is the FIRST framed record of the
 * run, so the host learns the nonce from a line only the kernel can have
 * produced at a point where no ring-3 code of this run has executed yet.
 * Concurrent invocation is not a supported shape (the launcher is a
 * boot-phase singleton driven from boot_tests_run) and fails closed if
 * attempted: the loser's records are counted but unframed, so the host's
 * reconciliation refuses the run. */
static void u_frame_begin(void)
{
    uint32_t expected = 0;

    if (!__atomic_load_n(&s_frame_ready, __ATOMIC_ACQUIRE)) {
        uint32_t nonce = u_frame_nonce_new();

        if (__atomic_compare_exchange_n(&s_frame_nonce, &expected, nonce, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            int framed = u_frame_tag_format(s_frame_tag,
                                            (uint32_t)sizeof(s_frame_tag),
                                            nonce);
            /* Keep the nonce off the one sink ring 3 can read. The live
             * disk log (X:\Logs\Serial_*.log) is openable through
             * SYS_OPENFILE, and klog appends+flushes each message to it
             * synchronously -- so without this the announcement is on disk
             * BEFORE the first test task runs, and a binary could read the
             * value back and emit a correctly framed terminator. Serial
             * keeps the frame; disk keeps the records under the plain tag,
             * so post-mortem diagnosis is unaffected. */
            if (framed)
                framed = klog_set_disk_alias(s_frame_tag, "UTEST");

            /* Framing is published ONLY when both halves took. Either
             * refusal leaves the authenticating tag reaching the
             * ring-3-readable disk log, and publishing anyway would assert a
             * property the run does not have -- so this fails CLOSED: the
             * records stay unframed, every host gate refuses them, and the
             * run reports the refusal instead of a false green. */
            if (framed) {
                /* The per-subsystem verbosity ceiling is a whole-string
                 * match, so the boot's klog_set_level("UTEST", LOG_DEBUG)
                 * does NOT cover "UTEST-<nonce>". Without this the records
                 * counted below could be dropped by the level filter before
                 * reaching serial -- klog_unrated bypasses only the RATE
                 * limiter -- and the host's count reconciliation would then
                 * refuse a complete run. */
                klog_set_level(s_frame_tag, LOG_DEBUG);
                __atomic_store_n(&s_frame_ready, 1u, __ATOMIC_RELEASE);
            } else {
                klog(LOG_ERROR, "UTEST",
                     "record framing unavailable -- launcher records will be "
                     "refused by the host (tag format or disk alias refused)");
            }
        }
    }

    __atomic_store_n(&s_frame_records, 0u, __ATOMIC_RELAXED);
    /* Atomic like its siblings: it is a run-identity field the host
     * reconciles against, and the public header documents
     * test_usermode_run() as safe to call repeatedly. */
    (void)__atomic_add_fetch(&s_frame_run, 1u, __ATOMIC_RELAXED);

    /* The body deliberately does NOT restate the nonce. The TAG carries it,
     * and a body copy would survive the disk alias below -- putting the
     * value back into the one sink ring 3 can read. Nothing is lost: a
     * tag-vs-body cross-check only ever caught producer drift, which the
     * record-count reconciliation now covers, and it was never a barrier to
     * a forger who can write both fields. */
    utest_record_log(LOG_INFO, "[UTEST-FRAME] v=1 run=%u",
                     (uint64_t)__atomic_load_n(&s_frame_run, __ATOMIC_RELAXED));
}

/* Close a framed run. `records=` counts every framed record emitted before
 * this line, so a host that saw the whole stream counts exactly
 * `records + 1` framed lines for the run. A binary that prints a plausible
 * summary and then hangs cannot produce this line, and a stream cut short
 * cannot reconcile -- which is what makes the terminator, not the summary,
 * the run's completion signal. */
static void u_frame_end(void)
{
    uint32_t n = __atomic_load_n(&s_frame_records, __ATOMIC_RELAXED);

    utest_record_log(LOG_INFO, "[UTEST-FRAME-END] run=%u records=%u",
                     (uint64_t)__atomic_load_n(&s_frame_run, __ATOMIC_RELAXED),
                     (uint64_t)n);
}

struct u_report {
    uint32_t asserts_passed;
    uint32_t asserts_failed;
    uint32_t skip_blocks;
    uint32_t state;          /* TASK_UTEST_REPORT_* */
};

/* Run-wide totals over every VALID report, plus the census of how the
 * submissions themselves landed. `invalid` and `unreported` are carried
 * so a consumer can tell "nothing was skipped" from "nobody said". */
struct u_report_totals {
    uint32_t asserts_passed;
    uint32_t asserts_failed;
    uint32_t skip_blocks;
    uint32_t skip_records;   /* synthetic skip testcases/TAP points emitted */
    uint32_t reported;       /* binaries with an accepted report            */
    uint32_t invalid;        /* binaries whose report contradicted itself   */
    uint32_t unreported;     /* binaries that never submitted one           */
};

static void u_report_snapshot(uint32_t child_pid, struct u_report *out)
{
    struct task *child = task_get_by_pid(child_pid);

    out->asserts_passed = 0;
    out->asserts_failed = 0;
    out->skip_blocks    = 0;
    out->state          = TASK_UTEST_REPORT_NONE;
    if (!child) {
        /* The TCB should still be live here -- the pid came from a
         * successful task_create and task_cleanup has not run yet -- so
         * its absence is an anomaly, not a legacy non-reporting binary.
         * Fail CLOSED and say so: leaving state NONE would file a
         * vanished task under "never reported" with no trace at all, on
         * the one path whose entire purpose is fail-closed reporting. */
        klog(LOG_ERROR, "UTEST",
             "report snapshot: pid %u vanished before reap -- treating as INVALID",
             (uint64_t)child_pid);
        out->state = TASK_UTEST_REPORT_INVALID;
        return;
    }
    /* Acquire the state first: the submitter publishes it with a release
     * compare-exchange AFTER writing the counts, so acquiring VALID here
     * guarantees the counts below are the ones that go with it. */
    out->state = __atomic_load_n(&child->utest_report.state,
                                 __ATOMIC_ACQUIRE);
    /* Read the counts ONLY behind an acquired VALID. On any other state a
     * submitter may still be mid-write (the three plain stores between
     * the claim and the publish), so reading them would be a formal race
     * for values that are discarded anyway. */
    if (out->state == TASK_UTEST_REPORT_VALID) {
        out->asserts_passed = child->utest_report.asserts_passed;
        out->asserts_failed = child->utest_report.asserts_failed;
        out->skip_blocks    = child->utest_report.skip_blocks;
    }
    /* A record still CLAIMED at reap means the submitter died between
     * taking the slot and publishing. Its counts were never completed, so
     * it is INVALID -- fail-closed, exactly like a contradiction. */
    if (out->state == TASK_UTEST_REPORT_CLAIMED)
        out->state = TASK_UTEST_REPORT_INVALID;
}

/* Reconcile an accepted report against the outcome the kernel actually
 * observed, and return the state the launcher should act on.
 *
 * The kernel-side syscall could not do this: at submission time the
 * binary has not exited yet, so the exit status does not exist. Here it
 * does, and a report that disagrees with it is the precise shape this
 * whole section exists to catch -- a binary claiming "all assertions
 * passed" while exiting non-zero, or claiming a whole-binary skip while
 * reporting assertions it ran.
 *
 * Deliberately NOT an exact `exit_status == failed` equality: test.h
 * documents `return g_fail;` as the convention, not a requirement, so a
 * binary that normalizes its exit code to 1 is well-formed. The
 * equivalence below catches every false-green direction without
 * outlawing that.
 *
 * Pure function of its arguments so the unit tests can drive the whole
 * matrix without spawning anything. */
static uint32_t u_report_reconcile(uint32_t state, uint32_t failed,
                                   int32_t exit_status, int timed_out)
{
    /* A half-written record (submitter died between claiming the slot and
     * publishing) is a contradiction like any other, and is mapped here
     * as well as at snapshot time so the two are not order-dependent:
     * letting CLAIMED fall through would make it neither VALID nor
     * INVALID, and the caller would count it as "never reported". */
    if (state == TASK_UTEST_REPORT_CLAIMED)
        return TASK_UTEST_REPORT_INVALID;
    if (state != TASK_UTEST_REPORT_VALID)
        return state;   /* NONE stays legacy; INVALID is already sticky */

    /* Timeout: exit_status is the launcher's own synthetic marker
     * (u_wait_with_timeout overwrote it), so it can never agree with any
     * report and reconciliation would be meaningless. The binary reached
     * UTEST_END and then hung -- its counts are real, and the timeout
     * verdict dominates on its own. */
    if (timed_out)
        return TASK_UTEST_REPORT_VALID;

    /* Exit 77 means "the whole binary was skippable", which contradicts
     * having reached UTEST_END with counters to submit. */
    if (exit_status == UTEST_EXIT_SKIP)
        return TASK_UTEST_REPORT_INVALID;

    if ((exit_status == 0) != (failed == 0))
        return TASK_UTEST_REPORT_INVALID;

    return TASK_UTEST_REPORT_VALID;
}

/* Apply an INVALID report to the binary's verdict and the run counters.
 * Returns the verdict after escalation.
 *
 * A PASS or a SKIP built on a self-contradicting report becomes a FAIL:
 * silently dropping the counts would leave the run green on a binary that
 * just proved its own reporting untrustworthy, which is the false-green
 * class this section exists to close. An already-FAIL is left alone --
 * its exit-code or timeout reason is more specific than "bad report".
 *
 * Split out of u_run_one because it is the load-bearing half of the
 * fail-closed path and u_run_one itself cannot be unit-tested (it spawns
 * a live child, which the test policy forbids). `counters` is the
 * [pass, fail, skip] triple; the decrement side is only ever reached from
 * a branch that incremented the same slot earlier in the same call, so it
 * cannot underflow. */
static int u_report_apply_invalid(int verdict, uint32_t *counters)
{
    if (verdict == 1)
        return 1;               /* already failed, for a better reason */
    if (verdict == 0)
        counters[0]--;          /* undo PASS */
    else
        counters[2]--;          /* undo SKIP */
    counters[1]++;
    return 1;
}

/* ---- emit helpers ---------------------------------------------- */

static void u_emit_xml_suite_open(void)
{
    if (!s_xml_mode) return;
    /* Attributes (tests/failures/skipped/time) are re-filled by
     * scripts/test.sh's post-processor because we don't yet know the
     * final counts; emit a placeholder header so the XML envelope is
     * recognizable even without post-processing. */
    utest_record_log(LOG_INFO,
         "[UTEST-XML] <testsuite name=\"impossible-os-usermode\" tests=\"0\" "
         "failures=\"0\" skipped=\"0\" errors=\"0\" time=\"0\">");
}

/* Build the [UTEST-XML-SUMMARY] body. Returns 1 on success, 0 if the
 * line would not fit.
 *
 * Split out of the emitter for two reasons. Every append is CHECKED here
 * -- the previous emitter ignored all of them, so a line that outgrew its
 * buffer was published silently truncated, and the host post-processor
 * would read a truncated `tests=` as a valid smaller number. And a
 * formatter that writes into a caller-supplied buffer can be driven to
 * overflow by a unit test with a deliberately small cap, which an emitter
 * that klogs directly cannot.
 *
 * `tests` and `skipped` count RECORDS (binary testcases plus the
 * synthetic per-skip-block ones), so the attributes the host patches into
 * <testsuite> match the number of <testcase>/<skipped> elements actually
 * emitted.
 *
 * `aborted` / `not_run` describe the RUN, not any testcase: a suite the
 * smoke gate cut short describes only the binaries that executed, so
 * without these two fields the artifact reads as a smaller COMPLETE run.
 * They are appended AFTER `time=` deliberately -- every host parser in
 * scripts/test.sh step 6 is a greedy `.*<key>=(...)` sed plus an
 * end-unanchored validating grep, so trailing fields extend the line
 * without disturbing any existing extraction. */
static int u_format_xml_summary(char *dst, uint32_t cap, uint32_t tests,
                                uint32_t failures, uint32_t skipped,
                                uint64_t total_ms, int aborted,
                                uint32_t not_run)
{
    uint32_t pos = 0;
    char time_buf[24];

    dst[0] = '\0';
    if (!u_append(dst, &pos, cap, "[UTEST-XML-SUMMARY] tests=")) return 0;
    if (!u_append_uint(dst, &pos, cap, tests)) return 0;
    if (!u_append(dst, &pos, cap, " failures=")) return 0;
    if (!u_append_uint(dst, &pos, cap, failures)) return 0;
    if (!u_append(dst, &pos, cap, " skipped=")) return 0;
    if (!u_append_uint(dst, &pos, cap, skipped)) return 0;
    u_format_seconds(time_buf, sizeof(time_buf), total_ms);
    if (!u_append(dst, &pos, cap, " time=")) return 0;
    if (!u_append(dst, &pos, cap, time_buf)) return 0;
    if (!u_append(dst, &pos, cap, " aborted=")) return 0;
    if (!u_append_uint(dst, &pos, cap, aborted ? 1u : 0u)) return 0;
    if (!u_append(dst, &pos, cap, " not_run=")) return 0;
    if (!u_append_uint(dst, &pos, cap, not_run)) return 0;
    return 1;
}

static void u_emit_xml_suite_close(uint32_t passed, uint32_t failed,
                                   uint32_t skipped, uint32_t skip_records,
                                   uint64_t total_ms, int aborted,
                                   uint32_t not_run)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (!s_xml_mode) return;
    /* Emit a summary line (not a valid XML fragment on its own --
     * scripts/test.sh patches the opening <testsuite ...> from these
     * numbers). Separate line prefixed with [UTEST-XML-SUMMARY] so the
     * post-processor can grep for it distinctly from the <testcase>
     * and closer lines. */
    if (u_format_xml_summary(line, sizeof(line),
                             passed + failed + skipped + skip_records,
                             failed, skipped + skip_records, total_ms,
                             aborted, not_run)) {
        utest_record_log(LOG_INFO, "%s", line);
    } else {
        /* Never publish a truncated summary: a short `tests=` reads as a
         * smaller-but-plausible run. Emit an unmistakable marker instead
         * and let the host gate fail the run on its presence. */
        utest_record_log(LOG_ERROR,
             "[UTEST-XML-SUMMARY-OVERFLOW] summary line exceeded %u bytes",
             (uint64_t)sizeof(line));
    }
    utest_record_log(LOG_INFO, "[UTEST-XML] </testsuite>");
}

/* Emit one `<testcase>` element for a binary. `verdict` is 0=PASS,
 * 1=FAIL, 2=SKIP. `reason` may be NULL; otherwise it's the
 * launcher-formatted reason string for FAIL/SKIP. `type` is the
 * taxonomy value that maps to the XML `classname` attribute (and
 * the JSON `type` field in the sibling emitter).
 *
 * Overflow contract: every u_append / u_xml_escape call is checked.
 * If any returns 0, we bail to an `overflow` label that emits a
 * minimal self-closing `<testcase name="..." classname="overflow"/>`
 * record so the CI artifact stays well-formed. The binary is logged
 * via LOG_WARN so the author knows their name/reason was too long.
 * Codex quality M 2026-04-20: glob-discovered names up to VFS_MAX_NAME
 * (256 chars) could otherwise exceed the line buffer and corrupt the
 * assembled XML file downstream. */
/* Split out of the emitter so the record can be BUILT without being
 * logged. The name bound is derived from these very literals, and a
 * derivation that only ever runs inside a klog call cannot be checked:
 * the unit tests format a bound-length name here and assert the record
 * fits, which is what proves UTEST_FIXED_XML still matches the format
 * string below rather than a copy of it that drifted. Returns 1 on
 * success, 0 if the record would not fit `cap`. */
static int u_format_xml_testcase(char *line, uint32_t cap, const char *name,
                                 utest_type_t type, int verdict,
                                 uint64_t time_ms, const char *reason,
                                 const char *classname_override)
{
    uint32_t pos = 0;
    char time_buf[24];

    if (cap == 0) return 0;
    line[0] = '\0';
    u_format_seconds(time_buf, sizeof(time_buf), u_clamp_time_ms(time_ms));

    #define APP(s)     do { if (!u_append(line, &pos, cap, (s))) goto overflow; } while (0)
    #define APP_XML(s) do { if (!u_xml_escape(line, &pos, cap, (s))) goto overflow; } while (0)

    APP("[UTEST-XML] <testcase name=\"");
    APP_XML(name);
    APP("\" classname=\"");
    /* Synthetic skip records override the taxonomy label so a JUnit
     * consumer can separate them from real binaries structurally, not
     * only by parsing the "::skipped-block-" name suffix. */
    APP(classname_override ? classname_override : u_type_label(type));
    APP("\" time=\"");
    APP(time_buf);
    APP("\"");

    if (verdict == 0) {
        APP("/>");
    } else {
        APP(">");
        if (verdict == 2) {
            APP("<skipped");
            if (reason && reason[0]) {
                APP(" message=\"");
                APP_XML(reason);
                APP("\"");
            }
            APP("/>");
        } else {
            APP("<failure");
            if (reason && reason[0]) {
                APP(" message=\"");
                APP_XML(reason);
                APP("\"");
            }
            APP("/>");
        }
        APP("</testcase>");
    }
    #undef APP
    #undef APP_XML
    return 1;

overflow:
    return 0;
}

static void u_emit_xml_testcase(const char *name, utest_type_t type,
                                 int verdict, uint64_t time_ms,
                                 const char *reason,
                                 const char *classname_override)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (!s_xml_mode) return;
    if (u_format_xml_testcase(line, sizeof(line), name, type, verdict,
                              time_ms, reason, classname_override)) {
        utest_record_log(LOG_INFO, "%s", line);
        return;
    }
    /* The fallback must preserve the VERDICT and the classname, not just
     * stay well-formed. A self-closing <testcase/> reads as PASSED, so an
     * overflowing skip record used to vanish from the failures/skipped
     * accounting while the suite header still counted it -- a record that
     * silently became a pass. The name is the only part we drop, because
     * the name is what did not fit. */
    utest_record_log(LOG_ERROR,
         "[UTEST-RECORD-OVERFLOW] XML record for '%s' exceeded its buffer",
         name);
    if (verdict == 0) {
        utest_record_log(LOG_INFO,
             "[UTEST-XML] <testcase name=\"overflow\" classname=\"%s\" time=\"0\"/>",
             classname_override ? classname_override : u_type_label(type));
    } else {
        utest_record_log(LOG_INFO,
             "[UTEST-XML] <testcase name=\"overflow\" classname=\"%s\" time=\"0\">"
             "%s</testcase>",
             classname_override ? classname_override : u_type_label(type),
             verdict == 2 ? "<skipped message=\"record name too long\"/>"
                          : "<failure message=\"record name too long\"/>");
    }
}

/* Build one `[UTEST-JSON] {...}` per binary. Split from its emitter for
 * the same reason as the XML formatter above: UTEST_FIXED_JSON_BINARY is
 * derived from these literals, and the tests format a bound-length name
 * here to prove the two have not drifted apart. Returns 1 on success, 0
 * if the record would not fit `cap`. */
static int u_format_json_testcase(char *line, uint32_t cap, const char *name,
                                  utest_type_t type, int verdict,
                                  uint64_t time_ms, const char *reason,
                                  const struct u_report *rep)
{
    uint32_t pos = 0;
    const char *status;

    if (cap == 0) return 0;
    line[0] = '\0';
    switch (verdict) {
    case 0: status = "PASS"; break;
    case 2: status = "SKIP"; break;
    default: status = "FAIL"; break;
    }

    #define APP(s)      do { if (!u_append(line, &pos, cap, (s))) goto overflow; } while (0)
    #define APP_JSON(s) do { if (!u_json_escape(line, &pos, cap, (s))) goto overflow; } while (0)
    #define APP_UINT(v) do { if (!u_append_uint(line, &pos, cap, (uint64_t)(v))) goto overflow; } while (0)

    /* record_kind is the stream's discriminator and leads every record.
     * The stream carries two kinds of object -- one per BINARY and one per
     * synthetic skip block -- and a consumer that counted every object
     * with a "name" would report more testcases than summary.total and
     * skew every pass rate derived from it. */
    APP("[UTEST-JSON] {\"record_kind\":\"binary\",\"name\":\"");
    APP_JSON(name);
    APP("\",\"type\":\"");
    APP(u_type_label(type));
    APP("\",\"status\":\"");
    APP(status);
    APP("\",\"time_ms\":");
    APP_UINT(u_clamp_time_ms(time_ms));
    if (reason && reason[0]) {
        APP(",\"reason\":\"");
        APP_JSON(reason);
        APP("\"");
    }
    /* Report dimension for this binary. Emitted only for an ACCEPTED
     * report: an invalid one already escalated the verdict to FAIL, and
     * republishing its counts would hand a consumer the very numbers the
     * kernel just refused to believe. JSON has no schema to violate here,
     * so the three-way outcome rides as named fields; XML gets it as
     * standard <skipped/> testcases instead. */
    if (rep && rep->state == TASK_UTEST_REPORT_VALID) {
        APP(",\"asserts_passed\":");
        APP_UINT(rep->asserts_passed);
        APP(",\"asserts_failed\":");
        APP_UINT(rep->asserts_failed);
        APP(",\"skip_blocks\":");
        APP_UINT(rep->skip_blocks);
    }
    APP("}");
    #undef APP
    #undef APP_JSON
    #undef APP_UINT
    return 1;

overflow:
    return 0;
}

static void u_emit_json_testcase(const char *name, utest_type_t type,
                                  int verdict, uint64_t time_ms,
                                  const char *reason,
                                  const struct u_report *rep)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (!s_json_mode) return;
    if (u_format_json_testcase(line, sizeof(line), name, type, verdict,
                               time_ms, reason, rep)) {
        utest_record_log(LOG_INFO, "%s", line);
        return;
    }
    /* Keep record_kind: it is the stream's discriminator, and a consumer
     * following the documented contract drops any record without it --
     * which would make an overflowing binary disappear entirely rather
     * than surface as the failure it is. */
    utest_record_log(LOG_ERROR,
         "[UTEST-RECORD-OVERFLOW] JSON record for '%s' exceeded its buffer",
         name);
    utest_record_log(LOG_INFO,
         "[UTEST-JSON] {\"record_kind\":\"binary\",\"name\":\"overflow\","
         "\"status\":\"FAIL\",\"time_ms\":0,\"reason\":\"record name too long\"}");
}

/* Build the [UTEST-JSON] summary body. Returns 1 on success, 0 if the
 * record would not fit. Same checked-append and testable-cap rationale as
 * u_format_xml_summary; this one matters more because a truncated JSON
 * object is not merely wrong, it is unparseable.
 *
 * The record carries the BINARY counts only. The assertion-level report
 * dimension it used to nest inline moved to its own `run_report` record
 * for transport reasons -- see u_format_json_run_report. The host
 * assembler nests it back under `summary.reported`, so the artifact's
 * shape is unchanged. */
static int u_format_json_summary(char *dst, uint32_t cap, uint32_t passed,
                                 uint32_t failed, uint32_t skipped,
                                 uint64_t total_ms)
{
    uint32_t pos = 0;

    dst[0] = '\0';
    #define JAPP(s)  do { if (!u_append(dst, &pos, cap, (s))) return 0; } while (0)
    #define JNUM(v)  do { if (!u_append_uint(dst, &pos, cap, (uint64_t)(v))) return 0; } while (0)

    JAPP("[UTEST-JSON] {\"summary\":{\"passed\":");
    JNUM(passed);
    JAPP(",\"failed\":");
    JNUM(failed);
    JAPP(",\"skipped\":");
    JNUM(skipped);
    JAPP(",\"total\":");
    JNUM(passed + failed + skipped);
    JAPP(",\"time_ms\":");
    JNUM(total_ms);
    JAPP("}}");
    #undef JAPP
    #undef JNUM
    return 1;
}

/* Build the assertion-level report record.
 *
 * Split out of the summary object for the same transport reason as
 * run_meta, and measured rather than assumed: with all seven counters
 * nested inline the live summary record reached 242 of the 255 bytes klog
 * can carry, so a suite an order of magnitude larger -- four-digit binary
 * counts, six-digit assertion counts -- overflowed it and the launcher
 * published an overflow marker instead of a summary. Each dimension now
 * gets its own record with room for every field at its uint32 maximum.
 *
 * The units stay separated exactly as before: assertion counts and binary
 * counts are different things, and the host assembler nests these back
 * under `summary.reported` so no consumer can read one as the other. */
static int u_format_json_run_report(char *dst, uint32_t cap,
                                    const struct u_report_totals *rt)
{
    uint32_t pos = 0;

    dst[0] = '\0';
    #define RAPP(s)  do { if (!u_append(dst, &pos, cap, (s))) return 0; } while (0)
    #define RNUM(v)  do { if (!u_append_uint(dst, &pos, cap, (uint64_t)(v))) return 0; } while (0)

    RAPP("[UTEST-JSON] {\"record_kind\":\"run_report\",\"asserts_passed\":");
    RNUM(rt->asserts_passed);
    RAPP(",\"asserts_failed\":");
    RNUM(rt->asserts_failed);
    RAPP(",\"skip_blocks\":");
    RNUM(rt->skip_blocks);
    RAPP(",\"skip_records\":");
    RNUM(rt->skip_records);
    RAPP(",\"binaries_reported\":");
    RNUM(rt->reported);
    RAPP(",\"binaries_invalid\":");
    RNUM(rt->invalid);
    RAPP(",\"binaries_unreported\":");
    RNUM(rt->unreported);
    RAPP("}");
    #undef RAPP
    #undef RNUM
    return 1;
}

/* Build the run-completeness record.
 *
 * This is a RECORD OF ITS OWN rather than two more fields on the summary,
 * and the reason is the transport. Every record reaches the host through
 * klog, whose ring entry is `message[256]` and which bounds the formatted
 * message to that size (klog_emit -> vformat_buf). The summary record
 * already measures 242 characters on a live run, so appending the
 * completeness pair to it produced a record longer than klog can carry:
 * the wire copy is silently cut mid-object, which for JSON means
 * unparseable rather than merely short. Measured 2026-07-29 -- the
 * assertion-report dimension had already spent nearly all the headroom.
 *
 * Splitting keeps every record small and leaves the summary's shape and
 * field order untouched; the host assembler folds this record into the
 * artifact's `summary` object, so a consumer reading the FILE still sees
 * one summary carrying `aborted` and `not_run`. `record_kind` is already
 * this stream's discriminator, so a third kind costs the consumer nothing
 * it was not already required to handle. */
static int u_format_json_run_meta(char *dst, uint32_t cap, int aborted,
                                  uint32_t not_run)
{
    uint32_t pos = 0;

    dst[0] = '\0';
    if (!u_append(dst, &pos, cap,
                  "[UTEST-JSON] {\"record_kind\":\"run_meta\",\"aborted\":"))
        return 0;
    if (!u_append(dst, &pos, cap, aborted ? "true" : "false")) return 0;
    if (!u_append(dst, &pos, cap, ",\"not_run\":")) return 0;
    if (!u_append_uint(dst, &pos, cap, not_run)) return 0;
    if (!u_append(dst, &pos, cap, "}")) return 0;
    return 1;
}

static void u_emit_json_summary(uint32_t passed, uint32_t failed,
                                uint32_t skipped,
                                const struct u_report_totals *rt,
                                uint64_t total_ms)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (!s_json_mode) return;
    /* Report dimension first, summary second: the summary is the record the
     * host anchors the stream's tail on, so everything it will be assembled
     * with must already be on the wire. */
    if (u_format_json_run_report(line, sizeof(line), rt)) {
        utest_record_log(LOG_INFO, "%s", line);
    } else {
        utest_record_log(LOG_ERROR,
             "[UTEST-JSON] {\"summary_error\":\"overflow\"}");
        utest_record_log(LOG_ERROR,
             "[UTEST-JSON-SUMMARY-OVERFLOW] run_report record exceeded %u bytes",
             (uint64_t)sizeof(line));
        return;
    }
    if (u_format_json_summary(line, sizeof(line), passed, failed, skipped,
                              total_ms)) {
        utest_record_log(LOG_INFO, "%s", line);
    } else {
        /* Syntactically valid JSON that cannot be mistaken for a run
         * summary, so a consumer fails rather than reading a truncated
         * object as zero tests. */
        utest_record_log(LOG_ERROR,
             "[UTEST-JSON] {\"summary_error\":\"overflow\"}");
        utest_record_log(LOG_ERROR,
             "[UTEST-JSON-SUMMARY-OVERFLOW] summary record exceeded %u bytes",
             (uint64_t)sizeof(line));
    }
}

static void u_emit_json_run_meta(int aborted, uint32_t not_run)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (!s_json_mode) return;
    if (u_format_json_run_meta(line, sizeof(line), aborted, not_run)) {
        utest_record_log(LOG_INFO, "%s", line);
    } else {
        /* Two fixed labels and one decimal count cannot outgrow a 256-byte
         * buffer, so reaching here means the buffer contract itself
         * regressed. Say so on the same overflow channel the host already
         * fails on rather than dropping the record: a missing completeness
         * record is exactly the ambiguity it exists to remove. */
        utest_record_log(LOG_ERROR,
             "[UTEST-JSON-SUMMARY-OVERFLOW] run_meta record exceeded %u bytes",
             (uint64_t)sizeof(line));
    }
}

/* Build the [UTEST-REPORT-SUMMARY] body -- the report dimension's own
 * line, deliberately NOT folded into the launcher's legacy
 * `=== N passed, N failed, N skipped of N total ===` summary.
 *
 * That legacy line is parsed by scripts/test.sh with position-sensitive
 * sed expressions AND is the string its boot-completion poll waits for;
 * appending a second passed/failed/skipped triplet to it would let the
 * greedy patterns capture the wrong numbers and silently redefine what
 * `make test` counts as a failure. Separate tag, separate parser. */
static int u_format_report_summary(char *dst, uint32_t cap,
                                   const struct u_report_totals *rt)
{
    uint32_t pos = 0;

    dst[0] = '\0';
    #define RAPP(s)  do { if (!u_append(dst, &pos, cap, (s))) return 0; } while (0)
    #define RNUM(v)  do { if (!u_append_uint(dst, &pos, cap, (uint64_t)(v))) return 0; } while (0)

    RAPP("[UTEST-REPORT-SUMMARY] asserts_passed=");
    RNUM(rt->asserts_passed);
    RAPP(" asserts_failed=");
    RNUM(rt->asserts_failed);
    RAPP(" skip_blocks=");
    RNUM(rt->skip_blocks);
    RAPP(" skip_records=");
    RNUM(rt->skip_records);
    RAPP(" reported=");
    RNUM(rt->reported);
    RAPP(" invalid=");
    RNUM(rt->invalid);
    RAPP(" unreported=");
    RNUM(rt->unreported);
    #undef RAPP
    #undef RNUM
    return 1;
}

static void u_emit_report_summary(const struct u_report_totals *rt)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (u_format_report_summary(line, sizeof(line), rt))
        utest_record_log(LOG_INFO, "%s", line);
    else
        utest_record_log(LOG_ERROR,
             "[UTEST-REPORT-SUMMARY-OVERFLOW] report summary exceeded %u bytes",
             (uint64_t)sizeof(line));
}

/* How many of `requested` skip records may still be emitted, given
 * `already_emitted` across the run so far. Pure, so the aggregate stop
 * can be unit-tested without spawning anything: it is the arithmetic that
 * decides whether a hostile fan-out is cut off, and an off-by-one here
 * either truncates a legitimate run's artifacts or leaves the amplifica-
 * tion path open by one binary's worth of records. */
static uint32_t u_skip_records_allowed(uint32_t already_emitted,
                                       uint32_t requested)
{
    uint32_t room = (already_emitted < UTEST_SKIP_RECORD_BUDGET)
                        ? (UTEST_SKIP_RECORD_BUDGET - already_emitted) : 0u;
    return (requested < room) ? requested : room;
}

/* Emit one synthetic skip-block record into the JSON stream.
 *
 * Separate from u_emit_json_testcase because it is a DIFFERENT record
 * kind, not a testcase with odd fields: it carries `record_kind`,
 * the `parent` binary it belongs to, and its `skip_index`, so a consumer
 * can group the records under their binary, count binaries and skip
 * blocks separately, and reconcile both against the summary. Reusing the
 * testcase emitter is what made the two indistinguishable.
 *
 * Same bounded-append + overflow-fallback contract as its sibling. */
/* Split from its emitter for the same reason as the two testcase
 * formatters -- and this is the one that matters most: the skip_block
 * carries the name TWICE, so it is the kind that BINDS the derived
 * bound. UTEST_FIXED_JSON_SKIP is a second copy of the literals below,
 * and only formatting a bound-length name through this function proves
 * the copy still matches. Returns 1 on success, 0 if it would not fit. */
static int u_format_json_skip_record(char *line, uint32_t cap,
                                     const char *rec_name, const char *parent,
                                     uint32_t index)
{
    uint32_t pos = 0;

    if (cap == 0) return 0;
    line[0] = '\0';

    #define APP(s)      do { if (!u_append(line, &pos, cap, (s))) goto overflow; } while (0)
    #define APP_JSON(s) do { if (!u_json_escape(line, &pos, cap, (s))) goto overflow; } while (0)
    #define APP_UINT(v) do { if (!u_append_uint(line, &pos, cap, (uint64_t)(v))) goto overflow; } while (0)

    APP("[UTEST-JSON] {\"record_kind\":\"skip_block\",\"name\":\"");
    APP_JSON(rec_name);
    APP("\",\"parent\":\"");
    APP_JSON(parent);
    APP("\",\"skip_index\":");
    APP_UINT(index);
    APP(",\"status\":\"SKIP\",\"reason\":\"sub-test block skipped "
        "(reason on serial log)\"}");
    #undef APP
    #undef APP_JSON
    #undef APP_UINT
    return 1;

overflow:
    return 0;
}

static void u_emit_json_skip_record(const char *rec_name, const char *parent,
                                    uint32_t index)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (!s_json_mode) return;
    if (u_format_json_skip_record(line, sizeof(line), rec_name, parent,
                                  index)) {
        utest_record_log(LOG_INFO, "%s", line);
        return;
    }
    utest_record_log(LOG_ERROR,
         "[UTEST-RECORD-OVERFLOW] JSON skip record for '%s' exceeded its buffer",
         parent);
    utest_record_log(LOG_INFO,
         "[UTEST-JSON] {\"record_kind\":\"skip_block\",\"name\":\"overflow\","
         "\"parent\":\"overflow\",\"skip_index\":0,\"status\":\"SKIP\","
         "\"reason\":\"record name too long\"}");
}

/* Build the synthetic record name for one skipped block:
 * `<binary>::skipped-block-<k>`. Returns 1 on success, 0 if it would not
 * fit (the caller then skips the synthetic record rather than emitting a
 * truncated, ambiguous name).
 *
 * The label is deliberately positional rather than descriptive: the
 * kernel receives a COUNT, not the identity of each skipped block, and
 * inventing a plausible-looking test name would be a fabricated identity
 * in an artifact whose whole purpose is to stop lying about coverage.
 * `::skipped-block-K` says exactly what is known -- the K-th skip site
 * this binary took -- and the reason text stays on serial where the
 * harness printed it. */
static int u_build_skip_record_name(char *dst, uint32_t cap,
                                    const char *base, uint32_t k)
{
    uint32_t pos = 0;

    dst[0] = '\0';
    if (!u_append(dst, &pos, cap, base)) return 0;
    if (!u_append(dst, &pos, cap, "::skipped-block-")) return 0;
    if (!u_append_uint(dst, &pos, cap, k)) return 0;
    return 1;
}

/* Emit one TAP point + one <testcase><skipped/> + one JSON record per
 * skip block an ACCEPTED report declared, and advance the TAP point
 * counter for each.
 *
 * Why records and not just a count on the binary's own record: the
 * binary passed, so its record must stay `ok` / non-skipped -- calling a
 * partly-verified binary skipped is the error the section's warning box
 * forbids. But leaving the skip only in prose means TAP's `# SKIP` and
 * JUnit's `skipped=` still read zero, which is the false-coverage signal
 * the section exists to remove. One record per skip block satisfies both:
 * the standard fields become literally accurate and the binary keeps its
 * true verdict. */
/* Emit one TAP point, bounded by the same transport cap as every other
 * record and FAIL-CLOSED on overflow.
 *
 * TAP puts its directive LAST (`ok 7 - name # SKIP`), and klog truncates at
 * its message field -- so a long enough binary name silently cut the
 * `# SKIP` off the end and turned a skipped test into a bare passing `ok`,
 * with no marker anywhere. Skip-record points are worse: their synthetic
 * name is built into a VFS_MAX_NAME+24 buffer, so for a long parent the
 * directive was ALWAYS lost.
 *
 * A point we cannot represent faithfully is emitted as `not ok` with an
 * overflow directive instead. Turning an unrepresentable record into a
 * failure is the only safe direction: the alternative reads as a pass. */
static int u_format_tap_point(char *dst, uint32_t cap, int ok, uint32_t point,
                              const char *name, const char *directive)
{
    uint32_t pos = 0;
    const char *p;

    if (cap == 0) return 0;
    dst[0] = '\0';
    /* A name carrying `#` would inject a TAP directive of its own: TAP reads
     * everything after ` # ` as SKIP/TODO, so a binary called
     * `test_a # SKIP .exe` turns its own point into a skip. Names are
     * checked by u_is_valid_manifest_name, which rejects path characters
     * and control bytes but PERMITS `#` and spaces -- and glob-discovered
     * names come from the filesystem, not the manifest. Refuse rather than
     * emit a point a binary chose the meaning of. */
    for (p = name; p && *p; p++)
        if (*p == '#')
            return 0;

    if (!u_append(dst, &pos, cap, ok ? "ok " : "not ok ")) return 0;
    if (!u_append_uint(dst, &pos, cap, point)) return 0;
    if (!u_append(dst, &pos, cap, " - ")) return 0;
    if (!u_append(dst, &pos, cap, name)) return 0;
    if (directive && directive[0]) {
        if (!u_append(dst, &pos, cap, " # ")) return 0;
        if (!u_append(dst, &pos, cap, directive)) return 0;
    }
    return 1;
}

static void u_emit_tap_point(int ok, uint32_t point, const char *name,
                             const char *directive)
{
    char line[UTEST_RECORD_LINE_MAX];

    if (!s_tap_mode) return;

    if (u_format_tap_point(line, sizeof(line), ok, point, name, directive)) {
        utest_record_log(LOG_INFO, "%s", line);
        return;
    }
    /* Fail CLOSED. A point we cannot represent faithfully -- too long for
     * the transport, or carrying a forgeable directive -- becomes a
     * FAILURE, never a silent pass. */
    utest_record_log(LOG_ERROR,
         "[UTEST-RECORD-OVERFLOW] TAP point %u could not be represented",
         (uint64_t)point);
    utest_record_log(LOG_INFO,
         "not ok %u - unrepresentable # TAP record refused",
         (uint64_t)point);
}

static uint32_t u_emit_skip_records(const char *name, utest_type_t type,
                                    const struct u_report *rep,
                                    uint32_t already_emitted,
                                    uint32_t *tap_point)
{
    char rec[VFS_MAX_NAME + 24];
    uint32_t k, allowed;

    if (!rep || rep->state != TASK_UTEST_REPORT_VALID || rep->skip_blocks == 0)
        return 0;

    /* No machine artifact requested means no records exist to count. The
     * three emitters below are individually no-ops when their mode is
     * off, so without this the default run would report skip_records it
     * never emitted -- and could even trip the run-wide budget and fail a
     * run that asked for no fan-out-producing artifact at all. The skip
     * BLOCKS themselves are still counted and reported; it is the
     * synthetic RECORDS that do not exist here. */
    if (!s_tap_mode && !s_xml_mode && !s_json_mode)
        return 0;

    /* RUN-WIDE budget, on top of the per-binary ceiling the syscall
     * enforces. The per-task bound is multiplicative: TASK_MAX binaries
     * each reporting the maximum are all individually legal and
     * collectively demand tens of thousands of records. This is the
     * aggregate stop. Exceeding it is never silently truncated -- one
     * explicit marker names what was requested, emitted and omitted, and
     * the host gate counts that marker as a run failure, because an
     * artifact knowingly missing records is not a passing run. */
    allowed = u_skip_records_allowed(already_emitted, rep->skip_blocks);
    if (allowed < rep->skip_blocks) {
        utest_record_log(LOG_ERROR,
             "[UTEST-SKIP-RECORD-BUDGET] %s requested=%u emitted=%u "
             "omitted=%u budget=%u",
             name, (uint64_t)rep->skip_blocks, (uint64_t)allowed,
             (uint64_t)(rep->skip_blocks - allowed),
             (uint64_t)UTEST_SKIP_RECORD_BUDGET);
        if (allowed == 0)
            return 0;
    }

    for (k = 1; k <= allowed; k++) {
        if (!u_build_skip_record_name(rec, sizeof(rec), name, k)) {
            /* Same machine-readable marker its two siblings use (the
             * budget clip and the record overflow), so the host gate
             * fails the run. Omitting records with only a WARN would keep
             * the run GREEN on artifacts that under-report skips, which
             * is the exact false-coverage class this section closes. */
            utest_record_log(LOG_ERROR,
                 "[UTEST-RECORD-OVERFLOW] %s skip-record name too long -- "
                 "%u record(s) omitted",
                 name, (uint64_t)(rep->skip_blocks - k + 1));
            return k - 1;
        }
        (*tap_point)++;
        u_emit_tap_point(1, *tap_point, rec, "SKIP reported by binary");
        u_emit_xml_testcase(rec, type, 2, 0,
                            "sub-test block skipped (reason on serial log)",
                            "skip-block");
        /* The JSON record carries an explicit discriminator. Without one a
         * consumer counting objects with a "name" field would count these
         * as BINARIES -- the stream would show more testcases than
         * summary.total and every pass-rate derived from it would be
         * wrong. XML discriminates structurally (these are the only
         * records with classname="skip-block"); JSON has no such
         * convention, so it gets the fields. */
        u_emit_json_skip_record(rec, name, k);
    }
    return allowed;
}

/* Per-binary report diagnostic. Separate from the verdict line so the
 * host cross-check can recount the report dimension from serial without
 * re-parsing verdict text, and so a NONE binary produces no line at all
 * (absence is the legacy signal). */
static void u_emit_report_line(const char *name, const struct u_report *rep)
{
    if (!rep || rep->state == TASK_UTEST_REPORT_NONE)
        return;
    utest_record_log(LOG_INFO,
         "[UTEST-REPORT] %s asserts_passed=%u asserts_failed=%u "
         "skip_blocks=%u state=%s",
         name, (uint64_t)rep->asserts_passed, (uint64_t)rep->asserts_failed,
         (uint64_t)rep->skip_blocks,
         rep->state == TASK_UTEST_REPORT_VALID ? "VALID" : "INVALID");
}

/* Compare two binary names the way the FILESYSTEM does.
 *
 * C: is IXFS (mounted in partition.c), and IXFS resolves directory names
 * case-insensitively over ASCII -- `ixfs_strcmp` folds, matching the VFS
 * uppercase fold in walk_path. So `test_Bad.exe` in a manifest and
 * `test_bad.exe` on disk are the SAME FILE, and a bytewise comparison
 * treats them as two. Every identity decision the launcher makes between
 * a manifest entry and a dirent has to use the filesystem's notion of
 * sameness, or it plans, runs or refuses one file twice.
 *
 * Only ASCII letters need folding: the accepted charset is
 * [A-Za-z0-9._-], and this is reached only for names a dirent could
 * actually carry.
 *
 * Used for REFUSAL identity only. The manifest-versus-dirent dedup for
 * ACCEPTED entries deliberately stays bytewise, because that path is
 * followed by a filter match and `utest_filter=` compares literally
 * (test_usermode_glob_match). A folded dedup there would suppress a
 * dirent whose manifest twin the filter had already rejected on case,
 * and the requested binary would run zero times while total_planned and
 * total_ran still agreed -- a false green, and a strictly worse one than
 * the duplicate run it was meant to prevent. Refusals have no such
 * interaction: they bypass the filter entirely. Aligning the filter's
 * case semantics with the filesystem's is a user-visible change to
 * `utest_filter=` and is owned by the enumeration-plan section. */
static int u_name_equal_fs(const char *a, const char *b, uint32_t cap)
{
    uint32_t i;

    for (i = 0; i < cap; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb)
            return 0;
        if (ca == '\0')
            return 1;
    }
    return 1;
}

/* Has this directory entry already been refused as a MANIFEST entry?
 *
 * A name refused for length or charset can perfectly well exist on disk,
 * and it is then discovered twice -- once from the manifest, once from
 * the glob -- which would publish ONE bad binary as TWO failures under
 * two ordinals. total_planned and total_ran would agree, so the
 * completeness reconciliation cannot catch it; the dedup has to.
 *
 * REFUSE_NUL entries are deliberately excluded. Their stored C string is
 * the TRUNCATION at the embedded NUL, not the name, and no filename can
 * contain a NUL -- so a dirent that matches that truncation is a
 * different file that would be wrongly suppressed. */
static int u_refusal_already_seen(const struct manifest_state *ms,
                                  const char *name)
{
    uint32_t i;

    for (i = 0; i < ms->refused_count; i++) {
        if (ms->refused_verdict[i] == UTEST_NAME_REFUSE_NUL)
            continue;
        if (u_name_equal_fs(ms->refused_names[i], name, VFS_MAX_NAME))
            return 1;
    }
    return 0;
}

/* Publish one refused binary as a counted infrastructure FAILURE.
 *
 * This is the whole point of the refusal taxonomy: the binary was planned
 * (it reached total_planned) and it will never launch, so if it produced
 * no record the run would report fewer results than it planned and still
 * exit green. It therefore takes the SAME shape a planned binary that
 * failed to load takes -- a verdict line, a TAP point, an XML testcase
 * and a JSON record -- which is why the host needs no new record kind to
 * consume it.
 *
 * Three pieces of accounting have to move together or an artifact
 * contradicts itself: counters[1] so the binary counts as failed and the
 * host's fail-closed recount sees a matching verdict line; the TAP point
 * so the trailing plan still reconciles; and rt->unreported, because the
 * JSON harvester requires reported + invalid + unreported to equal
 * summary.total and a refused binary submitted no self-report. The
 * caller adds total_ran, symmetrically with its u_run_one calls, so
 * not_run = total_planned - total_ran stays the completeness statement it
 * claims to be. */
static void u_emit_refusal(const char *reason, uint32_t ordinal,
                           const char *raw, uint32_t digest,
                           uint32_t *tap_point, uint32_t *counters,
                           struct u_report_totals *rt)
{
    char id[UTEST_MAX_BINARY_NAME + 1u];
    uint32_t point;

    counters[1]++;      /* a refusal is a FAILED binary ...             */
    rt->unreported++;   /* ... that could not submit a self-report      */

    if (!u_build_refusal_id(id, sizeof(id), ordinal, raw, digest)) {
        /* Unreachable while the identity's static assert holds: the
         * fixed shape plus a minimum prefix is proven to fit the derived
         * bound. Kept fail-closed anyway, and on the marker channel the
         * host already fails the run on, because the alternative to a
         * record without a name is no record at all. */
        utest_record_log(LOG_ERROR,
             "[UTEST-RECORD-OVERFLOW] refusal identity for planned binary "
             "%u could not be built", (uint64_t)ordinal);
        return;
    }

    point = ++(*tap_point);
    utest_record_log(LOG_ERROR, "%s: FAIL (%s)", id, reason);
    u_emit_tap_point(0, point, id, reason);
    u_emit_xml_testcase(id, UTEST_TYPE_CORRECTNESS, 1, 0, reason,
                        (const char *)0);
    u_emit_json_testcase(id, UTEST_TYPE_CORRECTNESS, 1, 0, reason,
                         (const struct u_report *)0);
}

/* ---- Per-binary run: spawn, wait, log, cleanup --------------------- *
 *
 * Called from test_usermode_run for each binary (either from the
 * manifest or from the directory glob). Fills out_* with the
 * verdict so the caller can aggregate counters and emit TAP lines.
 * out_verdict values: 0 = PASS, 1 = FAIL, 2 = SKIP.
 * ------------------------------------------------------------------ */

/* Inner helper: spawn + wait for ONE invocation of a binary. Returns
 * the child's exit status (or timeout/verdict markers) via out_*.
 * Kept as a helper so u_run_one stays readable now that the XML/JSON
 * + isolation contract live around it. */
static void u_spawn_one(const char *name_copy, const char *path,
                        int *out_pid, int32_t *out_exit_status,
                        int *out_timed_out, uint32_t *out_leaked,
                        struct u_report *out_report, int have_stem)
{
    int pid;

    s_pending_test_path = path;

    out_report->asserts_passed = 0;
    out_report->asserts_failed = 0;
    out_report->skip_blocks    = 0;
    out_report->state          = TASK_UTEST_REPORT_NONE;

    pid = task_create(utest_loader_func, name_copy);
    *out_pid = pid;
    if (pid < 0) {
        *out_exit_status = -1;
        *out_timed_out   = 0;
        *out_leaked      = 0;
        return;
    }
    *out_exit_status = u_wait_with_timeout((uint32_t)pid,
                                           s_timeout_ms, out_timed_out);
    /* Snapshot leaks BEFORE task_cleanup destroys the handle table.
     * Callers must then run task_cleanup; the destructive reap happens
     * even later (contract: vfs_unlink needs the child's handles
     * closed first). */
    *out_leaked = have_stem ? u_isolation_snapshot_leaks((uint32_t)pid) : 0u;
    /* Same window, same reason: the harness self-report lives on the TCB
     * that task_cleanup is about to release. Taken unconditionally --
     * unlike leaks it does not depend on per-test isolation being on. */
    u_report_snapshot((uint32_t)pid, out_report);
}

static void u_run_one(const char *name, utest_type_t type,
                      uint32_t *tap_point, uint32_t *counters,
                      struct u_report_totals *rt, int *out_verdict)
{
    char name_copy[VFS_MAX_NAME];
    char path[VFS_MAX_NAME + 4];
    char stem[VFS_MAX_NAME];
    /* Sized by the derivation that bounds the records this string rides
     * in, so widening a reason literal and overrunning the buffer are the
     * same build failure rather than two separate surprises. */
    char reason[UTEST_REASON_BUF];
    int  have_stem;
    int  isolation_failed = 0;
    uint32_t ni, pi;
    int pid = -1;
    int32_t exit_status = 0;
    int timed_out = 0;
    uint32_t leaked = 0;
    uint32_t test_num;
    struct u_report report;
    uint64_t start_ms;
    uint64_t end_ms;

    reason[0] = '\0';
    (void)type; /* used below for XML/JSON classname only */

    /* Snapshot `name` into launcher-owned storage BEFORE spawning.
     * The caller may have passed in a VFS dirent or manifest arena
     * pointer -- child syscalls (SYS_READDIR, our own kfree at end of
     * run) can invalidate either. */
    for (ni = 0; name[ni] && ni < sizeof(name_copy) - 1; ni++)
        name_copy[ni] = name[ni];
    name_copy[ni] = '\0';

    /* Build C:\<name>. */
    path[0] = 'C'; path[1] = ':'; path[2] = '\\';
    pi = 3;
    for (ni = 0; name_copy[ni] && pi < sizeof(path) - 1; ni++)
        path[pi++] = name_copy[ni];
    path[pi] = '\0';

    /* Derive the scratch-dir / Registry-key stem from the binary
     * name (`test_syscall.exe` -> `test_syscall`). If the name doesn't
     * match the *.exe shape we skip isolation for this run (this
     * shouldn't happen today because u_is_test_binary() gated entry,
     * but defending against the filter being loosened later is cheap). */
    have_stem = s_isolation_enabled &&
                u_derive_test_name(name_copy, stem, sizeof(stem));
    if (have_stem && u_isolation_setup(stem) != 0)
        isolation_failed = 1;

    /* Open color scope: all kernel klog output WHILE this binary is
     * dispatched (sched / exec / elf / signal / etc.) renders in the
     * UTEST color via the per-line override in klog.c. Closed at the
     * bottom of u_run_one after task_cleanup. */
    s_utest_color_active = 1;

    start_ms = u_uptime_ms();

    /* Single spawn per binary. Stress policy is IN-BINARY, not
     * launcher-side: stress binaries iterate inside main() (see
     * user/test/test_stress_libc.c for the canonical shape). The
     * launcher does NOT loop spawning per-iteration because task
     * slots in the kernel are monotonic (`num_tasks++` in task_create
     * with no reuse in task_cleanup); a 100-iteration launcher loop
     * would exhaust TASK_MAX (32) after ~20 binaries.  The
     * s_stress_iters boot.conf knob is RESERVED for a future
     * env-passing syscall that lets stress binaries query the desired
     * iteration count at runtime. */
    u_spawn_one(name_copy, path, &pid, &exit_status,
                &timed_out, &leaked, &report, have_stem);

    if (pid < 0) {
        klog(LOG_ERROR, "UTEST", "%s: task_create failed", name_copy);
        counters[1]++;
        rt->unreported++;   /* never ran, so it never reported */
        *out_verdict = 1;
        test_num = ++(*tap_point);
        u_emit_tap_point(0, test_num, name_copy, "task_create failed");
        u_emit_xml_testcase(name_copy, type, 1, 0, "task_create failed",
                            (const char *)0);
        u_emit_json_testcase(name_copy, type, 1, 0, "task_create failed",
                             (const struct u_report *)0);
        s_utest_color_active = 0;
        if (have_stem) {
            u_isolation_reap(stem);
            u_cleanup_manifest_apply();
        }
        return;
    }

    /* Format-coverage line: emit the binary-format name the exec
     * dispatcher matched for this binary (ELF / PE32+ / EIF) BEFORE
     * task_cleanup so the name is always visible regardless of
     * PASS/FAIL/timeout/leak verdict. The name string aliases a
     * static entry in the format registry (see exec.c s_formats[])
     * so it stays valid for the lifetime of the kernel; no copy
     * needed. A binary whose format field stays NULL means
     * exec_load_fmt failed before matching -- covered by the
     * "task_exec failed" path in u_spawn_one which already logged
     * FAIL (exit=-5), so we skip the format line there.
     *
     * Without this line, a regression that silently routed PE
     * binaries through the ELF loader would still print PASS --
     * the whole point of the format-coverage probe. */
    {
        struct task *t = task_get_by_pid((uint32_t)pid);
        const char *fmt = (t && t->loaded_format) ? t->loaded_format : (const char *)0;
        if (fmt)
            utest_record_log(LOG_INFO, "%s: format=%s", name_copy, fmt);
    }

    /* Compute preliminary verdict + reason from the child's exit
     * status. Escalations for leaked handles and isolation failures
     * apply BELOW; the single [UTEST]/TAP/XML/JSON emit happens at the
     * bottom so external consumers never see a contradictory "ok N
     * ... not ok N" pair for the same test_num.  Emitting a PASS log
     * before the leak check fires lets a subsequent FAIL produce two
     * lines for the same run. */
    if (exit_status == 0) {
        counters[0]++;  /* passed (may be rolled back by escalations) */
        *out_verdict = 0;
    } else if (exit_status == UTEST_EXIT_SKIP) {
        counters[2]++;  /* skipped */
        *out_verdict = 2;
    } else if (timed_out) {
        counters[1]++;  /* failed (timeout) */
        *out_verdict = 1;
        {
            uint32_t rp = 0;
            u_append(reason, &rp, sizeof(reason), "timeout after ");
            u_append_uint(reason, &rp, sizeof(reason),
                          s_timeout_ms ? s_timeout_ms
                                       : UTEST_DEFAULT_TIMEOUT_MS);
            u_append(reason, &rp, sizeof(reason), "ms");
        }
    } else {
        counters[1]++;  /* failed */
        *out_verdict = 1;
        {
            uint32_t rp = 0;
            u_append(reason, &rp, sizeof(reason), "exit=");
            if (exit_status < 0) {
                u_append(reason, &rp, sizeof(reason), "-");
                u_append_uint(reason, &rp, sizeof(reason),
                              (uint64_t)(-(int64_t)exit_status));
            } else {
                u_append_uint(reason, &rp, sizeof(reason),
                              (uint64_t)exit_status);
            }
        }
    }

    /* Handle leaks escalate a PASS to FAIL (test checkpoint:
     * "A binary that opens C:\\hello.txt without closing it surfaces
     * as [UTEST] FAIL test_x: 1 handle leaked"). Tests that already
     * FAIL/SKIP keep their stronger verdict -- we don't upgrade a
     * SKIP to FAIL just because it also leaked.
     *
     * The rationale for escalation over a WARN: leaking a handle
     * across process exit is the same class of bug as leaking memory,
     * and Linux kselftest / Windows HLK both treat resource leaks as
     * test failures. A passing binary that leaks is lying about its
     * cleanup invariant; surfacing that as FAIL makes CI reject it. */
    if (have_stem && leaked > 0 && *out_verdict == 0) {
        *out_verdict = 1;
        counters[0]--;  /* undo PASS */
        counters[1]++;  /* record FAIL */
    }

    task_cleanup((uint32_t)pid);

    /* Close color scope: subsequent klog lines (the launcher's own
     * `[UTEST] <name>: PASS/FAIL` and the cleanup WARNs) still route
     * through the subsystem-name "UTEST" path and get the UTEST color
     * that way; we only need the global override while OTHER
     * subsystems (sched/exec/elf) are emitting on behalf of the
     * spawned binary. */
    s_utest_color_active = 0;

    /* Destructive cleanup runs AFTER task_cleanup: the child's handles
     * are now closed (ob_handle_table_destroy ran inside task_cleanup),
     * so vfs_unlink can reach files that were held open at exit. Any
     * failure to delete after this point is a genuine FS or Registry
     * bug, not a ref_count race. */
    if (have_stem) {
        if (u_isolation_reap(stem) != 0)
            isolation_failed = 1;
        u_cleanup_manifest_apply();
    }

    /* Isolation contract violated: if setup or reap left stale state
     * behind (rmtree cap exhausted, vfs_create failed), the next
     * binary can no longer assume a clean scratch dir. Escalate the
     * verdict: a PASS becomes FAIL so CI surfaces the broken
     * invariant. Already-FAIL/SKIP keeps its stronger verdict plus a
     * WARN noting the isolation breach. Codex quality M 2026-04-20. */
    if (isolation_failed && *out_verdict == 0) {
        *out_verdict = 1;
        counters[0]--;
        counters[1]++;
        {
            uint32_t rp = 0;
            u_append(reason, &rp, sizeof(reason), "isolation failed");
        }
    }

    /* Handle-leak reason: only fill `reason` when we escalated from
     * PASS. For FAIL/SKIP the exit-code or timeout reason already
     * dominates; adding the leak count to a JSON blob would be nice
     * but would require a structured reason schema and is a
     * nice-to-have, not a correctness issue. */
    if (reason[0] == '\0' && leaked > 0 && *out_verdict == 1) {
        uint32_t rp = 0;
        u_append(reason, &rp, sizeof(reason), "");
        u_append_uint(reason, &rp, sizeof(reason), leaked);
        u_append(reason, &rp, sizeof(reason), " handle(s) leaked");
    }

    /* Ring-3 self-report reconciliation. The counts are the binary's own
     * claim about itself, so they are checked against the outcome the
     * kernel observed before any of them reach an artifact.
     *
     * An INVALID report escalates a PASS or a SKIP to FAIL. A binary that
     * says "no assertions failed" and exits non-zero -- or claims a
     * whole-binary skip while reporting assertions it ran -- is producing
     * exactly the contradictory signal this section exists to remove, and
     * silently dropping its counts would leave the run green on a binary
     * that just proved its own reporting untrustworthy. An already-FAIL
     * keeps its stronger, more specific reason. */
    report.state = u_report_reconcile(report.state, report.asserts_failed,
                                      exit_status, timed_out);
    if (report.state == TASK_UTEST_REPORT_INVALID) {
        int was_failing = (*out_verdict == 1);
        rt->invalid++;
        *out_verdict = u_report_apply_invalid(*out_verdict, counters);
        if (!was_failing && reason[0] == '\0') {
            uint32_t rp = 0;
            u_append(reason, &rp, sizeof(reason), "invalid test report");
        }
        klog(LOG_WARN, "UTEST",
             "%s: self-report contradicts outcome (exit=%d, reported "
             "failed=%u) -- counts discarded",
             name_copy, (int64_t)exit_status,
             (uint64_t)report.asserts_failed);
    } else if (report.state == TASK_UTEST_REPORT_VALID) {
        /* Bounded by TASK_UTEST_REPORT_MAX per binary and TASK_MAX
         * binaries per run, so these 32-bit sums cannot wrap. */
        rt->reported++;
        rt->asserts_passed += report.asserts_passed;
        rt->asserts_failed += report.asserts_failed;
        rt->skip_blocks    += report.skip_blocks;
    } else {
        rt->unreported++;
    }

    /* Single verdict emit: exactly one [UTEST] line and (when TAP is
     * on) one TAP line per binary. All escalations have applied above,
     * so `*out_verdict`, `reason`, and `leaked`/`isolation_failed` are
     * their final values.  Emitting inside the raw exit-status branches
     * produces a PASS followed by a FAIL (escalation) for the same
     * test_num. */
    test_num = ++(*tap_point);
    if (*out_verdict == 0) {
        utest_record_log(LOG_INFO, "%s: PASS (exit=0)", name_copy);
        u_emit_tap_point(1, test_num, name_copy, (const char *)0);
    } else if (*out_verdict == 2) {
        utest_record_log(LOG_INFO, "%s: SKIP (exit=77)", name_copy);
        u_emit_tap_point(1, test_num, name_copy, "SKIP");
    } else if (have_stem && leaked > 0 && exit_status == 0 && !timed_out &&
               !isolation_failed) {
        /* Escalated from PASS by leak detection only. */
        utest_record_log(LOG_ERROR,
             "%s: FAIL (%u handle(s) leaked -- escalated from PASS)",
             name_copy, (uint64_t)leaked);
        {
            char d[48];
            uint32_t dp = 0;
            d[0] = '\0';
            if (u_append_uint(d, &dp, sizeof(d), leaked))
                (void)u_append(d, &dp, sizeof(d), " handle(s) leaked");
            u_emit_tap_point(0, test_num, name_copy, d);
        }
    } else if (isolation_failed && exit_status == 0 && !timed_out) {
        /* Escalated from PASS by isolation failure only. */
        utest_record_log(LOG_ERROR,
             "%s: FAIL (isolation failed -- escalated from PASS)",
             name_copy);
        u_emit_tap_point(0, test_num, name_copy, "isolation failed");
    } else if (timed_out) {
        utest_record_log(LOG_ERROR, "%s: FAIL (timeout after %ums)",
             name_copy, (uint64_t)(s_timeout_ms ? s_timeout_ms
                                                : UTEST_DEFAULT_TIMEOUT_MS));
        u_emit_tap_point(0, test_num, name_copy, "timeout");
    } else {
        utest_record_log(LOG_ERROR, "%s: FAIL (exit=%d)",
             name_copy, (int64_t)exit_status);
        {
            char d[48];
            uint32_t dp = 0;
            d[0] = '\0';
            /* u_append_uint is the only numeric appender; carry the sign
             * separately so a negative exit status still reads correctly. */
            if (u_append(d, &dp, sizeof(d), "exit=")
                && (exit_status >= 0
                    || u_append(d, &dp, sizeof(d), "-"))) {
                uint64_t mag = (exit_status < 0)
                    ? (uint64_t)(-(int64_t)exit_status)
                    : (uint64_t)exit_status;
                (void)u_append_uint(d, &dp, sizeof(d), mag);
            }
            u_emit_tap_point(0, test_num, name_copy, d);
        }
    }

    /* Extra WARN context for leaks / isolation failures that ride on
     * top of an already-FAIL/SKIP verdict (exit_status != 0). The
     * primary verdict line above already reflects the dominant
     * reason (exit / timeout). */
    if (have_stem && leaked > 0 && !(exit_status == 0 && !timed_out)) {
        klog(LOG_WARN, "UTEST",
             "%s: %u handle(s) leaked (open at exit)",
             name_copy, (uint64_t)leaked);
    }
    if (isolation_failed && !(exit_status == 0 && !timed_out)) {
        klog(LOG_WARN, "UTEST",
             "%s: isolation failed (scratch/registry state may persist)",
             name_copy);
    }

    /* XML + JSON per-binary emit: one pair per run, reason string
     * reflects the final verdict including any escalations. time_ms
     * is the wall-clock elapsed from task_create to just before this
     * emit. */
    end_ms = u_uptime_ms();
    u_emit_xml_testcase(name_copy, type, *out_verdict, end_ms - start_ms,
                        reason[0] ? reason : (const char *)0,
                        (const char *)0);
    u_emit_json_testcase(name_copy, type, *out_verdict, end_ms - start_ms,
                         reason[0] ? reason : (const char *)0, &report);

    /* Report dimension, emitted last so the binary's own records are
     * already on the wire: the diagnostic line the host cross-check
     * recounts, then one standard skip record per reported skip block. */
    u_emit_report_line(name_copy, &report);
    rt->skip_records += u_emit_skip_records(name_copy, type, &report,
                                            rt->skip_records, tap_point);
}

/* ---- Enumerate binaries via directory glob (fallback path) --------- */

struct glob_state {
    struct vfs_node *root;
    uint32_t         idx;
};

static const char *u_glob_next(struct glob_state *gs,
                               char *out_name, uint32_t out_cap,
                               utest_name_verdict_t *out_verdict)
{
    struct vfs_dirent *de;

    while ((de = vfs_readdir(gs->root, gs->idx)) != (struct vfs_dirent *)0) {
        utest_name_verdict_t v;

        gs->idx++;
        if (de->type & VFS_DIRECTORY)
            continue;
        /* Apply the SAME classifier used for manifest entries: both
         * must reject path separators, control bytes, and `..`
         * components.  Without the stricter gate, a directory entry
         * like `test_bad\nline.exe` would pass `u_is_test_binary`
         * (prefix+suffix only) and split a subsequent [UTEST-XML]
         * testcase record across physical log lines, corrupting the
         * post-processed JUnit XML.
         *
         * The two outcomes are no longer the same. A file that is not
         * test_*.exe is skipped exactly as before -- C:\ holds plenty of
         * files this framework has no opinion about. A file that IS
         * test-shaped and still fails the gate is RETURNED, carrying its
         * verdict, so the caller counts it as the planned-and-refused
         * binary it is instead of stepping over it in silence. */
        v = u_classify_name(de->name);
        if (v == UTEST_NAME_NOT_TEST_SHAPED)
            continue;
        if (out_verdict)
            *out_verdict = v;
        /* Snapshot the name -- de->name is shared dirent storage. */
        {
            uint32_t i;
            for (i = 0; de->name[i] && i < out_cap - 1; i++)
                out_name[i] = de->name[i];
            out_name[i] = '\0';
        }
        return out_name;
    }
    return (const char *)0;
}

/* ---- Public entry: scan + run + report ----------------------------- */

void test_usermode_run(void)
{
    struct manifest_state manifest;
    struct vfs_node      *root;
    uint32_t              counters[3] = { 0, 0, 0 }; /* pass, fail, skip */
    /* Report dimension, kept strictly separate from `counters` above:
     * those count BINARIES (what the exit codes said), these count what
     * the binaries reported about their own assertions and skip blocks.
     * Folding the two together would make every total ambiguous. */
    struct u_report_totals rt = { 0, 0, 0, 0, 0, 0, 0 };
    /* Running TAP point number. No longer equal to the binary ordinal:
     * a binary that reports skip blocks contributes extra points after
     * its own, which is why the `1..N` plan moved to the end of the
     * stream (TAP permits leading or trailing; only a trailing one can
     * state a count the launcher does not know up front). */
    uint32_t              tap_point = 0;
    /* Set when a `Bail out!` was emitted. TAP makes bail-out terminal, so
     * the trailing plan must not follow it. */
    int                   tap_bailed = 0;
    /* Run-completeness state, published in both machine summaries. Declared
     * at function scope because the abort is DETECTED inside the phase loop
     * but ANNOUNCED and SERIALISED after it. */
    int                   suite_aborted = 0;
    uint32_t              not_run = 0;
    uint32_t              total_planned = 0;
    uint32_t              total_ran = 0;
    uint32_t              skipped_by_filter = 0;
    /* Per-run refusal counter. Function-local, so the launcher gains no
     * shared mutable state: test_usermode_run is the single enumerator,
     * and a static here would make two runs in one boot collide. It is
     * what makes refusal identities distinct BY CONSTRUCTION -- a digest
     * alone can only make collisions unlikely, and an operator who cannot
     * tell two refused binaries apart cannot act on either. */
    uint32_t              refusal_ordinal = 0;
    /* Sum of expected task_create costs across planned binaries. A
     * plain `test_*.exe` costs 1 slot; fork-heavy binaries like
     * test_process.exe tag `expects_tasks=<N>` in the manifest so the
     * pre-flight budget check reflects real TASK_MAX pressure. Codex
     * quality 2026-04-21. */
    uint32_t              total_task_budget = 0;
    int                   use_manifest;
    char                  scratch_name[VFS_MAX_NAME];
    uint64_t              run_start_ms;
    uint64_t              run_end_ms;
    uint32_t              i;

    if (!vfs_is_mounted('C')) {
        klog(LOG_DEBUG, "UTEST", "C:\\ not mounted -- skipping user-mode tests");
        return;
    }

    root = vfs_get_drive_root('C');
    if (!root || !root->ops || !root->ops->readdir) {
        klog(LOG_DEBUG, "UTEST", "C:\\ root has no readdir -- skipping");
        return;
    }

    /* Open the framed run BEFORE anything else can reach serial. Every
     * return path past this point pairs it with u_frame_end(); the two
     * checks above return without a frame on purpose, because an
     * unterminated frame is a stronger signal than no frame at all and
     * neither of those paths emits a single record. */
    u_frame_begin();

    /* Enable preemptive scheduler for the timeout watchdog. Pair with
     * scheduler_disable before returning so boot_phase3 continues in
     * its expected non-preemptive state. */
    scheduler_enable();

    use_manifest = u_manifest_load(&manifest);

    /* First pass: count planned binaries (post-filter) so the TAP
     * plan line can emit `1..N` once, up-front. Both manifest and
     * glob paths apply the same filter. */
    /* Refused manifest entries are planned UNCONDITIONALLY -- outside the
     * `use_manifest` gate and outside the filter.
     *
     * Outside the gate because `use_manifest` only decides whether the
     * manifest is authoritative for what RUNS; a malformed line somebody
     * wrote is a fault whether or not any valid line kept it company.
     * Outside the filter because the filter selects among binaries we can
     * IDENTIFY, and letting a name we just declined to trust decide
     * whether it gets reported would hand it back the authority the
     * refusal withdrew -- matching against the raw bytes is the only
     * alternative, and those are exactly what must not be used. */
    total_planned += manifest.refused_count;
    if (use_manifest) {
        for (i = 0; i < manifest.count; i++) {
            if (test_usermode_glob_match(s_filter, manifest.names[i])) {
                total_planned++;
                total_task_budget += manifest.expects_tasks[i];
            }
        }
    }
    /* Always also include glob-discovered binaries when the manifest
     * was absent OR overflowed. If the manifest is authoritative (no
     * overflow) we skip the glob. */
    if (!use_manifest || manifest.overflowed) {
        struct glob_state gs = { root, 0 };
        utest_name_verdict_t gv = UTEST_NAME_ACCEPT;
        while (u_glob_next(&gs, scratch_name, sizeof(scratch_name), &gv)) {
            /* When overflowed, skip binaries already listed in the
             * manifest to avoid double-running. */
            if (manifest.overflowed) {
                int already = 0;
                for (i = 0; i < manifest.count; i++) {
                    if (u_strncmp(manifest.names[i], scratch_name,
                                  VFS_MAX_NAME) == 0) {
                        already = 1;
                        break;
                    }
                }
                if (already) continue;
            }
            /* Same rule as the manifest walk above: a refused entry is
             * planned unconditionally, because the filter cannot be
             * applied to a name that was refused for being unusable --
             * unless the manifest already refused this same name, in
             * which case one bad binary would otherwise be planned
             * twice. This dedup is UNCONDITIONAL, unlike the
             * already-listed check below, which only guards the
             * overflowed-manifest case: manifest refusals are published
             * even when the manifest is not authoritative. */
            if (gv != UTEST_NAME_ACCEPT) {
                if (!u_refusal_already_seen(&manifest, scratch_name))
                    total_planned++;
                continue;   /* refusals never launch, so no task budget */
            }
            if (test_usermode_glob_match(s_filter, scratch_name)) {
                total_planned++;
                /* Glob-discovered binaries have no manifest metadata
                 * so assume the default 1-task cost. Fork-heavy
                 * binaries should be declared in the manifest with
                 * expects_tasks=<N>. */
                total_task_budget += 1u;
            }
        }
    }

    if (total_planned == 0) {
        if (s_filter)
            klog(LOG_DEBUG, "UTEST",
                 "no test_*.exe matches filter '%s' -- skipping",
                 s_filter);
        else
            klog(LOG_DEBUG, "UTEST",
                 "no test_*.exe found at C:\\ -- skipping summary");
        /* empty-suite artifact contract: when xml=1 or json=1 the
         * CI integration expects a parseable file regardless of
         * whether any binary ran. Emit a zero-test envelope so
         * scripts/test.sh always produces a valid build/test-results.xml
         * and tooling doesn't fail on missing artifact. Codex quality
         * M1 2026-04-20. */
        u_emit_xml_suite_open();
        u_emit_xml_suite_close(0, 0, 0, 0, 0, 0, 0);
        u_emit_json_summary(0, 0, 0, &rt, 0);
        u_emit_json_run_meta(0, 0);
        /* Empty suite still gets a plan (`1..0`) and a report summary, so
         * a consumer can tell "ran nothing" from "the launcher died before
         * emitting either". */
        if (s_tap_mode)
            utest_record_log(LOG_INFO, "1..0");
        u_emit_report_summary(&rt);
        /* The legacy summary line is emitted for the empty suite too. It
         * is not decoration: scripts/test.sh waits for exactly this string
         * before it post-processes anything (its poll requires the
         * user-mode summary whenever xml=/json=/tap= is set), so returning
         * without it made a zero-binary run under xml=1/json=1 hang until
         * the host timeout and exit 1 -- with the parseable empty envelope
         * sitting unread on serial. Same shape as the populated path
         * below, so the host's position-sensitive sed patterns apply
         * unchanged. */
        utest_record_log(LOG_INFO,
             "=== 0 passed, 0 failed, 0 skipped of 0 total ===");
        u_frame_end();
        u_manifest_free(&manifest);
        scheduler_disable();
        return;
    }

    /* Pre-flight task-slot budget check: task_create uses monotonic
     * pid = num_tasks++ and rejects once num_tasks == TASK_MAX. Slots
     * never come back today (owner: [scheduler enhancement TODO]).
     * Emitting `1..total_planned` when we know the tail would hit
     * task_create failures produces misleading TAP output. Clamp the
     * plan to the available budget, log a WARN naming the ceiling, and
     * let u_run_one surface the remaining binaries as task_create-fail
     * FAILs (deterministic, named, not silent). Codex quality H2,
     * 2026-04-20. */
    {
        uint32_t live = task_count();
        uint32_t budget = (live < TASK_MAX) ? (TASK_MAX - live) : 0u;
        /* Compare against total_task_budget (sum of per-binary
         * expects_tasks), not total_planned, so fork-heavy binaries
         * like test_process.exe get counted accurately. Codex quality
         * 2026-04-21: previously the check compared against
         * total_planned (one slot per binary) and missed the extra
         * slots consumed by nested sys_fork. */
        if (budget < total_task_budget) {
            klog(LOG_WARN, "UTEST",
                 "plan %u binaries need %u task slots, only %u free "
                 "(TASK_MAX=%u, live=%u) -- tail will FAIL with "
                 "task_create-failed until scheduler slot reuse ships",
                 (uint64_t)total_planned, (uint64_t)total_task_budget,
                 (uint64_t)budget, (uint64_t)TASK_MAX, (uint64_t)live);
        }
    }

    /* XML envelope opener. The testsuite attributes are filled in
     * by scripts/test.sh post-processing using the emitted
     * [UTEST-XML-SUMMARY] line; the raw launcher doesn't know the
     * final counts yet. */
    u_emit_xml_suite_open();

    /* Opening banner, matching the kernel test runner's shape so mixed
     * boot logs visually separate the kernel TEST_CAT_* sweep from the
     * user-mode launcher. The closing summary at the end of this
     * function already follows the same `=== N passed ... ===` form. */
    klog(LOG_INFO, "UTEST",
         "============ USER-MODE TEST BINARIES ============");
    if (s_filter && s_filter[0])
        klog(LOG_INFO, "UTEST",
             "=== Running %u binary/binaries [filter='%s'] ===",
             (uint64_t)total_planned, s_filter);
    else
        klog(LOG_INFO, "UTEST",
             "=== Running %u binary/binaries ===",
             (uint64_t)total_planned);

    run_start_ms = u_uptime_ms();

    /* two-phase execution: smoke binaries always run FIRST, and a
     * single smoke FAIL aborts the rest of the suite. Everything else
     * runs in the second phase in manifest-then-glob order.
     *
     * Phase 1: smokes only. Both manifest and glob sources are walked
     * but only entries whose derived type is UTEST_TYPE_SMOKE actually
     * dispatch. A FAIL or SKIP in this phase sets smoke_failed and
     * skips phase 2.
     *
     * Phase 2: everything non-smoke. Same walk order.
     *
     * Filter-skipped binaries contribute to skipped_by_filter in
     * phase 1 only, to avoid double counting. */
    /* Enumeration refusals are published BEFORE any binary launches.
     *
     * They are not executions -- they are results the ENUMERATION already
     * produced -- and emitting them inside the phase loop made them
     * hostage to it: refusals are correctness-typed (a name we refused to
     * trust must not claim smoke policy), so they landed in the non-smoke
     * phase, which a failing smoke binary breaks out of before reaching.
     * The run still went red through the abort gate, but every specific
     * refusal record this section promises -- verdict line, TAP point,
     * XML testcase, JSON record, and the counters behind them -- was lost
     * exactly when the operator most needed to know what was refused.
     *
     * Emitting here also removes the interaction entirely: a refusal can
     * neither trigger the smoke fast-fail nor be skipped by it, and the
     * phase loop below simply steps over refused entries. */
    for (i = 0; i < manifest.refused_count; i++) {
        total_ran++;
        u_emit_refusal(u_refusal_reason(
                           (utest_name_verdict_t)manifest.refused_verdict[i]),
                       ++refusal_ordinal, manifest.refused_names[i],
                       manifest.refused_digest[i], &tap_point, counters, &rt);
    }
    if (manifest.refused_overflowed) {
        /* More malformed manifest entries than the run can publish
         * individually. It rides the SAME path as any other refusal
         * rather than a bare diagnostic, because both artifact formats
         * reconcile record COUNT against the summary total (the JSON
         * harvester at scripts/utest-json-harvest.py checks
         * len(testcases) == summary.total, separately from the report
         * partition): a counter bumped without a record does not merely
         * under-describe the run, it makes the whole artifact
         * unparseable. Dropping the tail with a WARN instead is the
         * false-green this section exists to close. */
        total_ran++;
        total_planned++;
        u_emit_refusal("refusal array full", ++refusal_ordinal,
                       "manifest_refusal_overflow",
                       manifest.refused_count, &tap_point, counters, &rt);
    }
    if (!use_manifest || manifest.overflowed) {
        struct glob_state gs = { root, 0 };
        utest_name_verdict_t gv = UTEST_NAME_ACCEPT;
        while (u_glob_next(&gs, scratch_name, sizeof(scratch_name), &gv)) {
            uint32_t nlen = 0;
            if (gv == UTEST_NAME_ACCEPT)
                continue;
            /* Mirrors the planning walk's dedup exactly, or the two
             * would disagree and the completeness gate would fire on a
             * run that is actually consistent. */
            if (u_refusal_already_seen(&manifest, scratch_name))
                continue;
            if (manifest.overflowed) {
                int already = 0;
                for (i = 0; i < manifest.count; i++) {
                    if (u_strncmp(manifest.names[i], scratch_name,
                                  VFS_MAX_NAME) == 0) {
                        already = 1;
                        break;
                    }
                }
                if (already) continue;
            }
            while (scratch_name[nlen]) nlen++;
            total_ran++;
            u_emit_refusal(u_refusal_reason(gv), ++refusal_ordinal,
                           scratch_name, u_name_digest(scratch_name, nlen),
                           &tap_point, counters, &rt);
        }
    }

    {
        int smoke_failed = 0;
        int phase;
        for (phase = 0; phase < 2; phase++) {
            int want_smoke = (phase == 0);
            if (use_manifest) {
                for (i = 0; i < manifest.count; i++) {
                    int verdict;
                    const char *nm = manifest.names[i];
                    utest_type_t type = manifest.types[i];
                    int is_smoke = (type == UTEST_TYPE_SMOKE);
                    if (is_smoke != want_smoke) continue;
                    if (!test_usermode_glob_match(s_filter, nm)) {
                        if (phase == 0) {
                            skipped_by_filter++;
                            klog(LOG_DEBUG, "UTEST",
                                 "%s: SKIP (filter)", nm);
                        }
                        continue;
                    }
                    total_ran++;
                    u_run_one(nm, type, &tap_point, counters, &rt, &verdict);
                    if (want_smoke && verdict != 0) {
                        /* Fast-fail is IMMEDIATE, per the gate's own spec:
                         * stop this source right here rather than after the
                         * rest of the phase. Continuing would run further
                         * binaries past a smoke failure that may have left
                         * the system in the state the smoke exists to
                         * detect, and would inflate `total_ran` so the
                         * `not_run` count published in the artifacts
                         * described a boundary the gate never had. */
                        smoke_failed = 1;
                        break;
                    }
                }
            }
            if (!smoke_failed && (!use_manifest || manifest.overflowed)) {
                struct glob_state gs = { root, 0 };
                utest_name_verdict_t gv = UTEST_NAME_ACCEPT;
                while (u_glob_next(&gs, scratch_name, sizeof(scratch_name),
                                   &gv)) {
                    int verdict;
                    utest_type_t type;
                    int is_smoke;
                    /* Already published by the refusal preflight above.
                     * Skipping first is also what keeps a refused name
                     * from ever reaching u_type_for_name: bytes we
                     * declined to trust must not be able to claim smoke
                     * policy and abort the suite through the smoke gate. */
                    if (gv != UTEST_NAME_ACCEPT) continue;
                    type = u_type_for_name(scratch_name);
                    is_smoke = (type == UTEST_TYPE_SMOKE);
                    if (is_smoke != want_smoke) continue;
                    if (manifest.overflowed) {
                        int already = 0;
                        for (i = 0; i < manifest.count; i++) {
                            if (u_strncmp(manifest.names[i], scratch_name,
                                          VFS_MAX_NAME) == 0) {
                                already = 1;
                                break;
                            }
                        }
                        if (already) continue;
                    }
                    if (!test_usermode_glob_match(s_filter, scratch_name)) {
                        if (phase == 0) {
                            skipped_by_filter++;
                            klog(LOG_DEBUG, "UTEST",
                                 "%s: SKIP (filter)", scratch_name);
                        }
                        continue;
                    }
                    total_ran++;
                    u_run_one(scratch_name, type, &tap_point, counters,
                              &rt, &verdict);
                    if (want_smoke && verdict != 0) {
                        smoke_failed = 1;
                        break;
                    }
                }
            }
            if (smoke_failed && want_smoke) {
                /* One smoke non-PASS aborts the whole suite: skip phase 1.
                 * The abort is ANNOUNCED below, outside this loop, not in
                 * a phase-1 preamble -- this `break` leaves the loop
                 * entirely, so a preamble here would never execute (which
                 * is exactly how the announcement and the TAP bail-out
                 * silently stopped firing before 2026-07-29, leaving the
                 * host ABORT gate dead and TAP streams plan-inconsistent). */
                break;
            }
        }
        suite_aborted = smoke_failed;
    }

    /* Completeness reconciliation, for EVERY run and not only aborted
     * ones.
     *
     * total_planned comes from the planning walk and total_ran from a
     * SECOND traversal of C:\ taken after live children have run, so the
     * two can disagree if the directory changed in between. A blind
     * unsigned subtraction would then publish ~4.29e9 as the completeness
     * count into both machine summaries.
     *
     * Clamping alone is not enough either: reporting not_run=0 would
     * describe the run as having skipped nothing, which is the kind of
     * internally-consistent lie the completeness dimension exists to
     * prevent. So clamp AND say so on the channel the host already fails
     * on -- a disagreement between the two walks is a producer bug, not a
     * number to quietly round.
     *
     * Running it for COMPLETED runs is what closes the hole this section
     * would otherwise have left: a passing binary can delete or rename a
     * later entry between the two walks, the execution walk then omits it,
     * and a refusal that the planning walk had already counted disappears
     * from a run that still exits green. With the reconciliation here, a
     * planned binary that never produced a result is a diagnosed failure
     * instead of a silent subtraction. */
    if (total_ran > total_planned) {
        utest_record_log(LOG_ERROR,
             "[UTEST-RECORD-OVERFLOW] planned %u binaries but ran %u -- "
             "not_run is unknown",
             (uint64_t)total_planned, (uint64_t)total_ran);
        not_run = 0u;
    } else {
        not_run = total_planned - total_ran;
    }
    if (!suite_aborted && not_run > 0u) {
        /* Not an abort: nothing stopped the walk, so every planned binary
         * should have produced a record. The gap is the finding. */
        utest_record_log(LOG_ERROR,
             "[UTEST-RUN-INCOMPLETE] planned %u binaries but ran %u -- "
             "%u planned binary/binaries produced no result",
             (uint64_t)total_planned, (uint64_t)total_ran,
             (uint64_t)not_run);
    }

    /* Abort announcement -- unconditionally reachable, because it lives
     * outside the phase loop that every abort path breaks out of. */
    if (suite_aborted) {
        utest_record_log(LOG_ERROR,
             "suite ABORT (smoke failed) -- skipping %u non-smoke binaries",
             (uint64_t)not_run);
        /* TAP contract: if the plan `1..N` was emitted but we will not
         * produce N results, emit a `Bail out!` record so TAP consumers
         * (scripts/test.sh, kselftest-style runners, CI parsers) treat the
         * run as aborted instead of "missing N-K results = malformed".
         * Codex quality Phase-2, 2026-04-20. */
        if (s_tap_mode) {
            utest_record_log(LOG_INFO,
                 "Bail out! smoke failed -- %u non-smoke binaries skipped",
                 (uint64_t)not_run);
            /* Bail-out is TERMINAL in TAP: no further points and no plan
             * may follow it. Recorded so the trailing plan at the bottom
             * of this function stays suppressed. */
            tap_bailed = 1;
        }
    }

    u_manifest_free(&manifest);
    scheduler_disable();

    run_end_ms = u_uptime_ms();

    /* Summary. Counters: [0]=pass, [1]=fail, [2]=skip(exit=77). */
    if (skipped_by_filter > 0) {
        utest_record_log(LOG_INFO,
             "=== %u passed, %u failed, %u skipped of %u total "
             "(%u filtered) ===",
             (uint64_t)counters[0], (uint64_t)counters[1],
             (uint64_t)counters[2], (uint64_t)total_ran,
             (uint64_t)skipped_by_filter);
    } else {
        utest_record_log(LOG_INFO,
             "=== %u passed, %u failed, %u skipped of %u total ===",
             (uint64_t)counters[0], (uint64_t)counters[1],
             (uint64_t)counters[2], (uint64_t)total_ran);
    }

    /* TAP plan, emitted LAST. TAP 13 allows the plan at either end of the
     * stream, and only the trailing position can state a count that
     * includes the per-skip-block points -- the launcher does not know how
     * many binaries will report skips until they have all run.
     *
     * SUPPRESSED after a `Bail out!`. The TAP format makes a bail-out
     * terminal: nothing may follow it, and a consumer that has stopped
     * parsing would either miss the plan or treat the stream as malformed.
     * An aborted run is correctly plan-less -- the bail-out line IS the
     * verdict. */
    if (s_tap_mode && !tap_bailed)
        utest_record_log(LOG_INFO, "1..%u", (uint64_t)tap_point);

    /* summary emissions: close the XML envelope and drop the final
     * JSON summary record. Both are no-ops if the respective modes
     * were never enabled. The report summary is unconditional -- it is a
     * plain diagnostic line, not part of either machine artifact, and the
     * host cross-check needs it whenever any binary reported. */
    u_emit_xml_suite_close(counters[0], counters[1], counters[2],
                           rt.skip_records, run_end_ms - run_start_ms,
                           suite_aborted, not_run);
    u_emit_json_summary(counters[0], counters[1], counters[2], &rt,
                        run_end_ms - run_start_ms);
    /* Emitted AFTER the summary so the host assembler sees the completeness
     * record as the stream's terminator and can reject anything that
     * follows it as a cut-and-resumed stream. */
    u_emit_json_run_meta(suite_aborted, not_run);
    u_emit_report_summary(&rt);
    /* Terminator, emitted last on every path including an aborted suite:
     * it is what the host waits on instead of the summary, and what it
     * reconciles its own framed-line count against. */
    u_frame_end();
}

/* ---- Test-only exports --------------------------------------------- *
 *
 * Thin wrappers around file-local helpers so kernel unit tests can
 * exercise them without promoting the helpers to the public header.
 * The main public API (test_usermode_run + setters) stays minimal. */
#ifdef KERNEL_TESTS
int test_usermode_is_valid_manifest_name(const char *name)
{
    return u_is_valid_manifest_name(name);
}

/* The refusal taxonomy and the derived name bound. Exported as plain
 * integers so the test file does not need the file-local enum: the
 * numeric values ARE the contract the tests pin, and a reordering of the
 * enum that changed them would be caught by those assertions. */
int test_usermode_classify_name(const char *name);
int test_usermode_classify_name(const char *name)
{
    return (int)u_classify_name(name);
}

int test_usermode_classify_name_span(const char *name, uint32_t span_len);
int test_usermode_classify_name_span(const char *name, uint32_t span_len)
{
    return (int)u_classify_name_span(name, span_len);
}

uint32_t test_usermode_max_binary_name(void);
uint32_t test_usermode_max_binary_name(void)
{
    return (uint32_t)UTEST_MAX_BINARY_NAME;
}

/* Per-record room left at the derived bound. The tests use it to prove
 * the minimum was taken across EVERY formatter rather than assumed from
 * the one that looks worst -- the property the derivation exists for. */
uint32_t test_usermode_name_room(uint32_t kind);
uint32_t test_usermode_name_room(uint32_t kind)
{
    switch (kind) {
    case 0:  return UTEST_NAME_ROOM(UTEST_FIXED_VERDICT, 1u);
    case 1:  return UTEST_NAME_ROOM(UTEST_FIXED_XML, 1u);
    case 2:  return UTEST_NAME_ROOM(UTEST_FIXED_JSON_BINARY, 1u);
    case 3:  return UTEST_NAME_ROOM(UTEST_FIXED_JSON_SKIP, 2u);
    case 4:  return UTEST_NAME_ROOM(UTEST_FIXED_TAP, 1u);
    case 5:  return UTEST_NAME_ROOM(UTEST_FIXED_REPORT, 1u);
    default: return 0u;
    }
}

uint32_t test_usermode_name_digest(const char *p, uint32_t len);
uint32_t test_usermode_name_digest(const char *p, uint32_t len)
{
    return u_name_digest(p, len);
}

int test_usermode_build_refusal_id(char *dst, uint32_t cap, uint32_t ordinal,
                                   const char *raw, uint32_t digest);
int test_usermode_build_refusal_id(char *dst, uint32_t cap, uint32_t ordinal,
                                   const char *raw, uint32_t digest)
{
    return u_build_refusal_id(dst, cap, ordinal, raw, digest);
}

uint64_t test_usermode_clamp_time_ms(uint64_t ms);
uint64_t test_usermode_clamp_time_ms(uint64_t ms)
{
    return u_clamp_time_ms(ms);
}

/* The two per-testcase formatters, so a test can build a worst-case
 * record at the derived bound and assert it fits. This is what closes
 * the loop on the derivation: the fixed-cost macros are a SECOND copy of
 * these format strings, and only formatting through the real ones proves
 * the copies have not drifted. `report_valid` selects the widest shape
 * (the three report fields ride only on an accepted report). */
int test_usermode_format_xml_testcase(char *dst, uint32_t cap,
                                      const char *name, int verdict,
                                      uint64_t time_ms, const char *reason);
int test_usermode_format_xml_testcase(char *dst, uint32_t cap,
                                      const char *name, int verdict,
                                      uint64_t time_ms, const char *reason)
{
    return u_format_xml_testcase(dst, cap, name, UTEST_TYPE_CORRECTNESS,
                                 verdict, time_ms, reason,
                                 (const char *)0);
}

int test_usermode_format_json_testcase(char *dst, uint32_t cap,
                                       const char *name, int verdict,
                                       uint64_t time_ms, const char *reason,
                                       int report_valid);
int test_usermode_format_json_testcase(char *dst, uint32_t cap,
                                       const char *name, int verdict,
                                       uint64_t time_ms, const char *reason,
                                       int report_valid)
{
    struct u_report rep;

    rep.state          = TASK_UTEST_REPORT_VALID;
    rep.asserts_passed = TASK_UTEST_REPORT_MAX;
    rep.asserts_failed = TASK_UTEST_REPORT_MAX;
    rep.skip_blocks    = TASK_UTEST_REPORT_SKIP_MAX;
    return u_format_json_testcase(dst, cap, name, UTEST_TYPE_CORRECTNESS,
                                  verdict, time_ms, reason,
                                  report_valid ? &rep
                                               : (const struct u_report *)0);
}

/* The widest reason the launcher can compose, so a test can build the
 * worst-case record without hard-coding a literal that would drift from
 * the derivation it is meant to check. */
uint32_t test_usermode_reason_max(void);
uint32_t test_usermode_reason_max(void)
{
    return (uint32_t)UTEST_REASON_MAX;
}

/* Filesystem-identity comparison. Exported because getting it wrong does
 * not crash -- it silently plans, runs or refuses one file twice. */
int test_usermode_name_equal_fs(const char *a, const char *b);
int test_usermode_name_equal_fs(const char *a, const char *b)
{
    return u_name_equal_fs(a, b, VFS_MAX_NAME);
}

/* The BINDING formatter: the skip_block carries the name twice, so it is
 * the kind the derived bound comes from. Exported so a test can build it
 * at the bound with a worst-case skip index. */
int test_usermode_format_json_skip(char *dst, uint32_t cap,
                                   const char *rec_name, const char *parent,
                                   uint32_t index);
int test_usermode_format_json_skip(char *dst, uint32_t cap,
                                   const char *rec_name, const char *parent,
                                   uint32_t index)
{
    return u_format_json_skip_record(dst, cap, rec_name, parent, index);
}


int test_usermode_frame_tag_format(char *dst, uint32_t cap, uint32_t nonce)
{
    return u_frame_tag_format(dst, cap, nonce);
}

uint32_t test_usermode_frame_nonce_fold(uint64_t mixed)
{
    return u_frame_nonce_fold(mixed);
}

uint32_t test_usermode_frame_tag_cap(void)
{
    return (uint32_t)UTEST_FRAME_TAG_MAX;
}

int test_usermode_derive_test_name(const char *name_in,
                                   char *out, uint32_t out_cap)
{
    return u_derive_test_name(name_in, out, out_cap);
}

int test_usermode_path_join(const char *parent, const char *name,
                            char *out, uint32_t out_cap)
{
    return u_path_join(parent, name, out, out_cap);
}

int test_usermode_path_has_traversal(const char *p)
{
    return u_path_has_traversal(p);
}

int test_usermode_xml_escape(const char *src, char *dst, uint32_t cap);
int test_usermode_xml_escape(const char *src, char *dst, uint32_t cap)
{
    uint32_t pos = 0;
    if (cap == 0) return 0;
    dst[0] = '\0';
    return u_xml_escape(dst, &pos, cap, src);
}

int test_usermode_json_escape(const char *src, char *dst, uint32_t cap);
int test_usermode_json_escape(const char *src, char *dst, uint32_t cap)
{
    uint32_t pos = 0;
    if (cap == 0) return 0;
    dst[0] = '\0';
    return u_json_escape(dst, &pos, cap, src);
}

/* Ring-3 self-report helpers. The launcher's spawn path is hard-banned
 * from unit tests (it drives live task_create), so the report machinery
 * is exposed as pure functions instead: the reconciliation matrix and the
 * bounded formatters are the parts that can be wrong, and both are
 * reachable without a child process. */
uint32_t test_usermode_report_reconcile(uint32_t state, uint32_t failed,
                                        int32_t exit_status, int timed_out);
uint32_t test_usermode_report_reconcile(uint32_t state, uint32_t failed,
                                        int32_t exit_status, int timed_out)
{
    return u_report_reconcile(state, failed, exit_status, timed_out);
}

int test_usermode_report_apply_invalid(int verdict, uint32_t *counters);
int test_usermode_report_apply_invalid(int verdict, uint32_t *counters)
{
    return u_report_apply_invalid(verdict, counters);
}

uint32_t test_usermode_skip_records_allowed(uint32_t already, uint32_t want);
uint32_t test_usermode_skip_records_allowed(uint32_t already, uint32_t want)
{
    return u_skip_records_allowed(already, want);
}

uint32_t test_usermode_skip_record_budget(void);
uint32_t test_usermode_skip_record_budget(void)
{
    return UTEST_SKIP_RECORD_BUDGET;
}

int test_usermode_build_skip_record_name(char *dst, uint32_t cap,
                                         const char *base, uint32_t k);
int test_usermode_build_skip_record_name(char *dst, uint32_t cap,
                                         const char *base, uint32_t k)
{
    if (cap == 0) return 0;
    return u_build_skip_record_name(dst, cap, base, k);
}

int test_usermode_format_xml_summary(char *dst, uint32_t cap, uint32_t tests,
                                     uint32_t failures, uint32_t skipped,
                                     uint64_t total_ms, int aborted,
                                     uint32_t not_run);
int test_usermode_format_xml_summary(char *dst, uint32_t cap, uint32_t tests,
                                     uint32_t failures, uint32_t skipped,
                                     uint64_t total_ms, int aborted,
                                     uint32_t not_run)
{
    if (cap == 0) return 0;
    return u_format_xml_summary(dst, cap, tests, failures, skipped, total_ms,
                                aborted, not_run);
}

int test_usermode_format_json_summary(char *dst, uint32_t cap, uint32_t passed,
                                      uint32_t failed, uint32_t skipped,
                                      uint64_t total_ms);
int test_usermode_format_json_summary(char *dst, uint32_t cap, uint32_t passed,
                                      uint32_t failed, uint32_t skipped,
                                      uint64_t total_ms)
{
    if (cap == 0) return 0;
    return u_format_json_summary(dst, cap, passed, failed, skipped, total_ms);
}

/* The report totals are passed as a flat argument list rather than the
 * struct so the test file does not need the file-local type. */
int test_usermode_format_json_run_report(char *dst, uint32_t cap,
                                         uint32_t a_pass, uint32_t a_fail,
                                         uint32_t blocks, uint32_t records,
                                         uint32_t reported, uint32_t invalid,
                                         uint32_t unreported);
int test_usermode_format_json_run_report(char *dst, uint32_t cap,
                                         uint32_t a_pass, uint32_t a_fail,
                                         uint32_t blocks, uint32_t records,
                                         uint32_t reported, uint32_t invalid,
                                         uint32_t unreported)
{
    struct u_report_totals rt;
    if (cap == 0) return 0;
    rt.asserts_passed = a_pass;
    rt.asserts_failed = a_fail;
    rt.skip_blocks    = blocks;
    rt.skip_records   = records;
    rt.reported       = reported;
    rt.invalid        = invalid;
    rt.unreported     = unreported;
    return u_format_json_run_report(dst, cap, &rt);
}

/* The completeness record travels separately from the summary because it
 * must fit klog's message field; the emitter's real cap is exported too so
 * a test can prove the record fits the transport rather than a number the
 * test picked. */
uint32_t test_usermode_json_line_max(void);
uint32_t test_usermode_json_line_max(void)
{
    return UTEST_RECORD_LINE_MAX;
}

int test_usermode_format_json_run_meta(char *dst, uint32_t cap, int aborted,
                                       uint32_t not_run);
int test_usermode_format_json_run_meta(char *dst, uint32_t cap, int aborted,
                                       uint32_t not_run)
{
    if (cap == 0) return 0;
    return u_format_json_run_meta(dst, cap, aborted, not_run);
}

int test_usermode_format_tap_point(char *dst, uint32_t cap, int ok,
                                   uint32_t point, const char *name,
                                   const char *directive);
int test_usermode_format_tap_point(char *dst, uint32_t cap, int ok,
                                   uint32_t point, const char *name,
                                   const char *directive)
{
    return u_format_tap_point(dst, cap, ok, point, name, directive);
}

int test_usermode_format_report_summary(char *dst, uint32_t cap,
                                        uint32_t a_pass, uint32_t a_fail,
                                        uint32_t blocks, uint32_t records,
                                        uint32_t reported, uint32_t invalid,
                                        uint32_t unreported);
int test_usermode_format_report_summary(char *dst, uint32_t cap,
                                        uint32_t a_pass, uint32_t a_fail,
                                        uint32_t blocks, uint32_t records,
                                        uint32_t reported, uint32_t invalid,
                                        uint32_t unreported)
{
    struct u_report_totals rt;
    if (cap == 0) return 0;
    rt.asserts_passed = a_pass;
    rt.asserts_failed = a_fail;
    rt.skip_blocks    = blocks;
    rt.skip_records   = records;
    rt.reported       = reported;
    rt.invalid        = invalid;
    rt.unreported     = unreported;
    return u_format_report_summary(dst, cap, &rt);
}

/* taxonomy helpers -- return integer for ABI-stable test binding. */
int test_usermode_type_for_name(const char *name);
int test_usermode_type_for_name(const char *name)
{
    return (int)u_type_for_name(name);
}

int test_usermode_type_from_attr(const char *val);
int test_usermode_type_from_attr(const char *val)
{
    return (int)u_type_from_attr(val);
}

const char *test_usermode_type_label(int type);
const char *test_usermode_type_label(int type)
{
    return u_type_label((utest_type_t)type);
}
#endif
