/* ============================================================================
 * win32.c -- Minimal Win32 API shim for user-mode ELF test binaries
 *
 * See user/include/win32.h for the scope contract. This TU implements
 * the five functions the user-mode Win32 test binary probes plus the
 * infrastructure to reach them:
 *
 *   - `syscall_ssdt()` : inline wrapper around the x86-64 `syscall`
 *     instruction with the Windows kernel's 4-register ABI. Entry
 *     point is the kernel's `syscall_entry` (src/kernel/sched/
 *     syscall_entry.asm) which calls `ssdt_dispatch`. Args 5-6 are
 *     forced to 0 because the entry stub only forwards 4 registers
 *     today; consumers choose SSDT handlers that work under this
 *     constraint (e.g. NtOpenFile, not NtCreateFile).
 *
 *   - UNICODE_STRING / OBJECT_ATTRIBUTES / IO_STATUS_BLOCK local
 *     struct layouts matching the kernel's include/kernel/nt + ob
 *     definitions. User code cannot include those kernel headers,
 *     so the layouts are redeclared here.
 *
 *   - Minimal path-to-UNICODE conversion for CreateFileA (ASCII-only,
 *     bounded by a stack buffer; no heap, no locale tables). 260 is
 *     MAX_PATH in Win32; matches CLAUDE.md's Windows-style canonical
 *     path convention.
 *
 * NT service numbers are hand-coded constants (SSDT_NtOpenFile = 0x0011,
 * SSDT_NtReadFile = 0x0012, SSDT_NtClose = 0x0000). These live in
 * include/kernel/nt/service_numbers.h on the kernel side; a stable
 * INT-0x80-style ABI header shared between user and kernel belongs
 * under §2's Inputs once more Win32 thunks land. Hard-coding the
 * three numbers the shim needs keeps today's coupling explicit.
 * ============================================================================ */

#include "win32.h"

/* ---- NT service numbers (mirror of include/kernel/nt/service_numbers.h) -- */

#define SSDT_NtClose          0x0000
#define SSDT_NtOpenFile       0x0011
#define SSDT_NtReadFile       0x0012

/* ---- NT types (mirror of kernel headers) --------------------------------- */

typedef int32_t NTSTATUS;
#define STATUS_SUCCESS  ((NTSTATUS)0)
#define NT_SUCCESS(s)   (((NTSTATUS)(s)) >= 0)

/* UNICODE_STRING (byte-for-byte mirror of include/kernel/ob/peb.h).
 * Length is bytes, MaximumLength is buffer capacity in bytes, Buffer
 * points at WCHARs. The explicit _pad field forces Buffer to offset 8
 * on x64 -- do NOT rely on the compiler's natural alignment because a
 * different ABI target (e.g. IA-32 or ABI-compat builds) would shift
 * the pointer offset silently. */
typedef struct _UNICODE_STRING {
    uint16_t  Length;
    uint16_t  MaximumLength;
    uint32_t  _pad;
    uint16_t *Buffer;
} UNICODE_STRING;
_Static_assert(sizeof(UNICODE_STRING) == 16,
    "UNICODE_STRING must be 16 bytes to match kernel ABI");
_Static_assert(__builtin_offsetof(UNICODE_STRING, Buffer) == 8,
    "UNICODE_STRING.Buffer must be at offset 8");

/* OBJECT_ATTRIBUTES (byte-for-byte mirror of include/kernel/nt/nt_types.h).
 * Layout is kernel ABI and MUST NOT drift: the kernel reads ObjectName
 * from offset 16 and Attributes from offset 24; a user-side struct with
 * different offsets causes the kernel to dereference whatever random
 * bytes happen to land at those offsets -- Codex review 2026-04-22 H1
 * caught exactly this: my first pass used uint32_t Length without the
 * 64-bit width + _pad1 after RootDirectory, pushing ObjectName to
 * offset 8 instead of 16. The static asserts below prevent re-regression. */
