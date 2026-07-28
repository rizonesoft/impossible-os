/* ============================================================================
 * shell_loader.c -- cmd.exe task-entry loader
 *
 * PRODUCTION code (see shell_loader.h for why this is not test-only). Moved
 * out of test_threads.c 2026-07-17 (release-flavor test-surface exclusion): that file is
 * genuinely test-only content pruned entirely under KERNEL_TESTS=off, and
 * this function was the release flavor's only path to a shell -- both
 * boot_desktop.c and desktop.c reached it via a bare `extern` at the call
 * site, bypassing any header, so the misfiling was invisible at compile time.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/klog.h"
#include "kernel/main/shell_loader.h"
#include "kernel/sched/task.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/user_range.h"
#include "kernel/fs/vfs.h"

void shell_loader_func(void)
{
    struct vfs_node *file;
    uint8_t *buf;
    uint64_t fsize;
    int nread;

    if (!vfs_is_mounted('C')) {
        klog(LOG_WARN, "cmd", "C:\\ not mounted");
        return;
    }

    file = vfs_open("C:\\cmd.exe", VFS_O_READ);
    if (!file) {
        klog(LOG_WARN, "cmd", "cmd.exe not found on C:\\");
        return;
    }

    /* Snapshot the size ONCE: file->size lives on a vfs_node a concurrent
     * writer can grow between the allocation, the read, and the exec call.
     * Every use below reads this local, never file->size again. Reject
     * empty files and anything that cannot fit the user ELF load window --
     * a file wider than that can never load correctly regardless of how
     * it is staged.
     *
     * Allocation stays on the kmalloc heap for the FULL range (not tiered
     * to PMM above 4 KiB): pmm_alloc_contiguous()/pmm_free_frame() mutate
     * an SMP-unlocked bitmap (include/kernel/mm/pmm.h "CALLER CONTRACT")
     * and this function runs as a scheduled task with no boot barrier, so
     * calling them here would be a real bitmap race, not just a heap-tier
     * style violation. kmalloc/kfree are spinlock-protected (heap.c), so
     * this stays correctness-safe; the heap-capacity tradeoff is a real
     * but lesser concern than the SMP bug the tiered version introduced,
     * and its actual fix (a locked runtime large-buffer allocator) is
     * owned by TODO-03-advanced-allocator.md. */
    fsize = file->size;
    if (fsize == 0 || fsize > USER_ELF_SIZE) {
        klog(LOG_WARN, "cmd", "cmd.exe size %u out of range (1..%u)",
             fsize, (uint64_t)USER_ELF_SIZE);
        vfs_close(file);
        return;
    }

    buf = (uint8_t *)kmalloc(fsize);
    if (!buf) {
        klog(LOG_ERROR, "cmd", "cannot allocate buffer");
        vfs_close(file);
        return;
    }

    nread = vfs_read(file, 0, (uint32_t)fsize, buf);
    vfs_close(file);
    if (nread < 0 || (uint64_t)nread != fsize) {
        klog(LOG_ERROR, "cmd", "short read (%d of %u bytes)",
             (int64_t)nread, fsize);
        kfree(buf);
        return;
    }

    /* This function runs as an ordinary scheduled kernel task with interrupts
     * ENABLED, so it is the path the staging-release race actually bites: on a
     * successful exec, publication can hand the task to the new ring-3 image
     * before task_exec returns, and a kfree placed after the call would never
     * run. The ownership token moves the success-path release inside task_exec,
     * just before publication; the call below then frees only on the failure
     * returns, where this task is still alive to do it. */
    {
        struct task_exec_staging st = { task_exec_staging_kfree, buf, 0, 0 };
        int erc = task_exec(buf, fsize, &st);
        task_exec_staging_release(&st);   /* no-op when task_exec released */
        if (erc < 0) {
            klog(LOG_ERROR, "cmd", "exec failed");
            /* Past task_exec's commit point the shell image is gone; this task
             * must not continue as if it still had one. */
            if (erc == TASK_EXEC_IMAGE_DESTROYED)
                task_exit(TASK_EXIT_EXEC_IMAGE_DESTROYED);  /* no return */
            return;
        }
    }

    for (;;)
        __asm__ volatile("hlt");
}
