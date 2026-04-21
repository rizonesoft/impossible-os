/* ============================================================================
 * win32.h -- Minimal Win32 API surface for user-mode ELF test binaries
 *
 * Thin statically-linked shim that lets a user ELF call a handful of
 * Win32 functions without going through the PE32+/kernel32.dll/DLL-import
 * chain. Proves the Win32 API semantics at the source level on top of
 * the existing NT syscall dispatch (SYSCALL/SYSRET fast path into
 * `ssdt_dispatch`) plus the KUSER_SHARED_DATA / TEB direct-read paths.
 *
 * Scope today:
 *   - GetCurrentProcessId  -- reads TEB.ClientId.UniqueProcess (gs:0x40)
 *   - GetTickCount         -- reads KUSER_SHARED_DATA @ 0x7FFE0000
 *   - CreateFileA          -- routes to NtOpenFile via SYSCALL for
 *                             OPEN_EXISTING; other dispositions fail
 *                             fast (see scope note in win32.c).
 *   - ReadFile             -- routes to NtReadFile via SYSCALL
 *   - CloseHandle          -- routes to NtClose via SYSCALL
 *
 * NOT in scope: the full Win32 surface (CreateProcess, WaitForSingleObject,
 * VirtualAlloc, registry APIs, overlapped I/O, security descriptors,
 * Unicode-W variants, GDI, USER32). This is a test-framework shim, not
 * a kernel32 replacement. A real kernel32 needs the PE32+ dynamic linker
 * and the full Security Reference Monitor so that GENERIC_READ maps to
 * the right per-object-type mask at the OS layer instead of in the shim.
 *
 * The header intentionally defines only the Win32-idiomatic types the
 * five functions above need. If a future test binary wants more, it
 * either extends this header or picks up the full PE32+ path once the
 * dynamic linker lands.
 * ============================================================================ */

#pragma once

#include "types.h"
/* HANDLE + INVALID_HANDLE_VALUE come from syscall.h (int32_t slot index,
 * -1 sentinel). syscall.h is the authoritative user-mode ABI header;
 * Win32 HANDLE round-trips through the same int32_t so a Win32 caller
 * and an INT-0x80 caller see the same kernel handle slot. If this TU
 * is included without syscall.h first (rare -- test.h always pulls it
 * in), drop in a forward declaration so prototypes still compile. */
#ifndef INVALID_HANDLE_VALUE
#include "syscall.h"
#endif

/* ---- Basic Win32 types -------------------------------------------------- */

typedef int32_t         BOOL;
typedef uint8_t         BYTE;
typedef uint16_t        WORD;
typedef uint32_t        DWORD;
typedef uint64_t        DWORD64;
typedef int32_t         LONG;
typedef int64_t         LONGLONG;
typedef uint64_t        ULONGLONG;
typedef void           *LPVOID;
typedef const void     *LPCVOID;
typedef const char     *LPCSTR;
typedef char           *LPSTR;

#define TRUE            1
#define FALSE           0
#define NULL_HANDLE     ((HANDLE)0)

/* ---- CreateFile constants ---------------------------------------------- *
 *
 * Win32 standard values reproduced here so user code looks idiomatic.
 * The client-side GenericMask -> VFS mapping happens inside CreateFileA
 * because the kernel's Security Reference Monitor (RtlMapGenericMask +
 * SeAccessCheck) does not translate GENERIC_* bits yet. Once SRM lands,
 * this mapping migrates into the kernel and the shim becomes a pure
 * syscall wrapper. */

#define GENERIC_READ            0x80000000u
#define GENERIC_WRITE           0x40000000u
#define GENERIC_EXECUTE         0x20000000u
#define GENERIC_ALL             0x10000000u

#define FILE_SHARE_READ         0x00000001u
#define FILE_SHARE_WRITE        0x00000002u
#define FILE_SHARE_DELETE       0x00000004u

/* CreateDisposition values (Win32 layer; translated to NT FILE_* values
 * inside CreateFileA). OPEN_EXISTING is the only one reachable via the
 * current 4-arg SYSCALL path; see win32.c's scope-gap note. */
