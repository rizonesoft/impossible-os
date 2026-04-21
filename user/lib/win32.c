/* ============================================================================
 * win32.c -- Minimal Win32 API shim for user-mode ELF test binaries
 *
 * See user/include/win32.h for the scope contract. This TU implements
 * the five Win32 functions the user-mode Win32 test binary probes by
 * routing EVERY call through the existing INT 0x80 syscall surface
 * (sys_getpid, sys_openfile, sys_readhandle, sys_closehandle, sys_uptime):
 *
 *   - GetCurrentProcessId -> sys_getpid            (INT 0x80, SYS_GETPID=18)
 *   - GetTickCount        -> sys_uptime() * 1000   (INT 0x80, SYS_UPTIME=12)
 *   - CreateFileA         -> sys_openfile          (INT 0x80, SYS_OPENFILE=39)
 *   - ReadFile            -> sys_readhandle        (INT 0x80, SYS_READHANDLE=41)
 *   - CloseHandle         -> sys_closehandle       (INT 0x80, SYS_CLOSEHANDLE=40)
 *
 * Design rationale (2026-04-22 revision): the earlier version tried
 * three Windows-native mechanisms -- gs:0x40 TEB reads, KUSD direct
 * mapping at 0x7FFE0000, and the SYSCALL instruction into
 * ssdt_dispatch -- but none had been exercised by a prior user
 * binary. All three paths crashed silently on WHPX. The INT 0x80
 * surface has been proven end-to-end by test_syscall.exe,
 * test_libc.exe, test_ipc.exe, test_process.exe, and test_fileio.exe.
 * Routing every Win32 call through the same proven dispatch keeps
 * the shim honest -- the Win32 API CONTRACT is what §14 probes, not
 * the underlying transport.
 *
 * When the SYSCALL fast path + KUSD + gs:0x40 are later verified by
 * dedicated probes (owner TODO item in §2's Inputs), this shim can
 * migrate to the fast path without changing its public API. Until
 * then, the slight per-call overhead of INT 0x80 is the right trade
 * for "every Win32 caller actually works."
 *
 * Win32 GenericMask (GENERIC_READ / GENERIC_WRITE / GENERIC_ALL) is
 * translated client-side into the low-level VFS_O_READ / VFS_O_WRITE
 * bits the kernel's ob_file_read / ob_file_write gate on -- same
 * pattern as user/test/test_fileio.c + user/test/test_syscall.c.
 * When the Security Reference Monitor (RtlMapGenericMask +
 * SeAccessCheck) lands, this translation migrates into the kernel
 * and the shim becomes a pure syscall wrapper.
 * ============================================================================ */

#include "win32.h"
#include "syscall.h"

/* VFS access bits -- match include/kernel/fs/vfs.h. ob_file_read /
 * ob_file_write gate on these in the kernel; duplicated in
 * user/test/test_fileio.c and user/test/test_syscall.c. A shared
 * user/kernel ABI header is an open follow-up item on the user-mode
 * test framework roadmap. */
#define VFS_O_READ   0x01u
#define VFS_O_WRITE  0x02u

/* ---- GetCurrentProcessId ------------------------------------------------ *
 *
 * Routes through SYS_GETPID (INT 0x80, service 18). PID 0 is the kernel
 * idle task which never runs user code, so the returned value is always
 * >= 1 for a live user process. The syscall is stateless and has no
 * failure mode -- it simply reads task_current()->pid. */

DWORD GetCurrentProcessId(void)
{
    long pid = sys_getpid();
    return (DWORD)(pid < 0 ? 0 : pid);
}

/* ---- GetTickCount ------------------------------------------------------- *
 *
 * Returns milliseconds since boot. sys_uptime returns SECONDS (coarse,
 * timer-tick resolution). Multiplying by 1000 yields millisecond
 * granularity -- coarser than Windows' KUSD-backed GetTickCount (which
 * reads TickCountLow at ~10 ms resolution) but monotonically non-
 * decreasing, which is the contract user code expects.
 *
 * A future user-mode KUSD read (direct load from 0x7FFE0000) can swap
 * in transparently once verified. Until then, the INT 0x80 path gives
 * us a known-working monotonic clock. */

DWORD GetTickCount(void)
{
    long secs = sys_uptime();
    if (secs < 0)
        return 0;
    return (DWORD)(secs * 1000);
}

/* ---- CreateFileA -------------------------------------------------------- *
 *
 * Maps the Win32 narrow CreateFile to sys_openfile (INT 0x80, SYS_OPENFILE).
 * Only OPEN_EXISTING is routed -- every other Win32 disposition returns
 * INVALID_HANDLE_VALUE. sys_openfile takes a raw access mask that
 * ob_create_file_handle forwards to vfs_open; the read/write gate later
 * checks VFS_O_READ / VFS_O_WRITE bits in ob_file_read / ob_file_write.
 *
 * GenericMask -> VFS bit translation:
 *   GENERIC_READ  -> VFS_O_READ
 *   GENERIC_WRITE -> VFS_O_WRITE
 *   GENERIC_ALL   -> VFS_O_READ | VFS_O_WRITE
 *   raw VFS_O_*   -> passed through unchanged
 *
 * Zero-access opens (no GENERIC_* AND no VFS bits set) return
 * INVALID_HANDLE_VALUE because such a handle would be useless and
 * masks a caller bug. */

