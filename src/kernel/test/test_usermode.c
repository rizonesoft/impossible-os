/* ============================================================================
 * test_usermode.c -- Kernel-side launcher for user-mode test binaries
 *
 * Ships §3 (baseline spawn-and-wait) and §4 (manifest, timeouts, TAP,
 * SKIP, filter) of TODO-04. See include/kernel/test/test_usermode.h
 * for the public API contract.
 *
 * Sequencing: the launcher is single-threaded by design. Binaries run
 * one at a time so a leaked file handle, dirty Registry key, or stuck
 * process from binary N cannot perturb binary N+1's run. Per-test
 * isolation hardening (scratch dir + handle-leak detection) is owned
 * by §6; this file gets the basic sequence + watchdog right.
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
#include "registry.h"

/* ---- Internal state -------------------------------------------------- */

/* Path of the binary the next utest_loader_func() invocation will load.
 * Set by test_usermode_run() immediately before each task_create call;
 * read by utest_loader_func() once the new task is scheduled in.
 * Volatile because the loader runs in a different scheduling slot. */
static volatile const char *s_pending_test_path;

/* Filter set by §4 -- NULL means "run every test_*.exe". */
static const char *s_filter;

/* Per-binary wall-clock timeout in ms. 0 = default. */
static uint32_t s_timeout_ms;

/* TAP mode: 1 = emit `ok N - name` / `not ok N - name` / `1..N` plan. */
static int s_tap_mode;

/* §6 per-test isolation: 1 = scratch dir + Registry wipe + handle-leak
 * detection around each binary. Default 1 (ON); boot.conf
 * utest_isolation=0 flips it off for debugging broken cleanup hooks. */
static int s_isolation_enabled = 1;

/* UTEST color-scope flag. Set by the launcher around each spawn
 * (task_create -> task_cleanup); read by klog's color picker so every
 * kernel subsystem line emitted WHILE a user-mode test binary is the
 * live task (sched/exec/elf/signal/etc.) renders in the UTEST color
 * instead of the default per-level color. The launcher is
 * single-threaded on a single CPU at boot_tests_run time, so one
 * global suffices -- no per-CPU ABI churn. Exposed to klog via
 * test_usermode_color_active() below. */
static volatile int s_utest_color_active;

int test_usermode_color_active(void);
int test_usermode_color_active(void)
{
    return s_utest_color_active;
}

/* Defaults matching the §4 test checkpoint: 10s wall clock is long
 * enough for a trivial test_*.exe on WHPX TCG (launch overhead plus
 * ELF load plus a few syscalls is <2s), short enough that a genuine
 * hang is caught in one boot cycle. */
#define UTEST_DEFAULT_TIMEOUT_MS 10000u
/* Grace period after SIGKILL before we force state=DEAD. */
#define UTEST_KILL_GRACE_MS       500u
/* Max binaries a single manifest can list; beyond this, extras fall
 * through to the directory-glob fallback. 128 is ~2x the §9-§15
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

/* Stricter gate used for manifest entries: the file is user-provided
 * text and u_run_one concatenates `C:\<name>` before vfs_open+task_exec,
 * so entries must stay in the C:\ root and must not contain path
 * separators, drive-letter colons, or upwards traversal components.
 * The glob-discovery path is already implicitly safe because vfs_readdir
 * returns one directory entry at a time, but the manifest path has no
 * such guard. (Codex adversarial review M1, 2026-04-20.) */
static int u_is_valid_manifest_name(const char *name)
{
    const char *p;

    if (!name || !name[0])
        return 0;
    if (!u_is_test_binary(name))
        return 0;
    /* Reject path-shaped inputs. `.` and `..` can never be a valid
     * binary because they fail u_is_test_binary above, but an attacker
     * could still try `test_.._foo.exe` -- we forbid any `..` substring
     * as defense-in-depth. Same for `\`, `/`, and `:` which are the
     * three path-structure characters on the Windows-convention path. */
    for (p = name; *p; p++) {
        if (*p == '\\' || *p == '/' || *p == ':')
            return 0;
        if ((unsigned char)*p < 0x20)  /* control byte */
            return 0;
        if (p[0] == '.' && p[1] == '.')
            return 0;
    }
    return 1;
}

