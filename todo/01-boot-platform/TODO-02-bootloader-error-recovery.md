# TODO-02 -- Bootloader Error Recovery & ELF Hardening

> **Goal:** Eliminate every silent failure in `bootx64.c`. The bootloader currently has 15+ fragility points where errors cause infinite HLT loops with no visible output, corrupt memory from unchecked ELF segments, or silently use wrong defaults. After this TODO: every failure produces a visible error message on screen and serial with actionable information, serial port detection uses ACPI SPCR when available, the UEFI watchdog timer guards against hangs, memory map descriptors are validated for consistency, boot errors persist in NVRAM for cross-boot diagnostics, and the error screen includes a QR code for recovery. The bootloader never hangs silently -- it either boots or tells you exactly why it can't.

> [!IMPORTANT]
> **Current state:** Audit identified: ELF parser with zero bounds checking (can write to any address), ExitBootServices with only 1 retry (spec allows many), kernel missing = silent HLT, serial assumes COM1 exists, first filesystem protocol used blindly (wrong disk on multi-boot), memory map capped at 256 entries with silent truncation, GOP operations with no timeout, boot.conf missing = silent defaults. Every one of these has caused real boot failures on hardware.
>
> **Code-truth (2026-04-10):** `src/boot/uefi/bootx64.c` `load_kernel()` (from ~956) checks ELF magic/class/machine only; still indexes `ehdr->e_phoff` / `phdr[i]` without file-bounds or overlap checks; opens only `\\boot\\kernel.exe`; allocates 16 MiB only (no 32/8 fallback); `BOOT_MMAP_MAX_ENTRIES` is 256 (`#define` ~21); `ExitBootServices` has one remap+retry (~2412-2419); `SetWatchdogTimer(0,...)` disables firmware watchdog (~2280) without re-arm. `include/kernel/boot_info.h` has no `BOOT_INFO_MAGIC` / `boot_info_header` yet (`confirmed` via grep).

---

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) -- UEFI bootloader (2384 lines, 15+ fragility points)
- [`src/boot/entry.asm`](../../src/boot/entry.asm) -- 32-to-64-bit mode transition
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) -- boot data structures (boot_info at 0x10000)
- [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c) -- Phase 0 `boot_info` memcpy path (§16)
- [`src/kernel/acpi.c`](../../src/kernel/acpi.c) -- ACPI table parsing (SPCR table lookup for §10)
- -> XREF: `TODO-01-uefi-hardening-secureboot.md §7` -- boot UX polish
- -> XREF: `TODO-01-uefi-hardening-secureboot.md §9` -- shared max `ExitBootServices` attempt count with §2 here (single N in both TODOs before coding)
- -> XREF: `TODO-05-bare-metal-hardening.md §7` -- resilient boot with graceful degradation
- -> XREF: `TODO-03-boot-device-discovery.md §1` -- boot device identification (uses filesystem protocol correctly)
- -> XREF: `TODO-03-boot-device-discovery.md §5,§11` -- fallback when no kernel on any volume, and critical health-check path, invoke `boot_fatal` / error screen (this file §9)
- -> XREF: `TODO-07-boot-diagnostics.md §5` -- panic forensic evidence struct; §13 here provides the bootloader-stage error codes that §5 persists across reboots
- -> XREF: `TODO-07-boot-diagnostics.md §6` -- panic QR code; §14 here implements the UEFI-stage QR code before kernel handoff
- -> XREF: `TODO-16-boot-watchdog.md §1` -- kernel-stage software watchdog; §11 here covers the UEFI-stage watchdog before ExitBootServices
- -> XREF: [`02-kernel-core/TODO-01-kernel-init-sequencing.md §2`](../02-kernel-core/TODO-01-kernel-init-sequencing.md) -- boot_info ABI verify follow-up (context for §15-§16)
- -> XREF: `TODO-18-uefi-advanced.md §4` -- multi-GOP enumeration owner; §5 here wraps timeout and headless fallback only
- -> XREF: `TODO-15-recovery-partition.md` -- WinRE-style offline ESP repair and recovery shell (out of scope here; pre-kernel error UX only in this TODO)

---

## Outcome

- Every EFI call in `bootx64.c` has error handling with human-readable diagnostics on both serial and screen.
- ELF parser validates all header fields, segment offsets, and memory ranges before copying.
- ExitBootServices uses a bounded multi-retry loop per UEFI spec and the shared policy with `TODO-01-uefi-hardening-secureboot.md §9`.
- Missing kernel triggers fallback search across 3 paths before giving up.
- Serial port is probed before use; ACPI SPCR table consulted first, then I/O probe with COM1/COM2 fallback.
- Memory map overflow detected and logged (cap raised to 512 entries); descriptors validated for consistency.
- UEFI watchdog timer re-armed after disabling default to catch bootloader hangs.
- Fatal errors render a visible error screen with error code, recovery instructions, and QR code.
- Boot error codes persisted in UEFI NVRAM for next-boot diagnostics.
- Offline recovery partition chains after unrecoverable boot failures (-> XREF: `TODO-15-recovery-partition.md`).

