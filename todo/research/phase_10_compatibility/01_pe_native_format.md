# Running External Windows PE Programs — Research

## Goal

Enable Impossible OS to **load and execute external Windows `.exe` and `.dll` files** natively. The OS remains ELF-based — PE support is a kernel-level subsystem that parses Windows binaries, maps their sections, resolves DLL imports, and provides Win32 API translations.

---

## How Windows Programs Work

A Windows `.exe` doesn't talk to the kernel directly. Instead it calls functions exported by DLLs:

```
user_program.exe
  → calls kernel32.dll!WriteConsoleA()
    → calls ntdll.dll!NtWriteFile()
      → NT kernel syscall
```

To run a Windows program, we need three things:
1. **PE Loader** — parse the binary and map it into memory
2. **Import Resolution** — find every DLL function the program calls
3. **Win32 Stubs** — our implementations of those DLL functions

---

## PE Binary Format

### File layout

```
┌───────────────────────┐  offset 0
│  DOS Header (64 B)    │  e_magic = "MZ"
│                       │  e_lfanew → PE signature offset
├───────────────────────┤  offset e_lfanew
│  PE Signature (4 B)   │  "PE\0\0"
├───────────────────────┤
│  COFF Header (20 B)   │  Machine (AMD64), NumberOfSections
├───────────────────────┤
│  Optional Header      │  PE32+ (64-bit): ImageBase, EntryPointRVA,
│  (112+ bytes)         │  SectionAlignment, DataDirectory[]
├───────────────────────┤
│  Data Directories     │  Import Table, Export Table, Relocation Table,
│  (16 entries × 8 B)   │  Resource Table, etc.
├───────────────────────┤
│  Section Table        │  .text, .rdata, .data, .bss, .idata, .reloc
│  (40 B per section)   │
├───────────────────────┤
│  Section Data         │  Code, initialized data, resources
└───────────────────────┘
```

### Key data directories

| Index | Name | Purpose |
|-------|------|---------|
| 0 | Export Table | Functions this module exports (for DLLs) |
| 1 | **Import Table** | DLLs and functions this program needs |
| 5 | **Base Relocation** | Fixups if loaded at non-preferred address |
| 2 | Resource Table | Icons, strings, dialogs (future) |

---

## PE Loader Design (`pe.c`)

### Loading steps

```
1. Validate DOS header (e_magic == 0x5A4D "MZ")
2. Follow e_lfanew to PE signature, validate "PE\0\0"
3. Parse COFF header:
   - Machine must be IMAGE_FILE_MACHINE_AMD64 (0x8664)
   - Read NumberOfSections
4. Parse Optional Header (PE32+, Magic = 0x020B):
   - ImageBase (preferred load address)
   - AddressOfEntryPoint (RVA)
   - SizeOfImage
   - DataDirectory[1] = Import Table location
   - DataDirectory[5] = Base Relocation Table
5. Allocate SizeOfImage bytes at ImageBase
6. For each section:
   - memcpy file data → ImageBase + section.VirtualAddress
   - Zero-fill BSS (VirtualSize > RawDataSize)
7. Apply base relocations (if loaded at different address)
8. Resolve imports (see below)
9. Entry = ImageBase + AddressOfEntryPoint
```

### Auto-detection in exec path

```c
struct load_result load_binary(const uint8_t *data, uint64_t size)
{
    if (size >= 2 && data[0] == 'M' && data[1] == 'Z')
        return pe_load(data, size);     /* Windows PE */

    if (size >= 4 && *(uint32_t *)data == 0x464C457F)
        return elf_load(data, size);    /* Linux/Unix ELF */

    return (struct load_result){ .success = 0 };
}
```

---

## Import Resolution (DLL Loading)

This is where external Windows programs get connected to our OS:

### How the Import Table works

```
Program's Import Directory:
┌──────────────────────────────────┐
│ DLL: "kernel32.dll"              │
│   ├─ WriteConsoleA               │
│   ├─ ExitProcess                 │
│   ├─ GetStdHandle                │
│   └─ GetCommandLineA             │
├──────────────────────────────────┤
│ DLL: "msvcrt.dll"                │
│   ├─ printf                      │
│   ├─ malloc                      │
│   └─ free                        │
└──────────────────────────────────┘
```

