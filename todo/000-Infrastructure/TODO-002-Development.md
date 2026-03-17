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

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/build.sh` supports `clean`, `run`, `clean run` modes, produces `build/system-disk.img`, writes build logs to `build/build.log` with `=== BUILD OK ===` sentinel, and displays a live progress bar during compilation. Run `bash scripts/build.sh clean` and verify output. Fix any inconsistencies in the TODO items below.

- [x] `bash scripts/build.sh` — incremental build
- [x] `bash scripts/build.sh clean` — clean build
- [x] `bash scripts/build.sh run` — build + QEMU test
- [x] `bash scripts/build.sh clean run` — clean build + QEMU test
- [x] Build log with `=== BUILD OK ===` / `=== BUILD FAILED ===` sentinel
- [x] Live progress bar during compilation
- [x] Auto-create `build/` directory if missing

### 1.2 Incremental Build Optimization ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `-MMD -MP` flags are in `CFLAGS` and `USER_CFLAGS`, `.d` dependency files are included in the Makefile, and incremental builds only recompile changed files. Run `bash scripts/build.sh clean` and verify output. Fix any inconsistencies in the TODO items below.

- [x] Add `-MMD -MP` to `CFLAGS` in Makefile — generate `.d` dependency files
- [x] Add `-include $(DEP_FILES)` to Makefile (uses `find build/ -name '*.d'`)
- [x] Verify: change one `.c` file → only that `.o` is rebuilt + relink
- [x] Verify: change one `.h` file → all `.c` files including it are rebuilt
- [x] Measure: full build time vs incremental build time (log both)
- [x] Commit: `"build: incremental dependency tracking"`

> [!NOTE]
> **Build timing measurements (2026-03-17):**
> - Clean build: **19.0s** (101 `.d` files generated across kernel + userland)
> - Incremental (touch `main.c`): **3.3s** — only `main.c` recompiled + relink (**5.8× faster**)
> - Header change (touch `printk.h`): **12.1s** — 71/96 kernel files recompiled (correct: only dependents)
> - `-MMD -MP` added to `CFLAGS`, `USER_CFLAGS`; `SIMD_CFLAGS` inherits via `$(filter-out ..., $(CFLAGS))`
> - `.d` files live alongside `.o` files in `build/` and are cleaned by `rm -rf $(BUILD_DIR)`

### 1.3 Parallel Build Support ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `build.sh` uses `-j$(nproc)` by default, the `--jobs=N` flag works, and parallel clean builds succeed without race conditions. Run `bash scripts/build.sh clean` and verify output. Fix any inconsistencies in the TODO items below.

- [x] Ensure all Makefile dependency chains are correct (no missing prerequisites)
- [x] Update `build.sh` to pass `-j$(nproc)` to `make`
- [x] Add `--jobs` flag to `build.sh` (e.g., `bash scripts/build.sh --jobs=4`)
- [x] Test: `bash scripts/build.sh clean` with `--jobs=1` and `--jobs=12` → both succeed
- [x] Measure speedup (log single vs parallel build times)
- [x] Commit: `"build: parallel compilation support"`

> [!NOTE]
> **Parallel build measurements (2026-03-17, 12 cores):**
> - `-j1` clean build: **19.6s** (kernel 13.4s + userland 4.4s + disk 1.6s)
> - `-j12` clean build: **8.0s** (kernel 3.7s + userland 2.5s + disk 1.6s)
> - **2.5× total speedup**, **3.6× kernel compilation speedup**
> - Kernel hashes differ between runs only due to auto-incremented `VERSION_BUILD`
> - Generated headers (`os_logo.h`, `bsod_icon.h`, `boot_splash_font_data.h`) use
>   order-only prerequisites (`| $(GENERATED_HDRS)`) to prevent races on first clean build

### 1.4 Build Version & Metadata ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `include/build_info.h` is auto-generated with all required `#define` constants, `version_print()` is called at boot, BSOD screen includes version footer, and `ver` shell command shows full build metadata. Run `bash scripts/build.sh clean` and verify output. Fix any inconsistencies in the TODO items below.

- [x] Auto-increment build counter in `.build_number` on each build
- [x] Generate `include/build_info.h` with:
  - [x] `BUILD_NUMBER` — auto-incremented integer
  - [x] `BUILD_COMMIT` — `git rev-parse --short HEAD` (8-char)
  - [x] `BUILD_BRANCH` — `git rev-parse --abbrev-ref HEAD`
  - [x] `BUILD_TIMESTAMP` — ISO 8601 UTC (`date -u +%Y-%m-%dT%H:%M:%SZ`)
  - [x] `BUILD_VERSION` — CalVer from build date (`YY.M.D`, e.g., `"26.3.17"`)
- [x] `ver` shell command displays: `Impossible OS v26.3.17 (build 809, main@d1016ab, 2026-03-17T16:13:00Z)`
- [x] BSOD screen includes build version in crash dump footer
- [x] Boot log first line: `Impossible OS v26.3.17 (build 809, main@d1016ab, 2026-03-17T16:13:00Z)`
- [x] Commit: `"build: auto-increment build number + version metadata"`

> [!NOTE]
> **Build version metadata notes (2026-03-17):**
> - **CalVer versioning** (`YY.M.D`) — auto-generated from `date -u`, no static `VERSION` file
> - `include/build_info.h` is auto-generated by the Makefile's `$(BUILD_INFO_H): .FORCE` target
> - Uses `cmp -s` to avoid rewriting when content is unchanged → prevents full recompilation
> - `version.h` `#include`s `build_info.h` (replaced old `#ifndef` fallback defines)
> - `version.c` exposes `version_branch()`, `version_timestamp()`, `version_print()` APIs
> - `panic.c` shows version footer in dim color before crash dump save message
> - `shell.c` `ver`/`version` command uses `BUILD_VERSION`, `BUILD_NUMBER`, etc. (C string concatenation)
> - `include/build_info.h` is in `.gitignore` and cleaned by `make clean`

### 1.5 Scripts Directory Organization ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/` root contains only `build.sh`, `run-qemu.sh`, `debug.sh`, with subdirectories `deploy/`, `emulators/`, `secure-boot/` containing the moved scripts. Run `bash scripts/build.sh clean` and verify output. Fix any inconsistencies in the TODO items below.