---

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | ELF bounds checking                            | --         |  [x]   |
| 💎  |   2   | ExitBootServices retry loop (bounded)          | --         |  [x]   |
| 💎  |   3   | Fallback kernel search (3 paths)               | --         |  [x]   |
| 💎  |   4   | Serial port probe and COM2 fallback            | --         |  [x]   |
| 💎  |   5   | GOP timeout and graceful degradation           | --         |  [x]   |
| 💎  |   6   | Memory map overflow detection (512 entries)    | --         |  [x]   |
| 💎  |   7   | boot.conf validation and version field         | --         |  [x]   |
| 💎  |   8   | Kernel load allocation fallback (32-16-8 MiB)  | --         |  [x]   |
| ⭐  |   9   | Boot failure error screen                      | §1-§8      |  [x]   |
| 💎  |  10   | ACPI SPCR serial port auto-detection           | §4         |  [x]   |
| 💎  |  11   | UEFI watchdog timer management                 | --         |  [ ]   |
| 💎  |  12   | Memory map descriptor validation               | §6         |  [ ]   |
| ⭐  |  13   | Boot error code registry & NVRAM persistence   | §9         |  [ ]   |
| ⭐  |  14   | Error screen QR code                           | §9         |  [ ]   |
| 💎  |  15   | boot_info ABI header + bootloader populate     | --         |  [ ]   |
| 💎  |  16   | boot_info kernel validate + unit tests         | §15        |  [ ]   |

> 💎 = parity -- Windows bootmgfw.efi and GRUB2 both handle these error paths.
> ⭐ = exclusive -- visible error screen with recovery instructions, QR code, and NVRAM-persisted error codes; neither Windows nor Linux provides this level of pre-kernel diagnostic detail.

---

## 1. ELF Bounds Checking

Harden the kernel ELF parser in `load_kernel()` to reject malformed or corrupted binaries.

> [!NOTE]
> **Regression risk:** LOW -- additive validation. Existing valid kernels pass all checks. If a check false-positives, remove it specifically.

- [x] Validate `e_phoff` is within file bounds: `e_phoff + e_phnum * sizeof(Elf64_Phdr) <= file_size`
- [x] Validate each `PT_LOAD` segment: `p_offset + p_filesz <= file_size`
- [x] Validate `p_memsz >= p_filesz` (ELF spec requirement)
- [x] Reject segments that overlap boot_info region (0x10000--0x11000): checks `seg_start < 0x11000 && seg_end > BOOT_INFO_PHYS_ADDR`
- [x] Reject segments that overlap framebuffer base address: checks against `g_boot_info_ptr->fb.addr` + pitch*height when fb is initialized
- [x] Cap total kernel size at 32 MiB (`ELF_MAX_KERNEL_SIZE`): reject with clear error if exceeded
- [x] Log each loaded segment: `"[BOOT] ELF segment N: paddr=0xHHHH filesz=N memsz=N"` via serial_early_print per segment
- [x] On any validation failure: `"[FAIL] Kernel ELF corrupt: <reason>"` with specific error text for each check
- [x] Commit: `"boot: harden ELF parser -- bounds check all headers and segments"` (3c888540)

**Test checkpoint:** Build a test kernel with `e_phoff` pointing past EOF. Bootloader must reject with `"Kernel ELF corrupt: phdr offset past EOF"` on serial. Verify on QEMU WHPX and TCG. Normal kernel must pass all checks on all 4 platforms (WHPX, TCG, VBox, bare metal).

---

## 2. ExitBootServices Retry Loop

UEFI spec (Section 7.4.6) explicitly allows the memory map to change between GetMemoryMap and ExitBootServices. Real hardware with background events (thermal, EC) can cause repeated map_key mismatches.

> [!NOTE]
> **Regression risk:** MEDIUM -- touches the most critical boot transition. If retry logic corrupts map_key, boot fails. Rollback: revert to 1-retry.

- [x] -> XREF: `TODO-01-uefi-hardening-secureboot.md §9` -- N=4 locked (EBS_MAX_ATTEMPTS=4, commit 3c552022)
- [x] Bounded retry loop: `for (ebs_attempt = 0; ebs_attempt < EBS_MAX_ATTEMPTS; ebs_attempt++)` in `bootx64.c`
- [x] Each attempt: `get_memory_map()` -> fresh `map_key` -> `ExitBootServices(map_key)` with separate `ebs_status` tracking
- [x] Retry logging: `"[BOOT] ExitBootServices retry\n"` per failed attempt
- [x] On success: `"[BOOT] ExitBootServices OK\n"`
- [x] On final failure: `"[CRIT] ExitBootServices failed after retries\n"` + HLT forever
- [x] Re-populate `fill_memory_map()` + `fill_runtime_map()` on each retry after fresh `get_memory_map()`
- [x] Commit: implemented in TODO-01 §9 commit 3c552022 (shared EBS retry + Secure Boot DB registry mirror)

**Test checkpoint:** Difficult to test directly (requires firmware that changes map between calls). Verify normal boot still succeeds on all 4 platforms. Serial output should show the first `ExitBootServices attempt` log line using the agreed `N` from TODO-01 §9.

---

## 3. Fallback Kernel Search

If `\boot\kernel.exe` is not found, search alternative paths before giving up.

> [!NOTE]
> **Regression risk:** LOW -- additive search paths. Default path unchanged.

- [x] Try paths in order: `\boot\kernel.exe` -> `\kernel.exe` -> `\EFI\ImpossibleOS\kernel.exe`
- [x] Log each attempt: `"[BOOT] Trying <path>..."` with path name
- [x] On success: `"[BOOT] Kernel found at <path>"` and continue
- [x] On all paths failing: `"[FAIL] Kernel not found. Searched: ..."` with all 3 paths. Only continues search on EFI_NOT_FOUND; device/FS errors stop immediately with specific error. Closes root_dir on failure.
- [x] Use `LoadedImage->DeviceHandle` via `HandleProtocol` to get the boot device's filesystem; falls back to `LocateProtocol` if LoadedImage unavailable.
- [x] Commit: `"boot: fallback kernel search -- 3 paths before failure"` (f514594b)