### Resolution process

```
For each DLL in the Import Directory:
  1. Check builtin DLL table first
     └─ "kernel32.dll" → builtin_kernel32[]
     └─ "msvcrt.dll"   → builtin_msvcrt[]
     └─ "ntdll.dll"    → builtin_ntdll[]
  2. If not builtin, search filesystem:
     └─ C:\Windows\System32\{name}
     └─ Same directory as the .exe
  3. For filesystem DLLs: load as PE, parse export table
  4. For each imported function name:
     └─ Look up in DLL's export table (by name or ordinal)
     └─ Write function address into program's IAT (Import Address Table)
```

### Builtin DLL stub table

```c
struct win32_export {
    const char *name;
    void       *func_ptr;
};

static const struct win32_export builtin_kernel32[] = {
    { "GetStdHandle",    win32_GetStdHandle },
    { "WriteConsoleA",   win32_WriteConsoleA },
    { "ReadConsoleA",    win32_ReadConsoleA },
    { "ExitProcess",     win32_ExitProcess },
    { "VirtualAlloc",    win32_VirtualAlloc },
    { "VirtualFree",     win32_VirtualFree },
    { "CreateFileA",     win32_CreateFileA },
    { "ReadFile",        win32_ReadFile },
    { "WriteFile",       win32_WriteFile },
    { "CloseHandle",     win32_CloseHandle },
    { "GetLastError",    win32_GetLastError },
    { "SetLastError",    win32_SetLastError },
    { "GetCommandLineA", win32_GetCommandLineA },
    { NULL, NULL }
};
```

---

## Win32 API Stub Implementation

Each stub translates a Win32 call to an Impossible OS syscall or kernel function:

### Console I/O

```c
/* GetStdHandle — return our fd as a HANDLE */
HANDLE win32_GetStdHandle(DWORD nStdHandle) {
    switch (nStdHandle) {
        case STD_INPUT_HANDLE:  return (HANDLE)0;
        case STD_OUTPUT_HANDLE: return (HANDLE)1;
        case STD_ERROR_HANDLE:  return (HANDLE)2;
        default: return INVALID_HANDLE_VALUE;
    }
}

/* WriteConsoleA — write to fd via SYS_WRITE */
BOOL win32_WriteConsoleA(HANDLE h, const void *buf, DWORD len,
                         DWORD *written, void *reserved) {
    (void)reserved;
    int64_t ret = sys_write((int)(uintptr_t)h, buf, len);
    if (written) *written = (ret > 0) ? (DWORD)ret : 0;
    return ret >= 0;
}

/* ExitProcess — call SYS_EXIT */
void win32_ExitProcess(UINT code) {
    sys_exit(code);
}
```

### Memory management

```c
/* VirtualAlloc — map memory via future SYS_MMAP */
LPVOID win32_VirtualAlloc(LPVOID addr, SIZE_T size,
                          DWORD type, DWORD protect) {
    (void)type; (void)protect;
    return sys_mmap(addr, size, PROT_READ | PROT_WRITE);
}
```

### File I/O

```c
/* CreateFileA — open a file via VFS */
HANDLE win32_CreateFileA(LPCSTR filename, DWORD access, ...) {
    /* Translate "C:\path\file" to VFS path */
    int fd = vfs_open(filename, translate_access(access));
    return (fd >= 0) ? (HANDLE)(uintptr_t)fd : INVALID_HANDLE_VALUE;
}
```

---

## DLL Coverage Tiers

### Tier 1 — Console "Hello World" (~15 functions)

| DLL | Functions |
|-----|-----------|
| kernel32 | `GetStdHandle`, `WriteConsoleA`, `ExitProcess`, `GetCommandLineA`, `GetModuleHandleA` |
| msvcrt | `printf`, `puts`, `exit`, `malloc`, `free`, `memcpy`, `strlen` |
| ntdll | `RtlInitUnicodeString` (stub) |

**Result**: Run programs compiled with `x86_64-w64-mingw32-gcc -static` or simple console tools.

### Tier 2 — File I/O programs (~35 functions)

