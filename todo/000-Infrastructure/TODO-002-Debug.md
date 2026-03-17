# P0002 — Debug & Logging System

> **Goal:** Build a robust, unified logging and debugging system that works reliably
> across all environments: USB boot on real hardware, QEMU testing, and VirtualBox.
> Produce structured, useful logs with hardware dumps and numbered log files to
> avoid overwriting. Eliminate the current patchwork of inconsistent logging paths
> and unreliable debug mode activation.

> [!IMPORTANT]
> **This TODO touches multiple other subsystems.** Cross-references are marked with
> `→ XREF:` to indicate work in other TODO files that must be completed to fully
> realize the debug system.

> [!WARNING]
> **Current state (2026-03-17):** The debug/logging system is fragmented across 4 files
> (`klog.c`, `klog_flush.c`, `klog_live.c`, `serial.c`) with overlapping responsibilities,
> inconsistent naming (`serial.log` vs `debug.log`), unreliable debug mode activation
> (requires a `DEBUG` file on the X: partition _after_ kernel boot), and missing features
> (no log rotation, no boot config, no structured hardware dump format).

---

## Current Architecture Audit

### Files Involved

| File | Purpose | Issues |
|------|---------|--------|
| `src/kernel/klog.c` | Ring buffer (1000 entries), serial + framebuffer output | Ring buffer too small for verbose debug; no filter by subsystem |
| `src/kernel/klog_flush.c` | Batch flush to `C:\` (IXFS) + `X:\` (FAT32 `serial.log`) | FAT32 write is full-file overwrite; `serial.log` name loses history |
| `src/kernel/klog_live.c` | Live per-entry flush to `X:\debug.log` + hardware dump | Only active when DEBUG flag detected; duplicates klog_flush logic |
| `src/kernel/drivers/serial.c` | COM1 port I/O (0x3F8) | Works in QEMU/VBox; no output on real hardware without serial port |
| `include/kernel/klog.h` | API declarations | `klog_live_*` API mixed with core klog API |
| `src/kernel/boot_splash.c` | `boot_splash_abort()` for debug mode | Disabling splash is separate concern from logging |
| `src/kernel/main.c` | DEBUG flag detection, klog_flush calls, status messages | DEBUG detection too late (after X: mount); flush calls scattered |

### Output Paths

| Environment | Serial (COM1) | `X:\serial.log` | `X:\debug.log` | `C:\...\kernel.log` |
|-------------|---------------|-----------------|----------------|---------------------|
| QEMU (`-serial stdio`) | ✅ Console | ✅ After boot | ✅ If DEBUG flag | ✅ After boot |
| VBox (serial→file) | ✅ File | ✅ After boot | ✅ If DEBUG flag | ✅ After boot |
| Real hardware (USB) | ❌ No serial port | ❌ Empty if hang | ✅ If DEBUG flag | ❌ If hang |

### Known Bugs & Design Issues

1. **`serial.log` always named `SERIAL.LOG`** — overwrites previous boot. Should be numbered: `SERIAL_1.LOG`, `SERIAL_2.LOG`, etc.
2. **DEBUG flag requires X: to be mounted** — can't enable debug before kernel starts. Should be in boot configuration (UEFI `boot.conf`). → XREF: `TODO-010-Bootloader.md §7.1 Boot Configuration File`
3. **`debug.log` and `serial.log` contain the same data** — confusing. Should be one canonical log file per boot, numbered.
4. **Splash screen abort tied to debug mode** — not needed if logging is robust. Debug mode should only control log verbosity, not UI.
5. **`grub.cfg` exists but is unused** — the OS uses a custom UEFI bootloader (`bootx64.c`). `grub.cfg` is a leftover from the GRUB era and should be removed. → XREF: `TODO-010-Bootloader.md §1`
6. **Hardware dump in `klog_live.c` is not structured** — plain text, not machine-parseable. Should use a consistent format for driver development.
7. **Ring buffer (1000 entries) wraps** — early boot entries lost during verbose boots. No persistent early-boot capture.
8. **No log level filtering on disk** — everything goes to every log. Should have separate debug vs release log levels.
9. **FAT32 full-file overwrite** — every `klog_live_flush()` rewrites the entire file. Becomes slow as log grows.
10. **Reentrancy guard in `klog_live.c`** is a workaround — proper design should avoid recursive logging in VFS.

---

## 1. Unified Log File System ✅ → Refactor

### 1.1 Merge `klog_flush.c` and `klog_live.c` into single `klog_disk.c`

**Prompt:** The current system has two separate files handling disk logging: `klog_flush.c` (batch flush to `C:\` IXFS + `X:\` FAT32 as `serial.log`) and `klog_live.c` (live per-entry flush to `X:\debug.log` + hardware dump). These overlap in functionality and create confusion. Merge them into a single `klog_disk.c` that handles all disk logging with a unified API. The merged file should support both batch and live modes, numbered log files, and proper error handling. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"klog: merge flush + live into unified klog_disk.c"`. Add notes directly in this TODO section.

