/* ============================================================================
 * test_ipc.c -- §11 user-mode IPC coverage binary
 *
 * Exercises the two ring-3 IPC channels the kernel exposes today:
 *   - SYS_PIPE         -- anonymous pipe pair; write one end, read the other
 *   - SYS_SHMEM_CREATE -- anonymous shared-memory section
 *   - SYS_SHMEM_MAP    -- map the section into this task's address space
 *
 * Each assertion guards its dependents so one broken syscall produces one
 * FAIL instead of a cascade (same discipline as §9 test_syscall). Handle
 * cleanup runs on every path so the §6 launcher isolation check stays at
 * zero leaked handles (leak -> PASS escalates to FAIL with "1 handle(s)
 * leaked" in the launcher summary).
 *
 * Non-blocking policy: the pipe write fits in one kernel-buffer frame
 * (kernel pipe ring is 4 KiB today), so sys_readhandle on the read end
 * returns immediately after the write. No fork is used -- cross-task
 * pipe IPC is §12's job.
 *
 * Linked against the same crt0 + libc as every other user binary.
 * ============================================================================ */

#include "test.h"

UTEST_DEFINE_STATE();

int main(void)
{
    UTEST_BEGIN("test_ipc");

    /* ---- SYS_PIPE: anonymous pair + write/read round-trip -------- *
     * sys_pipe wrapper fills fds[0]=read, fds[1]=write. Returns 0 on
     * success, -1 if the kernel ran out of handle slots or the
     * ob_create_pipe_handles allocator failed. */
    HANDLE pipe_fds[2];
    pipe_fds[0] = INVALID_HANDLE_VALUE;
    pipe_fds[1] = INVALID_HANDLE_VALUE;
    UTEST_ASSERT(sys_pipe(pipe_fds) == 0,
                 "sys_pipe returns 0");
    UTEST_ASSERT(pipe_fds[0] != INVALID_HANDLE_VALUE &&
                 pipe_fds[1] != INVALID_HANDLE_VALUE,
                 "sys_pipe fills both handle slots");

    if (pipe_fds[0] != INVALID_HANDLE_VALUE &&
        pipe_fds[1] != INVALID_HANDLE_VALUE) {
        /* Short payload that fits in a single kernel ring write so the
         * subsequent read does not block on empty. The magic suffix
         * `-OK` proves we read the whole payload, not a prefix. */
        const char msg[] = "PIPE-OK";
        long n_written = sys_writehandle(pipe_fds[1], msg, sizeof(msg) - 1);
        UTEST_ASSERT(n_written == (long)(sizeof(msg) - 1),
                     "sys_writehandle(pipe write-end) returns full byte count");

        /* Read back through the read-end handle. A short read here
         * (n_read < n_written) would indicate the kernel pipe ring
         * dropped bytes -- not expected for a payload well under 4 KiB. */
        char rbuf[16];
        long n_read = sys_readhandle(pipe_fds[0], rbuf, sizeof(rbuf));
        UTEST_ASSERT(n_read == n_written,
                     "sys_readhandle(pipe read-end) returns written byte count");

        /* Byte-equality check proves the ring kept order + content
         * intact across the write -> read transition. */
        if (n_read >= (long)(sizeof(msg) - 1)) {
            UTEST_ASSERT(rbuf[0] == 'P' && rbuf[1] == 'I' && rbuf[2] == 'P' &&
                         rbuf[3] == 'E' && rbuf[4] == '-' && rbuf[5] == 'O' &&
                         rbuf[6] == 'K',
                         "pipe round-trip preserves \"PIPE-OK\" byte-for-byte");
        }

        /* Cleanup BOTH handles regardless of the round-trip outcome
         * so the launcher's leak check stays at zero. */
        UTEST_ASSERT(sys_closehandle(pipe_fds[0]) == 0,
                     "sys_closehandle(pipe read end) returns 0");
        UTEST_ASSERT(sys_closehandle(pipe_fds[1]) == 0,
                     "sys_closehandle(pipe write end) returns 0");
    }

    /* ---- SYS_SHMEM_CREATE: anonymous 4 KiB section ---------------- *
     * NULL name = anonymous section (unnamed in the OB namespace).
     * The kernel ObCreateSection allocates PMM frames backing the
     * section and returns a HANDLE scoped to this task's handle
     * table. Size is rounded up to page granularity internally. */
    HANDLE shm = sys_shmem_create((const char *)0, 4096);
    UTEST_ASSERT(shm != INVALID_HANDLE_VALUE,
                 "sys_shmem_create(NULL, 4096) returns valid HANDLE");

    if (shm != INVALID_HANDLE_VALUE) {
        /* ---- SYS_SHMEM_MAP: map into this task -------------------- *
         * ObMapViewOfSection installs PTEs in the caller's address
         * space. Returns the user VA where the section landed, or 0
         * on failure. The returned address is always page-aligned. */
        uintptr_t mapped = sys_shmem_map(shm);
        UTEST_ASSERT(mapped != 0,
                     "sys_shmem_map returns non-zero address");

        if (mapped != 0) {
            /* ---- Write + verify through the mapping -------------- *
             * Dereferencing the mapped VA should hit the shmem-backed
             * PMM frame. A round-trip via the pointer proves both the
             * map-side PTE install and the faulting read path work.
             * 32-bit writes are the simplest marker; endianness is
             * unambiguous for the byte-pattern check below. */
            volatile uint32_t *p = (volatile uint32_t *)mapped;
            p[0] = 0xDEADBEEFu;
            p[1] = 0xCAFEF00Du;
            UTEST_ASSERT(p[0] == 0xDEADBEEFu,
                         "shmem readback: p[0] == 0xDEADBEEF");
            UTEST_ASSERT(p[1] == 0xCAFEF00Du,
                         "shmem readback: p[1] == 0xCAFEF00D");

            /* Byte-granularity check via a char pointer proves no
             * word-level byte-swap happened during the round-trip. */
            volatile unsigned char *bp = (volatile unsigned char *)mapped;
            UTEST_ASSERT(bp[0] == 0xEFu && bp[1] == 0xBEu &&
                         bp[2] == 0xADu && bp[3] == 0xDEu,
                         "shmem readback: little-endian byte order preserved");

            /* Explicit unmap BEFORE closehandle. ObMapViewOfSectionFull
             * takes an extra ObReferenceObject pin that sys_closehandle
             * does NOT drop -- without sys_unmapview the section stays
             * pinned across process exit (task_cleanup does not walk
             * section views). Codex adversarial 2026-04-21 M2. */
            UTEST_ASSERT(sys_unmapview(mapped) == 0,
                         "sys_unmapview releases the view pin");
        }

        /* Close the section handle -- drops the handle-table entry.
         * Must happen AFTER sys_unmapview so the final Dereference
         * (from handle close) reclaims the SECTION_OBJECT backing. */
        UTEST_ASSERT(sys_closehandle(shm) == 0,
                     "sys_closehandle(shmem) returns 0");
    }

    UTEST_END();
    return g_fail;
}
