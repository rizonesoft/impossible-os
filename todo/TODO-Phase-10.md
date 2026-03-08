# Phase 10 — Compatibility & Internationalization

> **Goal:** Make Impossible OS a **tri-format platform** that natively runs ELF,
> Windows PE, and macOS Mach-O executables — the only hobby OS to load all three
> major binary formats. Provide Win32 and macOS API compatibility through stub
> libraries, support international keyboard layouts and Unicode, and optionally
> run Java bytecode — creating a truly versatile operating system.

---

## 1. PE Binary Loader
> *Research: [01_pe_native_format.md](research/phase_10_compatibility/01_pe_native_format.md), [02_dual_format_subsystem.md](research/phase_10_compatibility/02_dual_format_subsystem.md)*

### 1.1 PE Header Structures

- [ ] Create `include/pe.h` (~80 lines)
- [ ] Define `struct pe_dos_header` (e_magic "MZ", e_lfanew offset to PE signature)
- [ ] Define `struct pe_coff_header` (machine, num_sections, characteristics)
- [ ] Define `struct pe_optional_header_64` (PE32+ magic `0x020B`, image_base, entry_point_rva, section_alignment, size_of_image, num_data_dirs)
- [ ] Define `struct pe_section_header` (name, virtual_size, virtual_address, raw_data_size, raw_data_offset, characteristics)
- [ ] Define `struct pe_data_directory` (rva, size) — for import table, export table, relocation table
- [ ] Define PE constants: `PE_DOS_MAGIC`, `PE_SIGNATURE`, `PE_MACHINE_AMD64`, `PE32PLUS_MAGIC`
- [ ] Commit: `"kernel: PE/COFF header structures"`

### 1.2 PE Loader Core

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

### 1.3 Tri-Format Detection

- [ ] Create `load_binary(data, size)` in `src/kernel/task.c` (or new file)
- [ ] Auto-detect format by magic bytes:
  - [ ] `"MZ"` (0x5A4D) → `pe_load()` — Windows PE
  - [ ] `"\x7FELF"` (0x464C457F) → `elf_load()` — native ELF
  - [ ] `0xFEEDFACF` → `macho_load()` — macOS Mach-O (64-bit)
  - [ ] `0xFEEDFACE` → reject with "32-bit Mach-O not supported"
  - [ ] Unknown → return error
- [ ] Update exec path: replace direct `elf_load()` call with `load_binary()`
- [ ] All three loaders return identical `struct load_result` (entry_point, success)
- [ ] Commit: `"kernel: tri-format binary detection (ELF + PE + Mach-O)"`

### 1.4 Base Relocation

- [ ] Parse DataDirectory[5] (Base Relocation Table) from PE
- [ ] If PE loaded at address ≠ preferred ImageBase:
  - [ ] Calculate delta = actual_base − preferred_base
  - [ ] Walk relocation blocks: for each `IMAGE_REL_BASED_DIR64` entry, add delta to address
- [ ] Skip relocation if loaded at preferred address
- [ ] Commit: `"kernel: PE base relocation support"`

---

## 2. Import Resolution & DLL System
> *Research: [01_pe_native_format.md](research/phase_10_compatibility/01_pe_native_format.md), [02_dual_format_subsystem.md](research/phase_10_compatibility/02_dual_format_subsystem.md)*

### 2.1 Import Table Parser

- [ ] Parse Import Directory Table from DataDirectory[1]
- [ ] For each DLL entry: read DLL name string, Import Lookup Table (ILT), Import Address Table (IAT)
- [ ] For each function: read name (or ordinal) from Hint/Name Table
- [ ] Log imported DLLs and functions: `"PE imports: kernel32.dll!WriteConsoleA"`
- [ ] Commit: `"kernel: PE import table parser"`

### 2.2 Builtin DLL Stub Table