> [!NOTE]
> The `run-windows-*.bat` files are candidates for deletion once `§3.4 Multi-Resolution
> QEMU Launcher` consolidates them into `scripts/run-qemu.sh --resolution=1080p`.
> Defer deletion until §3.4 is complete.

**Root `scripts/` — daily workflow (keep here):**

- [x] `build.sh` — core build script (used every session)
- [x] `run-qemu.sh` — primary QEMU test runner
- [x] `debug.sh` — GDB debug launcher

**`scripts/deploy/` — hardware deployment (occasional):**

- [x] Move `write-usb.ps1` → `scripts/deploy/write-usb.ps1`
- [x] Move `write-usb.bat` → `scripts/deploy/write-usb.bat`
- [ ] *(Future)* `write-usb.sh` goes here too (§4.2)
- [ ] *(Future)* `read-usb-log.sh` goes here too (§4.3)

**`scripts/emulators/` — secondary emulator launchers:**

- [x] Move `run-vbox.ps1` → `scripts/emulators/run-vbox.ps1`
- [x] Move `run-vbox.bat` → `scripts/emulators/run-vbox.bat`
- [x] Move `run-windows.ps1` → `scripts/emulators/run-windows.ps1`
- [x] Move `run-windows.bat` → `scripts/emulators/run-windows.bat`
- [x] Move `run-windows-1080p.bat` → `scripts/emulators/run-windows-1080p.bat`
- [x] Move `run-windows-1440p.bat` → `scripts/emulators/run-windows-1440p.bat`
- [x] Move `run-windows-4k.bat` → `scripts/emulators/run-windows-4k.bat`
- [ ] *(After §3.4)* Delete `run-windows-*.bat` (consolidated into `run-qemu.sh --resolution`)

**`scripts/secure-boot/` — one-time setup:**

- [x] Move `build-shim.sh` → `scripts/secure-boot/build-shim.sh`

**Update all references:**

- [x] Update `build.sh` if it references moved scripts — no refs found
- [x] Update `Makefile` if it references moved scripts — 3 refs updated
- [x] Update `.agents/workflows/build.md` if it references moved paths — no refs found
- [x] Update `.agents/workflows/add-asset.md` if it references moved paths — no refs found
- [x] Grep all TODO files for `scripts/write-usb`, `scripts/run-vbox`, etc. and update paths — TODO-006 (8 refs), TODO-010 (1 ref) updated
- [x] Verify: `bash scripts/build.sh clean` still works after moves
- [x] Commit: `"tools: organize scripts directory"`

**After reorganization:**

```
scripts/
├── build.sh                 ← daily (core build)
├── run-qemu.sh              ← daily (primary tester)
├── debug.sh                 ← daily (GDB debug)
├── deploy/
│   ├── write-usb.ps1        ← occasional (USB write)
│   └── write-usb.bat
├── emulators/
│   ├── run-vbox.ps1         ← secondary (VirtualBox)
│   ├── run-vbox.bat
│   ├── run-windows.ps1      ← secondary (QEMU Windows host)
│   ├── run-windows.bat
│   ├── run-windows-1080p.bat  ← delete after §3.4
│   ├── run-windows-1440p.bat
│   └── run-windows-4k.bat
└── secure-boot/
    └── build-shim.sh        ← one-time (shim build)
```

> [!NOTE]
> **Scripts directory reorganization notes (2026-03-17):**
> - Moved 10 scripts into `deploy/` (2), `emulators/` (7), `secure-boot/` (1) — root keeps 3 daily-use scripts
> - **Internal path fixes required:** all `.ps1` scripts used `Split-Path -Parent $SCRIPT_DIR` to find repo root — needed extra `Split-Path` level after move. `build-shim.sh` needed `../..` instead of `..`
> - **External references updated:** `Makefile` (3 refs), `TODO-006-Real-Hardware.md` (8 refs), `TODO-010-Bootloader.md` (1 ref). No workflow refs needed changes
> - `.bat` files use `%~dp0` (self-relative) — worked correctly without changes

---

## 2. Toolchain & Dependency Management

### 2.1 Migrate to Clang/LLD Toolchain ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `CC=clang-19`, `LD=ld.lld-19`, `OBJCOPY=llvm-objcopy-19` in the root Makefile, `--target=x86_64-elf` in CFLAGS, NASM pipeline untouched, UEFI bootloader builds a valid PE32+ EFI binary. Run `bash scripts/build.sh clean` and verify output. Fix any inconsistencies in the TODO items below.

> [!IMPORTANT]
> → XREF: `TODO-002 §6.5` — The Clang migration is a **prerequisite** for clangd
> code intelligence. `compile_commands.json` relies on Clang-compatible compilation
> databases generated by Bear.
> → XREF: `TODO-003-Antigravity.md §5.3` — Gemini semantic search also benefits
> from accurate `compile_commands.json` generated after the Clang migration.

> [!WARNING]
> **Do NOT touch the NASM pipeline.** Assembly files (`.asm`) for boot entry,
> GDT/IDT stubs, ISR trampolines, and SMP trampoline remain compiled by NASM.
> Only the C compiler (`CC`) and linker (`LD`) change.

**Step 1: Update Makefile toolchain variables**

- [x] Replace `CC`, `LD`, `OBJCOPY`, `AR` in root Makefile → `clang-19`, `ld.lld-19`, `llvm-objcopy-19`, `llvm-ar-19`
- [x] Add `--target=x86_64-elf` to `CFLAGS` and `USER_CFLAGS`
- [x] Remove `-no-pie` (Clang only uses `-fno-pie`)
- [x] Verify all `$(CC)` invocations compile correctly with Clang
- [x] Verify `$(LD)` with LLD links the kernel ELF correctly

**Step 2: Migrate UEFI bootloader to Clang/LLD**

- [x] Update `src/boot/uefi/Makefile`: `CC=clang-19`, `LD=ld.lld-19`
- [x] Keep GNU `objcopy` for EFI conversion (llvm-objcopy lacks `efi-app-x86_64` target)
- [x] Verify BOOTX64.EFI is a valid PE32+ binary: `PE32+ executable (EFI application) x86-64`

