# Development Tooling & Automation

> Complete build system, test framework, asset pipeline, and development utilities for Impossible OS.

## Overview

Impossible OS uses a single-script build system (`bash scripts/build.sh`) that wraps a Makefile-based pipeline. The toolchain is Clang-19/LLD-19 targeting `x86_64-elf` in freestanding mode. The development environment includes incremental builds with parallel compilation, automated QEMU testing, a kernel unit test framework, an asset pipeline with validation, and code intelligence via clangd + Srclight.

```mermaid
graph TD
    subgraph Build
        A[scripts/build.sh] --> B[Makefile]
        B --> C["clang-19 (kernel + userland)"]
        B --> D["nasm (assembly)"]
        B --> E["Host gcc (tools)"]
        C --> F[kernel.exe ELF]
        D --> F
        E --> G["Asset tools (irespack, jpg2raw)"]
        G --> H[sysroot/]
        F --> I["build/system-disk.img (GPT)"]
        H --> I
    end

    subgraph Test
        I --> J[QEMU smoke test]
        I --> K[Filesystem test suite]
        I --> L[Unit test framework]
    end

    subgraph Deploy
        I --> M[USB write]
        I --> N[VirtualBox]
        I --> O[Hyper-V]
    end
```

---

## Build System

### Toolchain

| Tool          | Binary                                        | Purpose                                    |
| ------------- | --------------------------------------------- | ------------------------------------------ |
| C compiler    | `clang-19 --target=x86_64-elf`                | Kernel + userland C code                   |
| Assembler     | `nasm`                                        | x86-64 assembly (`.asm`)                   |
| Linker        | `ld.lld-19`                                   | ELF linking (kernel, user programs)        |
| Object tools  | `llvm-objcopy-19`, `llvm-ar-19`, `llvm-nm-19` | Binary manipulation                        |
| Host compiler | `gcc`                                         | Build tools only (jpg2raw, irespack, etc.) |

> [!NOTE]
> Migrated from GCC to Clang-19/LLD-19 for 8% smaller kernel binary (2,649,920 bytes vs 2,880,648 bytes GCC) and slightly faster builds (6.8s vs 8.0s clean at -j12). NASM pipeline is untouched — assembly files remain compiled by NASM.

### Compiler Flags

```
CFLAGS := -Wall -Wextra -Werror -ffreestanding -nostdlib -nostdinc \
          --target=x86_64-elf -fno-stack-protector -fno-pie -mno-red-zone \
          -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -std=gnu11 -O2 -g \
          -MMD -MP
```

Key flags:
- `-MMD -MP` — generates `.d` dependency files for incremental builds
- `--target=x86_64-elf` — cross-compilation target
- `-ffreestanding -nostdlib -nostdinc` — no standard library, no host headers

### Build Script

All builds go through `scripts/build.sh` — never raw `make` commands.

| Command                           | Description         |
| --------------------------------- | ------------------- |
| `bash scripts/build.sh`           | Incremental build   |
| `bash scripts/build.sh clean`     | Full clean build    |
| `bash scripts/build.sh run`       | Build + launch QEMU |
| `bash scripts/build.sh clean run` | Clean build + QEMU  |

**Features:**
- Live progress bar during compilation
- Auto-creates `build/` directory
- Writes log to `build/build.log` with `=== BUILD OK ===` or `=== BUILD FAILED ===` sentinel
- Wraps kernel build with `bear --append --` on clean builds (generates `compile_commands.json`)

### Incremental Builds

Uses `-MMD -MP` dependency tracking:

```mermaid
graph LR
    A[source.c] --> B[source.o + source.d]
    B --> C["make includes .d files"]
    C --> D["Only changed files + dependents recompile"]
```

| Scenario                       | Time  | Speedup               |
| ------------------------------ | ----- | --------------------- |
| Clean build (-j12)             | 8.0s  | Baseline              |
| Incremental (single .c change) | 3.3s  | **5.8× faster**       |
| Header change (printk.h)       | 12.1s | 71/96 files (correct) |