#define CREATE_NEW              1  /* fail if exists -- NT FILE_CREATE */
#define CREATE_ALWAYS           2  /* truncate or create -- NT FILE_OVERWRITE_IF */
#define OPEN_EXISTING           3  /* must exist -- NT FILE_OPEN (supported today) */
#define OPEN_ALWAYS             4  /* open or create -- NT FILE_OPEN_IF */
#define TRUNCATE_EXISTING       5  /* must exist, truncate -- NT FILE_OVERWRITE */

/* FlagsAndAttributes (Win32 layer -- largely cosmetic on Impossible OS
 * today; reproduced for API-shape completeness). */
#define FILE_ATTRIBUTE_NORMAL           0x00000080u
#define FILE_ATTRIBUTE_READONLY         0x00000001u

/* ---- OVERLAPPED -------------------------------------------------------- *
 *
 * ReadFile takes an LPOVERLAPPED for async I/O. Impossible OS today is
 * synchronous-only, so passing NULL is the expected usage and this type
 * is a forward declaration for ABI-shape parity. When async ReadFile
 * ships, OVERLAPPED will grow its event/completion-port fields. */

typedef struct _OVERLAPPED {
    uint64_t  Internal;
    uint64_t  InternalHigh;
    union {
        struct {
            uint32_t Offset;
            uint32_t OffsetHigh;
        };
        void *Pointer;
    };
    HANDLE    hEvent;
} OVERLAPPED, *LPOVERLAPPED;

/* ---- SECURITY_ATTRIBUTES stub ------------------------------------------ *
 *
 * CreateFileA takes an LPSECURITY_ATTRIBUTES; pass NULL for default
 * (no inheritance, default DACL). Until SRM ships, the shim ignores
 * any non-NULL pointer. The struct is declared only so callers compile. */

typedef struct _SECURITY_ATTRIBUTES {
    DWORD   nLength;
    LPVOID  lpSecurityDescriptor;
    BOOL    bInheritHandle;
} SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

/* ---- Function prototypes ----------------------------------------------- */

/* Returns the PID of the calling process. Reads
 * TEB.ClientId.UniqueProcess at `gs:0x40`, matching the Windows x64
 * kernel32 inline fast path. */
DWORD  GetCurrentProcessId(void);

/* Returns the tick count in milliseconds since boot. Reads
 * KUSER_SHARED_DATA->TickCountLowDeprecated at `0x7FFE0000+0x000`
 * and scales by TickCountMultiplier (at `+0x004`) -- the same math
 * Windows' inline kernel32 does to avoid a syscall for a global clock. */
DWORD  GetTickCount(void);

/* Opens (today: only OPEN_EXISTING) or creates (future) a file handle.
 * `dwDesiredAccess` is a Win32 ACCESS_MASK; GENERIC_READ / GENERIC_WRITE
 * are mapped to VFS_O_READ / VFS_O_WRITE client-side until SRM ships.
 * Returns INVALID_HANDLE_VALUE on failure. `lpFileName` is a narrow
 * Windows path (e.g. "C:\\hello.txt"). */
HANDLE CreateFileA(LPCSTR  lpFileName,
                   DWORD   dwDesiredAccess,
                   DWORD   dwShareMode,
                   LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                   DWORD   dwCreationDisposition,
                   DWORD   dwFlagsAndAttributes,
                   HANDLE  hTemplateFile);

/* Synchronous read from a file/pipe/device handle. `lpOverlapped` must
 * be NULL today (async not supported). Returns TRUE on success and
 * writes the byte count to `*lpNumberOfBytesRead`; FALSE on failure
 * (`*lpNumberOfBytesRead` is set to 0 so the caller can't misread
 * garbage). */
BOOL   ReadFile(HANDLE       hFile,
                LPVOID       lpBuffer,
                DWORD        nNumberOfBytesToRead,
                DWORD       *lpNumberOfBytesRead,
                LPOVERLAPPED lpOverlapped);

/* Close any HANDLE returned by CreateFileA / other handle-producing
 * APIs. Returns TRUE on success, FALSE if the kernel refused
 * (STATUS_INVALID_HANDLE, double-close, etc.). */
BOOL   CloseHandle(HANDLE hObject);
