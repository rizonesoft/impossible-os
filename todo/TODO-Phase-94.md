# Phase 10 — Compatibility & Internationalization

> **Goal:** Make Impossible OS a **tri-format platform** that natively runs ELF,
> Windows PE, and macOS Mach-O executables — the only hobby OS to load all three
> major binary formats. Provide Win32 and macOS API compatibility through stub
> libraries, support international keyboard layouts and Unicode, and optionally
> run Java bytecode — creating a truly versatile operating system.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.


---

## 1. PE Binary Loader

### 1.1 PE Header Structures

**Prompt:** PE (Portable Executable) is the Windows binary format. Define the structures per the PE/COFF spec: DOS header (e_magic "MZ", e_lfanew to PE signature), COFF header (machine=AMD64), PE32+ Optional Header (magic 0x020B, ImageBase, EntryPointRVA, SizeOfImage), section headers (name, VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData), data directories (RVA+Size for imports, relocations, etc). After completing all items, create `docs/architecture/pe-loader.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: PE/COFF header structures"`.


- [ ] Create `include/pe.h` (~80 lines)
- [ ] Define `struct pe_dos_header` (e_magic "MZ", e_lfanew offset to PE signature)
- [ ] Define `struct pe_coff_header` (machine, num_sections, characteristics)
- [ ] Define `struct pe_optional_header_64` (PE32+ magic `0x020B`, image_base, entry_point_rva, section_alignment, size_of_image, num_data_dirs)
- [ ] Define `struct pe_section_header` (name, virtual_size, virtual_address, raw_data_size, raw_data_offset, characteristics)
- [ ] Define `struct pe_data_directory` (rva, size) — for import table, export table, relocation table
- [ ] Define PE constants: `PE_DOS_MAGIC`, `PE_SIGNATURE`, `PE_MACHINE_AMD64`, `PE32PLUS_MAGIC`
- [ ] Commit: `"kernel: PE/COFF header structures"`

### 1.2 PE Loader Core

**Prompt:** `pe_load(data, size)` validates DOS "MZ" header, follows e_lfanew to PE signature, parses COFF (verify Machine==AMD64), reads PE32+ Optional Header (ImageBase, EntryPointRVA, SizeOfImage, DataDirectory imports+relocations). Allocate SizeOfImage at ImageBase, copy each section to ImageBase+VirtualAddress, zero-fill BSS. Return entry = ImageBase + AddressOfEntryPoint. After completing all items, update `docs/architecture/pe-loader.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: PE loader core"`.


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

**Prompt:** Create `load_binary(data, size)` that checks magic bytes: `"MZ"` → pe_load, `"\x7FELF"` → elf_load, `0xFEEDFACF` → macho_load, `0xFEEDFACE` → reject 32-bit. Update exec path to use load_binary() instead of elf_load(). All loaders return the same `struct load_result`. After completing all items, update `docs/architecture/pe-loader.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: tri-format binary detection"`.


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

**Prompt:** Parse DataDirectory[5] relocation blocks. For IMAGE_REL_BASED_DIR64 entries, add delta (actual_base - preferred_base) to 64-bit addresses. Skip type 0 padding. No relocation needed if loaded at preferred address. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: PE base relocation support"`.


- [ ] Parse DataDirectory[5] (Base Relocation Table) from PE
- [ ] If PE loaded at address ≠ preferred ImageBase:
  - [ ] Calculate delta = actual_base − preferred_base
  - [ ] Walk relocation blocks: for each `IMAGE_REL_BASED_DIR64` entry, add delta to address
- [ ] Skip relocation if loaded at preferred address
- [ ] Commit: `"kernel: PE base relocation support"`

---

## 2. Import Resolution & DLL System

### 2.1 Import Table Parser

**Prompt:** Parse DataDirectory[1] Import Directory Table. Each entry has DLL name RVA, Import Lookup Table, Import Address Table. Walk ILT entries: MSB set = ordinal, otherwise Hint/Name Table entry. Log imports: `"PE imports: kernel32.dll!WriteConsoleA"`. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: PE import table parser"`.