- [ ] Create `src/kernel/klog_disk.c` — replaces both `klog_flush.c` and `klog_live.c`
- [ ] Delete `src/kernel/klog_flush.c` and `src/kernel/klog_live.c`
- [ ] Single API: `klog_disk_init()`, `klog_disk_flush()`, `klog_disk_set_live(int on)`
- [ ] Write to `C:\Impossible\System\Logs\kernel.log` (IXFS, appendable)
- [ ] Write to `X:\BOOT_NNN.LOG` (FAT32, numbered per boot session)
- [ ] Move hardware dump to separate `hw_dump.c` (see §3)
- [ ] Commit: `"klog: merge flush + live into unified klog_disk.c"`

### 1.2 Numbered Log Files on X: (FAT32)

**Prompt:** Currently `serial.log` on X: always uses the same filename, overwriting previous boot logs. Implement numbered log files: `BOOT_001.LOG`, `BOOT_002.LOG`, etc. On each boot, scan X: for the highest existing `BOOT_NNN.LOG` and create `BOOT_(NNN+1).LOG`. This preserves boot history across multiple sessions — critical for tracking intermittent real-hardware issues. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"klog: numbered log files on X: partition"`. Add notes directly in this TODO section.

- [ ] On `klog_disk_init()`, scan X: root for `BOOT_*.LOG` files
- [ ] Find highest NNN, create `BOOT_(NNN+1).LOG`
- [ ] If no existing logs, start with `BOOT_001.LOG`
- [ ] Cap at 100 files (delete oldest when full) — 16MB partition can hold ~100 logs
- [ ] Log filename in serial output: `klog: writing to X:\BOOT_042.LOG`
- [ ] Commit: `"klog: numbered log files on X: partition"`

### 1.3 Live Flush Mode (Per-Entry Write)

> [!IMPORTANT]
> → XREF: `TODO-040-Filesystem.md §3.4.1 Offset-Aware Write (Append Support)` — **complete this
> FIRST.** Without FAT32 append support, every live flush rewrites the entire growing log file
> from scratch (full-file overwrite). After §3.4.1 is done, live flush becomes a simple append
> of ~100 bytes instead of a rewrite of the entire buffer. This is the #1 performance bottleneck.

**Prompt:** When debug mode is active, every `klog()` entry must be flushed to disk immediately so that even a boot hang leaves the last log line on disk. The current implementation in `klog_live.c` rewrites the entire file on every entry (FAT32 full-file overwrite limitation). Optimise this: use a PMM-backed growing buffer, and only flush every N entries or at explicit flush points. Since FAT32 can't append, each flush must rewrite the entire file — but batching reduces I/O cost. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"klog: optimized live flush with batching"`. Add notes directly in this TODO section.

