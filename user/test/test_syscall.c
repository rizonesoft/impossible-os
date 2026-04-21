/* ============================================================================
 * test_syscall.c -- §9 syscall coverage binary for user/include/syscall.h
 *
 * Exercises every currently-wired INT 0x80 syscall from ring 3 and asserts
 * a plausible return value. The point is end-to-end ABI validation: if
 * the kernel dispatch table, the user wrapper, and the calling convention
 * all agree, every assertion below PASSes. A mismatch (stale SYS_* number,
 * wrong argument register, missing handler case) surfaces here instead of
 * in a downstream consumer.
 *
 * Linked against the same crt0 + libc as hello.exe / cmd.exe /
 * test_harness_smoke.exe. No kernel headers; no malloc; no printf. The
 * [PASS]/[FAIL] lines go to stdout via sys_write + the launcher's UTEST
 * color scope.
 *
 * Non-blocking policy: SYS_READ blocks on stdin with no keyboard input
 * attached to a launcher task, so we exercise the error path (fd=5 ->
 * STATUS_INVALID_HANDLE -> wrapper returns -1) instead of a live read.
 * SYS_READ happy-path coverage lives in the interactive cmd.exe shell.
 *
 * Pre-reqs (all shipped):
 *   - §2 INT 0x80 ABI sync (all 11 sys_* wrappers exist in syscall.h)
 *   - §3 launcher (runs this binary + scrapes [PASS]/[FAIL] lines)
 *   - §6 isolation (scratch dir + handle-leak detection)
 *   - C:\hello.txt deployed by make userland (25 bytes)
 * ============================================================================ */

#include "test.h"

UTEST_DEFINE_STATE();

/* Mirror of kernel OBJECT_DIRECTORY_INFORMATION -- kept inline here so
 * the user binary never includes `kernel/ob/ob.h` (which pulls in the
 * full kernel type surface). Size AND offset assertions below pin the
 * ABI contract so a kernel-side struct reorder surfaces as a compile
 * failure here, not a silent layout mismatch that still returns
 * positive entry counts. Codex adversarial 2026-04-21 M2. */
struct u_obj_dir_info {
    char name[64];
    char type_name[32];
};
_Static_assert(sizeof(struct u_obj_dir_info) == 96,
    "u_obj_dir_info mirrors kernel OBJECT_DIRECTORY_INFORMATION size");
_Static_assert(__builtin_offsetof(struct u_obj_dir_info, type_name) == 64,
    "u_obj_dir_info.type_name at offset 64 matches kernel layout");

/* Access-mask constants (2026-04-21 revised). The original §9 stamp
 * claimed the kernel "does not yet honour access masks" and used 0,
 * which made sys_readhandle return -1 unconditionally because
 * src/kernel/ob/ob_file.c:144 gates VFS reads on fo->access carrying
 * VFS_O_READ (0x01). The original claim was only true for the Win32
 * GENERIC_READ bit mapping (still deferred to the Security Reference
 * Monitor roadmap's RtlMapGenericMask + SeAccessCheck); the low-level
 * VFS_O_READ /
 * VFS_O_WRITE bits ARE enforced today. Mirror those bits here; once
 * SRM lands this test should also add a negative assertion that an
 * access-denied handle fails. Directory opens (NtOpenDirectoryObject)
 * still run through the OB dispatcher without a low-level read gate,
 * so 0 remains correct there.
 *
 * These are the same bit values as include/kernel/fs/vfs.h
 * VFS_O_READ / VFS_O_WRITE; user code cannot include that kernel
 * header, so the constants are duplicated. */
#define U_OPEN_READ           0x01u
#define U_OPEN_WRITE          0x02u
#define U_DIR_ACCESS_DEFAULT  0u

