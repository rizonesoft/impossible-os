# TODO-12 — Bootloader Error Recovery & ELF Hardening

> **Goal:** Eliminate every silent failure in `bootx64.c`. The bootloader currently has 15+ fragility points where errors cause infinite HLT loops with no visible output, corrupt memory from unchecked ELF segments, or silently use wrong defaults. After this TODO: every failure produces a visible error message on screen and serial with actionable information. The bootloader never hangs silently — it either boots or tells you exactly why it can't.

> [!IMPORTANT]
> **Current state:** Audit identified: ELF parser with zero bounds checking (can write to any address), ExitBootServices with only 1 retry (spec allows many), kernel missing = silent HLT, serial assumes COM1 exists, first filesystem protocol used blindly (wrong disk on multi-boot), memory map capped at 256 entries with silent truncation, GOP operations with no timeout, boot.conf missing = silent defaults. Every one of these has caused real boot failures on hardware.

---

## Inputs

- `src/boot/uefi/bootx64.c` — UEFI bootloader (2384 lines, 15+ fragility points)
- `src/boot/entry.asm` — 32-to-64-bit mode transition
- `include/kernel/boot_info.h` — boot data structures (boot_info at 0x10000)
- → XREF: `TODO-01-uefi-hardening-secureboot.md §7` — boot UX polish
- → XREF: `TODO-06-bare-metal-hardening.md §7` — resilient boot with graceful degradation
- → XREF: `TODO-13-boot-device-discovery.md §1` — boot device identification (uses filesystem protocol correctly)

---

## Outcome

- Every EFI call in `bootx64.c` has error handling with human-readable diagnostics on both serial and screen.
- ELF parser validates all header fields, segment offsets, and memory ranges before copying.
- ExitBootServices retries up to 5 times per UEFI specification.
- Missing kernel triggers fallback search across 3 paths before giving up.
- Serial port is probed before use; fallback to COM2 if COM1 absent.
- Memory map overflow detected and logged (cap raised to 512 entries).
- Fatal errors render a visible error screen with error code and recovery instructions.

---

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | ELF bounds checking                            | —          |  [ ]   |
| 💎  |   2   | ExitBootServices retry loop (5 attempts)       | —          |  [ ]   |
| 💎  |   3   | Fallback kernel search (3 paths)               | —          |  [ ]   |
| 💎  |   4   | Serial port probe and COM2 fallback            | —          |  [ ]   |
| 💎  |   5   | GOP timeout and multi-GPU enumeration          | —          |  [ ]   |
| 💎  |   6   | Memory map overflow detection (512 entries)    | —          |  [ ]   |
| 💎  |   7   | boot.conf validation and version field         | —          |  [ ]   |
| 💎  |   8   | Kernel load allocation fallback (32→16→8 MiB)  | —          |  [ ]   |
| ⭐  |   9   | Boot failure error screen                      | §1–§8      |  [ ]   |

> 💎 = parity — Windows bootmgfw.efi and GRUB2 both handle these error paths.
> ⭐ = exclusive — a visible, human-readable boot failure screen with recovery instructions; neither Windows nor Linux shows this level of detail at the UEFI stage.

---

## 1. ELF Bounds Checking

Harden the kernel ELF parser in `load_kernel()` to reject malformed or corrupted binaries.

- [ ] Validate `e_phoff` is within file bounds: `e_phoff + e_phnum * sizeof(Elf64_Phdr) <= file_size`
- [ ] Validate each `PT_LOAD` segment: `p_offset + p_filesz <= file_size`
- [ ] Validate `p_memsz >= p_filesz` (ELF spec requirement)
- [ ] Reject segments that overlap boot_info region (0x10000–0x11000)
- [ ] Reject segments that overlap framebuffer base address
- [ ] Cap total kernel size at 32 MiB — reject with clear error if exceeded
- [ ] Log each loaded segment: `"[BOOT] ELF segment %u: vaddr=0x%llx filesz=%u memsz=%u"` (already done, verify)
- [ ] On any validation failure: `"[FAIL] Kernel ELF corrupt: <reason>"` with specific error
- [ ] Commit: `"boot: harden ELF parser — bounds check all headers and segments"`

