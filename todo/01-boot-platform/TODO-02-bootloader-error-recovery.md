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
- → XREF: `TODO-05-bare-metal-hardening.md §7` -- resilient boot with graceful degradation
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
| 💎  |  11   | UEFI watchdog timer management                 | --         |  [x]   |
| 💎  |  12   | Memory map descriptor validation               | §6         |  [x]   |
| ⭐  |  13   | Boot error code registry & NVRAM persistence   | §9         |  [x]   |
| ⭐  |  14   | Error screen QR code                           | §9         |  [x]   |
| 💎  |  15   | boot_info ABI header + bootloader populate     | --         |  [x]   |
| 💎  |  16   | boot_info kernel validate + unit tests         | §15        |  [ ]   |
| 💎  |  17   | Memory map overlap normalization (sort+carve)  | §12        |  [ ]   |
| ⭐  |  18   | Graphical error screen (ChromeOS/Win11-style)  | §9, §14    |  [ ]   |

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

> **Verified:** 2026-04-11 -- all 8 checks confirmed with line evidence. Subtraction-based overflow prevention correct. Framebuffer overlap uses pitch*height with overflow guard. Overlapping PT_LOAD segments accepted (valid ELF feature, matches Linux/GRUB). Accepted: none.
> **Quality reviewed:** 2026-04-11 -- 1 spec violation fixed (e_phentsize != sizeof(Elf64_Phdr) rejection), 1 best practice (e_phnum capped at 64). Accepted: p_align not checked (identity-mapped, not needed for direct physical copy).

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

> **Verified:** 2026-04-11 -- all 7 items confirmed. EBS_MAX_ATTEMPTS=4, bounded loop with fresh map_key per retry, fill_memory_map + fill_runtime_map on each retry, boot_fatal on exhaustion. Shared implementation with 01-boot-platform/TODO-01 §9. Accepted: none.
> **Quality reviewed:** 2026-04-11 -- no findings. Matches UEFI Spec §7.4.6 exactly. Linux efi-stub uses 8 retries; our 4 is conservative but sufficient (spec requires "retry with fresh map", not a count). ConOut guard via g_ebs_in_progress exceeds Linux/GRUB (neither disables console before EBS). Accepted: none.

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

> **Verified:** 2026-04-11 -- all 5 items confirmed. 3 hardcoded paths, LoadedImage->DeviceHandle + LocateProtocol fallback, EFI_NOT_FOUND continues search / device errors stop immediately, root_dir closed on all failure paths. Accepted: none.
> **Quality reviewed:** 2026-04-11 -- no findings. LoadedImage->DeviceHandle approach matches GRUB2 and exceeds Linux efi-stub (which uses a single hardcoded path). Error handling more robust than Linux (specific error + halt vs silent hang). Accepted: none.

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

> **Verified:** 2026-04-11 -- all 5 items confirmed. Scratch register probe (0xAE write/readback), COM1->COM2->0 fallback chain, boot_info.serial_port stored, kernel serial_init reads it. SPCR (§10) wraps this probe with ACPI table lookup as primary. Accepted: none.
> **Quality reviewed:** 2026-04-11 -- no findings. Scratch register probe matches Linux serial8250_config_port (same 0xAE test value). SPCR (§10) elevates this to ACPI-first detection, matching Windows EMS and Linux earlycon SPCR. COM3/COM4 not probed (extremely rare, SPCR covers non-standard). Accepted: none.

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

> **Verified:** 2026-04-11 -- all 4 items confirmed. QueryMode 100-error abort, SetMode->mode0->firmware fallback, headless boot on no GOP, logging at each stage. Additional hardening from 01-boot-platform/TODO-01 §3 quality review (FrameBufferSize bounds, Mode NULL guard, pitch validation). Accepted: none.
> **Quality reviewed:** 2026-04-11 -- no additional findings. Shared code with 01-boot-platform/TODO-01 §3 which received full quality review (FrameBufferSize bounds, Mode/Info NULL guard, pitch validation, gop_pixel_format_code dedup). Defensive wrappers match UEFI best practice for GOP error recovery. Accepted: none.

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