**Test checkpoint:** Rename `\boot\kernel.exe` to `\kernel.exe` on EFI partition. Boot must succeed with serial showing `"Trying \boot\kernel.exe... not found"` then `"Kernel found at \kernel.exe"`. Verify on QEMU TCG. Confirm default path works on all 4 platforms.

---

## 4. Serial Port Probe and COM2 Fallback

Modern hardware (laptops, tablets) may not have COM1 at 0x3F8. Blindly initializing it can write to unrelated I/O ports.

> [!NOTE]
> **Regression risk:** LOW -- additive probe before existing init. If probe gives false negative, serial is just silent.

- [x] Before `serial_early_init()`: `serial_probe_port()` writes 0xAE to scratch register (base+7), reads back -- if mismatch, port absent
- [x] If COM1 absent: try COM2 at 0x2F8 with same probe via `serial_probe_port(SERIAL_COM2)`
- [x] If both absent: `s_serial_port = 0`, `serial_early_putchar` returns immediately (no-op)
- [x] Selected port stored in `boot_info.serial_port` (0x3F8 / 0x2F8 / 0) -- `UINT16` field added to both bootloader and kernel `struct boot_info`
- [x] Kernel `serial_init()` reads `g_boot_info.serial_port` to set `s_serial_port` (dynamic). `serial_putchar_raw`, `serial_trygetchar` all use the dynamic port. Default 0x3F8 for early klog before serial_init.
- [x] Commit: `"boot: probe serial port before init -- COM1/COM2 fallback"` (d785ce92)

**Test checkpoint:** On QEMU (always has COM1), serial output works as before. On VirtualBox with serial disabled, bootloader skips serial silently. Verify no I/O port side effects on bare metal.

---

## 5. GOP Timeout and Graceful Degradation

GOP operations can hang on broken firmware. This section adds error recovery around GOP operations -- timeouts, fallback modes, and headless boot. Multi-GPU handle enumeration and primary display selection are owned by `TODO-18-uefi-advanced.md §4`; TODO-01 §3 owns single-GOP negotiation. This section adds defensive wrappers around whichever GOP path is active.

> [!NOTE]
> **Scope boundary:** `TODO-18-uefi-advanced.md §4` owns `LocateHandleBuffer()` multi-GOP enumeration, ConOut primary selection, and `boot_info.gop_handles[]`. This section owns timeout/error recovery: mode enumeration abort, SetMode fallback, headless-boot path. If TODO-18 §4 lands first, wrap its enumeration with these guards. If this lands first, wrap the existing `LocateProtocol` path.
> **Regression risk:** MEDIUM -- changes how GOP is located. If `LocateHandleBuffer` returns handles in different order than `LocateProtocol`, display may be on wrong GPU. Rollback: revert to `LocateProtocol`.

- [x] Wrap `QueryMode()` calls in a counted loop: 100 consecutive errors aborts mode enumeration with serial log
- [x] If `SetMode()` fails: log mode index, try mode 0 as fallback, then fall through to firmware default
- [x] If no GOP available at all: `fb.addr = 0`, `fb_available = 0`, return EFI_SUCCESS (headless boot continues)
- [x] Log: `"[BOOT] GOP: 1 handle found"` or `"[BOOT] GOP: none found, headless boot"`. SetMode failure logged with mode index.
- [x] Commit: `"boot: GOP timeout and graceful degradation -- headless fallback"` (18cc8d84)

**Test checkpoint:** Boot on QEMU (single GOP) -- works as before. Serial shows `"GOP: 1 handles found"` (or `"headless boot"` if GOP absent). If available, test on multi-GPU VirtualBox config. Verify on bare metal -- firmware GOP behavior differs from emulated.

---

## 6. Memory Map Overflow Detection

If firmware reports more memory regions than `BOOT_MMAP_MAX_ENTRIES` (currently 256), the array silently truncates.

> [!NOTE]
> **Regression risk:** LOW -- increases array size (adds ~4 KiB to boot_info). Verify boot_info doesn't overflow its 4 KiB page at 0x10000.

- [x] Increase `BOOT_MMAP_MAX_ENTRIES` from 256 to 512 in both `boot_info.h` and bootloader struct
- [x] Before filling mmap array: compare `total_descs` against `BOOT_MMAP_MAX_ENTRIES`
- [x] If exceeded: `"[WARN] Memory map has N entries, truncating to 512"` on serial
- [x] Set `boot_info.mmap_truncated = 1` flag (new `uint8_t` field in both kernel and bootloader structs)
- [x] Kernel PMM warns at init if `g_boot_info.mmap_truncated` is set: `"PMM: memory map truncated by bootloader"`
- [x] Commit: `"boot: detect memory map overflow -- increase cap to 512, warn on truncation"` (21766b72)

**Test checkpoint:** Normal boot (typically ~130 entries) works as before on all 4 platforms. Add a `mmap_truncated` field to boot_info and verify kernel reads it. Verify on bare metal -- real firmware often has more entries than QEMU.

---

## 7. boot.conf Validation

Malformed boot.conf should produce warnings, not silent misbehavior.

> [!NOTE]
> **Regression risk:** LOW -- validation is additive. Valid configs produce no new warnings.