- [ ] Live mode: flush every 5 entries (configurable) or at explicit `klog_disk_flush()` calls
- [ ] `boot_splash_status()` calls trigger immediate flush (critical checkpoints)
- [ ] Reentrancy guard to prevent recursive flush (VFS operations may trigger klog)
- [ ] PMM buffer: 256KB, grows monotonically, full-file rewrite on each flush
- [ ] Log flush statistics: `klog: live flush #42, 12847 bytes, 3ms`
- [ ] Commit: `"klog: optimized live flush with batching"`

---

## 2. Debug Mode Activation

### 2.1 Boot Configuration File (`boot.conf`)

**Prompt:** The current debug mode requires a `DEBUG` file on the X: partition, which can only be detected _after_ the kernel mounts X: — too late for early boot debugging. Move debug mode activation to the UEFI bootloader via a `boot.conf` file on the EFI partition. The bootloader reads `boot.conf` before `ExitBootServices()` and passes a `debug=1` flag in the boot params struct. The kernel reads this flag immediately in `kernel_main()` — before any filesystem is mounted. This enables debug logging from the very first instruction. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"boot: debug mode via boot.conf"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-010-Bootloader.md §7.1 Boot Configuration File` — this section depends on
> the boot.conf parser being implemented in `bootx64.c`. Coordinate with that TODO item.

- [ ] Add `debug=0|1` key to `boot.conf` format (§7.1 of Bootloader TODO)
- [ ] Parse in `bootx64.c`: `boot_params.debug_mode = (debug == 1)`
- [ ] Add `uint8_t debug_mode` field to `struct boot_info` / `boot_params`
- [ ] In `kernel_main()`: check `g_boot_info.debug_mode` immediately after boot info parsing
- [ ] If debug mode: `klog_set_screen_level(LOG_DEBUG)` — show all log levels on screen
- [ ] If debug mode: enable live disk flush as soon as X: is mounted
- [ ] Remove `X:\DEBUG` file detection from `main.c` (replaced by `boot.conf`)
- [ ] Update `write-usb.ps1` / `write-usb-debug.bat` to write `debug=1` in `boot.conf` instead of creating `DEBUG` file
- [ ] Commit: `"boot: debug mode via boot.conf"`

### 2.2 Remove Splash Screen Abort from Debug Mode

**Prompt:** Currently, debug mode disables the boot splash and shows raw printk output. This is not needed if the logging system is complete and robust — the splash should always show, and debug output goes to the log files. If the user wants to see live output on screen, that should be a separate `verbose=1` option in `boot.conf`, not tied to debug logging. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"boot: decouple splash abort from debug mode"`. Add notes directly in this TODO section.

- [ ] Remove `boot_splash_abort()` call from debug mode activation
- [ ] Add `verbose=0|1` key to `boot.conf` — controls whether printk appears on screen
- [ ] If `verbose=1`: call `boot_splash_abort()` and show live text output
- [ ] If `verbose=0` (default): splash runs normally, debug output goes only to log files
- [ ] Debug mode (`debug=1`) only controls: log verbosity + live disk flush + hardware dump
- [ ] Commit: `"boot: decouple splash abort from debug mode"`

### 2.3 Cleanup: Remove `grub.cfg`