**Step 3: Verify linker script compatibility**

- [x] `linker.ld` — simple `ENTRY`, `OUTPUT_FORMAT`, `SECTIONS` with `ALIGN()` — fully LLD-compatible
- [x] `uefi.lds` — same, no GNU-specific extensions
- [x] Kernel memory sections (`.text`, `.rodata`, `.data`, `.bss`) map correctly
- [x] Kernel entry point resolves correctly

**Step 4: Update build scripts and agent config**

- [x] Update `.agents/rules/rules.md`: `Use clang-19 --target=x86_64-elf with ld.lld-19`
- [x] Update `.agent/skills/impossible-os/SKILL.md`: native programs → `clang-19 --target=x86_64-elf`

**Step 5: Full verification cycle**

- [x] `bash scripts/build.sh clean` → `=== BUILD OK ===`
- [x] BOOTX64.EFI → valid PE32+ (EFI application) x86-64
- [x] Kernel size: 2,649,920 bytes (Clang) vs 2,880,648 bytes (GCC) — **8% smaller**
- [x] Fixed 12 Clang-specific warnings: 11 unused-function (`__attribute__((unused))`), 1 unused-but-set-variable
- [x] Commit: `"build: migrate to Clang/LLD toolchain"`

> [!NOTE]
> **Clang/LLD migration notes (2026-03-17):**
> - Installed: `clang-19` (19.1.1), `lld-19`, `llvm-19` via `sudo apt install`
> - `HOST_CC` remains `gcc` — host tools (jpg2raw, irespack, make-system-disk) target the build machine
> - UEFI bootloader uses Clang→ELF→GNU objcopy→PE/COFF pipeline (same as before, just different compiler/linker)
> - `llvm-objcopy` does NOT support `--target efi-app-x86_64` — this is GNU-specific
> - Clang is stricter about `-Wunused-function` on static inline helpers — 11 port I/O helpers
>   across 7 driver files needed `__attribute__((unused))`
> - Build time: 6.8s (Clang -j12) vs 8.0s (GCC -j12) — Clang is slightly faster

### 2.2 System Dependency Installer ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/setup-deps.sh` detects distro, installs all required packages idempotently, and prints a version summary. Run `bash scripts/setup-deps.sh` and verify output. Fix any inconsistencies in the TODO items below.

- [x] Create `scripts/setup-deps.sh`
- [x] Detect distro: Ubuntu/Debian (`apt`), Fedora (`dnf`), Arch (`pacman`)
- [x] Install: `nasm` (3.01+), `clang` (19+), `lld` (19+), `llvm` (19+)
- [x] Install: `xorriso`, `mtools`, `qemu-system-x86` (10.2+), `ovmf`
- [x] Install: `python3`, `python3-pil`, `dosfstools`, `parted`, `cppcheck` (2.20+)
- [x] Install: `bear` (for compile_commands.json generation — §6.5)
- [x] Install: `clangd` (19+ — for code intelligence — §6.5)
- [x] Idempotent: check if each package is already installed before installing
- [x] Print summary: "All dependencies installed" or list missing packages
- [x] Commit: `"tools: system dependency installer"`

> [!NOTE]
> **Dependency installer notes (2026-03-17):**
> - Uses `command -v` for binary checks, file existence for data packages (OVMF)
> - Pillow installed via `pip3` (not a system package on most distros)
> - Colored output with ✓/·/✗ status indicators
> - Prints version summary at the end (nasm, clang, qemu, python3)

### 2.3 One-Command Setup ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/setup.sh` installs all deps then verifies with a clean build, and `README.md` has a "Getting Started" section. Run `bash scripts/setup.sh` and verify output. Fix any inconsistencies in the TODO items below.

- [x] Create `scripts/setup.sh` — runs `setup-deps.sh` + verification build
- [x] Update `README.md` with "Getting Started" section:
  ```
  git clone https://github.com/rizonesoft/impossible-os.git
  cd impossible-os
  bash scripts/setup.sh
  bash scripts/build.sh run
  ```
- [ ] Test on fresh Ubuntu 22.04 WSL instance (clean slate)
- [x] Commit: `"tools: one-command dev environment setup"`

> [!NOTE]
> **One-command setup notes (2026-03-17):**
> - No separate `setup-toolchain.sh` needed — Clang-19 installs via system packages (no cross-compiler build)
> - `setup.sh` runs `setup-deps.sh` then does a verification `build.sh clean`
> - README "Getting Started" replaced old "Build Requirements" + "Quick Start" sections
> - Also fixed VirtualBox script path in README Testing section (`scripts/emulators/run-vbox.bat`)

---

## 3. Emulator Testing Scripts

### 3.1 QEMU Test Runner ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/run-qemu.sh` launches QEMU with the correct flags (UEFI firmware, AHCI disk, serial stdio, VGA resolution). Also confirm `scripts/build.sh run` wraps this correctly. Fix any inconsistencies in the TODO items below.

- [x] `scripts/run-qemu.sh` — launch QEMU with UEFI + AHCI + serial
- [x] `bash scripts/build.sh run` — build + auto-launch QEMU
- [x] QEMU flags: `-bios OVMF.fd`, AHCI disk, `-serial stdio`, VGA resolution
- [x] OVMF firmware auto-copy to `build/`

### 3.2 VirtualBox Test Runner ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/emulators/run-vbox.sh` creates/updates a VirtualBox VM with EFI, AHCI, VMSVGA, 2048 MB RAM, and supports `--headless` and `--debug` flags. Fix any inconsistencies in the TODO items below.

- [x] Create `scripts/emulators/run-vbox.sh`
- [x] Check if VBoxManage is available
- [x] Create/update VM: `ImpossibleOS` with EFI, AHCI, VMSVGA, 2048 MB RAM, 4 CPUs
- [x] Convert raw disk to VDI and attach as AHCI port 0
- [x] Start VM in headless mode or with GUI
- [x] Add `--headless` flag for CI use
- [x] Add `--debug` flag for debug boot (skip splash)
- [x] Commit: `"tools: cross-platform VirtualBox runner"`