- [ ] Parse Import Directory Table from DataDirectory[1]
- [ ] For each DLL entry: read DLL name string, Import Lookup Table (ILT), Import Address Table (IAT)
- [ ] For each function: read name (or ordinal) from Hint/Name Table
- [ ] Log imported DLLs and functions: `"PE imports: kernel32.dll!WriteConsoleA"`
- [ ] Commit: `"kernel: PE import table parser"`

### 2.2 Builtin DLL Stub Table

**Prompt:** Map DLL names to (func_name, func_ptr) arrays. `pe_resolve_builtin(dll_name, func_name)` searches and returns the stub pointer. Start with kernel32, msvcrt, ntdll. Future: user32, gdi32, ws2_32, advapi32. Table must be extensible. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: builtin DLL stub table"`.


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

**Prompt:** Patch Import Address Table: for each import, look up builtin table. If found, write stub pointer to IAT slot. If not, write `stub_unimplemented()` that logs `"UNIMPL: dll!func"` and returns 0. PE code calls stubs via normal IAT indirect calls. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: IAT patching"`.


- [ ] For each imported function:
  - [ ] Look up in builtin DLL table first
  - [ ] If found: write stub function pointer into IAT slot
  - [ ] If not found: log warning, write `stub_unimplemented()` (prints "UNIMPL: dll!func")
- [ ] After patching, PE code calls our stubs via normal function call through IAT
- [ ] Commit: `"kernel: IAT patching (import resolution)"`

### 2.4 External DLL Loading (Future)

**Prompt:** Stretch: search filesystem for DLLs not in builtin table (System32 dir, exe dir). Load DLL as PE, parse export table. Handle recursive DLL deps. Call DllMain(DLL_PROCESS_ATTACH). Implement LoadLibraryA/GetProcAddress for runtime loading. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: external DLL loading"`.


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

### 3.1 Windows Type Definitions

**Prompt:** Create `include/win32.h` with Windows types: HANDLE=void*, DWORD=uint32_t, BOOL=int, LPVOID=void*, LPCSTR=const char*. Constants: STD_INPUT/OUTPUT/ERROR_HANDLE, INVALID_HANDLE_VALUE, TRUE, FALSE. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: Win32 type definitions"`.


- [ ] Create `include/win32.h`
- [ ] Define Windows types: `HANDLE`, `DWORD`, `BOOL`, `LPVOID`, `LPCSTR`, `SIZE_T`, `UINT`, `LPARAM`, `WPARAM`
- [ ] Define constants: `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE`, `STD_ERROR_HANDLE`, `INVALID_HANDLE_VALUE`, `TRUE`, `FALSE`
- [ ] Commit: `"kernel: Win32 type definitions"`

### 3.2 kernel32.dll — Console & Process

**Prompt:** First tier stubs for console Hello World: GetStdHandle maps fd 0/1/2, WriteConsoleA → sys_write, ReadConsoleA → sys_read, ExitProcess → sys_exit, GetCommandLineA returns argv, GetModuleHandleA(NULL) returns ImageBase, GetLastError/SetLastError use global error var. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"win32: kernel32.dll console stubs"`.


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

**Prompt:** Map C stdlib: printf/puts → format + sys_write, malloc/free/calloc/realloc → kernel heap, memcpy/memmove/memset/memcmp → kernel implementations, strlen/strcpy/strcmp → kernel string functions, sprintf/snprintf → kernel printf engine, atoi/atol for conversions. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"win32: msvcrt.dll C runtime stubs"`.


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

