/* ============================================================================
 * test_usermode.c -- Kernel-side launcher for user-mode test binaries
 *
 * §3 of TODO-04. See include/kernel/test/test_usermode.h for the API
 * contract; this TU implements scan + spawn + wait + report.
 *
 * Sequencing: the launcher is single-threaded by design. Binaries run
 * one at a time so a leaked file handle, dirty Registry key, or stuck
 * process from binary N cannot perturb binary N+1's run. Per-test
 * isolation hardening (per-test scratch dir, per-binary handle-leak
 * detection) is owned by §6; this section just gets the basic
 * sequence right.
 *
 * The path-passing trick:
 *   task_create(loader_func, name) launches a kernel task that runs
 *   loader_func once. The loader function reads `s_pending_test_path`
 *   (a file-scope volatile pointer the launcher set just before the
 *   task_create call), opens the file, kmalloc's a buffer, and then
 *   calls task_exec(buf, size) to morph the kernel task into a user
 *   task running the binary. Same pattern as exec_loader_func in
 *   src/kernel/main/test_threads.c. Single-threaded launch + waitpid
 *   means there is no race on s_pending_test_path.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/klog.h"
#include "kernel/mm/heap.h"
#include "kernel/fs/vfs.h"
#include "kernel/sched/task.h"
#include "kernel/test/test_usermode.h"

/* ---- Internal state -------------------------------------------------- */

/* Path of the binary the next utest_loader_func() invocation will load.
 * Set by test_usermode_run() immediately before each task_create call;
 * read by utest_loader_func() once the new task is scheduled in.
 * Volatile because the loader runs in a different scheduling slot. */
static volatile const char *s_pending_test_path;

/* Filter set by §4 (when it lands); NULL means "run every test_*.exe". */
static const char *s_filter;