> [!NOTE]
> **VirtualBox runner notes (2026-03-17):**
> - Mirrors `run-vbox.ps1` settings exactly: 2048 MB, 4 CPUs, VMSVGA, 1280x720, PS/2, serial log
> - Converts raw `system-disk.img` → VDI on each run (picks up latest build)
> - Properly unregisters old VDI before re-converting (avoids UUID mismatch)
> - `--debug` injects DEBUG flag into Logs partition via `mcopy`
> - Mouse integration disabled (no Guest Additions)

### 3.3 Hyper-V Test Runner

> **Moved to [TODO-008-Hyper-V-Runner.md](TODO-008-Hyper-V-Runner.md)** — Full Hyper-V Gen 2
> boot support: test runner script, VMBus core, synthetic SCSI/HID/video/NIC,
> APIC-only mode, MMIO safety, synthetic timer, power management, and guest additions.

---

## 4. Hardware Deployment Scripts

### 4.1 USB Write Script ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/deploy/write-usb.ps1` creates a GPT-partitioned USB drive with an EFI System Partition (FAT32), a system partition (FAT32), and a logs partition (FAT32). Run on a test USB drive and verify all partitions are created correctly.

- [x] `scripts/deploy/write-usb.ps1` — write ISO to USB drive (Windows/PowerShell)
- [x] `scripts/deploy/write-usb.bat` — wrapper for PowerShell script
- [x] GPT partition layout: EFI (FAT32) + System (FAT32) + Logs (FAT32)
- [x] Auto-detect USB drive letter
- [x] Safety prompts before write

### 4.2 USB Write Script (Linux) ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/deploy/write-usb.sh` detects removable USB drives, requires double confirmation, writes the raw image via `dd`, and verifies boot files after write. Run `bash scripts/deploy/write-usb.sh --help` and verify output. Fix any inconsistencies in the TODO items below.

> [!CAUTION]
> **Safety:** This script writes to raw block devices. Triple-check the target device.
> Never auto-detect and write without user confirmation.

- [x] Create `scripts/deploy/write-usb.sh`
- [x] List removable USB devices: `lsblk -d -n -o NAME,SIZE,TRAN,RM,MODEL`
- [x] User selects device (e.g., `/dev/sdb`)
- [x] Double confirmation: Type "YES" + type device name to confirm
- [x] Write raw `system-disk.img` via `dd` (image includes GPT + all partitions)
- [x] Verify: mount EFI partition, check BOOTX64.EFI and kernel.exe
- [x] Print boot instructions at end
- [x] Commit: `"tools: Linux USB write script"`

> [!NOTE]
> **Linux USB write notes (2026-03-17):**
> - Writes the raw `system-disk.img` directly via `dd` (same approach as Windows script)
> - Image already contains GPT + EFI + System + Logs partitions — no `parted`/`mkfs` needed
> - Requires root (`sudo`) — checks `id -u` at start
> - After write: `sync` + `partprobe`, then mounts EFI partition to verify boot files
> - Double confirmation: YES + device name (e.g., "sdb") to prevent accidents

### 4.3 USB Log Reader ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/deploy/read-usb-log.sh` mounts the Logs partition read-only, copies logs to `build/logs/<timestamp>/`, and scans for panics. Fix any inconsistencies in the TODO items below.

- [x] Create `scripts/deploy/read-usb-log.sh`
- [x] Mount USB logs partition (read-only)
- [x] Copy `debug.log` and all `.log`/`.txt`/`crashdump*` files to `build/logs/<date>/`
- [x] Print summary: file sizes, first/last log line, panic/fault detection
- [x] Linux: auto-detect third partition of USB device (handles `/dev/sdX3` and `/dev/nvme0n1p3`)
- [x] Commit: `"tools: USB log reader"`

> [!NOTE]
> **USB log reader notes (2026-03-17):**
> - Script placed in `scripts/deploy/` alongside `write-usb.sh`
> - Accepts optional device argument (`/dev/sdX`) or auto-detects removable USB drives
> - Mounts read-only (`-o ro`) — safe for forensic log collection
> - Scans for panic/BSOD/fault keywords and highlights them in red
> - Logs saved to `build/logs/<timestamp>/` for historical comparison

---

## 5. Test Framework

### 5.1 Kernel Unit Test Framework
### 5.1 Kernel Unit Test Framework ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `include/kernel/test/test.h` provides `TEST_ASSERT()`, `test_suite_register()`, and conditional compilation. Confirm `src/kernel/test/test_runner.c` runs suites and prints summary. Run `bash scripts/build.sh clean` with and without `-DKERNEL_TESTS`. Fix any inconsistencies in the TODO items below.

- [x] Create `include/kernel/test/test.h` — `TEST_ASSERT(cond, msg)`, `test_suite_register(name, fn)`
- [x] Create `src/kernel/test/test_runner.c` — run all registered suites, print summary
- [x] `TEST_ASSERT()` logs to serial: `[PASS] suite :: msg` or `[FAIL] suite :: msg (file:line)`
- [x] Summary output: `"N passed, N failed"` via `log_info`/`log_error` to serial
- [x] Conditional compilation: `#ifdef KERNEL_TESTS` — without it, all APIs compile to nothing
- [x] Commit: `"test: kernel unit test framework"`

> [!NOTE]
> **Test framework notes (2026-03-17):**
> - Header at `include/kernel/test/test.h`, impl at `src/kernel/test/test_runner.c`
> - Uses `log_info("TEST", ...)` and `log_error("TEST", ...)` for serial output
> - `test_runner.c` is entirely inside `#ifdef KERNEL_TESTS` — empty object in normal builds
> - Max 64 suites, each suite is a `void (*fn)(void)` that calls `TEST_ASSERT()` internally
> - To enable: add `-DKERNEL_TESTS` to CFLAGS, call `test_runner_init()` + `test_runner_run()` from `main.c`
> - `test_runner_init()` has placeholder comments for registering subsystem tests (§5.2)

### 5.2 Core Subsystem Tests ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm 5 test files exist in `src/kernel/test/`, all are registered in `test_runner.c`, and `bash scripts/build.sh clean` passes. Fix any inconsistencies in the TODO items below.