### Parallel Compilation

Default: `-j$(nproc)`. Override with `--jobs=N`.

| Configuration      | Time                    |
| ------------------ | ----------------------- |
| `-j1` clean        | 19.6s                   |
| `-j12` clean       | 8.0s (**2.5× speedup**) |
| `-j12` kernel only | 3.7s (**3.6× speedup**) |

> [!NOTE]
> Generated headers (`os_logo.h`, `bsod_icon.h`, `boot_splash_font_data.h`) use order-only prerequisites (`| $(GENERATED_HDRS)`) to prevent races on first clean build.

### Build Version & Metadata

Version is auto-generated using CalVer: `YY.M.D` (e.g., `26.3.17`).

**Auto-generated `include/build_info.h`:**

| Define            | Source                            | Example                |
| ----------------- | --------------------------------- | ---------------------- |
| `BUILD_NUMBER`    | `.build_number` (auto-increment)  | `809`                  |
| `BUILD_COMMIT`    | `git rev-parse --short HEAD`      | `d1016ab`              |
| `BUILD_BRANCH`    | `git rev-parse --abbrev-ref HEAD` | `main`                 |
| `BUILD_TIMESTAMP` | `date -u +%Y-%m-%dT%H:%M:%SZ`     | `2026-03-17T16:13:00Z` |
| `BUILD_VERSION`   | CalVer from build date            | `26.3.17`              |

**Where version appears:**
- Boot log first line: `Impossible OS v26.3.17 (build 809, main@d1016ab, ...)`
- Shell `ver` command: same format
- BSOD crash screen: version footer in dim color

> [!IMPORTANT]
> `include/build_info.h` is auto-generated by the Makefile's `.FORCE` target and uses `cmp -s` to avoid rewriting when content is unchanged (prevents full recompilation). It is in `.gitignore`.

### Output

The build produces a bootable GPT disk image: `build/system-disk.img`

| Partition  | Size      | Format        | Contents                               |
| ---------- | --------- | ------------- | -------------------------------------- |
| EFI System | 64 MiB    | FAT32         | `BOOTX64.EFI`, `kernel.exe`            |
| Logs       | 16 MiB    | FAT32         | Runtime debug logs                     |
| IXFS       | Remaining | IXFS (custom) | System files, fonts, icons, wallpapers |

---

## Toolchain & Dependency Management

### Clang/LLD Migration

Migrated from GCC to Clang-19/LLD-19:

| Aspect             | Before (GCC)     | After (Clang-19)                 |
| ------------------ | ---------------- | -------------------------------- |
| Compiler           | `x86_64-elf-gcc` | `clang-19 --target=x86_64-elf`   |
| Linker             | `ld`             | `ld.lld-19`                      |
| Kernel size        | 2,880,648 bytes  | 2,649,920 bytes (**8% smaller**) |
| Clean build (-j12) | 8.0s             | 6.8s                             |

> [!CAUTION]
> **UEFI bootloader PE/COFF conversion:** `llvm-objcopy` does NOT support `--target efi-app-x86_64`. The pipeline uses GNU `objcopy` for the EFI conversion step: Clang → ELF → GNU objcopy → PE/COFF.

> [!WARNING]
> Clang is stricter about `-Wunused-function` on static inline helpers. 11 port I/O helpers across 7 driver files needed `__attribute__((unused))`.

### System Dependency Installer

`scripts/setup-deps.sh` — detects distro and installs all build dependencies:

| Distro        | Package Manager |
| ------------- | --------------- |
| Ubuntu/Debian | `apt`           |
| Fedora        | `dnf`           |
| Arch          | `pacman`        |

**Packages installed:**
- Build: `nasm`, `clang-19`, `lld-19`, `llvm-19`
- Disk: `xorriso`, `mtools`, `dosfstools`, `parted`
- Test: `qemu-system-x86`, `ovmf`
- Tools: `python3`, `python3-pil`, `bear`, `clangd-19`, `cppcheck`

Idempotent — checks `command -v` before installing. Colored output with ✓/·/✗ status.