typedef struct _OBJECT_ATTRIBUTES {
    uint64_t           Length;              /* offset 0  */
    HANDLE             RootDirectory;       /* offset 8  (int32_t) */
    uint32_t           _pad1;               /* offset 12 (align ObjectName) */
    UNICODE_STRING    *ObjectName;          /* offset 16 */
    uint32_t           Attributes;          /* offset 24 */
    uint32_t           _pad2;               /* offset 28 (align SecurityDescriptor) */
    void              *SecurityDescriptor;  /* offset 32 */
    void              *SecurityQualityOfService; /* offset 40 */
} OBJECT_ATTRIBUTES;
_Static_assert(sizeof(OBJECT_ATTRIBUTES) == 48,
    "OBJECT_ATTRIBUTES must be 48 bytes to match kernel ABI");
_Static_assert(__builtin_offsetof(OBJECT_ATTRIBUTES, ObjectName) == 16,
    "OBJECT_ATTRIBUTES.ObjectName must be at offset 16");
_Static_assert(__builtin_offsetof(OBJECT_ATTRIBUTES, Attributes) == 24,
    "OBJECT_ATTRIBUTES.Attributes must be at offset 24");
_Static_assert(__builtin_offsetof(OBJECT_ATTRIBUTES, SecurityDescriptor) == 32,
    "OBJECT_ATTRIBUTES.SecurityDescriptor must be at offset 32");

#define OBJ_CASE_INSENSITIVE  0x00000040u

/* IO_STATUS_BLOCK (mirror of include/kernel/nt/nt_types.h). The kernel
 * uses `NTSTATUS Status; uint32_t _pad; uint64_t Information;`. An 8-byte
 * union { NTSTATUS Status; void *Pointer; } here occupies the same 8
 * bytes (Status + _pad in the kernel layout) so the 16-byte total and
 * Information offset of 8 round-trip cleanly. */
typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        void    *Pointer;
    };
    uint64_t   Information;
} IO_STATUS_BLOCK;
_Static_assert(sizeof(IO_STATUS_BLOCK) == 16,
    "IO_STATUS_BLOCK must be 16 bytes to match kernel ABI");
_Static_assert(__builtin_offsetof(IO_STATUS_BLOCK, Information) == 8,
    "IO_STATUS_BLOCK.Information must be at offset 8");

/* ---- VFS access bits (mirror of include/kernel/fs/vfs.h) ----------------- *
 * ob_file_read / ob_file_write gate on these bits. Matches the
 * constants in user/test/test_fileio.c + user/test/test_syscall.c. */

#define VFS_O_READ   0x01u
#define VFS_O_WRITE  0x02u

/* ---- SYSCALL wrapper ---------------------------------------------------- *
 *
 * The kernel's syscall_entry.asm takes 4 register args after the service
 * number: R10=arg1, RDX=arg2, R8=arg3, R9=arg4. A 5th arg is hardcoded
 * to 0 inside the entry stub. The Windows x64 syscall ABI puts arg1 in
 * RCX at the call site; ntdll moves RCX to R10 before `syscall` because
 * `syscall` clobbers RCX with the user RIP. Our wrapper takes args in
 * C's SysV registers (RDI/RSI/RDX/RCX/R8/R9) and re-shuffles into the
 * Windows kernel ABI. We clobber everything the kernel's entry path
 * may touch (RCX, R11 per SYSCALL spec; R10 because we load it here). */