**Test checkpoint:** Build a test kernel with `e_phoff` pointing past EOF. Bootloader must reject with `"Kernel ELF corrupt: phdr offset past EOF"` on serial. Verify on QEMU WHPX and TCG.

**Regression risk:** LOW — additive validation. Existing valid kernels pass all checks. If a check false-positives, remove it specifically.

---

## 2. ExitBootServices Retry Loop

UEFI spec (Section 7.4.6) explicitly allows the memory map to change between GetMemoryMap and ExitBootServices. Real hardware with background events (thermal, EC) can cause repeated map_key mismatches.

- [ ] Replace current 1-retry with a loop of up to 5 attempts
- [ ] Each attempt: `GetMemoryMap()` → fresh `map_key` → `ExitBootServices(map_key)`
- [ ] Log each attempt: `"[BOOT] ExitBootServices attempt %u/5..."`
- [ ] On success: `"[BOOT] ExitBootServices OK (attempt %u)"`
- [ ] On final failure: `"[CRIT] ExitBootServices failed after 5 attempts (status=0x%x)"` then render error screen (§9)
- [ ] Re-populate boot_info memory map and runtime map on each retry (map may have changed)
- [ ] Commit: `"boot: ExitBootServices retry loop — 5 attempts per UEFI spec"`

**Test checkpoint:** Difficult to test directly (requires firmware that changes map between calls). Verify normal boot still succeeds on all 4 platforms. Serial output should show `"attempt 1/5"`.

**Regression risk:** MEDIUM — touches the most critical boot transition. If retry logic corrupts map_key, boot fails. Rollback: revert to 1-retry.

---

## 3. Fallback Kernel Search

If `\boot\kernel.exe` is not found, search alternative paths before giving up.

- [ ] Try paths in order: `\boot\kernel.exe` → `\kernel.exe` → `\EFI\ImpossibleOS\kernel.exe`
- [ ] Log each attempt: `"[BOOT] Trying %s..."` with path
- [ ] On success: `"[BOOT] Kernel found at %s"` and continue
- [ ] On all paths failing: `"[FAIL] Kernel not found. Searched: \boot\kernel.exe, \kernel.exe, \EFI\ImpossibleOS\kernel.exe"` then render error screen (§9)
- [ ] Use `LoadedImage->DeviceHandle` to get the boot device's filesystem (not `LocateProtocol` which returns an arbitrary filesystem)
- [ ] Commit: `"boot: fallback kernel search — 3 paths before failure"`

**Test checkpoint:** Rename `\boot\kernel.exe` to `\kernel.exe` on EFI partition. Boot must succeed with serial showing `"Trying \boot\kernel.exe... not found"` then `"Kernel found at \kernel.exe"`. Verify on QEMU TCG.

**Regression risk:** LOW — additive search paths. Default path unchanged.

---

## 4. Serial Port Probe and COM2 Fallback

Modern hardware (laptops, tablets) may not have COM1 at 0x3F8. Blindly initializing it can write to unrelated I/O ports.

- [ ] Before `serial_early_init()`: write 0xAE to scratch register (0x3F8+7), read back — if mismatch, COM1 absent
- [ ] If COM1 absent: try COM2 at 0x2F8 with same probe
- [ ] If both absent: set `serial_available = 0`, skip all serial output (no-op functions)
- [ ] Log selected port to boot_info: `boot_info.serial_port = 0x3F8 / 0x2F8 / 0`
- [ ] Kernel serial init reads `boot_info.serial_port` instead of hardcoding COM1
- [ ] Commit: `"boot: probe serial port before init — COM1/COM2 fallback"`

**Test checkpoint:** On QEMU (always has COM1), serial output works as before. On VirtualBox with serial disabled, bootloader skips serial silently. Verify no I/O port side effects on bare metal.

**Regression risk:** LOW — additive probe before existing init. If probe gives false negative, serial is just silent.

---

## 5. GOP Timeout and Multi-GPU Enumeration

GOP operations can hang on broken firmware. Multi-GPU systems may have the wrong GOP handle selected.