### One-Command Setup

```bash
git clone https://github.com/rizonesoft/impossible-os.git
cd impossible-os
bash scripts/setup.sh    # installs deps + verification build
bash scripts/build.sh run
```

`scripts/setup.sh` runs `setup-deps.sh` then does a verification `build.sh clean`.

---

## Emulator Testing Scripts

### QEMU Test Runner

`scripts/run-qemu.sh` — primary test environment.

| Feature       | Flag/Setting                              |
| ------------- | ----------------------------------------- |
| UEFI firmware | `-bios OVMF.fd` (auto-copied to `build/`) |
| Disk          | AHCI controller                           |
| Serial        | `-serial stdio`                           |
| Resolution    | VGA mode                                  |

`bash scripts/build.sh run` wraps this: build + auto-launch.

### VirtualBox Test Runner

`scripts/vm/run-vbox.sh` — cross-platform VirtualBox launcher.

| Setting  | Value                     |
| -------- | ------------------------- |
| VM Name  | `ImpossibleOS`            |
| Firmware | EFI                       |
| Storage  | AHCI                      |
| Graphics | VMSVGA, 1280×720          |
| Memory   | 2048 MB                   |
| CPUs     | 4                         |
| Mouse    | PS/2 (no Guest Additions) |

**Features:**
- Converts raw `system-disk.img` → VDI on each run
- Properly unregisters old VDI before re-converting (avoids UUID mismatch)
- `--headless` flag for CI
- `--debug` injects DEBUG flag via `mcopy`

### Hyper-V Test Runner

See [TODO-008-Hyper-V-Runner](../../todo/000-Infrastructure/TODO-008-Hyper-V-Runner.md) — dedicated runner with VMBus, synthetic devices, and Gen 2 VM support.

---

## Hardware Deployment Scripts

### USB Write (Windows)

`scripts/deploy/write-usb.ps1` — GPT-partitioned USB via PowerShell.

Creates EFI (FAT32) + System (FAT32) + Logs (FAT32) partitions. Auto-detects USB drive with safety prompts.

### USB Write (Linux)

`scripts/deploy/write-usb.sh` — raw `dd` write with double confirmation.

```mermaid
graph LR
    A[List removable USB] --> B[User selects device]
    B --> C["Type 'YES'"]
    C --> D[Type device name]
    D --> E["dd if=system-disk.img"]
    E --> F[sync + partprobe]
    F --> G[Mount + verify boot files]
```

> [!CAUTION]
> Requires root (`sudo`). The image already contains GPT + all partitions — no `parted`/`mkfs` needed. Double confirmation prevents accidents.

### USB Log Reader

`scripts/deploy/read-usb-log.sh` — retrieves debug logs from USB boot.

| Feature   | Description                                                    |
| --------- | -------------------------------------------------------------- |
| Mount     | Read-only (`-o ro`) — safe for forensic collection             |
| Detection | Auto-detects third partition (`/dev/sdX3` or `/dev/nvme0n1p3`) |
| Output    | Copies to `build/logs/<timestamp>/`                            |
| Scanning  | Highlights panic/BSOD/fault keywords in red                    |

---

## Test Framework

### Kernel Unit Test Framework

Header: `include/kernel/test/test.h`, implementation: `src/kernel/test/test_runner.c`

**API:**

| Function                        | Purpose                                   |
| ------------------------------- | ----------------------------------------- |
| `TEST_ASSERT(cond, msg)`        | Assert condition, log pass/fail to serial |
| `test_suite_register(name, fn)` | Register a test suite                     |
| `test_runner_init()`            | Initialize + register all suites          |
| `test_runner_run()`             | Run all suites, print summary             |

**Output format:**
```
[PASS] pmm :: alloc and free returns same frame
[FAIL] heap :: no overlap (test_heap.c:45)
--- TEST SUMMARY: 11 passed, 1 failed ---
```

> [!IMPORTANT]
> All test code is inside `#ifdef KERNEL_TESTS`. Without `-DKERNEL_TESTS` in CFLAGS, test code compiles to empty objects — zero overhead in production builds.

