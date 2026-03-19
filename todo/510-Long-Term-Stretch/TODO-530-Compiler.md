# P1502 — C/C++ Compiler for Impossible OS

> **Goal:** Port a C/C++ compiler to run natively on Impossible OS so developers
> can write, compile, and run PE programs directly on the OS without a cross-compiler.
> Start with TCC (Tiny C Compiler) for C, then eventually port GCC or Clang for
> full C++ support. All compilers output PE binaries.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

> [!WARNING]
> **All six prerequisites must be resolved before porting any compiler.**
> TCC has fewer dependencies than GCC/Clang and is the recommended first target.

**Prerequisites Status:**

| Prerequisite                       | Status                   | Effort | Reference  |
|------------------------------------|--------------------------|--------|------------|
| Working user-mode processes        | ❌ Skipped (ring 3 hangs) | Weeks  | P0105 §1   |
| Native SDK (headers + libs)        | ❌ None                   | Weeks  | P0105 §3   |
| `mmap`, `brk` (memory syscalls)    | ⚠️ Partial               | Weeks  | P0105 §2   |
| Filesystem with Win32 API          | ❌ In progress            | Weeks  | P0103 §3.6 |
| Process spawning (`CreateProcess`) | ❌ Skipped                | Weeks  | P0105 §2   |

---

## 1. Phase 1 — TCC (Tiny C Compiler) *(C only)*

> TCC is ideal as a first compiler: MIT license, ~50K LOC, self-hosting,
> compiles C99, minimal POSIX dependencies, blazingly fast (~9x faster than
> GCC). It can even compile the Linux kernel.

### 1.1 Cross-Compile TCC for Impossible OS

**Prompt:** Cross-compile TCC from Linux targeting Impossible OS x86-64. TCC already supports PE output natively (`-m64` flag). Build TCC as a static PE binary linked against the SDK (P0105 §3). Patch TCC's source to: (1) replace POSIX include paths with Impossible OS paths (`C:\Impossible\Include\`), (2) replace `/tmp/` with `C:\Temp\`, (3) use PE as the default output format. Test by cross-compiling TCC itself, then running it on the OS. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"userland: cross-compile TCC"`.
- [ ] Download TCC source (latest stable from repo.or.cz/tinycc.git)
- [ ] Configure for x86-64 target, PE output format, static linking
- [ ] Patch `#include` search paths:
  - [ ] System headers: `C:\Impossible\Include\`
  - [ ] Library path: `C:\Impossible\Lib\`
  - [ ] Temp directory: `C:\Temp\`
- [ ] Set PE as default output format (TCC supports PE natively)
- [ ] Link against SDK import libraries (`kernel32.lib`)
- [ ] Build static binary `tcc.exe`
- [ ] Commit: `"userland: cross-compile TCC"`

### 1.2 Install TCC on Impossible OS

**Prompt:** Install TCC and its supporting files onto the Impossible OS disk image. Place the binary at `C:\Impossible\Bin\tcc.exe`. Install TCC's runtime library (`libtcc1.a`), include files (tcc's own headers + newlib headers), and the linker script. Add `C:\Impossible\Bin\` to the shell's PATH. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"userland: install TCC on disk"`.
- [ ] Copy `tcc.exe` to `C:\Impossible\Bin\` in the disk image
- [ ] Copy `libtcc1.a` to `C:\Impossible\Lib\`
- [ ] Copy TCC include files to `C:\Impossible\Include\tcc\`
- [ ] Copy SDK headers to `C:\Impossible\Include\` (windows.h, impossible.h)
- [ ] Copy SDK import libraries to `C:\Impossible\Lib\` (kernel32.lib, user32.lib)
- [ ] Add `C:\Impossible\Bin\` to shell PATH (or hardcode in shell)
- [ ] Makefile target: `make install-tcc` → copies all files to disk image
- [ ] Commit: `"userland: install TCC on disk"`

### 1.3 Test TCC on Impossible OS

**Prompt:** Test that TCC can compile and run programs natively on Impossible OS. Test cases: (1) `tcc -run hello.c` — JIT compile and run (if supported), (2) `tcc hello.c -o hello.exe && ./hello.exe` — compile to binary then run, (3) `tcc -c math.c -o math.o && tcc main.c math.o -o app.exe` — separate compilation, (4) Self-hosting: `tcc tcc.c -o tcc2.exe` — TCC compiles itself. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"userland: TCC self-test"`.
- [ ] Test: `tcc hello.c -o hello.exe` → compile simple "Hello World" (PE output)
- [ ] Test: `./hello.exe` → prints "Hello from TCC on Impossible OS!"
- [ ] Test: `tcc -run hello.c` → JIT compile and run without intermediate file
- [ ] Test: separate compilation (`tcc -c` + `tcc -o` with .o files)
- [ ] Test: `#include <windows.h>` → finds SDK headers
- [ ] Test: `#include <impossible.h>` → finds SDK headers
- [ ] Test: `tcc tcc.c -o tcc2.exe` → self-hosting (TCC compiles itself)
- [ ] Commit: `"userland: TCC self-test"`