- [ ] Define `struct win32_export` (name, func_ptr)
- [ ] Create lookup function: `pe_resolve_builtin(dll_name, func_name)` → function pointer
- [ ] Builtin DLL registry:
  - [ ] `"kernel32.dll"` → `builtin_kernel32[]`
  - [ ] `"msvcrt.dll"` → `builtin_msvcrt[]`
  - [ ] `"ntdll.dll"` → `builtin_ntdll[]`
  - [ ] `"user32.dll"` → `builtin_user32[]` (future)
  - [ ] `"gdi32.dll"` → `builtin_gdi32[]` (future)
  - [ ] `"ws2_32.dll"` → `builtin_ws2_32[]` (from Phase 07)
  - [ ] `"advapi32.dll"` → `builtin_advapi32[]` (future)
- [ ] Commit: `"kernel: builtin DLL stub table"`

### 2.3 IAT Patching

- [ ] For each imported function:
  - [ ] Look up in builtin DLL table first
  - [ ] If found: write stub function pointer into IAT slot
  - [ ] If not found: log warning, write `stub_unimplemented()` (prints "UNIMPL: dll!func")
- [ ] After patching, PE code calls our stubs via normal function call through IAT
- [ ] Commit: `"kernel: IAT patching (import resolution)"`

### 2.4 External DLL Loading (Future)

- [ ] *(Stretch)* Search filesystem for DLLs not in builtin table:
  - [ ] `C:\Windows\System32\{name}`
  - [ ] Same directory as the .exe
- [ ] *(Stretch)* Load external DLL as PE: parse export table, resolve by name/ordinal
- [ ] *(Stretch)* Handle DLL dependencies recursively
- [ ] *(Stretch)* Call `DllMain(DLL_PROCESS_ATTACH)` on load
- [ ] *(Stretch)* `LoadLibraryA(name)` / `GetProcAddress(handle, name)` runtime loading
- [ ] Commit: `"kernel: external DLL loading"`

---

## 3. Win32 API Stubs — Tier 1 (Console)
> *Research: [01_pe_native_format.md](research/phase_10_compatibility/01_pe_native_format.md)*

### 3.1 Windows Type Definitions

- [ ] Create `include/win32.h`
- [ ] Define Windows types: `HANDLE`, `DWORD`, `BOOL`, `LPVOID`, `LPCSTR`, `SIZE_T`, `UINT`, `LPARAM`, `WPARAM`
- [ ] Define constants: `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE`, `STD_ERROR_HANDLE`, `INVALID_HANDLE_VALUE`, `TRUE`, `FALSE`
- [ ] Commit: `"kernel: Win32 type definitions"`

### 3.2 kernel32.dll — Console & Process

- [ ] Create `src/kernel/win32/kernel32.c`
- [ ] `GetStdHandle(nStdHandle)` → return fd 0/1/2 as HANDLE
- [ ] `WriteConsoleA(h, buf, len, written, reserved)` → `sys_write()`
- [ ] `ReadConsoleA(h, buf, len, read, reserved)` → `sys_read()`
- [ ] `ExitProcess(code)` → `sys_exit()`
- [ ] `GetCommandLineA()` → return command-line string
- [ ] `GetModuleHandleA(name)` → return ImageBase (NULL = current)
- [ ] `GetLastError()` → return thread-local error code
- [ ] `SetLastError(err)` → set thread-local error code
- [ ] Commit: `"win32: kernel32.dll console + process stubs"`

### 3.3 msvcrt.dll — C Runtime

- [ ] Create `src/kernel/win32/msvcrt.c`
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
- [ ] Commit: `"win32: msvcrt.dll C runtime stubs"`

### 3.4 ntdll.dll — NT Runtime

- [ ] Create `src/kernel/win32/ntdll.c`
- [ ] `RtlInitUnicodeString()` → stub (many programs import it)
- [ ] `NtCurrentTeb()` → stub (Thread Environment Block)
- [ ] `RtlGetVersion()` → return OS version info
- [ ] Commit: `"win32: ntdll.dll minimal stubs"`

### 3.5 Test: Run Windows Hello World

