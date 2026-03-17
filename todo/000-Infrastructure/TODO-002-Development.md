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

### 6.5 clangd + Bear (Deep C/C++ Intelligence)

**Prompt:** Set up `clangd` with a `compile_commands.json` compilation database for byte-accurate C/C++ code intelligence (go-to-definition, auto-complete, diagnostics, refactoring) in the freestanding kernel environment. `clangd` needs to understand the exact compiler flags, include paths, and `--target=x86_64-elf` triple for each translation unit — without this, it defaults to host-OS headers and produces false diagnostics. Use **Bear** (`bear`) to intercept the `make` invocation and auto-generate this database. Configure `.clangd` at the repo root to enforce freestanding flags and suppress host standard library injection. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"tools: clangd + Bear compilation database"`. Add notes directly in this TODO section.

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

> [!NOTE]
> `bear` must be installed on the build machine (added to `setup-deps.sh` in §2.2).
> `compile_commands.json` is regenerated on each `bash scripts/build.sh clean` and
> must be gitignored (it contains absolute paths specific to each developer's machine).

**Step 1: Install Bear and wrap the build** *(agent)*

- [ ] Verify `bear` and `clangd` are in `scripts/setup-deps.sh` (added in §2.2)
- [ ] Update `scripts/build.sh` — locate the final `make` invocation and wrap with `bear --`:
  ```bash
  # Inside scripts/build.sh — locate the make invocation
  echo "Building Impossible OS..."
  bear -- make all
  ```
- [ ] Verify: after `bash scripts/build.sh clean`, `compile_commands.json` exists at repo root
- [ ] Verify: `compile_commands.json` contains entries for **every** `.c` file with correct flags:
  - Each entry has `"command"` containing `clang --target=x86_64-elf -ffreestanding ...`
  - No entries reference host `/usr/include/` paths
- [ ] Add `compile_commands.json` to `.gitignore` (machine-specific absolute paths)

**Step 2: Create `.clangd` config for kernel environment** *(agent)*

- [ ] Create `.clangd` at repo root with this exact content:
  ```yaml
  CompileFlags:
    Add:
      - "--target=x86_64-elf"
      - "-nostdlib"
      - "-ffreestanding"
      - "-mno-red-zone"

  Index:
    Background: Build
  ```
- [ ] Commit `.clangd` to repo (it's project-wide config, not machine-specific)

**Step 3: Verify code intelligence works** *(manual — developer tests in editor)*

- [ ] Open the project in an LSP-compatible editor (VS Code + clangd extension, or Antigravity)
- [ ] Verify: `clangd` resolves `#include "kernel/types.h"` correctly (no red squiggles)
- [ ] Verify: `clangd` does NOT inject host `/usr/include/` headers
- [ ] Verify: go-to-definition works for kernel functions (`printk`, `kmalloc`, `vfs_open`)
- [ ] Verify: auto-complete suggests kernel symbols, not glibc symbols (`printk` not `printf`)
- [ ] Run the full cycle: `bash scripts/build.sh clean` → `compile_commands.json` regenerated → `clangd` picks up changes automatically

**Step 4: Wire clangd as an MCP server in Antigravity** *(manual — developer configures IDE)*

- [ ] Open Antigravity → Agent Manager → MCP Servers → Manage → View raw config
- [ ] Add the `impossible-os-clangd` server entry (see `TODO-003 §5.2` for full JSON)
- [ ] Verify: Antigravity agent can resolve kernel symbols via clangd MCP
- [ ] Commit: `"tools: clangd + Bear compilation database"`

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
| ✅ Done   | 1.4 Build Version & Metadata      | `ver` command, BSOD footer, boot log version info        |
| 🔴 P0     | 5.3 QEMU Smoke Test               | Catches boot regressions automatically                   |
| ✅ Done   | 1.2 Incremental Build             | Dev iteration speed — 5.8× faster incremental builds    |
| ✅ Done   | 2.1 Clang/LLD Migration           | Prerequisite for clangd + Bear + semantic search         |
| ✅ Done   | 2.2 System Dependency Installer   | Onboarding — new devs need one-command setup             |
| 🟠 P1     | 6.1 Symbol Map                    | BSOD stack traces need function names                    |
| 🟠 P1     | 6.5 clangd + Bear                 | Deep code intelligence — requires §2.1 Clang first       |
| ✅ Done   | 1.3 Parallel Build                | Build speed — 2.5× faster with -j12                     |
| ✅ Done   | 1.5 Scripts Directory Organization| Daily scripts at root, secondary in subdirs              |
| ✅ Done   | 2.3 One-Command Setup             | After §2.1 + §2.2                                        |
| 🟡 P2     | 5.1 Unit Test Framework           | Foundation for systematic testing                        |
| 🟡 P2     | 5.4 Filesystem Test Suite         | Validates FS drivers against real disk images            |
| 🟡 P2     | 7.1 Asset Pipeline                | Consolidates scattered asset build steps                 |
| ✅ Done   | 3.2 VirtualBox Test Runner        | Cross-platform VBox launcher with VBoxManage             |
| 🟢 P3     | 3.3 Hyper-V Runner                | Production target testing                                |
| 🟢 P3     | 3.4 Multi-Resolution Launcher     | Consolidates resolution-specific scripts                 |
| ✅ Done   | 4.2 USB Write (Linux)             | Cross-platform hardware deployment via dd                |
| ✅ Done   | 4.3 USB Log Reader                | Hardware debugging log retrieval                         |
| 🟢 P3     | 5.2 Core Subsystem Tests          | After §5.1 framework                                     |
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
| Parallel compilation              | ✅ `/MP` flag                     | ✅ `make -j$(nproc)`             | ✅ `-j$(nproc)` default + `--jobs=N`                     |
| Build version metadata            | ✅ Resource files (.rc)           | ✅ `uname -r` + git describe     | ✅ `include/build_info.h` (auto-generated)                |
| Compiler toolchain                | ✅ MSVC (WDK)                     | ✅ GCC (Kbuild)                  | ✅ Clang-19/LLD-19 (`--target=x86_64-elf`)               |
| One-command dev setup             | ❌ Manual VS + WDK install        | ⚠️ `make defconfig && make`      | ⬜ §2.3 P2 — **beats Windows**            |
| Automated smoke test              | ✅ HCK/HLK test framework         | ✅ kselftest + CI bots            | ⬜ §5.3 P0                                |
| Unit test framework (kernel)      | ✅ WDK test framework              | ✅ KUnit                          | ⬜ §5.1 P2                                |
| CI/CD build on push               | ✅ Azure DevOps                    | ✅ GitHub Actions + kernel.org    | ⬜ §8.1 P3                                |
| Symbol map + debug symbols        | ✅ PDB files                       | ✅ vmlinux + kallsyms             | ⬜ §6.1 P1                                |
| Code size tracking                | ⚠️ Manual / third-party          | ✅ `bloat-o-meter`               | ⬜ §6.2 P3                                |
| Pre-commit linting                | ⚠️ Optional VS extensions         | ✅ checkpatch.pl                  | ⬜ §8.2 P4                                |
| Language server (code intel)      | ✅ IntelliSense (MSVC)             | ✅ clangd + compile_commands     | ⬜ §6.5 P1 — clangd + Bear               |
| **Zero-install build wrapper**    | ❌ Requires VS + WDK              | ❌ Requires toolchain install     | ✅ **build.sh — single script, no IDE**   |
| **QEMU auto-test loop**           | ❌ Manual VM setup                 | ✅ virtme + kselftest             | ⬜ **§5.3 — build + boot + verify**       |