**Prompt:** The file `src/boot/grub.cfg` is a leftover from when the OS used GRUB as its bootloader. The OS now uses a custom UEFI bootloader (`src/boot/uefi/bootx64.c`). `grub.cfg` is unused and confusing. Remove it and clean up any Makefile references. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"boot: remove leftover grub.cfg"`. Add notes directly in this TODO section.

> [!NOTE]
> → XREF: `TODO-010-Bootloader.md §1` — confirm the custom UEFI bootloader is fully
> operational before removing GRUB artifacts.

- [ ] Delete `src/boot/grub.cfg`
- [ ] Remove any `grub.cfg` references from `Makefile`
- [ ] Remove GRUB-related targets from Makefile if any remain (`grub-mkrescue`, etc.)
- [ ] Verify `bash scripts/build.sh clean` still produces `=== BUILD OK ===`
- [ ] Commit: `"boot: remove leftover grub.cfg"`

---

## 3. Hardware Info Dump

### 3.1 Structured Hardware Dump (`hw_dump.c`)

**Prompt:** The current hardware dump in `klog_live.c` is embedded in the live logging code and writes unstructured plain text. Extract it into a standalone `hw_dump.c` that produces a structured, machine-parseable hardware report. This report is critical for driver development — it should contain everything needed to write a driver for any detected device. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"debug: structured hardware dump in hw_dump.c"`. Add notes directly in this TODO section.

- [ ] Create `src/kernel/hw_dump.c` and `include/kernel/hw_dump.h`
- [ ] Move hardware dump logic from `klog_live.c` to `hw_dump.c`
- [ ] `hw_dump_to_log()` — writes hardware info via `klog()` (appears in all logs)
- [ ] `hw_dump_to_file(const char *path)` — writes a standalone `X:\HARDWARE.TXT`
- [ ] Commit: `"debug: structured hardware dump in hw_dump.c"`

### 3.2 CPU Information (CPUID)

**Prompt:** Dump complete CPU identification for driver development: vendor string, brand string, family/model/stepping, all feature flags (SSE, AVX, AES, etc.), cache topology, and core/thread count from CPUID. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"debug: CPU CPUID dump"`. Add notes directly in this TODO section.

- [ ] CPUID leaf 0: vendor string (GenuineIntel / AuthenticAMD)
- [ ] CPUID leaf 0x80000002-0x80000004: brand string (e.g., "Intel Core i5-4210U")
- [ ] CPUID leaf 1: family, model, stepping, feature flags (EDX + ECX)
- [ ] CPUID leaf 7: extended features (AVX2, BMI1/2, RDSEED, ADX, SHA)
- [ ] CPUID leaf 4: cache topology (L1/L2/L3 size and associativity)
- [ ] CPUID leaf 0xB: core/thread topology (x2APIC)
- [ ] Commit: `"debug: CPU CPUID dump"`

### 3.3 PCI Device Enumeration

**Prompt:** Dump every PCI device with full identification data needed for driver development. The current PCI scan in `pci.c` only logs vendor:device and class name. The hardware dump should include: all 6 BARs (with memory vs I/O type), subsystem vendor/device IDs, revision ID, interrupt pin/line, capabilities pointer, and MSI/MSI-X capability if present. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"debug: full PCI device dump"`. Add notes directly in this TODO section.

- [ ] Bus:Dev.Func, Vendor:Device, Subsystem Vendor:Device
- [ ] Class:Subclass:ProgIF, Revision ID
- [ ] All 6 BARs with size detection (write all 1s, read back, mask type bits)
- [ ] BAR type: memory (32/64-bit, prefetchable) vs I/O
- [ ] Interrupt line, interrupt pin
- [ ] Header type (normal, bridge, cardbus)
- [ ] Capabilities list walk (power management, MSI, MSI-X, PCI Express)
- [ ] Commit: `"debug: full PCI device dump"`

### 3.4 ACPI Tables