- [ ] Cross-compile test program: `x86_64-w64-mingw32-gcc -o hello.exe hello.c`
- [ ] Include in initrd
- [ ] Execute: `hello.exe` in shell → should print "Hello, World!" via WriteConsoleA
- [ ] Commit: `"kernel: run first Windows PE program"`

---

## 4. Win32 API Stubs — Tier 2 (File I/O & Memory)
> *Research: [01_pe_native_format.md](research/phase_10_compatibility/01_pe_native_format.md)*

### 4.1 kernel32.dll — File I/O

- [ ] `CreateFileA(filename, access, ...)` → `vfs_open()` with Windows path translation
- [ ] `CreateFileW(filename, access, ...)` → UTF-16 → UTF-8, then `vfs_open()`
- [ ] `ReadFile(handle, buf, len, read, overlapped)` → `vfs_read()`
- [ ] `WriteFile(handle, buf, len, written, overlapped)` → `vfs_write()`
- [ ] `CloseHandle(handle)` → `vfs_close()`
- [ ] `GetFileSize(handle, high)` → `vfs_stat()`
- [ ] `SetFilePointer(handle, offset, high, method)` → `vfs_seek()`
- [ ] `GetCurrentDirectoryA(len, buf)` → return CWD
- [ ] `SetCurrentDirectoryA(path)` → set CWD
- [ ] `FindFirstFileA(pattern, data)` → VFS directory scan
- [ ] `FindNextFileA(handle, data)` → next directory entry
- [ ] `FindClose(handle)` → close scan
- [ ] `DeleteFileA(path)` → `vfs_delete()`
- [ ] `CreateDirectoryA(path, attr)` → `vfs_create_dir()`
- [ ] Commit: `"win32: kernel32.dll file I/O stubs"`

### 4.2 kernel32.dll — Memory Management

- [ ] `VirtualAlloc(addr, size, type, protect)` → `sys_mmap()`
- [ ] `VirtualFree(addr, size, type)` → `sys_munmap()`
- [ ] `HeapCreate(options, initial, max)` → create heap region
- [ ] `HeapAlloc(heap, flags, size)` → `kmalloc()` from heap
- [ ] `HeapFree(heap, flags, ptr)` → `kfree()`
- [ ] `GetProcessHeap()` → return default heap handle
- [ ] Add `SYS_MMAP` and `SYS_BRK` syscalls to `syscall.c`
- [ ] Commit: `"win32: kernel32.dll memory management stubs"`

### 4.3 Windows Path Translation

- [ ] `win32_path_to_vfs(win_path, vfs_path)` — translate Windows paths to VFS:
  - [ ] `"C:\\Users\\Derick\\file.txt"` → `"C:/Users/Derick/file.txt"`
  - [ ] Backslash → forward slash
  - [ ] Drive letters preserved (VFS already uses them)
- [ ] Handle relative paths (resolve against CWD)
- [ ] Handle UNC paths: `"\\\\server\\share"` → stub (not supported, return error)
- [ ] Commit: `"win32: Windows path translation"`

---

## 5. Calling Convention Bridge
> *Research: [02_dual_format_subsystem.md](research/phase_10_compatibility/02_dual_format_subsystem.md)*

### 5.1 Windows x64 Convention

- [ ] Win32 stubs compiled with Windows x64 convention: args in RCX, RDX, R8, R9
- [ ] Stubs internally use inline `INT 0x80` assembly with explicit register setup
- [ ] No automatic convention conflict since syscalls bypass C ABI
- [ ] Commit: `"kernel: Win32 stubs with Windows x64 calling convention"`

### 5.2 Convention Thunks (Future)

- [ ] *(Stretch)* Windows-to-SysV thunk: `mov rdi, rcx; mov rsi, rdx; mov rdx, r8; mov rcx, r9; jmp target`
- [ ] *(Stretch)* SysV-to-Windows thunk: reverse register mapping
- [ ] *(Stretch)* Needed if ELF libraries call PE functions or vice versa

---

## 6. Keyboard Layouts & Internationalization
> *Research: [03_keyboard_i18n.md](research/phase_10_compatibility/03_keyboard_i18n.md)*

### 6.1 Keyboard Layout System