- [x] PMM tests: alloc + free → frame reuse; contiguous allocation; page alignment
- [x] Heap tests: kmalloc + kfree round-trip; zero-byte returns NULL; no-overlap; krealloc
- [x] VFS tests: create/write/read/close file roundtrip; open nonexistent; mkdir+rmdir
- [x] Scheduler tests: thread_create returns valid TID
- [x] Registry tests: set/get REG_DWORD; set/get REG_SZ; key cleanup
- [x] Commit: `"test: core subsystem unit tests"`

> [!NOTE]
> **Core subsystem test notes (2026-03-17):**
> - 5 test files, 12 test suites total, all in `src/kernel/test/`
> - All tests are self-contained: allocate, test, clean up
> - All gated behind `#ifdef KERNEL_TESTS` — compile to empty objects in normal builds
> - `test_runner.c` calls all 5 `test_register_*()` functions
> - VFS tests use IXFS paths (`C:\Impossible\...`) and clean up temp files
> - Registry tests use Win32 API (`RegSetValueEx`, `RegGetValue`, `RegDeleteKey`)
> - Scheduler tests are minimal (thread creation only) — preemptive testing requires runtime

### 5.3 Automated QEMU Smoke Test ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/test-smoke.sh` builds, boots QEMU headless, captures serial for 30s, and checks for pass/fail patterns. Run `bash scripts/test-smoke.sh` and verify output. Fix any inconsistencies in the TODO items below.

- [x] Create `scripts/test-smoke.sh`
- [x] Build: `bash scripts/build.sh clean`
- [x] Launch QEMU headless: `-display none -serial file:build/smoke-test.log`
- [x] Capture serial output for 30 seconds (timeout)
- [x] Pass criteria: output contains `"Boot complete in"`
- [x] Fail criteria: output contains `"KERNEL PANIC"`, `"ASSERT FAILED"`, `"triple fault"`, `"Page Fault"`
- [x] Exit code: 0 = pass, 1 = fail
- [x] Print: `"SMOKE TEST PASSED"` or `"SMOKE TEST FAILED: <reason>"`
- [x] Commit: `"test: automated QEMU smoke test"`

> [!NOTE]
> **Smoke test notes (2026-03-17):**
> - Uses KVM acceleration when available (`/dev/kvm`)
> - Serial output to file (`build/smoke-test.log`), not stdio — avoids buffering issues
> - Polling loop checks log every 1s for pass/fail patterns; kills QEMU on match
> - Shows boot time on pass, last 10 serial lines on fail
> - QEMU launched as background process, cleaned up on exit

### 5.4 FAT32 Filesystem Test Suite ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/test-fs.sh` generates test disks, boots each in QEMU headless, captures serial, and reports per-filesystem pass/fail. Run `bash scripts/test-fs.sh fat32` and verify output. Fix any inconsistencies in the TODO items below.

> **XREF:** See `/test-fs-fat32` workflow for the FAT32-specific test patterns.

- [x] Create `scripts/test-fs.sh`
- [x] Generate test disk images via `tools/make-test-disks.sh` (FAT32, exFAT, ext2/3/4, NTFS, IXFS, ISO 9660, Joliet, UDF)
- [x] Each image contains known test files (test.txt, subdir/nested.txt, empty.txt, large.bin)
- [x] Boot QEMU headless with test disk on AHCI port 1 (or ATAPI CD for optical)
- [x] Check serial for boot completion + FS detection markers
- [x] Report: per-filesystem pass/fail summary with per-test log files
- [x] Selective testing: `bash scripts/test-fs.sh fat32 ext4`
- [x] Commit: `"test: filesystem test suite"`

> [!NOTE]
> **Filesystem test suite notes (2026-03-17):**
> - Uses existing `tools/make-test-disks.sh` for image generation (14 images)
> - Skips partition table tests (mbr, gpt) — no filesystem to test
> - Each test: fresh OVMF vars, 20s timeout, KVM when available
> - Logs saved per-filesystem in `build/fs-tests/<name>.log`
> - Optical media (ISO/UDF) attached as ATAPI CD via `ide-cd` on AHCI port 1

---

## 6. Development Utilities

### 6.1 Symbol Map Generator ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `build/kernel.map` and `build/kernel.sym` are generated during build, `symtab_init()` loads the symbol table from IXFS, and `panic.c` resolves addresses to symbol names. Run `bash scripts/build.sh clean` and check `wc -l build/kernel.map`. Fix any inconsistencies in the TODO items below.

- [x] Add `llvm-nm-19 -n $(KERNEL_BIN) > build/kernel.map` to Makefile after kernel link
- [x] Convert to binary format via `tools/convert_symmap.py`, load from IXFS at boot (`C:\Impossible\System\kernel.sym`)
- [x] `panic.c`: resolve RBP-chain addresses via `symtab_resolve()` — binary search in sorted table
- [x] Boot log: `[INFO ][SYMTAB] Symbol map loaded: N symbols (K KB)`
- [x] Stack trace output: `#0  0xADDR  func_name+0xoffset` (on both BSOD screen and serial)
- [x] Commit: `"build: symbol map generation"`

> [!NOTE]
> **Symbol map notes (2026-03-17):**
> - `tools/convert_symmap.py` converts nm text to packed binary (KSYM format: 8-byte addr + 32-byte name)
> - `src/kernel/symtab.c` loads from VFS, allocates via PMM (40 KB for ~1000 symbols)
> - O(log n) binary search — safe to call from panic context (no allocations)
> - Only includes T/t/D/d symbols, skips compiler internals (`.` and `$` prefixes)
> - `kernel.sym` is copied to sysroot and included in the IXFS partition automatically

### 6.2 Code Size & Bloat Tracking ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/size-report.sh` reports kernel.exe and system-disk.img sizes, tracks history in CSV, and shows deltas. Run `bash scripts/size-report.sh` and verify output. Fix any inconsistencies in the TODO items below.

- [x] Create `scripts/size-report.sh`
- [x] Report: kernel.exe size, each `.o` file size (top 10 largest), system-disk.img size
- [x] Append to `build/size-history.csv` (date, commit, kernel size, disk size)
- [x] Compare against previous build: show delta (+/- bytes) with color
- [x] Warn threshold: kernel > 8 MB, system disk > 1 GB
- [x] Section breakdown via `llvm-size-19`
- [x] Commit: `"tools: code size tracking"`