**Prompt:** Dump ACPI table signatures and addresses for driver development: RSDP version, RSDT/XSDT address, MADT (LAPIC entries, IOAPIC entries), FADT (PM timer, SCI interrupt), MCFG (PCIe ECAM base), HPET, DSDT/SSDT pointers. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"debug: ACPI table dump"`. Add notes directly in this TODO section.

- [ ] RSDP: version, RSDT/XSDT physical address
- [ ] Walk RSDT/XSDT: list all table signatures with physical addresses
- [ ] MADT: LAPIC entries (APIC ID, CPU ID, flags), IOAPIC entries (base, GSI base)
- [ ] FADT: PM1a/PM1b port, SCI interrupt, century register, boot flags
- [ ] MCFG: PCIe ECAM base address, bus range (for future PCIe MMIO config access)
- [ ] HPET: base address, timer count (for future high-precision timer)
- [ ] Commit: `"debug: ACPI table dump"`

### 3.5 Storage, Display, Network

**Prompt:** Dump storage controller details (AHCI ports, drive model strings, capacity, sector size), framebuffer configuration (resolution, BPP, physical address, pitch), and network interface details (NIC type, MAC address, link status). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"debug: storage + display + network dump"`. Add notes directly in this TODO section.

- [ ] AHCI: controller PCI address, ABAR, version, implemented ports, per-port: model string, serial, capacity, sector size, interface speed
- [ ] ATA: master/slave detection, model string (if IDE mode)
- [ ] Framebuffer: resolution, BPP, physical address, pitch, pixel format
- [ ] RTL8139: MAC address, link status, I/O base
- [ ] Network: IP address, subnet, gateway, DNS (if DHCP completed)
- [ ] Commit: `"debug: storage + display + network dump"`

---

## 4. Cross-Environment Logging

### 4.1 QEMU Serial Console

**Prompt:** QEMU's `-serial stdio` flag pipes COM1 output to the host terminal. This already works via `serial.c`. Ensure all QEMU launch scripts (`scripts/build.sh run`) include `-serial stdio`. Also ensure the X: partition is populated with numbered boot logs after each QEMU session. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean run`, and commit as `"debug: verify QEMU serial + X: log output"`. Add notes directly in this TODO section.

- [ ] Verify `scripts/build.sh run` includes `-serial stdio` flag
- [ ] Verify QEMU boot produces `BOOT_NNN.LOG` on X: partition
- [ ] Verify `HARDWARE.TXT` is written to X: in debug mode
- [ ] Test: `bash scripts/build.sh clean run` → serial output + X: log file
- [ ] Commit: `"debug: verify QEMU serial + X: log output"`

### 4.2 VirtualBox Serial Port

**Prompt:** VirtualBox can redirect COM1 to a host file via the serial port settings. The `run-vbox.ps1` script should configure this automatically. Ensure the X: partition is also populated with numbered boot logs. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run the VBox script, and commit as `"debug: VBox serial port + X: log output"`. Add notes directly in this TODO section.

- [ ] Verify `run-vbox.ps1` configures VBox serial port → host file (e.g., `serial.log`)
- [ ] Verify VBox boot produces `BOOT_NNN.LOG` on X: partition
- [ ] Add `--serial` flag to `run-vbox.ps1` to control serial output destination
- [ ] Commit: `"debug: VBox serial port + X: log output"`

### 4.3 Real Hardware (USB Boot)

**Prompt:** On real hardware booted from USB, there is no serial port output. The X: FAT32 partition is the only way to capture boot logs. The live flush mode (§1.3) is critical here — without it, a boot hang produces no logs. Ensure the USB write scripts create the boot.conf with debug settings, and that the X: partition is large enough for multiple boot logs. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, test on real hardware, and commit as `"debug: real hardware USB boot logging"`. Add notes directly in this TODO section.

- [ ] `write-usb.ps1` / `write-usb-debug.bat`: write `boot.conf` with `debug=1` for debug USB
- [ ] Normal USB: `boot.conf` with `debug=0` — logs still written but not live-flushed
- [ ] X: partition (16 MiB) can hold ~80 boot logs at ~200KB each
- [ ] On boot hang: `X:\BOOT_NNN.LOG` contains all entries up to the hang point
- [ ] `X:\HARDWARE.TXT` written once per debug boot — persistent across reboots
- [ ] Commit: `"debug: real hardware USB boot logging"`

---

## 5. Log Quality & Usefulness

### 5.1 Improve Log Content

