/* ============================================================================
 * test_fileio.c -- user-mode file I/O coverage binary
 *
 * Exercises the handle-based file-I/O surface from ring 3:
 *   sys_openfile  -- happy path + nonexistent-path error path
 *   sys_readhandle -- full content match (not just first-byte sniff)
 *   sys_closehandle -- followed by a post-close read that MUST fail
 *   sys_opendirobj + sys_querydirobj -- enumerate the OB root
 *
 * Scope vs: test_syscall.exe smoke-tests one [PASS] per syscall
 * along a representative happy path. pushes further -- it asserts
 *   (a) the read buffer EQUALS the canonical "Hello from Impossible OS!"
 *       payload (length + byte-for-byte), catching any partial-read or
 *       truncation regression that "starts with 'H'" would miss; and
 *   (b) sys_readhandle on a just-closed HANDLE returns -1, catching the
 *       failure mode where ob_close_handle drops the slot but some stale
 *       ref keeps the kernel file object reachable.
 *
 * Why this lives in its own binary instead of piggy-backing on
 * test_syscall.c:'s item "Open nonexistent file -> INVALID_HANDLE_VALUE"
 * is a deliberate NEGATIVE path test, and is already large. Keeping
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
 * in the syscall ABI header, not here -- this is the per-test
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

/* Mirror of the kernel's OBJECT_DIRECTORY_INFORMATION row shape
 * (name[64] + type_name[32] = 96 bytes). Re-asserted here so
 * test_fileio.exe is self-sufficient: a kernel-side layout drift
 * surfaces in this binary alone, not only in test_syscall.exe. The
 * review Codex 2026-04-21 M2 caught the false-confidence risk of
 * relying on for layout guarantees this binary depends on. */
struct u_obj_dir_info {
    char name[64];
    char type_name[32];
};
_Static_assert(sizeof(struct u_obj_dir_info) == 96,
    "u_obj_dir_info mirrors kernel OBJECT_DIRECTORY_INFORMATION size");
_Static_assert(__builtin_offsetof(struct u_obj_dir_info, type_name) == 64,
    "u_obj_dir_info.type_name at offset 64 matches kernel layout");