> **Verified:** 2026-04-11 -- all 5 items confirmed. BOOT_MMAP_MAX_ENTRIES=512 in both structs, total_descs comparison, truncation warning, mmap_truncated flag, PMM warns on boot. Loop guard prevents overrun. Accepted: none.
> **Quality reviewed:** 2026-04-11 -- no findings. 512-entry cap covers known hardware (worst case ~400 on enterprise servers). Linux/Windows/GRUB use dynamic allocation; our fixed array is a deliberate simplicity trade-off -- no heap needed before ExitBootServices. Truncation flag ensures kernel knows if entries were lost. Accepted: none.

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

> **Verified:** 2026-04-11 -- all 5 items confirmed. 4096-byte buffer with truncation warning, unknown key logging, config_version field, range validation for debug/verbose/test/splash_timeout. Accepted: none.
> **Quality reviewed:** 2026-04-11 -- no findings. More user-friendly than Windows BCD (human-readable + warnings) and more defensive than GRUB (range clamping vs silent accept). Accepted: none.

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

> **Verified:** 2026-04-11 -- all 6 items confirmed. Graduated alloc_sizes[] array (32/16/8 MiB), 3-element fallback loop, failure message on exhaustion, overlap checks for boot_info + framebuffer regions. Accepted: none.
> **Quality reviewed:** 2026-04-11 -- no findings. Graduated fallback exceeds Windows bootmgr, Linux efi-stub, and GRUB2 (none retry with smaller sizes). Explicit overlap checks are unique to our loader. Accepted: none.

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

> **Verified:** 2026-04-11 -- all 7 items confirmed. boot_fatal() with serial CRIT log + console blue/white BSOD + recovery instructions + ConIn keypress + ResetSystem fallback. 4 HLT loops replaced. g_ebs_in_progress gates ConOut after EBS attempted. Accepted: none.
> **Quality reviewed:** 2026-04-11 -- no findings. Exceeds Linux efi-stub (silent hang) and matches Windows BSOD UX. ConOut guard for post-EBS errors is unique -- neither Linux nor GRUB handles this. Serial CRIT logging provides remote diagnostics that Windows BSOD lacks. Accepted: none.

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

> **Verified:** 2026-04-11 -- all 7 items confirmed. Codex found unknown baud code treated as preserve-divisor (fixed: non-zero unknown codes now default 38400). Accepted: XSDT/RSDT child pointer validation (corrupt ACPI = unbootable system, SEH needed for recovery).
> **Quality reviewed:** 2026-04-11 -- 2 fixes: SPCR Interface Type check (ACPI spec Table 5-49, type 0/1 = 16550), kernel serial_init honors no-UART (serial_port=0 now authoritative instead of defaulting to COM1). Accepted: MMIO UART support (I/O-only for now, matches current hardware scope).

---

## 11. UEFI Watchdog Timer Management

The UEFI firmware starts a 5-minute watchdog timer at boot. The current bootloader disables it immediately (`SetWatchdogTimer(0, ...)`). If the bootloader hangs (e.g., GOP negotiation on broken firmware, USB enumeration), there's no timeout -- infinite HLT. Re-arming the watchdog after disabling the default provides a safety net.

> [!NOTE]
> **Regression risk:** LOW -- if watchdog fires unexpectedly, system reboots (recoverable via A/B rollback). Worst case is premature reboot on very slow firmware; extend timeout to 120s if seen.

- [x] After initial `SetWatchdogTimer(0, ...)`: re-arm with a 60-second timeout: `gBS->SetWatchdogTimer(60, 0x424F4F54, 0, NULL)` (code = "BOOT") -- with EFI_STATUS check and log on failure
- [x] Before `ExitBootServices()`: disable the watchdog (`SetWatchdogTimer(0, ...)`) -- disarmed with serial log
- [x] If any pre-EBS operation takes > 30s (GOP, USB discovery, kernel load), reset the timer: `gBS->SetWatchdogTimer(60, ...)` -- resets at init_gop, load_kernel, discover_usb_devices
- [x] Log: `"[BOOT] Watchdog: armed (60s)"` at entry, `"[BOOT] Watchdog: disarmed"` before ExitBootServices
- [x] On watchdog timeout: firmware resets the system automatically (UEFI spec behavior) -- no code needed, firmware handles it
- [x] Commit: `"boot: re-arm UEFI watchdog timer as boot hang safety net"`

