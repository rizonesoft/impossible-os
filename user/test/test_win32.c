/* ============================================================================
 * test_win32.c -- user-mode Win32 API coverage binary
 *
 * Exercises the Win32 shim (user/lib/win32.c) from ring 3:
 *
 *   1. GetCurrentProcessId    -- returns our PID (> 0; the idle task
 *                                at PID 0 never runs user code).
 *   2. CreateFileA(GENERIC_READ, OPEN_EXISTING)
 *                             -- opens C:\hello.txt via NtOpenFile
 *                                through the SYSCALL fast path.
 *   3. ReadFile               -- drains hello.txt (25 bytes) into a
 *                                stack buffer; byte-for-byte content
 *                                match against the canonical payload.
 *   4. CloseHandle            -- returns TRUE; double-close returns
 *                                FALSE (invalidation probe, mirrors
 *'s post-close test at the Win32
 *                                layer).
 *   5. GetTickCount           -- returns monotonically non-decreasing
 *                                millisecond count from
 *                                KUSER_SHARED_DATA at 0x7FFE0000.
 *                                Two back-to-back reads must satisfy
 *                                t2 >= t1 (KUSD writer is 32-bit
 *                                atomic on x64 so no torn reads).
 *
 * This is Impossible OS's first user-mode probe of the Win32 API
 * surface -- it proves the CLAUDE.md "Win32 native" orientation works
 * end-to-end at the source level WITHOUT the PE32+ dynamic linker
 *, because user/lib/win32.c is linked statically with the ELF
 * test binary.
 *
 * Scope boundary: is the Win32-level sibling of (raw INT 0x80
 * probe) and (handle-based file I/O). Async ReadFile, CreateProcess,
 * VirtualAlloc, registry APIs, WaitForSingleObject, and the full
 * Unicode-W variants are NOT in scope -- they belong either with
 * (PE32+ dynamic linker) or with their respective kernel32 feature
 * TODOs.
 * ============================================================================ */

#include "test.h"
#include "win32.h"

UTEST_DEFINE_STATE();

/* Canonical hello.txt payload. Mirror of test_fileio.c / the Makefile
 * `userland:` rule. `sizeof - 1` strips the compiler-added NUL so the
 * literal byte count matches what the kernel actually stores. */
#define HELLO_EXPECTED     "Hello from Impossible OS!"
#define HELLO_EXPECTED_LEN (sizeof(HELLO_EXPECTED) - 1)

/* Bounded 64-byte buffer: 2x the 25-byte payload so an over-read
 * regression fits without stack-frame corruption. */
#define FILE_BUF_SIZE 64