**Prompt:** Stub minimal ntdll functions: RtlInitUnicodeString (no-op), NtCurrentTeb (return static dummy TEB), RtlGetVersion (return plausible version info). Most ntdll functions can be safely stubbed as no-ops for basic compatibility. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"win32: ntdll.dll minimal stubs"`.


- [ ] Create `src/kernel/win32/ntdll.c`
- [ ] `RtlInitUnicodeString()` → stub (many programs import it)
- [ ] `NtCurrentTeb()` → stub (Thread Environment Block)
- [ ] `RtlGetVersion()` → return OS version info
- [ ] Commit: `"win32: ntdll.dll minimal stubs"`

### 3.5 Test: Run Windows Hello World

**Prompt:** Cross-compile `x86_64-w64-mingw32-gcc -o hello.exe hello.c`, include on C:\\. Execute in shell — should print "Hello, World!" via WriteConsoleA. This is the milestone proving PE+IAT+Win32 all work. Debug via serial UNIMPL warnings. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: run first Windows PE program"`.


- [ ] Cross-compile test program: `x86_64-w64-mingw32-gcc -o hello.exe hello.c`
- [ ] Include on C:\
- [ ] Execute: `hello.exe` in shell → should print "Hello, World!" via WriteConsoleA
- [ ] Commit: `"kernel: run first Windows PE program"`

---

## 4. Win32 API Stubs — Tier 2 (File I/O & Memory)

### 4.1 kernel32.dll — File I/O

**Prompt:** File I/O stubs: CreateFileA translates path via §4.3 then vfs_open, ReadFile/WriteFile → vfs_read/write, CloseHandle → vfs_close, GetFileSize → vfs_stat, SetFilePointer → vfs_seek. FindFirstFileA/FindNextFileA/FindClose for dir scanning. DeleteFileA → vfs_delete, CreateDirectoryA → vfs_create_dir. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"win32: kernel32.dll file I/O stubs"`.


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

**Prompt:** VirtualAlloc maps to sys_mmap (MEM_COMMIT|MEM_RESERVE = allocate+zero), VirtualFree releases. HeapCreate/HeapAlloc/HeapFree/GetProcessHeap map to kernel heap. Add SYS_MMAP and SYS_BRK syscalls. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"win32: kernel32.dll memory management stubs"`.


- [ ] `VirtualAlloc(addr, size, type, protect)` → `sys_mmap()`
- [ ] `VirtualFree(addr, size, type)` → `sys_munmap()`
- [ ] `HeapCreate(options, initial, max)` → create heap region
- [ ] `HeapAlloc(heap, flags, size)` → `kmalloc()` from heap
- [ ] `HeapFree(heap, flags, ptr)` → `kfree()`
- [ ] `GetProcessHeap()` → return default heap handle
- [ ] Add `SYS_MMAP` and `SYS_BRK` syscalls to `syscall.c`
- [ ] Commit: `"win32: kernel32.dll memory management stubs"`

### 4.3 Windows Path Translation

**Prompt:** `win32_path_to_vfs()` converts backslashes to forward slashes, preserves drive letters (VFS uses them). Handle relative paths via CWD. UNC paths return error. Called inside every file-related Win32 stub. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"win32: Windows path translation"`.


- [ ] `win32_path_to_vfs(win_path, vfs_path)` — translate Windows paths to VFS:
  - [ ] `"C:\\Users\\Derick\\file.txt"` → `"C:/Users/Derick/file.txt"`
  - [ ] Backslash → forward slash
  - [ ] Drive letters preserved (VFS already uses them)
- [ ] Handle relative paths (resolve against CWD)
- [ ] Handle UNC paths: `"\\\\server\\share"` → stub (not supported, return error)
- [ ] Commit: `"win32: Windows path translation"`

---

## 5. Calling Convention Bridge

### 5.1 Windows x64 Convention

**Prompt:** Windows x64 passes args in RCX, RDX, R8, R9 (vs System V: RDI, RSI, RDX, RCX). Compile Win32 stubs with `__attribute__((ms_abi))` or use inline asm to remap. Syscalls via INT 0x80 bypass C ABI, so no conflict. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: Win32 calling convention"`.


- [ ] Win32 stubs compiled with Windows x64 convention: args in RCX, RDX, R8, R9
- [ ] Stubs internally use inline `INT 0x80` assembly with explicit register setup
- [ ] No automatic convention conflict since syscalls bypass C ABI
- [ ] Commit: `"kernel: Win32 stubs with Windows x64 calling convention"`