Add: `CreateFileA/W`, `ReadFile`, `WriteFile`, `CloseHandle`, `GetFileSize`, `SetFilePointer`, `VirtualAlloc`, `VirtualFree`, `HeapCreate`, `HeapAlloc`, `HeapFree`, `GetLastError`, `SetLastError`, `GetCurrentDirectory`, `SetCurrentDirectory`, `FindFirstFileA`, `FindNextFileA`, `FindClose`.

**Result**: Run programs that read/write files, manage memory.

### Tier 3 — GUI programs (future, ~100+ functions)

Add user32 (`CreateWindowExW`, `ShowWindow`, `GetMessage`, `DispatchMessage`, `DefWindowProc`), gdi32 (`CreateDC`, `BitBlt`, `TextOut`). These hook into our window manager.

**Result**: Run simple Windows GUI applications on the Impossible OS desktop.

---

## Calling Convention

Windows x64 programs pass arguments as RCX, RDX, R8, R9 (not RDI, RSI, RDX, RCX like System V). Our Win32 stubs are compiled or written to match:

```c
/* Stubs use Windows x64 convention — args arrive in RCX, RDX, R8, R9.
 * Internally they call Impossible OS syscalls via inline asm which
 * explicitly sets the registers, so no convention conflict. */

BOOL win32_WriteFile(HANDLE hFile, const void *buf, DWORD len, ...) {
    int fd = (int)(uintptr_t)hFile;
    int64_t ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((uint64_t)SYS_WRITE), "D"((uint64_t)fd),
          "S"((uint64_t)buf), "d"((uint64_t)len));
    return ret >= 0;
}
```

---

## Prerequisites

| Requirement | Status | Needed for |
|-------------|--------|------------|
| Per-process page tables | ❌ Not yet | Loading at preferred ImageBase safely |
| `mmap()` syscall | ❌ Not yet | `VirtualAlloc` stub |
| `brk()` syscall | ❌ Not yet | `HeapAlloc` stub |
| VFS path translation | 🟡 Partial | `CreateFileA` (Windows paths → VFS) |
| Thread support | ❌ Not yet | `CreateThread` (Tier 2+) |
| WM integration | ✅ Exists | GUI stubs (Tier 3) |

---

## Implementation Plan

### Phase 1: PE Loader (2-3 days)
- [ ] `include/pe.h` — header structures
- [ ] `src/kernel/pe.c` — load sections, validate headers
- [ ] `load_binary()` — dual-format detect (MZ vs ELF)
- [ ] Test with a minimal hand-crafted PE

### Phase 2: Import Resolution (3-5 days)
- [ ] Parse Import Directory Table
- [ ] Builtin DLL lookup table
- [ ] IAT patching (write stub addresses)
- [ ] Base relocation support

### Phase 3: Win32 Console Stubs (1-2 weeks)
- [ ] `src/kernel/win32/kernel32.c` — Tier 1 functions
- [ ] `src/kernel/win32/msvcrt.c` — printf, malloc, etc.
- [ ] `src/kernel/win32/ntdll.c` — minimal stubs
- [ ] Run a MinGW "Hello World" `.exe`

### Phase 4: File I/O + Memory (2-3 weeks)
- [ ] Implement `SYS_MMAP` / `SYS_BRK`
- [ ] Windows path → VFS translation (`C:\` → drive C)
- [ ] Tier 2 kernel32 functions (file + heap)

### Phase 5: GUI Integration (future)
- [ ] user32 stubs → WM window creation
- [ ] gdi32 stubs → framebuffer drawing
- [ ] Message loop → WM event integration

---

## Files

| Action | File | Purpose |
|--------|------|---------|
| **[NEW]** | `include/pe.h` | PE/COFF structures (~80 lines) |
| **[NEW]** | `src/kernel/pe.c` | PE loader + import resolver (~350 lines) |
| **[NEW]** | `src/kernel/win32/kernel32.c` | kernel32.dll stubs |
| **[NEW]** | `src/kernel/win32/msvcrt.c` | C runtime stubs |
| **[NEW]** | `src/kernel/win32/ntdll.c` | NT runtime stubs |
| **[NEW]** | `include/win32.h` | Windows type definitions (HANDLE, DWORD, etc.) |
| **[MODIFY]** | `src/kernel/task.c` | Use `load_binary()` dual-format dispatch |
| **[KEEP]** | `src/kernel/elf.c` | ELF loader (native, unchanged) |