### Core Subsystem Tests

5 test files in `src/kernel/test/`, 12 test suites total:

| File              | Suites | What's Tested                                                  |
| ----------------- | ------ | -------------------------------------------------------------- |
| `test_pmm.c`      | 2      | alloc+free frame reuse, contiguous allocation, page alignment  |
| `test_heap.c`     | 3      | kmalloc+kfree round-trip, zero-byte→NULL, no-overlap, krealloc |
| `test_vfs.c`      | 3      | create/write/read/close, open nonexistent, mkdir+rmdir         |
| `test_sched.c`    | 1      | thread_create returns valid TID                                |
| `test_registry.c` | 3      | set/get REG_DWORD, set/get REG_SZ, key cleanup                 |

> [!NOTE]
> VFS tests use IXFS paths (`C:\Impossible\...`) and clean up temp files. Registry tests use Win32 API (`RegSetValueEx`, `RegGetValue`, `RegDeleteKey`). Scheduler tests are minimal — preemptive testing requires runtime.

### Automated QEMU Smoke Test

`scripts/test-smoke.sh` — headless boot verification.

```mermaid
graph LR
    A[Clean build] --> B["QEMU headless (-display none)"]
    B --> C[Capture serial 30s]
    C --> D{Contains 'Boot complete in'?}
    D -->|Yes| E["SMOKE TEST PASSED (exit 0)"]
    D -->|No| F{Contains PANIC/FAULT?}
    F -->|Yes| G["SMOKE TEST FAILED (exit 1)"]
    F -->|No| H[Timeout → FAILED]
```

- Uses KVM acceleration when available (`/dev/kvm`)
- Serial output to file (avoids buffering issues)
- Shows boot time on pass, last 10 serial lines on fail

### Filesystem Test Suite

`scripts/test-fs.sh` — tests filesystem drivers against real disk images.

| Image Source               | Filesystems                                               |
| -------------------------- | --------------------------------------------------------- |
| `tools/make-test-disks.sh` | FAT32, exFAT, ext2/3/4, NTFS, IXFS, ISO 9660, Joliet, UDF |

Each test: fresh OVMF vars, 20s timeout, KVM when available. Optical media (ISO/UDF) attached as ATAPI CD via `ide-cd` on AHCI port 1. Logs saved per-filesystem in `build/fs-tests/<name>.log`.

Selective testing: `bash scripts/test-fs.sh fat32 ext4`

---

## Development Utilities

### Symbol Map Generator

Provides `func_name+0xoffset` in BSOD stack traces.

```mermaid
graph LR
    A["llvm-nm-19 -n kernel.exe"] --> B["kernel.map (text)"]
    B --> C["convert_symmap.py"]
    C --> D["kernel.sym (KSYM binary)"]
    D --> E["Copied to C:\\Impossible\\System\\kernel.sym"]
    E --> F["symtab_init() loads at boot"]
    F --> G["symtab_resolve() — O(log n) binary search"]
```

| Detail     | Value                                            |
| ---------- | ------------------------------------------------ |
| Format     | KSYM: 8-byte addr + 32-byte name (packed)        |
| Size       | ~40 KB for ~1000 symbols                         |
| Lookup     | O(log n) binary search — safe in panic context   |
| Allocation | PMM (not kmalloc)                                |
| Filters    | Only T/t/D/d symbols, skips `.` and `$` prefixes |

### Code Size Tracking

`scripts/size-report.sh` — tracks binary sizes across builds.

| Metric            | Current | Threshold    |
| ----------------- | ------- | ------------ |
| `kernel.exe`      | ~2.6 MB | Warn at 8 MB |
| `system-disk.img` | 512 MB  | Warn at 1 GB |

Features: top 10 largest `.o` files, section breakdown via `llvm-size-19`, delta tracking with colored output (red = grew, green = shrank), CSV history in `build/size-history.csv`.

### Code Style Linter

`scripts/lint.sh` — 6 automated style checks.