> [!NOTE]
> **Size tracking notes (2026-03-17):**
> - Current sizes: kernel.exe ~2.6 MB, system-disk.img 512 MB
> - Thresholds intentionally high — kernel shouldn't hit 8 MB, disk shouldn't hit 1 GB
> - CSV history is append-only, survives incremental builds, reset on `clean`
> - Shows human-readable sizes (KB/MB/GB) with colored deltas (red = grew, green = shrank)

### 6.3 Code Style Linter ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/lint.sh` checks all 6 style rules and reports violations. Run `bash scripts/lint.sh` and review output. Fix any inconsistencies in the TODO items below.

- [x] Create `scripts/lint.sh`
- [x] Check: snake_case for function definitions (detects camelCase, excludes Win32 API wrappers)
- [x] Check: UPPER_CASE for `#define` macros (flags pure lowercase macros)
- [x] Check: `#pragma once` or include guard in every `.h` file
- [x] Check: lines ≤ 120 characters (excludes comment lines)
- [x] Check: no trailing whitespace
- [x] Check: functions ≤ 50 lines (warn, not error)
- [x] Report: `file:line: violation description` with colored output
- [x] Exit 0 (clean) or 1 (violations found — errors only, warnings don't fail)
- [x] Commit: `"tools: code style linter"`

> [!NOTE]
> **Linter notes (2026-03-17):**
> - Excludes auto-generated files: `build_info.h`, `os_logo.h`, `bsod_icon.h`, `boot_splash_font_data.h`
> - Excludes third-party: `stb_truetype`, `stb_image`
> - camelCase detection skips Win32 API names (`Reg*`, `HKEY*`)
> - Function length uses awk brace-depth tracking for accuracy
> - Supports path argument: `bash scripts/lint.sh src/kernel/mm/`

### 6.4 Debug Script Enhancement ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `scripts/debug.sh` generates a `.gdbinit`, connects to QEMU, loads symbols, and sets breakpoints. Run `bash scripts/debug.sh --help` and verify flags. Fix any inconsistencies in the TODO items below.

- [x] Update `scripts/debug.sh` to launch QEMU with `-s -S` via `run-qemu.sh --debug`
- [x] Auto-generate `.gdbinit` with: `target remote :1234`, `symbol-file build/kernel.exe`
- [x] Default breakpoints: `kernel_main`, `panic`, `page_fault_handler`, `general_protection_fault_handler`, `double_fault_handler`
- [x] `--breakpoint=<function>` — add custom breakpoint(s)
- [x] `--no-default-bp` — skip default breakpoints
- [x] Print GDB connection banner with useful commands
- [x] Commit: `"tools: enhanced GDB debug script"`

> [!NOTE]
> **Debug script notes (2026-03-17):**
> - Generates `build/.gdbinit-kernel` dynamically (cleaned up on exit)
> - Intel disassembly syntax, pagination off, confirm off
> - Shows symbol count from `kernel.map` in banner
> - Auto-builds kernel if `kernel.exe` not found
> - QEMU cleanup on GDB exit (kills background process)

### 6.5 clangd + Bear (Deep C/C++ Intelligence) ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `bash scripts/build.sh clean` generates `compile_commands.json`, `.clangd` config exists at repo root, and `compile_commands.json` is in `.gitignore`. Run `python3 -c "import json; print(len(json.load(open('compile_commands.json'))))"` to check entry count.

> [!IMPORTANT]
> **Prerequisites:** Complete `§2.1 Migrate to Clang/LLD` first — Bear intercepts
> the Clang compilation commands to generate `compile_commands.json`.
>
> → XREF: `TODO-003-Antigravity.md §5.3` — Gemini semantic search MCP also consumes
> the `compile_commands.json` generated here. The clangd MCP server configured here
> is Phase 2 of the 3-phase intelligence pipeline:
> - **Phase 1:** Clang/LLD migration (§2.1) — produces Clang-compatible build
> - **Phase 2:** clangd + Bear (this section) — C/C++ code intelligence
> - **Phase 3:** Gemini semantic search (`TODO-003 §5.3`) — AI-powered codebase search

**Step 1: Install Bear and wrap the build** *(agent)* ✅

- [x] Verify `bear` and `clangd-19` are in `scripts/setup-deps.sh` (added in §2.2)
- [x] Update `scripts/build.sh` — kernel build step wrapped with `bear --append --` on clean builds
- [x] Verify: after `bash scripts/build.sh clean`, `compile_commands.json` exists (101 entries, 73 KB)
- [x] Verify: entries contain `clang-19 --target=x86_64-elf -ffreestanding ...`
- [x] Add `compile_commands.json` to `.gitignore` (machine-specific absolute paths)

**Step 2: Create `.clangd` config for kernel environment** *(agent)* ✅

- [x] Create `.clangd` at repo root with `--target=x86_64-elf`, `-nostdlib`, `-ffreestanding`, `-mno-red-zone`
- [x] Commit `.clangd` to repo (it's project-wide config, not machine-specific)

**Step 3: Verify code intelligence works** *(manual — developer tests in editor)* ✅

- [x] Open the project in an LSP-compatible editor (VS Code + clangd extension)
- [x] Verify: `clangd` resolves `#include "kernel/types.h"` correctly (no red squiggles)
- [x] Verify: `clangd` does NOT inject host `/usr/include/` headers
- [x] Verify: go-to-definition works for kernel functions (`printk`, `kmalloc`, `vfs_open`)

**Step 4: ~~Wire clangd as an MCP server in Antigravity~~** *(N/A — see gotcha below)*

- [x] ~~Open Antigravity → Agent Manager → MCP Servers → Manage → View raw config~~ — N/A
- [x] ~~Add the `impossible-os-clangd` server entry~~ — N/A (clangd speaks LSP, not MCP)
- [x] Commit: `"tools: clangd + Bear compilation database"`

> [!NOTE]
> **clangd + Bear notes (2026-03-17):**
> - Bear only wraps kernel build step (not userland/host tools) to avoid PIPESTATUS issues
> - Uses `bear --append --` to accumulate entries across make invocations in a single build
> - `compile_commands.json` regenerated on every `bash scripts/build.sh clean`
> - `.clangd` uses `Index.Background: Build` for faster indexing
> - Step 3 verified manually — clangd extension installed, `clangd.path` set to `/usr/bin/clangd-19`

> [!WARNING]
> **Gotcha: clangd is LSP, not MCP.** clangd speaks the Language Server Protocol (LSP), not
> the Model Context Protocol (MCP). Attempting to add clangd as an MCP server in Antigravity
> causes it to hang indefinitely on "refreshing" — the two protocols are incompatible.
> **Use clangd via the VS Code/Cursor clangd extension** (LSP), not via `mcp_config.json`.
> *(Discovered 2026-03-17)*

---

## 7. Asset Pipeline

### 7.1 Asset Build Script ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `make assets` builds all generated headers (os-logo, bsod-icon, boot-font) and populates sysroot with fonts, wallpapers, cursors, and icons.ires. Verify dependency tracking: `touch resources/fonts/selawk.ttf && make assets` should only rebuild fonts. Run `bash scripts/build.sh clean` and verify output. Fix any inconsistencies in the TODO items below.

> **XREF:** See `/add-asset` workflow for the full checklist when adding a new asset.

- [x] Create `make assets` target in Makefile — groups 7 sub-targets
- [x] PNG icons → BGRA C array headers (via `tools/convert_icon.py` — multi-size os\_logo, `tools/convert_bsod_icon.py` — BSOD icon)
- [x] ~~BMP cursors → BGRA C array headers~~ — N/A (cursors are Adwaita XCursor format, copied as binary blobs to sysroot)
- [x] TTF fonts → copy to `sysroot/Impossible/Fonts/` (11 .ttf files via `sysroot-fonts`)
- [x] Wallpaper → copy to `sysroot/Impossible/Wallpapers/` (decoded at runtime by `image_load`)
- [x] Dependency tracking: rebuild only changed assets (stamp files per sub-target)
- [x] Commit: `"build: unified asset pipeline"`

> [!NOTE]
> **Asset pipeline notes (2026-03-17):**
> - `make assets` groups: `os-logo`, `bsod-icon`, `boot-font`, `sysroot-fonts`, `sysroot-wallpapers`, `sysroot-cursors`, `sysroot-icons`
> - Monolithic `sysroot` recipe split into 5 tracked sub-targets with `.stamp` files
> - `sysroot-dirs` creates the standard directory tree (order-only prerequisite of all others)
> - Incremental rebuild: touching `selawk.ttf` → only fonts recopy; touching `background.jpg` → only wallpaper recopy
> - No `png2header.py` exists — the TODO mentioned it but the existing `convert_icon.py` and `convert_bsod_icon.py` already handle PNG→BGRA
> - Cursors are Adwaita XCursor (not BMP) — the kernel's cursor driver reads XCursor natively
> - `sysroot-icons` packs PNG icons into IRES via `irespack` host tool

### 7.2 Asset Validation ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `make validate-assets` runs `tools/validate-assets.py`, checks all PNG/TTF/XCursor/JPEG assets, and returns exit 1 on any failure. Corrupt a PNG file and verify the build fails with a clear error. Run `bash scripts/build.sh clean` and verify output. Fix any inconsistencies in the TODO items below.

- [x] Validate PNG icons: expected dimensions, RGBA format, file size < 1 MB (13 logos + BSOD icon)
- [x] Validate TTF fonts: parseable, glyph count > 0 (11 fonts, e.g. CascadiaCode 4319 glyphs)
- [x] Validate cursors: dimensions ≤ 256, hotspot within bounds (11 Adwaita XCursor)
- [x] Validate wallpapers: JPEG SOI/EOI markers + Pillow decode when available (< 8K)
- [x] Print: `[OK] 37 assets validated` or `[FAIL] os_logo_32.png: invalid PNG magic`
- [x] Commit: `"build: asset validation"`

> [!NOTE]
> **Asset validation notes (2026-03-17):**
> - Script at `tools/validate-assets.py` — no external dependencies (stdlib only, Pillow optional for JPEG)
> - `make validate-assets` runs as first dependency of `assets` → fails the build before any conversion starts
> - 37 assets validated: 14 PNGs, 11 TTFs, 11 XCursors, 1 JPEG
> - TTF validation checks sfVersion (TrueType vs OpenType/CFF), numTables > 0, and `maxp` glyph count
> - XCursor validation reads the Xcur TOC, checks image dims ≤ 256 and hotspot within bounds
> - Cursors are Adwaita XCursor format (not BMP) — the TODO mentioned BMP but that doesn't match reality
> - Negative test confirmed: corrupted PNG → `[FAIL] 1/37 assets failed validation` → exit 1

---

## 8. Local CI Hooks

> [!NOTE]
> **GitHub Actions CI/CD** (build workflows, release automation, stale issue cleanup,
> auto-labeling) lives in **[TODO-001-GitHub.md §4](TODO-001-GitHub.md)** — that's
> the canonical location for all GitHub-specific infrastructure.
> This section covers **local developer hooks** only.

### 8.1 Pre-Commit Hooks

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
| ✅ Done   | 1.4 Build Version & Metadata      | `ver` command, BSOD footer, boot log version info        |
| ✅ Done   | 5.3 QEMU Smoke Test               | Catches boot regressions automatically                   |
| ✅ Done   | 1.2 Incremental Build             | Dev iteration speed — 5.8× faster incremental builds     |
| ✅ Done   | 2.1 Clang/LLD Migration           | Prerequisite for clangd + Bear + semantic search         |
| ✅ Done   | 2.2 System Dependency Installer   | Onboarding — new devs need one-command setup             |
| ✅ Done   | 6.1 Symbol Map                    | BSOD stack traces show function names                    |
| ✅ Done   | 6.5 clangd + Bear                 | Deep code intelligence — compile_commands.json generated |
| ✅ Done   | 1.3 Parallel Build                | Build speed — 2.5× faster with -j12                      |
| ✅ Done   | 1.5 Scripts Directory Organization| Daily scripts at root, secondary in subdirs              |
| ✅ Done   | 2.3 One-Command Setup             | After §2.1 + §2.2                                        |
| ✅ Done   | 5.1 Unit Test Framework           | Foundation for systematic testing                        |
| ✅ Done   | 5.4 Filesystem Test Suite         | Validates FS drivers against real disk images            |
| ✅ Done   | 7.1 Asset Pipeline                | Unified `make assets` with dependency tracking           |
| ✅ Done   | 3.2 VirtualBox Test Runner        | Cross-platform VBox launcher with VBoxManage             |
| 🟢 P3     | 3.3 Hyper-V Runner                | Production target testing                                |
| 🟢 P3     | 3.4 Multi-Resolution Launcher     | Consolidates resolution-specific scripts                 |
| ✅ Done   | 4.2 USB Write (Linux)             | Cross-platform hardware deployment via dd                |
| ✅ Done   | 4.3 USB Log Reader                | Hardware debugging log retrieval                         |
| ✅ Done   | 5.2 Core Subsystem Tests          | PMM, heap, VFS, sched, registry — 12 suites              |
| ✅ Done   | 6.2 Size Tracking                 | Detect bloat early — tracks kernel + disk sizes          |
| ✅ Done   | 6.3 Code Linter                   | Style consistency — 6 automated checks                   |
| ✅ Done   | 6.4 GDB Debug Enhancement         | Symbol-aware debugging with custom breakpoints           |
| ✅ Done   | 7.2 Asset Validation              | 37 assets validated at build time (PNG/TTF/XCursor/JPEG) |
| 🔵 P4     | 8.1 Pre-Commit Hooks              | Local lint hook — CI build is in TODO-001 §4             |

---

## Key Files

| File                                | Purpose                                  |
|-------------------------------------|------------------------------------------|
| `scripts/build.sh`                  | [EXISTS] Core build script               |
| `scripts/run-qemu.sh`               | [EXISTS] QEMU launcher                   |
| `scripts/debug.sh`                  | [EXISTS] GDB debug launcher              |
| `scripts/deploy/write-usb.ps1`      | [EXISTS] USB write (Windows)             |
| `scripts/setup-toolchain.sh`        | [NEW] Cross-compiler bootstrap           |
| `scripts/setup-deps.sh`             | [NEW] System dependency installer        |
| `scripts/setup.sh`                  | [NEW] One-command setup                  |
| `scripts/emulators/run-vbox.sh`     | [NEW] VirtualBox launcher (Linux)        |
| `scripts/run-hyperv.ps1`            | [NEW] Hyper-V Gen 2 launcher             |
| `scripts/deploy/write-usb.sh`       | [NEW] USB write (Linux)                  |
| `scripts/read-usb-log.sh`           | [NEW] USB log reader                     |
| `scripts/test-smoke.sh`             | [NEW] Automated smoke test               |
| `scripts/test-fs.sh`                | [NEW] Filesystem test suite              |
| `scripts/size-report.sh`            | [NEW] Code size tracker                  |
| `scripts/lint.sh`                   | [NEW] Code style linter                  |
| `.github/workflows/build.yml`       | [NEW] CI build + test                    |
| `.githooks/pre-commit`              | [NEW] Pre-commit lint hook               |
| `include/build_info.h`              | [NEW] Auto-generated build metadata      |
| `.clangd`                           | [NEW] clangd language server config      |
| `compile_commands.json`             | [GENERATED] Bear compilation database    |

---

## OS Comparison

| Feature                           | Windows 11 (WDK/VS)              | Linux Kernel                      | Impossible OS                             |
|-----------------------------------|----------------------------------|-----------------------------------|-------------------------------------------|
| Build system                      | ✅ MSBuild / WDK                  | ✅ Kbuild (make)                 | ✅ Make + build.sh wrapper                |
| Incremental builds                | ✅ MSBuild deps                   | ✅ `.d` dependency files          | ✅ `-MMD -MP` + `.d` includes             |
| Parallel compilation              | ✅ `/MP` flag                     | ✅ `make -j$(nproc)`             | ✅ `-j$(nproc)` default + `--jobs=N`      |
| Build version metadata            | ✅ Resource files (.rc)           | ✅ `uname -r` + git describe     | ✅ `include/build_info.h` (auto-generated)|
| Compiler toolchain                | ✅ MSVC (WDK)                     | ✅ GCC (Kbuild)                  | ✅ Clang-19/LLD-19 (`--target=x86_64-elf`)|
| One-command dev setup             | ❌ Manual VS + WDK install        | ⚠️ `make defconfig && make`      | ✅ `bash scripts/setup.sh` — **beats both**|
| Automated smoke test              | ✅ HCK/HLK test framework         | ✅ kselftest + CI bots            | ✅ `scripts/test-smoke.sh` (headless QEMU)|
| Unit test framework (kernel)      | ✅ WDK test framework              | ✅ KUnit                          | ✅ `test.h` + `test_runner.c` (12 suites) |
| CI/CD build on push               | ✅ Azure DevOps                    | ✅ GitHub Actions + kernel.org    | ⬜ TODO-001 §4.1 P3                       |
| Symbol map + debug symbols        | ✅ PDB files                       | ✅ vmlinux + kallsyms             | ✅ `kernel.sym` + `symtab_resolve()` (O(log n))|
| Code size tracking                | ⚠️ Manual / third-party          | ✅ `bloat-o-meter`               | ✅ `scripts/size-report.sh` + CSV history |
| Pre-commit linting                | ⚠️ Optional VS extensions         | ✅ checkpatch.pl                  | ⬜ §8.1 P4 — `.githooks/pre-commit`      |
| Language server (code intel)      | ✅ IntelliSense (MSVC)             | ✅ clangd + compile_commands     | ✅ clangd-19 + Bear (101 entries)         |
| Asset pipeline                    | ✅ MSBuild resource compiler       | ⚠️ Manual `make` targets         | ✅ `make assets` (7 sub-targets + stamps) |
| Asset validation                  | ❌ Runtime discovery               | ❌ No built-in                    | ✅ 37 assets validated at build time      |
| **Zero-install build wrapper**    | ❌ Requires VS + WDK              | ❌ Requires toolchain install     | ✅ **build.sh — single script, no IDE**   |
| **QEMU auto-test loop**           | ❌ Manual VM setup                 | ✅ virtme + kselftest             | ✅ **build.sh run — build + boot + verify**|
