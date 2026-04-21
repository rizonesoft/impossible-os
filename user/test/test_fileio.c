/* ============================================================================
 * test_fileio.c -- §13 user-mode file I/O coverage binary
 *
 * Exercises the handle-based file-I/O surface from ring 3:
 *   sys_openfile  -- happy path + nonexistent-path error path
 *   sys_readhandle -- full content match (not just first-byte sniff)
 *   sys_closehandle -- followed by a post-close read that MUST fail
 *   sys_opendirobj + sys_querydirobj -- enumerate the OB root
 *
 * Scope vs §9: test_syscall.exe smoke-tests one [PASS] per syscall
 * along a representative happy path. §13 pushes further -- it asserts
 *   (a) the read buffer EQUALS the canonical "Hello from Impossible OS!"
 *       payload (length + byte-for-byte), catching any partial-read or
 *       truncation regression that "starts with 'H'" would miss; and
 *   (b) sys_readhandle on a just-closed HANDLE returns -1, catching the
 *       failure mode where ob_close_handle drops the slot but some stale
 *       ref keeps the kernel file object reachable.
 *
 * Why this lives in its own binary instead of piggy-backing on
 * test_syscall.c: §13's item "Open nonexistent file -> INVALID_HANDLE_VALUE"
 * is a deliberate NEGATIVE path test, and §9 is already large. Keeping
 * it separate lets the launcher timeline show file-I/O regressions
 * distinct from general-syscall regressions.
 *
 * hello.txt content: the 25-byte literal "Hello from Impossible OS!"
 * (no trailing newline). Written by the Makefile `userland:` rule at
 * `echo -n "Hello from Impossible OS!" > $(SYSROOT)/hello.txt`, which
 * then ships to `C:\hello.txt` via the IXFS sysroot. If that literal
 * ever changes, update HELLO_EXPECTED + HELLO_EXPECTED_LEN below.
 * ============================================================================ */

#include "test.h"

UTEST_DEFINE_STATE();

/* Access-mask mirror of the kernel's VFS_O_READ (0x01) +
 * VFS_O_WRITE (0x02) defined in include/kernel/fs/vfs.h. User-mode
 * code cannot include kernel headers, so the bits are duplicated
 * here with a compile-time check that they stay in sync with the
 * kernel -- ob_file.c:144 gates sys_readhandle on fo->access carrying
 * VFS_O_READ, and sys_openfile(path, 0) produces a handle that the
 * read path rejects with -1. Any future change to VFS_O_READ must
 * update this constant too (the kernel + user values are compared
 * in the §2 syscall ABI header, not here -- this is the per-test
 * mirror so the probe's intent is explicit). A `U_OPEN_READ`
 * style zero-value token is deliberately avoided: "default" is
 * ambiguous now that access is enforced, and a zero mask is not
 * what any real caller wants. */
#define U_OPEN_READ   0x01u
#define U_OPEN_WRITE  0x02u

/* Directory objects go through ob_create_dir_handle / NtOpenDirectoryObject
 * rather than the VFS open path, so the access mask is opaque to the
 * read-gate check and 0 is still the current "no explicit rights"
 * value. Kept as a separate constant so a future SRM-aware dir-access
 * enforcement only touches this one line. */
#define U_DIR_ACCESS_DEFAULT 0u

/* Mirror of the Makefile userland-rule literal written to C:\hello.txt.
 * 25 bytes, no NUL, no newline. sizeof-1 shaves the compiler's implicit
 * NUL -- we want the literal byte count, not a string length that an
 * overzealous optimizer could confuse with strlen(). */
#define HELLO_EXPECTED     "Hello from Impossible OS!"
#define HELLO_EXPECTED_LEN (sizeof(HELLO_EXPECTED) - 1)

/* Bounded buffer for sys_readhandle. Sized to 2x the expected payload
 * so a silent regression that over-reads (returns > expected length)
 * still fits and is caught by the length check instead of corrupting
 * the stack frame. */
#define FILE_BUF_SIZE 64

/* Opaque struct mirroring the kernel's OBJECT_DIRECTORY_INFORMATION
 * row shape (name[64] + type_name[32] = 96 bytes). Only used as an
 * opaque byte region for §13's enumeration probe -- we just confirm
 * the kernel wrote non-zero bytes into the first entry's name field.
 * The §9 test_syscall.exe already asserts layout offsets; there is
 * no reason to re-assert them here. */
struct u_obj_dir_info {
    char name[64];
    char type_name[32];
};