**Test checkpoint:** Normal boot completes in < 10s; watchdog is disarmed before ExitBootServices. Serial shows `"Watchdog: armed"` and `"Watchdog: disarmed"`. No unexpected reboots on all 4 platforms. Verify on bare metal -- real firmware watchdog behavior may differ from emulated; some firmware ignores the watchdog code parameter.

> **Verified:** 2026-04-11 -- all 5 items confirmed. 60s arm with status check, 3 resets at long ops, disarm with retry before EBS. Codex found unchecked disarm during implementation (fixed: status check + retry). Accepted: none.
> **Quality reviewed:** 2026-04-11 -- 1 fix: centralized watchdog_reset() helper with g_wd_armed tracking (Codex found silent reset failures). Matches UEFI Spec §7.5 (WatchdogCode in OS range 0x10000+). Exceeds Win11/Linux/GRUB (none re-arm, all just disable). Accepted: disarm failure continues to kernel (kernel boots <10s, within 60s timeout -- watchdog reset on failure is actually beneficial).

---

## 12. Memory Map Descriptor Validation

§6 handles memory map overflow. This section validates individual descriptors for consistency -- overlapping physical ranges, invalid memory types, and zero-length regions that can cause PMM corruption.

> [!NOTE]
> **Regression risk:** LOW -- validation is read-only. Invalid entries are stripped, not rejected. Quirky firmware boots with warnings rather than failure.

- [x] After `GetMemoryMap()`: iterate all descriptors and check: `PhysicalStart` page-aligned, `NumberOfPages > 0`, `Type <= EfiMaxMemoryType`, no overlapping physical ranges (O(n^2) scan, n~130, runs once) -- all in `fill_memory_map()` validation pass
- [x] On invalid descriptor: `"[WARN] Memory map entry N: <reason> -- skipping"` with specific reason (zero pages, unaligned, invalid type, overlaps)
- [x] Do NOT reject the map -- log warnings, skip (continue) invalid entries so they're stripped from boot_info copy
- [x] Set `boot_info.mmap_quirks = 1` if any warnings fired -- kernel PMM logs `"memory map had quirky descriptors"`
- [x] Commit: `"boot: validate memory map descriptors -- detect overlaps, zero-length, invalid types"`

**Test checkpoint:** Normal boot on QEMU produces no warnings (OVMF generates clean maps). Add a synthetic zero-length descriptor injection test if feasible. Serial log shows descriptor count and any warnings. Verify on bare metal -- real firmware is the primary target for memory map quirks; log any warnings for firmware bug reporting.

> **Verified:** 2026-04-11 -- all 4 items confirmed. 6 validation checks (overflow, wrap, zero, align, type, overlap) + desc_size geometry guard at caller. Codex found NumberOfPages overflow (fixed) and desc_size=0 path (fixed with boot_fatal at caller level). Accepted: overlap normalization (-> XREF: 01-boot-platform/TODO-02 §17).
> **Quality reviewed:** 2026-04-11 -- boot-code-quality 10/10 gates passed. Codex quality found desc_size geometry + map_size remainder (fixed: boot_fatal guard before both fill functions). EfiMaxMemoryType off-by-one fixed (>= not >). Accepted: overlap priority normalization (-> XREF: 01-boot-platform/TODO-02 §17).

---

## 13. Boot Error Code Registry & NVRAM Persistence

Windows has BootStatusPolicy but error codes are opaque hex values without context. Linux has no bootloader-level error persistence. This section creates a structured error code system where each failure has a unique code, and the last-boot error is saved in UEFI NVRAM for next-boot diagnostics.

> [!TIP]
> **Competitive advantage:** Neither Windows nor Linux persists structured bootloader error codes in NVRAM. The next boot can display "Last boot failed: 0x0003 -- Kernel not found at \boot\kernel.exe" before trying again. Combined with TODO-14 A/B rollback and TODO-07 §5 panic forensics, this gives a complete cross-boot diagnostic chain.
> **Regression risk:** LOW -- NVRAM writes are non-destructive (single variable). If NVRAM is full or read-only, write silently fails and boot continues.