- [ ] Create `src/kernel/kbd_layout.c` and `include/kbd_layout.h`
- [ ] Define `struct kbd_layout` (name, code, normal[128], shift[128], altgr[128])
- [ ] Replace hardcoded US QWERTY scancode→ASCII table in `keyboard.c` with layout lookup
- [ ] `kbd_set_layout(code)` — switch active layout
- [ ] `kbd_get_layout()` — return current layout code
- [ ] Codex: `System\Input\KeyboardLayout = "en-US"`
- [ ] Commit: `"kernel: keyboard layout system"`

### 6.2 Built-in Layouts

- [ ] Create `resources/layouts/` directory with layout data
- [ ] **US English (QWERTY)** — `en-US` (default)
- [ ] **UK English** — `en-GB` (different symbols: £ vs $, @ position)
- [ ] **German (QWERTZ)** — `de-DE` (Z/Y swapped, umlauts on AltGr)
- [ ] **French (AZERTY)** — `fr-FR` (A/Q, Z/W swapped, accents)
- [ ] **Spanish** — `es-ES` (ñ, accents)
- [ ] **Dvorak** — `en-DV` (alternative layout)
- [ ] Commit: `"kernel: built-in keyboard layouts (6 layouts)"`

### 6.3 Layout Switching

- [ ] Win+Space → cycle through installed layouts
- [ ] System tray indicator: show current layout code (`EN`, `FR`, `DE`)
- [ ] Click tray indicator → layout picker popup
- [ ] Input → keyboard settings applet for layout management
- [ ] Commit: `"desktop: keyboard layout switching (Win+Space)"`

### 6.4 Unicode / UTF-8 Support

- [ ] Store all text strings internally as UTF-8
- [ ] UTF-8 encode/decode helpers: `utf8_encode(codepoint, buf)`, `utf8_decode(buf, codepoint_out)`
- [ ] Keyboard input: convert layout output to UTF-8 codepoints
- [ ] Font rendering: `stb_truetype` already supports Unicode codepoints
- [ ] *(Stretch)* Noto Sans as fallback font (covers all Unicode scripts)
- [ ] Commit: `"kernel: UTF-8 Unicode support"`

### 6.5 Localization Framework (Future)

- [ ] *(Stretch)* UI string files: `C:\Impossible\System\Locale\{code}.ini`
- [ ] *(Stretch)* `locale_get(key)` — return localized string for current locale
- [ ] *(Stretch)* Default: `en-US.ini`, additional: `fr-FR.ini`, `de-DE.ini`, `es-ES.ini`
- [ ] *(Stretch)* All UI elements use `locale_get()` instead of hardcoded strings
- [ ] Commit: `"kernel: localization framework"`

---

## 7. Java Runtime (Optional)
> *Research: [04_java_runtime.md](research/phase_10_compatibility/04_java_runtime.md)*

### 7.1 GraalVM Native Images (Approach B — Recommended First)

- [ ] *(Stretch)* Compile Java programs to native ELF/PE on host using `native-image`
- [ ] *(Stretch)* Include compiled binary in initrd
- [ ] *(Stretch)* Execute like any other ELF/PE program — no JVM needed at runtime
- [ ] *(Stretch)* Prerequisite: working ELF loader + enough syscalls (`mmap`, file I/O)

### 7.2 Mini-JVM Bytecode Interpreter (Approach C — Educational)

- [ ] *(Stretch)* Create `src/apps/jvm/jvm.c` (~2000-4000 lines)
- [ ] *(Stretch)* Parse `.class` file format: magic, constant pool, methods, code attribute
- [ ] *(Stretch)* Implement operand stack (per method frame)
- [ ] *(Stretch)* Implement bytecode interpreter loop (~40 essential opcodes):
  - [ ] Constants: `iconst_0`–`iconst_5`, `ldc`
  - [ ] Arithmetic: `iadd`, `isub`, `imul`, `idiv`
  - [ ] Variables: `iload`, `istore`, `aload`, `astore`
  - [ ] Control: `if_icmpge`, `goto`, `ireturn`, `return`
  - [ ] Objects: `new`, `invokespecial`, `invokevirtual`
  - [ ] I/O: `getstatic` (System.out), `invokevirtual` (println)
