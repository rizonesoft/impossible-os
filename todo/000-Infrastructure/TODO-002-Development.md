# Development Tooling & Automation

> **Goal:** Provide a comprehensive, production-grade development environment with
> build tooling, test automation, emulator scripts, hardware deployment scripts,
> dependency management, and CI/CD integration — enabling fast, reliable, and
> reproducible development cycles.

> [!CAUTION]
> **Build Constraints:** Always compile with `-Wall -Wextra -Werror -ffreestanding -nostdlib -nostdinc`.
> Use `x86_64-elf-gcc` when available; fall back to system GCC with freestanding flags.
> **ALWAYS** use `bash scripts/build.sh` — never raw `make` commands.

---

## 1. Build System

### 1.1 Build Script Enhancements ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/build.sh` supports `clean`, `run`, `clean run` modes, produces `build/os-build.iso`, writes build logs to `build/build.log` with `=== BUILD OK ===` sentinel, and displays a live progress bar during compilation. Run `bash scripts/build.sh clean` and verify output. Fix any inconsistencies in the TODO items below.

- [x] `bash scripts/build.sh` — incremental build
- [x] `bash scripts/build.sh clean` — clean build
- [x] `bash scripts/build.sh run` — build + QEMU test
- [x] `bash scripts/build.sh clean run` — clean build + QEMU test
- [x] Build log with `=== BUILD OK ===` / `=== BUILD FAILED ===` sentinel
- [x] Live progress bar during compilation
- [x] Auto-create `build/` directory if missing

### 1.2 Incremental Build Optimization

