---
schema_version: 1
id: sdk-distribution
domain: 12-user-platform-sdk
status: active
title: "TODO-06 -- SDK Distribution & Developer Experience"
---

# TODO-06 -- SDK Distribution & Developer Experience

> **Goal:** Package the SDK into a distributable ZIP, build developer tooling (sampling
> profiler, unit test runner, debugger enhancements), generate API documentation, ship
> code samples and IxUI starter templates, and automate the release pipeline -- everything
> a third-party developer needs to build apps for Impossible OS.

> [!IMPORTANT]
> **Prerequisites before starting:**
> - `10-platform-services/TODO-08 §15` -- `sdk/include/windows.h` / `impossible.h`, import
>   libs (`kernel32.lib`, `user32.lib`, `gdi32.lib`, `ntdll.lib`), `impossible-cc` wrapper.
> - `10-platform-services/TODO-09` -- TCC compiler, `libc.lib`, IxUI toolkit (`libixui.a`),
>   `make` utility, basic SDK installer to `C:\Impossible\Include\` + `C:\Impossible\Bin\`.
> - `10-platform-services/TODO-12 §3` -- base `debugger.exe` with `SYS_DEBUG_ATTACH/DETACH`,
>   `ReadProcessMemory`/`WriteProcessMemory`/`GetThreadContext`/`WaitForDebugEvent`,
>   INT3 + DR* breakpoints. This TODO adds call-stack display, disassembler, and
>   source-level debug on top of that foundation.
>
> **Scope boundary**: §2 (docs) extends `D10T09 §11` basic getting-started + porting guide;
> §6 (debugger enhancements) extends `D10T12 §3`; §1 (packaging) extends `D10T09 §10`
> installer. Do not re-specify the base compiler, import lib generation, or core debugger
> already owned by those TODOs.
>
> **Existing `sdk/` layout**: `sdk/include/`, `sdk/lib/`, `sdk/tools/`, `sdk/docs/`,
> `sdk/examples/`. Use these canonical paths -- do not create parallel structures.

---

## Inputs

- `sdk/include/` -- all headers from `D10T08 §15` + `D10T09 §1 §2` -- §1 packaging
- `sdk/lib/` -- `kernel32.lib`, `user32.lib`, `gdi32.lib`, `ntdll.lib`, `libc.lib`, `libixui.a` -- §1 §8
- `sdk/tools/` -- `tcc.exe`, `make.exe` from `D10T09 §5 §9` -- §1 §8
- `tools/` (host-side build tools) -- `jpg2raw.c`, `irespack.c` -- §2 `gendoc.c` pattern
- `10-platform-services/TODO-09-compiler-sdk.md §11` (→ XREF) -- basic `getting-started.md`, `porting-guide.md`
- `10-platform-services/TODO-08-win32-api-surface.md §15` (→ XREF) -- SDK headers + `impossible-cc`
- `10-platform-services/TODO-12-long-term-features.md §3` (→ XREF) -- base `debugger.exe`; this TODO adds on top
- `include/kernel/sched/syscall.h` -- PIT interrupt path for profiler (§4); `SYS_PROFILER_START/STOP`
- `include/kernel/sched/task.h` -- `struct task`, per-task CPU ticks -- §4 profiler RIP sampling
- `include/kernel/elf.h` -- ELF symbol table scan for profiler symbol resolution -- §4 §6
- `include/pe.h` -- PE export directory for profiler symbol resolution -- §4 §6
- `scripts/build.sh` -- extend with `make sdk` target -- §1
- `include/kernel/timer.h` -- `system_get_ticks()`, PIT frequency -- §4

---

## Outcome

`make sdk` produces `sdk/impossible-os-sdk-{version}.zip` with headers, libs, tools, and
docs. Developers install it, run `ixui-new MyApp`, get a skeleton that compiles with
`tcc.exe` immediately. `profile myapp.exe` shows a flame-ready flat profile. `itest.h`
lets any app ship self-tests that run via the `itest` shell command. `debugger.exe` shows
call stacks with symbol names. `scripts/release-sdk.sh` publishes a GitHub Release.

---

## Implementation Order

| Step | Section                                          | 💎/⭐ | Dependency                                                 |
| ---- | ------------------------------------------------ | ----- | ---------------------------------------------------------- |
| 1    | SDK packaging (`make sdk`, ZIP, SHA-256)         | 💎    | `D10T08 §15`; `D10T09 §10`; miniz ZIP writer               |
| 2    | SDK documentation (`gendoc.c`, Markdown API ref) | ⭐    | §1 headers in place; `D10T09 §11` basic docs               |
| 3    | Code samples (7 projects)                        | 💎    | §1 SDK installed; TCC compiles samples                     |
| 4    | Sampling profiler (`profile` command)            | ⭐    | PIT interrupt; `struct task` RIP access; symbol resolution |
| 5    | Unit test framework (`itest.h`)                  | ⭐    | §1 headers; `TODO-01 §8` host-side test pattern            |
| 6    | Debugger enhancements (call stack, disasm)       | 💎    | `D10T12 §3` base debugger; ELF/PE symbol tables            |
| 7    | IxUI starter templates (`ixui-new`)              | ⭐    | §1 SDK; TCC (`D10T09 §5`); §4 samples as template basis    |
| 8    | SDK release pipeline (`release-sdk.sh`)          | ⭐    | §1 zip + SHA-256; GitHub CLI; Registry SDK version         |

---

## 1. SDK Packaging (`make sdk`) `[Sonnet]`

**Source:** `scripts/sdk-package.sh`; integrated as `make sdk` target in root `Makefile`

- [ ] **`make sdk` target**: invoke `scripts/sdk-package.sh` after a successful `make` (check `build/build.log` tail); fail fast if build log does not end `=== BUILD OK ===`
- [ ] **`scripts/sdk-package.sh`**:
  1. Read version string from `HKLM\SYSTEM\Version` Registry key (or `include/kernel/version.h` constant at build time): `SDK_VERSION=$(grep KERNEL_VERSION include/kernel/version.h | awk '{print $3}')`
  2. Assemble staging directory `build/sdk-staging/`:
     - `sdk-staging/include/` ← `sdk/include/` (all headers)
     - `sdk-staging/lib/` ← `sdk/lib/kernel32.lib user32.lib gdi32.lib ntdll.lib libc.lib libixui.a shell32.lib msvcrt.lib`
     - `sdk-staging/bin/` ← `sdk/tools/tcc.exe make.exe ipkg_create.exe` (copy from build output)
     - `sdk-staging/tools/` ← `sdk/tools/debugger.exe profiler.exe` (built by §4 §6)
     - `sdk-staging/docs/` ← `sdk/docs/` (generated by §2)
     - `sdk-staging/samples/` ← `sdk/samples/` (§3)
     - `sdk-staging/README.md` ← `sdk/README.md`
  3. Create ZIP: `zip_create("build/impossible-os-sdk-${SDK_VERSION}.zip", staging/*)` -- use host-side `tools/sdk_zip.c` (wraps miniz `MZ_ZIP_FLAG_WRITE_ZIP64` mode for portability; compiled with `gcc`)
  4. Generate SHA-256: `sha256sum build/impossible-os-sdk-${SDK_VERSION}.zip > build/impossible-os-sdk-${SDK_VERSION}.zip.sha256`
  5. Print: `SDK built: build/impossible-os-sdk-${SDK_VERSION}.zip ($(du -h ...) bytes)`
- [ ] **`make sdk-upload`** target: calls `scripts/release-sdk.sh` (§8); guarded by `SDK_UPLOAD=1` env var to prevent accidental upload
- [ ] **Version bump**: `scripts/bump-version.sh <major|minor|patch>` increments the constant in `include/kernel/version.h`; commits `"build: bump version to X.Y.Z"`
- [ ] **`ipkg_create.exe`** (on-OS package creator): user-mode tool that bundles an app + its assets into an `.ipkg` archive (ZIP + `manifest.ini` metadata); built from `src/tools/ipkg_create.c`; installed to `C:\Impossible\Bin\`

---

## 2. SDK Documentation (`gendoc.c`) `[Sonnet]`

**Host tool:** `tools/gendoc.c` (compiled with `gcc -O2`); integrated as `make docs` target

- [ ] **`tools/gendoc.c`** doc-comment extractor:
  - Scan all `sdk/include/*.h` and subdirectories
  - Extract `/** ... */` multi-line comments immediately preceding a function declaration, typedef, or `#define`
  - Parse: first sentence → description; `@param name desc` → parameter table; `@returns desc` → return value; `@example code` → fenced code block
  - Emit one `sdk/docs/api-reference/{header-name}.md` per header file: H2 per function/type/macro with description, signature, params table, returns, example
  - Fallback for undocumented symbols: emit stub entry `> *Documentation pending.*`
- [ ] **Hand-authored docs** (not generated -- create as static Markdown):
  - `sdk/docs/getting-started.md`: prerequisites (QEMU or real hardware), install steps, "Hello World" console + window in 5 min
  - `sdk/docs/ixui-guide.md`: window lifecycle, layout model, all built-in controls, theming with `theme_set()`, event handling patterns
  - `sdk/docs/porting-guide.md`: extends `D10T09 §11`; adds PE vs ELF differences, Win32 gotchas, `#include <windows.h>` on Impossible OS
  - `sdk/docs/faq.md`: 20 common questions (compile errors, missing symbols, DLL stubs, etc.)
- [ ] **`api-reference/` structure**:
  - `kernel32.md`, `user32.md`, `gdi32.md`, `ntdll.md`, `shell32.md`, `msvcrt.md`, `ixui.md`
  - Index page `api-reference/README.md` with alphabetical function → file links
- [ ] **`make docs`** target: runs `tools/gendoc` on all headers; places output in `sdk/docs/api-reference/`; then copies `sdk/docs/` to `build/sdk-staging/docs/` for packaging

---

## 3. Code Samples `[Sonnet]`

**Directory:** `sdk/samples/`; each has a `Makefile` using `impossible-cc`, a `README.md`, and expected output comment

- [ ] **`hello-console/`**: `main.c` -- `printf("Hello, Impossible OS!\n"); return 0;`; README shows compile + run; expected output: `Hello, Impossible OS!`
- [ ] **`hello-window/`**: `main.c` -- `RegisterClassEx` + `CreateWindowEx` + message loop + `WM_PAINT` `TextOut("Hello, Window!")` + `WM_DESTROY → PostQuitMessage`; expected: window appears with centered text
- [ ] **`file-ops/`**: `main.c` -- `CreateFile` write + `ReadFile` verify + `FindFirstFile` dir list of `C:\Temp\`; expected: prints file listing
- [ ] **`registry/`**: `main.c` -- `RegOpenKey(HKCU, "Software\\MyApp")` + `RegSetValue` + `RegQueryValue` + `RegDeleteKey`; expected: prints read-back value
- [ ] **`networking/`**: `main.c` -- `socket(AF_INET, SOCK_STREAM, 0)` + `connect` to `8.8.8.8:80` + `send` HTTP GET + `recv` 256 bytes + print; expected: prints HTTP response header
- [ ] **`ixui-app/`**: `main.c` -- IxUI window with `IxCreateButton`, `IxCreateTextBox`, `IxCreateListView`; button click appends textbox content to listview; expected: functional GUI
- [ ] **`custom-control/`**: `main.c` -- registers custom `"ColorBox"` window class; `WM_PAINT` fills rect with color from `GWLP_USERDATA`; parent creates 8 color boxes; expected: colored grid
- [ ] **Each sample `Makefile`**:
  ```makefile
  CC = impossible-cc
  CFLAGS = -O2
  hello: main.c
  	$(CC) $(CFLAGS) -o hello.exe main.c
  clean:
  	del hello.exe
  ```

---

## 4. Sampling Profiler `[Opus]`

> Novel: PIT interrupt at 1000 Hz samples RIP per thread; maps samples to symbol names
> from ELF `.symtab` or PE export directory. No prior Impossible OS profiler exists.

**Source:** `src/tools/profiler/profiler.c`; kernel hook in `src/kernel/sched/profiler_hook.c`

- [ ] **Kernel sampling hook**: in the PIT interrupt handler (after updating `g_ticks`): if `g_profiler_active && g_profiler_pid != 0`:
  - Get current task; if `task->pid == g_profiler_pid`: record `RIP` from interrupt frame into `g_profile_samples[g_profile_count++ % PROFILE_BUF_SIZE]`
  - `PROFILE_BUF_SIZE = 65536` (64K samples × 8 bytes = 512 KB via `pmm_alloc_contiguous`)
  - Add `SYS_PROFILER_START(pid)=78` / `SYS_PROFILER_STOP()=79` syscalls to `include/kernel/sched/syscall.h`
- [ ] **`profiler_start(pid)`**: `SYS_PROFILER_START(pid)` → kernel sets `g_profiler_pid`, allocates `g_profile_samples`, sets `g_profiler_active = 1`; returns 0 on success
- [ ] **`profiler_stop()`**: `SYS_PROFILER_STOP()` → kernel clears `g_profiler_active`; copies sample buffer to user-provided buffer via `vmm_read_user`; returns sample count
- [ ] **Symbol resolution** (`profiler_resolve_rip(rip)`):
  - Try PE export directory: walk `g_hwnd_table` loaded-module list for ranges; `IMAGE_DIRECTORY_ENTRY_EXPORT` → find largest address ≤ `rip`; return function name + offset
  - Try ELF `.symtab`: if ELF module in range, scan `Elf64_Sym` table for largest `st_value ≤ rip` with `STT_FUNC`; return `st_name` string
  - Fallback: `"0x{rip:016x}"` hex address
- [ ] **`profiler_report(samples, count, output_buf)`**:
  - Build frequency map: `rip → hit_count` (use simple hash table, 4096 buckets)
  - Sort top 20 by hit count descending
  - Resolve each to symbol name
  - Format:
    ```
    Flat profile: 12543 samples (12.543 sec at 1000 Hz)
    
     %     Samples  Function
     23.4%   2939   pmm_alloc_frame
     18.1%   2270   vfs_read
      9.7%   1217   ttf_measure_width
      ...
    ```
- [ ] **`profile` shell command**: `profile <exe.exe> [args...]`:
  - `task_exec(exe)` in child process; parent calls `profiler_start(child_pid)` immediately
  - Wait for child exit (`SYS_WAITPID`); call `profiler_stop()` to collect samples
  - `profiler_report(...)` → print to console
  - Also write raw samples to `C:\Temp\profile-{timestamp}.prf` for future flame-graph tooling

---

## 5. Unit Test Framework (`itest.h`) `[Sonnet]`

**Output:** `sdk/include/itest.h` (single-header, ~250 lines); host-side integration via `scripts/test-libs.sh`

- [ ] **`itest.h` core macros**:
  ```c
  /* Suite registration */
  #define ITEST_SUITE(name) \
      void itest_suite_##name(void); \
      static itest_reg_t __itest_reg_##name \
          __attribute__((used, section("itest_suites"))) = { #name, itest_suite_##name }; \
      void itest_suite_##name(void)

  #define ITEST_CASE(name) static void itest_case_##name(void); \
      static itest_case_reg_t __itest_casereg_##name = { #name, itest_case_##name, 0 }; \
      /* auto-registered by suite runner */ \
      static void itest_case_##name(void)

  /* Assertions */
  #define ITEST_ASSERT(expr) \
      do { if (!(expr)) { itest_fail(__FILE__, __LINE__, #expr); return; } } while(0)
  #define ITEST_ASSERT_EQ(a, b) \
      do { if ((a) != (b)) { itest_fail_eq(__FILE__, __LINE__, #a, #b, (int64_t)(a), (int64_t)(b)); return; } } while(0)
  #define ITEST_ASSERT_STR_EQ(a, b) \
      do { if (strcmp(a, b) != 0) { itest_fail_str(__FILE__, __LINE__, #a, #b, a, b); return; } } while(0)
  #define ITEST_ASSERT_NULL(p) \
      do { if ((p) != NULL) { itest_fail(__FILE__, __LINE__, #p " expected NULL"); return; } } while(0)
  #define ITEST_EXPECT_FAIL(expr) \
      do { int __r = (int)(expr); (void)__r; /* mark expected failure, continue */ } while(0)
  ```
- [ ] **`itest_run_all()`**: walks `itest_suites` linker section; for each suite: runs all registered test cases; prints `[PASS] suite::case` or `[FAIL] suite::case: <message> at file:line`; at end: prints `Passed: N / Total: M`; returns 0 if all pass, 1 if any fail; calls `ExitProcess(result)` when used as standalone binary
- [ ] **Output format**:
  ```
  [RUN ] string_suite
    [PASS] string_suite::memcpy_basic
    [PASS] string_suite::strlen_empty
    [FAIL] string_suite::snprintf_overflow: assertion failed: buf[127] == '\0' at string_test.c:42
  [RUN ] math_suite
    [PASS] math_suite::sin_pi_half
  Results: 3 passed, 1 failed (4 total)
  ```
- [ ] **`make test` host-side**: `scripts/test-libs.sh` compiles a host binary (`gcc -ffreestanding -nostdlib ...`) linking `src/libs/libc/string.c` + test files; runs `itest_run_all()`; exit 0/1; called by CI
- [ ] **`itest` shell command** (on-OS): `itest <test-binary.exe>` launches test binary, captures exit code; prints pass/fail summary; returns exit code to shell

---

## 6. Debugger Enhancements `[Opus]`

> Extends `10-platform-services/TODO-12 §3` base debugger. Novel additions: `.pdata`
> section unwind, x86-64 disassembler, source-level debug map. Complex algorithm design.

**Source:** `src/tools/debugger/` (extends existing `debugger.exe`)

- [ ] **Call stack display** via `.pdata` section unwind:
  - Windows x64 ABI `.pdata` section: array of `RUNTIME_FUNCTION { DWORD BeginAddress, EndAddress, UnwindInfoAddress }`; `UNWIND_INFO` encodes prolog operations (push RBP, alloc stack, save non-volatile regs)
  - `stack_unwind(rip, rsp, rbp, frame_count_out)`:
    1. Find `RUNTIME_FUNCTION` entry for `rip` in loaded PE's `.pdata` section (binary search by `BeginAddress`)
    2. Parse `UNWIND_INFO`; replay unwind codes backward to recover caller's `RSP` and return address
    3. If no `.pdata` (ELF/frameless): frame-pointer fallback -- `*(rsp)` as return addr, `rbp` chain walk
    4. Recurse up to 64 frames; stop at `0xFFFFFFFFFFFFFFFF` sentinel or `rip < 0x1000`
  - Print: `#N  0x{addr:016x}  {symbol_name}+{offset}  ({module}.exe)`
- [ ] **Register dump with symbolic names**: `debugger regs <pid>`: call `GetThreadContext`; print all 16 GPRs + `RIP`/`RFLAGS`/`CS`/`SS` with symbolic names; flag decode: `OF ZF SF CF PF AF` + numeric value
- [ ] **Breakpoint list management**: `debugger bplist` shows all breakpoints with index, address, hit count, enable state; `debugger bp clear <index>` removes one; `debugger bp enable/disable <index>` toggles; persist breakpoint list to `C:\Temp\dbg-{pid}.bps` Registry key
- [ ] **Minimal x86-64 disassembler** (`src/tools/debugger/disasm.c`, ~600 lines):
  - Single-instruction decode: opcode prefix scan (REX, 66h, F2h/F3h), 1–3-byte opcode, ModRM/SIB, displacement, immediate
  - Cover ~80 most common instructions: `MOV`, `ADD`, `SUB`, `CMP`, `TEST`, `JMP`, `JCC`, `CALL`, `RET`, `PUSH`, `POP`, `LEA`, `XOR`, `AND`, `OR`, `NOT`, `NEG`, `INC`, `DEC`, `MUL`, `IMUL`, `DIV`, `IDIV`, `NOP`, `INT3`, `SYSCALL`, `SYSRET`, `HLT`, `XCHG`, `MOVSX`, `MOVZX`, SSE `MOVAPS`/`MOVDQU`
  - `disasm_one(const uint8_t *bytes, char *out, size_t out_len)` → returns instruction byte length (1–15); `"??  db 0x{b}"` for unknown
  - `debugger disasm <pid> <rip> [count=10]`: read `count` instructions from target process memory; print `{addr}:  {hex bytes}  {mnemonic}`
- [ ] **Source-level debugging** (stretch): if binary has `.debug_info` DWARF or `.pdb`-format sidecar (`{exe}.pdb` in same dir): map `rip` → source file + line number; print alongside call stack; `debugger src <pid>` shows current source line

---

## 7. IxUI Starter Templates (`ixui-new`) `[Sonnet]`

**Source:** `src/tools/ixui_new.c` (host or on-OS tool); installed as `ixui-new.exe`

- [ ] **`ixui-new <app-name> [--type <console|window|ixui>]`** generates `<app-name>/` directory:
  ```
  <app-name>/
  ├── main.c          ← template code
  ├── Makefile        ← uses impossible-cc / tcc
  ├── app.ico         ← 32×32 default icon (copy from SDK resources)
  ├── README.md       ← "Replace this with your app description"
  ├── install.ini     ← name, version, author, category, min_os_version
  └── manifest.ini    ← app name, entry point, required_permissions
  ```
- [ ] **Template `main.c` content per type**:
  - **Console**: `#include <stdio.h>\nint main(int argc, char **argv) { printf("Hello from %s!\n", argv[0]); return 0; }`
  - **Window**: `#include <windows.h>` + `WinMain` + `RegisterClassEx` + `CreateWindowEx` + message loop + `WM_PAINT` stub + `WM_DESTROY`
  - **IxUI**: `#include <ixui.h>` + `IxCreateWindow` + `IxRegisterWindowClass` + `IxGetMessage` loop + one `IxButton` + click handler
- [ ] **`install.ini`** template:
  ```ini
  [App]
  Name = <app-name>
  Version = 1.0.0
  Author = Your Name
  Category = Utilities
  MinOSVersion = 1.0.0
  ```
- [ ] **Post-generation compile check**: after emitting files, run `tcc.exe main.c -o <app-name>.exe` (or `impossible-cc`) and check exit code; print `✓ Template compiled successfully` or `✗ Compile failed -- check your SDK installation`
- [ ] **`ixui-new` help**: `--list-types` shows available templates; `--sdk-path <path>` overrides SDK location (defaults to `C:\Impossible\Include\`)
- [ ] **On-OS availability**: `ixui-new.exe` installed to `C:\Impossible\Bin\` at SDK install time; also available as host-side binary for cross-development

---

## 8. SDK Release Pipeline `[Sonnet]`

**Source:** `scripts/release-sdk.sh`

- [ ] **`scripts/release-sdk.sh`**:
  1. Run `bash scripts/build.sh` -- verify `=== BUILD OK ===`
  2. Run `make sdk` (§1) -- verify ZIP exists
  3. Run `sha256sum` -- verify `.sha256` file matches
  4. Run `make test` (§5) -- verify all tests pass; abort if exit code ≠ 0
  5. Read version: `VERSION=$(grep KERNEL_VERSION include/kernel/version.h | awk '{print $3}')`
  6. Tag: `git tag sdk/v${VERSION} -m "SDK release ${VERSION}"`
  7. Push: `git push origin sdk/v${VERSION}`
  8. GitHub Release: `gh release create sdk/v${VERSION} build/impossible-os-sdk-${VERSION}.zip build/impossible-os-sdk-${VERSION}.zip.sha256 --title "Impossible OS SDK v${VERSION}" --notes-file sdk/docs/CHANGELOG.md`
  9. Print: `SDK v${VERSION} released → https://github.com/rizonesoft/impossible-os/releases/tag/sdk/v${VERSION}`
- [ ] **`sdk-update` shell command** (on-OS):
  - Read `HKLM\SYSTEM\SDK\InstalledVersion` (set by `D10T09 §10` SDK installer)
  - Fetch `https://sdk.impossible-os.dev/latest` → get `latest_version` string (via `SYS_PING`-equivalent HTTP GET)
  - If `latest_version > installed_version`: prompt `"SDK update available: {latest}. Download? [Y/n]"`
  - On yes: download ZIP, verify SHA-256, extract to `C:\Impossible\Include\` + `C:\Impossible\Bin\`, update `HKLM\SYSTEM\SDK\InstalledVersion`
- [ ] **`HKLM\SYSTEM\SDK\InstalledVersion`**: string value e.g. `"1.2.0"`; written by SDK installer; read by `sdk-update` + `sdk-info` shell commands
- [ ] **`sdk-info` shell command**: prints `Installed SDK: v{version}`, `Headers: C:\Impossible\Include\`, `Libs: C:\Impossible\Lib\`, `Compiler: tcc.exe v{tcc-version}`

---

## OS Comparison


| ⭐  | Feature                                    | 🪟 Win11                                   | 🐧 Linux                                          | 🚀 Impossible OS                                                             |
| --- | ------------------------------------------ | ------------------------------------------ | ------------------------------------------------- | ---------------------------------------------------------------------------- |
| 💎  | SDK distribution                           | ✅ Windows SDK installer (GB-size); WinGet | ✅ `apt install build-essential`; distro packages | ⬜ §1 -- single-ZIP `impossible-os-sdk-{ver}.zip` with SHA-256; `make        |
| ⭐  | Doc-comment → Markdown API reference       | ✅ MSDN auto-generated; WinRT metadata     | ✅ Doxygen; kernel-doc                            | ⬜ §2 -- `tools/gendoc.c` extracts `/ */` from                               |
| 💎  | Code samples                               | ✅ MSDN samples; `winui3gallery.exe`       | ✅ Linux kernel samples; GTK demos                | ⬜ §3 -- 7 samples each with Makefile                                        |
| ⭐  | 1000 Hz PIT sampling profiler              | ✅ VTune; ETW/xperf; WPR/WPA               | ✅ `perf stat`/`perf record`; gprof               | ⬜ §4 -- PIT hook records RIP per-task                                       |
| ⭐  | Single-header test framework               | ✅ Google Test; CTest                      | ✅ CUnit; Unity; `make check`                     | ⬜ §5 `ITEST_SUITE`/`ITEST_CASE`/`ITEST_ASSERT_EQ`; linker auto-registration |
| 💎  | Debugger call-stack display + disassembler | ✅ WinDbg; Visual Studio debugger          | ✅ GDB; LLDB; `addr2line`                         | ⬜ §6 -- `.pdata` unwind + frame-pointer fallback                            |
| ⭐  | IxUI skeleton generator                    | ✅ VS project wizard; `dotnet new`         | ✅ `gnome-builder` templates; `cookiecutter`      | ⬜ §7 -- 3 template types; post-generation TCC                               |
| ⭐  | Automated SDK release pipeline             | ✅ Azure DevOps; GitHub Actions; WiX       | ✅ Launchpad PPA; Copr; GitHub Actions            | ⬜ §8 -- `git tag` → `gh release                                             |

Impossible OS's `⭐` advantage: the **entire SDK is a single well-known ZIP** (not a GB
installer), the **profiler is kernel-native** at 1000 Hz with zero user-space overhead,
and `ixui-new` does a **live compile check** immediately after template generation --
shortening the "zero to working app" time to under 2 minutes on-OS.

---

## Verification

Run `bash scripts/build.sh` then the specific step for each item.

- [ ] **SDK packaging**: `make sdk` succeeds; `build/impossible-os-sdk-*.zip` exists; `unzip -l` shows `include/`, `lib/`, `bin/`, `docs/`, `samples/` directories; `sha256sum -c *.sha256` passes
- [ ] **gendoc**: `make docs` runs without error; `sdk/docs/api-reference/kernel32.md` exists and contains at least one function with signature + description
- [ ] **Samples compile**: `cd sdk/samples/hello-console && make` → `hello.exe` produced; run on-OS → prints `Hello, Impossible OS!`; `hello-window` → window appears; all 7 samples compile without error
- [ ] **Profiler**: `profile hello.exe` runs and exits; prints flat profile with at least 5 symbols; `pmm_alloc_frame` or kernel functions appear in top 20 for a compute-heavy test; `C:\Temp\profile-*.prf` created
- [ ] **itest.h**: compile `sdk/samples/itest-demo/` (string + math tests); run on-OS `itest itest-demo.exe` → `Passed: N / Total: N`; introduce a deliberate failure → `FAIL` output + exit code 1; `make test` on host passes for all lib tests
- [ ] **Debugger call stack**: `debugger.exe attach <pid>`; type `bt` → shows ≥ 5 stack frames with symbol names; `disasm <pid> <rip> 5` → prints 5 disassembly lines; `regs <pid>` → prints all GPRs with flag decode
- [ ] **ixui-new**: `ixui-new MyTestApp --type ixui` → directory created with all 6 files; `cd MyTestApp && make` → compiles without error; run on-OS → IxUI window with button appears
- [ ] **Release pipeline**: `scripts/release-sdk.sh` dry-run (no `gh` push): ZIP exists, SHA-256 matches, tests pass, tag would be `sdk/v{version}`
- [ ] Commit: `"sdk: packaging, gendoc, samples, profiler, itest.h, debugger enhancements, ixui-new, release pipeline"`
