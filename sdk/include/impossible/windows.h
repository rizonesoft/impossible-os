/**
 * @file windows.h
 * @brief Win32-compatible API for Impossible OS
 *
 * This is the primary SDK header. It exposes the user-space API surface
 * that maps to kernel VFS, process, and registry operations.
 *
 * Kernel VFS (vfs.h) → Win32 SDK mapping:
 *   vfs_open()     → CreateFile()
 *   vfs_read()     → ReadFile()
 *   vfs_write()    → WriteFile()
 *   vfs_close()    → CloseHandle()
 *   vfs_stat()     → GetFileAttributes() / GetFileSize()
 *   vfs_create()   → CreateFile() with CREATE_NEW
 *   vfs_unlink()   → DeleteFile()
 *   vfs_rename()   → MoveFile()
 *   vfs_readdir()  → FindFirstFile() / FindNextFile()
 *   vfs_truncate() → SetEndOfFile()
 *   vfs_mount()    → (kernel-only, not exposed to user space)
 */

#pragma once

/* ── Freestanding type definitions ────────────────────────────────────────── */
/* SDK headers are standalone — no dependency on system headers.              */

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;
typedef unsigned long long  uintptr_t;
typedef signed long long    intptr_t;
typedef unsigned long long  size_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ── Windows type definitions ─────────────────────────────────────────────── */

typedef void *          HANDLE;
typedef uint32_t        DWORD;
typedef int32_t         LONG;
typedef int             BOOL;
typedef uint16_t        WORD;
typedef uint8_t         BYTE;
typedef char *          LPSTR;
typedef const char *    LPCSTR;
typedef void *          LPVOID;
typedef const void *    LPCVOID;
typedef DWORD *         LPDWORD;

/* ── Handle constants ─────────────────────────────────────────────────────── */

#define INVALID_HANDLE_VALUE    ((HANDLE)(intptr_t)-1)
#define NULL_HANDLE             ((HANDLE)0)

#define TRUE                    1
#define FALSE                   0

/* ── Generic access rights ────────────────────────────────────────────────── */

#define GENERIC_READ            0x80000000
#define GENERIC_WRITE           0x40000000
#define GENERIC_EXECUTE         0x20000000
#define GENERIC_ALL             0x10000000

/* ── File creation dispositions ───────────────────────────────────────────── */
/* Maps to VFS: vfs_open() flags (VFS_O_CREATE, VFS_O_TRUNC)                 */

#define CREATE_NEW              1   /* Fail if exists */
#define CREATE_ALWAYS           2   /* Overwrite if exists */
#define OPEN_EXISTING           3   /* Fail if not exists */
#define OPEN_ALWAYS             4   /* Create if not exists, open otherwise */
#define TRUNCATE_EXISTING       5   /* Open and truncate */

/* ── File attributes ──────────────────────────────────────────────────────── */
/* Maps to VFS: vfs_node.type (VFS_FILE=0x01, VFS_DIRECTORY=0x02)            */

#define FILE_ATTRIBUTE_NORMAL       0x00000080
#define FILE_ATTRIBUTE_READONLY     0x00000001
#define FILE_ATTRIBUTE_HIDDEN       0x00000002
#define FILE_ATTRIBUTE_DIRECTORY    0x00000010

/* ── File share modes ─────────────────────────────────────────────────────── */

#define FILE_SHARE_READ         0x00000001
#define FILE_SHARE_WRITE        0x00000002
#define FILE_SHARE_DELETE       0x00000004

/* ── Seek methods ─────────────────────────────────────────────────────────── */

#define FILE_BEGIN              0
#define FILE_CURRENT            1
#define FILE_END                2

/* ── Find data structure (for FindFirstFile / FindNextFile) ────────────────── */
/* Maps to VFS: struct vfs_dirent + struct vfs_stat                           */

typedef struct {
    DWORD   dwFileAttributes;
    DWORD   nFileSizeHigh;
    DWORD   nFileSizeLow;
    char    cFileName[256];     /* VFS_MAX_NAME = 256 */
} WIN32_FIND_DATA;

