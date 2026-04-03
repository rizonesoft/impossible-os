# TODO-02 -- Bootloader Error Recovery & ELF Hardening

> **Goal:** Eliminate every silent failure in `bootx64.c`. The bootloader currently has 15+ fragility points where errors cause infinite HLT loops with no visible output, corrupt memory from unchecked ELF segments, or silently use wrong defaults. After this TODO: every failure produces a visible error message on screen and serial with actionable information, serial port detection uses ACPI SPCR when available, the UEFI watchdog timer guards against hangs, memory map descriptors are validated for consistency, boot errors persist in NVRAM for cross-boot diagnostics, and the error screen includes a QR code for recovery. The bootloader never hangs silently -- it either boots or tells you exactly why it can't.

> [!IMPORTANT]
> **Current state:** Audit identified: ELF parser with zero bounds checking (can write to any address), ExitBootServices with only 1 retry (spec allows many), kernel missing = silent HLT, serial assumes COM1 exists, first filesystem protocol used blindly (wrong disk on multi-boot), memory map capped at 256 entries with silent truncation, GOP operations with no timeout, boot.conf missing = silent defaults. Every one of these has caused real boot failures on hardware.

---

## Inputs

- `src/boot/uefi/bootx64.c` -- UEFI bootloader (2384 lines, 15+ fragility points)
- `src/boot/entry.asm` -- 32-to-64-bit mode transition
- `include/kernel/boot_info.h` -- boot data structures (boot_info at 0x10000)
- `src/kernel/acpi.c` -- ACPI table parsing (SPCR table lookup for §10)
- → XREF: `TODO-01-uefi-hardening-secureboot.md §7` -- boot UX polish
- → XREF: `TODO-05-bare-metal-hardening.md §7` -- resilient boot with graceful degradation
- → XREF: `TODO-03-boot-device-discovery.md §1` -- boot device identification (uses filesystem protocol correctly)
- → XREF: `TODO-07-boot-diagnostics.md §5` -- panic forensic evidence struct; §13 here provides the bootloader-stage error codes that §5 persists across reboots
- → XREF: `TODO-07-boot-diagnostics.md §6` -- panic QR code; §14 here implements the UEFI-stage QR code before kernel handoff
- → XREF: `TODO-16-boot-watchdog.md §1` -- kernel-stage software watchdog; §11 here covers the UEFI-stage watchdog before ExitBootServices

---

## Outcome

- Every EFI call in `bootx64.c` has error handling with human-readable diagnostics on both serial and screen.
- ELF parser validates all header fields, segment offsets, and memory ranges before copying.
- ExitBootServices retries up to 5 times per UEFI specification.
- Missing kernel triggers fallback search across 3 paths before giving up.
- Serial port is probed before use; ACPI SPCR table consulted first, then I/O probe with COM1/COM2 fallback.
- Memory map overflow detected and logged (cap raised to 512 entries); descriptors validated for consistency.
- UEFI watchdog timer re-armed after disabling default to catch bootloader hangs.
- Fatal errors render a visible error screen with error code, recovery instructions, and QR code.
- Boot error codes persisted in UEFI NVRAM for next-boot diagnostics.

---

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | ELF bounds checking                            | --          |  [ ]   |
| 💎  |   2   | ExitBootServices retry loop (5 attempts)       | --          |  [ ]   |
| 💎  |   3   | Fallback kernel search (3 paths)               | --          |  [ ]   |
| 💎  |   4   | Serial port probe and COM2 fallback            | --          |  [ ]   |
| 💎  |   5   | GOP timeout and graceful degradation           | --          |  [ ]   |
| 💎  |   6   | Memory map overflow detection (512 entries)    | --          |  [ ]   |
| 💎  |   7   | boot.conf validation and version field         | --          |  [ ]   |
| 💎  |   8   | Kernel load allocation fallback (32--16--8 MiB) | --          |  [ ]   |
| ⭐  |   9   | Boot failure error screen                      | §1--§8     |  [ ]   |
| 💎  |  10   | ACPI SPCR serial port auto-detection           | §4         |  [ ]   |
| 💎  |  11   | UEFI watchdog timer management                 | --          |  [ ]   |
| 💎  |  12   | Memory map descriptor validation               | §6         |  [ ]   |
| ⭐  |  13   | Boot error code registry & NVRAM persistence   | §9         |  [ ]   |
| ⭐  |  14   | Error screen QR code                           | §9         |  [ ]   |