- [ ] Wrap `QueryMode()` calls in a counted loop: if 100 consecutive errors, abort mode enumeration
- [ ] If `SetMode()` fails, log the mode index and error code, try next-best mode
- [ ] Use `LocateHandleBuffer()` with GOP GUID to enumerate all GOP handles (not just first via `LocateProtocol`)
- [ ] Prefer the GOP handle associated with the active display (check `FrameBufferBase != 0`)
- [ ] If no GOP available at all: continue boot without display, set `boot_info.fb.base = 0`
- [ ] Log: `"[BOOT] GOP: %u handles found, using handle %u (%ux%u)"` with resolution
- [ ] Commit: `"boot: enumerate all GOP handles with timeout — prefer active display"`

**Test checkpoint:** Boot on QEMU (single GOP) — works as before. Serial shows `"GOP: 1 handles found"`. If available, test on multi-GPU VirtualBox config.

**Regression risk:** MEDIUM — changes how GOP is located. If `LocateHandleBuffer` returns handles in different order than `LocateProtocol`, display may be on wrong GPU. Rollback: revert to `LocateProtocol`.

---

## 6. Memory Map Overflow Detection

If firmware reports more memory regions than `BOOT_MMAP_MAX_ENTRIES` (currently 256), the array silently truncates.

- [ ] Increase `BOOT_MMAP_MAX_ENTRIES` from 256 to 512 in `boot_info.h`
- [ ] Before filling mmap array: compare descriptor count against max
- [ ] If exceeded: `"[WARN] Memory map has %u entries, truncating to %u"` on serial
- [ ] Set `boot_info.mmap_truncated = 1` flag so kernel knows the map is incomplete
- [ ] Kernel PMM should warn if `mmap_truncated` is set
- [ ] Commit: `"boot: detect memory map overflow — increase cap to 512, warn on truncation"`

**Test checkpoint:** Normal boot (typically ~130 entries) works as before. Add a `mmap_truncated` field to boot_info and verify kernel reads it.

**Regression risk:** LOW — increases array size (adds ~4 KiB to boot_info). Verify boot_info doesn't overflow its 4 KiB page at 0x10000.

---

## 7. boot.conf Validation

Malformed boot.conf should produce warnings, not silent misbehavior.

- [ ] Check file size: if > 4096 bytes, log `"[WARN] boot.conf too large (%u bytes), truncating"` and cap read
- [ ] After parsing each key=value: check key against whitelist of known keys
- [ ] Unknown keys: `"[WARN] boot.conf: unknown key '%s' (ignored)"` — don't silently drop
- [ ] Add `config_version=1` field — future boot.conf changes can key on version number
- [ ] Validate numeric values: `splash_timeout` must be 0–60; `debug` must be 0 or 1; etc.
- [ ] Out-of-range values: `"[WARN] boot.conf: %s=%s out of range, using default %u"` and clamp
- [ ] Commit: `"boot: validate boot.conf — warn on unknown keys and out-of-range values"`

**Test checkpoint:** Add `bogus_key=42` to boot.conf. Serial must show `"unknown key 'bogus_key'"`. Set `splash_timeout=999` — serial must show `"out of range, using default 3"`.

**Regression risk:** LOW — validation is additive. Valid configs produce no new warnings.

---

## 8. Kernel Load Memory Allocation Fallback

If 16 MiB contiguous allocation fails (fragmented memory), try smaller sizes.

- [ ] Try `AllocatePages(32 MiB)` first
- [ ] If fails: try 16 MiB, then 8 MiB
- [ ] Log actual allocation: `"[BOOT] Kernel buffer: %u MiB allocated"`
- [ ] If all fail: `"[FAIL] Cannot allocate kernel buffer (tried 32/16/8 MiB)"` then error screen (§9)
- [ ] After loading kernel: verify actual kernel file size fits in allocated buffer
- [ ] Verify allocated buffer doesn't overlap boot_info (0x10000) or framebuffer region
- [ ] Commit: `"boot: kernel allocation fallback — 32→16→8 MiB with overlap check"`

**Test checkpoint:** Normal boot works (kernel is ~1.5 MiB, fits in any allocation). Serial shows `"Kernel buffer: 32 MiB allocated"`.

**Regression risk:** LOW — tries larger first, same result as current behavior (which allocates 16 MiB).

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
- [ ] Commit: `"boot: visible error screen on fatal failures — no more silent halts"`

