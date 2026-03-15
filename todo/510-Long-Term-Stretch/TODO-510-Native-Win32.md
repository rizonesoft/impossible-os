# P0105 — Native Win32 Programs & SDK

> **Goal:** Impossible OS is **natively Win32**. PE (Portable Executable) is the
> only binary format. Programs use Win32 APIs (CreateFile, ReadFile, MessageBox,
> CreateWindow, etc.) as the **native OS interface** — not a compatibility shim.
> This phase covers: ring 3 user-mode execution, the PE loader, the native
> Win32 API surface (organized as DLL exports), the SDK, and the IxUI GUI toolkit.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

> [!IMPORTANT]
> **Prerequisite:** User-mode execution (ring 3) must be working before most of
> this phase can begin. Currently skipped due to ring 3 transition hang. Fix the
> user-mode TSS/GDT/syscall path first.

> [!NOTE]
> **Consolidated from P1503.** The former `TODO-510-Native-Win32.md` has been merged
> here because Impossible OS implements Win32 **natively** — there is no separate
> "compatibility layer." The PE loader, DLL system, and Win32 API stubs are all
> part of the native OS interface.

**Related TODOs:**
- **P1502** — C/C++ compiler (TCC → GCC, PE output)
- **P0103 §3.6** — Native file I/O API (CreateFile, ReadFile, etc.)
- **P0104** — MessageBox (already implemented)

**Win32 API Scope:**

| API Surface                          | ~Functions | Status     | Impossible OS Mapping             |
|--------------------------------------|------------|------------|-----------------------------------|
| File I/O (CreateFile, ReadFile)      | ~30        | **Done** ✅ | Native API (P0103 §3.6)           |
| Registry (RegOpenKey, etc.)          | ~15        | **Done** ✅ | Native Registry API               |
| MessageBox, DialogBox                | ~10        | **Done** ✅ | Native `MessageBox()` (P0104)     |
| PE loader (.exe, .dll)               | ~20        | TODO       | Section mapper + IAT resolver     |
| Console (WriteConsole, etc.)         | ~10        | TODO       | Thin wrappers on native I/O       |
| CreateWindow, WndProc, message loop  | ~50        | TODO       | IxUI / `wm_create()` + events     |
| GDI (CreateDC, BitBlt, SelectObject) | ~200       | Stretch    | `gfx_*()` primitives + compositor |

---

## 1. Fix User-Mode Execution

**Prompt:** User-mode (ring 3) execution is currently disabled due to a hang during the privilege transition. Debug and fix the user-mode path: verify TSS is loaded with a valid RSP0 (kernel stack), GDT has correct DPL=3 user code/data segments, `sysret`/`iretq` correctly sets CS/SS/RIP/RSP for ring 3, and the syscall entry point saves/restores all registers. Test with a minimal user-mode function that calls `SYS_EXIT`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: fix user-mode execution"`.


- [ ] Debug ring 3 transition hang (TSS RSP0, GDT segments, STAR/LSTAR MSRs)
- [ ] Verify `syscall_init()` sets STAR, LSTAR, SFMASK MSRs correctly
- [ ] Verify GDT user segments: code (DPL=3, long mode), data (DPL=3)
- [ ] Verify TSS has valid kernel stack pointer (RSP0)
- [ ] Test: minimal ring 3 function that does `syscall(SYS_EXIT, 0)`
- [ ] Commit: `"kernel: fix user-mode execution"`

---

## 2. Syscall Interface (Win x64 Convention)

**Prompt:** Define the Impossible OS syscall interface using the Windows x64 calling convention (args in RCX, RDX, R8, R9, then stack). Syscall number in RAX, invoked via `int 0x80` or `syscall` instruction. Document all existing syscalls and assign stable numbers. Group by category: process (ExitProcess, CreateProcess), file I/O (CreateFile, ReadFile, WriteFile, CloseHandle), memory (VirtualAlloc, VirtualFree), display (MessageBox, CreateWindow). Create `include/kernel/sched/abi.h` with all syscall numbers. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: define syscall interface"`.


- [ ] Define syscall convention: Win x64 (RCX, RDX, R8, R9), number in RAX
- [ ] Assign stable syscall numbers to all existing syscalls
- [ ] File I/O: `SYS_CREATEFILE`, `SYS_READFILE`, `SYS_WRITEFILE`, `SYS_CLOSEHANDLE`
- [ ] Process: `SYS_EXITPROCESS`, `SYS_CREATEPROCESS`, `SYS_GETCOMMANDLINE`
- [ ] Memory: `SYS_VIRTUALALLOC`, `SYS_VIRTUALFREE`
- [ ] Display: `SYS_MSGBOX`, `SYS_CREATEWINDOW`, `SYS_DRAWTEXT`
- [ ] Create `include/kernel/sched/abi.h` with `#define SYS_*` constants
- [ ] Create `docs/architecture/native-abi.md`
- [ ] Commit: `"kernel: define syscall interface"`