HANDLE CreateFileA(LPCSTR  lpFileName,
                   DWORD   dwDesiredAccess,
                   DWORD   dwShareMode,
                   LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                   DWORD   dwCreationDisposition,
                   DWORD   dwFlagsAndAttributes,
                   HANDLE  hTemplateFile)
{
    (void)dwShareMode;
    (void)lpSecurityAttributes;
    (void)dwFlagsAndAttributes;
    (void)hTemplateFile;

    if (!lpFileName)
        return INVALID_HANDLE_VALUE;
    if (dwCreationDisposition != OPEN_EXISTING)
        return INVALID_HANDLE_VALUE;

    uint32_t vfs_access = 0;
    if (dwDesiredAccess & GENERIC_READ)   vfs_access |= VFS_O_READ;
    if (dwDesiredAccess & GENERIC_WRITE)  vfs_access |= VFS_O_WRITE;
    if (dwDesiredAccess & GENERIC_ALL)    vfs_access |= VFS_O_READ | VFS_O_WRITE;
    if (vfs_access == 0)
        vfs_access = dwDesiredAccess & (VFS_O_READ | VFS_O_WRITE);
    if (vfs_access == 0)
        return INVALID_HANDLE_VALUE;

    HANDLE h = sys_openfile(lpFileName, vfs_access);
    /* sys_openfile returns INVALID_HANDLE_VALUE (-1) on failure -- the
     * same sentinel Win32 callers expect. Pass through as-is. */
    return h;
}

/* ---- ReadFile ----------------------------------------------------------- *
 *
 * Thin wrapper over sys_readhandle (INT 0x80, SYS_READHANDLE). Returns
 * TRUE on success with the byte count in *lpNumberOfBytesRead; FALSE
 * on failure.
 *
 * Win32 edge cases:
 *   - lpOverlapped != NULL  -> FALSE (async not supported)
 *   - nNumberOfBytesToRead == 0 -> TRUE + *NumberOfBytesRead = 0
 *     (legal per Win32 spec regardless of buffer validity)
 *   - lpBuffer == NULL + non-zero length -> FALSE (caller bug)
 *
 * EOF handling: sys_readhandle returns 0 at EOF (matching POSIX
 * read-until-zero semantics on the VFS read path). That naturally
 * maps to Win32's "ReadFile succeeds with 0 bytes at EOF" -- callers
 * use FALSE for real I/O errors, not end-of-stream. */

BOOL ReadFile(HANDLE       hFile,
              LPVOID       lpBuffer,
              DWORD        nNumberOfBytesToRead,
              DWORD       *lpNumberOfBytesRead,
              LPOVERLAPPED lpOverlapped)
{
    if (lpOverlapped != (LPOVERLAPPED)0)
        return FALSE;
    if (nNumberOfBytesToRead == 0) {
        if (lpNumberOfBytesRead)
            *lpNumberOfBytesRead = 0;
        return TRUE;
    }
    if (!lpBuffer) {
        if (lpNumberOfBytesRead)
            *lpNumberOfBytesRead = 0;
        return FALSE;
    }

    long n = sys_readhandle(hFile, lpBuffer, nNumberOfBytesToRead);
    if (n < 0) {
        if (lpNumberOfBytesRead)
            *lpNumberOfBytesRead = 0;
        return FALSE;
    }
    if (lpNumberOfBytesRead)
        *lpNumberOfBytesRead = (DWORD)n;
    return TRUE;
}

/* ---- CloseHandle -------------------------------------------------------- *
 *
 * Routes to sys_closehandle (INT 0x80, SYS_CLOSEHANDLE). Sentinel
 * guards at the Win32 boundary:
 *   - (HANDLE)NULL              -> FALSE (Windows convention)
 *   - INVALID_HANDLE_VALUE (-1) -> FALSE (Windows convention)
 *
 * NULL rejection is safe because the kernel's ObpAllocateHandle
 * reserves slot 0 (returns handle value 4 at minimum), so (HANDLE)0
 * can never be a valid handle -- the reservation was added in
 * src/kernel/ob/handle_table.c when this shim's CloseHandle(NULL)
 * semantics became load-bearing. Without slot 0 reserved, the guard
 * here would reject a legitimate handle that happened to be 0. */

BOOL CloseHandle(HANDLE hObject)
{
    if (hObject == NULL_HANDLE || hObject == INVALID_HANDLE_VALUE)
        return FALSE;
    long rc = sys_closehandle(hObject);
    return (rc == 0) ? TRUE : FALSE;
}