### 5.2 Convention Thunks (Future)

**Prompt:** Stretch: Win-to-SysV thunk moves RCX→RDI, RDX→RSI, R8→RDX, R9→RCX then jumps. Reverse for SysV-to-Win plus 32-byte shadow space. Only needed for cross-format interop. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: calling convention thunks"`.


- [ ] *(Stretch)* Windows-to-SysV thunk: `mov rdi, rcx; mov rsi, rdx; mov rdx, r8; mov rcx, r9; jmp target`
- [ ] *(Stretch)* SysV-to-Windows thunk: reverse register mapping
- [ ] *(Stretch)* Needed if ELF libraries call PE functions or vice versa

---

## 6. Keyboard Layouts & Internationalization

### 6.1 Keyboard Layout System

**Prompt:** Replace hardcoded US QWERTY scancode table in `keyboard.c` with a layout system. Define `struct kbd_layout` with name, code ("en-US"), normal[128], shift[128], altgr[128] arrays. `kbd_set_layout(code)` switches active layout. Store in Registry `System\Input\KeyboardLayout`. After completing all items, create `docs/architecture/keyboard-layouts.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: keyboard layout system"`.


- [ ] Create `src/kernel/kbd_layout.c` and `include/kbd_layout.h`
- [ ] Define `struct kbd_layout` (name, code, normal[128], shift[128], altgr[128])
- [ ] Replace hardcoded US QWERTY scancode→ASCII table in `keyboard.c` with layout lookup
- [ ] `kbd_set_layout(code)` — switch active layout
- [ ] `kbd_get_layout()` — return current layout code
- [ ] Registry: `System\Input\KeyboardLayout = "en-US"`
- [ ] Commit: `"kernel: keyboard layout system"`

### 6.2 Built-in Layouts

**Prompt:** Define layout tables: US QWERTY (default), UK English (£ vs $), German QWERTZ (Z/Y swap, umlauts on AltGr), French AZERTY (A/Q, Z/W swap), Spanish (ñ, accents), Dvorak (alt layout). Store as static arrays in `resources/layouts/`. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: built-in keyboard layouts"`.


- [ ] Create `resources/layouts/` directory with layout data
- [ ] **US English (QWERTY)** — `en-US` (default)
- [ ] **UK English** — `en-GB` (different symbols: £ vs $, @ position)
- [ ] **German (QWERTZ)** — `de-DE` (Z/Y swapped, umlauts on AltGr)
- [ ] **French (AZERTY)** — `fr-FR` (A/Q, Z/W swapped, accents)
- [ ] **Spanish** — `es-ES` (ñ, accents)
- [ ] **Dvorak** — `en-DV` (alternative layout)
- [ ] Commit: `"kernel: built-in keyboard layouts (6 layouts)"`

### 6.3 Layout Switching

**Prompt:** Win+Space cycles installed layouts. System tray shows 2-letter indicator ("EN", "FR", "DE"). Click indicator → layout picker popup. Settings applet for layout management. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: keyboard layout switching"`.


- [ ] Win+Space → cycle through installed layouts
- [ ] System tray indicator: show current layout code (`EN`, `FR`, `DE`)
- [ ] Click tray indicator → layout picker popup
- [ ] Input → keyboard settings applet for layout management
- [ ] Commit: `"desktop: keyboard layout switching (Win+Space)"`

### 6.4 Unicode / UTF-8 Support

**Prompt:** Store all text as UTF-8. Implement `utf8_encode(codepoint, buf)` and `utf8_decode(buf, codepoint_out)`. Keyboard outputs UTF-8. stb_truetype already supports Unicode codepoints. Stretch: Noto Sans fallback font for CJK. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: UTF-8 Unicode support"`.


- [ ] Store all text strings internally as UTF-8
- [ ] UTF-8 encode/decode helpers: `utf8_encode(codepoint, buf)`, `utf8_decode(buf, codepoint_out)`
- [ ] Keyboard input: convert layout output to UTF-8 codepoints
- [ ] Font rendering: `stb_truetype` already supports Unicode codepoints
- [ ] *(Stretch)* Noto Sans as fallback font (covers all Unicode scripts)
- [ ] Commit: `"kernel: UTF-8 Unicode support"`