### 2.1 Calling Convention

**Prompt:** Windows x64 passes args in RCX, RDX, R8, R9 (vs System V: RDI, RSI, RDX, RCX). Compile Win32 API functions with `__attribute__((ms_abi))` or use inline asm to remap. Syscalls via INT 0x80 bypass C ABI, so no conflict. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: Win32 calling convention"`.


- [ ] Win32 API functions use Windows x64 convention: args in RCX, RDX, R8, R9
- [ ] Functions internally use inline `INT 0x80` assembly with explicit register setup
- [ ] No automatic convention conflict since syscalls bypass C ABI
- [ ] Commit: `"kernel: native Win32 calling convention"`

### 2.2 Convention Thunks *(Stretch)*

- [ ] *(Stretch)* Windows-to-SysV thunk: `mov rdi, rcx; mov rsi, rdx; mov rdx, r8; mov rcx, r9; jmp target`
- [ ] *(Stretch)* SysV-to-Windows thunk: reverse register mapping
- [ ] *(Stretch)* Only needed if linking third-party SysV-convention libraries

---

## 3. PE Binary Loader

### 3.1 PE Header Structures

**Prompt:** PE (Portable Executable) is the native binary format for Impossible OS. Define the structures per the PE/COFF spec: DOS header (e_magic "MZ", e_lfanew to PE signature), COFF header (machine=AMD64), PE32+ Optional Header (magic 0x020B, ImageBase, EntryPointRVA, SizeOfImage), section headers (name, VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData), data directories (RVA+Size for imports, relocations, etc). After completing all items, create `docs/architecture/pe-loader.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: PE/COFF header structures"`.


- [ ] Create `include/pe.h` (~80 lines)
- [ ] Define `struct pe_dos_header` (e_magic "MZ", e_lfanew offset to PE signature)
- [ ] Define `struct pe_coff_header` (machine, num_sections, characteristics)
- [ ] Define `struct pe_optional_header_64` (PE32+ magic `0x020B`, image_base, entry_point_rva, section_alignment, size_of_image, num_data_dirs)
- [ ] Define `struct pe_section_header` (name, virtual_size, virtual_address, raw_data_size, raw_data_offset, characteristics)
- [ ] Define `struct pe_data_directory` (rva, size) — for import table, export table, relocation table
- [ ] Define PE constants: `PE_DOS_MAGIC`, `PE_SIGNATURE`, `PE_MACHINE_AMD64`, `PE32PLUS_MAGIC`
- [ ] Commit: `"kernel: PE/COFF header structures"`

### 3.2 PE Loader Core

**Prompt:** `pe_load(data, size)` validates DOS "MZ" header, follows e_lfanew to PE signature, parses COFF (verify Machine==AMD64), reads PE32+ Optional Header (ImageBase, EntryPointRVA, SizeOfImage, DataDirectory imports+relocations). Allocate SizeOfImage at ImageBase, copy each section to ImageBase+VirtualAddress, zero-fill BSS. Return entry = ImageBase + AddressOfEntryPoint. After completing all items, update `docs/architecture/pe-loader.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: PE loader core"`.


- [ ] Create `src/kernel/pe.c` (~250 lines)
- [ ] Implement `pe_load(data, size)`:
  - [ ] Validate DOS header: `e_magic == 0x5A4D` ("MZ")
  - [ ] Follow `e_lfanew` to PE signature, validate `"PE\0\0"`
  - [ ] Parse COFF header: verify `Machine == IMAGE_FILE_MACHINE_AMD64 (0x8664)`
  - [ ] Parse PE32+ Optional Header: read ImageBase, EntryPointRVA, SizeOfImage
  - [ ] Read DataDirectory[1] (Import Table) and DataDirectory[5] (Base Relocation)
  - [ ] Allocate `SizeOfImage` bytes at ImageBase (or available address)
  - [ ] For each section: memcpy file data → ImageBase + VirtualAddress
  - [ ] Zero-fill BSS (VirtualSize > RawDataSize)
  - [ ] Return entry point = ImageBase + AddressOfEntryPoint
- [ ] Commit: `"kernel: PE loader core"`

### 3.3 Base Relocation

**Prompt:** Parse DataDirectory[5] relocation blocks. For IMAGE_REL_BASED_DIR64 entries, add delta (actual_base - preferred_base) to 64-bit addresses. Skip type 0 padding. No relocation needed if loaded at preferred address. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: PE base relocation support"`.


- [ ] Parse DataDirectory[5] (Base Relocation Table) from PE
- [ ] If PE loaded at address ≠ preferred ImageBase:
  - [ ] Calculate delta = actual_base − preferred_base
  - [ ] Walk relocation blocks: for each `IMAGE_REL_BASED_DIR64` entry, add delta to address