| Check                  | Type    | Notes                                         |
| ---------------------- | ------- | --------------------------------------------- |
| snake_case functions   | Error   | Excludes Win32 API wrappers (`Reg*`, `HKEY*`) |
| UPPER_CASE macros      | Error   | Flags pure lowercase `#define`                |
| `#pragma once`         | Error   | Required in every `.h`                        |
| Lines ≤ 120 chars      | Error   | Excludes comment lines                        |
| No trailing whitespace | Error   | —                                             |
| Functions ≤ 50 lines   | Warning | Doesn't fail build                            |

Excludes auto-generated files (`build_info.h`, `os_logo.h`, etc.) and third-party code (`stb_truetype`, `stb_image`).

Supports path filtering: `bash scripts/lint.sh src/kernel/mm/`

### GDB Debug Script

`scripts/debug.sh` — enhanced GDB debugging.

| Feature             | Flag                                   |
| ------------------- | -------------------------------------- |
| Default breakpoints | `kernel_main`, `panic`, fault handlers |
| Custom breakpoint   | `--breakpoint=<function>`              |
| Skip defaults       | `--no-default-bp`                      |

Auto-generates `build/.gdbinit-kernel` with Intel syntax, pagination off, symbol count banner. Auto-builds kernel if not found. Cleans up QEMU on GDB exit.

### clangd + Bear (Code Intelligence)

Provides deep C code intelligence for editors.

| Component | File                       | Notes                                                |
| --------- | -------------------------- | ---------------------------------------------------- |
| Config    | `.clangd` (repo root)      | `--target=x86_64-elf`, `-nostdlib`, `-ffreestanding` |
| Database  | `compile_commands.json`    | Generated by Bear on `build.sh clean` (101 entries)  |
| Editor    | VS Code + clangd extension | `clangd.path` → `/usr/bin/clangd-19`                 |

> [!WARNING]
> **clangd is LSP, not MCP.** Attempting to add clangd as an MCP server causes it to hang — the protocols are incompatible. Use clangd via the VS Code/Cursor clangd extension (LSP).

> [!NOTE]
> Bear only wraps the kernel build step (not userland/host tools) to avoid PIPESTATUS issues. Uses `bear --append --` to accumulate entries. `.clangd` uses `Index.Background: Build` for faster indexing.

---

## Asset Pipeline

### Build Target

`make assets` groups 7 sub-targets with stamp-file dependency tracking:

| Sub-target           | Source                                 | Output                                |
| -------------------- | -------------------------------------- | ------------------------------------- |
| `os-logo`            | `resources/icons/color/*.png`          | `include/os_logo.h` (BGRA C array)    |
| `bsod-icon`          | `resources/icons/bsod/bsod_icon.png`   | `include/bsod_icon.h`                 |
| `boot-font`          | Font data                              | `include/boot_splash_font_data.h`     |
| `sysroot-fonts`      | `resources/fonts/*.ttf` (11 files)     | `sysroot/Impossible/Fonts/`           |
| `sysroot-wallpapers` | `resources/backgrounds/background.jpg` | `sysroot/Impossible/Wallpapers/`      |
| `sysroot-cursors`    | `resources/cursors/` (Adwaita XCursor) | `sysroot/Impossible/System/Cursors/`  |
| `sysroot-icons`      | `resources/icons/color/`               | `sysroot/Impossible/Icons/icons.ires` |

> [!NOTE]
> Cursors are Adwaita XCursor format (not BMP) — the kernel's cursor driver reads XCursor natively. Icon packing uses the `irespack` host tool to create IRES bundles.

### Asset Validation

`make validate-assets` runs `tools/validate-assets.py` **before** any asset conversion:

| Asset Type     | Count | Validation                                  |
| -------------- | ----- | ------------------------------------------- |
| PNG icons      | 14    | Expected dimensions, RGBA, size < 1 MB      |
| TTF fonts      | 11    | Parseable, glyph count > 0, sfVersion check |
| XCursor        | 11    | Dimensions ≤ 256, hotspot within bounds     |
| JPEG wallpaper | 1     | SOI/EOI markers + Pillow decode (optional)  |