### 6.5 Localization Framework (Future)

**Prompt:** Stretch: per-locale .ini files at `C:\Impossible\System\Locale\{code}.ini`. `locale_get(key)` returns localized string. All UI uses locale_get() instead of hardcoded English. Start with en-US, add fr-FR/de-DE/es-ES. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: localization framework"`.


- [ ] *(Stretch)* UI string files: `C:\Impossible\System\Locale\{code}.ini`
- [ ] *(Stretch)* `locale_get(key)` — return localized string for current locale
- [ ] *(Stretch)* Default: `en-US.ini`, additional: `fr-FR.ini`, `de-DE.ini`, `es-ES.ini`
- [ ] *(Stretch)* All UI elements use `locale_get()` instead of hardcoded strings
- [ ] Commit: `"kernel: localization framework"`

---

## 7. Java Runtime (Optional)

### 7.1 GraalVM Native Images (Approach B — Recommended First)

**Prompt:** Compile Java to native ELF/PE via GraalVM `native-image` on host. No JVM needed at runtime — runs through existing loaders. Requires ELF+syscalls (mmap, file I/O). Test with Hello World. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: GraalVM native image support"`.


- [ ] *(Stretch)* Compile Java programs to native ELF/PE on host using `native-image`
- [ ] *(Stretch)* Include compiled binary on C:\
- [ ] *(Stretch)* Execute like any other ELF/PE program — no JVM needed at runtime
- [ ] *(Stretch)* Prerequisite: working ELF loader + enough syscalls (`mmap`, file I/O)

### 7.2 Mini-JVM Bytecode Interpreter (Approach C — Educational)

**Prompt:** Build minimal JVM (~2-4K lines). Parse .class format: magic 0xCAFEBABE, constant pool, methods, Code attribute. Implement ~40 opcodes: constants, arithmetic, variables, control flow, objects. Map System.out.println → sys_write. Shell: `java HelloWorld.class`. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: Mini-JVM"`.


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

**Prompt:** Port JamVM (~15K lines, GPL 2.0). Requires mmap, file I/O, threading, ZIP parsing (miniz). Most complete Java but most effort. Stretch goal — prioritize approaches B and C first. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: JamVM port"`.


- [ ] *(Stretch)* Port JamVM interpreter (~15K lines, GPL 2.0)
- [ ] *(Stretch)* Prerequisites: `mmap()`, file I/O, threading
- [ ] *(Stretch)* Covers: classes, methods, strings, arrays, exceptions
- [ ] *(Stretch)* Load `.jar` files (ZIP parsing via miniz)

---

## 8. Mach-O Binary Loader (macOS Compatibility)

> **Unique differentiator:** Impossible OS as a tri-format platform — the only hobby
> OS that natively loads ELF + PE + Mach-O executables.

### 8.1 Mach-O Header Structures

**Prompt:** Define Mach-O structs: `mach_header_64` (magic 0xFEEDFACF, cputype X86_64), load commands (LC_SEGMENT_64, LC_MAIN, LC_LOAD_DYLIB, LC_SYMTAB, LC_DYSYMTAB), `segment_command_64` (vmaddr, vmsize, fileoff, filesize, nsects), `section_64` (sectname, segname, addr, size). After all items, create `docs/architecture/macho-loader.md`, mark `[x]`, run `make clean && make all && make run`, commit `"kernel: Mach-O header structures"`.