- [x] Buffer increased to 4096 bytes; if file fills buffer, logs `"[WARN] boot.conf too large (N bytes), truncating"`
- [x] After parsing each key=value: unknown keys produce `"[WARN] boot.conf: unknown key 'X' (ignored)"`
- [x] Known-key whitelist: all existing keys + new `config_version` key
- [x] `config_version` field added to `boot_config` struct (1 byte from `_reserved[]`). `config_version=1` in boot.conf sets it.
- [x] Range validation: `debug`/`verbose`/`test` clamped to 0-1, `splash_timeout` clamped to 0-60 (default 3). Out-of-range logs `"[WARN] boot.conf: X out of range, using N"`
- [x] Commit: `"boot: validate boot.conf -- warn on unknown keys and out-of-range values"` (83b2ccb8)

**Test checkpoint:** Add `bogus_key=42` to boot.conf. Serial must show `"unknown key 'bogus_key'"`. Set `splash_timeout=999` -- serial must show `"out of range, using default 3"`.

---

## 8. Kernel Load Memory Allocation Fallback

If 16 MiB contiguous allocation fails (fragmented memory), try smaller sizes.

> [!NOTE]
> **Regression risk:** LOW -- tries larger first, same result as current behavior (which allocates 16 MiB).

- [x] Try `AllocatePages(32 MiB)` first via graduated fallback array
- [x] If fails: try 16 MiB, then 8 MiB (3-element loop)
- [x] Log actual allocation: `"[BOOT] Kernel buffer: N MiB allocated"`
- [x] If all fail: `"[FAIL] Cannot allocate kernel buffer (tried 32/16/8 MiB)"` + return EFI_LOAD_ERROR
- [x] Kernel file size validated by §1 ELF bounds checks (32 MiB cap + per-segment validation)
- [x] Buffer overlap checks: boot_info (0x10000-0x11000) and framebuffer (addr + pitch*height)
- [x] Commit: `"boot: kernel allocation fallback -- 32-16-8 MiB with overlap check"` (c5cd3704)

**Test checkpoint:** Normal boot works (kernel is ~1.5 MiB, fits in any allocation). Serial shows `"Kernel buffer: 32 MiB allocated"`.

---

## 9. Boot Failure Error Screen

Replace all `for (;;) hlt;` loops with a visible error screen rendered using the UEFI console output protocol (still available pre-ExitBootServices) or the GOP framebuffer (if available).

> [!NOTE]
> **Regression risk:** LOW -- replaces existing HLT loops. If error screen rendering crashes, falls back to HLT (same as before, no worse).

- [x] Create `boot_fatal(const char *title, const char *detail)` function (ASCII args, internally converts to UCS-2)
- [x] Renders on UEFI console: white text on blue background (BSOD style) via `ConOut->SetAttribute` + `ClearScreen`
- [x] Recovery steps: "1. Check boot media is inserted", "2. Verify \\boot\\kernel.exe exists", "3. Press any key to reboot or power off"
- [x] Wait for keypress via `ConIn->ReadKeyStroke` polling loop; then attempts `ResetSystem(EfiResetCold)` before HLT fallback
- [x] Replaced 4 `for (;;) hlt;` error paths with `boot_fatal()`: kernel load, GetMemoryMap, EBS retry GetMemoryMap, EBS final failure
- [x] Log error to serial before displaying screen: `"[CRIT] BOOT FATAL: <title>"` + detail
- [x] Added `EFI_SIMPLE_TEXT_INPUT_PROTOCOL` + `EFI_INPUT_KEY` + console color defines to `efi.h`; typed `ConIn` and `SetAttribute`
- [x] Commit: `"boot: visible error screen on fatal failures -- no more silent halts"` (f734faf3)

**Test checkpoint:** Delete `\boot\kernel.exe` from boot disk. Boot must show error screen with `"Kernel not found"` message and recovery instructions -- not a black screen. Verify on QEMU WHPX, TCG, and VBox. Verify on bare metal -- confirm ConIn keypress works on real keyboard.

---

## 10. ACPI SPCR Serial Port Auto-Detection

Modern firmware provides the ACPI Serial Port Console Redirection Table (SPCR) specifying the exact serial port address, baud rate, and terminal type. Windows and Linux both consult SPCR before falling back to I/O probing. §4's scratch-register probe is necessary as a fallback, but SPCR should be the primary detection method.

> [!NOTE]
> **Regression risk:** LOW -- SPCR lookup is read-only; if table is absent or unparseable, falls through to existing §4 I/O probe unchanged.

- [x] Before §4 I/O probe: search ACPI config tables (via `gST->ConfigurationTable`) for SPCR signature `"SPCR"` -- `serial_spcr_probe()` in `bootx64.c` walks RSDP->XSDT/RSDT->SPCR
- [x] If SPCR found: extract `BaseAddress.Address` for port I/O base, `BaudRate` field, `FlowControl`, `TerminalType` -- `BL_ACPI_SPCR` struct, `spcr_decode_baud()` maps encoded field to actual rate
- [x] Store SPCR-detected port in `boot_info.serial_port` and `boot_info.serial_baud` -- stored in `efi_main()` alongside existing `serial_port`
- [x] Add `boot_info.serial_source` field: 0=none, 1=SPCR, 2=I/O-probe -- added to `boot_info.h` and bootloader's struct
- [x] If SPCR address differs from COM1/COM2 (e.g., MMIO UART on ARM-like platforms): log `"[BOOT] SPCR: non-standard port at 0x%llx (MMIO), skipping"` and fall through to I/O probe -- checks `base_addr_space != 1` and port != COM1/COM2
- [x] Log: `"[BOOT] Serial: SPCR detected port=0x%x baud=%u"` or `"[BOOT] Serial: SPCR absent, falling back to I/O probe"` -- logged after `boot_log_init()` in `efi_main()`
- [x] Kernel serial init honors `boot_info.serial_baud` from SPCR instead of hardcoding 38400 -- `serial.c` computes divisor from `serial_baud`
- [x] Commit: `"boot: ACPI SPCR serial port auto-detection before I/O probe"` -- 437fde7a