No external dependencies (stdlib only, Pillow optional for JPEG). Negative test confirmed: corrupted PNG → exit 1.

---

## Local CI Hooks

### Pre-Commit Lint Hook

`.githooks/pre-commit` — opt-in lint checking on commit.

**Setup:** `git config core.hooksPath .githooks`

| Behavior      | Detail                             |
| ------------- | ---------------------------------- |
| Scope         | Only staged `.c`/`.h` files        |
| Fast path     | No C files staged → exits in < 1ms |
| Errors        | Block commit (exit 1)              |
| Warnings      | Don't block                        |
| Deleted files | Skipped (`--diff-filter=d`)        |

---

## Scripts Directory Structure

```
scripts/
├── build.sh                 ← Core build (daily)
├── run-qemu.sh              ← QEMU launcher (daily)
├── debug.sh                 ← GDB debug (daily)
├── setup.sh                 ← One-command setup
├── setup-deps.sh            ← Dependency installer
├── test-smoke.sh            ← Automated smoke test
├── test-fs.sh               ← Filesystem test suite
├── size-report.sh           ← Binary size tracker
├── lint.sh                  ← Code style linter
├── deploy/
│   ├── write-usb.ps1        ← USB write (Windows)
│   ├── write-usb.bat
│   ├── write-usb.sh         ← USB write (Linux)
│   └── read-usb-log.sh      ← USB log reader
├── vm/
│   ├── run-vbox.sh          ← VirtualBox (Linux)
│   ├── run-vbox.ps1         ← VirtualBox (Windows)
│   ├── run-vbox.bat
│   ├── run-hyperv.ps1       ← Hyper-V (Windows)
│   ├── run-hyperv.bat
│   ├── run-qemu-kvm.sh      ← QEMU KVM (Linux)
│   ├── run-qemu-kvm.bat
│   ├── run-qemu-tcg.sh      ← QEMU TCG (Linux)
│   ├── run-qemu-tcg.bat
│   └── run-qemu.ps1         ← QEMU (Windows)
└── secure-boot/
    └── build-shim.sh        ← Shim build (one-time)
```

---

## Key Files

| File                            | Purpose                                         |
| ------------------------------- | ----------------------------------------------- |
| `scripts/build.sh`              | Core build script with progress bar + sentinel  |
| `scripts/run-qemu.sh`           | Primary QEMU launcher                           |
| `scripts/debug.sh`              | GDB debug with symbol-aware breakpoints         |
| `scripts/setup.sh`              | One-command dev environment setup               |
| `scripts/setup-deps.sh`         | System dependency installer                     |
| `scripts/test-smoke.sh`         | Headless QEMU boot verification                 |
| `scripts/test-fs.sh`            | Filesystem driver test suite                    |
| `scripts/size-report.sh`        | Binary size tracker + CSV history               |
| `scripts/lint.sh`               | Code style linter (6 checks)                    |
| `tools/convert_symmap.py`       | nm → KSYM binary symbol table                   |
| `tools/validate-assets.py`      | Build-time asset validation (37 assets)         |
| `include/build_info.h`          | Auto-generated build metadata (in `.gitignore`) |
| `.clangd`                       | clangd language server config                   |
| `compile_commands.json`         | Bear compilation database (generated)           |
| `.githooks/pre-commit`          | Pre-commit lint hook (opt-in)                   |
| `src/kernel/test/test_runner.c` | Unit test framework runner                      |
| `src/kernel/test/test_*.c`      | 5 test files, 12 suites                         |

---

## Gotchas

> [!CAUTION]
> **`llvm-objcopy` cannot produce EFI binaries.** The UEFI bootloader pipeline must use GNU `objcopy` for the `--target efi-app-x86_64` conversion. Do not replace with `llvm-objcopy-19`.

> [!CAUTION]
> **clangd speaks LSP, not MCP.** Adding clangd as an MCP server in Antigravity causes infinite "refreshing" hang. Use clangd via editor LSP extensions only.