- [ ] Skip relocation if loaded at preferred address
- [ ] Commit: `"kernel: PE base relocation support"`

### 3.4 Binary Format Detection

**Prompt:** Create `load_binary(data, size)` that checks magic bytes: `"MZ"` → `pe_load()`. Reject unknown formats with an error message. Update exec path to use `load_binary()` instead of `elf_load()`. The kernel itself is still an ELF (loaded by GRUB), but all user-mode programs are PE. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: PE-only binary detection"`.


- [ ] Create `load_binary(data, size)` in `src/kernel/task.c` (or new file)
- [ ] Auto-detect format by magic bytes:
  - [ ] `"MZ"` (0x5A4D) → `pe_load()` — PE executable (native format)
  - [ ] Unknown → return error ("unsupported binary format")
- [ ] Update exec path: replace direct `elf_load()` call with `load_binary()`
- [ ] Commit: `"kernel: PE-only binary detection"`

### 3.5 PE Resource Section Parser

> **Required** for extracting icons from `.exe` and `.dll` files (taskbar
> icons, title bar icons, File Manager icons).

**Prompt:** Parse DataDirectory[2] Resource Table — it's a 3-level tree: Level 1 = resource type (RT_ICON=3, RT_GROUP_ICON=14, RT_VERSION=16, RT_STRING=6), Level 2 = resource name/ID, Level 3 = language. Implement `pe_find_resource(pe_data, type, id)` that walks the tree and returns a pointer+size to the resource data. For RT_GROUP_ICON, parse the GRPICONDIR structure to find the best-matching size, then load the corresponding RT_ICON entry. For RT_VERSION, parse VS_VERSIONINFO → VS_FIXEDFILEINFO to extract FileVersion, ProductName, CompanyName. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: PE resource section parser"`.


- [ ] Implement `pe_find_resource(pe_data, resource_type, resource_id)` → pointer + size
  - [ ] Walk 3-level resource directory tree (type → name → language)
  - [ ] Handle both integer IDs and string names
- [ ] RT_GROUP_ICON / RT_ICON extraction:
  - [ ] Parse GRPICONDIR structure (count, entries with width/height/bpp/id)
  - [ ] Find best-matching size from available entries
  - [ ] Load corresponding RT_ICON data (BMP DIB or PNG)
  - [ ] Return `icon_bitmap_t` compatible with icon store
- [ ] RT_VERSION extraction:
  - [ ] Parse VS_VERSIONINFO → VS_FIXEDFILEINFO
  - [ ] Extract: FileVersion, ProductName, CompanyName, FileDescription
- [ ] *(Stretch)* RT_STRING extraction (string table blocks)
- [ ] Commit: `"kernel: PE resource section parser"`

---

## 4. Import Resolution & DLL System

### 4.1 Import Table Parser

**Prompt:** Parse DataDirectory[1] Import Directory Table. Each entry has DLL name RVA, Import Lookup Table, Import Address Table. Walk ILT entries: MSB set = ordinal, otherwise Hint/Name Table entry. Log imports: `"PE imports: kernel32.dll!WriteConsoleA"`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: PE import table parser"`.


- [ ] Parse Import Directory Table from DataDirectory[1]
- [ ] For each DLL entry: read DLL name string, Import Lookup Table (ILT), Import Address Table (IAT)
- [ ] For each function: read name (or ordinal) from Hint/Name Table
- [ ] Log imported DLLs and functions: `"PE imports: kernel32.dll!WriteConsoleA"`
- [ ] Commit: `"kernel: PE import table parser"`

### 4.2 Native DLL Export Table

**Prompt:** Map DLL names to (func_name, func_ptr) arrays. `pe_resolve_export(dll_name, func_name)` searches and returns the function pointer. These DLLs are provided natively by the OS. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: native DLL export table"`.


- [ ] Define `struct win32_export` (name, func_ptr)
- [ ] Create lookup function: `pe_resolve_export(dll_name, func_name)` → function pointer
- [ ] Native DLL registry:
  - [ ] `"kernel32.dll"` → `native_kernel32[]`
  - [ ] `"msvcrt.dll"` → `native_msvcrt[]`
  - [ ] `"ntdll.dll"` → `native_ntdll[]`
  - [ ] `"user32.dll"` → `native_user32[]`
  - [ ] `"gdi32.dll"` → `native_gdi32[]`
  - [ ] `"shell32.dll"` → `native_shell32[]`
  - [ ] `"ws2_32.dll"` → `native_ws2_32[]` (networking)
  - [ ] `"advapi32.dll"` → `native_advapi32[]`
  - [ ] `"comctl32.dll"` → `native_comctl32[]`
- [ ] Commit: `"kernel: native DLL export table"`

### 4.3 IAT Patching

