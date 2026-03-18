# SDK API Reference

## Overview

The Impossible OS SDK provides a Win32-compatible API for building native applications.
Applications are PE32+ executables that call familiar Windows-style functions, which are
dispatched to the kernel's VFS layer internally.

## Headers

| Header | Description |
|--------|-------------|
| `<impossible/windows.h>` | Win32 API: file I/O, directories, attributes |

## Type Reference

| Type | Definition | Win32 Equivalent |
|------|-----------|------------------|
| `HANDLE` | `void *` | Opaque kernel object handle |
| `DWORD` | `uint32_t` | 32-bit unsigned integer |
| `BOOL` | `int` | Boolean (TRUE=1, FALSE=0) |
| `LPCSTR` | `const char *` | Read-only string pointer |
| `LPVOID` | `void *` | Generic pointer |
| `LPDWORD` | `uint32_t *` | Pointer to DWORD |
| `WIN32_FIND_DATA` | struct | Directory entry (maps to `vfs_dirent`) |

## File I/O Functions

### CreateFile

```c
HANDLE CreateFile(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                  SECURITY_ATTRIBUTES *lpSecurityAttrs, DWORD dwCreationDisp,
                  DWORD dwFlagsAndAttrs, HANDLE hTemplateFile);
```

Opens or creates a file. Maps to `vfs_open()` + `vfs_create()`.

| Parameter | Description |
|-----------|-------------|
| `lpFileName` | Windows path, e.g. `"C:\\file.txt"` |
| `dwDesiredAccess` | `GENERIC_READ`, `GENERIC_WRITE`, or both |
| `dwCreationDisp` | `CREATE_NEW`, `CREATE_ALWAYS`, `OPEN_EXISTING`, etc. |
| Returns | File handle, or `INVALID_HANDLE_VALUE` on failure |

### ReadFile / WriteFile

```c
BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nBytesToRead,
              LPDWORD lpBytesRead, LPVOID lpOverlapped);
BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nBytesToWrite,
               LPDWORD lpBytesWritten, LPVOID lpOverlapped);
```

Read/write data. Maps to `vfs_read()` / `vfs_write()`.

### CloseHandle

```c
BOOL CloseHandle(HANDLE hObject);
```

Close any open handle. Maps to `vfs_close()`.

## File Management Functions

| Function | Signature | VFS Mapping |
|----------|-----------|-------------|
| `DeleteFile` | `BOOL DeleteFile(LPCSTR)` | `vfs_unlink()` |
| `MoveFile` | `BOOL MoveFile(LPCSTR, LPCSTR)` | `vfs_rename()` |
| `GetFileAttributes` | `DWORD GetFileAttributes(LPCSTR)` | `vfs_stat()` |
| `GetFileSize` | `DWORD GetFileSize(HANDLE, LPDWORD)` | `vfs_stat()` |
| `SetEndOfFile` | `BOOL SetEndOfFile(HANDLE)` | `vfs_truncate()` |

## Directory Functions

| Function | Signature | VFS Mapping |
|----------|-----------|-------------|
| `FindFirstFile` | `HANDLE FindFirstFile(LPCSTR, WIN32_FIND_DATA*)` | `vfs_readdir()` |
| `FindNextFile` | `BOOL FindNextFile(HANDLE, WIN32_FIND_DATA*)` | `vfs_readdir()` |
| `FindClose` | `BOOL FindClose(HANDLE)` | — |
| `CreateDirectory` | `BOOL CreateDirectory(LPCSTR, ...)` | `vfs_create(VFS_DIRECTORY)` |
| `RemoveDirectory` | `BOOL RemoveDirectory(LPCSTR)` | `vfs_unlink()` |

## Constants

### Access Rights

| Constant | Value | Description |
|----------|-------|-------------|
| `GENERIC_READ` | `0x80000000` | Read access |
| `GENERIC_WRITE` | `0x40000000` | Write access |

### Creation Dispositions

| Constant | Value | Description |
|----------|-------|-------------|
| `CREATE_NEW` | `1` | Create new, fail if exists |
| `CREATE_ALWAYS` | `2` | Create new, overwrite if exists |
| `OPEN_EXISTING` | `3` | Open existing, fail if not exists |
| `OPEN_ALWAYS` | `4` | Open if exists, create if not |
| `TRUNCATE_EXISTING` | `5` | Open and truncate |