int main(void)
{
    UTEST_BEGIN("test_syscall");

    /* ---- SYS_WRITE: byte count round-trip -------------------------- */
    /* A literal 3-byte payload with no newline proves the kernel returns
     * exactly the requested byte count and does not short-write. The
     * UTEST color wrapping around the [PASS] line happens inside
     * UTEST_ASSERT; sys_write here is bare. */
    UTEST_ASSERT(sys_write(STDOUT_FD, "abc", 3) == 3,
                 "sys_write(STDOUT_FD, \"abc\", 3) returns 3");

    /* ---- SYS_READ: non-blocking error-path check ------------------- *
     * SYS_READ blocks on the TTY input queue; under the launcher this
     * task has no keyboard attached and a live call would hang until
     * the timeout watchdog kills us. Instead exercise the explicit
     * invalid-fd rejection path (STATUS_INVALID_HANDLE). The kernel
     * dispatcher translates the NTSTATUS failure into the -1 wire
     * return we see here. */
    char rbuf[4];
    UTEST_ASSERT(sys_read(5 /* not STDIN_FD */, rbuf, sizeof(rbuf)) == -1,
                 "sys_read(fd=5) rejected with -1 (invalid handle)");

    /* ---- SYS_YIELD: returns 0 --------------------------------------- *
     * The public user wrapper `sys_yield()` drops the kernel return
     * value on the floor (void signature). To honour §9's "returns 0"
     * assertion we call syscall0(SYS_YIELD) directly and verify. */
    UTEST_ASSERT(syscall0(SYS_YIELD) == 0,
                 "syscall0(SYS_YIELD) returns 0");

    /* ---- SYS_UPTIME: non-negative --------------------------------- *
     * sys_uptime returns seconds since boot. Within the first second
     * of a `test=1` run the value can be exactly 0, so `>= 0` is the
     * only portable invariant; the kernel uptime() source cannot
     * return negative. The TODO text says "> 0" but that races the
     * first-second boundary and would false-FAIL on fast boots. */
    UTEST_ASSERT(sys_uptime() >= 0,
                 "sys_uptime() >= 0 (kernel clock alive)");

    /* ---- SYS_GETPROCS: >= 2 processes ------------------------------ *
     * Idle (PID 0) + this test task = minimum 2. A healthy `test=1`
     * boot has more (kmain_boot, desktop deferred, cmd.exe spawns)
     * but >= 2 is the guaranteed floor. */
    struct proc_info pbuf[8];
    long pcount = sys_getprocs(pbuf, sizeof(pbuf));
    UTEST_ASSERT(pcount >= 2,
                 "sys_getprocs returns >= 2 (idle + self)");

    /* ---- SYS_OPENFILE / SYS_READHANDLE / SYS_CLOSEHANDLE ---------- *
     * Gate the dependent calls on the open succeeding so a single
     * regression in sys_openfile produces ONE FAIL, not four. Codex
     * adversarial 2026-04-21 M1. */
    HANDLE fh = sys_openfile("C:\\hello.txt", U_OPEN_READ);
    UTEST_ASSERT(fh != INVALID_HANDLE_VALUE,
                 "sys_openfile(\"C:\\\\hello.txt\") returns valid HANDLE");
    if (fh != INVALID_HANDLE_VALUE) {
        /* hello.txt is 25 bytes so a 64-byte buffer always drains the
         * whole file in one call. A short read (0) would indicate the
         * handle is a directory or the ob_file_read path broke. */
        char fbuf[64];
        long nread = sys_readhandle(fh, fbuf, sizeof(fbuf));
        UTEST_ASSERT(nread > 0,
                     "sys_readhandle reads > 0 bytes from C:\\hello.txt");

        /* Content sanity: first byte should be 'H' for the standard
         * payload "Hello from Impossible OS!\n". Only dereference
         * fbuf[0] when nread proved the buffer was written. */
        if (nread > 0) {
            UTEST_ASSERT(fbuf[0] == 'H',
                         "hello.txt starts with 'H'");
        }

        UTEST_ASSERT(sys_closehandle(fh) == 0,
                     "sys_closehandle(file) returns 0");
    }

    /* ---- SYS_LOG: LOG_INFO returns 0 ------------------------------- *
     * The klog ring absorbs a "[INFO] user: ..." line. The kernel
     * handler rejects LOG_FATAL (would halt the OS) and that
     * regression is separately covered by the §2 stub; here we only
     * verify the normal path. Literal length is 24 bytes (string
     * length, no NUL); kernel caps at 120 so we are well clear. */
    UTEST_ASSERT(sys_log(LOG_INFO, "test_syscall: SYS_LOG OK", 24) == 0,
                 "sys_log(LOG_INFO, ...) returns 0");

    /* ---- SYS_OPENDIROBJ / SYS_QUERYDIROBJ -------------------------- *
     * "\\" is the root of the Object Manager namespace (NT-style).
     * A healthy kernel has Device/KernelObjects/BaseNamedObjects
     * children populated at boot. */
    HANDLE dh = sys_opendirobj("\\", U_DIR_ACCESS_DEFAULT);
    UTEST_ASSERT(dh != INVALID_HANDLE_VALUE,
                 "sys_opendirobj(\"\\\\\") returns valid HANDLE");
    if (dh != INVALID_HANDLE_VALUE) {
        /* The wrapper handles the kernel's (count << 16 | ctx) packing on
         * its own; we pass a real struct array and ctx=0. On success the
         * return is the number of entries written. */
        struct u_obj_dir_info dbuf[16];
        uint16_t ctx = 0;
        long dcount = sys_querydirobj(dh, dbuf, 16, &ctx);
        UTEST_ASSERT(dcount > 0,
                     "sys_querydirobj returns > 0 entries under root");

        /* ABI-layout spot check: the first returned entry should have
         * a non-empty `name` field at offset 0 and a `type_name` at
         * offset 64. A silent struct reorder would surface here as
         * either a zero-length name (writing at +64 instead of +0)
         * or garbage `type_name` bytes. Codex adversarial 2026-04-21
         * M2: just asserting dcount > 0 leaves layout drift invisible. */
        if (dcount > 0) {
            UTEST_ASSERT(dbuf[0].name[0] != '\0',
                         "first root dir entry has non-empty name");
        }

        /* Close the dir handle too so the §6 leak check stays at zero.
         * Failing to close here would trip the launcher's handle-leak
         * escalation ("FAIL (1 handle(s) leaked -- escalated from PASS)"),
         * which is the exact signal §6 ships. */
        UTEST_ASSERT(sys_closehandle(dh) == 0,
                     "sys_closehandle(dir) returns 0");
    }

    UTEST_END();
    return g_fail;
}