- [ ] *(Stretch)* Implement `System.out.println(String)` → `sys_write()`
- [ ] *(Stretch)* Shell command: `java HelloWorld.class` → run
- [ ] *(Stretch)* Test: "Hello, World!" Java program

### 7.3 JamVM Port (Approach A — Full JVM)

- [ ] *(Stretch)* Port JamVM interpreter (~15K lines, GPL 2.0)
- [ ] *(Stretch)* Prerequisites: `mmap()`, file I/O, threading
- [ ] *(Stretch)* Covers: classes, methods, strings, arrays, exceptions
- [ ] *(Stretch)* Load `.jar` files (ZIP parsing via miniz)

---

## 8. Mach-O Binary Loader (macOS Compatibility)

> **Unique differentiator:** Impossible OS as a tri-format platform — the only hobby
> OS that natively loads ELF + PE + Mach-O executables.

### 8.1 Mach-O Header Structures

- [ ] Create `include/macho.h` (~80 lines)
- [ ] Define `struct mach_header_64` (magic `0xFEEDFACF`, cputype `CPU_TYPE_X86_64`, ncmds, sizeofcmds)
- [ ] Define `struct load_command` (cmd, cmdsize) — base for all load commands
- [ ] Define `struct segment_command_64` (cmd `LC_SEGMENT_64`, segname, vmaddr, vmsize, fileoff, filesize, nsects)
- [ ] Define `struct section_64` (sectname, segname, addr, size, offset)
- [ ] Define Mach-O constants: `MH_MAGIC_64`, `CPU_TYPE_X86_64`, `LC_SEGMENT_64`, `LC_MAIN`, `LC_LOAD_DYLIB`, `LC_SYMTAB`, `LC_DYSYMTAB`
- [ ] Commit: `"kernel: Mach-O header structures"`

### 8.2 Mach-O Loader Core

- [ ] Create `src/kernel/macho.c` (~300 lines)
- [ ] Implement `macho_load(data, size)`:
  - [ ] Validate header: `magic == 0xFEEDFACF` (64-bit)
  - [ ] Verify `cputype == CPU_TYPE_X86_64 (0x01000007)`
  - [ ] Walk load commands (`ncmds` entries):
    - [ ] `LC_SEGMENT_64`: map each segment (`__TEXT`, `__DATA`, `__DATA_CONST`, `__LINKEDIT`)
    - [ ] `LC_MAIN`: read entry point offset (replaces deprecated `LC_UNIXTHREAD`)
    - [ ] `LC_LOAD_DYLIB`: collect required dylib names
  - [ ] For each segment: allocate vmsize at vmaddr, memcpy filesize bytes from fileoff
  - [ ] Zero-fill remainder (vmsize − filesize = BSS-like)
  - [ ] Return entry point from `LC_MAIN` offset + `__TEXT` vmaddr
- [ ] Commit: `"kernel: Mach-O loader core"`

### 8.3 Mach-O Dylib Import Resolution

- [ ] Parse `LC_LOAD_DYLIB` commands to find required libraries:
  - [ ] `/usr/lib/libSystem.B.dylib` — macOS system library (libc + POSIX)
  - [ ] `/usr/lib/libc++.1.dylib` — C++ standard library (future)
- [ ] Parse `LC_SYMTAB` + `LC_DYSYMTAB` for symbol table and indirect symbols
- [ ] Read `__stubs` + `__la_symbol_ptr` sections in `__DATA` segment
- [ ] Patch lazy symbol pointers with our stub function addresses
- [ ] Builtin dylib registry:
  - [ ] `"libSystem.B.dylib"` → `builtin_libsystem[]`
- [ ] Commit: `"kernel: Mach-O dylib import resolution"`

### 8.4 libSystem.B.dylib Stubs (macOS API)