int main(void)
{
    UTEST_BEGIN("test_win32");

    /* ---- 1. GetCurrentProcessId ----------------------------------- *
     * Our PID must be strictly > 0 because PID 0 is the kernel idle
     * task (never runs user code). A return of 0 would indicate the
     * TEB.ClientId.UniqueProcess field was never populated, which
     * would point at a task_exec / teb_alloc_for_task regression. */
    DWORD pid = GetCurrentProcessId();
    UTEST_ASSERT(pid > 0,
                 "GetCurrentProcessId() returns PID > 0");

    /* ---- 2. CreateFileA(GENERIC_READ, OPEN_EXISTING) --------------- *
     * Opens hello.txt via NtOpenFile through the SYSCALL path. Gating
     * all dependent assertions on a valid handle produces exactly one
     * FAIL on regression (the open) rather than cascading through
     * read/content-match/close/double-close with false failures. */
    HANDLE fh = CreateFileA("C:\\hello.txt",
                            GENERIC_READ,
                            FILE_SHARE_READ,
                            (LPSECURITY_ATTRIBUTES)0,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL,
                            (HANDLE)0);
    UTEST_ASSERT(fh != INVALID_HANDLE_VALUE,
                 "CreateFileA(\"C:\\\\hello.txt\", GENERIC_READ, OPEN_EXISTING) "
                 "returns valid HANDLE");

    if (fh != INVALID_HANDLE_VALUE) {
        /* ---- 3. ReadFile ---------------------------------------- *
         * hello.txt is 25 bytes so a 64-byte request returns exactly
         * HELLO_EXPECTED_LEN in lpNumberOfBytesRead. A short read
         * (N < 25) would indicate the file shrank or NtReadFile
         * truncated; the content-match assertion below catches the
         * latter. */
        char buf[FILE_BUF_SIZE];
        DWORD nread = 0;
        BOOL ok = ReadFile(fh, buf, FILE_BUF_SIZE, &nread, (LPOVERLAPPED)0);
        UTEST_ASSERT(ok == TRUE,
                     "ReadFile(hello.txt, 64 bytes) returns TRUE");
        UTEST_ASSERT(nread == (DWORD)HELLO_EXPECTED_LEN,
                     "ReadFile reports 25 bytes read (hello.txt length)");

        /* Byte-for-byte match gated on the length check -- never
         * read past `nread` into uninitialized stack memory. */
        if (ok == TRUE && nread == (DWORD)HELLO_EXPECTED_LEN) {
            int match = 1;
            DWORD i;
            for (i = 0; i < nread; i++) {
                if (buf[i] != HELLO_EXPECTED[i]) {
                    match = 0;
                    break;
                }
            }
            UTEST_ASSERT(match == 1,
                         "hello.txt content matches 'Hello from Impossible OS!'");
        }

        /* ---- 3b. EOF probe: second read drains to EOF ---------- *
         * NtReadFile returns STATUS_END_OF_FILE when offset >= file
         * size. Win32 ReadFile must translate that to a SUCCESSFUL
         * 0-byte read (TRUE + nread=0) -- callers reserve FALSE for
         * genuine I/O errors. A regression that collapsed EOF into
         * FALSE would silently break any sequential reader that
         * uses read-until-zero as its termination condition.
         * Review Codex 2026-04-22 M2. */
        char eof_buf[FILE_BUF_SIZE];
        DWORD eof_read = 999;  /* sentinel to detect unset */
        BOOL eof_ok = ReadFile(fh, eof_buf, FILE_BUF_SIZE, &eof_read,
                               (LPOVERLAPPED)0);
        UTEST_ASSERT(eof_ok == TRUE,
                     "ReadFile at EOF returns TRUE (not FALSE)");
        UTEST_ASSERT(eof_read == 0,
                     "ReadFile at EOF sets *lpNumberOfBytesRead to 0");

        /* ---- 4. CloseHandle + double-close --------------------- *
         * First close must succeed (TRUE). Second close on the same
         * stale handle must fail (FALSE) because ob_close_handle
         * freed the slot and the handle-table lookup now rejects
         * the int32_t index. Mirrors the post-close invalidation
         * probe at the Win32 API layer. */
        UTEST_ASSERT(CloseHandle(fh) == TRUE,
                     "CloseHandle(file) returns TRUE");
        UTEST_ASSERT(CloseHandle(fh) == FALSE,
                     "CloseHandle on already-closed handle returns FALSE");
    }

    /* ---- 4b. CloseHandle sentinel rejects --------------------- *
     * Win32 convention: CloseHandle((HANDLE)NULL) and
     * CloseHandle(INVALID_HANDLE_VALUE) both return FALSE. The
     * NULL-handle guard prevents accidentally closing slot 0 (the
     * kernel's handle-table indexes 0 as a valid slot); the
     * INVALID_HANDLE_VALUE guard catches the classic
     * uninitialized-sentinel mistake. Review Codex 2026-04-22 H1. */
    UTEST_ASSERT(CloseHandle((HANDLE)0) == FALSE,
                 "CloseHandle((HANDLE)NULL) returns FALSE");
    UTEST_ASSERT(CloseHandle(INVALID_HANDLE_VALUE) == FALSE,
                 "CloseHandle(INVALID_HANDLE_VALUE) returns FALSE");

    /* ---- 5. GetTickCount + monotonicity ---------------------------- *
     * KUSER_SHARED_DATA->TickCountLowDeprecated starts at 0 and only
     * increments (timer ISR writes a monotone sequence). A single
     * read > 0 requires the kernel to have ticked at least once since
     * boot, which is trivially true by the time the launcher gets a
     * user task running (boot is ~2 seconds on KVM).
     *
     * Monotonicity check: two back-to-back reads must satisfy t2 >= t1.
     * The 32-bit TickCountLow writer in timer_hal.c is atomic on x64
     * (single STOR instruction), so there is no torn-read race to
     * tolerate. */
    DWORD t1 = GetTickCount();
    UTEST_ASSERT(t1 > 0,
                 "GetTickCount() returns > 0 (kernel timer alive)");
    DWORD t2 = GetTickCount();
    UTEST_ASSERT(t2 >= t1,
                 "GetTickCount() is monotonically non-decreasing");

    UTEST_END();
    return g_fail;
}