> [!WARNING]
> **`command_status` gets stuck on builds.** The tool falsely reports `RUNNING` after builds finish. Use sentinel-based checking via `tail -1 build/build.log` instead. See [safety.md](../../.agents/rules/safety.md) for the full workaround.

> [!NOTE]
> **Generated header race condition.** On fresh clean builds with `-j12+`, generated headers (`os_logo.h`, `bsod_icon.h`, `boot_splash_font_data.h`) must use order-only prerequisites (`| $(GENERATED_HDRS)`) to avoid compilation races.

---

## OS Comparison

| Feature                        | 🪟 Windows 11 (WDK/VS)       | 🐧 Linux Kernel                | 🚀 Impossible OS                                |
| ------------------------------ | --------------------------- | ----------------------------- | ---------------------------------------------- |
| Build system                   | ✅ MSBuild / WDK             | ✅ Kbuild (make)               | ✅ Make + build.sh wrapper                      |
| Incremental builds             | ✅ MSBuild deps              | ✅ `.d` dependency files       | ✅ `-MMD -MP` + `.d` includes                   |
| Parallel compilation           | ✅ `/MP` flag                | ✅ `make -j$(nproc)`           | ✅ `-j$(nproc)` default + `--jobs=N`            |
| Build version metadata         | ✅ Resource files (.rc)      | ✅ `uname -r` + git describe   | ✅ `include/build_info.h` (auto-generated)      |
| Compiler toolchain             | ✅ MSVC (WDK)                | ✅ GCC (Kbuild)                | ✅ Clang-19/LLD-19 (`--target=x86_64-elf`)      |
| One-command dev setup          | ❌ Manual VS + WDK install   | ⚠️ `make defconfig && make`   | ✅ `bash scripts/setup.sh` — **beats both**     |
| Automated smoke test           | ✅ HCK/HLK test framework    | ✅ kselftest + CI bots         | ✅ `scripts/test-smoke.sh` (headless QEMU)      |
| Unit test framework (kernel)   | ✅ WDK test framework        | ✅ KUnit                       | ✅ `test.h` + `test_runner.c` (12 suites)       |
| CI/CD build on push            | ✅ Azure DevOps              | ✅ GitHub Actions + kernel.org | ✅ [GitHub Actions](github-setup.md#build--smoke-test-buildyml) |
| Symbol map + debug symbols     | ✅ PDB files                 | ✅ vmlinux + kallsyms          | ✅ `kernel.sym` + `symtab_resolve()` (O(log n)) |
| Code size tracking             | ⚠️ Manual / third-party     | ✅ `bloat-o-meter`             | ✅ `scripts/size-report.sh` + CSV history       |
| Pre-commit linting             | ⚠️ Optional VS extensions   | ✅ checkpatch.pl               | ✅ `.githooks/pre-commit` (opt-in, staged only) |
| Language server (code intel)   | ✅ IntelliSense (MSVC)       | ✅ clangd + compile_commands   | ✅ clangd-19 + Bear (101 entries)               |
| Asset pipeline                 | ✅ MSBuild resource compiler | ⚠️ Manual `make` targets      | ✅ `make assets` (7 sub-targets + stamps)       |
| Asset validation               | ❌ Runtime discovery         | ❌ No built-in                 | ✅ 37 assets validated at build time            |
| **Zero-install build wrapper** | ❌ Requires VS + WDK         | ❌ Requires toolchain install  | ✅ **build.sh — single script, no IDE**         |
| **QEMU auto-test loop**        | ❌ Manual VM setup           | ✅ virtme + kselftest          | ✅ **build.sh run — build + boot + verify**     |

---

## References

- Source: `scripts/`, `tools/`, `src/kernel/test/`, `include/kernel/test/`
- Makefile: `Makefile` (root)
- Build info: `include/build_info.h` (auto-generated)
- Spec: [UEFI 2.10](../specs/hardware/firmware/uefi-2.10.md)