static uint64_t u_uptime_ms(void)
{
    return uptime_ns() / 1000000ULL;
}

/* ---- §6 per-test isolation helpers --------------------------------- *
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
 * Codex adversarial review H3 (2026-04-20): before this guard the raw
 * prefix check authorized `C:\Impossible\..\hello.txt` because the VFS
 * walker resolves real `..` entries in IXFS directories, so the delete
 * escapes outside the allowed subtree. The prefix match alone is not
 * a containment primitive; we have to ban the characters that let the
 * walker leave the subtree. */
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
            /* Guard: reject empty suffix (`HKLM\\` alone) -- Codex H2,
             * 2026-04-20: RegDeleteTree with an empty lpSubKey wipes
             * every child of HKEY_LOCAL_MACHINE, i.e. the whole
             * registry, which is catastrophic even under test=1.
             * Also restrict to the ImpossibleOS test subtree to bound
             * the blast radius. */
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

#define UTEST_MANIFEST_ARENA_BYTES 8192u  /* 128 entries * avg 64 bytes */
#define UTEST_MANIFEST_ARENA_PAGES 2u     /* 2 x 4 KiB */

struct manifest_state {
    const char *names[UTEST_MANIFEST_MAX];
    uint32_t    count;
    char       *arena;         /* pmm_alloc_contiguous()'d; NULL if not loaded */
    uintptr_t   arena_phys;    /* matching physical base for pmm_free_frame loop */
    uint32_t    arena_used;
    uint32_t    arena_cap;
    int         overflowed;    /* 1 = hit UTEST_MANIFEST_MAX cap */
};

static int u_manifest_load(struct manifest_state *ms)
{
    struct vfs_node *f;
    uint8_t *buf;
    int n;
    uint32_t size;

    ms->count       = 0;
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
     * arena[size]. Otherwise a file of exactly ARENA bytes whose last
     * line lacks a trailing newline lets the in-place tokenizer's
     * `*line_end = '\0'` write at arena[size], which is past the end
     * of the allocation. Codex adversarial review H1, 2026-04-20:
     * kernel-heap OOB write triggered by a user-provided file. */
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
            /* Everything from line_start..line_end is the binary name. */
            if (line_end > line_start) {
                if (ms->count >= UTEST_MANIFEST_MAX) {
                    ms->overflowed = 1;
                    break;
                }
                /* Terminate the name in-place. Safe because size was
                 * capped at ARENA - 1 in the file-read guard above, so
                 * arena[size] is always within the 8192-byte kmalloc
                 * even when the last line has no trailing newline. */
                *line_end = '\0';
                /* Enforce the manifest trust boundary: even a valid
                 * test_*.exe suffix does not grant write-access to the
                 * C:\ path concatenation later. Reject path-shaped or
                 * control-byte entries here so the manifest can never
                 * dispatch an arbitrary vfs_open call. (Codex M1,
                 * 2026-04-20.) */
                if (!u_is_valid_manifest_name(line_start)) {
                    klog(LOG_WARN, "UTEST",
                         "manifest: rejecting invalid entry '%s'",
                         line_start);
                    continue;
                }
                ms->names[ms->count++] = line_start;
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
 * watchdog (Codex H1, 2026-04-20). task_exit is the canonical wakeup
 * path. Status codes:
 *   -1: NULL pending path (launcher bug)
 *   -2: vfs_open failed
 *   -3: pmm_alloc_contiguous failed
 *   -4: vfs_read short / size mismatch
 *   -5: task_exec failed
 * The launcher renders any negative exit code as `[UTEST] <name>: FAIL
 * (exit=N)` so loader failures surface even though the binary itself
 * never produced output.
 * --------------------------------------------------------------------- */

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
     * task_exec returns 0 the staging buffer is no longer needed. */
    rc = task_exec(buf, size);
    if (rc < 0) {
        klog(LOG_ERROR, "UTEST", "%s: task_exec failed", path);
        for (p = 0; p < pages; p++)
            pmm_free_frame(buf_phys + (uintptr_t)p * 4096u);
        task_exit(-5);
    }
    for (p = 0; p < pages; p++)
        pmm_free_frame(buf_phys + (uintptr_t)p * 4096u);

    /* Force a cooperative reschedule so the prepared user-mode iretq
     * frame is consumed. yield() goes through schedule_now() which
     * switches regardless of the preemptive sched_enabled flag.
     * CRITICAL: do NOT replace this with `for(;;) hlt;` -- a user-mode
     * spinloop would block forever if we relied on HLT here. (§3
     * regression, 2026-04-20.) */
    for (;;)
        yield();
}

/* ---- Polled wait with timeout -------------------------------------- *
 *
 * Replaces task_waitpid in the §4 path. Semantics:
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
         * Mirror task_exit()'s Object-Manager teardown by calling
         * ob_process_mark_dead(pid) FIRST so the permanent Process
         * object flag is cleared and task_cleanup() can reclaim the
         * namespace entry. Without this the \\KernelObjects\\Process
         * <PID> entry leaks across every timed-out run (Codex quality
         * H1, 2026-04-20).
         *
         * Defensive guard: refuse to force-kill ourselves; would
         * leave the running task DEAD and trip a cascading crash. */
        if (t->state != TASK_DEAD && t != task_current()) {
            ob_process_mark_dead(pid);
            t->state = TASK_DEAD;
        }
        /* Either way, surface TIMEOUT so the launcher log / TAP / bat
         * output names the actual reason rather than SIGKILL's -9. */
        t->exit_status = UTEST_EXIT_TIMEOUT;
    }

    return t->exit_status;
}