> 💎 = parity -- Windows bootmgfw.efi and GRUB2 both handle these error paths.
> ⭐ = exclusive -- visible error screen with recovery instructions, QR code, and NVRAM-persisted error codes; neither Windows nor Linux provides this level of pre-kernel diagnostic detail.

---

## 1. ELF Bounds Checking

Harden the kernel ELF parser in `load_kernel()` to reject malformed or corrupted binaries.

- [ ] Validate `e_phoff` is within file bounds: `e_phoff + e_phnum * sizeof(Elf64_Phdr) <= file_size`
- [ ] Validate each `PT_LOAD` segment: `p_offset + p_filesz <= file_size`
- [ ] Validate `p_memsz >= p_filesz` (ELF spec requirement)
- [ ] Reject segments that overlap boot_info region (0x10000--0x11000)
- [ ] Reject segments that overlap framebuffer base address
- [ ] Cap total kernel size at 32 MiB -- reject with clear error if exceeded
- [ ] Log each loaded segment: `"[BOOT] ELF segment %u: vaddr=0x%llx filesz=%u memsz=%u"` (already done, verify)
- [ ] On any validation failure: `"[FAIL] Kernel ELF corrupt: <reason>"` with specific error
- [ ] Commit: `"boot: harden ELF parser -- bounds check all headers and segments"`

**Test checkpoint:** Build a test kernel with `e_phoff` pointing past EOF. Bootloader must reject with `"Kernel ELF corrupt: phdr offset past EOF"` on serial. Verify on QEMU WHPX and TCG. Normal kernel must pass all checks on all 4 platforms (WHPX, TCG, VBox, bare metal).

**Regression risk:** LOW -- additive validation. Existing valid kernels pass all checks. If a check false-positives, remove it specifically.

---

## 2. ExitBootServices Retry Loop

UEFI spec (Section 7.4.6) explicitly allows the memory map to change between GetMemoryMap and ExitBootServices. Real hardware with background events (thermal, EC) can cause repeated map_key mismatches.

- [ ] Replace current 1-retry with a loop of up to 5 attempts
- [ ] Each attempt: `GetMemoryMap()` → fresh `map_key` → `ExitBootServices(map_key)`
- [ ] Log each attempt: `"[BOOT] ExitBootServices attempt %u/5..."`
- [ ] On success: `"[BOOT] ExitBootServices OK (attempt %u)"`
- [ ] On final failure: `"[CRIT] ExitBootServices failed after 5 attempts (status=0x%x)"` then render error screen (§9)
- [ ] Re-populate boot_info memory map and runtime map on each retry (map may have changed)
- [ ] Commit: `"boot: ExitBootServices retry loop -- 5 attempts per UEFI spec"`

**Test checkpoint:** Difficult to test directly (requires firmware that changes map between calls). Verify normal boot still succeeds on all 4 platforms. Serial output should show `"attempt 1/5"`.

**Regression risk:** MEDIUM -- touches the most critical boot transition. If retry logic corrupts map_key, boot fails. Rollback: revert to 1-retry.

---

## 3. Fallback Kernel Search

If `\boot\kernel.exe` is not found, search alternative paths before giving up.

- [ ] Try paths in order: `\boot\kernel.exe` → `\kernel.exe` → `\EFI\ImpossibleOS\kernel.exe`
- [ ] Log each attempt: `"[BOOT] Trying %s..."` with path
- [ ] On success: `"[BOOT] Kernel found at %s"` and continue
- [ ] On all paths failing: `"[FAIL] Kernel not found. Searched: \boot\kernel.exe, \kernel.exe, \EFI\ImpossibleOS\kernel.exe"` then render error screen (§9)
- [ ] Use `LoadedImage->DeviceHandle` to get the boot device's filesystem (not `LocateProtocol` which returns an arbitrary filesystem)
- [ ] Commit: `"boot: fallback kernel search -- 3 paths before failure"`

**Test checkpoint:** Rename `\boot\kernel.exe` to `\kernel.exe` on EFI partition. Boot must succeed with serial showing `"Trying \boot\kernel.exe... not found"` then `"Kernel found at \kernel.exe"`. Verify on QEMU TCG. Confirm default path works on all 4 platforms.

**Regression risk:** LOW -- additive search paths. Default path unchanged.

---

## 4. Serial Port Probe and COM2 Fallback