- [ ] Create `src/kernel/macos/libsystem.c`
- [ ] macOS uses **System V x64 calling convention** (same as ELF!) — no register bridge needed
- [ ] Console I/O stubs:
  - [ ] `write(fd, buf, len)` → `sys_write()`
  - [ ] `read(fd, buf, len)` → `sys_read()`
  - [ ] `_exit(code)` → `sys_exit()`
- [ ] Memory stubs:
  - [ ] `malloc(size)` → `kmalloc()`
  - [ ] `free(ptr)` → `kfree()`
  - [ ] `calloc(n, size)` → zero-initialized malloc
  - [ ] `realloc(ptr, size)` → resize
  - [ ] `mmap(addr, len, prot, flags, fd, off)` → `sys_mmap()`
  - [ ] `munmap(addr, len)` → `sys_munmap()`
- [ ] String stubs:
  - [ ] `printf(fmt, ...)` → format + `sys_write()`
  - [ ] `puts(str)` → write + newline
  - [ ] `strlen()`, `strcpy()`, `strcmp()`, `memcpy()`, `memset()`
- [ ] File I/O stubs:
  - [ ] `open(path, flags)` → `vfs_open()`
  - [ ] `close(fd)` → `vfs_close()`
  - [ ] `stat(path, buf)` → `vfs_stat()`
- [ ] Process stubs:
  - [ ] `getpid()` → return current process ID
  - [ ] `abort()` → `sys_exit(134)`
- [ ] Commit: `"macos: libSystem.B.dylib stubs"`

### 8.5 macOS Path Translation