**Test checkpoint:** On QEMU with `-device isa-debug-exit` (SPCR absent), fallback I/O probe activates and serial works as before. On QEMU OVMF with SPCR table present, serial log shows `"SPCR detected"`. Verify on bare metal -- real firmware may provide SPCR with non-standard baud rates; kernel must honor the SPCR baud.

---

## 11. UEFI Watchdog Timer Management

The UEFI firmware starts a 5-minute watchdog timer at boot. The current bootloader disables it immediately (`SetWatchdogTimer(0, ...)`). If the bootloader hangs (e.g., GOP negotiation on broken firmware, USB enumeration), there's no timeout -- infinite HLT. Re-arming the watchdog after disabling the default provides a safety net.

> [!NOTE]
> **Regression risk:** LOW -- if watchdog fires unexpectedly, system reboots (recoverable via A/B rollback). Worst case is premature reboot on very slow firmware; extend timeout to 120s if seen.

- [ ] After initial `SetWatchdogTimer(0, ...)`: re-arm with a 60-second timeout: `gBS->SetWatchdogTimer(60, 0x424F4F54, 0, NULL)` (code = "BOOT")
- [ ] Before `ExitBootServices()`: disable the watchdog (`SetWatchdogTimer(0, ...)`) -- no longer needed post-EBS
- [ ] If any pre-EBS operation takes > 30s (GOP, USB discovery, kernel load), reset the timer: `gBS->SetWatchdogTimer(60, ...)` to extend the window
- [ ] Log: `"[BOOT] Watchdog: armed (60s)"` at entry, `"[BOOT] Watchdog: disarmed"` before ExitBootServices
- [ ] On watchdog timeout: firmware resets the system automatically (UEFI spec behavior) -- combined with TODO-14 A/B rollback, this prevents infinite boot loops
- [ ] Commit: `"boot: re-arm UEFI watchdog timer as boot hang safety net"`

**Test checkpoint:** Normal boot completes in < 10s; watchdog is disarmed before ExitBootServices. Serial shows `"Watchdog: armed"` and `"Watchdog: disarmed"`. No unexpected reboots on all 4 platforms. Verify on bare metal -- real firmware watchdog behavior may differ from emulated; some firmware ignores the watchdog code parameter.

---

## 12. Memory Map Descriptor Validation

§6 handles memory map overflow. This section validates individual descriptors for consistency -- overlapping physical ranges, invalid memory types, and zero-length regions that can cause PMM corruption.

> [!NOTE]
> **Regression risk:** LOW -- validation is read-only. Invalid entries are stripped, not rejected. Quirky firmware boots with warnings rather than failure.

- [ ] After `GetMemoryMap()`: iterate all descriptors and check:
  - `PhysicalStart` is page-aligned (4 KiB boundary)
  - `NumberOfPages > 0` (zero-length descriptors are invalid)
  - `Type` is a valid `EFI_MEMORY_TYPE` enum value (0--15 per UEFI 2.10)
  - No two descriptors' physical ranges overlap: sort by `PhysicalStart`, check `[start, start + pages*4096)` intervals
- [ ] On invalid descriptor: `"[WARN] Memory map entry %u: %s"` with specific reason (e.g., `"zero pages"`, `"invalid type 0x%x"`, `"overlaps entry %u"`)
- [ ] Do NOT reject the map -- firmware quirks are common. Log warnings, strip invalid entries from boot_info copy
- [ ] Set `boot_info.mmap_quirks` flag if any warnings fired -- kernel PMM can be extra cautious
- [ ] Commit: `"boot: validate memory map descriptors -- detect overlaps, zero-length, invalid types"`

**Test checkpoint:** Normal boot on QEMU produces no warnings (OVMF generates clean maps). Add a synthetic zero-length descriptor injection test if feasible. Serial log shows descriptor count and any warnings. Verify on bare metal -- real firmware is the primary target for memory map quirks; log any warnings for firmware bug reporting.

---

## 13. Boot Error Code Registry & NVRAM Persistence

Windows has BootStatusPolicy but error codes are opaque hex values without context. Linux has no bootloader-level error persistence. This section creates a structured error code system where each failure has a unique code, and the last-boot error is saved in UEFI NVRAM for next-boot diagnostics.

> [!TIP]
> **Competitive advantage:** Neither Windows nor Linux persists structured bootloader error codes in NVRAM. The next boot can display "Last boot failed: 0x0003 -- Kernel not found at \boot\kernel.exe" before trying again. Combined with TODO-14 A/B rollback and TODO-07 §5 panic forensics, this gives a complete cross-boot diagnostic chain.
> **Regression risk:** LOW -- NVRAM writes are non-destructive (single variable). If NVRAM is full or read-only, write silently fails and boot continues.

- [ ] Define `enum boot_error_code` in `efi.h` or a new `boot_errors.h`:
  - `BOOT_OK = 0x0000`
  - `BOOT_ERR_ELF_CORRUPT = 0x0001` (§1)
  - `BOOT_ERR_EXIT_BS_FAIL = 0x0002` (§2)
  - `BOOT_ERR_KERNEL_NOT_FOUND = 0x0003` (§3)
  - `BOOT_ERR_NO_SERIAL = 0x0004` (§4)
  - `BOOT_ERR_NO_GOP = 0x0005` (§5)
  - `BOOT_ERR_MMAP_OVERFLOW = 0x0006` (§6)
  - `BOOT_ERR_CONF_INVALID = 0x0007` (§7)
  - `BOOT_ERR_ALLOC_FAIL = 0x0008` (§8)
  - `BOOT_ERR_WATCHDOG_TIMEOUT = 0x000B` (§11)