**Test checkpoint:** Delete `\boot\kernel.exe` from boot disk. Boot must show error screen with `"Kernel not found"` message and recovery instructions — not a black screen. Verify on QEMU WHPX.

**Regression risk:** LOW — replaces existing HLT loops. If error screen rendering crashes, falls back to HLT (same as before, no worse).

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11                      | 🐧 Linux                     | 🚀 Impossible OS              |
|----|----------------------------|----------------------------|---------------------------|----------------------------|
| 💎 | ELF/PE bounds checking     | ✅ winload validates PE    | ✅ GRUB validates ELF     | ⬜ §1                      |
| 💎 | ExitBootServices retry     | ✅ Multiple retries        | ✅ GRUB retries           | ⬜ §2                      |
| 💎 | Kernel fallback paths      | ✅ BCD store + recovery    | ✅ GRUB menu + rescue     | ⬜ §3                      |
| 💎 | Serial port detection      | ✅ ACPI-enumerated         | ✅ earlycon probe         | ⬜ §4                      |
| 💎 | Multi-GPU GOP handling     | ✅ WDDM driver selection   | ✅ efifb multi-head       | ⬜ §5                      |
| 💎 | Memory map overflow        | ✅ Dynamic allocation      | ✅ Growable buffer        | ⬜ §6                      |
| 💎 | Config validation          | ✅ BCD schema enforced     | ✅ grub.cfg syntax check  | ⬜ §7                      |
| 💎 | Allocation fallback        | ✅ Variable kernel sizes   | ✅ Dynamic loading        | ⬜ §8                      |
| ⭐ | Human-readable error screen | ❌ Generic UEFI error     | ⚠️ GRUB rescue text       | ⬜ §9 🚀                   |

After §1–§8, Impossible OS matches Windows and Linux bootloader error handling. §9 goes further — a visible, human-readable error screen at the UEFI stage with specific recovery instructions, which neither Windows (generic firmware error) nor Linux (GRUB rescue requires expertise) provides.

---

## Unit Tests

> Bootloader code runs pre-ExitBootServices in UEFI context -- not kernel test framework.
> Use `scripts/test-smoke.sh` serial pattern matching for boot-level validation.

- [ ] Add smoke test patterns to `scripts/test-smoke.sh`:
  - Serial line `"[BOOT] ELF segment"` present (§1 — ELF loader logs each segment)
  - Serial line `"ExitBootServices attempt 1/"` present (§2 — retry loop logs attempt)
  - Serial line `"[BOOT] Kernel found at"` present (§3 — fallback search logs selected path)
  - Absence of `"[FAIL] Kernel ELF corrupt"` on normal boot (§1 — no corruption)
  - Absence of `"[CRIT] ExitBootServices failed"` on normal boot (§2 — exit succeeds)
- [ ] Create `scripts/test-boot-elf-corrupt.sh`:
  - Build disk image, truncate `\boot\kernel.exe` to 512 bytes
  - Boot QEMU headless, capture serial
  - Assert serial contains `"[FAIL] Kernel ELF corrupt"` (§1 rejects truncated ELF)
  - Assert serial contains `"Kernel not found"` or error screen text (§9)
- [ ] Create `scripts/test-boot-missing-kernel.sh`:
  - Build disk image, delete `\boot\kernel.exe`
  - Boot QEMU headless, capture serial
  - Assert serial contains `"Trying \boot\kernel.exe... not found"` (§3 fallback search)
- [ ] Commit: `"test: add bootloader error recovery smoke tests"`

## Verification

- [ ] **ELF corruption test**: build test kernel with corrupted phdr → bootloader rejects with specific error message, not crash.
- [ ] **Missing kernel test**: delete `\boot\kernel.exe` → error screen shows "Kernel not found" with paths searched.
- [ ] **Normal boot regression**: all 4 platforms (QEMU WHPX, TCG, VBox, bare metal) boot cleanly with no new warnings in serial.
- [ ] **Serial probe test**: VirtualBox with serial disabled → bootloader skips serial silently, boot succeeds.
- [ ] **Memory map test**: verify `mmap_truncated` field is 0 on normal boot, logged correctly in kernel.
- [ ] Commit: `"boot: bootloader error recovery complete — zero silent failures"`