int main(void)
{
    UTEST_BEGIN("test_fileio");

    /* ---- 1. Happy path: open + read + content match + close ------- *
     * Single unified sub-test. Each assertion is gated on the prior
     * one succeeding so a regression in sys_openfile produces ONE
     * FAIL (the open) instead of cascading through read + compare +
     * close with false failures. Same pattern the Codex adversarial
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
         * surfaces the bug. -1 is the sole success.
         *
         * Fill buf with a sentinel byte pattern BEFORE the post-close
         * read so a failure that still writes to caller memory (stale
         * ref -> type-confused copy) is caught -- not just the return
         * value. Review Codex 2026-04-21 M1 required this hardening:
         * "after_close < 0" alone passes even if the kernel corrupts
         * user memory along the way. */
        const uint8_t SENTINEL = 0xA5u;
        {
            uint32_t i;
            for (i = 0; i < FILE_BUF_SIZE; i++)
                ((uint8_t *)buf)[i] = SENTINEL;
        }
        long after_close = sys_readhandle(fh, buf, FILE_BUF_SIZE);
        UTEST_ASSERT(after_close < 0,
                     "sys_readhandle on closed handle returns < 0");

        /* Buffer integrity: every byte must still be the sentinel.
         * A single differing byte proves the kernel wrote to caller
         * memory on the failure path -- a correctness bug regardless
         * of the return code. */
        {
            int clean = 1;
            uint32_t i;
            for (i = 0; i < FILE_BUF_SIZE; i++) {
                if (((uint8_t *)buf)[i] != SENTINEL) {
                    clean = 0;
                    break;
                }
            }
            UTEST_ASSERT(clean == 1,
                         "failed sys_readhandle leaves caller buffer unchanged");
        }
    }

    /* ---- 2b. Negative access coverage: write-only open rejects reads -- *
     * Codex review 2026-04-21 H1: the happy-path probe only proves
     * that bit 0x01 (U_OPEN_READ) opens hello.txt readably. Without
     * exercising the opposite mode, an access-gate regression that
     * accepts ANY non-zero mask (or flips VFS_O_WRITE into VFS_O_READ)
     * would still pass.
     *
     * Review Codex 2026-04-21 M2 tightened this further: the probe
     * MUST NOT silently degrade to a no-op if the open ever starts
     * failing. IXFS is read-write today and vfs_open accepts a
     * WRITE-only handle on any file, so the open is expected to
     * succeed unconditionally. If a future share-mode / ACL / policy
     * change makes write-only open fail, that MUST surface as a FAIL
     * here so the operator updates this fixture -- not as a silent
     * loss of read-reject coverage. The subsequent read-reject
     * assertion then proves ob_file.c:144's `!(fo->access & VFS_O_READ)`
     * branch is still honoured. */
    HANDLE wh = sys_openfile("C:\\hello.txt", U_OPEN_WRITE);
    UTEST_ASSERT(wh != INVALID_HANDLE_VALUE,
                 "sys_openfile(\"C:\\\\hello.txt\", WRITE-only) succeeds "
                 "(if this FAILs, update the test fixture -- open policy changed)");
    if (wh != INVALID_HANDLE_VALUE) {
        char wbuf[16];
        long wread = sys_readhandle(wh, wbuf, sizeof(wbuf));
        UTEST_ASSERT(wread < 0,
                     "sys_readhandle on WRITE-only handle returns < 0");
        UTEST_ASSERT(sys_closehandle(wh) == 0,
                     "sys_closehandle(write-only handle) returns 0");
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

    /* ---- 3b. BlackBox round-trip: read X:\Diag\blackbox-marker.txt --- *
     * Proves the full FAT32 BlackBox stack works end-to-end from
     * user-mode: BPB validate (after dirty-mount fsck), partition
     * mount as X:\, walk_path through dir-cache, sys_openfile + read
     * across the VFS handle layer. Marker file is built into the
     * FAT32 image at make-system-disk time (Makefile mcopy line);
     * if any kernel layer regresses (the 0xF8 BPB validator bug
     * pattern fixed in commit d96b76fc, fat32 dir-cache walk_path
     * lookups, drive-letter routing, vfs_o_read on FAT32), this
     * probe fails first.
     *
     * Marker payload: literal 11 bytes "BlackBox-v1", no trailing
     * NUL or newline. Update both this constant AND the Makefile
     * `printf "BlackBox-v1"` line in lockstep -- the printf does
     * not emit a trailing newline, so the on-disk size is exactly
     * sizeof(MARKER_EXPECTED)-1. */
    #define MARKER_EXPECTED     "BlackBox-v1"
    #define MARKER_EXPECTED_LEN (sizeof(MARKER_EXPECTED) - 1)

    HANDLE mh = sys_openfile("X:\\Diag\\blackbox-marker.txt", U_OPEN_READ);
    UTEST_ASSERT(mh != INVALID_HANDLE_VALUE,
                 "sys_openfile(\"X:\\\\Diag\\\\blackbox-marker.txt\") returns valid HANDLE");

    if (mh != INVALID_HANDLE_VALUE) {
        char mbuf[32];
        long mread = sys_readhandle(mh, mbuf, sizeof(mbuf));
        UTEST_ASSERT(mread == (long)MARKER_EXPECTED_LEN,
                     "sys_readhandle returns 11 bytes (marker length)");

        if (mread == (long)MARKER_EXPECTED_LEN) {
            int match = 1;
            size_t i;
            for (i = 0; i < MARKER_EXPECTED_LEN; i++) {
                if (mbuf[i] != MARKER_EXPECTED[i]) {
                    match = 0;
                    break;
                }
            }
            UTEST_ASSERT(match == 1,
                         "marker content matches 'BlackBox-v1' (round-trip OK)");
        }

        UTEST_ASSERT(sys_closehandle(mh) == 0,
                     "sys_closehandle(marker) returns 0");
    }

    /* ---- 4. Directory-object enumeration --------------------------- *
     * "\\" is the OB namespace root. At boot-time the kernel populates
     * Device / KernelObjects / BaseNamedObjects / RPC Control, so a
     * healthy enumeration returns >= 1 entry and the first entry's
     * name is non-empty. Full ABI layout is asserted in's
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