---

## 2. Phase 2 — TCC Enhancements

### 2.1 IxUI Header Support

**Prompt:** Add the IxUI GUI toolkit headers (from P0105 §1.5) to TCC's include path so native GUI apps can be compiled on the OS. Programs should be able to `#include <ixui.h>` and link against `libixui.a`. Test by compiling a simple windowed app that creates a window and draws text. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"userland: TCC with IxUI support"`.
- [ ] Install `ixui.h` to `C:\Impossible\Include\`
- [ ] Install `libixui.a` to `C:\Impossible\Lib\`
- [ ] Test: `tcc gui_hello.c -lixui -o gui_hello.exe` → compiles
- [ ] Test: `./gui_hello.exe` → window appears with text
- [ ] Commit: `"userland: TCC with IxUI support"`

### 2.2 Shell Integration

**Prompt:** Integrate TCC into the shell for a developer experience. Add shell commands: `cc` as alias for `tcc`, `run <file.c>` as shortcut for `tcc -run <file.c>`. Add a simple `make` equivalent: read a `Makefile` with basic variable substitution and dependency tracking. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"shell: compiler integration"`.
- [ ] Shell alias: `cc` → `C:\Impossible\Bin\tcc.exe`
- [ ] Shell command: `run hello.c` → shortcut for `tcc -run hello.c`
- [ ] Create minimal `make` utility (parse Makefile, dependencies, variable substitution)
- [ ] Commit: `"shell: compiler integration"`

---

## 3. Phase 3 — GCC/Clang *(Long-term, C++ support)*

> GCC and Clang are **much** larger and more complex than TCC. This is a
> long-term goal that requires a mature userland environment.

### 3.1 Additional Prerequisites for GCC

**Prompt:** GCC requires significantly more infrastructure than TCC. Verify and implement the additional prerequisites before attempting the port. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"userland: GCC prerequisites"`.

**Additional prerequisites beyond TCC:**

| Prerequisite                  | Status | Why Needed                                           |
|-------------------------------|--------|------------------------------------------------------|
| `fork()` + `exec()`           | ❌      | GCC spawns cc1, as, ld as child processes            |
| `pipe()`                      | ❌      | Pipeline between preprocessor → compiler → assembler |
| `/tmp/` writable              | ⚠️     | Temporary files during compilation                   |
| Larger heap / virtual memory  | ⚠️     | GCC uses 100+ MB during C++ compilation              |
| `libgmp`, `libmpfr`, `libmpc` | ❌      | GCC math dependencies (build with GCC)               |
| Dynamic linking (`ld.so`)     | ❌      | Shared libraries for plugins                         |
| Working `make` utility        | ❌      | Build system for GCC itself                          |
| POSIX shell (`/bin/sh`)       | ❌      | GCC configure scripts                                |

- [ ] Verify `CreateProcess()` works reliably (P0105 §2)
- [ ] Verify pipes work for inter-process communication
- [ ] Verify `C:\Temp\` writable with sufficient space
- [ ] Verify virtual memory supports 256+ MB per process
- [ ] Cross-compile libgmp, libmpfr, libmpc for Impossible OS
- [ ] Port or implement a `make` utility
- [ ] Commit: `"userland: GCC prerequisites"`

### 3.2 Cross-Compile GCC

**Prompt:** Cross-compile a minimal GCC targeting Impossible OS. Build a two-stage cross-compiler: (1) build a cross-GCC on Linux that outputs Impossible OS binaries, (2) use that cross-GCC to build a native GCC that runs on Impossible OS. Start with C support only (--enable-languages=c), then add C++ (--enable-languages=c,c++). Target: GCC 13+ with x86-64 backend. Link against musl libc. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"userland: cross-compile GCC"`.
- [ ] Build Stage 1: cross-GCC on Linux targeting `x86_64-w64-mingw32`
  - [ ] Configure: `--target=x86_64-w64-mingw32 --enable-languages=c --disable-shared`
  - [ ] Build binutils (as, ld) for `x86_64-w64-mingw32`
  - [ ] Build GCC cross-compiler
- [ ] Build Stage 2: native GCC using the cross-compiler
  - [ ] Cross-compile GCC to run on Impossible OS
  - [ ] Link against SDK libraries
- [ ] Install to `C:\Impossible\Bin\gcc.exe`
- [ ] Test: `gcc hello.c -o hello.exe` → compiles and runs
- [ ] Commit: `"userland: cross-compile GCC"`