**Prompt:** The current log output contains a lot of noise (test results, self-test pass/fail for every subsystem) and is missing critical information (exact timestamps in seconds.milliseconds, memory state, interrupt state). Audit every `klog()` call in the kernel and improve: remove redundant test output from production logs, add timing deltas for boot profiling, add memory watermark after each major allocation. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"klog: improve log content quality"`. Add notes directly in this TODO section.

- [ ] Convert timestamp format from PIT ticks to `[SSS.mmm]` (seconds.milliseconds)
- [ ] Add memory watermark after each major allocation: `[heap: 45% used, 912KB free]`
- [ ] Add phase delimiters in boot log: `═══ PHASE: Storage ═══`, `═══ PHASE: Network ═══`
- [ ] Self-tests: log `[OK]` on pass, full details only on `[FAIL]` — reduce noise
- [ ] Add interrupt state logging after each ISR setup: `IRQ0 → vector 32, PIT at 100Hz`
- [ ] Commit: `"klog: improve log content quality"`

### 5.2 Subsystem Log Level Control

**Prompt:** Add per-subsystem log level filtering so that verbose subsystems (like PCI scan, IXFS operations) can be silenced in production while remaining available in debug mode. This requires a subsystem registry in klog that maps subsystem names to minimum levels. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"klog: per-subsystem log level control"`. Add notes directly in this TODO section.

- [ ] Add `klog_set_subsystem_level(const char *subsystem, log_level_t min_level)`
- [ ] Default: all subsystems at `LOG_DEBUG`
- [ ] In debug mode: everything at `LOG_DEBUG`
- [ ] In production: noisy subsystems (e.g., `"pci"`, `"ixfs"`, `"test"`) at `LOG_INFO`
- [ ] Subsystem levels configurable via `boot.conf`: `log.pci=INFO`, `log.ixfs=WARN`
- [ ] Commit: `"klog: per-subsystem log level control"`

---

## 6. Cleanup & Technical Debt

### 6.1 Remove `grub.cfg` and GRUB References

> → See §2.3 above

### 6.2 Remove `DEBUG` File Mechanism