int main(void)
{
    UTEST_BEGIN("test_fileio");

    /* ---- 1. Happy path: open + read + content match + close ------- *
     * Single unified sub-test. Each assertion is gated on the prior
     * one succeeding so a regression in sys_openfile produces ONE
     * FAIL (the open) instead of cascading through read + compare +
     * close with false failures. Same pattern the §9 Codex adversarial
     * round required (M1). */
    HANDLE fh = sys_openfile("C:\\hello.txt", U_OPEN_READ);
    UTEST_ASSERT(fh != INVALID_HANDLE_VALUE,
                 "sys_openfile(\"C:\\\\hello.txt\") returns valid HANDLE");

    if (fh != INVALID_HANDLE_VALUE) {
        char buf[FILE_BUF_SIZE];

        /* Full-buffer read. hello.txt is 25 bytes; FILE_BUF_SIZE=64
         * so a healthy read returns exactly HELLO_EXPECTED_LEN. A
         * short read (N < 25) would indicate the file shrank or the
         * ob_file_read path truncated -- the exact-match assertion
         * catches it. */
        long nread = sys_readhandle(fh, buf, FILE_BUF_SIZE);
        UTEST_ASSERT(nread == (long)HELLO_EXPECTED_LEN,
                     "sys_readhandle returns 25 bytes (hello.txt length)");

        /* Byte-for-byte match. strncmp over the full expected length
         * so any single-byte drift (encoding flip, partial fragment,
         * BOM insertion, stale cached page from a different file) is
         * caught. Gated on the length check so we never read past
         * nread into uninitialized stack memory. */
        if (nread == (long)HELLO_EXPECTED_LEN) {
            int match = 1;
            size_t i;
            for (i = 0; i < HELLO_EXPECTED_LEN; i++) {
                if (buf[i] != HELLO_EXPECTED[i]) {
                    match = 0;
                    break;
                }
            }
            UTEST_ASSERT(match == 1,
                         "hello.txt content matches 'Hello from Impossible OS!'");
        }

        /* Close the handle. Return code 0 is the only accepted success
         * per ob_close_handle (< 0 means bad handle or already closed). */
        UTEST_ASSERT(sys_closehandle(fh) == 0,
                     "sys_closehandle(file) returns 0");

        /* ---- 2. Post-close invalidity: read on closed handle fails ---- *
         * After ob_close_handle drops the slot, the HANDLE value is stale
         * and ob_reference_object_by_handle should reject it. If the
         * read path silently re-resolves a freed kernel object (or a
         * slot got reused without us noticing), this returns >= 0 and
         * surfaces the bug. -1 is the sole success. */
        long after_close = sys_readhandle(fh, buf, FILE_BUF_SIZE);
        UTEST_ASSERT(after_close < 0,
                     "sys_readhandle on closed handle returns < 0");
    }

    /* ---- 3. Error path: open nonexistent file ---------------------- *
     * A path that cannot resolve in the VFS MUST come back as
     * INVALID_HANDLE_VALUE (-1). A regression where the OB layer
     * allocates a slot before the VFS lookup completes would return
     * a positive handle here -- the operator could then sys_readhandle
     * a zombie handle. "This_file_definitely_does_not_exist.bin" is
     * long enough to make a collision with any future sysroot literal
     * extremely unlikely. */
    HANDLE missing = sys_openfile("C:\\this_file_definitely_does_not_exist.bin",
                                  U_OPEN_READ);
    UTEST_ASSERT(missing == INVALID_HANDLE_VALUE,
                 "sys_openfile(nonexistent) returns INVALID_HANDLE_VALUE");

    /* ---- 4. Directory-object enumeration --------------------------- *
     * "\\" is the OB namespace root. At boot-time the kernel populates
     * Device / KernelObjects / BaseNamedObjects / RPC Control, so a
     * healthy enumeration returns >= 1 entry and the first entry's
     * name is non-empty. Full ABI layout is asserted in §9's
     * test_syscall; this is a functional enumeration smoke test. */
    HANDLE dh = sys_opendirobj("\\", U_DIR_ACCESS_DEFAULT);
    UTEST_ASSERT(dh != INVALID_HANDLE_VALUE,
                 "sys_opendirobj(\"\\\\\") returns valid HANDLE");

    if (dh != INVALID_HANDLE_VALUE) {
        struct u_obj_dir_info dbuf[8];
        uint16_t ctx = 0;
        long dcount = sys_querydirobj(dh, dbuf, 8, &ctx);
        UTEST_ASSERT(dcount > 0,
                     "sys_querydirobj returns > 0 entries under OB root");

        /* Non-empty name check: a broken kernel that wrote at the
         * wrong struct offset would leave name[0] as '\0'. Gated on
         * dcount so we never peek into uninitialized dbuf. */
        if (dcount > 0) {
            UTEST_ASSERT(dbuf[0].name[0] != '\0',
                         "first OB root entry has non-empty name");
        }

        UTEST_ASSERT(sys_closehandle(dh) == 0,
                     "sys_closehandle(dir) returns 0");
    }

    UTEST_END();
    return g_fail;
}