void test_usermode_set_filter(const char *filter)
{
    s_filter = (filter && filter[0]) ? filter : (const char *)0;
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
 * actual full POSIX implementation is overkill -- the §4 spec calls
 * for "literal name or `*`-glob" and that's exactly what tests need
 * (`test_smoke_*.exe` etc.). NULL pattern matches everything. */
static int u_glob_match(const char *pattern, const char *name)
{
    if (!pattern)
        return 1;
    /* Locate the wildcard, if any. */
    const char *star = pattern;
    while (*star && *star != '*') star++;
    if (!*star) {
        /* No wildcard -- literal compare. */
        const char *p = pattern, *n = name;
        while (*p && *n && *p == *n) { p++; n++; }
        return *p == 0 && *n == 0;
    }
    /* Pattern is "<prefix>*<suffix>". Both halves must match. */
    uint32_t prefix_len = (uint32_t)(star - pattern);
    const char *suffix = star + 1;
    uint32_t name_len = 0;
    while (name[name_len]) name_len++;
    if (name_len < prefix_len)
        return 0;
    if (u_strncmp(pattern, name, prefix_len) != 0)
        return 0;
    /* Suffix must match the END of name (after the prefix). */
    uint32_t suffix_len = 0;
    while (suffix[suffix_len]) suffix_len++;
    if (suffix_len > name_len - prefix_len)
        return 0;
    return u_strncmp(name + (name_len - suffix_len), suffix, suffix_len + 1) == 0;
}

static int u_is_test_binary(const char *name)
{
    /* `test_*.exe` -- prefix `test_`, suffix `.exe`. Excludes the
     * production hello.exe / cmd.exe / etc. */
    if (!u_starts_with(name, "test_"))
        return 0;
    if (!u_ends_with(name, ".exe"))
        return 0;
    return 1;
}

/* ---- Task entry: the loader that morphs into the test binary ---------
 *
 * Failure paths use task_exit(STATUS) -- a plain `return` from a kernel
 * task only sets TASK_DEAD but never wakes the parent's task_waitpid
 * (Codex H1, 2026-04-20). task_exit is the canonical wakeup path.
 * Status codes:
 *   -1: NULL pending path (launcher bug)
 *   -2: vfs_open failed
 *   -3: kmalloc failed
 *   -4: vfs_read short / size mismatch
 *   -5: task_exec failed
 * The launcher renders any negative exit code as `[UTEST] <name>: FAIL
 * (exit=N)` so loader failures surface even though the binary itself
 * never produced output.
 * --------------------------------------------------------------------- */

static void utest_loader_func(void)
{
    /* Snapshot the path immediately so a future launcher invocation
     * cannot race us if it set s_pending_test_path again before we
     * consumed it. (Today the launcher waits in task_waitpid for us,
     * but defending against future API changes is cheap.) */
    const char *path = (const char *)s_pending_test_path;

    if (!path || !path[0]) {
        klog(LOG_ERROR, "UTEST", "loader: NULL pending path");
        task_exit(-1);
    }

    struct vfs_node *file = vfs_open(path, VFS_O_READ);
    if (!file) {
        klog(LOG_ERROR, "UTEST", "%s: vfs_open failed", path);
        task_exit(-2);
    }

    uint32_t size = file->size;
    uint8_t *buf = (uint8_t *)kmalloc(size);
    if (!buf) {
        klog(LOG_ERROR, "UTEST", "%s: kmalloc(%u) failed", path, (uint64_t)size);
        vfs_close(file);
        task_exit(-3);
    }

    int n = vfs_read(file, 0, size, buf);
    vfs_close(file);
    if (n <= 0 || (uint32_t)n != size) {
        klog(LOG_ERROR, "UTEST", "%s: vfs_read short (n=%d size=%u)",
             path, (int64_t)n, (uint64_t)size);
        kfree(buf);
        task_exit(-4);
    }

    /* task_exec stages an iretq frame for user mode (consumed on the
     * next scheduling switch). It DOES NOT take ownership of `buf` --
     * exec_load() inside copies the binary into user pages, so once
     * task_exec returns 0 the staging buffer is no longer needed.
     * Free it now (Codex M1, 2026-04-20) before the for(;;)hlt;
     * yields control to the prepared user-mode frame; otherwise every
     * passing test leaks its full file size from the kernel heap. */
    int rc = task_exec(buf, size);
    if (rc < 0) {
        klog(LOG_ERROR, "UTEST", "%s: task_exec failed", path);
        kfree(buf);
        task_exit(-5);
    }
    kfree(buf);

    /* Unreachable in practice; task_exec iretq's into user mode on the
     * next scheduling tick. The hlt loop is just a defensive yield in
     * case scheduling is ever delayed. */
    for (;;)
        __asm__ volatile("hlt");
}

/* ---- Public entry: scan + run + report ------------------------------- */

void test_usermode_run(void)
{
    if (!vfs_is_mounted('C')) {
        klog(LOG_DEBUG, "UTEST", "C:\\ not mounted -- skipping user-mode tests");
        return;
    }

    struct vfs_node *root = vfs_get_drive_root('C');
    if (!root || !root->ops || !root->ops->readdir) {
        klog(LOG_DEBUG, "UTEST", "C:\\ root has no readdir -- skipping");
        return;
    }

    uint32_t passed = 0;
    uint32_t failed = 0;
    uint32_t total  = 0;
    uint32_t skipped = 0;

    /* Scan the C:\ root directory. The launcher does NOT recurse into
     * subdirectories -- by convention all test_*.exe binaries deploy
     * to the root (see Makefile userland target). */
    uint32_t idx = 0;
    struct vfs_dirent *de;
    while ((de = vfs_readdir(root, idx)) != (struct vfs_dirent *)0) {
        idx++;
        if (de->type & VFS_DIRECTORY)
            continue;
        if (!u_is_test_binary(de->name))
            continue;

        /* Apply §4 filter (today: s_filter is always NULL until §4
         * ships the boot.conf utest_filter parser; the helper handles
         * NULL by matching everything). */
        if (!u_glob_match(s_filter, de->name)) {
            skipped++;
            klog(LOG_DEBUG, "UTEST", "%s: SKIP (filter)", de->name);
            continue;
        }

        total++;

        /* Snapshot de->name into a launcher-owned stable buffer
         * BEFORE spawning the child. VFS readdir implementations
         * return shared/static dirent storage (IXFS uses vol->dirent;
         * NTFS a static buffer; FAT32 likewise) -- the user-mode
         * test we're about to spawn could call SYS_READDIR /
         * NtQueryDirectoryFile, overwrite the shared dirent, and
         * make us log PASS/FAIL against the wrong filename + park
         * tasks[pid].name pointing at garbage. (Codex M2, 2026-04-20.) */
        char name_copy[VFS_MAX_NAME];
        uint32_t ni;
        for (ni = 0; de->name[ni] && ni < sizeof(name_copy) - 1; ni++)
            name_copy[ni] = de->name[ni];
        name_copy[ni] = '\0';

        /* Build the C:\<name> path inline. Path + name_copy must outlive
         * the spawned task because they stay in s_pending_test_path /
         * tasks[pid].name until the task's loader consumes them and the
         * launcher logs the result. Single-threaded launcher waits in
         * task_waitpid before next iteration, so both buffers' lifetimes
         * span one spawn/wait cycle. */
        char path[VFS_MAX_NAME + 4];
        path[0] = 'C'; path[1] = ':'; path[2] = '\\';
        uint32_t pi = 3;
        for (ni = 0; name_copy[ni] && pi < sizeof(path) - 1; ni++)
            path[pi++] = name_copy[ni];
        path[pi] = '\0';

        s_pending_test_path = path;

        int pid = task_create(utest_loader_func, name_copy);
        if (pid < 0) {
            klog(LOG_ERROR, "UTEST", "%s: task_create failed", name_copy);
            failed++;
            continue;
        }

        int32_t exit_status = task_waitpid((uint32_t)pid);
        if (exit_status == 0) {
            passed++;
            klog(LOG_INFO, "UTEST", "%s: PASS (exit=0)", name_copy);
        } else {
            failed++;
            klog(LOG_ERROR, "UTEST", "%s: FAIL (exit=%d)",
                 name_copy, (int64_t)exit_status);
        }
    }

    if (total == 0 && skipped == 0) {
        klog(LOG_DEBUG, "UTEST",
             "no test_*.exe found at C:\\ -- skipping summary");
        return;
    }

    if (skipped > 0) {
        klog(LOG_INFO, "UTEST",
             "=== %u passed, %u failed of %u total (%u skipped by filter) ===",
             (uint64_t)passed, (uint64_t)failed,
             (uint64_t)total, (uint64_t)skipped);
    } else {
        klog(LOG_INFO, "UTEST",
             "=== %u passed, %u failed of %u total ===",
             (uint64_t)passed, (uint64_t)failed, (uint64_t)total);
    }
}