**Prompt:** Patch Import Address Table: for each import, look up the native DLL export table. If found, write function pointer to IAT slot. If not, write `stub_unimplemented()` that logs `"UNIMPL: dll!func"` and returns 0. PE code calls native functions via normal IAT indirect calls. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: IAT patching"`.


- [ ] For each imported function:
  - [ ] Look up in native DLL export table
  - [ ] If found: write function pointer into IAT slot
  - [ ] If not found: log warning, write `stub_unimplemented()` (prints "UNIMPL: dll!func")
- [ ] After patching, PE code calls native functions via normal IAT indirect calls
- [ ] Commit: `"kernel: IAT patching (import resolution)"`

### 4.4 External DLL Loading *(Stretch)*

**Prompt:** Stretch: search filesystem for DLLs not in the native export table. Load DLL as PE, parse export table. Handle recursive DLL deps. Call DllMain(DLL_PROCESS_ATTACH). Implement LoadLibraryA/GetProcAddress for runtime loading. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: external DLL loading"`.


- [ ] *(Stretch)* Search filesystem for DLLs not in native export table:
  - [ ] `C:\Impossible\System\` (system directory)
  - [ ] Same directory as the .exe
- [ ] *(Stretch)* Load external DLL as PE: parse export table, resolve by name/ordinal
- [ ] *(Stretch)* Handle DLL dependencies recursively
- [ ] *(Stretch)* Call `DllMain(DLL_PROCESS_ATTACH)` on load
- [ ] *(Stretch)* `LoadLibraryA(name)` / `GetProcAddress(handle, name)` runtime loading
- [ ] Commit: `"kernel: external DLL loading"`

---

## 5. Win32 API — Tier 1: Types, Console & Process

### 5.1 Windows Type Definitions

**Prompt:** Create `include/win32.h` with Windows types: HANDLE=void*, DWORD=uint32_t, BOOL=int, LPVOID=void*, LPCSTR=const char*. Constants: STD_INPUT/OUTPUT/ERROR_HANDLE, INVALID_HANDLE_VALUE, TRUE, FALSE. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: Win32 type definitions"`.


- [ ] Create `include/win32.h`
- [ ] Define Windows types: `HANDLE`, `DWORD`, `BOOL`, `LPVOID`, `LPCSTR`, `SIZE_T`, `UINT`, `LPARAM`, `WPARAM`
- [ ] Define constants: `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE`, `STD_ERROR_HANDLE`, `INVALID_HANDLE_VALUE`, `TRUE`, `FALSE`
- [ ] Define `HICON` type (opaque handle to `icon_bitmap_t`)
- [ ] Commit: `"kernel: Win32 type definitions"`

### 5.2 kernel32.dll — Console & Process

**Prompt:** First tier: console Hello World and process management. GetStdHandle, WriteConsoleA, ReadConsoleA are thin wrappers on native file I/O. ExitProcess calls sys_exit. File I/O functions (CreateFile, ReadFile, WriteFile, etc.) are **direct exports** of the native API (P0103 §3.6). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"win32: kernel32.dll console stubs"`.

> [!NOTE]
> File I/O functions (`CreateFile`, `ReadFile`, `WriteFile`, `CloseHandle`,
> `SetFilePointer`, `GetFileSize`, `FindFirstFile`, `FindNextFile`,
> `FindClose`, `DeleteFile`, `CreateDirectory`, `RemoveDirectory`,
> `MoveFile`, `CopyFile`, `GetFileAttributes`) are all **native**
> Impossible OS API defined in P0103 §3.6. The kernel32 export table
> simply exposes the same function pointers.

- [ ] Create `src/win32/kernel32.c`
- [ ] `GetStdHandle(nStdHandle)` → native `GetStdHandle()` (P0103 §3.6.1)
- [ ] `WriteConsoleA(h, buf, len, written, reserved)` → thin wrapper: calls `WriteFile()`
- [ ] `ReadConsoleA(h, buf, len, read, reserved)` → thin wrapper: calls `ReadFile()`
- [ ] File I/O exports → native API (P0103 §3.6):
  - [ ] `CreateFileA/W`, `ReadFile`, `WriteFile`, `CloseHandle`
  - [ ] `SetFilePointer`, `GetFileSize`
  - [ ] `FindFirstFileA`, `FindNextFileA`, `FindClose`
  - [ ] `DeleteFileA`, `CreateDirectoryA`, `RemoveDirectoryA`
  - [ ] `MoveFileA`, `CopyFileA`, `GetFileAttributesA`
  - [ ] `GetCurrentDirectoryA`, `SetCurrentDirectoryA`
- [ ] `ExitProcess(code)` → `SYS_EXIT`
- [ ] `GetCommandLineA/W()` → return command-line string
- [ ] `GetModuleHandleA(name)` → return ImageBase (NULL = current)
- [ ] `GetLastError()` → return thread-local error code
- [ ] `SetLastError(err)` → set thread-local error code
- [ ] Commit: `"win32: kernel32.dll console + process"`

### 5.3 msvcrt.dll — C Runtime

**Prompt:** Map C stdlib: printf/puts → format + sys_write, malloc/free/calloc/realloc → kernel heap, memcpy/memmove/memset/memcmp → kernel implementations, strlen/strcpy/strcmp → kernel string functions, sprintf/snprintf → kernel printf engine, atoi/atol for conversions. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"win32: msvcrt.dll C runtime"`.