- [ ] Translate macOS-style paths to VFS:
  - [ ] `/Users/name/file.txt` → `C:\Users\name\file.txt`
  - [ ] `/tmp/` → `C:\Temp\`
  - [ ] `/` → `C:\`
- [ ] Forward slash preserved (VFS already supports it)
- [ ] Commit: `"macos: path translation"`

### 8.6 Test: Run macOS Hello World

- [ ] Cross-compile test: `clang -target x86_64-apple-macos -o hello hello.c` (on macOS host)
- [ ] Or: hand-craft minimal Mach-O binary with `write()` + `_exit()` syscalls
- [ ] Include in initrd
- [ ] Execute: `hello` → prints "Hello from macOS binary!"
- [ ] Commit: `"kernel: run first Mach-O program"`

### 8.7 Unimplemented macOS Function Logger

- [ ] Same pattern as Win32 logger (§9.3): log unresolved dylib symbols
- [ ] `"UNIMPL: libSystem.B.dylib!pthread_create"`
- [ ] Return safe default instead of crashing
- [ ] Commit: `"macos: unimplemented function logger"`

---

## 9. Agent-Recommended Additions

> Items not in the research files but important for a complete compatibility layer.

### 9.1 Win32 GUI Stubs — Tier 3 (Future)

- [ ] *(Stretch)* `src/kernel/win32/user32.c`:
  - [ ] `RegisterClassExW()` → register window class with WM
  - [ ] `CreateWindowExW()` → `wm_create_window()`
  - [ ] `ShowWindow()` → `wm_show_window()`
  - [ ] `GetMessage()` → WM event queue poll
  - [ ] `TranslateMessage()` → convert key events
  - [ ] `DispatchMessage()` → call window procedure
  - [ ] `DefWindowProc()` → default message handling
  - [ ] `MessageBoxA()` → show dialog
  - [ ] `PostQuitMessage()` → exit message loop
- [ ] *(Stretch)* `src/kernel/win32/gdi32.c`:
  - [ ] `CreateDC()` → create device context (surface wrapper)
  - [ ] `BitBlt()` → `gfx_blit()`
  - [ ] `TextOutA()` → `font_draw_string()`
  - [ ] `SetPixel()` / `GetPixel()` → framebuffer access
- [ ] Commit: `"win32: user32.dll + gdi32.dll GUI stubs"`

### 9.2 PE Resource Section Parser

- [ ] *(Stretch)* Parse DataDirectory[2] (Resource Table)
- [ ] *(Stretch)* Extract embedded icons for taskbar/window title
- [ ] *(Stretch)* Extract version info for file properties dialog
- [ ] *(Stretch)* Extract string tables for localization
- [ ] Commit: `"kernel: PE resource section parser"`

### 9.3 Unimplemented Function Logger

- [ ] When a PE program calls an unimplemented Win32 function:
  - [ ] Log to serial: `"UNIMPL: kernel32.dll!CreateThread"`
  - [ ] Return safe default (0 / NULL / FALSE) instead of crashing
  - [ ] Track call counts for unimplemented functions
- [ ] Shell command: `win32log` — show unimplemented function call log
- [ ] Helps identify which stubs to implement next
- [ ] Commit: `"win32: unimplemented function logger"`

### 9.4 ELF Dynamic Linking (Parallel Track)

- [ ] *(Stretch)* Parse `PT_DYNAMIC` segment in ELF
- [ ] *(Stretch)* Load `.so` shared libraries
- [ ] *(Stretch)* Symbol resolution + GOT/PLT patching
- [ ] *(Stretch)* `SYSCALL`/`SYSRET` instruction support (Linux-compatible)
- [ ] *(Stretch)* Run static musl-linked Linux binaries
- [ ] Commit: `"kernel: ELF dynamic linking"`

### 9.5 Compatibility Test Suite

- [ ] Create `tests/compat/` directory
- [ ] MinGW "Hello World" console program → test PE load + WriteConsoleA
- [ ] MinGW file I/O program → test CreateFileA + ReadFile + WriteFile
- [ ] MinGW memory allocation program → test VirtualAlloc + HeapAlloc
- [ ] Mach-O "Hello World" → test Mach-O load + libSystem write()
- [ ] Native ELF regression tests → ensure tri-format doesn't break ELF
- [ ] Commit: `"tests: tri-format compatibility test suite"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 1.1–1.3 PE Loader + Tri-Format | Foundation — load Windows + macOS binaries |
| 🔴 P0 | 2.1–2.3 Import Resolution + IAT | Connect PE programs to OS |
| 🔴 P0 | 3.1–3.2 Win32 Console Stubs | Run first Windows PE "Hello World" |
| 🟠 P1 | 3.3–3.4 msvcrt + ntdll Stubs | C runtime for compiled programs |
| 🟠 P1 | 8.1–8.2 Mach-O Loader Core | Load macOS binaries |
| 🟠 P1 | 8.3–8.4 Mach-O Dylib + libSystem | Run macOS console programs |
| 🟠 P1 | 6.1–6.2 Keyboard Layout System | International input support |
| 🟠 P1 | 6.4 UTF-8 Unicode | Text support for all languages |
| 🟠 P1 | 9.3 Unimplemented Function Logger | Debug PE/Mach-O compatibility |
| 🟡 P2 | 4.1 File I/O Stubs | Windows programs that read/write files |
| 🟡 P2 | 4.2 Memory Stubs | VirtualAlloc / HeapAlloc |
| 🟡 P2 | 4.3 Windows Path Translation | Drive letters + backslashes |
| 🟡 P2 | 8.5 macOS Path Translation | POSIX → VFS paths |
| 🟡 P2 | 1.4 Base Relocation | Load PE at non-preferred addresses |
| 🟡 P2 | 6.3 Layout Switching | Win+Space, system tray indicator |
| 🟢 P3 | 5.1 Calling Convention | Proper x64 register mapping |
| 🟢 P3 | 9.5 Compatibility Tests | Tri-format regression testing |
| 🟢 P3 | 2.4 External DLL Loading | Load real PE DLLs |
| 🟢 P3 | 6.5 Localization | Multi-language UI |
| 🔵 P4 | 9.1 Win32 GUI Stubs | Windows GUI programs (long-term) |
| 🔵 P4 | 9.2 PE Resource Parser | Icons, version info |
| 🔵 P4 | 9.4 ELF Dynamic Linking | .so shared libraries |
| 🔵 P4 | 7. Java Runtime | JVM support (educational/future) |