- [ ] Create `include/macho.h` (~80 lines)
- [ ] Define `struct mach_header_64` (magic `0xFEEDFACF`, cputype `CPU_TYPE_X86_64`, ncmds, sizeofcmds)
- [ ] Define `struct load_command` (cmd, cmdsize) — base for all load commands
- [ ] Define `struct segment_command_64` (cmd `LC_SEGMENT_64`, segname, vmaddr, vmsize, fileoff, filesize, nsects)
- [ ] Define `struct section_64` (sectname, segname, addr, size, offset)
- [ ] Define Mach-O constants: `MH_MAGIC_64`, `CPU_TYPE_X86_64`, `LC_SEGMENT_64`, `LC_MAIN`, `LC_LOAD_DYLIB`, `LC_SYMTAB`, `LC_DYSYMTAB`
- [ ] Commit: `"kernel: Mach-O header structures"`

### 8.2 Mach-O Loader Core

**Prompt:** `macho_load(data, size)` validates magic+cputype, walks load commands. LC_SEGMENT_64: allocate vmsize at vmaddr, copy filesize from fileoff, zero-fill remainder. LC_MAIN: entry offset from __TEXT base. Collect LC_LOAD_DYLIB names. Return entry point. After all items, mark `[x]`, run `make clean && make all && make run`, commit `"kernel: Mach-O loader core"`.


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

**Prompt:** Parse LC_SYMTAB + LC_DYSYMTAB for symbol/indirect tables. Patch __la_symbol_ptr entries (lazy symbol pointers) with builtin stub addresses. Map libSystem.B.dylib to our stubs. Use indirect symbol table to match stub indices to names. After all items, mark `[x]`, run `make clean && make all && make run`, commit `"kernel: Mach-O dylib import resolution"`.


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

**Prompt:** macOS uses System V x64 convention (same as ELF) — no register bridge needed. Stubs: write/read/_exit → sys_write/read/exit, malloc/free → kernel heap, mmap/munmap → sys_mmap, printf/puts/strlen/strcpy/strcmp/memcpy/memset, open/close/stat → VFS, getpid/abort. After all items, mark `[x]`, run `make clean && make all && make run`, commit `"macos: libSystem.B.dylib stubs"`.


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

**Prompt:** Translate macOS Unix paths: `/Users/name/file.txt` → `C:\Users\name\file.txt`, `/tmp/` → `C:\Temp\`, `/` → `C:\`. VFS already supports forward slashes, main work is prepending drive letter. After all items, mark `[x]`, run `make clean && make all && make run`, commit `"macos: path translation"`.


- [ ] Translate macOS-style paths to VFS:
  - [ ] `/Users/name/file.txt` → `C:\Users\name\file.txt`
  - [ ] `/tmp/` → `C:\Temp\`
  - [ ] `/` → `C:\`
- [ ] Forward slash preserved (VFS already supports it)
- [ ] Commit: `"macos: path translation"`

### 8.6 Test: Run macOS Hello World

**Prompt:** Cross-compile on macOS: `clang -target x86_64-apple-macos -o hello hello.c`, or hand-craft minimal Mach-O binary. Include on C:\\. Execute → should print "Hello from macOS binary!" proving tri-format works. After all items, mark `[x]`, run `make clean && make all && make run`, commit `"kernel: run first Mach-O program"`.


- [ ] Cross-compile test: `clang -target x86_64-apple-macos -o hello hello.c` (on macOS host)
- [ ] Or: hand-craft minimal Mach-O binary with `write()` + `_exit()` syscalls
- [ ] Include on C:\
- [ ] Execute: `hello` → prints "Hello from macOS binary!"
- [ ] Commit: `"kernel: run first Mach-O program"`

### 8.7 Unimplemented macOS Function Logger

**Prompt:** Same as Win32 logger: log unresolved dylib symbols `"UNIMPL: libSystem.B.dylib!pthread_create"`, return safe default, track call counts. After all items, mark `[x]`, run `make clean && make all && make run`, commit `"macos: unimplemented function logger"`.


- [ ] Same pattern as Win32 logger (§9.3): log unresolved dylib symbols
- [ ] `"UNIMPL: libSystem.B.dylib!pthread_create"`
- [ ] Return safe default instead of crashing
- [ ] Commit: `"macos: unimplemented function logger"`

---

## 9. Agent-Recommended Additions

> Items not in the research files but important for a complete compatibility layer.

### 9.1 Win32 GUI Stubs — Tier 3 (Future)

**Prompt:** Stretch: map Windows GUI APIs to WM. RegisterClassExW registers window class, CreateWindowExW → wm_create_window, ShowWindow makes visible. GetMessage/TranslateMessage/DispatchMessage implement message loop. MessageBoxA shows Phase 05 dialog. GDI: CreateDC wraps surface, BitBlt → gfx_blit, TextOutA → font_draw_string. After all items, mark `[x]`, run `make clean && make all && make run`, commit `"win32: GUI stubs"`.


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

> **Promoted from Stretch** — required for extracting icons from third-party `.exe`
> and `.dll` files (taskbar icons, title bar icons, File Manager icons). Also needed
> by §9.5 (Shell Icon API) for `ExtractIconEx` and `LoadIcon`.

**Prompt:** Parse DataDirectory[2] Resource Table — it's a 3-level tree: Level 1 = resource type (RT_ICON=3, RT_GROUP_ICON=14, RT_VERSION=16, RT_STRING=6), Level 2 = resource name/ID, Level 3 = language. Implement `pe_find_resource(pe_data, type, id)` that walks the tree and returns a pointer+size to the resource data. For RT_GROUP_ICON, parse the GRPICONDIR structure to find the best-matching size, then load the corresponding RT_ICON entry. For RT_VERSION, parse VS_VERSIONINFO → VS_FIXEDFILEINFO to extract FileVersion, ProductName, CompanyName. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: PE resource section parser"`. Create or update documentation in `docs/architecture/pe-loader.md`.


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
  - [ ] Used by: File Manager properties dialog, taskbar tooltips