- [ ] Create `src/win32/msvcrt.c`
- [ ] `printf(fmt, ...)` → format + `sys_write()` to stdout
- [ ] `puts(str)` → write string + newline
- [ ] `exit(code)` → `sys_exit()`
- [ ] `malloc(size)` → `kmalloc()` (or user heap)
- [ ] `free(ptr)` → `kfree()`
- [ ] `calloc(n, size)` → zero-initialized malloc
- [ ] `realloc(ptr, size)` → resize allocation
- [ ] `memcpy()`, `memmove()`, `memset()`, `memcmp()`
- [ ] `strlen()`, `strcpy()`, `strncpy()`, `strcmp()`, `strncmp()`
- [ ] `sprintf()`, `snprintf()`
- [ ] `atoi()`, `atol()`
- [ ] Commit: `"win32: msvcrt.dll C runtime"`

### 5.4 ntdll.dll — NT Runtime

**Prompt:** Stub minimal ntdll functions: RtlInitUnicodeString (no-op), NtCurrentTeb (return static dummy TEB), RtlGetVersion (return plausible version info). Most ntdll functions can be safely stubbed as no-ops for basic compatibility. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"win32: ntdll.dll minimal stubs"`.


- [ ] Create `src/win32/ntdll.c`
- [ ] `RtlInitUnicodeString()` → stub (many programs import it)
- [ ] `NtCurrentTeb()` → stub (Thread Environment Block)
- [ ] `RtlGetVersion()` → return OS version info
- [ ] Commit: `"win32: ntdll.dll minimal stubs"`

### 5.5 Test: Run Native PE Hello World

**Prompt:** Cross-compile `x86_64-w64-mingw32-gcc -o hello.exe hello.c`, include on C:\. Execute in shell — should print "Hello, World!" via WriteConsoleA. This is the milestone proving PE+IAT+Win32 all work. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: run first native PE program"`.


- [ ] Cross-compile test program: `x86_64-w64-mingw32-gcc -o hello.exe hello.c`
- [ ] Include on C:\
- [ ] Execute: `hello.exe` in shell → should print "Hello, World!" via WriteConsoleA
- [ ] Commit: `"kernel: run first native PE program"`

---

## 6. Win32 API — Tier 2: Memory & Registry

### 6.1 kernel32.dll — Memory Management