- [x] Define `BOOT_ERR_*` codes as `#define` constants in `efi.h` (13 codes: 0x0000-0x000C covering §1-§12 failure modes)
- [x] On fatal error: `boot_fatal()` writes error code to UEFI NVRAM variable `BootError` (Impossible OS vendor GUID, non-volatile + boot-service-access + runtime-access) via `nvram_write_boot_error()` helper
- [x] On successful boot: kernel clears `BootError` NVRAM variable in `boot_phase0()` after `uefi_vars_init()` succeeds -- clearing in the kernel (not bootloader) ensures a crash between EBS and Phase 0 preserves the error evidence
- [x] At boot entry: `nvram_read_boot_error()` reads NVRAM -- if non-zero, logs `"[BOOT] Previous boot failed: code=0x%04x"` on serial
- [x] Pass `boot_info.last_boot_error` to kernel -- field added to both bootloader struct (bootx64.c) and kernel header (boot_info.h) after `serial_baud`; kernel logs `"Previous boot failed: code=0x%04X"` via klog in `boot_hw.c`
- [x] `boot_fatal()` includes the error code in on-screen display: `"Error code: 0xNNNN"` line on BSOD screen, plus hex code in serial log
- [x] Commit: `"boot: structured error codes with NVRAM persistence -- cross-boot diagnostics"` (373a29db)

**Test checkpoint:** Delete `\boot\kernel.exe`, boot (gets error screen). Reboot normally with kernel restored. Serial shows `"Previous boot failed: code=0x0003"`. Verify NVRAM variable is cleared on successful boot.

> **Verified:** 2026-04-11 -- Codex adversarial clean. Evidence: 13 BOOT_ERR_* codes (efi.h:56-68), nvram_write/read helpers (bootx64.c:794-825), boot_fatal NVRAM persist + hex display, boot_info.last_boot_error ABI-synced, kernel klog (boot_hw.c:94-97). Accepted: none.
> **Quality reviewed:** 2026-04-11 -- moved NVRAM clear from bootloader post-EBS to kernel after uefi_vars_init() per Codex finding (crash between EBS and kernel entry now preserves error evidence). 8 unused BOOT_ERR_* codes are future registry entries by design. Accepted: none.

---

## 14. Error Screen QR Code

Add a QR code to the boot failure error screen (§9) that encodes a recovery URL with the error code. Smartphones can scan it to get step-by-step recovery instructions. ChromeOS does this for recovery; neither Windows UEFI-stage errors nor Linux GRUB rescue provides it.

> [!TIP]
> **Competitive advantage:** A QR code on the pre-kernel error screen is actionable for non-technical users. Instead of "call support", they scan and get a page explaining exactly what error code 0x0003 means and how to fix it. ChromeOS has this for recovery; neither Win11 nor Linux has it at the UEFI bootloader stage.
> **Regression risk:** LOW -- QR rendering is additive to §9 error screen. If QR encoder has a bug, error screen still shows text error message.

- [x] Implement QR code encoder in bootloader: QR Version 3 (29x29 modules), byte mode, ECL-L, Reed-Solomon EC (15 codewords), mask pattern 0 -- self-contained in `bootx64.c`, verified 0-diff against segno (spec-compliant library)
- [x] `boot_fatal()` renders QR code in bottom-right corner of the GOP framebuffer (direct pixel write, works even during EBS retry); if ConOut unavailable but framebuffer available, renders QR-only
- [x] QR payload: `https://impossibleos.co/err/XXXX` where `XXXX` is the hex error code from §13 (lowercase URL via byte mode)
- [x] QR module size: 4px at 1280x720, 6px at 1920x1080, 8px at 2560+ -- with 4-module white quiet zone
- [x] If GOP unavailable (gFramebuffer NULL or size 0): QR rendering silently skipped, text error screen still shows
- [x] Standalone encoder (TODO-07 §6 not yet implemented) -- when §6 lands, the kernel-side encoder can be factored from this implementation
- [x] `error_screen_test=1` boot.conf key triggers `boot_fatal()` before kernel load for QR/BSOD testing (halts instead of rebooting so screen stays visible)
- [x] Commit: `"boot: QR code on boot error screen -- scan for recovery instructions"` (7e588e7e)