- [ ] On fatal error: write `boot_error_code` to UEFI NVRAM variable `ImpossibleOS-BootError` (vendor GUID, non-volatile, boot-service-access + runtime-access)
- [ ] On successful boot: write `BOOT_OK` to clear previous error
- [ ] At boot entry: read `ImpossibleOS-BootError` -- if non-zero, log `"[BOOT] Previous boot failed: code=0x%04x"` on serial
- [ ] Pass `boot_info.last_boot_error` to kernel -- kernel can display toast notification: "Previous boot failed: <description>"
- [ ] `boot_fatal()` (§9) includes the error code in the on-screen display
- [ ] Commit: `"boot: structured error codes with NVRAM persistence -- cross-boot diagnostics"`

**Test checkpoint:** Delete `\boot\kernel.exe`, boot (gets error screen). Reboot normally with kernel restored. Serial shows `"Previous boot failed: code=0x0003"`. Verify NVRAM variable is cleared on successful boot.

---

## 14. Error Screen QR Code

Add a QR code to the boot failure error screen (§9) that encodes a recovery URL with the error code. Smartphones can scan it to get step-by-step recovery instructions. ChromeOS does this for recovery; neither Windows UEFI-stage errors nor Linux GRUB rescue provides it.

> [!TIP]
> **Competitive advantage:** A QR code on the pre-kernel error screen is actionable for non-technical users. Instead of "call support", they scan and get a page explaining exactly what error code 0x0003 means and how to fix it. ChromeOS has this for recovery; neither Win11 nor Linux has it at the UEFI bootloader stage.
> **Regression risk:** LOW -- QR rendering is additive to §9 error screen. If QR encoder has a bug, error screen still shows text error message.

- [ ] Implement minimal QR code encoder in bootloader (QR Version 2, 25x25 modules, alphanumeric mode -- fits `https://impossible.os/err/0003` in ~200 bytes of code)
- [ ] `boot_fatal()` renders QR code in bottom-right corner of the GOP error screen (if GOP available)
- [ ] QR payload: `https://impossible.os/err/XXXX` where `XXXX` is the hex error code from §13
- [ ] QR module size: 4x4 pixels minimum for scannability on 1280x720 resolution
- [ ] If GOP unavailable: skip QR code (console-only error screen has no pixel rendering)
- [ ] Reuse QR logic from TODO-07 §6 panic QR code if already implemented; otherwise implement standalone minimal encoder
- [ ] Commit: `"boot: QR code on boot error screen -- scan for recovery instructions"`

**Test checkpoint:** Trigger boot failure (delete kernel). Error screen shows QR code in bottom-right. Scan with phone -- URL resolves (or shows the encoded URL). Verify QR is scannable at 1280x720 and 1920x1080 resolutions.

---

## 15. boot_info ABI Header and Bootloader Populate

Place a fixed-size header at offset 0 of `struct boot_info` and have the UEFI bootloader fill magic, version, and size before `ExitBootServices()` so the kernel can validate before any `memcpy`.

> [!NOTE]
> **Regression risk:** MEDIUM -- any `struct boot_info` layout change must bump `BOOT_INFO_VERSION` and rebuild bootloader + kernel together.

- [ ] Add `struct boot_info_header` at offset 0: `uint32_t magic` (`BOOT_INFO_MAGIC` e.g. `0x49504F53` "IPOS"), `uint16_t version` (`BOOT_INFO_VERSION`), `uint16_t size` (bootloader writes `sizeof(struct boot_info)` at compile time) in `include/kernel/boot_info.h`.
- [ ] Bootloader `src/boot/uefi/bootx64.c`: assign `boot_info.header.magic`, `.version`, `.size = sizeof(struct boot_info)` before handing off to the kernel path that jumps to `kernel_main`.
- [ ] `_Static_assert(__builtin_offsetof(struct boot_info, header) == 0, ...)` in `boot_info.h`.
- [ ] `_Static_assert(sizeof(struct boot_info_header) == 8, ...)` to pin header layout.
- [ ] Extend `boot_info.h` comments with the ABI and version bump rules.
- [ ] Add a short `CLAUDE.md` note under Bare Metal Gotchas or a new ABI subsection: never ship mismatched `BOOTX64.EFI` vs `kernel.exe` after a `BOOT_INFO_VERSION` bump.
- [ ] Commit: `"boot: boot_info ABI header fields and bootloader populate"`

**Test checkpoint:** Clean build boots on QEMU WHPX, QEMU TCG, VirtualBox, bare metal; header fields visible in memory at the handoff pointer before kernel entry (debugger or serial hex dump).

---

## 16. boot_info Kernel Validation, boot_hw Path, and Unit Tests

The kernel must reject invalid `mbi` before copying `struct boot_info`. The post-hoc cmdline ASCII check at `src/kernel/main/boot_hw.c:79-91` stays as a second-line defense after the validated copy.

> [!IMPORTANT]
> **Found during TODO-01 Phase 0 verify (2026-04-08, Codex round 1, commit b402cf21).** Uncoordinated edits to `struct boot_info` widen the silent-corruption window until this lands.
> **Regression risk:** MEDIUM -- stale `BOOTX64.EFI` after a version bump halts early; mitigate by always using `bash scripts/build.sh` for paired images.