**Prompt:** VirtualAlloc maps to sys_mmap (MEM_COMMIT|MEM_RESERVE = allocate+zero), VirtualFree releases. HeapCreate/HeapAlloc/HeapFree/GetProcessHeap map to kernel heap. Add SYS_MMAP and SYS_BRK syscalls. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"win32: kernel32.dll memory management"`.


- [ ] `VirtualAlloc(addr, size, type, protect)` → `sys_mmap()`
- [ ] `VirtualFree(addr, size, type)` → `sys_munmap()`
- [ ] `HeapCreate(options, initial, max)` → create heap region
- [ ] `HeapAlloc(heap, flags, size)` → `kmalloc()` from heap
- [ ] `HeapFree(heap, flags, ptr)` → `kfree()`
- [ ] `GetProcessHeap()` → return default heap handle
- [ ] Add `SYS_MMAP` and `SYS_BRK` syscalls to `syscall.c`
- [ ] Commit: `"win32: kernel32.dll memory management"`

### 6.2 Registry API ✅

> Already implemented via the native Registry API.

- [x] `RegOpenKeyExA/W` → `registry_open()`
- [x] `RegQueryValueExA/W` → `registry_get()`
- [x] `RegSetValueExA/W` → `registry_set()`
- [x] `RegCloseKey` → `registry_close()`
- [x] `RegCreateKeyExA/W` → `registry_create()`
- [x] `RegEnumKeyExA/W` → `registry_enum()`

### 6.3 Windows Path Edge Cases

> [!NOTE]
> Path translation is built into the native `CreateFile()` (P0103 §3.6.2)
> — VFS already uses `C:\` drive-letter paths. Backslash-to-forward-slash
> normalization is handled in `CreateFile` itself.

- [ ] UNC paths: `"\\server\share"` → return `ERROR_NOT_SUPPORTED`
- [ ] Device paths: `"\\.\PhysicalDrive0"` → stub (return error)
- [ ] Extended-length paths: `"\\?\C:\very\long\path"` → strip prefix, pass to `CreateFile`
- [ ] Commit: `"win32: Windows path edge cases"`

---

## 7. Win32 API — Tier 3: GUI

### 7.1 user32.dll — Window Management & MessageBox

**Prompt:** Map native GUI APIs. RegisterClassExW registers window class, CreateWindowExW → wm_create_window, ShowWindow makes visible. GetMessage/TranslateMessage/DispatchMessage implement message loop. MessageBoxA/W is a direct pass-through to `MessageBox()` (P0104, same ABI). After all items, mark `[x]`, run `bash scripts/build.sh clean`, commit `"win32: user32.dll window management"`.


- [ ] Create `src/win32/user32.c`:
  - [ ] `RegisterClassExW()` → register window class with WM
  - [ ] `CreateWindowExW()` → `wm_create_window()`
  - [ ] `ShowWindow()` → `wm_show_window()`
  - [ ] `GetMessage()` → WM event queue poll
  - [ ] `TranslateMessage()` → convert key events
  - [ ] `DispatchMessage()` → call window procedure
  - [ ] `DefWindowProc()` → default message handling
  - [ ] `MessageBoxA/W()` → native `MessageBox()` (P0104)
  - [ ] `PostQuitMessage()` → exit message loop

### 7.2 gdi32.dll — Graphics Rendering

- [ ] Create `src/win32/gdi32.c`:
  - [ ] `CreateDC()` → create device context (surface wrapper)
  - [ ] `BitBlt()` → `gfx_blit()`
  - [ ] `TextOutA/W()` → TrueType text render
  - [ ] `FillRect()` → `gfx_fill_rect()`
  - [ ] `DrawText()` → gfx text primitives
  - [ ] `BeginPaint()` / `EndPaint()` → get drawable surface
  - [ ] `GetDC()` / `ReleaseDC()`
  - [ ] `SetPixel()` / `GetPixel()` → framebuffer access
- [ ] Test: PE app with a window and "Hello World" text → renders on desktop
- [ ] Commit: `"win32: user32.dll + gdi32.dll"`

### 7.3 shell32.dll — Shell Icon API

**Prompt:** Implement shell icon API functions. ExtractIconEx uses the PE resource parser (§3.5) for `.exe`/`.dll` files and `ico_load()` for `.ico` files. SHGetFileInfo delegates to `icon_for_extension()`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"win32: shell32.dll icon API"`.


- [ ] Create `src/win32/shell32.c`
- [ ] `ExtractIconExA(path, index, phiconLarge, phiconSmall, nIcons)`:
  - [ ] System DLL → `win32_icon_lookup()` → `icon_get()`
  - [ ] `.ico` file → `ico_load()` (P0201 §4.6)
  - [ ] `.dll`/`.exe` → PE resource parser (§3.5) for RT_GROUP_ICON
- [ ] `SHGetFileInfoA(path, ...)` → `icon_for_extension()`
- [ ] `SHGetStockIconInfo(siid, ...)` → SHSTOCKICONID mapping
- [ ] `LoadIconA()` / `LoadImageA()` → PE resource extraction
- [ ] `DestroyIcon(hIcon)` → release cached handle
- [ ] Commit: `"win32: shell32.dll icon API"`

### 7.4 comctl32.dll — Common Controls *(Stretch)*

**Prompt:** Map Windows common controls to Impossible OS controls library. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"win32: common controls"`.


- [ ] `InitCommonControlsEx` → no-op (controls always available)
- [ ] `CreateStatusWindow` → `ctrl_create_statusbar()` (120-Theme §2.10)
- [ ] `CreateToolbarEx` → `ctrl_create_toolbar()` (120-Theme §2.8)
- [ ] `CreateUpDownControl` → map to slider
- [ ] ListView, TreeView → `ctrl_create_listview/treeview` (120-Theme §2.6–2.7)
- [ ] Commit: `"win32: common controls"`

---

## 8. Native SDK (Headers & Cross-Compiler)

**Prompt:** Create the Impossible OS SDK so developers can compile native PE programs. The SDK includes: (1) `windows.h` — exports the Win32 API, (2) `impossible.h` — Impossible OS extensions (IxUI, system info), (3) import libraries (`kernel32.lib`, `user32.lib`) for the PE linker. Cross-compilation uses `x86_64-w64-mingw32-gcc` with our headers. Create a wrapper script `impossible-cc` that sets the correct include paths and libraries. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"sdk: Impossible OS SDK"`.


- [ ] Create `sdk/include/windows.h` — Win32 API declarations
  - [ ] File I/O: `CreateFile`, `ReadFile`, `WriteFile`, `CloseHandle`, `FindFirstFile`, etc.
  - [ ] Memory: `VirtualAlloc`, `VirtualFree`, `HeapAlloc`, `HeapFree`
  - [ ] Process: `ExitProcess`, `GetCommandLine`, `CreateProcess`
  - [ ] Console: `WriteConsoleA`, `ReadConsoleA`, `GetStdHandle`
  - [ ] GUI: `MessageBox`, `CreateWindow` (wraps IxUI)