**Test checkpoint:** Run `scripts/debug/run-error-screen-test.bat` (sets `error_screen_test=1`). Error screen shows QR code in bottom-right. Scan with phone -- URL `https://impossibleos.co/err/0003` appears. Verify QR is scannable at 1280x720 and 1920x1080 resolutions.

> **Verified:** 2026-04-11 -- Codex adversarial found RS coefficients wrong + format placement wrong (both fixed). Post-fix Codex clean. Real-hardware testing found 3 additional bugs: (1) pixel colors 0xFF000000=blue in BGRX, (2) format bits MSB/LSB reversed (Codex was wrong about this), (3) V2 alphanumeric uppercase URL -- switched to V3 byte mode for lowercase. Accepted: none.
> **Quality reviewed:** 2026-04-11 -- Switched to QR V3 byte mode after user feedback. Three V3 bugs fixed: zigzag loop skipped column pairs after timing column, zigzag direction formula wrong for cols < 6, byte padding added zero codeword when already aligned. All verified against segno: 0 differences. Accepted: none.

---

## 15. boot_info ABI Header and Bootloader Populate

Place a fixed-size header at offset 0 of `struct boot_info` and have the UEFI bootloader fill magic, version, and size before `ExitBootServices()` so the kernel can validate before any `memcpy`.

> [!NOTE]
> **Regression risk:** MEDIUM -- any `struct boot_info` layout change must bump `BOOT_INFO_VERSION` and rebuild bootloader + kernel together.

- [x] Add `struct boot_info_header` at offset 0: `uint32_t magic` (BOOT_INFO_MAGIC=0x49504F53 "IPOS"), `uint16_t version` (BOOT_INFO_VERSION=1), `uint16_t size` -- defined in both `include/kernel/boot_info.h` and `src/boot/uefi/bootx64.c` (ABI sync)
- [x] Bootloader populates `header.magic`, `.version`, `.size = sizeof(struct boot_info)` as the last step before `jump_to_kernel()` -- after all other fields are set
- [x] `_Static_assert(offsetof(boot_info, header) == 0)` in `boot_info.h` -- enforces offset 0
- [x] `_Static_assert(sizeof(boot_info_header) == 8)` in `boot_info.h` -- pins header layout
- [x] `boot_info.h` comments document ABI rules: bump version on layout changes, rebuild both images together, `_reserved` fields don't require bumps
- [x] `CLAUDE.md` new "boot_info ABI" section: documents magic, version mismatch halt, rebuild rule
- [x] Kernel `boot_hw.c` validates magic + version after memcpy, halts on mismatch with clear error message; logs header version, size, and kernel-side sizeof for diagnostics
- [x] Commit: `"boot: boot_info ABI header fields and bootloader populate"` (375e4764)

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

## 17. Memory Map Overlap Normalization

> Found during §12 quality review (2026-04-11). The current overlap check in `fill_memory_map()` skips overlapping descriptors with a `[WARN]` log. Real firmware (particularly with ACPI reclaim regions) can produce legitimate overlaps that should be resolved by priority, not skipped. This section adds sort + carve logic to normalize the map before the kernel PMM consumes it.

- [ ] Sort memory map descriptors by base address before overlap detection (insertion sort is fine for <= 512 entries)
- [ ] On overlap: resolve by UEFI memory type priority (Reserved > ACPI NVS > ACPI Reclaim > Runtime > Conventional) -- higher-priority type wins the contested range
- [ ] Carve contested ranges: split the lower-priority descriptor around the higher-priority one (may increase descriptor count by 1 per overlap)
- [ ] Guard against exceeding `BOOT_MMAP_MAX_ENTRIES` during carving -- if the carve would overflow, fall back to current skip+warn behavior
- [ ] Update `fill_memory_map()` to call sort + normalize before copying to `boot_info`
- [ ] Log normalized overlaps: `"[BOOT] mmap: overlap resolved at 0xNNNN -- type N wins over type M"`
- [ ] Commit: `"boot: memory map sort + overlap normalization by type priority"`