### 3.3 Add C++ Support

**Prompt:** Enable C++ in the GCC build. This requires libstdc++ to be cross-compiled and installed. Test with C++ features: classes, templates, STL containers, exceptions, RTTI. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"userland: GCC C++ support"`.
- [ ] Rebuild GCC with `--enable-languages=c,c++`
- [ ] Cross-compile libstdc++ for Impossible OS
- [ ] Install `libstdc++.a` to `C:\Impossible\Lib\`
- [ ] Install C++ headers to `C:\Impossible\Include\c++\`
- [ ] Test: `g++ hello.cpp -o hello.exe` → compiles C++ code
- [ ] Test: classes, inheritance, virtual functions
- [ ] Test: templates, STL (`<vector>`, `<string>`, `<map>`)
- [ ] Test: exceptions (`try/catch/throw`)
- [ ] Test: `<iostream>` with `std::cout`
- [ ] Commit: `"userland: GCC C++ support"`

### 3.4 Clang/LLVM *(Alternative to GCC)*

**Prompt:** As an alternative or complement to GCC, port Clang/LLVM. LLVM has a modular design and can be easier to port for specific targets. However, it's even larger than GCC (~30M LOC vs GCC's ~15M LOC). Consider this only after GCC is working. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"userland: Clang/LLVM"`.
- [ ] Add `x86_64-impossible` target triple to LLVM
- [ ] Cross-compile Clang for Impossible OS
- [ ] Cross-compile libc++ (LLVM's C++ standard library)
- [ ] Install `clang.exe` to `C:\Impossible\Bin\`
- [ ] Test: `clang hello.c -o hello.exe` → works
- [ ] Test: `clang++ hello.cpp -o hello.exe` → C++ works
- [ ] Commit: `"userland: Clang/LLVM"`

---

## 4. Documentation

**Prompt:** Document the compiler toolchain: installation paths, how to compile programs, available headers and libraries, self-hosting status, and the developer workflow. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"docs: compiler toolchain"`.
- [ ] Document TCC: installation, usage, limitations
- [ ] Document newlib/musl headers available
- [ ] Document IxUI compilation workflow
- [ ] Document GCC/Clang: prerequisites, build process, C++ support
- [ ]
- [ ] Commit: `"docs: compiler toolchain"`

---

## Priority Order

1. **§1.1** Cross-compile TCC (depends on P0105 §1–§3)
2. **§1.2** Install TCC on disk
3. **§1.3** Test TCC (hello world, self-hosting)
4. **§2.1** IxUI header support
5. **§2.2** Shell integration (cc, run, make)
6. **§3.1** GCC prerequisites check
7. **§3.2** Cross-compile GCC (C only, PE output)
8. **§3.3** Add C++ support
9. **§3.4** Clang/LLVM *(optional alternative)*
10. **§4** Documentation

---

## Key Files

| File                                      | Purpose                           |
|-------------------------------------------|-----------------------------------|
| `C:\Impossible\Bin\tcc.exe`               | TCC compiler binary               |
| `C:\Impossible\Bin\gcc.exe`               | GCC compiler binary (Phase 3)     |
| `C:\Impossible\Bin\g++.exe`               | G++ C++ compiler (Phase 3)        |
| `C:\Impossible\Include\`                  | SDK headers (windows.h + IxUI)    |
| `C:\Impossible\Lib\kernel32.lib`          | Import library (PE linker)        |
| `C:\Impossible\Lib\user32.lib`            | Import library (PE linker)        |
| `C:\Impossible\Lib\libtcc1.a`             | TCC runtime library               |
| `C:\Impossible\Lib\libstdc++.a`           | C++ standard library (Phase 3)    |
| `tools/build-tcc.sh`                      | [NEW] Script to cross-compile TCC |
| `tools/build-gcc.sh`                      | [NEW] Script to cross-compile GCC |

---

## Effort Estimates

| Component             | Effort     | Dependencies                     |
|-----------------------|------------|----------------------------------|
| Cross-compile TCC     | Days–Weeks | SDK, user-mode, file I/O         |
| Install + test TCC    | Days       | TCC built, disk image            |
| TCC self-hosting      | Days       | TCC running on OS                |
| IxUI headers          | Days       | IxUI library (P0105 §4)          |
| Shell integration     | Days       | Shell, TCC installed             |
| GCC prerequisites     | Weeks      | CreateProcess, pipes, C:\\Temp\\ |
| Cross-compile GCC (C) | Weeks      | All prerequisites, binutils      |
| GCC C++ (libstdc++)   | Weeks      | GCC C working                    |
| Clang/LLVM            | Months     | Mature userland                  |