- [ ] Create `sdk/include/impossible.h` — IxOS-specific extensions
- [ ] Create `sdk/lib/kernel32.lib` — import library for PE linker
- [ ] Create `sdk/lib/user32.lib` — import library for PE linker
- [ ] Create `tools/impossible-cc` wrapper script:
  ```bash
  x86_64-w64-mingw32-gcc -I$SDK/include -L$SDK/lib -lkernel32 -luser32 "$@"
  ```
- [ ] Test: `impossible-cc hello.c -o hello.exe` → produces valid PE
- [ ] Commit: `"sdk: Impossible OS SDK"`

---

## 9. Native GUI Toolkit (IxUI)

**Prompt:** Build a native GUI toolkit called IxUI that wraps the kernel's window manager and compositor. User programs call IxUI functions via syscalls. The IxUI API uses Win32-compatible patterns: `CreateWindow(className, title, style, x, y, w, h, parent, menu, hInstance, param)`, `ShowWindow(hWnd, nCmdShow)`, `SendMessage(hWnd, msg, wParam, lParam)`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"sdk: IxUI native GUI toolkit"`.


- [ ] Create `sdk/include/ixui.h` (userland-facing API header)
- [ ] `CreateWindow(className, title, style, x, y, w, h, parent, ...)` → syscall → `wm_create()`
- [ ] `ShowWindow(hWnd, nCmdShow)` / `DestroyWindow(hWnd)`
- [ ] `DrawText(hDC, lpString, nCount, lpRect, uFormat)` → TrueType render
- [ ] `FillRect(hDC, lprc, hBrush)` → `gfx_fill_rect()`
- [ ] `MessageBox(hWnd, lpText, lpCaption, uType)` → `SYS_MSGBOX` (P0104)
- [ ] `GetMessage(lpMsg, hWnd, ...)` → event dispatch (paint, click, key, resize)
- [ ] `DispatchMessage(lpMsg)` → call window procedure
- [ ] Build as `sdk/lib/libixui.a` (static library for userland programs)
- [ ] Test: native PE app with window, text, button click → works in ring 3
- [ ] Commit: `"sdk: IxUI native GUI toolkit"`

---

## 10. Unimplemented Function Logger

**Prompt:** When PE calls unimplemented function, log `"UNIMPL: kernel32.dll!CreateThread"` to serial. Return safe default (0/NULL/FALSE). Track call counts. Shell `win32log` dumps log sorted by count to prioritize next implementations. After all items, mark `[x]`, run `bash scripts/build.sh clean`, commit `"win32: unimplemented function logger"`.


- [ ] When a PE program calls an unimplemented function:
  - [ ] Log to serial: `"UNIMPL: kernel32.dll!CreateThread"`
  - [ ] Return safe default (0 / NULL / FALSE) instead of crashing
  - [ ] Track call counts for unimplemented functions
- [ ] Shell command: `win32log` — show unimplemented function call log
- [ ] Helps identify which functions to implement next
- [ ] Commit: `"win32: unimplemented function logger"`

---

## 11. Shell Integration