Modern hardware (laptops, tablets) may not have COM1 at 0x3F8. Blindly initializing it can write to unrelated I/O ports.

- [ ] Before `serial_early_init()`: write 0xAE to scratch register (0x3F8+7), read back -- if mismatch, COM1 absent
- [ ] If COM1 absent: try COM2 at 0x2F8 with same probe
- [ ] If both absent: set `serial_available = 0`, skip all serial output (no-op functions)
- [ ] Log selected port to boot_info: `boot_info.serial_port = 0x3F8 / 0x2F8 / 0`
- [ ] Kernel serial init reads `boot_info.serial_port` instead of hardcoding COM1
- [ ] Commit: `"boot: probe serial port before init -- COM1/COM2 fallback"`

**Test checkpoint:** On QEMU (always has COM1), serial output works as before. On VirtualBox with serial disabled, bootloader skips serial silently. Verify no I/O port side effects on bare metal.

**Regression risk:** LOW -- additive probe before existing init. If probe gives false negative, serial is just silent.

---

## 5. GOP Timeout and Graceful Degradation

GOP operations can hang on broken firmware. This section adds error recovery around GOP operations -- timeouts, fallback modes, and headless boot. Multi-GPU handle enumeration and primary display selection are owned by TODO-01 §12; this section adds the defensive wrappers around whatever GOP path is used.

> [!NOTE]
> **Scope boundary:** TODO-01 §12 owns `LocateHandleBuffer()` GOP enumeration, ConOut primary selection, and `boot_info.gop_handles[]`. This section owns timeout/error recovery: mode enumeration abort, SetMode fallback, headless-boot path. If TODO-01 §12 lands first, wrap its enumeration with these guards. If this lands first, wrap the existing `LocateProtocol` path.

- [ ] Wrap `QueryMode()` calls in a counted loop: if 100 consecutive errors, abort mode enumeration
- [ ] If `SetMode()` fails, log the mode index and error code, try next-best mode
- [ ] If no GOP available at all: continue boot without display, set `boot_info.fb.base = 0`
- [ ] Log: `"[BOOT] GOP: %u handles found, using handle %u (%ux%u)"` with resolution (or `"[BOOT] GOP: none found, headless boot"`)
- [ ] Commit: `"boot: GOP timeout and graceful degradation -- headless fallback"`

**Test checkpoint:** Boot on QEMU (single GOP) -- works as before. Serial shows `"GOP: 1 handles found"` (or `"headless boot"` if GOP absent). If available, test on multi-GPU VirtualBox config. Verify on bare metal -- firmware GOP behavior differs from emulated.

**Regression risk:** MEDIUM -- changes how GOP is located. If `LocateHandleBuffer` returns handles in different order than `LocateProtocol`, display may be on wrong GPU. Rollback: revert to `LocateProtocol`.

---

## 6. Memory Map Overflow Detection

If firmware reports more memory regions than `BOOT_MMAP_MAX_ENTRIES` (currently 256), the array silently truncates.

- [ ] Increase `BOOT_MMAP_MAX_ENTRIES` from 256 to 512 in `boot_info.h`
- [ ] Before filling mmap array: compare descriptor count against max
- [ ] If exceeded: `"[WARN] Memory map has %u entries, truncating to %u"` on serial
- [ ] Set `boot_info.mmap_truncated = 1` flag so kernel knows the map is incomplete
- [ ] Kernel PMM should warn if `mmap_truncated` is set
- [ ] Commit: `"boot: detect memory map overflow -- increase cap to 512, warn on truncation"`

**Test checkpoint:** Normal boot (typically ~130 entries) works as before on all 4 platforms. Add a `mmap_truncated` field to boot_info and verify kernel reads it. Verify on bare metal -- real firmware often has more entries than QEMU.

**Regression risk:** LOW -- increases array size (adds ~4 KiB to boot_info). Verify boot_info doesn't overflow its 4 KiB page at 0x10000.

---

## 7. boot.conf Validation

Malformed boot.conf should produce warnings, not silent misbehavior.

- [ ] Check file size: if > 4096 bytes, log `"[WARN] boot.conf too large (%u bytes), truncating"` and cap read
- [ ] After parsing each key=value: check key against whitelist of known keys
- [ ] Unknown keys: `"[WARN] boot.conf: unknown key '%s' (ignored)"` -- don't silently drop
- [ ] Add `config_version=1` field -- future boot.conf changes can key on version number
- [ ] Validate numeric values: `splash_timeout` must be 0--60; `debug` must be 0 or 1; etc.
- [ ] Out-of-range values: `"[WARN] boot.conf: %s=%s out of range, using default %u"` and clamp
- [ ] Commit: `"boot: validate boot.conf -- warn on unknown keys and out-of-range values"`

