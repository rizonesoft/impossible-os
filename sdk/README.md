# Impossible OS SDK

Development kit for building applications that run on Impossible OS.

## Contents

| Directory | Description |
|-----------|-------------|
| `include/impossible/` | Public API headers (Win32-compatible) |
| `lib/` | Static libraries for linking |
| `docs/` | API reference and guides |
| `examples/` | Sample applications |
| `tools/` | Build utilities and scripts |

## Quick Start

```c
#include <impossible/windows.h>

int WinMain(void) {
    /* Create and write a file using Win32 API */
    HANDLE hFile = CreateFile("C:\\hello.txt",
                              GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    const char *msg = "Hello from Impossible OS!";
    DWORD written;
    WriteFile(hFile, msg, 25, &written, NULL);
    CloseHandle(hFile);

    return 0;
}
```

## Building

```bash
# Cross-compile with clang targeting Impossible OS
clang-19 --target=x86_64-elf \
    -I sdk/include \
    -ffreestanding -nostdlib \
    -o hello.exe hello.c \
    -L sdk/lib -limpossible
```

## API Surface

Impossible OS exposes a **Win32-compatible API** as the native user-space interface.
Each function maps to a kernel VFS operation internally.

### File I/O

| Win32 API | Kernel VFS | Status |
|-----------|-----------|--------|
| `CreateFile()` | `vfs_open()` / `vfs_create()` | ✅ Declared |
| `ReadFile()` | `vfs_read()` | ✅ Declared |
| `WriteFile()` | `vfs_write()` | ✅ Declared |
| `CloseHandle()` | `vfs_close()` | ✅ Declared |

### File Management

| Win32 API | Kernel VFS | Status |
|-----------|-----------|--------|
| `DeleteFile()` | `vfs_unlink()` | ✅ Declared |
| `MoveFile()` | `vfs_rename()` | ✅ Declared |
| `GetFileAttributes()` | `vfs_stat()` | ✅ Declared |
| `GetFileSize()` | `vfs_stat()` | ✅ Declared |
| `SetEndOfFile()` | `vfs_truncate()` | ✅ Declared |

### Directory Enumeration

| Win32 API | Kernel VFS | Status |
|-----------|-----------|--------|
| `FindFirstFile()` | `vfs_readdir()` | ✅ Declared |
| `FindNextFile()` | `vfs_readdir()` | ✅ Declared |
| `FindClose()` | -- | ✅ Declared |
| `CreateDirectory()` | `vfs_create(VFS_DIRECTORY)` | ✅ Declared |
| `RemoveDirectory()` | `vfs_unlink()` | ✅ Declared |

### Planned (Not Yet Implemented)

| API | Functions |
|-----|-----------|
| Process | `CreateProcess`, `ExitProcess`, `GetExitCodeProcess` |
| Memory | `VirtualAlloc`, `VirtualFree`, `HeapAlloc` |
| Threading | `CreateThread`, `WaitForSingleObject`, `CreateMutex` |
| Registry | `RegOpenKeyEx`, `RegQueryValueEx`, `RegSetValueEx` |
| Window | `CreateWindow`, `ShowWindow`, `GetMessage`, `DispatchMessage` |

## Paths

Impossible OS uses **Windows-style paths** as canonical:

```
C:\Impossible\System32\kernel.exe
C:\Users\Default\Documents\readme.txt
D:\Data\backup.zip
```

Drive letters A–Z are supported (VFS mount points).

## License

GPL-3.0 -- see [LICENSE](../LICENSE)