**Prompt:** The current build recompiles every source file on each `make all`. Optimize for incremental builds by ensuring proper `.o` → `.c` / `.h` dependency tracking. Generate `.d` dependency files (`-MMD -MP` flags) and include them in the Makefile. A single-file change should only recompile the changed file + relink. Measure build times before and after to confirm improvement. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"build: incremental dependency tracking"`. Add notes directly in this TODO section.

- [ ] Add `-MMD -MP` to `CFLAGS` in Makefile — generate `.d` dependency files
- [ ] Add `-include $(wildcard $(BUILD_DIR)/*.d)` to Makefile
- [ ] Verify: change one `.c` file → only that `.o` is rebuilt + relink
- [ ] Verify: change one `.h` file → all `.c` files including it are rebuilt
- [ ] Measure: full build time vs incremental build time (log both)
- [ ] Commit: `"build: incremental dependency tracking"`

### 1.3 Parallel Build Support

**Prompt:** Enable parallel compilation via `make -j$(nproc)` in `build.sh`. The Makefile must have correct dependency declarations so that targets can build in parallel without race conditions. Test with `make -j8` and verify the output is deterministic and identical to single-threaded builds. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"build: parallel compilation support"`. Add notes directly in this TODO section.

- [ ] Ensure all Makefile dependency chains are correct (no missing prerequisites)
- [ ] Update `build.sh` to pass `-j$(nproc)` to `make`
- [ ] Add `--jobs` flag to `build.sh` (e.g., `bash scripts/build.sh --jobs=4`)
- [ ] Test: `bash scripts/build.sh clean` with `-j1` and `-j8` → identical ISO output
- [ ] Measure speedup (log single vs parallel build times)
- [ ] Commit: `"build: parallel compilation support"`

### 1.4 Build Version & Metadata

**Prompt:** Auto-increment the build number on each build and embed version metadata into the kernel binary. The build number, git commit hash (short), branch name, and build timestamp are written to `include/build_info.h` as `#define` constants before compilation. The BSOD screen, boot log, `ver` shell command, and Settings → About panel all display this information. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"build: auto-increment build number + version metadata"`. Add notes directly in this TODO section.

- [ ] Auto-increment build counter in `build/build_number.txt` on each build
- [ ] Generate `include/build_info.h` with:
  - [ ] `BUILD_NUMBER` — auto-incremented integer
  - [ ] `BUILD_COMMIT` — `git rev-parse --short HEAD`
  - [ ] `BUILD_BRANCH` — `git rev-parse --abbrev-ref HEAD`
  - [ ] `BUILD_TIMESTAMP` — ISO 8601 UTC (`date -u +%Y-%m-%dT%H:%M:%SZ`)
  - [ ] `BUILD_VERSION` — from `VERSION` file (e.g., `"0.1.0"`)
- [ ] `ver` shell command displays: `Impossible OS v0.1.0 (build 547, main@a1b2c3d, 2026-03-17T12:00:00Z)`
- [ ] BSOD screen includes build version in crash dump footer
- [ ] Boot log first line: `Impossible OS v0.1.0 build 547`
- [ ] Commit: `"build: auto-increment build number + version metadata"`

---

## 2. Cross-Compiler & Dependency Management

### 2.1 Cross-Compiler Bootstrap Script

**Prompt:** Create `scripts/setup-toolchain.sh` that downloads and builds the `x86_64-elf-gcc` cross-compiler from source (binutils + GCC) into `tools/cross/`. This ensures every developer and CI runner uses an identical compiler version, eliminating "works on my machine" bugs. The script checks if the cross-compiler already exists before rebuilding. Target: GCC 15.2+ (latest stable as of 2026), binutils 2.46+. Build with `--target=x86_64-elf --disable-nls --without-headers`. After completing all items, mark every item as `[x]`, run the script to verify, and commit as `"tools: cross-compiler bootstrap script"`. Add notes directly in this TODO section.

- [ ] Create `scripts/setup-toolchain.sh`
- [ ] Download binutils 2.46+ and GCC 15.2+ source tarballs (latest stable as of 2026)
- [ ] Build binutils: `--target=x86_64-elf --prefix=$(pwd)/tools/cross`
- [ ] Build GCC 15.2: `--target=x86_64-elf --disable-nls --without-headers --enable-languages=c`
- [ ] Install to `tools/cross/bin/x86_64-elf-gcc`
- [ ] Skip rebuild if `tools/cross/bin/x86_64-elf-gcc` already exists and matches version
- [ ] Add `tools/cross/` to `.gitignore`
- [ ] Update `Makefile` to check `tools/cross/bin/` first, then system `x86_64-elf-gcc`, then fallback
- [ ] Commit: `"tools: cross-compiler bootstrap script"`

### 2.2 System Dependency Installer

**Prompt:** Create `scripts/setup-deps.sh` that installs all required system packages for building Impossible OS. Detect the Linux distribution (Ubuntu/Debian via `apt`, Fedora via `dnf`, Arch via `pacman`) and install the appropriate packages. Required packages: `nasm` (3.01+), `xorriso`, `mtools`, `qemu-system-x86` (10.2+), `ovmf`, `python3`, `python3-pil` (for asset generation), `dosfstools` (for FAT32 image creation), `parted` (for disk images). The script is idempotent — running it twice changes nothing. After completing all items, mark every item as `[x]`, and commit as `"tools: system dependency installer"`. Add notes directly in this TODO section.

- [ ] Create `scripts/setup-deps.sh`
- [ ] Detect distro: Ubuntu/Debian (`apt`), Fedora (`dnf`), Arch (`pacman`)
- [ ] Install: `nasm` (3.01+), `xorriso`, `mtools`, `qemu-system-x86` (10.2+), `ovmf`
- [ ] Install: `python3`, `python3-pil`, `dosfstools`, `parted`, `cppcheck` (2.20+)
- [ ] Idempotent: check if each package is already installed before installing
- [ ] Print summary: "All dependencies installed" or list missing packages
- [ ] Commit: `"tools: system dependency installer"`

### 2.3 One-Command Setup

**Prompt:** Create a top-level `scripts/setup.sh` that runs `setup-deps.sh` (§2.2) then `setup-toolchain.sh` (§2.1) in sequence. A new developer should be able to clone the repo, run `bash scripts/setup.sh`, and immediately build with `bash scripts/build.sh`. Add a "Getting Started" section to `README.md`. After completing all items, mark every item as `[x]`, and commit as `"tools: one-command dev environment setup"`. Add notes directly in this TODO section.

- [ ] Create `scripts/setup.sh` — runs `setup-deps.sh` + `setup-toolchain.sh`
- [ ] Update `README.md` with "Getting Started" section:
  ```
  git clone https://github.com/rizonesoft/impossible-os.git
  cd impossible-os
  bash scripts/setup.sh
  bash scripts/build.sh run
  ```
- [ ] Test on fresh Ubuntu 22.04 WSL instance (clean slate)
- [ ] Commit: `"tools: one-command dev environment setup"`

---

## 3. Emulator Testing Scripts

### 3.1 QEMU Test Runner ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/run-qemu.sh` launches QEMU with the correct flags (UEFI firmware, AHCI disk, serial stdio, VGA resolution). Also confirm `scripts/build.sh run` wraps this correctly. Fix any inconsistencies in the TODO items below.

- [x] `scripts/run-qemu.sh` — launch QEMU with UEFI + AHCI + serial
- [x] `bash scripts/build.sh run` — build + auto-launch QEMU
- [x] QEMU flags: `-bios OVMF.fd`, AHCI disk, `-serial stdio`, VGA resolution
- [x] OVMF firmware auto-copy to `build/`

### 3.2 VirtualBox Test Runner

**Prompt:** `scripts/run-vbox.ps1` exists but only works on Windows. Create a cross-platform `scripts/run-vbox.sh` for Linux/WSL that registers a VirtualBox VM with the correct settings (EFI boot, AHCI controller, 512 MB RAM, VGA adapter), attaches the ISO, and starts the VM. The script creates the VM if it doesn't exist and updates it if settings have changed. After completing all items, mark every item as `[x]`, run the script to verify, and commit as `"tools: cross-platform VirtualBox runner"`. Add notes directly in this TODO section.

- [ ] Create `scripts/run-vbox.sh`
- [ ] Check if VBoxManage is available
- [ ] Create/update VM: `ImpossibleOS-Dev` with EFI, AHCI, 512 MB RAM
- [ ] Attach `build/os-build.iso` as DVD
- [ ] Start VM in headless mode or with GUI
- [ ] Add `--headless` flag for CI use
- [ ] Commit: `"tools: cross-platform VirtualBox runner"`

### 3.3 Hyper-V Test Runner

**Prompt:** Create `scripts/run-hyperv.ps1` that creates or updates a Hyper-V Generation 2 VM with Secure Boot disabled, 512 MB RAM, and the ISO attached. Hyper-V is the primary target for production testing on Windows hosts. The script must handle: VM doesn't exist (create), VM exists but is running (stop first), and VM exists but settings changed (update). After completing all items, mark every item as `[x]`, and commit as `"tools: Hyper-V Gen 2 test runner"`. Add notes directly in this TODO section.

> **XREF:** Hyper-V Gen 2 architectural requirements are documented in
> `TODO-080-Drivers.md §1.6` and `TODO-010-Bootloader.md §7`.

- [ ] Create `scripts/run-hyperv.ps1`
- [ ] Create Hyper-V Generation 2 VM: `ImpossibleOS-Dev`
- [ ] Settings: 512 MB RAM, Secure Boot OFF, 1 vCPU, DVD drive
- [ ] Attach `build/os-build.iso` to virtual DVD
- [ ] Handle existing VM: stop if running, update settings if changed
- [ ] Start VM and connect to console
- [ ] Commit: `"tools: Hyper-V Gen 2 test runner"`

### 3.4 Multi-Resolution QEMU Launcher

**Prompt:** The current `run-windows-*.bat` files launch QEMU at specific resolutions (1080p, 1440p, 4K). Consolidate these into a single `scripts/run-qemu.sh --resolution=1080p|1440p|4k` flag. Default to 720p (current QEMU default). After completing all items, mark every item as `[x]`, and commit as `"tools: multi-resolution QEMU launcher"`. Add notes directly in this TODO section.

- [ ] Add `--resolution` flag to `scripts/run-qemu.sh` (720p, 1080p, 1440p, 4k)
- [ ] Map resolution to QEMU OVMF framebuffer size
- [ ] Default: 720p (1280×720) — current behavior
- [ ] Remove `run-windows-1080p.bat`, `run-windows-1440p.bat`, `run-windows-4k.bat` (consolidated)
- [ ] Commit: `"tools: multi-resolution QEMU launcher"`

---

## 4. Hardware Deployment Scripts

### 4.1 USB Write Script ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/write-usb.ps1` creates a GPT-partitioned USB drive with an EFI System Partition (FAT32), a system partition (FAT32), and a logs partition (FAT32). Run on a test USB drive and verify all partitions are created correctly.

- [x] `scripts/write-usb.ps1` — write ISO to USB drive (Windows/PowerShell)
- [x] `scripts/write-usb.bat` — wrapper for PowerShell script
- [x] GPT partition layout: EFI (FAT32) + System (FAT32) + Logs (FAT32)
- [x] Auto-detect USB drive letter
- [x] Safety prompts before write

### 4.2 USB Write Script (Linux)

**Prompt:** Create `scripts/write-usb.sh` for Linux/WSL that performs the same USB write operation as `write-usb.ps1`. Uses `parted` for partition table creation and `mkfs.fat` for FAT32 formatting. The script must: detect removable USB drives with `lsblk`, show them to the user, require explicit confirmation before writing, create GPT partition layout matching the Windows script. After completing all items, mark every item as `[x]`, and commit as `"tools: Linux USB write script"`. Add notes directly in this TODO section.

> [!CAUTION]
> **Safety:** This script writes to raw block devices. Triple-check the target device.
> Never auto-detect and write without user confirmation.

- [ ] Create `scripts/write-usb.sh`
- [ ] List removable USB devices: `lsblk --json --output NAME,SIZE,TRAN,RM`
- [ ] User selects device (e.g., `/dev/sdb`)
- [ ] Confirmation prompt: "This will ERASE /dev/sdb (32GB SanDisk). Type YES to continue."
- [ ] Create GPT partition table: `parted /dev/sdb mklabel gpt`
- [ ] Create partitions: EFI (256 MB FAT32), System (256 MB FAT32), Logs (rest FAT32)
- [ ] Copy bootloader, kernel, and assets to partitions
- [ ] Print boot instructions at end
- [ ] Commit: `"tools: Linux USB write script"`

### 4.3 USB Log Reader

**Prompt:** Create `scripts/read-usb-log.sh` that reads logs from the USB drive's logs partition (X: or the third partition on Linux). After booting on real hardware, the logs partition contains `debug.log` and hardware information. The script mounts the partition (read-only), copies the log files to `build/logs/<timestamp>/`, and displays a summary. This replaces the deleted `read-usb-log.bat` / `read-usb-log.ps1`. After completing all items, mark every item as `[x]`, and commit as `"tools: USB log reader"`. Add notes directly in this TODO section.

- [ ] Create `scripts/read-usb-log.sh`
- [ ] Mount USB logs partition (read-only)
- [ ] Copy `debug.log` and all `.log` files to `build/logs/<date>/`
- [ ] Print summary: file sizes, first/last timestamp, any panic indicators
- [ ] Linux: auto-detect third partition of USB device
- [ ] Commit: `"tools: USB log reader"`

---

## 5. Test Framework

### 5.1 Kernel Unit Test Framework

**Prompt:** Create a minimal kernel-mode unit test framework for testing core subsystems (PMM, heap, VFS, scheduler) without booting the full desktop. `test_assert(condition, msg)` checks a condition and logs pass/fail to serial. A `test_runner()` function runs all registered test suites and prints a summary: "42 tests passed, 0 failed." Tests run during boot when `--run-tests` is passed via `boot.conf` or compile-time `#ifdef KERNEL_TESTS`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"test: kernel unit test framework"`. Add notes directly in this TODO section.

- [ ] Create `src/kernel/test/test.h` — `test_assert(cond, msg)`, `test_suite_register(name, fn)`
- [ ] Create `src/kernel/test/test_runner.c` — run all registered suites, print summary
- [ ] `test_assert()` logs to serial: `[PASS] msg` or `[FAIL] msg (file:line)`
- [ ] Summary output: `"42 passed, 0 failed"` in serial and boot log
- [ ] Conditional compilation: `#ifdef KERNEL_TESTS` or `boot.conf` flag `run_tests=1`
- [ ] Commit: `"test: kernel unit test framework"`

### 5.2 Core Subsystem Tests

**Prompt:** Write unit tests for the core kernel subsystems using the framework from §5.1. Test PMM allocation/free cycles, heap allocation/free + overflow detection, VFS open/read/write/close, and scheduler task creation. Each test must be self-contained — allocate, test, and clean up. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"test: core subsystem unit tests"`. Add notes directly in this TODO section.

- [ ] PMM tests: alloc + free → frame reuse; alloc until OOM → returns NULL
- [ ] Heap tests: kmalloc + kfree round-trip; zero-byte alloc returns NULL; double-free detection
- [ ] VFS tests: open/write/read/close file; create/delete directory; path resolution
- [ ] Scheduler tests: create task → verify it runs; yield → switches tasks; kill → cleanup
- [ ] Registry tests: set/get string, DWORD, binary; delete key; flush to disk
- [ ] Commit: `"test: core subsystem unit tests"`

### 5.3 Automated QEMU Smoke Test

**Prompt:** Create `scripts/test-smoke.sh` that builds the OS, boots in QEMU headless mode, captures serial output for 30 seconds, and checks for expected boot messages and absence of panics. The test passes if the boot log contains `"Desktop ready"` and does not contain `"KERNEL PANIC"`. Exit with code 0 on pass, 1 on failure. This enables CI/CD automated testing. After completing all items, mark every item as `[x]`, and commit as `"test: automated QEMU smoke test"`. Add notes directly in this TODO section.

- [ ] Create `scripts/test-smoke.sh`
- [ ] Build: `bash scripts/build.sh clean`
- [ ] Launch QEMU headless: `-nographic -serial stdio -display none`
- [ ] Capture serial output for 30 seconds (timeout)
- [ ] Pass criteria: output contains `"Desktop ready"` or `"=== BOOT COMPLETE ==="`
- [ ] Fail criteria: output contains `"KERNEL PANIC"` or `"ASSERT FAILED"`
- [ ] Exit code: 0 = pass, 1 = fail
- [ ] Print: `"SMOKE TEST PASSED"` or `"SMOKE TEST FAILED: <reason>"`
- [ ] Commit: `"test: automated QEMU smoke test"`

### 5.4 FAT32 Filesystem Test Suite

**Prompt:** Create `scripts/test-fs.sh` that generates test disk images with various filesystems (FAT32, exFAT, NTFS, ext2/3/4, ISO 9660, UDF) using Linux tools, mounts them in QEMU, and verifies the kernel can read them correctly. Tests are data-driven: a test descriptor file lists the filesystem type, expected file names, and expected file contents. After completing all items, mark every item as `[x]`, and commit as `"test: filesystem test suite"`. Add notes directly in this TODO section.

> **XREF:** See `/test-fs-fat32` workflow for the FAT32-specific test patterns.

- [ ] Create `scripts/test-fs.sh`
- [ ] Generate test disk images: FAT32 (various cluster sizes), exFAT, NTFS
- [ ] Each image contains known test files (filenames, sizes, contents)
- [ ] Boot QEMU with test disk attached as second drive
- [ ] Kernel reads test files → outputs checksums to serial
- [ ] Script compares serial output checksums against expected values
- [ ] Report: per-filesystem pass/fail summary
- [ ] Commit: `"test: filesystem test suite"`

---

## 6. Development Utilities

### 6.1 Symbol Map Generator

**Prompt:** Generate a kernel symbol map (`build/kernel.map`) during the build process. This maps function names to addresses, enabling the stack trace in BSOD/panic to show function names instead of raw hex addresses. `nm -n build/kernel.bin > build/kernel.map`. The panic handler uses this map to resolve addresses during stack trace printing. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"build: symbol map generation"`. Add notes directly in this TODO section.

- [ ] Add `nm -n $(KERNEL_BIN) > build/kernel.map` to Makefile after kernel link
- [ ] Embed symbol map as raw data in the kernel (or load from initrd at boot)
- [ ] `panic.c`: resolve RBP-chain addresses to function names via binary search in map
- [ ] Boot log: `[OK] Symbol map loaded: 1,247 symbols`
- [ ] Stack trace output: `  0xFFFF800000012345  kernel_main+0x42  (main.c)`
- [ ] Commit: `"build: symbol map generation"`

### 6.2 Code Size & Bloat Tracking

**Prompt:** Create `scripts/size-report.sh` that reports the size of the kernel binary, each object file, and the total ISO size. Track these in `build/size-history.csv` (appended on each build) so that size regressions can be detected. Display a comparison: `"kernel.bin: 245,760 bytes (+1,024 from last build)"`. Warn if kernel exceeds 512 KB or ISO exceeds 32 MB. After completing all items, mark every item as `[x]`, and commit as `"tools: code size tracking"`. Add notes directly in this TODO section.

- [ ] Create `scripts/size-report.sh`
- [ ] Report: kernel.bin size, each `.o` file size (top 10 largest), total ISO size
- [ ] Append to `build/size-history.csv` (date, commit, kernel size, ISO size)
- [ ] Compare against previous build: show delta (+/- bytes)
- [ ] Warn threshold: kernel > 512 KB, ISO > 32 MB
- [ ] Commit: `"tools: code size tracking"`

### 6.3 Code Style Linter

**Prompt:** Create `scripts/lint.sh` that checks all C source files against the project's coding standards: snake_case for functions/variables, UPPER_CASE for macros, `#pragma once` or include guards, lines ≤ 120 characters, no trailing whitespace, functions ≤ 50 lines. Use a combination of `grep`, `awk`, and/or `cppcheck` 2.20+ (static analysis for memory leaks, buffer overflows, undefined behavior). Report violations with file:line and a description. Exit with code 0 if clean, 1 if violations found. After completing all items, mark every item as `[x]`, and commit as `"tools: code style linter"`. Add notes directly in this TODO section.

- [ ] Create `scripts/lint.sh`
- [ ] Check: snake_case for function definitions
- [ ] Check: UPPER_CASE for `#define` macros
- [ ] Check: `#pragma once` or include guard in every `.h` file
- [ ] Check: lines ≤ 120 characters
- [ ] Check: no trailing whitespace
- [ ] Check: functions ≤ 50 lines (warn, not error)
- [ ] Report: `file:line: violation description`
- [ ] Exit 0 (clean) or 1 (violations found)
- [ ] Commit: `"tools: code style linter"`

### 6.4 Debug Script Enhancement

**Prompt:** `scripts/debug.sh` provides GDB debugging support. Enhance it to connect to QEMU's GDB stub (`-s -S` flags), load the kernel symbol map, and set common breakpoints (kernel_main, panic, page_fault_handler). Add `--breakpoint=<function>` flag for custom breakpoints. After completing all items, mark every item as `[x]`, and commit as `"tools: enhanced GDB debug script"`. Add notes directly in this TODO section.

- [ ] Update `scripts/debug.sh` to launch QEMU with `-s -S` (GDB stub, wait for connection)
- [ ] Auto-start GDB with: `target remote :1234`, `symbol-file build/kernel.bin`
- [ ] Load symbol map: `add-symbol-file build/kernel.bin 0xFFFF800000000000`
- [ ] Default breakpoints: `kernel_main`, `panic`, `page_fault_handler`
- [ ] `--breakpoint=<function>` — add custom breakpoint
- [ ] Print: "GDB connected. Type 'c' to continue."
- [ ] Commit: `"tools: enhanced GDB debug script"`

---

## 7. Asset Pipeline

### 7.1 Asset Build Script

**Prompt:** Create a unified asset pipeline that converts all source assets (PNG icons, JPG wallpapers, TTF fonts, BMP cursors) into kernel-embeddable formats during `make all`. PNG icons → BGRA C arrays. Wallpaper JPGs → converted at runtime (already handled). TTF fonts → copied to sysroot. Cursors → BGRA C arrays. This consolidates the scattered asset conversion steps into one `make assets` target. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"build: unified asset pipeline"`. Add notes directly in this TODO section.

> **XREF:** See `/add-asset` workflow for the full checklist when adding a new asset.

- [ ] Create `make assets` target in Makefile
- [ ] PNG icons → BGRA C array headers (via `tools/png2header.py`)
- [ ] BMP cursors → BGRA C array headers
- [ ] TTF fonts → copy to `sysroot/Impossible/Fonts/`
- [ ] Wallpaper → copy to `sysroot/Impossible/Wallpapers/`
- [ ] Dependency tracking: rebuild only changed assets
- [ ] Commit: `"build: unified asset pipeline"`

### 7.2 Asset Validation

**Prompt:** Validate all embedded assets at build time: check PNG dimensions and color depth match expected values, verify TTF files parse correctly, confirm cursor hotspot coordinates are within bounds. Fail the build early with a clear error message if an asset is malformed rather than discovering it at runtime. After completing all items, mark every item as `[x]`, and commit as `"build: asset validation"`. Add notes directly in this TODO section.

- [ ] Validate PNG icons: expected dimensions, RGBA format, file size < 1 MB
- [ ] Validate TTF fonts: parseable, glyph count > 0
- [ ] Validate cursors: dimensions match expected, hotspot within bounds
- [ ] Validate wallpapers: JPEG decodeable, reasonable dimensions (< 8K)
- [ ] Print: `[OK] 12 assets validated` or `[FAIL] cursor.bmp: hotspot (99,99) out of bounds (32x32)`
- [ ] Commit: `"build: asset validation"`

---

## 8. CI/CD Integration

### 8.1 GitHub Actions Build Workflow

**Prompt:** Create `.github/workflows/build.yml` that runs on every push to `main` and on pull requests. Steps: install dependencies (§2.2), build (§1), run smoke test (§5.3), and report size (§6.2). The workflow must complete in under 10 minutes. Cache the cross-compiler and dependencies between runs for speed. After completing all items, mark every item as `[x]`, and commit as `"ci: build and smoke test on push"`. Add notes directly in this TODO section.

> **XREF:** See `TODO-001-GitHub.md §4` for release CI (separate from build CI).

- [ ] Create `.github/workflows/build.yml`
- [ ] Trigger: push to `main` + pull requests
- [ ] Steps:
  - [ ] Cache cross-compiler (`tools/cross/`) between runs
  - [ ] Install deps: `bash scripts/setup-deps.sh`
  - [ ] Build: `bash scripts/build.sh clean`
  - [ ] Smoke test: `bash scripts/test-smoke.sh`
  - [ ] Size report: `bash scripts/size-report.sh`
- [ ] Upload ISO as build artifact (downloadable from Actions tab)
- [ ] Status badge in `README.md`: [![Build](badge-url)](workflow-url)
- [ ] Commit: `"ci: build and smoke test on push"`

### 8.2 Pre-Commit Hooks

**Prompt:** Create `.githooks/pre-commit` that runs the code linter (§6.3) and verifies the build succeeds before allowing a commit. Developers opt-in by running `git config core.hooksPath .githooks`. The hook should be fast (< 5 seconds) — only lint changed files, not the entire codebase. After completing all items, mark every item as `[x]`, and commit as `"tools: pre-commit lint hook"`. Add notes directly in this TODO section.

- [ ] Create `.githooks/pre-commit`
- [ ] Run `scripts/lint.sh` on staged `.c` and `.h` files only
- [ ] Block commit if lint violations found (exit 1)
- [ ] Print: "Pre-commit lint passed" or "Fix lint errors before committing"
- [ ] Add setup instruction to README: `git config core.hooksPath .githooks`
- [ ] Commit: `"tools: pre-commit lint hook"`

---

## Priority Order

| Priority | Section                           | Reason                                                   |
|----------|-----------------------------------|----------------------------------------------------------|
| ✅ Done   | 1.1 Build Script                  | Core build — implemented and working                     |
| ✅ Done   | 3.1 QEMU Runner                   | Primary test environment                                 |
| ✅ Done   | 4.1 USB Write (Windows)           | Hardware deployment — working                            |
| 🔴 P0     | 1.4 Build Version & Metadata      | `ver` command, BSOD footer, boot log need version info   |
| 🔴 P0     | 5.3 QEMU Smoke Test               | Catches boot regressions automatically                   |
| 🟠 P1     | 1.2 Incremental Build             | Dev iteration speed — avoid full recompiles              |
| 🟠 P1     | 2.2 System Dependency Installer   | Onboarding — new devs need one-command setup             |
| 🟠 P1     | 6.1 Symbol Map                    | BSOD stack traces need function names                    |
| 🟡 P2     | 1.3 Parallel Build                | Build speed — 4x faster on multi-core                    |
| 🟡 P2     | 2.1 Cross-Compiler Bootstrap      | Reproducible builds across machines                      |
| 🟡 P2     | 2.3 One-Command Setup             | After §2.1 + §2.2                                       |
| 🟡 P2     | 5.1 Unit Test Framework           | Foundation for systematic testing                        |
| 🟡 P2     | 5.4 Filesystem Test Suite         | Validates FS drivers against real disk images            |
| 🟡 P2     | 7.1 Asset Pipeline                | Consolidates scattered asset build steps                 |
| 🟢 P3     | 3.2 VirtualBox Runner             | Secondary test environment                               |
| 🟢 P3     | 3.3 Hyper-V Runner                | Production target testing                                |
| 🟢 P3     | 3.4 Multi-Resolution Launcher     | Consolidates resolution-specific scripts                 |
| 🟢 P3     | 4.2 USB Write (Linux)             | Cross-platform hardware deployment                       |
| 🟢 P3     | 4.3 USB Log Reader                | Hardware debugging workflow                              |
| 🟢 P3     | 5.2 Core Subsystem Tests          | After §5.1 framework                                    |
| 🟢 P3     | 6.2 Size Tracking                 | Detect bloat early                                       |
| 🟢 P3     | 6.3 Code Linter                   | Style consistency                                        |
| 🟢 P3     | 6.4 GDB Debug Enhancement         | Developer productivity                                   |
| 🟢 P3     | 7.2 Asset Validation              | Catch malformed assets at build time                     |
| 🟢 P3     | 8.1 CI Build Workflow             | After §5.3 smoke test exists                             |
| 🔵 P4     | 8.2 Pre-Commit Hooks              | After §6.3 linter exists                                 |

---

## Key Files

| File                                | Purpose                                  |
|-------------------------------------|------------------------------------------|
| `scripts/build.sh`                  | [EXISTS] Core build script               |
| `scripts/run-qemu.sh`              | [EXISTS] QEMU launcher                   |
| `scripts/debug.sh`                  | [EXISTS] GDB debug launcher              |
| `scripts/write-usb.ps1`             | [EXISTS] USB write (Windows)             |
| `scripts/setup-toolchain.sh`        | [NEW] Cross-compiler bootstrap           |
| `scripts/setup-deps.sh`             | [NEW] System dependency installer        |
| `scripts/setup.sh`                  | [NEW] One-command setup                  |
| `scripts/run-vbox.sh`               | [NEW] VirtualBox launcher (Linux)        |
| `scripts/run-hyperv.ps1`            | [NEW] Hyper-V Gen 2 launcher             |
| `scripts/write-usb.sh`              | [NEW] USB write (Linux)                  |
| `scripts/read-usb-log.sh`           | [NEW] USB log reader                     |
| `scripts/test-smoke.sh`             | [NEW] Automated smoke test               |
| `scripts/test-fs.sh`                | [NEW] Filesystem test suite              |
| `scripts/size-report.sh`            | [NEW] Code size tracker                  |
| `scripts/lint.sh`                   | [NEW] Code style linter                  |
| `.github/workflows/build.yml`       | [NEW] CI build + test                    |
| `.githooks/pre-commit`              | [NEW] Pre-commit lint hook               |
| `include/build_info.h`              | [NEW] Auto-generated build metadata      |

---

## OS Comparison

| Feature                           | Windows 11 (WDK/VS)              | Linux Kernel                      | Impossible OS                             |
|-----------------------------------|----------------------------------|-----------------------------------|-------------------------------------------|
| Build system                      | ✅ MSBuild / WDK                  | ✅ Kbuild (make)                 | ✅ Make + build.sh wrapper                |
| Incremental builds                | ✅ MSBuild deps                   | ✅ `.d` dependency files          | ⬜ §1.2 P1                                |
| Parallel compilation              | ✅ `/MP` flag                     | ✅ `make -j$(nproc)`             | ⬜ §1.3 P2                                |
| Build version metadata            | ✅ Resource files (.rc)           | ✅ `uname -r` + git describe     | ⬜ §1.4 P0                                |
| Cross-compiler setup              | ✅ WDK installer                  | ✅ `make.cross` script            | ⬜ §2.1 P2                                |
| One-command dev setup             | ❌ Manual VS + WDK install        | ⚠️ `make defconfig && make`      | ⬜ §2.3 P2 — **beats Windows**            |
| Automated smoke test              | ✅ HCK/HLK test framework         | ✅ kselftest + CI bots            | ⬜ §5.3 P0                                |
| Unit test framework (kernel)      | ✅ WDK test framework              | ✅ KUnit                          | ⬜ §5.1 P2                                |
| CI/CD build on push               | ✅ Azure DevOps                    | ✅ GitHub Actions + kernel.org    | ⬜ §8.1 P3                                |
| Symbol map + debug symbols        | ✅ PDB files                       | ✅ vmlinux + kallsyms             | ⬜ §6.1 P1                                |
| Code size tracking                | ⚠️ Manual / third-party          | ✅ `bloat-o-meter`               | ⬜ §6.2 P3                                |
| Pre-commit linting                | ⚠️ Optional VS extensions         | ✅ checkpatch.pl                  | ⬜ §8.2 P4                                |
| **Zero-install build wrapper**    | ❌ Requires VS + WDK              | ❌ Requires toolchain install     | ✅ **build.sh — single script, no IDE**   |
| **QEMU auto-test loop**           | ❌ Manual VM setup                 | ✅ virtme + kselftest             | ⬜ **§5.3 — build + boot + verify**       |