- [ ] Implement `boot_result_t boot_info_validate(const void *p, size_t kernel_struct_size)` in kernel code (pure logic): reject NULL; require `magic == BOOT_INFO_MAGIC`; `version` in the supported set; `size == kernel_struct_size`; pointer in conventional/loader memory (not below 1 MiB, not overlapping known reserved holes per project map).
- [ ] `boot_phase0()` (or first safe callsite before `memcpy`): call `boot_info_validate(mbi, sizeof(struct boot_info))`; on failure call `boot_halt("boot_info handoff validation failed: ...")` including observed bad values.
- [ ] Update `src/kernel/main/boot_hw.c` (~62-67) to copy only `header.size` bytes after validation succeeds and matches `sizeof(struct boot_info)`.
- [ ] Keep and relocate the `cmdline` ASCII sanity check (`boot_hw.c:79-91`) to run after the validated copy; remove stale "struct shifted" wording if any.
- [ ] Add `src/kernel/test/test_boot_info.c` (or extend `test_boot_init.c`) with synthetic buffers: OK path, NULL, bad magic, version mismatch, size mismatch (no forbidden boot/VPD calls per CLAUDE.md).
- [ ] Wire tests in `test_runner_init()` under `SUITE=boot` when the file lands.
- [ ] Commit: `"boot: kernel boot_info_validate before Phase 0 memcpy"`

**Test checkpoint:** Normal boot on QEMU WHPX, QEMU TCG, VirtualBox, bare metal reaches Phase 0 with validation passing. Corrupt `header.magic` in the bootloader build -- serial shows `boot_info handoff validation failed` with expected vs actual. Intentional `BOOT_INFO_VERSION` mismatch across images -- early halt with version text. Unit tests for `boot_info_validate()` PASS.

---

## OS Comparison

| ⭐ | Feature         | 🪟 Win11                         | 🐧 Linux                        | 🚀 Impossible OS                |
| -- | --------------- | --------------------------------- | ------------------------------- | -------------------------------- |
| 💎 | ELF bounds      | ✅ PE header + SizeOfImage check | ✅ GRUB ELF phdr bounds         | ✅ §1 phdr+seg+overlap+32M cap  |
| 💎 | EBS retry       | ✅ bootmgr bounded retry loop    | ✅ efi-stub retry on map stale  | ✅ §2 N=4 bounded + map refresh |
| 💎 | Kernel fallback | ✅ BCD alternate paths + WinRE   | ✅ GRUB rescue + fallback.cfg   | ✅ §3 3-path search + DeviceHdl |
| 💎 | Serial detect   | ✅ ACPI SPCR + EMS headless      | ✅ earlycon=uart,io,0x3f8       | ✅ §4 COM1/COM2 probe+boot_info |
| 💎 | GOP degrade     | ✅ Fallback to basic display     | ✅ efifb + simpledrm fallback   | ✅ §5 headless + SetMode fallbk |
| 💎 | Mmap overflow   | ✅ Dynamic buffer reallocation   | ✅ Grow buf + retry loop        | ✅ §6 512 cap + truncate warn   |
| 💎 | boot.conf parse | ✅ BCD registry schema + edit    | ✅ grub.cfg + grub-mkconfig     | ✅ §7 key whitelist + range chk |
| 💎 | Alloc fallback  | ✅ Graduated pool sizes          | ✅ Dynamic retry allocation     | ✅ §8 32/16/8 MiB + overlap chk |
| 💎 | SPCR serial     | ✅ EMS Emergency Management      | ✅ earlycon SPCR auto-detect    | ✅ §10 RSDP->XSDT->SPCR parse  |
| 💎 | UEFI watchdog   | ✅ Re-arm via SetWatchdogTimer   | ✅ efi_stub disables watchdog   | ⬜ §11 watchdog re-arm/disable  |
| 💎 | Mmap validate   | ✅ Descriptor version + size     | ✅ efi_stub sanity checks       | ⬜ §12 mmap descriptor verify   |
| ⭐ | Error screen    | ❌ Generic BSOD (no boot ctx)    | ⚠️ GRUB text menu (no graphics) | ✅ §9 blue BSOD + key + reboot |
| ⭐ | NVRAM errors    | ⚠️ Opaque status codes           | ❌ No persistent boot errors    | ⬜ §13 NVRAM error log persist  |
| ⭐ | Boot QR         | ❌ No UEFI-phase QR codes        | ❌ No GRUB QR support           | ⬜ §14 QR code error link       |
| 💎 | Handoff ABI     | ✅ BCD signature + protocol      | ✅ Multiboot2 / Linux boot      | ⬜ §15-§16 versioned handoff    |
| 💎 | Offline repair  | ✅ Windows Recovery Environment  | ✅ rescue/live ISO image        | ⬜ TODO-15 recovery partition   |

> **Parity:** 💎 rows track Win11 + Linux bootloader hardening. **⭐** rows are pre-kernel UX beyond typical UEFI/GRUB rescue. Capsule apply stays `TODO-18 §2`; multi-GOP enumeration stays `TODO-18 §4` with §5 here as timeout wrapper only. Full recovery partition / WinRE-class repair is `TODO-15-recovery-partition.md`, not duplicated here.

---

## Unit Tests

> Bootloader code runs pre-ExitBootServices in UEFI context -- not kernel test framework.
> Use `scripts/test-smoke.sh` serial pattern matching for boot-level validation.