**Prompt:** After `boot.conf` (§2.1) is implemented, the `X:\DEBUG` file mechanism is obsolete. Remove all code that checks for this file. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"boot: remove DEBUG file mechanism"`. Add notes directly in this TODO section.

- [ ] Remove `X:\DEBUG` finddir check from `main.c`
- [ ] Remove `DEBUG` file creation from `write-usb.ps1`
- [ ] Remove `DEBUG` file injection from `run-vbox.ps1` and `run-windows.ps1`
- [ ] Delete `write-usb-debug.bat`, `run-vbox-debug.bat`, `run-windows-debug.bat` (use `boot.conf` instead)
- [ ] Commit: `"boot: remove DEBUG file mechanism"`

### 6.3 Remove Stale `serial.log` in Repository Root

**Prompt:** There is a `serial.log` file in the repository root (tracked by git). This is a build/test artifact and should not be in the repository. Remove it and add to `.gitignore`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, and commit as `"cleanup: remove stale serial.log from repo"`. Add notes directly in this TODO section.

- [ ] `git rm serial.log`
- [ ] Add `serial.log` to `.gitignore`
- [ ] Commit: `"cleanup: remove stale serial.log from repo"`

---

## Cross-References

| This TODO Section | Depends On | Other TODO File |
|-------------------|-----------|-----------------|
| §1.3 Live Flush Mode | §3.4.1 FAT32 Offset-Aware Write | `TODO-040-Filesystem.md` |
| §2.1 Boot Config File | §7.1 Boot Config File | `TODO-010-Bootloader.md` |
| §2.2 Verbose Mode | §7.1 Boot Config File | `TODO-010-Bootloader.md` |
| §2.3 Remove grub.cfg | §1 Custom UEFI Bootloader | `TODO-010-Bootloader.md` |
| §3.4 ACPI Tables | ACPI parser | `TODO-010-Bootloader.md §1.4` |
| §4.3 USB Logging | Partition layout | `TODO-060-Storage.md` (if exists) |
| §5.2 Subsystem Levels | Registry system | `TODO-030-Registry.md` (if exists) |

---

## Priority Order

| Priority | Section | Description |
|----------|---------|-------------|
| 🔴 P0 | 6.3 Remove stale `serial.log` | Quick cleanup — artifact in repo |
| 🔴 P0 | 2.3 Remove `grub.cfg` | Quick cleanup — dead file |
| 🟠 P1 | 1.1 Merge klog files | Foundation — unified logging |
| 🟠 P1 | 1.2 Numbered log files | Stop overwriting logs |
| 🟠 P1 | 1.3 Live flush mode | Critical for real hardware debugging |
| 🟠 P1 | 3.1 Structured hw dump | Extract from `klog_live.c` |
| 🟠 P1 | 3.3 PCI device dump | Full BARs, capabilities — driver dev |
| 🟠 P1 | 3.5 Storage/display/network | Complete hardware picture |
| 🟡 P2 | 2.1 Boot config file | Depends on bootloader §7.1 |
| 🟡 P2 | 2.2 Decouple splash | Depends on §2.1 |
| 🟡 P2 | 3.2 CPU CPUID dump | Nice to have — basic version exists |
| 🟡 P2 | 3.4 ACPI tables | Nice to have — for advanced drivers |
| 🟡 P2 | 5.1 Log content quality | Polish — reduce noise |
| 🟡 P2 | 5.2 Subsystem filtering | Advanced — depends on registry |
| 🟢 P3 | 4.1-4.3 Cross-env verification | Testing — verify all environments |
| 🟢 P3 | 6.2 Remove DEBUG file | After §2.1 is complete |

---

## OS Comparison

| Feature | Windows 11 | Linux | Impossible OS |
|---------|-----------|-------|---------------|
| Kernel log ring buffer | ✅ KD ring (64K) | ✅ dmesg (256K) | ✅ klog ring (128K) |
| Boot log to disk | ✅ `%windir%\Logs\CBS` | ✅ journald | ⬜ §1 — scattered, unreliable |
| Numbered/rotated logs | ✅ Automatic | ✅ logrotate | ⬜ §1.2 — **missing** |
| Live log flush (pre-hang) | ✅ ETW real-time | ✅ journald sync | ⬜ §1.3 — exists but fragile |
| Boot config file | ✅ BCD | ✅ cmdline / grub.cfg | ⬜ §2.1 — **missing** |
| Debug mode toggle | ✅ `bcdedit /debug` | ✅ `debug` cmdline | ⬜ §2.1 — uses file flag |
| Verbose boot | ✅ `bcdedit /bootlog` | ✅ `loglevel=7` | ⬜ §2.2 — tied to splash |
| Hardware info dump | ✅ Device Manager | ✅ `lspci -vvv` | ⬜ §3 — basic, unstructured |
| PCI full enumeration | ✅ Full BARs, caps | ✅ `lspci` | ⬜ §3.3 — vendor:device only |
| ACPI table dump | ✅ `acpidump` | ✅ `acpidump` | ⬜ §3.4 — **missing** |
| Per-subsystem filtering | ✅ ETW providers | ✅ `printk` levels | ⬜ §5.2 — **missing** |
| Serial console debug | ✅ Kernel debugger | ✅ serial console | ✅ COM1 via `klog.c` |
| Multi-env (QEMU/VBox/HW) | N/A | ✅ Works everywhere | ✅ §4 — serial + X: logs |

> **After P0+P1 items:** Impossible OS has a reliable, robust logging system that works across
> all environments and produces useful, numbered logs with hardware dumps.
> **After P2 items:** Matches Linux's logging capabilities and exceeds Windows in transparency.

---