**Test checkpoint:** Add `bogus_key=42` to boot.conf. Serial must show `"unknown key 'bogus_key'"`. Set `splash_timeout=999` -- serial must show `"out of range, using default 3"`.

**Regression risk:** LOW -- validation is additive. Valid configs produce no new warnings.

---

## 8. Kernel Load Memory Allocation Fallback

If 16 MiB contiguous allocation fails (fragmented memory), try smaller sizes.

- [ ] Try `AllocatePages(32 MiB)` first
- [ ] If fails: try 16 MiB, then 8 MiB
- [ ] Log actual allocation: `"[BOOT] Kernel buffer: %u MiB allocated"`
- [ ] If all fail: `"[FAIL] Cannot allocate kernel buffer (tried 32/16/8 MiB)"` then error screen (§9)
- [ ] After loading kernel: verify actual kernel file size fits in allocated buffer
- [ ] Verify allocated buffer doesn't overlap boot_info (0x10000) or framebuffer region
- [ ] Commit: `"boot: kernel allocation fallback -- 32→16→8 MiB with overlap check"`

**Test checkpoint:** Normal boot works (kernel is ~1.5 MiB, fits in any allocation). Serial shows `"Kernel buffer: 32 MiB allocated"`.

**Regression risk:** LOW -- tries larger first, same result as current behavior (which allocates 16 MiB).

---

## 9. Boot Failure Error Screen

Replace all `for (;;) hlt;` loops with a visible error screen rendered using the UEFI console output protocol (still available pre-ExitBootServices) or the GOP framebuffer (if available).

- [ ] Create `boot_fatal(const CHAR16 *title, const CHAR16 *detail)` function
- [ ] Renders on UEFI console: red text with error code, description, and recovery steps
- [ ] Recovery steps: `"1. Check boot media is inserted"`, `"2. Verify \boot\kernel.exe exists"`, `"3. Press any key to reboot or power off"`
- [ ] If GOP is available: render a minimal error screen (white text on blue background, similar to BSOD)
- [ ] Wait for keypress before halting (using UEFI `ConIn->ReadKeyStroke` if available)
- [ ] Replace all 4 `for (;;) hlt;` instances with `boot_fatal()` calls
- [ ] Log error to serial before displaying screen
- [ ] Commit: `"boot: visible error screen on fatal failures -- no more silent halts"`

**Test checkpoint:** Delete `\boot\kernel.exe` from boot disk. Boot must show error screen with `"Kernel not found"` message and recovery instructions -- not a black screen. Verify on QEMU WHPX, TCG, and VBox. Verify on bare metal -- confirm ConIn keypress works on real keyboard.

**Regression risk:** LOW -- replaces existing HLT loops. If error screen rendering crashes, falls back to HLT (same as before, no worse).

---

## 10. ACPI SPCR Serial Port Auto-Detection

Modern firmware provides the ACPI Serial Port Console Redirection Table (SPCR) specifying the exact serial port address, baud rate, and terminal type. Windows and Linux both consult SPCR before falling back to I/O probing. §4's scratch-register probe is necessary as a fallback, but SPCR should be the primary detection method.

- [ ] Before §4 I/O probe: search ACPI config tables (via `gST->ConfigurationTable`) for SPCR signature `"SPCR"`
- [ ] If SPCR found: extract `BaseAddress.Address` for port I/O base, `BaudRate` field, `FlowControl`, `TerminalType`
- [ ] Store SPCR-detected port in `boot_info.serial_port` and `boot_info.serial_baud`
- [ ] Add `boot_info.serial_source` field: 0=none, 1=SPCR, 2=I/O-probe -- kernel can report how serial was discovered
- [ ] If SPCR address differs from COM1/COM2 (e.g., MMIO UART on ARM-like platforms): log `"[BOOT] SPCR: non-standard port at 0x%llx (MMIO), skipping"` and fall through to I/O probe
- [ ] Log: `"[BOOT] Serial: SPCR detected port=0x%x baud=%u"` or `"[BOOT] Serial: SPCR absent, falling back to I/O probe"`
- [ ] Kernel serial init honors `boot_info.serial_baud` from SPCR instead of hardcoding 115200
- [ ] Commit: `"boot: ACPI SPCR serial port auto-detection before I/O probe"`

