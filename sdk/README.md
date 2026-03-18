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
    HANDLE file = CreateFile("C:\\hello.txt", GENERIC_WRITE, 0, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    const char *msg = "Hello from Impossible OS!";
    DWORD written;
    WriteFile(file, msg, 25, &written, NULL);
    CloseHandle(file);
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

Impossible OS exposes a **Win32-compatible API** as the native interface:

| API | Status | Functions |
|-----|--------|-----------|
| File I/O | 🔜 Planned | `CreateFile`, `ReadFile`, `WriteFile`, `CloseHandle` |
| Process | 🔜 Planned | `CreateProcess`, `ExitProcess`, `GetExitCodeProcess` |
| Memory | 🔜 Planned | `VirtualAlloc`, `VirtualFree`, `HeapAlloc` |
| Threading | 🔜 Planned | `CreateThread`, `WaitForSingleObject`, `CreateMutex` |
| Registry | 🔜 Planned | `RegOpenKeyEx`, `RegQueryValueEx`, `RegSetValueEx` |
| Window | 🔜 Planned | `CreateWindow`, `ShowWindow`, `GetMessage`, `DispatchMessage` |

> **Note:** The SDK is in early development. Headers and libraries will be
> populated as the Win32-compatible API layer (TODO-045) is implemented.

## License

GPL-3.0 — see [LICENSE](../LICENSE)