- [ ] RT_STRING extraction (stretch):
  - [ ] Parse string table blocks (16 strings per block)
  - [ ] Used by: localization, app-defined text
- [ ] Commit: `"kernel: PE resource section parser"`

### 9.3 Unimplemented Function Logger

**Prompt:** When PE calls unimplemented stub, log `"UNIMPL: kernel32.dll!CreateThread"` to serial. Return safe default (0/NULL/FALSE). Track call counts. Shell `win32log` dumps log sorted by count to prioritize next stubs. After all items, mark `[x]`, run `make clean && make all && make run`, commit `"win32: unimplemented function logger"`.


- [ ] When a PE program calls an unimplemented Win32 function:
  - [ ] Log to serial: `"UNIMPL: kernel32.dll!CreateThread"`
  - [ ] Return safe default (0 / NULL / FALSE) instead of crashing
  - [ ] Track call counts for unimplemented functions
- [ ] Shell command: `win32log` — show unimplemented function call log
- [ ] Helps identify which stubs to implement next
- [ ] Commit: `"win32: unimplemented function logger"`

### 9.4 ELF Dynamic Linking (Parallel Track)

**Prompt:** Stretch: parse PT_DYNAMIC segment, load .so shared libraries, resolve GOT/PLT. Add SYSCALL/SYSRET for Linux-compatible syscalls. Static musl-linked binaries are the easier target. After all items, mark `[x]`, run `make clean && make all && make run`, commit `"kernel: ELF dynamic linking"`.


- [ ] *(Stretch)* Parse `PT_DYNAMIC` segment in ELF
- [ ] *(Stretch)* Load `.so` shared libraries
- [ ] *(Stretch)* Symbol resolution + GOT/PLT patching
- [ ] *(Stretch)* `SYSCALL`/`SYSRET` instruction support (Linux-compatible)
- [ ] *(Stretch)* Run static musl-linked Linux binaries
- [ ] Commit: `"kernel: ELF dynamic linking"`

### 9.5 Win32 Shell Icon API *(from P0201 §4.8)*

> **Moved from P0201** — these Win32 API stubs depend on the PE loader (§1),
> builtin DLL table (§2.2), IAT patching (§2.3), and PE resource parser (§9.2).
> The icon index mapping table in P0201 §4.7 provides the data; this section
> provides the API that Win32 apps call.