static inline NTSTATUS syscall_ssdt(uint32_t service,
                                    uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4)
{
    NTSTATUS ret;
    register uint64_t r10 __asm__("r10") = a1;
    register uint64_t r8  __asm__("r8")  = a3;
    register uint64_t r9  __asm__("r9")  = a4;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((uint64_t)service), "r"(r10), "d"(a2), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* ---- ASCII path -> UNICODE_STRING --------------------------------------- *
 *
 * Converts a narrow Win32 canonical path ("C:\\hello.txt") into the
 * UNICODE_STRING + WCHAR buffer shape NtOpenFile expects. The kernel's
 * oa_extract_path strips an optional "\\??\\" prefix and then walks
 * the Buffer as UTF-16. We ignore locale; every input byte < 0x80 maps
 * 1:1 to a UTF-16 code unit, which covers the full ASCII subset the
 * test fixture ever needs. `out_wchars` must have at least
 * `strlen(src) + 1` elements; the NUL terminator is written but the
 * UNICODE_STRING.Length field is bytes WITHOUT the NUL (Windows
 * convention). Returns -1 if the path is too long. */

static int ascii_to_unicode(const char *src, uint16_t *out_wchars,
                            uint32_t cap, UNICODE_STRING *out)
{
    uint32_t n = 0;
    while (src[n] != '\0') {
        if (n + 1 >= cap)
            return -1;
        out_wchars[n] = (uint16_t)(uint8_t)src[n];
        n++;
    }
    out_wchars[n] = 0;
    /* Length is bytes, NOT code units. `n` wide chars = `n * 2` bytes;
     * MaximumLength reserves one extra code unit for the NUL. */
    out->Length        = (uint16_t)(n * 2u);
    out->MaximumLength = (uint16_t)((n + 1u) * 2u);
    out->_pad          = 0;
    out->Buffer        = out_wchars;
    return 0;
}

/* ---- GetCurrentProcessId ------------------------------------------------ *
 *
 * TEB.ClientId.UniqueProcess sits at offset 0x40 within the 4 KiB TEB
 * page; user mode has %gs pointing at TEB after swapgs in the kernel's
 * ISR/SYSCALL exit. The value is written by the kernel's
 * teb_alloc_for_task() when the process was created. Returns 0 if the
 * TEB was not populated (shouldn't happen for a running user task).
 *
 * Reading TEB directly avoids a syscall for this "always-stable for the
 * life of a process" query, matching the Windows kernel32 fast path.
 * The `volatile` prevents the compiler from hoisting the read out of
 * any future loop that depends on it. */

DWORD GetCurrentProcessId(void)
{
    uint64_t pid;
    __asm__ volatile("movq %%gs:0x40, %0" : "=r"(pid));
    return (DWORD)pid;
}

/* ---- GetTickCount ------------------------------------------------------- *
 *
 * KUSER_SHARED_DATA lives at a fixed user-mode virtual address
 * 0x7FFE0000, mapped read-only by the kernel (kusd_time.c). Layout
 * matches Windows x64: offset 0x000 = TickCountLowDeprecated (u32),
 * offset 0x004 = TickCountMultiplier (u32). Standard Windows math:
 *
 *     ticks = ((uint64_t)TickCountLow * TickCountMultiplier) >> 24
 *
 * gives milliseconds since boot. TickCountMultiplier is pre-computed
 * by the kernel for the current tick resolution (10 ms today, so
 * TickCountMultiplier = 0x0FA00000, which makes this expression yield
 * milliseconds directly).
 *
 * Reading KUSD bypasses the syscall layer entirely. Note that the
 * kernel's TickCountLow ISR writer (timer_hal.c) is non-atomic on
 * 32-bit systems; on x64 the 32-bit write is a single instruction, so
 * there's no torn-read race. */

DWORD GetTickCount(void)
{
    volatile const uint32_t *kusd_lo  = (volatile const uint32_t *)0x7FFE0000ULL;
    volatile const uint32_t *kusd_mul = (volatile const uint32_t *)0x7FFE0004ULL;
    uint64_t ticks = (uint64_t)*kusd_lo * (uint64_t)*kusd_mul;
    return (DWORD)(ticks >> 24);
}

/* ---- CreateFileA -------------------------------------------------------- *
 *
 * Maps the Win32 narrow CreateFile to the NT API. The only disposition
 * routed through the current SYSCALL path is OPEN_EXISTING, which goes
 * to SSDT_NtOpenFile (a kernel-side alias that bakes FILE_OPEN into
 * the disposition and accepts 4 register args cleanly). Other Win32
 * dispositions (CREATE_NEW, CREATE_ALWAYS, OPEN_ALWAYS, TRUNCATE_EXISTING)
 * return INVALID_HANDLE_VALUE today -- they require the full 6-arg
 * NtCreateFile signature, and the kernel's syscall_entry.asm only
 * forwards 4 register args. Extending the SYSCALL path to read the 5th
 * and 6th Windows-ABI slots off the user stack (RSP+0x28, RSP+0x30)
 * is tracked as a user-mode test-framework follow-up item; once that
 * lands, every Win32 disposition routes straight to NtCreateFile.
 *
 * GenericMask -> VFS mapping happens here (not kernel-side) because
 * the kernel's Security Reference Monitor (RtlMapGenericMask +
 * SeAccessCheck) is deferred. Once SRM ships, this mapping migrates
 * into the kernel.
 *
 * The returned HANDLE is the kernel's int32_t slot index, zero-extended
 * to 64-bit and cast to HANDLE for Win32 compatibility. INVALID_HANDLE_VALUE
 * is the Win32 (HANDLE)-1 convention; the kernel uses -1 as its own
 * sentinel so the cast round-trips. */

/* MAX_PATH per Win32 convention; stack buffer bounded so a malicious
 * path cannot overflow. ObjectName.Buffer lives here for the lifetime
 * of the syscall (no async reads). */
#define WIN32_MAX_PATH  260

HANDLE CreateFileA(LPCSTR  lpFileName,
                   DWORD   dwDesiredAccess,
                   DWORD   dwShareMode,
                   LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                   DWORD   dwCreationDisposition,
                   DWORD   dwFlagsAndAttributes,
                   HANDLE  hTemplateFile)
{
    (void)lpSecurityAttributes; /* SRM-gated; ignored today */
    (void)dwFlagsAndAttributes; /* cosmetic until NTFS attrs land */
    (void)hTemplateFile;        /* only meaningful on CREATE_* paths */

    if (!lpFileName)
        return INVALID_HANDLE_VALUE;

    /* OPEN_EXISTING is the only disposition reachable via SSDT_NtOpenFile
     * under today's 4-arg SYSCALL path. Fail-fast rather than silently
     * open the wrong semantics for CREATE_NEW / CREATE_ALWAYS / OPEN_ALWAYS
     * / TRUNCATE_EXISTING callers. */
    if (dwCreationDisposition != OPEN_EXISTING)
        return INVALID_HANDLE_VALUE;

    /* Win32 GenericMask -> VFS bit translation. Bit 0x01=READ, 0x02=WRITE
     * per include/kernel/fs/vfs.h. GENERIC_EXECUTE has no VFS analogue
     * today (no mmap-exec in user-mode files); ignore silently.
     * GENERIC_ALL is the union. If no GENERIC_* bit is set, honour the
     * caller's raw mask (matches test_fileio.c's direct VFS_O_READ
     * pattern). */
    uint32_t vfs_access = 0;
    if (dwDesiredAccess & GENERIC_READ)    vfs_access |= VFS_O_READ;
    if (dwDesiredAccess & GENERIC_WRITE)   vfs_access |= VFS_O_WRITE;
    if (dwDesiredAccess & GENERIC_ALL)     vfs_access |= VFS_O_READ | VFS_O_WRITE;
    if (vfs_access == 0)
        vfs_access = dwDesiredAccess & (VFS_O_READ | VFS_O_WRITE);
    if (vfs_access == 0)
        return INVALID_HANDLE_VALUE;  /* zero-access open is not useful */

    /* Build the Windows-path -> UNICODE_STRING + OBJECT_ATTRIBUTES. */
    uint16_t         wbuf[WIN32_MAX_PATH];
    UNICODE_STRING   uname;
    OBJECT_ATTRIBUTES oa;
    if (ascii_to_unicode(lpFileName, wbuf, WIN32_MAX_PATH, &uname) < 0)
        return INVALID_HANDLE_VALUE;

    oa.Length                   = sizeof(oa);
    oa.RootDirectory            = NULL_HANDLE;
    oa._pad1                    = 0;
    oa.ObjectName               = &uname;
    oa.Attributes               = OBJ_CASE_INSENSITIVE;
    oa._pad2                    = 0;
    oa.SecurityDescriptor       = (void *)0;
    oa.SecurityQualityOfService = (void *)0;

    /* Call NtOpenFile via SYSCALL. Returns STATUS_SUCCESS and writes
     * the handle slot to *fh, or an NT error code with fh left as
     * INVALID_HANDLE_VALUE (-1). ShareAccess (a5) and OpenOptions (a6)
     * default to 0 on the 4-arg SYSCALL path -- benign for a read-only
     * open of an existing file when no other opener is contending. */
    HANDLE          fh = INVALID_HANDLE_VALUE;
    IO_STATUS_BLOCK iosb = { .Status = 0, .Information = 0 };
    NTSTATUS        rc =
        syscall_ssdt(SSDT_NtOpenFile,
                     (uint64_t)(uintptr_t)&fh,
                     (uint64_t)vfs_access,
                     (uint64_t)(uintptr_t)&oa,
                     (uint64_t)(uintptr_t)&iosb);

    (void)dwShareMode; /* Ignored until SYSCALL extension lands. */

    if (!NT_SUCCESS(rc))
        return INVALID_HANDLE_VALUE;
    return fh;
}

/* ---- ReadFile ----------------------------------------------------------- *
 *
 * Thin wrapper over NtReadFile. The 5th arg (ByteOffset*) defaults to
 * 0/NULL under the 4-arg SYSCALL path, which NtReadFile treats as "use
 * the file object's current offset" -- exactly the Windows
 * ReadFile-without-OVERLAPPED behaviour. IOSB.Information is copied to
 * *lpNumberOfBytesRead on success.
 *
 * lpOverlapped must be NULL: async Win32 ReadFile requires an event
 * handle + a completion port path the kernel doesn't have yet. The
 * shim fails fast rather than silently dropping the OVERLAPPED pointer. */

BOOL ReadFile(HANDLE       hFile,
              LPVOID       lpBuffer,
              DWORD        nNumberOfBytesToRead,
              DWORD       *lpNumberOfBytesRead,
              LPOVERLAPPED lpOverlapped)
{
    if (lpOverlapped != (LPOVERLAPPED)0)
        return FALSE;  /* async not supported */
    /* A 0-byte read request is Windows-legal and returns TRUE with
     * *lpNumberOfBytesRead = 0 even when lpBuffer is NULL. But a
     * positive-length read into a NULL buffer is a caller bug; return
     * FALSE so the error surfaces instead of being hidden behind a
     * synthetic EOF. Codex review 2026-04-22 M1 caught the collapsed
     * case that silently translated NULL + non-zero len into success. */
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

    IO_STATUS_BLOCK iosb = { .Status = 0, .Information = 0 };
    NTSTATUS rc = syscall_ssdt(SSDT_NtReadFile,
                               (uint64_t)(uintptr_t)hFile,
                               (uint64_t)(uintptr_t)&iosb,
                               (uint64_t)(uintptr_t)lpBuffer,
                               (uint64_t)nNumberOfBytesToRead);
    if (!NT_SUCCESS(rc)) {
        if (lpNumberOfBytesRead)
            *lpNumberOfBytesRead = 0;
        return FALSE;
    }
    if (lpNumberOfBytesRead)
        *lpNumberOfBytesRead = (DWORD)iosb.Information;
    return TRUE;
}

/* ---- CloseHandle -------------------------------------------------------- *
 *
 * Routes to NtClose via SYSCALL. NT_SUCCESS(STATUS_SUCCESS) -> TRUE;
 * any error (STATUS_INVALID_HANDLE on double-close, etc.) -> FALSE.
 * The raw int32_t handle value is sign-extended through the uintptr_t
 * cast so the kernel's handle-table lookup treats (HANDLE)-1 correctly
 * on the INVALID_HANDLE_VALUE path. */

BOOL CloseHandle(HANDLE hObject)
{
    NTSTATUS rc = syscall_ssdt(SSDT_NtClose,
                               (uint64_t)(uintptr_t)hObject,
                               0, 0, 0);
    return NT_SUCCESS(rc) ? TRUE : FALSE;
}