**Test checkpoint:** Normal boot produces no overlap warnings on clean firmware. Inject a synthetic overlap in QEMU (if feasible via firmware customization) and verify the sort + carve produces a clean, non-overlapping map. Kernel PMM `total_frames` and `used_frames` are consistent with the normalized map.

---

## 18. Graphical Error Screen (ChromeOS/Win11-Style)

The current error screen uses UEFI text console (`ConOut`) with white-on-blue text. ChromeOS shows a clean graphical recovery screen with a large icon, minimal text, and a QR code. Windows 11 BSOD uses a sad face `:(` with a stop code and QR. This section replaces the text-only error screen with a pixel-rendered graphical screen drawn directly to the GOP framebuffer.

> [!TIP]
> **Competitive advantage:** ChromeOS and Windows both render graphical error screens, but only at the OS level -- their UEFI-stage errors are plain text or invisible. Rendering a polished graphical error screen at the UEFI bootloader stage (before the OS loads) puts Impossible OS ahead of both competitors for pre-kernel diagnostics.
> **Regression risk:** LOW -- the text error screen via ConOut remains as fallback when GOP is unavailable. The graphical screen is additive.

- [ ] Render error screen directly to GOP framebuffer (solid blue background fill, no dependency on ConOut/Boot Services)
- [ ] Large sad face icon or warning triangle rendered as pixel art (16x16 or 32x32 bitmap scaled to 64x64+)
- [ ] Error title in large font: "Impossible OS could not start" (render using the existing boot splash font or a minimal embedded bitmap font)
- [ ] Error code prominently displayed: "Error 0x0003 -- Kernel not found"
- [ ] Human-readable description below the code: explain what went wrong and what the user can do (2-3 lines max)
- [ ] QR code in bottom-right (already implemented in §14)
- [ ] URL text below QR: "Scan for help: impossibleos.co/err/0003"
- [ ] If GOP unavailable: fall back to current ConOut text screen (§9 behavior preserved)
- [ ] ConOut text output still emitted in parallel for serial capture and headless servers
- [ ] Commit: `"boot: graphical error screen -- ChromeOS-style recovery UX"`

**Test checkpoint:** Trigger `error_screen_test=1`. Error screen renders a clean graphical layout with icon, error text, description, QR code, and URL. ConOut text still appears on serial. Headless mode (no GOP) shows text-only fallback. Compare visually against ChromeOS recovery and Windows 11 BSOD for polish level.

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
| 💎 | UEFI watchdog   | ✅ Re-arm via SetWatchdogTimer   | ✅ efi_stub disables watchdog   | ✅ §11 60s arm + disarm pre-EBS |
| 💎 | Mmap validate   | ✅ Descriptor version + size     | ✅ efi_stub sanity checks       | ✅ §12 align+pages+type+overlap |
| ⭐ | Error screen    | ❌ Generic BSOD (no boot ctx)    | ⚠️ GRUB text menu (no graphics) | ✅ §9 blue BSOD + key + reboot |
| ⭐ | NVRAM errors    | ⚠️ Opaque status codes           | ❌ No persistent boot errors    | ✅ §13 13 codes + NVRAM persist |
| ⭐ | Boot QR         | ❌ No UEFI-phase QR codes        | ❌ No GRUB QR support           | ✅ §14 QR V3 byte mode + scan   |
| ⭐ | Graphical error | ✅ :( BSOD (OS-level only)       | ❌ GRUB text menu only          | ⬜ §18 ChromeOS-style pixel UX  |
| 💎 | Mmap normalize  | ✅ Hal.dll coalesces overlaps    | ✅ efi_fake_memmap + sanitize   | ⬜ §17 sort + carve by priority |
| 💎 | Handoff ABI     | ✅ BCD signature + protocol      | ✅ Multiboot2 / Linux boot      | ✅ §15 magic+ver+size + halt    |
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
- [ ] §14: Boot failure error screen includes QR code in bottom-right. QR scans to `https://impossibleos.co/err/XXXX` with correct error code.
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