/* ---- Per-binary run: spawn, wait, log, cleanup --------------------- *
 *
 * Called from test_usermode_run for each binary (either from the
 * manifest or from the directory glob). Fills out_* with the
 * verdict so the caller can aggregate counters and emit TAP lines.
 * out_verdict values: 0 = PASS, 1 = FAIL, 2 = SKIP.
 * ------------------------------------------------------------------ */

static void u_run_one(const char *name, uint32_t test_num, uint32_t *counters,
                      int *out_verdict)
{
    char name_copy[VFS_MAX_NAME];
    char path[VFS_MAX_NAME + 4];
    char stem[VFS_MAX_NAME];
    int  have_stem;
    int  isolation_failed = 0;
    uint32_t ni, pi;
    int pid;
    int32_t exit_status;
    int timed_out = 0;
    uint32_t leaked;

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

    /* Derive the §6 scratch-dir / Registry-key stem from the binary
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

    s_pending_test_path = path;

    pid = task_create(utest_loader_func, name_copy);
    if (pid < 0) {
        klog(LOG_ERROR, "UTEST", "%s: task_create failed", name_copy);
        counters[1]++;  /* failed */
        *out_verdict = 1;
        if (s_tap_mode)
            klog(LOG_INFO, "UTEST",
                 "not ok %u - %s # task_create failed",
                 (uint64_t)test_num, name_copy);
        /* No task was ever dispatched, so no leaked handles are
         * possible. Reap the scratch dir + Registry subkey we created
         * in u_isolation_setup (task_cleanup isn't called on this
         * path, but there's nothing to clean up from the task side
         * either -- only the launcher's own setup artifacts). */
        s_utest_color_active = 0;
        if (have_stem) {
            u_isolation_reap(stem);
            u_cleanup_manifest_apply();
        }
        return;
    }

    exit_status = u_wait_with_timeout((uint32_t)pid,
                                      s_timeout_ms, &timed_out);

    /* Snapshot the leak count BEFORE task_cleanup destroys the handle
     * table. The destructive cleanup (u_isolation_reap) runs AFTER
     * task_cleanup so the child's handles are already closed -- a
     * leaked handle on a file inside the scratch dir would otherwise
     * block its unlink (vfs_unlink rejects ref_count > 0). Codex
     * quality H1, 2026-04-20. */
    leaked = have_stem ? u_isolation_snapshot_leaks((uint32_t)pid) : 0u;

    if (exit_status == 0) {
        counters[0]++;  /* passed */
        *out_verdict = 0;
        klog(LOG_INFO, "UTEST", "%s: PASS (exit=0)", name_copy);
        if (s_tap_mode)
            klog(LOG_INFO, "UTEST", "ok %u - %s",
                 (uint64_t)test_num, name_copy);
    } else if (exit_status == UTEST_EXIT_SKIP) {
        counters[2]++;  /* skipped */
        *out_verdict = 2;
        klog(LOG_INFO, "UTEST", "%s: SKIP (exit=77)", name_copy);
        if (s_tap_mode)
            klog(LOG_INFO, "UTEST", "ok %u - %s # SKIP",
                 (uint64_t)test_num, name_copy);
    } else if (timed_out) {
        counters[1]++;  /* failed (timeout) */
        *out_verdict = 1;
        klog(LOG_ERROR, "UTEST", "%s: FAIL (timeout after %ums)",
             name_copy, (uint64_t)(s_timeout_ms ? s_timeout_ms
                                                : UTEST_DEFAULT_TIMEOUT_MS));
        if (s_tap_mode)
            klog(LOG_INFO, "UTEST",
                 "not ok %u - %s # timeout",
                 (uint64_t)test_num, name_copy);
    } else {
        counters[1]++;  /* failed */
        *out_verdict = 1;
        klog(LOG_ERROR, "UTEST", "%s: FAIL (exit=%d)",
             name_copy, (int64_t)exit_status);
        if (s_tap_mode)
            klog(LOG_INFO, "UTEST",
                 "not ok %u - %s # exit=%d",
                 (uint64_t)test_num, name_copy, (int64_t)exit_status);
    }

    /* Handle leaks escalate a PASS to FAIL (§6 test checkpoint:
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
        klog(LOG_ERROR, "UTEST",
             "%s: FAIL (%u handle(s) leaked -- escalated from PASS)",
             name_copy, (uint64_t)leaked);
        if (s_tap_mode)
            klog(LOG_INFO, "UTEST",
                 "not ok %u - %s # %u handle(s) leaked",
                 (uint64_t)test_num, name_copy, (uint64_t)leaked);
    } else if (have_stem && leaked > 0) {
        /* Already FAIL/SKIP: just note the leak as extra context. */
        klog(LOG_WARN, "UTEST",
             "%s: %u handle(s) leaked (open at exit)",
             name_copy, (uint64_t)leaked);
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
    if (isolation_failed) {
        if (*out_verdict == 0) {
            *out_verdict = 1;
            counters[0]--;
            counters[1]++;
            klog(LOG_ERROR, "UTEST",
                 "%s: FAIL (isolation failed -- escalated from PASS)",
                 name_copy);
            if (s_tap_mode)
                klog(LOG_INFO, "UTEST",
                     "not ok %u - %s # isolation failed",
                     (uint64_t)test_num, name_copy);
        } else {
            klog(LOG_WARN, "UTEST",
                 "%s: isolation failed (scratch/registry state may persist)",
                 name_copy);
        }
    }
}