**Test checkpoint:** On QEMU with `-device isa-debug-exit` (SPCR absent), fallback I/O probe activates and serial works as before. On QEMU OVMF with SPCR table present, serial log shows `"SPCR detected"`. Verify on bare metal -- real firmware may provide SPCR with non-standard baud rates; kernel must honor the SPCR baud.

**Regression risk:** LOW -- SPCR lookup is read-only; if table is absent or unparseable, falls through to existing §4 I/O probe unchanged.

---

## 11. UEFI Watchdog Timer Management

The UEFI firmware starts a 5-minute watchdog timer at boot. The current bootloader disables it immediately (`SetWatchdogTimer(0, ...)`). If the bootloader hangs (e.g., GOP negotiation on broken firmware, USB enumeration), there's no timeout -- infinite HLT. Re-arming the watchdog after disabling the default provides a safety net.

- [ ] After initial `SetWatchdogTimer(0, ...)`: re-arm with a 60-second timeout: `gBS->SetWatchdogTimer(60, 0x424F4F54, 0, NULL)` (code = "BOOT")
- [ ] Before `ExitBootServices()`: disable the watchdog (`SetWatchdogTimer(0, ...)`) -- no longer needed post-EBS
- [ ] If any pre-EBS operation takes > 30s (GOP, USB discovery, kernel load), reset the timer: `gBS->SetWatchdogTimer(60, ...)` to extend the window
- [ ] Log: `"[BOOT] Watchdog: armed (60s)"` at entry, `"[BOOT] Watchdog: disarmed"` before ExitBootServices
- [ ] On watchdog timeout: firmware resets the system automatically (UEFI spec behavior) -- combined with TODO-14 A/B rollback, this prevents infinite boot loops
- [ ] Commit: `"boot: re-arm UEFI watchdog timer as boot hang safety net"`

**Test checkpoint:** Normal boot completes in < 10s; watchdog is disarmed before ExitBootServices. Serial shows `"Watchdog: armed"` and `"Watchdog: disarmed"`. No unexpected reboots on all 4 platforms. Verify on bare metal -- real firmware watchdog behavior may differ from emulated; some firmware ignores the watchdog code parameter.

**Regression risk:** LOW -- if watchdog fires unexpectedly, system reboots (recoverable via A/B rollback). Worst case is premature reboot on very slow firmware; extend timeout to 120s if seen.

---

## 12. Memory Map Descriptor Validation

§6 handles memory map overflow. This section validates individual descriptors for consistency -- overlapping physical ranges, invalid memory types, and zero-length regions that can cause PMM corruption.

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

**Regression risk:** LOW -- validation is read-only. Invalid entries are stripped, not rejected. Quirky firmware boots with warnings rather than failure.

---

## 13. Boot Error Code Registry & NVRAM Persistence

Windows has BootStatusPolicy but error codes are opaque hex values without context. Linux has no bootloader-level error persistence. This section creates a structured error code system where each failure has a unique code, and the last-boot error is saved in UEFI NVRAM for next-boot diagnostics.

> [!TIP]
> **Competitive advantage:** Neither Windows nor Linux persists structured bootloader error codes in NVRAM. The next boot can display "Last boot failed: 0x0003 -- Kernel not found at \boot\kernel.exe" before trying again. Combined with TODO-14 A/B rollback and TODO-07 §5 panic forensics, this gives a complete cross-boot diagnostic chain.

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

**Regression risk:** LOW -- NVRAM writes are non-destructive (single variable). If NVRAM is full or read-only, write silently fails and boot continues.

---

## 14. Error Screen QR Code

Add a QR code to the boot failure error screen (§9) that encodes a recovery URL with the error code. Smartphones can scan it to get step-by-step recovery instructions. ChromeOS does this for recovery; neither Windows UEFI-stage errors nor Linux GRUB rescue provides it.

> [!TIP]
> **Competitive advantage:** A QR code on the pre-kernel error screen is actionable for non-technical users. Instead of "call support", they scan and get a page explaining exactly what error code 0x0003 means and how to fix it. ChromeOS has this for recovery; neither Win11 nor Linux has it at the UEFI bootloader stage.