**Prompt:** Implement Win32 Shell icon API functions as builtin stubs in `src/kernel/win32/shell32.c`. These are registered in the builtin DLL stub table (§2.2) under `"shell32.dll"`. `ExtractIconEx` uses the PE resource parser (§9.2) for third-party `.exe`/`.dll` files and `ico_load()` for `.ico` files — for system DLLs (shell32, imageres) it uses `win32_icon_lookup()` from P0201 §4.7 to resolve indices to `system_icon_t` and returns icons from IRES. `SHGetFileInfo` delegates to `icon_for_extension()` for file type icons. `SHGetStockIconInfo` uses the SHSTOCKICONID mapping from P0201 §4.7. `LoadIcon`/`LoadImage` extract from PE resource sections. All return `HICON` handles (opaque pointers to `icon_bitmap_t`). After completing all items, update `docs/architecture/pe-loader.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"win32: shell32.dll icon API stubs"`.


- [ ] Define `HICON` type in `include/win32.h` (opaque handle to cached `icon_bitmap_t`)
- [ ] Create `src/kernel/win32/shell32.c`
- [ ] Implement `ExtractIconExA(path, index, phiconLarge, phiconSmall, nIcons)`:
  - [ ] If path is system DLL (shell32, imageres) → `win32_icon_lookup()` → `icon_get()`
  - [ ] If path ends in `.ico` → use `ico_load()` (P0201 §4.6)
  - [ ] If path ends in `.dll`/`.exe` → PE resource parser (§9.2) for RT_GROUP_ICON at index
  - [ ] Return large (32×32) and small (16×16) icon handles
- [ ] Implement `SHGetFileInfoA(path, dwFileAttributes, psfi, cbFileInfo, uFlags)`:
  - [ ] `SHGFI_ICON` flag → return icon for file type via `icon_for_extension()`
  - [ ] `SHGFI_TYPENAME` flag → return type description (e.g., "Text Document")
  - [ ] `SHGFI_DISPLAYNAME` flag → return filename
  - [ ] For `.exe`/`.dll` → extract embedded icon from PE resources (§9.2)
- [ ] Implement `SHGetStockIconInfo(siid, uFlags, psii)`:
  - [ ] Use SHSTOCKICONID → `system_icon_t` mapping from P0201 §4.7
  - [ ] Return HICON handle at requested size
- [ ] Implement `LoadIconA(hInstance, lpIconName)` / `LoadImageA(hInst, name, IMAGE_ICON, cx, cy, flags)`:
  - [ ] Extract icon from the PE module's resource section (§9.2)
  - [ ] Support both integer resource IDs (`MAKEINTRESOURCE`) and string names
- [ ] Implement `DestroyIcon(hIcon)` — release cached icon handle
- [ ] Register all functions in `builtin_shell32[]` export table (§2.2)
- [ ] Commit: `"win32: shell32.dll icon API stubs"`

### 9.6 Compatibility Test Suite

**Prompt:** Create `tests/compat/` with pre-compiled binaries: MinGW PE Hello World, MinGW file I/O, MinGW memory allocation, Mach-O Hello World, ELF regression tests. Run all and verify expected output. After all items, mark `[x]`, run `make clean && make all && make run`, commit `"tests: tri-format compatibility test suite"`.


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
| 🟢 P3 | 9.6 Compatibility Tests | Tri-format regression testing |
| 🟢 P3 | 2.4 External DLL Loading | Load real PE DLLs |
| 🟢 P3 | 6.5 Localization | Multi-language UI |
| 🔵 P4 | 9.1 Win32 GUI Stubs | Windows GUI programs (long-term) |
| 🔵 P4 | 9.2 PE Resource Parser | Icons from third-party .exe/.dll (promoted from Stretch) |
| 🔵 P4 | 9.5 Shell Icon API | ExtractIcon, SHGetFileInfo, LoadIcon (from P0201) |
| 🔵 P4 | 9.4 ELF Dynamic Linking | .so shared libraries |
| 🔵 P4 | 7. Java Runtime | JVM support (educational/future) |