/* ---- Enumerate binaries via directory glob (fallback path) --------- */

struct glob_state {
    struct vfs_node *root;
    uint32_t         idx;
};

static const char *u_glob_next(struct glob_state *gs,
                               char *out_name, uint32_t out_cap)
{
    struct vfs_dirent *de;

    while ((de = vfs_readdir(gs->root, gs->idx)) != (struct vfs_dirent *)0) {
        gs->idx++;
        if (de->type & VFS_DIRECTORY)
            continue;
        if (!u_is_test_binary(de->name))
            continue;
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
    uint32_t              total_planned = 0;
    uint32_t              total_ran = 0;
    uint32_t              skipped_by_filter = 0;
    int                   use_manifest;
    char                  scratch_name[VFS_MAX_NAME];
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

    /* Enable preemptive scheduler for the timeout watchdog. Pair with
     * scheduler_disable before returning so boot_phase3 continues in
     * its expected non-preemptive state. */
    scheduler_enable();

    use_manifest = u_manifest_load(&manifest);

    /* First pass: count planned binaries (post-filter) so the TAP
     * plan line can emit `1..N` once, up-front. Both manifest and
     * glob paths apply the same filter. */
    if (use_manifest) {
        for (i = 0; i < manifest.count; i++) {
            if (test_usermode_glob_match(s_filter, manifest.names[i]))
                total_planned++;
        }
    }
    /* Always also include glob-discovered binaries when the manifest
     * was absent OR overflowed. If the manifest is authoritative (no
     * overflow) we skip the glob. */
    if (!use_manifest || manifest.overflowed) {
        struct glob_state gs = { root, 0 };
        while (u_glob_next(&gs, scratch_name, sizeof(scratch_name))) {
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
            if (test_usermode_glob_match(s_filter, scratch_name))
                total_planned++;
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
        u_manifest_free(&manifest);
        scheduler_disable();
        return;
    }

    /* Pre-flight task-slot budget check: task_create uses monotonic
     * pid = num_tasks++ and rejects once num_tasks == TASK_MAX. Slots
     * never come back today (owner: [scheduler enhancement TODO] §13).
     * Emitting `1..total_planned` when we know the tail would hit
     * task_create failures produces misleading TAP output. Clamp the
     * plan to the available budget, log a WARN naming the ceiling, and
     * let u_run_one surface the remaining binaries as task_create-fail
     * FAILs (deterministic, named, not silent). Codex quality H2,
     * 2026-04-20. */
    {
        uint32_t live = task_count();
        uint32_t budget = (live < TASK_MAX) ? (TASK_MAX - live) : 0u;
        if (budget < total_planned) {
            klog(LOG_WARN, "UTEST",
                 "plan %u exceeds free task slots %u (TASK_MAX=%u, live=%u) "
                 "-- tail will FAIL with task_create-failed until TODO-06 S13",
                 (uint64_t)total_planned, (uint64_t)budget,
                 (uint64_t)TASK_MAX, (uint64_t)live);
        }
    }

    /* TAP plan line (emitted once before any test) */
    if (s_tap_mode)
        klog(LOG_INFO, "UTEST", "1..%u", (uint64_t)total_planned);

    /* Second pass: actually run. Manifest order takes precedence so
     * tests can declare a deterministic execution order. */
    if (use_manifest) {
        for (i = 0; i < manifest.count; i++) {
            int verdict;
            const char *nm = manifest.names[i];
            if (!test_usermode_glob_match(s_filter, nm)) {
                skipped_by_filter++;
                klog(LOG_DEBUG, "UTEST", "%s: SKIP (filter)", nm);
                continue;
            }
            total_ran++;
            u_run_one(nm, total_ran, counters, &verdict);
        }
    }

    if (!use_manifest || manifest.overflowed) {
        struct glob_state gs = { root, 0 };
        while (u_glob_next(&gs, scratch_name, sizeof(scratch_name))) {
            int verdict;
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
                skipped_by_filter++;
                klog(LOG_DEBUG, "UTEST", "%s: SKIP (filter)", scratch_name);
                continue;
            }
            total_ran++;
            u_run_one(scratch_name, total_ran, counters, &verdict);
        }
    }

    u_manifest_free(&manifest);
    scheduler_disable();

    /* Summary. Counters: [0]=pass, [1]=fail, [2]=skip(exit=77). */
    if (skipped_by_filter > 0) {
        klog(LOG_INFO, "UTEST",
             "=== %u passed, %u failed, %u skipped of %u total "
             "(%u filtered) ===",
             (uint64_t)counters[0], (uint64_t)counters[1],
             (uint64_t)counters[2], (uint64_t)total_ran,
             (uint64_t)skipped_by_filter);
    } else {
        klog(LOG_INFO, "UTEST",
             "=== %u passed, %u failed, %u skipped of %u total ===",
             (uint64_t)counters[0], (uint64_t)counters[1],
             (uint64_t)counters[2], (uint64_t)total_ran);
    }
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
#endif