**Prompt:** Integrate PE execution into the shell and desktop. Auto-detect PE executables by their `MZ` magic bytes. File extension `.exe` is recognized and routed to the PE loader. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"shell: PE exe support"`.


- [ ] Detect PE magic bytes (`MZ`, `0x4D 0x5A`) in `SYS_EXEC`
- [ ] Route `.exe` files to PE loader automatically
- [ ] File Manager: double-click `.exe` → run via PE loader
- [ ] Commit: `"shell: PE exe support"`

---

## 12. Documentation

- [ ] Create `docs/architecture/pe-loader.md`
- [ ] Create `docs/architecture/native-abi.md`
- [ ] Document PE loader: header parsing, section mapping, relocation
- [ ] Document import resolution: native DLL export table, IAT patching
- [ ] Document supported kernel32/user32/gdi32 functions with mapping table
- [ ] Document calling convention (ms_abi)
- [ ] Document how to compile: `x86_64-w64-mingw32-gcc hello.c -o hello.exe`
- [ ] Document known limitations and unimplemented features
- [ ] Commit: `"docs: native Win32 API documentation"`

---

## Priority Order

| Priority | Section                          | Reason                            |
|----------|----------------------------------|-----------------------------------|
| 🔴 P0     | §1 Fix User-Mode                 | Prerequisite for everything       |
| 🔴 P0     | §2 Syscall Interface             | Foundation for all API calls      |
| 🔴 P0     | §3.1–3.2 PE Loader               | Load native PE binaries           |
| 🔴 P0     | §4.1–4.3 Import Resolution + IAT | Connect PE programs to native API |
| 🔴 P0     | §5.1–5.2 Type Defs + Console     | Run first PE "Hello World"        |
| 🟠 P1     | §5.3–5.4 msvcrt + ntdll          | C runtime for compiled programs   |
| 🟠 P1     | §10 Unimplemented Logger         | Debug API coverage                |
| 🟠 P1     | §11 Shell Integration            | Auto-detect .exe                  |
| 🟡 P2     | §6.1 Memory Management           | VirtualAlloc / HeapAlloc          |
| 🟡 P2     | §3.3 Base Relocation             | Load at non-preferred addresses   |
| 🟡 P2     | §8 SDK (headers + libs)          | Developer toolchain               |
| 🟢 P3     | §3.5 PE Resource Parser          | Icons from .exe/.dll              |
| 🟢 P3     | §7.1–7.2 GUI (user32 + gdi32)    | Window creation + rendering       |
| 🟢 P3     | §7.3 Shell Icon API              | ExtractIcon, SHGetFileInfo        |
| 🟢 P3     | §9 IxUI Toolkit                  | Native GUI library                |
| 🔵 P4     | §7.4 Common Controls             | comctl32 widgets                  |
| 🔵 P4     | §4.4 External DLL Loading        | Load real PE DLLs                 |

---

## Milestone Targets

**Target 1: Console Hello World**
- [ ] MinGW `hello.exe` (WriteConsoleA + ExitProcess) → runs, prints text

**Target 2: MessageBox**
- [ ] MinGW app calling `MessageBoxA("Hello", "Win32 on Impossible OS", MB_OK)` → dialog appears

**Target 3: File Operations**
- [ ] MinGW app using CreateFile/ReadFile/WriteFile → reads and writes files

**Target 4: GUI Window** *(Stretch)*
- [ ] MinGW app with CreateWindow + message loop → window renders on desktop

---

## Key Files

| File                         | Purpose                                  |
|------------------------------|------------------------------------------|
| `include/pe.h`               | [NEW] PE/COFF header structures          |
| `include/win32.h`            | [NEW] Windows type definitions           |
| `include/kernel/sched/abi.h` | [NEW] Syscall numbers and ABI constants  |
| `src/kernel/pe.c`            | [NEW] PE executable loader               |
| `src/win32/kernel32.c`       | [NEW] kernel32.dll native implementation |
| `src/win32/user32.c`         | [NEW] user32.dll native implementation   |
| `src/win32/msvcrt.c`         | [NEW] C runtime implementation           |
| `src/win32/ntdll.c`          | [NEW] NT runtime stubs                   |
| `src/win32/gdi32.c`          | [NEW] GDI rendering (stretch)            |
| `src/win32/shell32.c`        | [NEW] Shell icon API                     |
| `src/win32/comctl32.c`       | [NEW] Common controls (stretch)          |
| `src/win32/win32_import.c`   | [NEW] IAT import resolver                |
| `sdk/include/windows.h`      | [NEW] Win32 API header for SDK           |
| `sdk/include/impossible.h`   | [NEW] Impossible OS extensions           |
| `sdk/include/ixui.h`         | [NEW] Native GUI toolkit header          |
| `sdk/lib/kernel32.lib`       | [NEW] PE import library                  |
| `sdk/lib/libixui.a`          | [NEW] IxUI static library                |
| `tools/impossible-cc`        | [NEW] Cross-compiler wrapper script      |

---

## Effort Estimates

| Component                       | Effort          | Dependencies                    |
|---------------------------------|-----------------|---------------------------------|
| Fix user-mode                   | Medium          | TSS/GDT debugging               |
| Syscall interface               | Low             | User-mode working               |
| PE loader + headers             | Weeks           | VMM, user-mode                  |
| Import resolution + IAT         | Weeks           | PE loader                       |
| Calling convention              | Days            | PE loader                       |
| kernel32 console                | Weeks           | PE loader, native syscalls      |
| msvcrt + ntdll                  | Weeks           | kernel32                        |
| File I/O                        | **Done** ✅      | Native API (P0103 §3.6)         |
| Registry                        | **Done** ✅      | Native Registry API             |
| MessageBox                      | **Done** ✅      | Native MessageBox (P0104)       |
| Memory management               | Days            | SYS_MMAP                        |
| SDK headers + libs              | Medium          | Win32 API defined               |
| IxUI toolkit                    | Medium          | WM syscalls, native ABI         |
| PE resource parser              | Weeks           | PE loader                       |
| Shell icon API                  | Weeks           | PE resources, icon system       |
| GDI subset                      | Months          | WM, gfx, full message loop      |
| Common controls                 | Months          | GDI, widget toolkit (120-Theme) |
| **Total (core: PE + kernel32)** | **~4–6 weeks**  |                                 |
| **Total (full with GDI)**       | **~6–9 months** |                                 |
