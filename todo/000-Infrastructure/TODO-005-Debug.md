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
> (requires a `DEBUG` file on the B: partition _after_ kernel boot), and missing features
> (no log rotation, no boot config, no structured hardware dump format).

> [!TIP]
> **The Black Box (`B:\`) — Impossible OS Flight Recorder**
>
> Every Impossible OS installation includes a dedicated 32 MiB FAT32 partition
> called the **Black Box** (volume label: `BLACKBOX`, drive letter: `B:\`).
> Like an aircraft flight recorder, it captures everything needed for
> post-crash analysis and driver development:
>
> ```
> B:\                              (32 MiB, FAT32, volume label: BLACKBOX)
> ├── Boot_2026-03-19_14-41_001.log    ← Boot log (human-readable, timestamped)
> ├── Boot_2026-03-19_08-30_001.log    ← Previous boot
> ├── Boot_2026-03-18_22-15_001.log    ← Day before
> ├── Crash_2026-03-19_14-42.bmp       ← Framebuffer screenshot at panic
> ├── Hardware_Report.txt              ← Comprehensive hardware dump (driver dev)
> ├── ACPI_Tables\                     ← Raw ACPI binary tables
> │   ├── MADT.bin
> │   ├── FADT.bin
> │   ├── DSDT.aml
> │   └── SSDT_0.aml
> ├── Timeline.bin                     ← Last 256 events (IRQs, faults, exceptions)
> └── boot.conf                        ← Boot configuration (debug=1, verbose=1)
> ```
>
> **Why a dedicated partition?**
> - **Transparent:** Shows up as a normal drive on Windows, Linux, macOS — no tools needed
> - **Accessible:** Users can email `Hardware_Report.txt` to the dev team
> - **Survivable:** Separate from IXFS — even if the OS filesystem is corrupted, logs survive
> - **Hidden in production:** Impossible OS hides `B:\` from Explorer (like Windows hides the EFI partition)
> - **Self-managing:** Automatic cleanup when disk space runs low

---

## Current Architecture Audit

### Files Involved

| File                              | Purpose                                                     | Issues                                                              |
| --------------------------------- | ----------------------------------------------------------- | ------------------------------------------------------------------- |
| `src/kernel/klog.c`               | Ring buffer (1000 entries), serial + framebuffer output      | Ring buffer too small for verbose debug; no filter by subsystem      |
| `src/kernel/klog_flush.c`         | Batch flush to `C:\` (IXFS) + `B:\` (FAT32 `serial.log`)   | FAT32 write is full-file overwrite; `serial.log` name loses history |
| `src/kernel/klog_live.c`          | Live per-entry flush to `B:\debug.log` + hardware dump      | Only active when DEBUG flag detected; duplicates klog_flush logic    |
| `src/kernel/drivers/serial.c`     | COM1 port I/O (0x3F8)                                       | Works in QEMU/VBox; no output on real hardware without serial port   |
| `include/kernel/klog.h`           | API declarations                                             | `klog_live_*` API mixed with core klog API                          |
| `src/kernel/boot_splash.c`        | `boot_splash_abort()` for debug mode                         | Disabling splash is separate concern from logging                    |
| `src/kernel/main.c`               | DEBUG flag detection, klog_flush calls, status messages       | DEBUG detection too late (after B: mount); flush calls scattered     |

### Output Paths

| Environment              | Serial (COM1) | `B:\` Boot Logs       | `C:\...\kernel.log` |
| ------------------------ | ------------- | --------------------- | -------------------- |
| QEMU (`-serial stdio`)   | ✅ Console    | ✅ After boot          | ✅ After boot         |
| VBox (serial→file)       | ✅ File       | ✅ After boot          | ✅ After boot         |
| Real hardware (USB)      | ❌ No serial  | ✅ If live flush on    | ❌ If hang            |

### Known Bugs & Design Issues

1. **`serial.log` always named `SERIAL.LOG`** — overwrites previous boot. Should use long filenames with date/time.
2. **DEBUG flag requires B: to be mounted** — can't enable debug before kernel starts. Should be in boot configuration (UEFI `boot.conf`). → XREF: `TODO-010-Bootloader.md §7.1 Boot Configuration File`
3. **`debug.log` and `serial.log` contain the same data** — confusing. Should be one canonical log file per boot.
4. **Splash screen abort tied to debug mode** — not needed if logging is robust. Debug mode should only control log verbosity, not UI.
5. **`grub.cfg` exists but is unused** — the OS uses a custom UEFI bootloader (`bootx64.c`). `grub.cfg` is a leftover from the GRUB era and should be removed. → XREF: `TODO-010-Bootloader.md §1`
6. **Hardware dump in `klog_live.c` is not structured** — plain text, not machine-parseable. Should use a consistent format for driver development.
7. **Ring buffer (1000 entries) wraps** — early boot entries lost during verbose boots. No persistent early-boot capture.
8. **No log level filtering on disk** — everything goes to every log. Should have separate debug vs release log levels.
9. **FAT32 full-file overwrite** — every `klog_live_flush()` rewrites the entire file. Becomes slow as log grows.
10. **Reentrancy guard in `klog_live.c`** is a workaround — proper design should avoid recursive logging in VFS.
11. **No disk space management** — B: partition can fill up with no warning or cleanup.
12. **No crash screenshot** — on panic, the framebuffer state is lost when the machine reboots.

---

## 1. Unified Log File System ✅ → Refactor

### 1.1 Merge `klog_flush.c` and `klog_live.c` into single `klog_disk.c`

**Prompt — VERIFICATION:** Verify the klog disk merge is correct and complete. Confirm `klog_disk.c` compiles and provides `klog_disk_init()`, `klog_disk_flush()`, `klog_disk_set_live()`, `klog_disk_live_active()`, `klog_disk_append()`. Confirm `hw_dump.c` provides `hw_dump_to_log()` and uses `klog()` for output. Confirm `klog_flush.c` and `klog_live.c` are deleted. Confirm `klog.h` exports the new API. Confirm all callers in `klog.c` and `main.c` use the new function names. Run `bash scripts/build.sh clean` → `=== BUILD OK ===`. Verify commit `"klog: merge flush + live into unified klog_disk.c"`.

- [x] Create `src/kernel/klog_disk.c` — replaces both `klog_flush.c` and `klog_live.c`
- [x] Delete `src/kernel/klog_flush.c` and `src/kernel/klog_live.c`
- [x] Single API: `klog_disk_init()`, `klog_disk_flush()`, `klog_disk_set_live(int on)`
- [x] Write to `C:\Impossible\System\Logs\kernel.log` (IXFS, appendable)
- [x] Write to `B:\Boot_YYYY-MM-DD_HH-MM_NNN.log` (FAT32, long filenames)
- [x] Move hardware dump to separate `hw_dump.c` (see §3)
- [x] Commit: `"klog: merge flush + live into unified klog_disk.c"`

### 1.2 Long Filename Boot Logs on B: (FAT32)

**Prompt — VERIFICATION:** Verify date-stamped boot log files work correctly with long filenames. Confirm `pick_log_name()` reads the RTC date and time, builds `Boot_YYYY-MM-DD_HH-MM_NNN.log` filenames, and performs automatic cleanup when disk space is low. Run `bash scripts/build.sh clean` → `=== BUILD OK ===`. Verify commit `"klog: long filename boot logs on Black Box"`.

> [!NOTE]
> **Long filename format:** `Boot_2026-03-19_14-41_001.log`
> - `Boot_` prefix — identifies as a boot log
> - `2026-03-19` — ISO date from RTC
> - `14-41` — time (24h) from RTC
> - `_001` — sequence number (for multiple boots at same minute)
> - Human-readable at a glance in any file manager

- [x] On `klog_disk_init()`, scan `B:\` for `Boot_*.log` files via `vfs_readdir`
- [x] Build filename from RTC: `Boot_YYYY-MM-DD_HH-MM_NNN.log`
- [x] If identical timestamp exists, increment sequence number `_002`, `_003`, etc.
- [x] Log filename in serial output: `klog: writing to B:\Boot_2026-03-19_14-41_001.log`
- [x] Commit: `"klog: long filename boot logs on Black Box"`

### 1.3 Live Flush Mode (Per-Entry Write)

> [!IMPORTANT]
> → XREF: `TODO-040-Filesystem.md §3.4.1 Offset-Aware Write (Append Support)` — **complete this
> FIRST.** Without FAT32 append support, every live flush rewrites the entire growing log file
> from scratch (full-file overwrite). After §3.4.1 is done, live flush becomes a simple append
> of ~100 bytes instead of a rewrite of the entire buffer. This is the #1 performance bottleneck.

**Prompt:** When debug mode is active, every `klog()` entry must be flushed to disk immediately so that even a boot hang leaves the last log line on disk. The current implementation in `klog_live.c` rewrites the entire file on every entry (FAT32 full-file overwrite limitation). Optimise this: use a PMM-backed growing buffer, and only flush every N entries or at explicit flush points. Since FAT32 can't append, each flush must rewrite the entire file — but batching reduces I/O cost. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"klog: optimized live flush with batching"`. Add notes directly in this TODO section.

- [ ] Live mode: flush every 5 entries (configurable) or at explicit `klog_disk_flush()` calls
- [ ] `boot_progress()` calls trigger immediate flush (critical checkpoints)
- [ ] Reentrancy guard to prevent recursive flush (VFS operations may trigger klog)
- [ ] PMM buffer: 256KB, grows monotonically, full-file rewrite on each flush
- [ ] Log flush statistics: `klog: live flush #42, 12847 bytes, 3ms`
- [ ] Commit: `"klog: optimized live flush with batching"`

### 1.4 Black Box Disk Space Management *(agent)*

**Prompt:** The Black Box partition is 32 MiB. Without management, it will fill up and logging will silently fail. Implement automatic cleanup and a boot warning when space is low. On every `klog_disk_init()`, check free space. If below 4 MiB, delete the oldest boot logs (by filename date) until 8 MiB is free. If below 2 MiB, show a warning during boot: `[WARN] Black Box nearly full — oldest logs deleted`. The warning should appear both on serial and in the boot splash status text. Never delete `Hardware_Report.txt`, `boot.conf`, or `ACPI_Tables\` — only `Boot_*.log` and `Crash_*.bmp` files. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"blackbox: automatic disk space management"`. Add notes directly in this TODO section.

- [ ] On `klog_disk_init()`: call `vfs_statfs("B:")` → check free bytes
- [ ] If free < 4 MiB: enter cleanup mode
  - [ ] Scan `B:\Boot_*.log` files, sort by filename (oldest first = earliest date)
  - [ ] Delete oldest logs until free ≥ 8 MiB (or no more logs to delete)
  - [ ] Log: `blackbox: cleanup — deleted 12 old boot logs, recovered 6.2 MiB`
- [ ] If free < 2 MiB after cleanup: show warning
  - [ ] Serial: `[WARN] Black Box nearly full — 1.8 MiB free`
  - [ ] Boot splash: `⚠ Black Box storage low — oldest logs deleted`
  - [ ] Never delete protected files: `Hardware_Report.txt`, `boot.conf`, `ACPI_Tables\*`, `Timeline.bin`
- [ ] Track partition stats in boot log header:
  ```
  ═══ BLACK BOX ═══
  Partition: B:\ (BLACKBOX) 32 MiB FAT32
  Free space: 24.3 MiB (76%)
  Boot logs:  14 files, 7.7 MiB total
  Oldest log: Boot_2026-03-05_09-12_001.log
  ```
- [ ] Commit: `"blackbox: automatic disk space management"`

### 1.5 Black Box Partition Hiding *(agent)*

**Prompt:** In production, the Black Box partition should be hidden from the Impossible OS file manager and desktop — just like Windows hides the EFI System Partition from Explorer. The partition is still accessible programmatically via `B:\` and from the command line, but it doesn't appear in the sidebar, drive list, or desktop icons. A "Show hidden partitions" toggle in Control Panel → System → Storage reveals it. Advanced users and developers can always access it. On external machines (Windows, Linux), it appears as a normal FAT32 volume with label `BLACKBOX`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"blackbox: hide partition from file manager"`. Add notes directly in this TODO section.

- [ ] Add `GPT_ATTR_HIDDEN` flag to the Black Box partition's GPT entry
  - [ ] Bit 62 (OS-specific) in GPT attributes — custom "hidden from UI" flag
- [ ] File manager: skip drives with `GPT_ATTR_HIDDEN` in sidebar listing
- [ ] Desktop: skip hidden drives in "This PC" / drive icon view
- [ ] Shell: `B:\` still accessible via `dir B:\`, `type B:\Hardware_Report.txt`
- [ ] Control Panel → System → Storage → "Show hidden partitions" checkbox
  - [ ] → XREF: `TODO-050-Registry.md` — store setting in `HKLM\SYSTEM\Explorer\ShowHidden`
- [ ] `write-usb.ps1`: set hidden flag when creating GPT layout
- [ ] External access: Windows/Linux mount as normal `BLACKBOX` volume (GPT bit is OS-specific)
- [ ] Commit: `"blackbox: hide partition from file manager"`

---

## 2. Debug Mode Activation

### 2.1 Boot Configuration File (`boot.conf`)

**Prompt:** The current debug mode requires a `DEBUG` file on the B: partition, which can only be detected _after_ the kernel mounts B: — too late for early boot debugging. Move debug mode activation to the UEFI bootloader via a `boot.conf` file on the EFI partition. The bootloader reads `boot.conf` before `ExitBootServices()` and passes a `debug=1` flag in the boot params struct. The kernel reads this flag immediately in `kernel_main()` — before any filesystem is mounted. This enables debug logging from the very first instruction. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"boot: debug mode via boot.conf"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-010-Bootloader.md §7.1 Boot Configuration File` — this section depends on
> the boot.conf parser being implemented in `bootx64.c`. Coordinate with that TODO item.

- [ ] Add `debug=0|1` key to `boot.conf` format (§7.1 of Bootloader TODO) — **default: `debug=1`** (always on during development)
- [ ] Parse in `bootx64.c`: `boot_params.debug_mode = (debug == 1)`
- [ ] Add `uint8_t debug_mode` field to `struct boot_info` / `boot_params`
- [ ] In `kernel_main()`: check `g_boot_info.debug_mode` immediately after boot info parsing
- [ ] If debug mode: `klog_set_screen_level(LOG_DEBUG)` — show all log levels on screen
- [ ] If debug mode: enable live disk flush as soon as B: is mounted
- [ ] Remove `B:\DEBUG` file detection from `main.c` (replaced by `boot.conf`)
- [x] ~~Remove `-DebugBoot` switch from `run-windows.ps1` and `write-usb.ps1`~~ ✅ Done
- [x] ~~Delete `run-windows-DebugBoot.bat` and `write-usb-DebugBoot.bat`~~ ✅ Done
- [x] ~~Remove DEBUG flag injection code from `run-windows.ps1` and `write-usb.ps1`~~ ✅ Done
- [ ] Commit: `"boot: debug mode via boot.conf (default=1)"`

### 2.2 Remove Splash Screen Abort from Debug Mode

> [!IMPORTANT]
> → XREF: `TODO-010-Bootloader.md §7.1 Boot Configuration File` — the `verbose=1`
> option below requires `boot.conf` parsing in the bootloader. Complete §2.1 first.

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

## 3. Comprehensive Hardware Report (Black Box Driver Development Kit) *(agent)*

> [!TIP]
> **Use case:** A user boots Impossible OS from USB on their laptop. It works but
> the WiFi card isn't supported. They email us `B:\Hardware_Report.txt` and the
> `B:\ACPI_Tables\` folder. From that single report, we can write a driver for
> their exact hardware — PCI IDs, BARs, capabilities, ACPI methods, USB topology,
> SMBIOS model, and all. **No other consumer OS produces a single file this comprehensive.**

### 3.1 Structured Hardware Dump (`hw_dump.c`)

**Prompt:** The current hardware dump in `klog_live.c` is embedded in the live logging code and writes unstructured plain text. Extract it into a standalone `hw_dump.c` that produces a structured, machine-parseable hardware report. This report is critical for driver development — it should contain everything needed to write a driver for any detected device. The report is written to `B:\Hardware_Report.txt` on every boot (overwritten each time — it's always current). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"debug: structured hardware dump in hw_dump.c"`. Add notes directly in this TODO section.

- [ ] Create `src/kernel/hw_dump.c` and `include/kernel/hw_dump.h`
- [ ] Move hardware dump logic from `klog_live.c` to `hw_dump.c`
- [ ] `hw_dump_to_log()` — writes hardware info via `klog()` (appears in all logs)
- [ ] `hw_dump_to_file(const char *path)` — writes standalone `B:\Hardware_Report.txt`
- [ ] Report header — identifies the machine:
  ```
  ╔══════════════════════════════════════════════════╗
  ║  IMPOSSIBLE OS — HARDWARE REPORT                ║
  ║  Generated: 2026-03-19 14:41:22 UTC             ║
  ║  OS Version: 0.1.0-dev (build 2026.03.19)       ║
  ║  Device Fingerprint: a3f7c2d1                    ║
  ╚══════════════════════════════════════════════════╝
  ```
- [ ] Commit: `"debug: structured hardware dump in hw_dump.c"`

### 3.2 Device Fingerprint

**Prompt:** Generate a 32-bit hash that uniquely identifies the hardware configuration. This fingerprint stays the same across reboots on the same machine but differs between machines. It's used to track reports from the same hardware. Hash inputs: CPU vendor+family+model+stepping, PCI device list (vendor:device pairs sorted), memory size, SMBIOS board product. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"debug: device fingerprint hash"`. Add notes directly in this TODO section.

- [ ] Collect hash inputs: CPU signature, sorted PCI IDs, RAM size, SMBIOS board
- [ ] CRC32 or FNV-1a hash → 8-char hex string (e.g., `a3f7c2d1`)
- [ ] Include in Hardware Report header and every boot log header
- [ ] Store in Registry: `HKLM\HARDWARE\Description\Fingerprint`
- [ ] Commit: `"debug: device fingerprint hash"`

### 3.3 CPU Information (CPUID)

**Prompt:** Dump complete CPU identification for driver development: vendor string, brand string, family/model/stepping, all feature flags (SSE, AVX, AES, etc.), cache topology, and core/thread count from CPUID. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"debug: CPU CPUID dump"`. Add notes directly in this TODO section.

- [ ] CPUID leaf 0: vendor string (GenuineIntel / AuthenticAMD)
- [ ] CPUID leaf 0x80000002-0x80000004: brand string (e.g., "Intel Core i5-4210U")
- [ ] CPUID leaf 1: family, model, stepping, feature flags (EDX + ECX)
- [ ] CPUID leaf 7: extended features (AVX2, BMI1/2, RDSEED, ADX, SHA)
- [ ] CPUID leaf 4: cache topology (L1/L2/L3 size and associativity)
- [ ] CPUID leaf 0xB: core/thread topology (x2APIC)
- [ ] TSC frequency (if calibrated)
- [ ] Commit: `"debug: CPU CPUID dump"`

### 3.4 PCI Device Enumeration

**Prompt:** Dump every PCI device with full identification data needed for driver development. The current PCI scan in `pci.c` only logs vendor:device and class name. The hardware dump should include: all 6 BARs (with memory vs I/O type), subsystem vendor/device IDs, revision ID, interrupt pin/line, capabilities pointer, and MSI/MSI-X capability if present. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"debug: full PCI device dump"`. Add notes directly in this TODO section.

- [ ] Bus:Dev.Func, Vendor:Device, Subsystem Vendor:Device
- [ ] Class:Subclass:ProgIF, Revision ID
- [ ] All 6 BARs with size detection (write all 1s, read back, mask type bits)
- [ ] BAR type: memory (32/64-bit, prefetchable) vs I/O
- [ ] Interrupt line, interrupt pin
- [ ] Header type (normal, bridge, cardbus)
- [ ] Capabilities list walk (power management, MSI, MSI-X, PCI Express)
- [ ] PCIe: link speed, link width, max payload, device capabilities
- [ ] Commit: `"debug: full PCI device dump"`

### 3.5 ACPI Tables + Binary Dump

> [!IMPORTANT]
> → XREF: `TODO-010-Bootloader.md §1.4` — ACPI RSDP discovery happens in the bootloader.
> The ACPI parser must be in place before table-level dumping can work.

> [!IMPORTANT]
> → XREF: `TODO-063-Drivers.md §2.2` — The MADT `PCAT_COMPAT` flag (bit 0 at offset 36)
> indicates whether the 8259 PIC is present. On Hyper-V Gen 2, this flag is **cleared to 0**.
> The hardware dump must log this flag so developers can instantly see whether PIC init
> should be skipped. This is critical for debugging boot failures on legacy-free platforms.

**Prompt:** Dump ACPI table signatures and addresses for driver development: RSDP version, RSDT/XSDT address, MADT (LAPIC entries, IOAPIC entries, **PCAT_COMPAT flag**), FADT (PM timer, SCI interrupt), MCFG (PCIe ECAM base), HPET, DSDT/SSDT pointers. Additionally, save raw ACPI binary tables to `B:\ACPI_Tables\` so they can be decompiled with `iasl` on another machine — this is essential for writing ACPI-dependent drivers (power management, thermal, battery, embedded controllers). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"debug: ACPI table dump + binary export"`. Add notes directly in this TODO section.

- [ ] RSDP: version, RSDT/XSDT physical address
- [ ] Walk RSDT/XSDT: list all table signatures with physical addresses
- [ ] MADT: **PCAT_COMPAT flag** (bit 0), LAPIC entries (APIC ID, CPU ID, flags), IOAPIC entries (base, GSI base)
- [ ] FADT: PM1a/PM1b port, SCI interrupt, century register, boot flags
- [ ] MCFG: PCIe ECAM base address, bus range (for future PCIe MMIO config access)
- [ ] HPET: base address, timer count (for future high-precision timer)
- [ ] **Binary table export:** Save each ACPI table as a raw file in `B:\ACPI_Tables\`:
  - [ ] `MADT.bin`, `FADT.bin`, `MCFG.bin`, `HPET.bin`
  - [ ] `DSDT.aml` — the main AML bytecode (decompile with `iasl -d DSDT.aml`)
  - [ ] `SSDT_0.aml`, `SSDT_1.aml`, ... — supplemental AML
  - [ ] Create `B:\ACPI_Tables\` directory on first dump
- [ ] Commit: `"debug: ACPI table dump + binary export"`

### 3.6 SMBIOS / DMI Tables *(agent)*

**Prompt:** Dump SMBIOS (System Management BIOS) tables for machine identification. This is the data Windows shows in System Information: motherboard manufacturer, model, BIOS version, serial numbers, memory module details. The SMBIOS entry point is found either via UEFI configuration table (SMBIOS3_TABLE_GUID) or by scanning 0xF0000-0xFFFFF. Parse Type 0 (BIOS Info), Type 1 (System Info), Type 2 (Board Info), Type 4 (Processor), Type 16/17 (Memory Array/Device). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"debug: SMBIOS/DMI table dump"`. Add notes directly in this TODO section.

- [ ] Find SMBIOS entry point via UEFI config table (`SMBIOS3_TABLE_GUID`)
  - [ ] Pass SMBIOS base address in `boot_info` from bootloader
- [ ] Parse Type 0 — BIOS Information:
  - [ ] Vendor, version, release date, BIOS ROM size
- [ ] Parse Type 1 — System Information:
  - [ ] Manufacturer, product name, version, serial number, UUID
- [ ] Parse Type 2 — Baseboard Information:
  - [ ] Manufacturer, product, version, serial number
- [ ] Parse Type 4 — Processor Information:
  - [ ] Socket designation, manufacturer, version, max speed, core count
- [ ] Parse Type 16 + Type 17 — Memory:
  - [ ] Memory array: max capacity, number of slots
  - [ ] Per-DIMM: size, speed, manufacturer, part number, form factor
- [ ] Format in report:
  ```
  ═══ SYSTEM INFORMATION ═══
  Manufacturer:  Dell Inc.
  Product:       XPS 15 9520
  BIOS:          1.14.0 (2024-09-15)
  Board:         Dell 0W2X3N
  Serial:        ABC123XYZ
  UUID:          4C4C4544-0042-4310-8042-B2C04F335831
  Memory:        2 × 8 GB DDR5-4800 (Samsung M425R1GB4BB0-CQKOL)
  ```
- [ ] Commit: `"debug: SMBIOS/DMI table dump"`

### 3.7 Storage, Display, Network

**Prompt:** Dump storage controller details (AHCI ports, drive model strings, capacity, sector size), framebuffer configuration (resolution, BPP, physical address, pitch), and network interface details (NIC type, MAC address, link status). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"debug: storage + display + network dump"`. Add notes directly in this TODO section.

- [ ] AHCI: controller PCI address, ABAR, version, implemented ports, per-port: model string, serial, capacity, sector size, interface speed
- [ ] NVMe: controller PCI address, model, serial, capacity, namespace count (for future NVMe support)
- [ ] Framebuffer: resolution, BPP, physical address, pitch, pixel format, all available GOP modes
- [ ] RTL8139: MAC address, link status, I/O base
- [ ] Network: IP address, subnet, gateway, DNS (if DHCP completed)
- [ ] Audio: HDA controller PCI address, codec vendor/device IDs, widget tree summary
- [ ] Commit: `"debug: storage + display + network dump"`

### 3.8 USB Descriptor Dump *(agent)*

**Prompt:** For USB driver development, dump the complete USB topology: root hubs, connected devices, and their descriptors. Each device needs: vendor/product ID, device class, speed, configuration descriptors, interface descriptors, and endpoint descriptors. This is the equivalent of `lsusb -v` on Linux. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"debug: USB descriptor dump"`. Add notes directly in this TODO section.

> [!NOTE]
> → XREF: This requires XHCI/EHCI host controller driver support. May need to
> be added iteratively as USB support is implemented. Initial version can dump
> PCI-level HCI information (XHCI/EHCI/UHCI base addresses) even without a
> full USB stack.

- [ ] xHCI/EHCI/UHCI: controller PCI address, BAR, version, port count
- [ ] Root hub: number of ports, per-port: connection status, speed, device present
- [ ] Per connected device: vendor:product ID, device class/subclass/protocol
- [ ] Device descriptor: USB version, max packet size, manufacturer string, product string
- [ ] Configuration descriptors: power requirements, number of interfaces
- [ ] Interface descriptors: class, subclass, protocol, endpoint count
- [ ] Endpoint descriptors: address, type (bulk/interrupt/iso), max packet size, interval
- [ ] Format in report (similar to `lsusb -t` + `lsusb -v`)
- [ ] Commit: `"debug: USB descriptor dump"`

### 3.9 Hypervisor Detection *(agent)*

**Prompt:** Detect and report the hypervisor environment. Many users will run Impossible OS in VMs — Hyper-V, VirtualBox, VMware, QEMU/KVM. The hardware report should identify the hypervisor (if any) and list available paravirtual features. This helps diagnose VM-specific bugs. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"debug: hypervisor detection dump"`. Add notes directly in this TODO section.

- [ ] CPUID leaf 0x40000000: hypervisor vendor string
  - [ ] `"Microsoft Hv"` → Hyper-V (list generation, enlightenments)
  - [ ] `"KVMKVMKVM"` → KVM
  - [ ] `"VMwareVMware"` → VMware
  - [ ] `"VBoxVBoxVBox"` → VirtualBox
  - [ ] `"TCGTCGTCGTCG"` → QEMU TCG
- [ ] Hyper-V specific: generation, synthetic devices available, enlightenments supported
- [ ] VMware: backdoor port availability, virtual hardware version
- [ ] Report "bare metal" if CPUID bit 31 of ECX (leaf 1) is clear
- [ ] Commit: `"debug: hypervisor detection dump"`

---

## 4. Crash Forensics *(agent)*

### 4.1 Crash Screenshot *(agent)*

**Prompt:** When the kernel panics, capture the framebuffer contents as a BMP image and save it to the Black Box partition. This preserves the exact screen state — the BSOD with error info, registers, and any boot progress bars (from §TODO-010.98 Kernel Heartbeat). The user doesn't need to photograph the screen with their phone. File format: uncompressed 24-bit BMP (simplest to implement — no compression needed, BMP header is 54 bytes). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"panic: crash screenshot to Black Box"`. Add notes directly in this TODO section.

> [!TIP]
> **No other consumer OS does this.** Windows and Linux write minidumps (binary,
> requires tools to read). A BMP screenshot is instantly viewable on any OS —
> open the file, see exactly what was on screen at crash time.

- [ ] In `panic_screen()` — after rendering the BSOD, before the countdown:
  - [ ] Capture framebuffer to BMP (raw pixel copy + BMP header)
  - [ ] Filename: `B:\Crash_YYYY-MM-DD_HH-MM.bmp` (from RTC)
  - [ ] BMP format: 54-byte header + raw BGR pixel data (bottom-up scanlines)
  - [ ] For 1280×720×32bpp: ~3.6 MiB per screenshot
- [ ] If B: is not mounted (crash before filesystem init):
  - [ ] Store framebuffer address + size in a well-known physical memory location
  - [ ] On next boot: check for pending crash screenshot, write it to B:
- [ ] Delete old crash screenshots during cleanup (§1.4) — keep last 3
- [ ] Log: `panic: screenshot saved to B:\Crash_2026-03-19_14-42.bmp`
- [ ] Commit: `"panic: crash screenshot to Black Box"`

### 4.2 Event Timeline Ring Buffer *(agent)*

**Prompt:** Maintain a kernel-level ring buffer of the last 256 significant events: IRQs received, page faults, exceptions, timer ticks, scheduler context switches. On panic, flush this buffer to `B:\Timeline.bin`. This gives millisecond-level insight into what the kernel was doing right before the crash — far more detail than the boot progress history (which only tracks init function calls). Think of it as a kernel-level "dashcam." After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"debug: event timeline ring buffer"`. Add notes directly in this TODO section.

- [ ] Define event entry (16 bytes each, 256 entries = 4 KiB buffer):
  ```c
  struct timeline_event {
      uint32_t timestamp_ticks;  /* PIT/TSC ticks         */
      uint16_t event_type;       /* IRQ, PF, EXC, SCHED   */
      uint16_t data;             /* vector, fault address  */
      uint64_t extra;            /* RIP, CR2, or context   */
  };
  ```
- [ ] Record in ISR-safe locations: `irq_dispatch()`, `page_fault_handler()`, `scheduler_switch()`
- [ ] Atomic write via per-CPU index — no locks needed (single producer per CPU)
- [ ] On panic: `timeline_flush("B:\\Timeline.bin")` — raw binary dump
- [ ] Include event type legend in `Hardware_Report.txt` footer
- [ ] Commit: `"debug: event timeline ring buffer"`

### 4.3 Boot Regression Detection *(agent)*

**Prompt:** Compare boot times across sessions to detect regressions. On each boot, record total boot time and per-phase times. On the next boot, compare with the previous session's times. If any phase is >3× slower, log a warning: `[WARN] AHCI init took 340ms (was 45ms) — possible regression`. This catches performance problems early, before they accumulate. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"debug: boot regression detection"`. Add notes directly in this TODO section.

> [!NOTE]
> → XREF: `TODO-010.98-Kernel-Heartbeat.md §1` — boot_progress() records per-stage timestamps
> → XREF: `TODO-005-Debug.md §7` — boot time profiler provides the timing data

- [ ] On B:, maintain `Boot_Times.dat` — binary file: last 10 boot time records
- [ ] Each record: `{ total_ms, per_phase_ms[16], timestamp }` (fixed-size struct)
- [ ] On boot complete: compare current times with average of last 5 boots
- [ ] If any phase is >3× average: serial warning + boot log warning
- [ ] If total boot time is >2× average: show warning on splash screen
- [ ] Commit: `"debug: boot regression detection"`

---

## 5. Cross-Environment Logging

### 5.1 QEMU Serial Console

**Prompt:** QEMU's `-serial stdio` flag pipes COM1 output to the host terminal. This already works via `serial.c`. Ensure all QEMU launch scripts (`scripts/build.sh run`) include `-serial stdio`. Also ensure the B: partition is populated with boot logs after each QEMU session. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean run`, and commit as `"debug: verify QEMU serial + B: log output"`. Add notes directly in this TODO section.

- [ ] Verify `scripts/build.sh run` includes `-serial stdio` flag
- [ ] Verify QEMU boot produces `Boot_YYYY-MM-DD_HH-MM_NNN.log` on B:
- [ ] Verify `Hardware_Report.txt` is written to B: in debug mode
- [ ] Test: `bash scripts/build.sh clean run` → serial output + B: log file
- [ ] Commit: `"debug: verify QEMU serial + B: log output"`

### 5.2 VirtualBox Serial Port

**Prompt:** VirtualBox can redirect COM1 to a host file via the serial port settings. The `run-vbox.ps1` script should configure this automatically. Ensure the B: partition is also populated with boot logs. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run the VBox script, and commit as `"debug: VBox serial port + B: log output"`. Add notes directly in this TODO section.

- [ ] Verify `run-vbox.ps1` configures VBox serial port → host file (e.g., `serial.log`)
- [ ] Verify VBox boot produces `Boot_YYYY-MM-DD_HH-MM_NNN.log` on B:
- [ ] Add `--serial` flag to `run-vbox.ps1` to control serial output destination
- [ ] Commit: `"debug: VBox serial port + B: log output"`

### 5.3 Real Hardware (USB Boot)

> [!IMPORTANT]
> → XREF: `TODO-040-Filesystem.md §3.4.1` — FAT32 append support is critical here.
> Without it, live flush rewrites the entire log on every entry, which on slow USB
> hardware can cause visible stalls during boot.

**Prompt:** On real hardware booted from USB, there is no serial port output. The Black Box `B:\` partition is the only way to capture boot logs. The live flush mode (§1.3) is critical here — without it, a boot hang produces no logs. Ensure the USB write scripts create the boot.conf with debug settings. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, test on real hardware, and commit as `"debug: real hardware USB boot logging"`. Add notes directly in this TODO section.

> [!TIP]
> **Reading B: from Windows:** The Black Box partition shows up as a normal FAT32
> volume in Windows Explorer (volume label: `BLACKBOX`). Just plug in the USB drive
> and open it — no `mountvol` or `diskpart` needed. The partition is transparent
> and accessible on Windows, Linux, and macOS without any special tools.

- [ ] `write-usb.ps1`: create B: partition (32 MiB, FAT32, label `BLACKBOX`)
- [ ] `write-usb.ps1`: write `boot.conf` with `debug=1` for debug USB
- [ ] Normal USB: `boot.conf` with `debug=0` — logs still written but not live-flushed
- [ ] B: partition (32 MiB) can hold ~80 boot logs + hardware report + ACPI tables
- [ ] On boot hang: `B:\Boot_YYYY-MM-DD_HH-MM_NNN.log` contains all entries up to hang
- [ ] `B:\Hardware_Report.txt` written on every boot — always current
- [ ] Commit: `"debug: real hardware USB boot logging"`

---

## 6. Log Quality & Usefulness

### 6.1 Improve Log Content

**Prompt:** The current log output contains a lot of noise (test results, self-test pass/fail for every subsystem) and is missing critical information (exact timestamps in seconds.milliseconds, memory state, interrupt state). Audit every `klog()` call in the kernel and improve: remove redundant test output from production logs, add timing deltas for boot profiling, add memory watermark after each major allocation. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"klog: improve log content quality"`. Add notes directly in this TODO section.

- [ ] Convert timestamp format from PIT ticks to `[SSS.mmm]` (seconds.milliseconds)
- [ ] Add memory watermark after each major allocation: `[heap: 45% used, 912KB free]`
- [ ] Add phase delimiters in boot log: `═══ PHASE: Storage ═══`, `═══ PHASE: Network ═══`
- [ ] Self-tests: log `[OK]` on pass, full details only on `[FAIL]` — reduce noise
- [ ] Add interrupt state logging after each ISR setup: `IRQ0 → vector 32, PIT at 100Hz`
- [ ] Commit: `"klog: improve log content quality"`

### 6.2 Subsystem Log Level Control

> [!IMPORTANT]
> → XREF: `TODO-050-Registry.md` — subsystem levels can optionally be persisted in the
> Registry under `HKLM\SYSTEM\Debug\LogLevels\{subsystem}`. Not a hard dependency, but
> enables runtime reconfiguration without editing `boot.conf`.

**Prompt:** Add per-subsystem log level filtering so that verbose subsystems (like PCI scan, IXFS operations) can be silenced in production while remaining available in debug mode. This requires a subsystem registry in klog that maps subsystem names to minimum levels. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"klog: per-subsystem log level control"`. Add notes directly in this TODO section.

- [ ] Add `klog_set_subsystem_level(const char *subsystem, log_level_t min_level)`
- [ ] Default: all subsystems at `LOG_DEBUG`
- [ ] In debug mode: everything at `LOG_DEBUG`
- [ ] In production: noisy subsystems (e.g., `"pci"`, `"ixfs"`, `"test"`) at `LOG_INFO`
- [ ] Subsystem levels configurable via `boot.conf`: `log.pci=INFO`, `log.ixfs=WARN`
- [ ] Commit: `"klog: per-subsystem log level control"`

---

## 7. Cleanup & Technical Debt

### 7.1 Remove `grub.cfg` and GRUB References

> → See §2.3 above

### 7.2 Remove `DEBUG` File Mechanism

**Prompt:** After `boot.conf` (§2.1) is implemented, the `B:\DEBUG` file mechanism is obsolete. Remove all code that checks for this file. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"boot: remove DEBUG file mechanism"`. Add notes directly in this TODO section.

- [ ] Remove `DEBUG` finddir check from `main.c`
- [ ] Remove `DEBUG` file creation from `write-usb.ps1`
- [ ] Remove `DEBUG` file injection from `run-vbox.ps1` and `run-windows.ps1`
- [ ] Delete `write-usb-debug.bat`, `run-vbox-debug.bat`, `run-windows-debug.bat` (use `boot.conf` instead)
- [ ] Commit: `"boot: remove DEBUG file mechanism"`

### 7.3 Remove Stale `serial.log` in Repository Root

**Prompt:** There is a `serial.log` file in the repository root (tracked by git). This is a build/test artifact and should not be in the repository. Remove it and add to `.gitignore`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, and commit as `"cleanup: remove stale serial.log from repo"`. Add notes directly in this TODO section.

- [ ] `git rm serial.log`
- [ ] Add `serial.log` to `.gitignore`
- [ ] Commit: `"cleanup: remove stale serial.log from repo"`

---

## 8. Boot Time Profiler

**Prompt:** Track how long each boot stage takes using PIT ticks (or TSC if calibrated). At the start and end of each major init function, record the elapsed time. Print a summary to serial at boot completion: "PMM: 12ms, ACPI: 45ms, Drivers: 230ms, FS: 85ms, Desktop: 150ms — Total: 522ms". Store the boot time in Registry `HKLM\SYSTEM\Boot\LastBootTime` for display in the Settings → System applet. This is essential for identifying and fixing boot regressions. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: boot time profiler"`. Add notes directly in this TODO section.

> **Production requirement:** Windows has Boot Event Collector and Event Viewer boot
> timing. Linux has `systemd-analyze blame`. Impossible OS needs boot stage timing
> to identify and fix performance regressions.

- [ ] Define boot stages: PMM, ACPI, Drivers, Filesystem, Registry, Desktop
- [ ] `boot_timer_start(stage)` / `boot_timer_end(stage)` — record PIT tick delta
- [ ] Print boot time summary to serial on boot completion
- [ ] Store total boot time in Registry: `HKLM\SYSTEM\Boot\LastBootTime` (milliseconds)
- [ ] Shell command: `boottime` — display last boot stage breakdown
- [ ] Commit: `"kernel: boot time profiler"`

---

## Cross-References

| This TODO Section       | Depends On                       | Other TODO File                    |
| ----------------------- | -------------------------------- | ---------------------------------- |
| §1.3 Live Flush Mode    | §3.4.1 FAT32 Offset-Aware Write | `TODO-040-Filesystem.md`           |
| §2.1 Boot Config File   | §7.1 Boot Config File           | `TODO-010-Bootloader.md`           |
| §2.2 Verbose Mode       | §7.1 Boot Config File           | `TODO-010-Bootloader.md`           |
| §2.3 Remove grub.cfg    | §1 Custom UEFI Bootloader       | `TODO-010-Bootloader.md`           |
| §3.5 ACPI Tables        | ACPI parser                     | `TODO-010-Bootloader.md §1.4`     |
| §3.6 SMBIOS             | SMBIOS base from bootloader     | `TODO-010-Bootloader.md`           |
| §4.1 Crash Screenshot   | Panic screen                    | `TODO-010-Bootloader.md §5.3`     |
| §4.3 Boot Regression    | Boot progress API               | `TODO-010.98-Kernel-Heartbeat.md`  |
| §5.3 USB Logging        | §3.4.1 FAT32 Append             | `TODO-040-Filesystem.md`           |
| §6.2 Subsystem Levels   | Registry system                 | `TODO-050-Registry.md`            |

---

## Priority Order

| Priority | Section                            | Description                                      |
| -------- | ---------------------------------- | ------------------------------------------------ |
| 🔴 P0   | 7.3 Remove stale `serial.log`     | Quick cleanup — artifact in repo                 |
| 🔴 P0   | 2.3 Remove `grub.cfg`             | Quick cleanup — dead file                        |
| 🟠 P1   | 1.1 Merge klog files              | Foundation — unified logging                     |
| 🟠 P1   | 1.2 Long filename boot logs       | Human-readable filenames with date/time          |
| 🟠 P1   | 1.3 Live flush mode               | Critical for real hardware debugging             |
| 🟠 P1   | 1.4 Disk space management         | Prevent silent logging failure                   |
| 🟠 P1   | 3.1 Structured hw dump            | Extract from `klog_live.c`                       |
| 🟠 P1   | 3.4 PCI device dump               | Full BARs, capabilities — driver dev             |
| 🟠 P1   | 3.7 Storage/display/network       | Complete hardware picture                        |
| 🟡 P2   | 2.1 Boot config file              | Depends on bootloader §7.1                       |
| 🟡 P2   | 2.2 Decouple splash               | Depends on §2.1                                  |
| 🟡 P2   | 3.2 Device fingerprint            | Hardware tracking hash                           |
| 🟡 P2   | 3.3 CPU CPUID dump                | Nice to have — basic version exists              |
| 🟡 P2   | 3.5 ACPI tables + binary dump     | Full ACPI export for `iasl` analysis             |
| 🟡 P2   | 3.6 SMBIOS/DMI tables             | Machine identification — motherboard, BIOS, RAM  |
| 🟡 P2   | 4.1 Crash screenshot ⭐           | BMP screenshot on panic — no other OS does this  |
| 🟡 P2   | 6.1 Log content quality           | Polish — reduce noise                            |
| 🟡 P2   | 6.2 Subsystem filtering           | Advanced — depends on registry                   |
| 🟢 P3   | 1.5 Partition hiding              | Production polish — hide B: from file manager    |
| 🟢 P3   | 3.8 USB descriptor dump           | Depends on USB HCI driver                        |
| 🟢 P3   | 3.9 Hypervisor detection           | VM-specific diagnostics                          |
| 🟢 P3   | 4.2 Event timeline ⭐             | Kernel "dashcam" — last 256 events before crash  |
| 🟢 P3   | 4.3 Boot regression detection ⭐  | Auto-detect performance regressions              |
| 🟢 P3   | 5.1-5.3 Cross-env verification    | Testing — verify all environments                |
| 🟢 P3   | 7.2 Remove DEBUG file             | After §2.1 is complete                           |
| 🟢 P3   | 8.1 Boot Time Profiler            | Performance measurement                          |

> [!NOTE]
> ⭐ = Feature where Impossible OS can be **superior** to both Windows and Linux.

---

## OS Comparison

| Feature                              | 🪟 Windows 11                  | 🐧 Linux                   | 🚀 Impossible OS                        |
| ------------------------------------ | ------------------------------ | --------------------------- | ---------------------------------------- |
| Dedicated diagnostic partition       | ❌ No equivalent                | ❌ No equivalent             | ⬜ §1 — **Black Box** `B:\` (unique) ⭐ |
| Boot log with timestamps             | ✅ `%windir%\Logs\CBS`          | ✅ journald                  | ⬜ §1.2 — long filename boot logs        |
| Log auto-rotation                    | ✅ Automatic                    | ✅ logrotate                 | ⬜ §1.4 — auto cleanup with warnings     |
| Live log flush (pre-hang)            | ✅ ETW real-time                | ✅ journald sync             | ⬜ §1.3 — exists but fragile             |
| Boot config file                     | ✅ BCD                          | ✅ cmdline / grub.cfg        | ⬜ §2.1 — **missing**                    |
| Debug mode toggle                    | ✅ `bcdedit /debug`             | ✅ `debug` cmdline           | ⬜ §2.1 — uses file flag                 |
| **Comprehensive HW report** ⭐       | ⚠️ msinfo32 (GUI, not portable)| ⚠️ `lshw` (separate tool)  | ⬜ §3 — single-file driver dev kit       |
| **SMBIOS dump** ⭐                   | ✅ msinfo32                     | ✅ `dmidecode`               | ⬜ §3.6 — in Hardware Report             |
| **ACPI binary export** ⭐            | ⚠️ `acpidump` (dev tools)     | ✅ `acpidump`                | ⬜ §3.5 — auto-saved to `B:\ACPI_Tables` |
| **Device fingerprint** ⭐            | ❌ No equivalent                | ❌ No equivalent             | ⬜ §3.2 — hardware config hash           |
| PCI full enumeration                 | ✅ Full BARs, caps              | ✅ `lspci -vvv`              | ⬜ §3.4 — vendor:device only             |
| **Crash screenshot** ⭐              | ❌ No equivalent                | ❌ No equivalent             | ⬜ §4.1 — BMP on panic                   |
| **Event timeline** ⭐                | ❌ ETW post-hoc only            | ❌ No kernel ring            | ⬜ §4.2 — last 256 events before crash   |
| **Boot regression detection** ⭐     | ❌ No equivalent                | ❌ No equivalent             | ⬜ §4.3 — auto-warns if slower           |
| Kernel log ring buffer               | ✅ KD ring (64K)                | ✅ dmesg (256K)              | ✅ klog ring (128K)                       |
| Serial console debug                 | ✅ Kernel debugger              | ✅ serial console            | ✅ COM1 via `klog.c`                      |
| Multi-env (QEMU/VBox/HW)            | N/A                            | ✅ Works everywhere          | ✅ §5 — serial + B: logs                 |
| Per-subsystem filtering              | ✅ ETW providers                | ✅ `printk` levels           | ⬜ §6.2 — **missing**                    |
| Boot time profiling                  | ✅ Boot Event Collector         | ✅ `systemd-analyze blame`   | ⬜ §8 P3 — **missing**                   |

> **After P0+P1 items:** Impossible OS has a reliable, robust logging system that works across
> all environments and produces useful, named logs with hardware dumps on the Black Box.
> **After P2 items:** Matches Linux's logging capabilities and exceeds Windows in transparency.
> **After P3 items:** The Black Box, crash screenshot, event timeline, and boot regression
> detection provide diagnostic capabilities that **no other consumer OS offers**.

---