- [ ] Add smoke test patterns to `scripts/test-smoke.sh`:
  - Serial line `"[BOOT] ELF segment"` present (§1 -- ELF loader logs each segment)
  - Serial line matching `ExitBootServices attempt 1/` (or agreed prefix from §2 / TODO-01 §9) present on normal boot
  - Serial line `"[BOOT] Kernel found at"` present (§3 -- fallback search logs selected path)
  - Absence of `"[FAIL] Kernel ELF corrupt"` on normal boot (§1 -- no corruption)
  - Absence of `"[CRIT] ExitBootServices failed"` on normal boot (§2 -- exit succeeds)
  - Serial line `"[BOOT] Serial:"` present with either `"SPCR detected"` or `"SPCR absent"` (§10 -- SPCR probed)
  - Serial line `"[BOOT] Watchdog: armed"` present (§11 -- watchdog re-armed at entry)
  - Serial line `"[BOOT] Watchdog: disarmed"` present (§11 -- watchdog disarmed before ExitBootServices)
- [ ] Create `scripts/test-boot-elf-corrupt.sh`:
  - Build disk image, truncate `\boot\kernel.exe` to 512 bytes
  - Boot QEMU headless, capture serial
  - Assert serial contains `"[FAIL] Kernel ELF corrupt"` (§1 rejects truncated ELF)
  - Assert serial contains `"Kernel not found"` or error screen text (§9)
- [ ] Create `scripts/test-boot-missing-kernel.sh`:
  - Build disk image, delete `\boot\kernel.exe`
  - Boot QEMU headless, capture serial
  - Assert serial contains `"Trying \boot\kernel.exe... not found"` (§3 fallback search)
  - §13: Assert serial contains `"Previous boot failed: code=0x0003"` on subsequent reboot after kernel-missing failure
- [ ] Create `scripts/test-boot-error-nvram.sh`:
  - Boot with missing kernel (writes NVRAM error code)
  - Restore kernel, reboot same QEMU instance (NVRAM persists)
  - Assert serial contains `"Previous boot failed: code="` (§13 -- NVRAM error read on next boot)
  - Assert serial contains `"Previous boot failed: code=0x0000"` does NOT appear (cleared only after successful boot)
  - `boot_info_validate()` unit tests from §16 run under `SUITE=boot` and PASS
- [ ] Commit: `"test: add bootloader error recovery smoke tests"`

**Test checkpoint:** `scripts/test-smoke.sh` (or successor harness) passes new patterns on a normal `bash scripts/build.sh run` boot log; `test-boot-*.sh` scripts exit 0 when run from repo root on CI or dev host with QEMU available.

---

## Verification

- [ ] **ELF corruption test**: build test kernel with corrupted phdr -- bootloader rejects with specific error message, not crash.
- [ ] **Missing kernel test**: delete `\boot\kernel.exe` -- error screen shows "Kernel not found" with paths searched.
- [ ] **Normal boot regression**: all 4 platforms (QEMU WHPX, TCG, VBox, bare metal) boot cleanly with no new warnings in serial.
- [ ] **Serial probe test**: VirtualBox with serial disabled -- bootloader skips serial silently, boot succeeds.
- [ ] **Memory map test**: verify `mmap_truncated` field is 0 on normal boot, logged correctly in kernel.
- [ ] §10: Serial log shows `"[BOOT] Serial: SPCR"` line (detected or absent) before serial port output begins.
- [ ] §11: Serial log shows `"Watchdog: armed"` after efi_main entry and `"Watchdog: disarmed"` before ExitBootServices. No unexpected reboots on any platform.
- [ ] §12: Serial log shows no `"[WARN] Memory map entry"` warnings on QEMU OVMF (clean firmware). If warnings appear on real hardware, log them for firmware bug reporting.
- [ ] §13: After boot failure, next boot serial shows `"Previous boot failed: code=0x"`. After successful boot, NVRAM variable reads `BOOT_OK`.
- [ ] §14: Boot failure error screen includes QR code in bottom-right. QR scans to `https://impossible.os/err/XXXX` with correct error code.
- [ ] §15-§16: `boot_info` header populated by bootloader; kernel rejects tampered magic or version skew before Phase 0 copy.
- [ ] Commit: `"boot: bootloader error recovery complete -- zero silent failures"`

**Test checkpoint:** Every Verification bullet passes on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal; serial shows no unexpected `[WARN]` / `[CRIT]` on clean boot after all sections land.

**Test runner:** `scripts\debug\run-boot-tests.bat` (SUITE=boot)

---

## History

| Date | Action | Summary |
| --- | --- | --- |
| 2026-04-10 | validate | validate-todo-file: ASCII `->` XREFs; Inputs links + TODO-18 §4 + kernel-init §2; §1-§15 regression notes moved before checklists; §5 stale TODO-01 §12 fixed to TODO-18 §4; OS table compact; Unit Tests + Verification checkpoints + boot test runner; History added. |
| 2026-04-10 | gap-analysis | gap-analysis-todo: Code-truth IMPORTANT; §15 split -> §15+§16 + Impl row 16; EBS N aligned XREF TODO-01 §9; OS TODO-15 row + Sources; Inputs TODO-15; Unit/Verify §16; TODO-15 + kernel-init + TODO-01 patches; 6 searches + MS Learn fetch. |
| 2026-04-10 | validate | validate-todo-file: continuation rg clean; Inputs + `boot_hw.c` + TODO-01 §9 Inputs XREF; §2 policy bullet de-staled (shared N); OS `TODO-15` cell; 16 `##` sections Commit+Test-last OK; `run-boot-tests.bat` present; external XREF section anchors spot-checked. |
| 2026-04-10 | validate | Inputs: `-> XREF` `TODO-03-boot-device-discovery.md §5,§11` for error-screen handoff from fallback / health-check (paired with TODO-03 validate pass). |