- [ ] Implement minimal QR code encoder in bootloader (QR Version 2, 25x25 modules, alphanumeric mode -- fits `https://impossible.os/err/0003` in ~200 bytes of code)
- [ ] `boot_fatal()` renders QR code in bottom-right corner of the GOP error screen (if GOP available)
- [ ] QR payload: `https://impossible.os/err/XXXX` where `XXXX` is the hex error code from §13
- [ ] QR module size: 4x4 pixels minimum for scannability on 1280x720 resolution
- [ ] If GOP unavailable: skip QR code (console-only error screen has no pixel rendering)
- [ ] Reuse QR logic from TODO-07 §6 panic QR code if already implemented; otherwise implement standalone minimal encoder
- [ ] Commit: `"boot: QR code on boot error screen -- scan for recovery instructions"`

**Test checkpoint:** Trigger boot failure (delete kernel). Error screen shows QR code in bottom-right. Scan with phone -- URL resolves (or shows the encoded URL). Verify QR is scannable at 1280x720 and 1920x1080 resolutions.

**Regression risk:** LOW -- QR rendering is additive to §9 error screen. If QR encoder has a bug, error screen still shows text error message.

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11                      | 🐧 Linux                     | 🚀 Impossible OS              |
|----|----------------------------|----------------------------|---------------------------|----------------------------|
| 💎 | ELF/PE bounds checking     | ✅ winload validates PE    | ✅ GRUB validates ELF     | ⬜ §1                      |
| 💎 | ExitBootServices retry     | ✅ Multiple retries        | ✅ efi_stub retries       | ⬜ §2                      |
| 💎 | Kernel fallback paths      | ✅ BCD store + recovery    | ✅ GRUB menu + rescue     | ⬜ §3                      |
| 💎 | Serial port detection      | ✅ ACPI-enumerated         | ✅ earlycon probe         | ⬜ §4                      |
| 💎 | GOP graceful degradation  | ✅ Fallback driver         | ✅ efifb fallback         | ⬜ §5                      |
| 💎 | Memory map overflow        | ✅ Dynamic allocation      | ✅ Growable buffer        | ⬜ §6                      |
| 💎 | Config validation          | ✅ BCD schema enforced     | ✅ grub.cfg syntax check  | ⬜ §7                      |
| 💎 | Allocation fallback        | ✅ Variable kernel sizes   | ✅ Dynamic loading        | ⬜ §8                      |
| 💎 | ACPI SPCR serial config    | ✅ EMS via SPCR            | ✅ earlycon=SPCR          | ⬜ §10                     |
| 💎 | UEFI watchdog management   | ✅ Re-arms during boot     | ✅ efi_stub manages       | ⬜ §11                     |
| 💎 | Memory map validation      | ✅ Validates descriptors   | ✅ efi_stub checks        | ⬜ §12                     |
| ⭐ | Human-readable error screen | ❌ Generic UEFI error     | ⚠️ GRUB rescue text       | ⬜ §9 🚀                   |
| ⭐ | NVRAM error persistence    | ⚠️ BootStatusPolicy (opaque) | ❌ No persistence       | ⬜ §13 🚀                  |
| ⭐ | Error screen QR code       | ❌ No QR at UEFI stage    | ❌ No QR at GRUB stage    | ⬜ §14 🚀                  |

> **After parity items:** Impossible OS matches Windows and Linux on all bootloader error handling: ELF validation, ExitBootServices retry, kernel search fallback, ACPI SPCR serial detection, UEFI watchdog management, and memory map validation. The exclusive items push beyond: the human-readable error screen with QR code is actionable for non-technical users (scan to get step-by-step recovery), and NVRAM-persisted error codes give cross-boot diagnostics that neither Windows (opaque BootStatusPolicy) nor Linux (no bootloader error persistence) provides.

---

## Unit Tests

> Bootloader code runs pre-ExitBootServices in UEFI context -- not kernel test framework.
> Use `scripts/test-smoke.sh` serial pattern matching for boot-level validation.

- [ ] Add smoke test patterns to `scripts/test-smoke.sh`:
  - Serial line `"[BOOT] ELF segment"` present (§1 -- ELF loader logs each segment)
  - Serial line `"ExitBootServices attempt 1/"` present (§2 -- retry loop logs attempt)
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
- [ ] Commit: `"test: add bootloader error recovery smoke tests"`

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
- [ ] Commit: `"boot: bootloader error recovery complete -- zero silent failures"`