/* ── Security attributes (placeholder) ────────────────────────────────────── */

typedef struct {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  File I/O API                                                              */
/*  Backed by kernel VFS: vfs_open, vfs_read, vfs_write, vfs_close           */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Open or create a file.
 * Kernel: vfs_open(path, flags) + vfs_create(path, VFS_FILE)
 *
 * @param lpFileName        Windows-style path, e.g. "C:\\Users\\file.txt"
 * @param dwDesiredAccess   GENERIC_READ, GENERIC_WRITE, or both
 * @param dwShareMode       FILE_SHARE_READ, FILE_SHARE_WRITE (reserved)
 * @param lpSecurityAttrs   Reserved, pass NULL
 * @param dwCreationDisp    CREATE_NEW, CREATE_ALWAYS, OPEN_EXISTING, etc.
 * @param dwFlagsAndAttrs   FILE_ATTRIBUTE_NORMAL
 * @param hTemplateFile     Reserved, pass NULL
 * @return File handle, or INVALID_HANDLE_VALUE on failure
 */
HANDLE CreateFile(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                  SECURITY_ATTRIBUTES *lpSecurityAttrs, DWORD dwCreationDisp,
                  DWORD dwFlagsAndAttrs, HANDLE hTemplateFile);

/**
 * Read from an open file.
 * Kernel: vfs_read(node, offset, size, buffer)
 */
BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
              LPDWORD lpNumberOfBytesRead, LPVOID lpOverlapped);

/**
 * Write to an open file.
 * Kernel: vfs_write(node, offset, size, buffer)
 */
BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
               LPDWORD lpNumberOfBytesWritten, LPVOID lpOverlapped);

/**
 * Close an open handle (file, process, thread, etc.)
 * Kernel: vfs_close(node)
 */
BOOL CloseHandle(HANDLE hObject);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  File Management API                                                       */
/*  Backed by kernel VFS: vfs_unlink, vfs_rename, vfs_stat, vfs_truncate     */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Delete a file.
 * Kernel: vfs_unlink(path)
 */
BOOL DeleteFile(LPCSTR lpFileName);

/**
 * Move or rename a file.
 * Kernel: vfs_rename(old_path, new_path)
 * Note: Both paths must be on the same drive (VFS constraint).
 */
BOOL MoveFile(LPCSTR lpExistingFileName, LPCSTR lpNewFileName);

/**
 * Get file attributes (type, etc.)
 * Kernel: vfs_stat(path, &st) → st.type
 */
DWORD GetFileAttributes(LPCSTR lpFileName);

/**
 * Get file size.
 * Kernel: vfs_stat(path, &st) → st.size
 */
DWORD GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh);

/**
 * Set end of file at current file pointer position.
 * Kernel: vfs_truncate(path, new_size)
 */
BOOL SetEndOfFile(HANDLE hFile);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Directory Enumeration API                                                  */
/*  Backed by kernel VFS: vfs_readdir, vfs_finddir                           */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Find first file matching a pattern.
 * Kernel: vfs_readdir(dir_node, 0)
 */
HANDLE FindFirstFile(LPCSTR lpFileName, WIN32_FIND_DATA *lpFindFileData);

/**
 * Find next file in enumeration.
 * Kernel: vfs_readdir(dir_node, index++)
 */
BOOL FindNextFile(HANDLE hFindFile, WIN32_FIND_DATA *lpFindFileData);

/**
 * Close a find handle.
 */
BOOL FindClose(HANDLE hFindFile);

/**
 * Create a directory.
 * Kernel: vfs_create(path, VFS_DIRECTORY)
 */
BOOL CreateDirectory(LPCSTR lpPathName, SECURITY_ATTRIBUTES *lpSecurityAttrs);

/**
 * Remove an empty directory.
 * Kernel: vfs_unlink(path)
 */
BOOL RemoveDirectory(LPCSTR lpPathName);
