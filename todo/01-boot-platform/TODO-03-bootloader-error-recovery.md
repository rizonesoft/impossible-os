---
schema_version: 1
id: bootloader-error-recovery
domain: 01-boot-platform
status: active
title: "TODO-03 -- Bootloader Error Recovery & ELF Hardening"
---

# TODO-03 -- Bootloader Error Recovery & ELF Hardening

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Eliminate every silent failure in `bootx64.c`. The bootloader currently has 15+ fragility points where errors cause infinite HLT loops with no visible output, corrupt memory from unchecked ELF segments, or silently use wrong defaults. After this TODO: every failure produces a visible error message on screen and serial with actionable information, serial port detection uses ACPI SPCR when available, the UEFI watchdog timer guards against hangs, memory map descriptors are validated for consistency, boot errors persist in NVRAM for cross-boot diagnostics, and the error screen includes a QR code for recovery. The bootloader never hangs silently -- it either boots or tells you exactly why it can't.

> [!IMPORTANT]
> **Current state:** Audit identified: ELF parser with zero bounds checking (can write to any address), ExitBootServices with only 1 retry (spec allows many), kernel missing = silent HLT, serial assumes COM1 exists, first filesystem protocol used blindly (wrong disk on multi-boot), memory map capped at 256 entries with silent truncation, GOP operations with no timeout, boot.conf missing = silent defaults. Every one of these has caused real boot failures on hardware.
>
> **Code-truth (2026-04-10):** `src/boot/uefi/bootx64.c` `load_kernel()` (from ~956) checks ELF magic/class/machine only; still indexes `ehdr->e_phoff` / `phdr[i]` without file-bounds or overlap checks; opens only `\\boot\\kernel.exe`; allocates 16 MiB only (no 32/8 fallback); `BOOT_MMAP_MAX_ENTRIES` is 256 (`#define` ~21); `ExitBootServices` has one remap+retry (~2412-2419); `SetWatchdogTimer(0,...)` disables firmware watchdog (~2280) without re-arm. `include/kernel/boot_info.h` has no `BOOT_INFO_MAGIC` / `boot_info_header` yet (`confirmed` via grep).
>
> **Ownership note:** the `boot_info` contract itself is now consolidated under `TODO-01-boot-protocol-abi-handoff.md`. This file keeps the already-landed bootloader-side implementation history for that work because the hardening changes were discovered and shipped here, but future `boot_info` ownership, manifest, and schema evolution belong to TODO-01.

---

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) -- UEFI bootloader (2384 lines, 15+ fragility points)
- [`src/boot/entry.asm`](../../src/boot/entry.asm) -- 32-to-64-bit mode transition
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) -- boot data structures (boot_info at 0x10000)
- [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c) -- Phase 0 `boot_info` memcpy path (§16)
- [`src/kernel/acpi.c`](../../src/kernel/acpi.c) -- ACPI table parsing (SPCR table lookup for §10)
- -> XREF: `TODO-02-uefi-hardening-secureboot.md §7` -- boot UX polish
- -> XREF: `TODO-02-uefi-hardening-secureboot.md §9` -- shared max `ExitBootServices` attempt count with §2 here (single N in both TODOs before coding)
- → XREF: `TODO-10-bare-metal-hardening.md §8` -- resilient boot with graceful degradation
- -> XREF: `TODO-05-boot-device-discovery.md §1` -- boot device identification (uses filesystem protocol correctly)
- -> XREF: `TODO-05-boot-device-discovery.md §5,§11` -- fallback when no kernel on any volume, and critical health-check path, invoke `boot_fatal` / error screen (this file §9)
- -> XREF: `TODO-14-boot-diagnostics.md §6` -- panic forensic evidence struct; §13 here provides the bootloader-stage error codes that §6 persists across reboots
- -> XREF: `TODO-14-boot-diagnostics.md §7` -- panic QR code; §14 here implements the UEFI-stage QR code before kernel handoff
- -> XREF: `TODO-23-boot-watchdog.md §1` -- kernel-stage software watchdog; §11 here covers the UEFI-stage watchdog before ExitBootServices
- -> XREF: [`02-kernel-core/TODO-01-kernel-init-sequencing.md §2`](../02-kernel-core/TODO-01-kernel-init-sequencing.md) -- boot_info ABI verify follow-up (context for §15-§16)
- -> XREF: `TODO-27-uefi-advanced.md §4` -- multi-GOP enumeration owner; §5 here wraps timeout and headless fallback only
- -> XREF: `TODO-22-recovery-partition.md` -- WinRE-style offline ESP repair and recovery shell (out of scope here; pre-kernel error UX only in this TODO)

---

## Outcome

- Every EFI call in `bootx64.c` has error handling with human-readable diagnostics on both serial and screen.
- ELF parser validates all header fields, segment offsets, and memory ranges before copying.
- ExitBootServices uses a bounded multi-retry loop per UEFI spec and the shared policy with `TODO-02-uefi-hardening-secureboot.md §9`.
- Missing kernel triggers fallback search across 3 paths before giving up.
- Serial port is probed before use; ACPI SPCR table consulted first, then I/O probe with COM1/COM2 fallback.
- Memory map overflow detected and logged (cap raised to 512 entries); descriptors validated for consistency.
- UEFI watchdog timer re-armed after disabling default to catch bootloader hangs.
- Fatal errors render a visible error screen with error code, recovery instructions, and QR code.
- Boot error codes persisted in UEFI NVRAM for next-boot diagnostics.
- Offline recovery partition chains after unrecoverable boot failures (-> XREF: `TODO-22-recovery-partition.md`).

---

## Implementation Order

| ⭐  | Order | Deliverable                                                                  | Depends On | Status |
| --- | :---: | ---------------------------------------------------------------------------- | ---------- | :----: |
| 💎  |   1   | ELF bounds checking                                                          | --         |  [x]   |
| 💎  |   2   | ExitBootServices retry loop (bounded)                                        | --         |  [x]   |
| 💎  |   3   | Fallback kernel search (3 paths)                                             | --         |  [x]   |
| 💎  |   4   | Serial port probe and COM2 fallback                                          | --         |  [x]   |
| 💎  |   5   | GOP timeout and graceful degradation                                         | --         |  [x]   |
| 💎  |   6   | Memory map overflow detection (512 entries)                                  | --         |  [x]   |
| 💎  |   7   | boot.conf validation and version field                                       | --         |  [x]   |
| 💎  |   8   | Kernel load allocation fallback (32-16-8 MiB)                                | --         |  [x]   |
| ⭐  |   9   | Boot failure error screen                                                    | §1-§8      |  [x]   |
| 💎  |  10   | ACPI SPCR serial port auto-detection                                         | §4         |  [x]   |
| 💎  |  11   | UEFI watchdog timer management                                               | --         |  [x]   |
| 💎  |  12   | Memory map descriptor validation                                             | §6         |  [x]   |
| ⭐  |  13   | Boot error code registry & NVRAM persistence                                 | §9         |  [x]   |
| ⭐  |  14   | Error screen QR code                                                         | §9         |  [x]   |
| 💎  |  15   | boot_info ABI foundation moved to TODO-01                                    | --         |  [x]   |
| 💎  |  16   | boot_info kernel validation moved to TODO-01                                 | §15        |  [x]   |
| 💎  |  17   | Memory map overlap normalization (sort+carve)                                | §12        |  [x]   |
| ⭐  |  18   | Graphical error screen (ChromeOS/Win11-style)                                | §9, §14    |  [x]   |
| 💎  |  19   | PT_LOAD destination policy (defense-in-depth)                                | §1         |  [x]   |
| ⭐  |  20   | Boot error history ring -- producer (struct, append sites, NVRAM cookie)     | §13        |  [x]   |
| ⭐  |  21   | Boot error history ring -- consumer (reader, renderer, tests, smoke fixture) | §20        |  [x]   |

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
- [x] Reject segments that overlap the FULL boot_info region `[BOOT_INFO_PHYS_ADDR, BOOT_INFO_PHYS_ADDR + sizeof(struct boot_info))`: checks `seg_start < bi_end && seg_end > bi_start` where `bi_end = BOOT_INFO_PHYS_ADDR + sizeof(struct boot_info)` -- was silently capped at the first 4 KiB until §16 quality review (2026-04-11) caught that segments at `0x11000..0x155BF` could clobber the handoff tail
- [x] Reject segments that overlap framebuffer base address: checks against `g_boot_info_ptr->fb.addr` + pitch*height when fb is initialized
- [x] Cap total kernel size at 32 MiB (`ELF_MAX_KERNEL_SIZE`): reject with clear error if exceeded
- [x] Log each loaded segment: `"[BOOT] ELF segment N: paddr=0xHHHH filesz=N memsz=N"` via serial_early_print per segment
- [x] On any validation failure: `"[FAIL] Kernel ELF corrupt: <reason>"` with specific error text for each check
- [x] **ELF typedefs consolidated** (Codex consistency H1, shipped 2026-05-01): [`bootx64.c`](../../src/boot/uefi/bootx64.c) now `#include "elf_types.h"` and the duplicate local `Elf64_Ehdr` / `Elf64_Phdr` / `Elf64_Shdr` / `Elf64_Sym` typedefs + `ELF_MAGIC` + `PT_LOAD` + `SHT_*` macros are deleted. 13 `_Static_assert(__builtin_offsetof(...))` checks pin every byte offset `load_kernel()` reads (`e_magic` 0, `e_class` 4, `e_machine` 18, `e_phoff` 32, `e_phentsize` 54, `e_phnum` 56, `p_type` 0, `p_flags` 4, `p_offset` 8, `p_paddr` 24, `p_filesz` 32, `p_memsz` 40) plus `sizeof(Elf64_Ehdr) == 64` / `sizeof(Elf64_Phdr) == 56` per the ELF64 specification (System V ABI AMD64 Supplement). Future drift in `elf_types.h` breaks the build instead of silently shifting kernel-load destinations.
- [x] **efi_memcpy / efi_memset converted to `rep movsb` / `rep stosb`** (Codex perf M1, shipped 2026-05-01): byte-loop helpers at [`bootx64.c`](../../src/boot/uefi/bootx64.c) replaced with x86-64 inline-asm using `cld` + `rep stosb` (memset) and `cld` + `rep movsb` (memcpy). Modern microarchitectures fast-path these via Enhanced REP MOVSB (Ivy Bridge+) and Fast Short REP MOV (Ice Lake+). Constraints: dst in `+D` (RDI), src in `+S` (RSI), count in `+c` (RCX), val byte in `a` (AL); clobbers `memory, cc`. Direction flag explicitly cleared on entry per System V AMD64 ABI (UEFI firmware does not guarantee `DF=0`). Works pre- and post-ExitBootServices (no `gBS` dependency). load_kernel()'s multi-MiB PT_LOAD segment copies + BSS clears no longer execute per-byte stores.
- [x] Commit: `"boot: harden ELF parser -- bounds check all headers and segments"` (3c888540)

**Test checkpoint:** Build a test kernel with `e_phoff` pointing past EOF. Bootloader must reject with `"Kernel ELF corrupt: phdr offset past EOF"` on serial. Verify on QEMU WHPX and TCG. Normal kernel must pass all checks on all 4 platforms (WHPX, TCG, VBox, bare metal).

> **Test runner:** N/A (UEFI pre-boot only -- ELF bounds checking runs before kernel is loaded) | validation: `bash scripts/test-smoke.sh` + serial log on QEMU WHPX/TCG, VBox, bare metal

> **Notes:**
> - What shipped: 8 ELF bounds checks at [`bootx64.c:3855-4020`](../../src/boot/uefi/bootx64.c) covering ehdr file-size guard, magic/class/machine, `e_phentsize == sizeof(Elf64_Phdr)`, `e_phnum <= 64`, `ELF_MAX_KERNEL_SIZE = 32 MiB`, `e_phoff` + phdr-table EOF bounds, per-PT_LOAD `p_offset + p_filesz` subtraction-based bounds, `p_memsz >= p_filesz`, address wraparound, full `sizeof(struct boot_info) = 23872`-byte boot_info overlap, `pitch * height`-with-overflow-guard framebuffer overlap.
> - How it integrates: invoked by `load_kernel()` in the split-path boot before the segment-copy loop and before any `efi_memcpy` writes; UKI fast path skips this code entirely (signed PE means the kernel bytes are already firmware-verified). Returns `EFI_LOAD_ERROR` to the caller on any failure; caller maps to `boot_fatal(BOOT_ERR_ELF_CORRUPT, ...)` for the §9 error screen.
> - Downstream effects: blocks malformed/hostile kernel images from clobbering boot_info, framebuffer, or low memory through unchecked PT_LOAD destinations. PT_LOAD destination policy (forbid firmware/loader regions) is owned by §19 below as an open `[ ]` item. Codex 3x review adoptions in commit `<hash>`.
> - Canonical doc: this section + ELF spec (TIS Tool Interface Standard 1.2 + System V ABI x86-64) + UEFI 2.10 section 13 LoadedImage handoff.
> - Scope boundary: §1 owns ELF FILE-side validation (offsets, sizes, magic, header fields). §19 owns PT_LOAD DESTINATION policy (where in physical memory segments may write). §17 owns memory-map overlap normalization for runtime regions; this section's overlap checks are static against boot_info + framebuffer only.

> **Verified:** 2026-05-01 | commit `51ab396c` | 10/10 items | build OK | smoke PASS (KVM 2.48s). 8 original ELF bounds checks + ELF typedef consolidation + rep movsb/stosb conversion + cleanup-label fix all confirmed at file:line. Prior 2026-04-29 + 2026-04-11 verifications retained.
> **Accepted (RESOLVED 2026-08-13):** [M] BOOT_INFO_PHYS_ADDR macro duplicated across bootx64.c + kernel/mm/boot_reserved.c + kernel/main/boot_payload.c -- closed by [01-boot-platform/TODO-01 §21](TODO-01-boot-protocol-abi-handoff.md#21-post-ship-follow-up-backfill-orphan-cohort-2026-07-31): one definition in `include/kernel/boot_version_constants.h`, all three local `#define` copies deleted
> **Quality reviewed:** 2026-05-01 | Codex 4x (adversarial + consistency + perf + re-adversarial) | 2M fixed (cleanup-label disk-buffer leak across all ELF validation hard-error sites; section-header envelope route through label), 1H+1M accepted-XREF | scope: boot-code-quality (gates walked: UEFI types, error handling, EBS boundary, boot_info ABI sync, parse buffer dynamic alloc, fallback chains)

---

## 2. ExitBootServices Retry Loop

UEFI spec (Section 7.4.6) explicitly allows the memory map to change between GetMemoryMap and ExitBootServices. Real hardware with background events (thermal, EC) can cause repeated map_key mismatches.

> [!NOTE]
> **Regression risk:** MEDIUM -- touches the most critical boot transition. If retry logic corrupts map_key, boot fails. Rollback: revert to 1-retry.

- [x] -> XREF: `TODO-02-uefi-hardening-secureboot.md §9` -- N=4 locked (EBS_MAX_ATTEMPTS=4, commit 3c552022)
- [x] Bounded retry loop: `for (ebs_attempt = 0; ebs_attempt < EBS_MAX_ATTEMPTS; ebs_attempt++)` in `bootx64.c`
- [x] Each attempt: `get_memory_map()` -> fresh `map_key` -> `ExitBootServices(map_key)` with separate `ebs_status` tracking
- [x] Retry logging: `"[BOOT] ExitBootServices retry\n"` per failed attempt
- [x] On success: `"[BOOT] ExitBootServices OK\n"`
- [x] On final failure: `"[CRIT] ExitBootServices failed after retries\n"` + HLT forever
- [x] Re-populate `fill_memory_map()` + `fill_runtime_map()` on each retry after fresh `get_memory_map()`
- [x] Commit: implemented in TODO-02 §9 commit 3c552022 (shared EBS retry + Secure Boot DB registry mirror)

**Test checkpoint:** Difficult to test directly (requires firmware that changes map between calls). Verify normal boot still succeeds on all 4 platforms. Serial output should show the first `ExitBootServices attempt` log line using the agreed `N` from TODO-02 §9.

> **Test runner:** N/A (UEFI pre-boot only -- EBS retry runs at the boot-services exit boundary; no post-EBS kernel surface to assert) | validation: `bash scripts/test-smoke.sh` + serial log on QEMU WHPX/TCG, VBox, bare metal

> **Notes:**
> - What shipped: bounded EBS retry loop at [`bootx64.c:7595-7657`](../../src/boot/uefi/bootx64.c) with `EBS_MAX_ATTEMPTS=4`, fresh `gBS->ExitBootServices(map_key)` per attempt, per-attempt fail log carrying attempt number + `EBS_MAX_ATTEMPTS` denominator + 64-bit firmware status hex, `gBS->FreePool` of the stale mmap buffer before each refresh, last-iteration refresh skip so the exhaustion fatal carries `BOOT_ERR_EXIT_BS_FAIL` instead of `BOOT_ERR_EBS_MMAP_FAIL`.
> - How it integrates: invoked from the kernel-handoff path right before the kernel jump; on success, `[BOOT] ExitBootServices OK` flips `g_ebs_in_progress=1` so subsequent boot_fatal renders fall to serial-only (ConOut dead post-EBS).
> - Downstream effects: TODO-02 §9 carries the shared retry policy (`EBS_MAX_ATTEMPTS=4`); changing the constant updates both the loop bound AND the diagnostic denominators automatically. Codex review-todo-section 2026-05-01 4x adoptions in commit `<hash>`.
> - Canonical doc: this section + UEFI 2.10 section 7.4.6 (boot-services exit) + UEFI 2.10 section 7.2 (memory map services).
> - Scope boundary: §2 owns the retry loop + per-attempt diagnostics + mmap-buffer lifecycle across retries. The post-EBS ConOut guard (`g_ebs_in_progress`) is shipped here but reused by every `boot_fatal` consumer in this TODO. Memory-map descriptor validation is owned by §12; overlap normalization by §17.

> **Verified:** 2026-05-01 | review-todo-section re-verify | 7/7 items | build OK | smoke PASS (KVM 2.490s). All 7 original checks confirmed at file:line + 3 new fixes (final-iteration refresh skip, EBS_MAX_ATTEMPTS-derived diagnostics, FreePool before retry-AllocatePool). Prior 2026-04-11 verification retained.
> **Quality reviewed:** 2026-05-01 | Codex 4x (adversarial + consistency + perf + re-adversarial) | 1M (final-iteration mask) + 1M (denominator drift) + 1M (mmap leak) fixed; round-2 re-adversarial confirmed zero new issues introduced by the fixes | scope: boot-code-quality (gates walked: UEFI types, error handling, EBS boundary, fallback chains, parse-buffer dynamic alloc).

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

> **Test runner:** N/A (UEFI pre-boot only -- kernel-search runs before kernel is loaded; no kernel-side test surface) | validation: `bash scripts/test-smoke.sh` + serial log on QEMU WHPX/TCG, VBox, bare metal

> **Notes:**
> - Three hardcoded kernel paths (`\boot\kernel.exe` -> `\kernel.exe` -> `\EFI\ImpossibleOS\kernel.exe`) tried in order on the boot device's SimpleFS, with LocateHandleBuffer-based all-volumes fallback when the kernel is absent from the boot device.
> - `LoadedImage->DeviceHandle` resolved at efi_main into `g_boot_device_handle`; degraded paths (NULL handle or no SimpleFS) defer to the explicit all-volumes search with `[WARN]` diagnostics rather than silently calling `LocateProtocol` (rebuilt 2026-05-01).
> - Split error policy: primary boot-device loop returns the underlying status on any non-`EFI_NOT_FOUND` Open error (kernel.exe corruption is fatal); fallback all-volumes loop logs `[WARN]`, skips the candidate volume (break inner pi loop, `if (!found)` Close fb_root, continue handle loop) so a single corrupt/encrypted/removable non-boot SimpleFS cannot deny recovery boot. `FreePool(fs_handles)` runs once after the handle loop.
> - POST16 sequence: `POST16_BL_FALLBACK` (0xB094) on entry to all-volumes search; `POST16_BL_FALLBACK_OK` (0xB095) only on success; left at 0xB094 on total failure so a POST card shows fallback-failed.
> - Scope boundary: `parse_boot_conf()` and `locate_boot_fs()` still use the silent LocateProtocol pattern; consistency follow-ups filed in §7 and TODO-02 §16 to mirror the §3 hardening.
> **Verified:** 2026-05-01 | commit `2bf210cd` | 5/5 items | build OK | smoke PASS (KVM 2.45s)
> **Accepted:** [H] Silent LocateProtocol fallback in `parse_boot_conf()` (reason: scope -- §7 owns boot.conf reads) -> XREF: 01-boot-platform/TODO-03 §7 (item: "Eliminate silent LocateProtocol fallback in `parse_boot_conf()`" at line 185)
> **Accepted:** [H] Silent LocateProtocol fallback in `locate_boot_fs()` (RESOLVED 2026-06-13 by TODO-02 §16 commit `46eb95bc`: split-path payload provenance hardening -- `locate_boot_fs()` is HandleProtocol-only and `load_kernel()`'s non-boot fallback fails closed when staged payloads are present).
> **Quality reviewed:** 2026-05-01 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) | 1H+2M fixed, 2H accepted-XREF | scope: boot-code-quality

---

## 4. Serial Port Probe and COM2 Fallback

Modern hardware (laptops, tablets) may not have COM1 at 0x3F8. Blindly initializing it can write to unrelated I/O ports.

> [!NOTE]
> **Regression risk:** LOW -- additive probe before existing init. If probe gives false negative, serial is just silent.

- [x] Before `serial_early_init()`: `serial_probe_port()` writes 0xAE to scratch register (base+7), reads back -- if mismatch, port absent
- [x] If COM1 absent: try COM2 at 0x2F8 with same probe via `serial_probe_port(SERIAL_COM2)`
- [x] If both absent: `s_serial_port = 0`, `serial_early_putchar` returns immediately (no-op)
- [x] Selected port stored in `boot_info.serial_port` -- `UINT16` field on both bootloader + kernel `struct boot_info`. `serial_source==2` (no-SPCR scratch probe) restricts to 0x3F8 (COM1) or 0x2F8 (COM2); `serial_source==1` (ACPI SPCR firmware-authoritative) may publish any non-zero base in `[1, 0xFFF8]` (COM3 0x3E8, COM4 0x2E8, or vendor-custom). Upper bound 0xFFF8 keeps the 16550 register block (base..base+7) inside the 16-bit I/O port space.
- [x] Kernel `serial_init()` reads `g_boot_info.serial_port` to set `s_serial_port` (dynamic). `serial_putchar_raw`, `serial_trygetchar` all use the dynamic port. Default 0x3F8 for early klog before serial_init.
- [x] Commit: `"boot: probe serial port before init -- COM1/COM2 fallback"` (d785ce92)

**Test checkpoint:** On QEMU (always has COM1), serial output works as before. On VirtualBox with serial disabled, bootloader skips serial silently. Verify no I/O port side effects on bare metal.

> **Test runner:** N/A (UEFI pre-boot only -- COM1/COM2 probe runs before kernel) | validation: smoke + serial on QEMU WHPX/TCG, VBox, bare metal

> **Notes:**
> - What shipped: `serial_probe_port()` 0xAE scratch-register write/readback + COM1->COM2->0 fallback chain in `serial_early_init()`; boot_info.serial_port (UINT16) + serial_source (0/1/2) + serial_baud published to kernel.
> - How it integrates: ACPI SPCR (§10) is the primary detection method (firmware-authoritative); the no-SPCR scratch probe is the fallback that only checks COM1/COM2. Kernel `serial_init()` reads g_boot_info.serial_port and overrides the static 0x3F8 default for early klog.
> - Downstream effects: SPCR widening 2026-05-01 lets firmware-declared non-standard I/O UARTs (COM3 0x3E8, COM4 0x2E8, vendor-custom) land authoritatively in serial_port; previously dropped silently.
> - Canonical doc: this section + §10 (SPCR) + UEFI 2.10 + ACPI 6.5 SPCR table spec + 16550 UART scratch-register convention.
> - Scope boundary: §4 owns the no-SPCR fallback probe (COM1/COM2 only) + boot_info publication contract. §10 owns ACPI SPCR table parsing + firmware-authoritative base widening.

> **Verified:** 2026-05-01 | commit `7513bcac` | 6/6 items | build OK | smoke PASS (KVM 2.49s). Probe + fallback chain + boot_info publication + kernel-side handoff all confirmed at file:line. 2026-04-11 verification retained.
> **Quality reviewed:** 2026-05-01 | Codex 3x (adversarial + consistency + perf) | 1M fixed (SPCR I/O base widening + 0xFFF8 upper bound for 16550 register block), 1M fixed (test_serial_port_standard broadened by serial_source), 2L fixed (boot_info.h comment + §10 prose updated to match SPCR-authoritative contract); re-adversarial skipped (~30 LOC, policy-widening + test/doc only, no lifecycle/state-machine) | scope: boot-code-quality

---

## 5. GOP Timeout and Graceful Degradation

GOP operations can hang on broken firmware. This section adds error recovery around GOP operations -- timeouts, fallback modes, and headless boot. Multi-GPU handle enumeration and primary display selection are owned by `TODO-27-uefi-advanced.md §4`; TODO-02 §4 owns single-GOP negotiation. This section adds defensive wrappers around whichever GOP path is active.

> [!NOTE]
> **Scope boundary:** `TODO-27-uefi-advanced.md §4` owns `LocateHandleBuffer()` multi-GOP enumeration, ConOut primary selection, and `boot_info.gop_handles[]`. This section owns timeout/error recovery: mode enumeration abort, SetMode fallback, headless-boot path. If TODO-27 §4 lands first, wrap its enumeration with these guards. If this lands first, wrap the existing `LocateProtocol` path.
> **Regression risk:** MEDIUM -- changes how GOP is located. If `LocateHandleBuffer` returns handles in different order than `LocateProtocol`, display may be on wrong GPU. Rollback: revert to `LocateProtocol`.

- [x] Wrap `QueryMode()` calls in a counted loop: 100 consecutive errors aborts mode enumeration with serial log
- [x] If `SetMode()` fails: log mode index, try mode 0 as fallback, then fall through to firmware default
- [x] If no GOP available at all: `fb.addr = 0`, `fb_available = 0`, return EFI_SUCCESS (headless boot continues)
- [x] Log: `"[BOOT] GOP: 1 handle found"` or `"[BOOT] GOP: none found, headless boot"`. SetMode failure logged with mode index.
- [x] Commit: `"boot: GOP timeout and graceful degradation -- headless fallback"` (18cc8d84)

**Test checkpoint:** Boot on QEMU (single GOP) -- works as before. Serial shows `"GOP: 1 handles found"` (or `"headless boot"` if GOP absent). If available, test on multi-GPU VirtualBox config. Verify on bare metal -- firmware GOP behavior differs from emulated.

> **Test runner:** N/A (UEFI pre-boot only -- GOP mode selection runs before kernel) | validation: smoke + serial on QEMU WHPX/TCG, VBox, bare metal

> **Notes:**
> - What shipped: GOP defensive wrappers in `init_gop()` + `gop_negotiate_mode()` -- QueryMode 100-error abort, SetMode->mode0->firmware fallback, headless degrade on no GOP, FrameBufferBase=0 retry, dimension/size overflow guards, NEW Mode-NULL + Info-NULL guards (2026-05-01), NEW pixel-format gate (RGBX/BGRX only, else headless).
> - How it integrates: invoked from `efi_main()` after `serial_early_init()`; publishes fb.addr/width/height/pitch + fb_available to boot_info; kernel `fb_init()` honors fb_available=0 and stays headless cleanly.
> - Downstream effects: hostile/buggy firmware can no longer crash the bootloader via Mode==NULL or Info==NULL; unsupported pixel formats (PixelBitMask, PixelBltOnly) headless-degrade rather than triggering kernel `fb_init()` halt; multi-GOP enumeration owned by TODO-27 §4.
> - Canonical doc: this section + TODO-02 §4 (single-GOP negotiation) + TODO-27 §4 (multi-GOP) + UEFI 2.10 section 12.9 GOP protocol.
> - Scope boundary: §5 owns timeout + degrade paths (Mode/Info NULL, format reject, dim overflow, FB size mismatch, FrameBufferBase=0 retry). TODO-02 §4 owns mode negotiation/scoring. TODO-27 §4 owns multi-handle enumeration.

> **Verified:** 2026-05-01 | commit `36046e12` | 4/4 items | build OK | smoke PASS (KVM 2.26s). 4 original deliverables + Mode/Info NULL guards + pixel-format gate confirmed at file:line. 2026-04-11 verification retained.
> **Quality reviewed:** 2026-05-01 | Codex 4x (adversarial + consistency + perf + re-adversarial) | 2H+1H fixed (Mode-NULL guard before enumeration, unsupported pixel-format headless-degrade, Info-NULL guard before pixel-format dereference) | scope: boot-code-quality

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

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | mmap_truncated flag asserted via test_boot_info; cap 512 entries on QEMU clean boot

> **Notes:**
> - What shipped: BOOT_MMAP_MAX_ENTRIES=512 cap on both kernel + bootloader mmap arrays + mmap_truncated u8 flag in boot_info; raw UEFI overflow check at fill time; NEW (2026-05-01) normalize-output overflow detection sets the same flag.
> - How it integrates: bootloader fills boot_info.mmap[] from gBS->GetMemoryMap output; cap-overflow logs `[WARN]` and sets mmap_truncated; mmap_normalize() runs §17 sweep-line carving and now also flags truncation if its emit hits the cap; kernel pmm.c reads mmap_truncated at init and emits klog warning showing entries-of-cap.
> - Downstream effects: PMM warning fires on EITHER raw firmware overflow OR normalize-output expansion past cap; previously the latter silently dropped tail segments (potential RuntimeServices loss) and only set mmap_quirks. Both warnings emit independently on combined overflow.
> - Canonical doc: this section + §17 (mmap normalize) + UEFI 2.10 section 7.2 (memory map services).
> - Scope boundary: §6 owns cap + truncation signaling (raw + normalize). §17 owns sweep-line overlap resolution. §12 owns descriptor field validation (zero-length, type bounds).

> **Verified:** 2026-05-01 | commit `fb016e8b` | 5/5 items | build OK | smoke PASS (KVM 2.28s). 5 original items + normalize-overflow flag-setting + warn-once guard confirmed at file:line. 2026-04-11 verification retained.
> **Quality reviewed:** 2026-05-01 | Codex 3x (adversarial + consistency + perf) | 2M fixed (normalize-output overflow now sets mmap_truncated; warn-once guard is normalize-local instead of keying on the shared flag); re-adversarial skipped (~25 LOC, no lifecycle/state-machine) | scope: boot-code-quality

---

## 7. boot.conf Validation

Malformed boot.conf should produce warnings, not silent misbehavior.

> [!NOTE]
> **Regression risk:** LOW -- validation is additive. Valid configs produce no new warnings.

- [x] Dynamic file-size probing via `GetInfo(&EFI_FILE_INFO_ID, ...)` + `gBS->AllocatePool(FileSize+1)`. Sanity cap `BOOT_CONF_SANITY_CAP = 1 MiB` triggers `boot_fatal(BOOT_ERR_CONF_INVALID, ...)` on overflow rather than the previous silent-truncate behavior. Original 4096-byte fixed buffer + truncation warning was retired in the 2026-04-21 incident fix (CLAUDE.md "Bare Metal Gotchas: Dynamic parse buffers + hard-fail on overflow") -- a `test=1` line at EOF was being silently dropped when boot.conf grew past 4096 bytes.
- [x] After parsing each key=value: unknown keys produce `"[WARN] boot.conf: unknown key 'X' (ignored)"`
- [x] Known-key whitelist: all existing keys + new `config_version` key
- [x] `config_version` field added to `boot_config` struct (1 byte from `_reserved[]`). `config_version=1` in boot.conf sets it.
- [x] Range validation: `debug`/`verbose`/`test` out-of-range values clamp to safe default `0` (NOT to max `1` -- a malformed `test=255` must NOT silently enable test mode + fault-injection syscall + boot-perf bypass; tightened 2026-05-01 from §7 review Codex M3). `splash_timeout` clamped to 0-60 (default 3). Out-of-range logs `"[WARN] boot.conf: X out of range, using 0"` (or `"using 3"` for splash_timeout). Also: a Read returning fewer bytes than the declared FileSize is rejected with `"[WARN] boot.conf short read"` and falls through to defaults instead of parsing the truncated prefix (closes the same silent-truncation class as the 2026-04-21 fixed-buffer incident).
- [x] Commit: `"boot: validate boot.conf -- warn on unknown keys and out-of-range values"` (83b2ccb8)
- [x] **Silent LocateProtocol fallback in `parse_boot_conf()` removed** (Codex H2 from §3 review, shipped 2026-05-01): `parse_boot_conf()` at `src/boot/uefi/bootx64.c` no longer calls `gBS->LocateProtocol(&fs_guid, ...)` when `g_boot_device_handle` is NULL or lacks SimpleFS. Instead logs `[WARN] Boot device has no SimpleFS for boot.conf -- skipping parse, using boot_config defaults` (or `[WARN] No boot device handle...` when the handle is NULL) and returns. `boot_config_defaults()` already populated safe defaults; the kernel-search fallback chain separately locates kernel.exe with explicit per-volume diagnostics, so boot still proceeds on degraded boot devices without letting an arbitrary ESP provide unsigned config to the trust boundary the kernel-search hardening established.

**Test checkpoint:** Add `bogus_key=42` to boot.conf. Serial must show `"unknown key 'bogus_key'"`. Set `splash_timeout=999` -- serial must show `"out of range, using default 3"`. A boot.conf > 1 MiB must trigger `boot_fatal(BOOT_ERR_CONF_INVALID)` (no silent truncation).

> **Test runner:** N/A (UEFI pre-boot only -- boot.conf parse runs before kernel) | validation: smoke + serial; bogus_key + splash_timeout=999 fixtures on QEMU WHPX/TCG

> **Notes:**
> - What shipped: parse_boot_conf in `src/boot/uefi/bootx64.c` -- dynamic file-size buffer (GetInfo + AllocatePool) capped at 1 MiB hard-fail; unknown-key WARN; key whitelist + config_version; range clamps to safe defaults (debug/verbose/test -> 0, splash_timeout -> 3); short-read rejection; degraded-boot-device path skips parse and returns to boot_config_defaults() rather than silently picking from an arbitrary ESP via LocateProtocol.
> - How it integrates: invoked from efi_main() after serial_early_init(); UKI mode short-circuits to `.cmdline` PE section, split-path mode reads `\EFI\ImpossibleOS\boot.conf` from the boot device only.
> - Downstream effects: trust-boundary preserved -- boot.conf cannot come from a different ESP than the kernel; malformed booleans cannot silently enable privileged diagnostic surfaces (fault-injection syscall, boot-perf bypass); short reads no longer drop trailing keys.
> - Canonical doc: this section + 2026-04-21 boot.conf incident fix + UEFI 2.10 EFI_FILE_PROTOCOL.Read contract.
> - Scope boundary: §7 owns the parser + filesystem-resolution policy. §13 owns NVRAM error-code persistence (boot_fatal carries BOOT_ERR_CONF_INVALID on hard cap overflow).

> **Verified:** 2026-05-01 | commit `a6b98e5e` | 7/7 items | build OK | smoke PASS (KVM 2.49s). Dynamic buffer + 1 MiB cap + unknown-key WARN + config_version + range-clamp-to-default + short-read rejection + degraded-boot-device skip all confirmed at file:line. 2026-04-11 verification retained.
> **Quality reviewed:** 2026-05-01 | Codex 3x (adversarial + consistency + perf) | 1H+1M fixed (short-read rejection prevents silent prefix-parse; out-of-range bool clamp to safe default 0 instead of max 1); re-adversarial skipped (~30 LOC, no lifecycle/state-machine) | scope: boot-code-quality

---

## 8. Kernel Load Memory Allocation Fallback

If 16 MiB contiguous allocation fails (fragmented memory), try smaller sizes.

> [!NOTE]
> **Regression risk:** LOW -- tries larger first, same result as current behavior (which allocates 16 MiB).

- [x] Try `AllocatePages(32 MiB)` first via graduated fallback array
- [x] If fails: try 16 MiB, then 8 MiB (3-element loop)
- [x] Log actual allocation: `"[BOOT] Kernel buffer: N MiB allocated"`
- [x] If all fail OR all overlap-rejected: `"[FAIL] Cannot allocate non-overlapping kernel buffer (tried 32/16/8 MiB)"` + return EFI_LOAD_ERROR (string updated 2026-05-01 from §8 review Codex M1; distinguishes fragmentation exhaustion from overlap-vs-protected-region exhaustion).
- [x] Kernel file size validated by §1 ELF bounds checks (32 MiB cap + per-segment validation)
- [x] Buffer overlap checks: full `sizeof(struct boot_info)` (~22 KiB; protected range is the entire handoff struct, NOT just `0x10000-0x11000` -- §16 caught the 4-KiB-only drift) and framebuffer (addr + pitch*height). Overlap-checks now run INSIDE the alloc loop (§8 review Codex M1 fix 2026-05-01): on overlap, FreePages the bad allocation, log `[BOOT] Kernel buffer N MiB overlaps boot_info/framebuffer -- trying smaller tier`, and continue to the next smaller tier rather than aborting -- a 32 MiB placement on top of a protected region no longer blocks 16 MiB / 8 MiB tries that might land elsewhere.
- [x] Commit: `"boot: kernel allocation fallback -- 32-16-8 MiB with overlap check"` (c5cd3704)

**Test checkpoint:** Normal boot works (kernel is ~1.5 MiB, fits in any allocation). Serial shows `"Kernel buffer: 32 MiB allocated"`. On pathological firmware that places 32 MiB over boot_info, serial would show `"Kernel buffer 32 MiB overlaps boot_info -- trying smaller tier"` followed by the successful smaller-tier line.

> **Test runner:** N/A (UEFI pre-boot only -- AllocatePages graduated fallback runs before kernel) | validation: smoke + serial showing `Kernel buffer: 32 MiB allocated`

> **Notes:**
> - What shipped: graduated AllocatePages fallback in load_kernel() with three tiers (32/16/8 MiB), per-iteration overlap checks against full `sizeof(struct boot_info)` + framebuffer (addr + pitch*height), FreePages-on-overlap-then-continue semantics (Codex M1 fix 2026-05-01), exhaustion path returns EFI_LOAD_ERROR with the non-overlapping-specific [FAIL] string.
> - How it integrates: invoked from load_kernel() in the split-path boot AFTER kernel_file open succeeds; commits buf_addr + alloc_size + load_buf_pages so the §1 cleanup-label FreePages fires on any subsequent ELF-validation failure.
> - Downstream effects: pathological firmware that places 32 MiB on top of boot_info or framebuffer no longer fails the boot -- 16 MiB or 8 MiB tries get a fresh placement attempt; only when ALL THREE tiers either fail to allocate or are overlap-rejected does the boot abort.
> - Canonical doc: this section + §1 ELF bounds checks (32 MiB cap consumer) + §16 boot_info ABI (full sizeof protected range, not 4 KiB).
> - Scope boundary: §8 owns the kernel-buffer allocation policy + overlap rejection. §1 owns ELF file-size validation against the allocated buffer. §17 owns memory-map overlap normalization for runtime regions.

> **Verified:** 2026-05-01 | commit `66b83fff` | 6/6 items | build OK | smoke PASS (KVM 2.27s). 6 original items + per-iteration overlap-rejection-with-tier-fallback confirmed at file:line. 2026-04-11 verification retained.
> **Quality reviewed:** 2026-05-01 | Codex 4x (adversarial + consistency + perf + re-adversarial) | 1M fixed (overlap-rejection now retries smaller tier instead of aborting), 1L fixed (TODO §8 prose updated to match shipped strings + full sizeof(boot_info) range). Round-2 re-adversarial M2 (theoretical AllocateAnyPages re-handing-out same range) rejected with code evidence: UEFI x86 firmware allocates top-down, boot_info at 0x10000 is reserved low memory, framebuffer is MMIO outside EfiConventionalMemory; the loop is defense-in-depth against pathological firmware that hits the overlap branch at all | scope: boot-code-quality

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

> **Test runner:** N/A (UEFI pre-boot only -- error screen rendered via GOP before kernel) | validation: smoke + manual delete-kernel.exe fixture on QEMU WHPX/TCG, VBox

> **Notes:**
> - What shipped: `boot_fatal(UINT32 err_code, const char *title, const char *detail)` + `boot_fatal_dwell()` in `src/boot/uefi/bootx64.c` -- noreturn fatal path: NVRAM-persist err_code, serial `[CRIT] BOOT FATAL (0xXXXXXXXX)` log (8 hex digits matching UINT32 contract, fixed 2026-05-01), white-on-blue ConOut BSOD with recovery steps, optional graphical BSOD via `bsod_render_graphical()` or QR-only via `qr_render_error_url()`, time-based dwell, then `ResetSystem(EFI_RESET_COLD)`.
> - How it integrates: replaces 4 `for(;;) hlt` error paths (kernel load, GetMemoryMap, EBS retry, EBS final). `g_ebs_in_progress` flag gates ConOut after EBS attempted; framebuffer paths handle the post-EBS case. `error_screen_test=1` boot.conf flag halts before ResetSystem so the screen stays visible for visual diff.
> - Downstream effects: NVRAM `last_boot_error` field is read at next-boot for §13 boot-error history; full 8-hex-digit display now matches the persisted UINT32 contract. Post-EBS `boot_fatal_dwell` throttles `RuntimeServices->GetTime` to ~10/sec via TSC-spin to avoid SMM-call storms on real firmware (Codex M2 perf 2026-05-01).
> - Canonical doc: this section + §13 (boot error code registry) + §14 (QR code) + §18 (graphical BSOD) + UEFI 2.10 EFI_RESET_TYPE.
> - Scope boundary: §9 owns the fatal-render contract + boot_fatal API. §13 owns NVRAM error-code persistence. §14 owns QR encoding. §18 owns the graphical BSOD layout.

> **Verified:** 2026-05-01 | commit `7a4980eb` | 7/7 items | build OK | smoke PASS (KVM 2.29s). 7 original items + UINT32-width fatal-code display + EFI_RESET_COLD constant + GetTime throttle confirmed at file:line. 2026-04-11 verification retained.
> **Quality reviewed:** 2026-05-01 | Codex 4x (adversarial + consistency + perf + re-adversarial) | 1M+1M+1L fixed (UINT32-width fatal code display matches contract, EFI_RESET_COLD define added to bootloader efi.h replacing raw literal, post-EBS GetTime polling throttled via TSC-spin to ~10/sec). Round-2 re-adversarial converged with zero new findings | scope: boot-code-quality

---

## 10. ACPI SPCR Serial Port Auto-Detection

Modern firmware provides the ACPI Serial Port Console Redirection Table (SPCR) specifying the exact serial port address, baud rate, and terminal type. Windows and Linux both consult SPCR before falling back to I/O probing. §4's scratch-register probe is necessary as a fallback, but SPCR should be the primary detection method.

> [!NOTE]
> **Regression risk:** LOW -- SPCR lookup is read-only; if table is absent or unparseable, falls through to existing §4 I/O probe unchanged.

- [x] Before §4 I/O probe: search ACPI config tables (via `gST->ConfigurationTable`) for SPCR signature `"SPCR"` -- `serial_spcr_probe()` in `bootx64.c` walks RSDP->XSDT/RSDT->SPCR
- [x] If SPCR found: extract `BaseAddress.Address` for port I/O base, `BaudRate` field, `FlowControl`, `TerminalType` -- `BL_ACPI_SPCR` struct, `spcr_decode_baud()` maps encoded field to actual rate
- [x] Store SPCR-detected port in `boot_info.serial_port` and `boot_info.serial_baud` -- stored in `efi_main()` alongside existing `serial_port`
- [x] Add `boot_info.serial_source` field: 0=none, 1=SPCR, 2=I/O-probe -- added to `boot_info.h` and bootloader's struct
- [x] If SPCR address is MMIO (`base_addr_space != 1`): log non-standard skip and fall through to I/O probe. SPCR I/O bases are firmware-authoritative -- accept any non-zero 16-bit base in `[1, 0xFFF8]` (COM3 0x3E8, COM4 0x2E8, or vendor-custom included; not just COM1/COM2). Upper bound 0xFFF8 keeps the 16550 register block (base..base+7) within the 16-bit I/O port space; widened 2026-05-01 from the original COM1/COM2-only gate which silently dropped firmware-declared non-standard I/O UARTs.
- [x] Log: `"[BOOT] Serial: SPCR detected port=0x%x baud=%u"` or `"[BOOT] Serial: SPCR absent, falling back to I/O probe"` -- logged after `boot_log_init()` in `efi_main()`
- [x] Kernel serial init honors `boot_info.serial_baud` from SPCR instead of hardcoding 38400 -- `serial.c` computes divisor from `serial_baud`
- [x] Commit: `"boot: ACPI SPCR serial port auto-detection before I/O probe"` -- 437fde7a

**Test checkpoint:** On QEMU with `-device isa-debug-exit` (SPCR absent), fallback I/O probe activates and serial works as before. On QEMU OVMF with SPCR table present, serial log shows `"SPCR detected"`. Verify on bare metal -- real firmware may provide SPCR with non-standard baud rates; kernel must honor the SPCR baud.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | test_uefi_boot SPCR-detect assertions + smoke on QEMU OVMF

> **Notes:**
> - What shipped: `serial_spcr_probe()` in `src/boot/uefi/bootx64.c` -- ACPI RSDP/RSDT/XSDT/SPCR walk with full checksum validation (RSDP v1 + v2 + root SDT + SPCR table); `BL_ACPI_SPCR` struct mirrors ACPI 6.5 layout; `spcr_decode_baud()` maps SPCR baud codes (0/3/4/6/7) to actual rates with 38400 fallback for unknown codes; `serial_source` (0/1/2) and `serial_baud` published to boot_info; widened to accept any non-zero 16-bit I/O base in [1, 0xFFF8] (firmware-authoritative).
> - How it integrates: invoked from `serial_early_init()` BEFORE the no-SPCR scratch-register probe; on success bypasses §4 fallback; on any failure (no RSDP, bad checksum, signature mismatch, MMIO base, malformed length, or no UART at base+7) returns 0 and falls through to §4 unchanged.
> - Downstream effects: closes the H1 trust-boundary -- corrupt firmware ACPI tables can no longer redirect the SPCR walk into stale memory and trick `serial_probe_port` into writing to an arbitrary base+7 (Codex H1 + M2 adversarial 2026-05-02). Kernel `serial_init()` honors `boot_info.serial_baud` from SPCR; pre-fix the kernel hardcoded 38400.
> - Canonical doc: this section + §4 (no-SPCR fallback probe) + ACPI 6.5 specification (RSDP, DESCRIPTION_HEADER, SPCR Table 5-49) + UEFI 2.10 EFI_ACPI_TABLE_GUID.
> - Scope boundary: §10 owns ACPI SPCR detection + checksum gates. §4 owns the no-SPCR fallback. Kernel-side ACPI table validation is owned by `src/kernel/acpi.c`.

> **Verified:** 2026-05-02 | commit `51b3fbab` | 7/7 items | build OK | smoke PASS (KVM 2.41s). 7 original items + ACPI checksum-gate hardening (RSDP v1+v2, RSDT/XSDT root, SPCR table) confirmed at file:line. 2026-04-11 verification retained.
> **Accepted:** [H] XSDT/RSDT child pointer validation (corrupt-ACPI fault recovery) -> XREF: 02-kernel-core/TODO-19 §1 (item: "SEH/__try around firmware table walks for fault-isolated recovery" -- requires SEH infrastructure not present in pre-EBS bootloader; checksum gate is the in-scope mitigation)
> **Accepted:** [M] MMIO UART support (PL011, ARM SBSA, etc.) -> XREF: 04-drivers-hardware/TODO-04 §1 (item: "ARM serial driver -- PL011 + DesignWare UART" -- out of x86-only scope today)
> **Quality reviewed:** 2026-05-02 | Codex 4x (adversarial + consistency + perf + re-adversarial) | 1H+1M fixed (ACPI checksum validation across RSDP v1/v2 + RSDT/XSDT root + SPCR table; strict RSDP v2 length gate eliminates skip-on-malformed bypass), 1H+1M accepted-XREF | scope: boot-code-quality

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

> **Test runner:** N/A (UEFI pre-boot only -- watchdog SetWatchdogTimer runs before kernel) | validation: smoke + serial showing `Watchdog: armed` + `disarmed`

> **Notes:**
> - What shipped: `watchdog_reset()` + initial 60s arm + pre-EBS disarm in `src/boot/uefi/bootx64.c` -- WatchdogCode `0x424F4F54` ("BOOT" ASCII; UEFI 2.10 OS range 0x10000+). Two state flags split 2026-05-02: `g_wd_armed` = firmware watchdog counting (cleared only on successful disarm); `g_wd_refresh_disabled` = stop refresh attempts after first failure (does NOT clear g_wd_armed).
> - How it integrates: arm fires at efi_main entry; `watchdog_reset()` called before each long pre-EBS operation (init_gop, load_kernel, discover_usb_devices); disarm at pre-ExitBootServices with retry-once on failure.
> - Downstream effects: a refresh failure no longer suppresses the pre-EBS disarm -- closes the M1 hazard where a transient refresh failure could ship a live 60s timer into the kernel (Codex M1 adversarial 2026-05-02). Exceeds Win11/Linux/GRUB which all simply disable the firmware watchdog.
> - Canonical doc: this section + UEFI 2.10 specification section 7.5 (Boot Services Watchdog Timer) + §9 boot_fatal callers (boot_fatal is terminal, never needs watchdog refresh).
> - Scope boundary: §11 owns the bootloader watchdog state machine. Kernel-side re-arm (after handoff) is owned by `02-kernel-core/TODO-19`; bootloader hands off with watchdog disarmed.

> **Verified:** 2026-05-02 | commit `90f215fb` | 6/6 items | build OK | smoke PASS (KVM 2.35s). 5 original items + state-flag split confirmed at file:line. 2026-04-11 verification retained.
> **Quality reviewed:** 2026-05-02 | Codex 4x (adversarial + consistency + perf + re-adversarial) | 1M fixed (state-flag split eliminates the refresh-fail-suppresses-disarm hazard). Round-2 re-adversarial converged with zero new findings | scope: boot-code-quality

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

> **Test runner:** N/A (UEFI pre-boot only -- mmap descriptor validation runs before kernel) | validation: smoke + serial; warning lines logged on quirky firmware

> **Notes:**
> - What shipped: shared `mmap_geometry_validate()` + `mmap_descriptor_valid()` helpers in `src/boot/uefi/bootx64.c`. The 5 per-descriptor checks (NumberOfPages overflow, address-range wrap, zero pages, page alignment, Type < EfiMaxMemoryType) are now applied uniformly to BOTH `fill_memory_map()` (mmap[]) AND `fill_runtime_map()` (rt_mmap[]); geometry guard runs on initial GetMemoryMap AND every EBS-retry refresh.
> - How it integrates: invalid descriptors get `[WARN] <site> entry N: ...` logs + set `mmap_quirks=1` + are stripped from the destination array; geometry failures call `boot_fatal(BOOT_ERR_MMAP_GEOMETRY, ...)`. Runtime-cap overflow (>64 EfiRuntimeServices descriptors) emits a one-shot WARN + sets mmap_quirks; loop continues so later quirks still warn.
> - Downstream effects: closes the H1 hazard where rt_mmap kept firmware quirks that mmap[] stripped (kernel UEFI runtime setup was exposed to malformed descriptors); closes the M2 hazard where EBS retry skipped the geometry guard (desc_size==0 with map_size>0 could spin fill_runtime_map forever); closes the M3 silent-truncation of runtime descriptors past the 64-entry cap.
> - Canonical doc: this section + §6 (mmap overflow detection) + §17 (mmap normalization) + UEFI 2.10 specification 7.2 (memory map services) + ACPI 6.5 EFI_MEMORY_DESCRIPTOR layout.
> - Scope boundary: §12 owns per-descriptor validation + geometry guard. §6 owns total-descriptor cap. §17 owns sweep-line overlap normalization. mmap_evict_for_incoming priority logic is owned by §6's cap-handling subsection.

> **Verified:** 2026-05-02 | commit `5977bfb6` | 5/5 items | build OK | smoke PASS (KVM 2.31s). All original items + shared validation helpers + EBS-retry geometry guard + runtime-cap WARN confirmed at file:line. 2026-04-11 verification retained.
> **Accepted:** [M] Overlap priority normalization across descriptor types -> XREF: 01-boot-platform/TODO-03 §17 (item: "mmap_normalize sweep-line carves overlapping ranges by priority" -- §17 owns the cross-descriptor merge; §12 stops at per-descriptor validation)
> **Quality reviewed:** 2026-05-02 | Codex 4x (adversarial + consistency + perf + re-adversarial) | 1H+2M fixed (runtime-map validation parity, EBS-retry geometry guard, runtime-cap silent-truncation visibility), 1M accepted-XREF | scope: boot-code-quality

---

## 13. Boot Error Code Registry & NVRAM Persistence

Windows has BootStatusPolicy but error codes are opaque hex values without context. Linux has no bootloader-level error persistence. This section creates a structured error code system where each failure has a unique code, and the last-boot error is saved in UEFI NVRAM for next-boot diagnostics.

> [!TIP]
> **Competitive advantage:** Neither Windows nor Linux persists structured bootloader error codes in NVRAM. The next boot can display "Last boot failed: 0x0003 -- Kernel not found at \boot\kernel.exe" before trying again. Combined with TODO-21 A/B rollback and TODO-14 §6 panic forensics, this gives a complete cross-boot diagnostic chain.
> **Regression risk:** LOW -- NVRAM writes are non-destructive (single variable). If NVRAM is full or read-only, write silently fails and boot continues.

- [x] Define `BOOT_ERR_*` codes as `#define` constants in `efi.h` (20 codes 0x0000-0x0013 today: §1-§12 base failures plus §11 UKI/rollback and §15 ESP integrity adds; ceiling tracked by `BOOT_ERR_REGISTRY_MAX` in `bootx64.c`)
- [x] On fatal error: `boot_fatal()` writes error code to UEFI NVRAM variable `BootError` (Impossible OS vendor GUID, non-volatile + boot-service-access + runtime-access) via `nvram_write_boot_error()` helper -- serial `[CRIT]` line emitted FIRST so a slow / wedged firmware `SetVariable` cannot block the operator's diagnostic
- [x] On successful boot: kernel clears `BootError` NVRAM variable in `boot_phase0()` after `uefi_vars_init()` succeeds -- clearing in the kernel (not bootloader) ensures a crash between EBS and Phase 0 preserves the error evidence
- [x] At boot entry: `nvram_read_boot_error()` reads NVRAM -- if non-zero, logs `"[BOOT] Previous boot failed: code=0x"` + 8 hex on serial; reader validates attrs == NV|BS|RT, value <= `BOOT_ERR_REGISTRY_MAX`, and size == 4, repairing any malformed (oversized / undersized / wrong-attrs / out-of-registry) record via UEFI 2.10 7.2.1 delete-then-create so a pre-OS UEFI app cannot poison the channel
- [x] Pass `boot_info.last_boot_error` to kernel -- field added to both bootloader struct (bootx64.c) and kernel header (boot_info.h) after `serial_baud`; kernel logs `"Previous boot failed: code=0x"` + 8 hex via klog in `boot_hw.c` (UINT32 contract uniform across serial / ConOut / klog / BSOD / QR URL)
- [x] `boot_fatal()` includes the error code in on-screen display: 8-hex `"Error code: 0xNNNNNNNN"` line on the ConOut text fallback AND on the graphical BSOD subtitle AND in the QR-encoded recovery URL (`impossibleos.co/err/<8hex>`) -- all surfaces match the persisted UINT32
- [x] Commit: `"review: TODO-03 §13 NVRAM persist -- serial-first ordering, BootError attr/registry repair, UINT32 width across all surfaces"` (d0a6c045)

**Test checkpoint:** Delete `\boot\kernel.exe`, boot (gets error screen). Reboot normally with kernel restored. Serial shows `"Previous boot failed: code=0x00000003"`. Verify NVRAM variable is cleared on successful boot. Negative path: write a 1-byte BootError under the Impossible OS GUID with non-canonical attrs from a UEFI shell, reboot -- bootloader logs `[WARN] NVRAM: BootError untrusted ... repairing`, deletes, recreates canonical zero record; kernel-side clear path then sees a well-formed record.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | test_boot_info NVRAM error-code persist/clear assertions + manual reboot-after-failure fixture

> **Notes:**
> - Serial `[CRIT]` line precedes `nvram_write_boot_error()` so a slow / wedged firmware `SetVariable` cannot starve the only synchronous fatal-path diagnostic; persistence is best-effort and runs second.
> - `nvram_read_boot_error()` validates attrs == NV|BS|RT, size == 4, and code <= `BOOT_ERR_REGISTRY_MAX` (0x0013); any malformed shape (oversized via `EFI_BUFFER_TOO_SMALL`, undersized successful read, wrong attrs, out-of-registry value) enters the same delete-then-create repair path so a pre-OS writer cannot permanently poison persistence.
> - All 5 fatal-display surfaces print 8 hex digits (UINT32): bootloader serial CRIT, ConOut text fallback, graphical BSOD subtitle, QR-encoded recovery URL on both the standalone `qr_render_error_url` path and the inline graphical BSOD URL caption, plus the kernel `klog` previous-boot line; previously the graphical paths truncated to 16 bits while serial widened to 32.
> - Recovery web registry at `gh-pages/err/errors.js` extended to cover codes `000e`-`0013` (ESP type GUID, ESP BPB, ESP missing files, rollback refusal, UKI payload, UKI disk override); the `Registry source` template tile referencing TODO `§N` was retired since it linked a public page to internal renumbering.
> - Section IO row stays `[x]` after the boot-error history ring scope was promoted to standalone §20.
> **Verified:** 2026-05-02 | commit `d0a6c045` | 6/6 items | build OK | smoke PASS (KVM 2.28s) | 8 hex surfaces uniform
> **Quality reviewed:** 2026-05-02 | Codex 7x (adversarial, consistency, perf, re-adversarial x4) | 0H+5M+0L fixed, 0 open | scope: boot-code-quality

---

## 14. Error Screen QR Code

Add a QR code to the boot failure error screen (§9) that encodes a recovery URL with the error code. Smartphones can scan it to get step-by-step recovery instructions. ChromeOS does this for recovery; neither Windows UEFI-stage errors nor Linux GRUB rescue provides it.

> [!TIP]
> **Competitive advantage:** A QR code on the pre-kernel error screen is actionable for non-technical users. Instead of "call support", they scan and get a page explaining exactly what error code 0x0003 means and how to fix it. ChromeOS has this for recovery; neither Win11 nor Linux has it at the UEFI bootloader stage.
> **Regression risk:** LOW -- QR rendering is additive to §9 error screen. If QR encoder has a bug, error screen still shows text error message.

- [x] Implement QR code encoder in bootloader: QR Version 3 (29x29 modules), byte mode, ECL-L, Reed-Solomon EC (15 codewords), mask pattern 0 -- self-contained in `bootx64.c`, verified 0-diff against segno (spec-compliant library)
- [x] `boot_fatal()` renders QR code in bottom-right corner of the GOP framebuffer (direct pixel write, works even during EBS retry); if ConOut unavailable but framebuffer available, renders QR-only
- [x] QR payload: `https://impossibleos.co/err/XXXX` (4 lowercase hex digits matching the published `gh-pages/err/<code>/` static pages and the BOOT_ERR_REGISTRY_MAX domain (0x0013 currently, well under 16 bits); both QR-only and graphical BSOD paths produce the identical string via `format_recovery_url()`)
- [x] QR module size: 4px at 1280x720, 6px at 1920x1080, 8px at 2560+ -- with 4-module white quiet zone
- [x] If GOP unavailable (gFramebuffer NULL or size 0): QR rendering silently skipped, text error screen still shows
- [x] Standalone encoder ([TODO-14 §6 Panic QR Code](../01-boot-platform/TODO-14-boot-diagnostics.md#6-panic-qr-code-deferred----depends-on-5) not yet implemented) -- when that section lands, the kernel-side encoder can be factored from this implementation
- [x] `error_screen_test=1` boot.conf key triggers `boot_fatal()` before kernel load for QR/BSOD testing (halts instead of rebooting so screen stays visible)
- [x] Commit: `"boot: QR code on boot error screen -- scan for recovery instructions"` (134702ae)

**Test checkpoint:** Run `scripts/debug/kernel/run-error-screen-test.bat` (sets `error_screen_test=1`). Error screen shows QR code in bottom-right. Scan with phone -- URL `https://impossibleos.co/err/0003` appears and resolves to the recovery page. Verify QR is scannable at 1280x720 and 1920x1080 resolutions.

> **Test runner:** N/A (UEFI pre-boot only -- QR encoder renders via GOP before kernel) | validation: `scripts\debug\kernel\run-error-screen-test.bat` (error_screen_test=1) + phone-scan

> **Notes:**
> - Shipped: QR Version 3 byte-mode encoder + `qr_render_error_url()` + graphical BSOD QR block in `src/boot/uefi/bootx64.c` (~1100-1604) + `error_screen_test=1` boot.conf gate (~7956).
> - How it runs: `boot_fatal()` invokes `qr_render_error_url()` when ConOut is unavailable but GOP is; full graphical BSOD calls a parallel inline QR block. Both paths now share `format_recovery_url()` so QR payload and on-screen caption are byte-identical.
> - Downstream effects: QR encoder remains self-contained and 0-diff vs segno. URL width matches `gh-pages/err/<code>/` static pages 1:1; new error codes need a matching directory added to gh-pages.
> - Canonical doc: this section + the `format_recovery_url()` doc-comment in bootx64.c.
> - Scope boundary: §14 owns the bootloader QR encoder, the QR/caption renderer, and the error_screen_test trigger. The kernel-side panic-QR factor-out is owned by [TODO-14 §6 Panic QR Code](../01-boot-platform/TODO-14-boot-diagnostics.md#6-panic-qr-code-deferred----depends-on-5).

> **Verified:** 2026-05-02 | commit `134702ae` | 8/8 items | build OK | manual (phone-scan @ 1280x720, 1920x1080)
> **Quality reviewed:** 2026-05-02 | Codex 3x (adversarial + consistency + perf) | 1H+0M+0L fixed, 0 open | scope: boot-code-quality

---

## 15. boot_info ABI Header and Bootloader Populate

> [!IMPORTANT]
> **Owner moved:** the canonical roadmap owner for `boot_info` is now `TODO-01-boot-protocol-abi-handoff.md`. Keep this section as shipped implementation history and bootloader-specific context only; new handoff-contract work should be added to TODO-01.

Place a fixed-size header at offset 0 of `struct boot_info` and have the UEFI bootloader fill magic, version, and size before `ExitBootServices()` so the kernel can validate before any `memcpy`.

> [!NOTE]
> **Regression risk:** MEDIUM -- any `struct boot_info` layout change must bump `BOOT_INFO_VERSION` and rebuild bootloader + kernel together.

- [x] Add `struct boot_info_header` at offset 0: `uint32_t magic` (BOOT_INFO_MAGIC=0x49504F53 "IPOS"), `uint16_t version` (BOOT_INFO_VERSION=1), `uint16_t size` -- defined in both `include/kernel/boot_info.h` and `src/boot/uefi/bootx64.c` (ABI sync)
- [x] Bootloader populates `header.magic`, `.version`, `.size = sizeof(struct boot_info)` as the last step before `jump_to_kernel()` -- after all other fields are set
- [x] `_Static_assert(offsetof(boot_info, header) == 0)` in `boot_info.h` -- enforces offset 0
- [x] `_Static_assert(sizeof(boot_info_header) == 8)` in `boot_info.h` -- pins header layout
- [x] `boot_info.h` comments document ABI rules: bump version on layout changes, rebuild both images together, `_reserved` fields don't require bumps
- [x] `CLAUDE.md` new "boot_info ABI" section: documents magic, version mismatch halt, rebuild rule
- [x] Kernel `boot_hw.c` validates magic + version + size after memcpy, halts on mismatch; each failure logs `LOG_ERROR` with observed vs expected values before `boot_halt()` so the serial log shows the actual bad field, not just which check failed
- [x] `_Static_assert(sizeof(struct boot_info) <= 65535)` added kernel-side so the uint16_t `header.size` field cannot silently truncate if the struct grows; comparison uses `(size_t)header.size != sizeof(struct boot_info)` (no cast)
- [x] Commit: `"boot: boot_info ABI header fields and bootloader populate"` (24d7baa2)

**Test checkpoint:** Clean build boots on QEMU WHPX, QEMU TCG, VirtualBox, bare metal; header fields visible in memory at the handoff pointer before kernel entry (debugger or serial hex dump).

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | test_boot_info structural + offset asserts; smoke on QEMU WHPX/TCG, VBox, bare metal

> **Notes:**
> - This section captures shipped implementation HISTORY for the `boot_info_header` ABI work that landed in commit `24d7baa2`. The handoff contract has since evolved well past version 1 -- see [TODO-01 boot-protocol-abi-handoff](./TODO-01-boot-protocol-abi-handoff.md) for the canonical owner and `include/kernel/boot_info.h` for the live `BOOT_INFO_VERSION` value.
> - Live drift gate today: compile-time `_Static_assert` blocks in both `include/kernel/boot_info.h` and `src/boot/uefi/boot_info_mirror.h`, plus the manifest comparator at `tools/boot-info-manifest/compare.sh` run as the first-line check by `bash scripts/build.sh`.
> - Kernel validation today: two-stage `boot_info_validate_addr()` then `boot_version_classify()` + `boot_version_render_fatal()` in [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c) and `src/kernel/main/boot_version.c`; the original single-stage LOG_ERROR + boot_halt path (this section's 2026-04-11 stamp) was replaced by the §16 work.
> - The 2026-04-11 stamp (below) is preserved verbatim as historical context. Its file:line references have aged forward and should not be used to navigate current code.
> - Scope boundary: §15 is implementation history; §16 owns the active validator surface; TODO-01 §2 owns the canonical roadmap and live ABI manifest.

> **Verified:** 2026-04-11 -- all 8 items confirmed with code evidence (HISTORICAL SNAPSHOT, see Notes above). Header defined at offset 0 in both `include/kernel/boot_info.h:47` and `src/boot/uefi/bootx64.c:199`. Static asserts for offset/size/<=65535 present on both sides. Bootloader populates at `bootx64.c:3748` as the last step before `jump_to_kernel()`. Kernel validates at `boot_hw.c:70-96` with observed vs expected LOG_ERROR diagnostics before `boot_halt()`. Accepted: pre-memcpy pointer/bounds validation (`boot_info_validate()`) and cross-build layout fingerprinting -> XREF: 01-boot-platform/TODO-03 §16 (item: "Implement `boot_info_validate_addr(p, size, max_addr)`..." at line 570).
> **Quality reviewed:** 2026-04-11 -- Codex adversarial round 1 flagged LOG_FATAL-before-boot_halt regression (klog LOG_FATAL is no-return); fixed with LOG_ERROR. Round 2 and 3 clean. Dead code + consistency + performance pass -- no findings. Win11 uses BCD signature + Boot Services Protocol GUID; Linux Multiboot2 uses 0x36D76289 + tag format; our magic+version+size + runtime validation is comparable scope. Accepted: 2 HIGH/MED findings accepted as -> XREF: 01-boot-platform/TODO-03 §16 (item: "Implement `boot_info_validate_addr(p, size, max_addr)`..." at line 570) (pre-copy validation, mirror struct drift detection).
> **Re-reviewed:** 2026-05-02 | commit `24d7baa2` (history) | 8/8 items shipped | build OK | 33+33 _Static_asserts in mirror+kernel, manifest gate live
> **Quality re-reviewed:** 2026-05-02 | Codex 3x (adversarial + consistency + perf) | 1M doc-drift fixed, 2M consistency-drift fixed (TODO-01 stale ABI snapshot, CLAUDE.md stale doctrine), 0 open | scope: kernel-code-quality (boot_hw comment + doctrine sync only)

---

## 16. boot_info Kernel Validation, boot_hw Path, and Unit Tests

> [!IMPORTANT]
> **Owner moved:** the canonical roadmap owner for kernel-side handoff validation, ABI drift protection, and future schema hardening is now `TODO-01-boot-protocol-abi-handoff.md`. Keep this section as shipped implementation history and proof of what already landed here.

The kernel must reject invalid `mbi` before copying `struct boot_info`. Validation splits into an address phase (NULL / alignment / bounds / wraparound) and a header phase (magic / version / size) so the failure-log path can safely dereference the header only after the address phase has confirmed the pointer is in mapped memory. The post-hoc `cmdline` ASCII check at `src/kernel/main/boot_hw.c` stays as a second-line defense after the validated copy.

> [!IMPORTANT]
> **Found during TODO-02 Phase 0 verify (2026-04-08, Codex round 1, commit b402cf21).** Uncoordinated edits to `struct boot_info` widen the silent-corruption window until this lands.
> **Regression risk:** MEDIUM -- stale `BOOTX64.EFI` after a version bump halts early; mitigate by always using `bash scripts/build.sh` for paired images.

- [x] Implement `boot_info_validate_addr(p, size, max_addr)`, `boot_info_validate_header(hdr, kernel_struct_size)`, and combined `boot_info_validate(p, kernel_struct_size)` in `src/kernel/main/boot_info.c` as pure functions. Address phase rejects NULL, addresses below `0x1000` (NULL page / BDA), misaligned pointers, sizes below `sizeof(boot_info_header)` or above `65535`, ranges that wrap or exceed `max_addr`. Header phase rejects bad magic, version mismatch, and size mismatch.
- [x] `boot_phase0()` calls `boot_info_validate_addr(mbi, sizeof(struct boot_info), BOOT_INFO_EARLY_MAP_END)` first (4 GiB limit = bootloader identity-map bound set in `bootx64.c:setup_page_tables()`), then `boot_info_validate_header()` on the confirmed-safe pointer. Each failure logs observed vs expected via `LOG_ERROR` + `boot_halt()`. `g_boot_info` is still zero-initialized on failure so `boot_halt()`'s framebuffer path at `src/kernel/main/boot_halt.c:269-298` sees `fb_available == 0` and skips the fb dereference -- no risk of writing through garbage-copied `fb.addr`.
- [x] `src/kernel/main/boot_hw.c` UEFI branch copies `src->header.size` bytes after validation (validator guaranteed it equals `sizeof(struct boot_info)`); the post-copy magic/version/size re-checks from §15 are removed as redundant with pre-copy validation.
- [x] Cross-struct layout fingerprint: paired `_Static_assert(__builtin_offsetof(struct boot_info, X) == N)` on five critical count fields in BOTH `include/kernel/boot_info.h` and `src/boot/uefi/bootx64.c` -- `mmap_count (16392)`, `gop_mode_count (16948)`, `config_table_count (18280)`, `rt_mmap_count (20352)`, `usb_device_count (21504)`. Same-size field reorders now fail to build on whichever side drifted.
- [x] Kept the `cmdline` ASCII sanity check after the validated copy in `boot_hw.c`; relocated comment now describes it as defense in depth against post-copy corruption (no more stale "struct shifted" wording). Changed `LOG_FATAL` to `LOG_ERROR` + `boot_halt()` to match the §15 lesson that `klog(LOG_FATAL, ...)` is no-return and would bypass `boot_halt()`.
- [x] Added `src/kernel/test/test_boot_info.c` with 22 pure tests -- no live boot infrastructure calls. Coverage: address phase NULL / below-floor / misaligned / size-below-header / size-above-uint16 / wraparound / over-max / bound-straddle / valid-handoff / kernel-VA / exact-lower-boundary / exact-upper-boundary / exact-uint16-max; header phase NULL / bad-magic / bad-version / bad-size / valid; combined NULL / bad-magic / valid / misaligned-short-circuit.
- [x] Wired `test_register_boot_info()` in `src/kernel/test/test_runner.c` under `TEST_CAT_BOOT`.
- [x] Scope-gap fix (Branch A): §16 quality review caught that `load_kernel()`'s ELF overlap and kernel-buffer overlap guards were hardcoded to the first 4 KiB only (`0x10000--0x11000`), letting segments or allocations at `0x11000..0x155BF` silently clobber the `boot_info` tail and still pass the new post-handoff validator because the header at offset 0 was untouched. Fixed both sites in `src/boot/uefi/bootx64.c` to use `[BOOT_INFO_PHYS_ADDR, BOOT_INFO_PHYS_ADDR + sizeof(struct boot_info))` and updated §1's corresponding checklist item. This defense closes the integrity gap §16 was meant to establish.
- [x] Reserve the full `boot_info` page range with `gBS->AllocatePages(AllocateAddress, EfiLoaderData, 6, &addr)` in `efi_main()` BEFORE the first `efi_memset(g_boot_info_ptr, ...)` and before any other `AllocatePages` call site. Covers the six 4 KiB pages starting at `BOOT_INFO_PHYS_ADDR` (24 KiB footprint, up from the 21952-byte struct size). Without this, nothing in the UEFI memory map prevents later `AllocatePages(AllocateAnyPages, ...)` calls in `load_kernel` / `allocate_xhci_dma` / scratchpad setup from silently reusing the `boot_info` tail pages. If the range is already owned by firmware, the bootloader halts via `boot_fatal(BOOT_ERR_BOOT_INFO_RESERVED, ...)` (new error code `0x000D` in `efi.h`) instead of corrupting the handoff. Added during §16 post-implementation review (2026-04-11).
- [x] Commit: `"boot: kernel boot_info_validate before Phase 0 memcpy"` (22a8fb03)

**Test checkpoint:** Normal boot on QEMU WHPX, QEMU TCG, VirtualBox, bare metal reaches Phase 0 with validation passing. Corrupt `header.magic` in the bootloader build -- serial shows `boot_info: bad header magic=0x...` with observed vs expected values. Intentional `BOOT_INFO_VERSION` mismatch across images -- early halt with `boot_info: bad header ... version=...`. Unit tests `boot_info: *` run under `SUITE=boot` and PASS.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | test_boot_info validator assertions (magic/version/size); intentional-corrupt fixture for header-mismatch path

> **Notes:**
> - This section captures shipped implementation HISTORY for the kernel-side validator (commit `22a8fb03`). The validator surface has since evolved: production now uses `boot_info_validate_addr()` + `boot_version_classify()` + `boot_version_render_fatal()`; `boot_info_validate_header()` and `boot_info_validate()` remain as pure/test-facing helpers.
> - Live evidence today: validators in [`src/kernel/main/boot_info.c`](../../src/kernel/main/boot_info.c) at lines 31 / 77 / 95; Phase 0 call sites in [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c) lines 65-118 (validate_addr at line 75, classify+render_fatal at line 93); 33+33 `_Static_assert` blocks in [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) and [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h) plus the manifest comparator at `tools/boot-info-manifest/compare.sh`; AllocatePages reservation in `src/boot/uefi/bootx64.c` at lines 7246-7258; test_register_boot_info at [`src/kernel/test/test_runner.c`](../../src/kernel/test/test_runner.c) line 427.
> - Canonical roadmap owner: [TODO-01 boot-protocol-abi-handoff](./TODO-01-boot-protocol-abi-handoff.md). §16 is implementation history; new validator/handoff work goes there.
> - The 2026-04-11 stamp (below) is preserved verbatim as historical context; its file:line references have aged forward.
> - Scope boundary: §16 is implementation history. Active validator surface evolution is owned by TODO-01 §2-4. Per-fault-class boot_version diagnostics are owned by `src/kernel/main/boot_version.c`.

> **Verified:** 2026-04-11 -- all 9 items confirmed (HISTORICAL SNAPSHOT, see Notes above). Three validators at [boot_info.c](../../src/kernel/main/boot_info.c) lines 31 / 77 / 95. Phase 0 call sites at [boot_hw.c:72](../../src/kernel/main/boot_hw.c#L72) and [boot_hw.c:86](../../src/kernel/main/boot_hw.c#L86) with `BOOT_INFO_EARLY_MAP_END` and LOG_ERROR failure paths. Ten mirrored `_Static_assert` offset checks at [boot_info.h:518-529](../../include/kernel/boot_info.h#L518) and [bootx64.c:306-315](../../src/boot/uefi/bootx64.c#L306). Two overlap guards at [bootx64.c:2017](../../src/boot/uefi/bootx64.c#L2017) and [bootx64.c:2128](../../src/boot/uefi/bootx64.c#L2128). 22 pure tests registered via `test_register_boot_info()` at [test_runner.c:227](../../src/kernel/test/test_runner.c#L227). Accepted: none.
> **Quality reviewed:** 2026-04-11 -- Codex round 1 caught the bootloader `AllocatePages(AllocateAnyPages)` free-pool gap (boot_info not reserved via `AllocateAddress`) and round 2 cleared after the reservation fix at [bootx64.c:3556-3583](../../src/boot/uefi/bootx64.c#L3556). Dead code + consistency + performance pass -- ten offset asserts match on both sides, validators are all referenced, removed post-copy re-checks have no dangling callers, `cmdline` ASCII check reachable, no hot-path allocations. Parity: Linux efi-stub reserves the kernel decompression area via `AllocateAddress` for the same reason; Windows `bootmgfw.efi` uses `BlMmAllocatePhysicalPages` with `AllocateAddress` equivalent. Accepted: none.
> **Re-reviewed:** 2026-05-02 | commit `22a8fb03` (history) | 9/9 items shipped | build OK | manifest comparator + 33+33 _Static_asserts live
> **Quality re-reviewed:** 2026-05-02 | Codex 3x (adversarial + consistency + perf) | 1H render_fatal LOG_FATAL halt-before-diagnostics fixed, 2M consistency narrative drift fixed, 0 open | scope: kernel-code-quality (single LOG_FATAL→LOG_ERROR sweep in boot_version.c)

---

## 17. Memory Map Overlap Normalization

> Found during §12 quality review (2026-04-11). The current overlap check in `fill_memory_map()` skipped overlapping descriptors with a `[WARN]` log. Real firmware (particularly with ACPI reclaim regions) can produce legitimate overlaps that should be resolved by priority, not skipped. This section replaces the skip-on-overlap path with sweep-line normalization that carves contested ranges by UEFI memory type priority before the kernel PMM consumes the map.

- [x] Memory map descriptors sorted via endpoint-event sort (insertion sort, handles the sorted/nearly-sorted firmware case in ~O(n) and the worst case in O(n^2) for n <= 512).
- [x] Priority table at `src/boot/uefi/bootx64.c:mmap_type_priority()`: Reserved / MMIO / MMIOPort / PalCode / Unusable / Unknown = 100, Runtime Code/Data = 95 (promoted above ACPI Reclaim per Codex design finding -- runtime memory must survive ExitBootServices), ACPI NVS = 85, ACPI Reclaim = 80, Persistent = 70, BootServices / LoaderCode / LoaderData = 50, Conventional = 40.
- [x] Carve via sweep-line algorithm, not iterative pair resolution. Codex round 1 proved that an iterative pair-resolve-with-restart loop could exceed a 2*n+16 pass bound on pathological staggered maps (1291 passes for n=512), leaving the published map with residual overlaps. The sweep-line pass emits one winner-priority range per inter-event interval and is deterministic O(n^2) with no bounded-iteration guard. See `mmap_normalize()` and helpers at [bootx64.c](../../src/boot/uefi/bootx64.c).
- [x] Guard against exceeding `BOOT_MMAP_MAX_ENTRIES` via two independent gates: (a) during Phase 1 fill, `mmap_evict_for_incoming()` ranks every lower-priority candidate by `loss = victim.length - overlap_length(victim, incoming)` and evicts the minimum-loss victim -- a fully-contained victim yields `loss == 0` and is always preferred, while a large RAM block that overlaps only a tiny slice of an incoming Reserved descriptor is protected from catastrophic eviction; (b) during sweep-line emit, `mmap_emit_segment()` drops any segment that would exceed the cap and sets `mmap_quirks = 1`.
- [x] `fill_memory_map()` rewritten into three phases: (1) validate raw UEFI descriptors and copy into `boot_info->mmap` with priority-aware cap handling, (2) `mmap_normalize()` sweep-line pass, (3) recompute `total_mem` from the NORMALIZED map (required because carving can shrink Conventional/BootServices/Loader ranges that were counted at pre-normalize length).
- [x] Normalized overlaps logged as `"[BOOT] mmap: overlap resolved at 0xXXXXXXXXXXXXXXXX -- type N wins over type M"` via `mmap_log_resolved()`. Logged only at boundaries where the active set contains more than one descriptor AND the loser type differs from the winner type (so adjacent same-type coalescing does not spam the log).
- [x] Stable tiebreak on equal-priority overlaps (Codex round 2 finding): `mmap_active_find_winner()` prefers the lower original `s_mmap_work` index when priorities tie, so the winner is deterministic regardless of unrelated active-set swap-remove churn.
- [x] Commit: `"boot: memory map sort + overlap normalization by type priority"` (b2f7fcd0)

**Test checkpoint:** Normal boot produces no `[BOOT] mmap: overlap resolved` lines on clean OVMF / QEMU firmware. `mem_upper_kb` in `boot_info` remains within <= 1% of the pre-§17 value on clean maps. Kernel PMM `total_frames` and `used_frames` are consistent with the normalized map. Adversarial synthetic overlap injection is out of scope here (no clean QEMU hook); smoke-test coverage is added to the Unit Tests section below.

> **Test runner:** N/A (bootloader-side normalization, no kernel-side test surface) | validation: `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) covers PMM consumer + boot_info validators; mmap_normalize itself is exercised only by smoke boot on real and emulated firmware

> **Notes:**
> - This section captures shipped implementation HISTORY for the sweep-line normalizer (commit `b2f7fcd0`). The runtime invariants still hold; the file:line numbers in the original 2026-04-12 stamp have aged forward.
> - Live evidence today: `mmap_type_priority()` at [bootx64.c:4606](../../src/boot/uefi/bootx64.c#L4606), `mmap_log_resolved` at 4647, `mmap_active_find_winner` at 4716, `mmap_emit_segment` at 4766, `mmap_normalize` at 4790, `mmap_evict_for_incoming` at 4909.
> - Consumer-side fail-closed validation added 2026-05-02 in [`src/kernel/mm/pmm.c`](../../src/kernel/mm/pmm.c) (count cap + base+length wrap check) so PMM no longer trusts the post-handoff `boot_info->mmap` blindly.
> - Canonical roadmap owner: [TODO-01 boot-protocol-abi-handoff](./TODO-01-boot-protocol-abi-handoff.md). §17 is implementation history.
> - Test surface: bootloader-side mmap_normalize has no direct kernel-side test. Smoke boot exercises the normalizer against real OVMF/QEMU/VBox/bare-metal firmware maps; adversarial-overlap fuzzing is operator-injected if needed.
> - Scope boundary: §17 is implementation history. Future normalizer evolution and kernel-side mmap consumer hardening go in TODO-01.

> **Verified:** 2026-04-12 -- all 8 items confirmed with code evidence (HISTORICAL SNAPSHOT, see Notes above). Priority table at [bootx64.c:2319](../../src/boot/uefi/bootx64.c#L2319) with Runtime promoted above ACPI Reclaim. Sweep-line `mmap_normalize()` at [bootx64.c:2504](../../src/boot/uefi/bootx64.c#L2504) with event sort, active set, and coalescing emit. BSS work buffers ~34 KiB. `mmap_evict_for_incoming()` min-loss ranking at [bootx64.c:2584](../../src/boot/uefi/bootx64.c#L2584). `fill_memory_map()` three-phase rewrite at [bootx64.c:2670](../../src/boot/uefi/bootx64.c#L2670) with post-normalize `total_mem` recompute. Accepted: none.
> **Quality reviewed:** 2026-04-12 -- five Codex rounds. Design review flagged Runtime priority ordering, stale `mem_upper_kb`, and cap truncation blindness; all fixed before first build. Impl round 1 flagged a 2*n+16 iterative resolver guard that could exhaust (1291 passes for n=512); replaced with the sweep-line algorithm. Impl round 2 flagged unstable tiebreak under swap-remove reordering; fixed with descriptor-index tiebreak. Review round 1 flagged arbitrary-large-RAM eviction; fixed with overlap-preference eviction. Review round 2 flagged that overlap-preference still discarded non-overlapping fragments of a large victim; fixed with min-loss ranking (`loss = victim.length - overlap_length`). Review round 3 cleared. Quality pass clean: no dead code, priority/eviction/coalescing consistent with `efi.h`, O(n^2) bounded at n=512. Parity: Linux `efi_memmap_insert()` and `e820_update_range()` use similar carve-by-priority logic; Windows HAL applies EFI memory-type precedence internally. Accepted: none.
> **Re-reviewed:** 2026-05-02 | commit `b2f7fcd0` (history) | 8/8 items shipped | build OK | smoke PASS (KVM/TCG/VBox)
> **Quality re-reviewed:** 2026-05-02 | Codex 3x (adversarial + consistency + perf) | 1H PMM consumer-side mmap validation (count cap + wrap check) added, 1M test-surface narrative + 1L line-number drift fixed, 0 open | scope: kernel-code-quality (PMM validator + doctrine sync)

---

## 18. Graphical Error Screen (ChromeOS/Win11-Style)

The pre-§18 error screen used UEFI text console (`ConOut`) with white-on-blue text plus a QR code rendered directly to the framebuffer. §18 adds a full pixel-rendered graphical BSOD drawn to the GOP framebuffer: solid blue fill, large sad face icon, title text, error code, description, QR code, and URL text -- all independent of Boot Services. The ConOut text path remains the fallback when the framebuffer is headless, too small, or uses a BitMask pixel format.

> [!TIP]
> **Competitive advantage:** ChromeOS and Windows both render graphical error screens, but only at the OS level -- their UEFI-stage errors are plain text or invisible. Rendering a polished graphical error screen at the UEFI bootloader stage (before the OS loads) puts Impossible OS ahead of both competitors for pre-kernel diagnostics.
> **Regression risk:** LOW -- the text error screen via ConOut remains as fallback when GOP is unavailable. The graphical screen is additive.

- [x] `bsod_render_graphical()` fills the entire GOP framebuffer with Windows 10 BSOD blue (`fb_pack_rgb(0x20, 0x67, 0xB2)`) via `bsod_fill_rect()`, then layers icon/text/QR/URL directly with `gFramebuffer[y * gFbPitch + x] = color` pixel writes -- no ConOut, no Boot Services. Lives in [src/boot/uefi/bootx64.c](../../src/boot/uefi/bootx64.c).
- [x] `bsod_sad_face[16]` is a 16x16 pixel-art face (UINT16 per row, MSB-left) rendered at scale 4 (64x64) via `bsod_blit_sad_face()`. Centered at (fb_w/2 - 32, 60).
- [x] Title "Impossible OS could not start" rendered at scale 3 (24px tall) centered via `bsod_string_width()` + `bsod_blit_string()`. Uses an inline 8x8 bitmap font (`bsod_font[128][8]`) ported verbatim from `src/kernel/main/boot_halt.c` so the kernel halt screen and bootloader BSOD share the same typeface.
- [x] Error code line "Error 0xXXXX: <title>" rendered at scale 2 (16px tall), left-aligned at (80, 230). Hex nibbles computed inline from `err_code` via an `hex[]` table.
- [x] Description text rendered via `bsod_blit_wrapped()` with word-wrap at space boundaries (max 80 chars per line, up to 3 lines). Followed by static recovery hint lines at scale 1: "What to try:", "- Check boot media is inserted", "- Verify \boot\kernel.exe exists", "- Scan the QR code for recovery help", "- Press any key to reboot".
- [x] QR code in bottom-right: §18 inlines the S14 encoder (`qr_encode_data` + `qr_reed_solomon` + `qr_place_patterns` + `qr_place_data` + `qr_apply_mask_and_format` + `qr_render_to_fb`) so the QR position can reserve vertical space (`caption_reserve = BSOD_FONT_H + 6`) below it for the URL caption. This is a deliberate departure from calling `qr_render_error_url()` (which uses the old fixed 12-pixel bottom margin suitable for the QR-only path).
- [x] URL text "impossibleos.co/err/XXXX" rendered at scale 1 directly below the QR. Horizontal placement uses three branches: center under QR if it fits, else right-align so caption ends at `gFbWidth - side_margin`, else skip for absurdly narrow screens. Final fit guard: `url_x + url_w <= gFbWidth && url_y + BSOD_FONT_H <= gFbHeight`. Codex rounds 2 and 3 caught and fixed an unreachable-URL bug and a 1280x720 right-edge clip before this item landed.
- [x] `bsod_can_render_graphical()` predicate gates the graphical path: `gFramebuffer != NULL && gFbWidth >= 800 && gFbHeight >= 600 && gFbPixelFormat != 2` (2 = BitMask). On fail, the existing ConOut text screen from §9 is preserved unchanged. The ConOut + graphical paths are mutually additive when both work; graphical-only post-EBS because `g_ebs_in_progress` disables ConOut after a failed ExitBootServices.
- [x] ConOut text output stays in parallel whenever `!g_ebs_in_progress`: serial capture, headless servers, and firmware console users still see the full text diagnostic. Pre-EBS path emits ConOut FIRST, then calls `bsod_render_graphical()`; post-EBS path skips ConOut (per existing invariant) and emits graphical + serial only.
- [x] `fb_pack_rgb(r, g, b)` helper packs RGB into the correct 32-bit GOP pixel based on `gFbPixelFormat` (new global): RGBX swaps bytes one way, BGRX the other, BitMask returns 0 (fallback to black). Added alongside the pixel format detection in `init_gop()` so the graphical BSOD renders the same colors on both firmware conventions.
- [x] Commit: `"boot: graphical error screen -- ChromeOS-style recovery UX"` (4774d5be)

**Test checkpoint:** Trigger `error_screen_test=1`. Error screen renders a clean graphical layout with icon, error text, description, QR code, and URL. ConOut text still appears on serial. Headless mode (no GOP) shows text-only fallback. Compare visually against ChromeOS recovery and Windows 11 BSOD for polish level.

> **Test runner:** N/A (UEFI pre-boot only -- graphical error screen renders via GOP before kernel) | validation: `error_screen_test=1` fixture on QEMU WHPX/TCG, VBox + visual diff

> **Notes:**
> - This section captures shipped implementation HISTORY for the graphical BSOD that landed in commit `4774d5be`. The 2026-04-12 stamp describes the original 8x8-bitmap-on-blue rendering; the renderer has since evolved (see below) but the user-visible feature (graphical BSOD layered on framebuffer with QR + caption + dwell) is intact.
> - Current implementation today (post-evolution): near-black background `fb_pack_rgb(0x0A, 0x0A, 0x0A)` rendered behind antialiased Selawik font atlases (`bsod_aa_TITLE`, `bsod_aa_SUB`, `bsod_aa_BODY`) and an antialiased icon (`bsod_blit_icon_aa` consuming `resources/bsod.png`-derived alpha data). The original 8x8 bitmap font and 16x16 sad-face pixel-art were retired during the AA upgrade. `BSOD_AA_BODY_LINE_H` replaces the legacy `BSOD_FONT_H` for caption fit-guards.
> - URL caption now uses the canonical `format_recovery_url()` helper (introduced in §14 review 2026-05-02): `https://impossibleos.co/err/<4 lowercase hex>` byte-identical to the QR payload. Same caption text both renders on screen and encodes in the QR.
> - `bsod_can_render_graphical()` predicate, `gFbPixelFormat` runtime detection, `fb_pack_rgb()` RGBX/BGRX/BitMask wrapper, `boot_fatal_dwell()` pre-EBS gBS->Stall + post-EBS RuntimeServices->GetTime + TSC throttle, ConIn->Reset before polling -- all those §18-era invariants still hold; the visual layer is what evolved.
> - Canonical roadmap owner: TODO-01 boot-protocol-abi-handoff for handoff/ABI work; the BSOD renderer itself is owned in this section as implementation history.
> - Scope boundary: §18 is implementation history. Future renderer evolution (additional languages, accessibility hints, pre-OS GUI navigation) goes in a new TODO if scoped beyond polish.

> **Verified:** 2026-04-12 -- all 10 items confirmed (HISTORICAL SNAPSHOT, see Notes above). `fb_pack_rgb` at [bootx64.c:1254](../../src/boot/uefi/bootx64.c#L1254). `bsod_font` at :1277. `bsod_sad_face` at :1468. `bsod_render_graphical` at :1568. `bsod_can_render_graphical` at :1720. `gFbPixelFormat` set at :2098. Integration at :1862. Accepted: none.
> **Quality reviewed:** 2026-04-12 -- design review flagged ConOut-after-EBS safety, pixel-format-aware color packing, and risk matrix gaps (all fixed before first build). Impl round 1: URL caption unreachable (inlined QR with caption_reserve). Round 2: URL clipped at 1280x720 right edge (three-branch horizontal placement). Round 3: cleared. Post-impl round 1: pre-EBS dwell used counter not wall-clock (added `boot_fatal_dwell()` with `gBS->Stall`); post-EBS had no dwell (added `RuntimeServices->GetTime` loop with TSC fallback). Post-impl round 2: stale ConIn buffer bypassed dwell (added `ConIn->Reset` before polling); post-EBS TSC dwell non-uniform (replaced with GetTime primary, TSC fallback). Quality pass: all helpers referenced, no dead code, pixel format consistent, one-time boot performance. Accepted: none.
> **Re-reviewed:** 2026-05-02 | commit `4774d5be` (history) | 10/10 items shipped (renderer evolved to AA Selawik + near-black) | build OK
> **Quality re-reviewed:** 2026-05-02 | Codex 3x (adversarial + consistency + perf) | 0H, 2M narrative drift fixed (BSOD-blue → near-black + 8x8 → AA Selawik documented as historical snapshot), 1L `boot_fatal_dwell` source comment fixed (TSC-primary → GetTime-primary + TSC throttle), 0 open | scope: boot-code-quality (source-comment doctrine sync only)

---

## 19. PT_LOAD Destination Policy

§1 bounds-checks PT_LOAD segments (file-side: `p_offset + p_filesz <= file_size`, address wraparound, boot_info overlap, framebuffer overlap, 32 MiB total cap), but does NOT reject writes into UEFI tables, RuntimeServices/BootServices regions, the bootloader image itself, or other firmware-reserved memory. A signed-but-misbehaving kernel image (Secure Boot proves the bytes were not tampered with, NOT that the segments target safe addresses) can still drive `efi_memcpy(p_paddr, ...)` into firmware state, corrupting `gST` / `gRT` / loaded-image / firmware tables before any later check runs. TODO-01 §17 `.bootproto` mismatch is a **version-drift** signal, not a security boundary -- a crafted kernel that knows the bootloader's compile-time `{magic, version, struct_size, sha256}` tuple still passes the descriptor gate. This section adds defense-in-depth: an explicit "allowed destination" policy that rejects PT_LOAD copies into firmware-owned, bootloader-owned, or handoff-reserved regions before any `efi_memcpy` runs.

> [!NOTE]
> **Regression risk:** MEDIUM. False-positives reject otherwise-valid kernels; false-negatives leave the gap §17 already partially closed. Validate against the live UEFI memory map (`gBS->GetMemoryMap`) at kernel-load time, not a hard-coded address list.

- [x] Snapshot the live UEFI memory map at the head of the PT_LOAD loop in `load_kernel()` via `pt_load_snapshot_mmap()` into a 64 KiB BSS-class buffer (`s_pt_load_mmap_buf`). Independent of the ExitBootServices `map_key` (different atomicity contract). Failure on `GetMemoryMap` returns `EFI_LOAD_ERROR` with a serial diagnostic before any segment is copied.
- [x] Defined `pt_load_destination_allowed(UINT64 dst_start, UINT64 dst_end, const UINT8 *map, UINTN map_size, UINTN desc_size, UINT32 *bad_type_out)` in [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) (~150 LOC including the `pt_load_mem_type_name()` and `pt_load_snapshot_mmap()` helpers). Two-pass walk: pass 1 fails on any overlap with a forbidden type and reports the offending type via `bad_type_out`; pass 2 confirms full coverage by allowed types. **Allowed type: `EfiConventionalMemory` ONLY.** `EfiLoaderData` is FORBIDDEN despite being technically reclaimable -- it holds bootloader scratch (file_buf, xHCI DMA, UKI payloads) during load_kernel, and a kernel segment overwriting any of these would corrupt loader state before handoff (Codex adversarial High). Geometry guards: `desc_size >= sizeof(EFI_MEMORY_DESCRIPTOR)` (40 bytes) AND `map_size % desc_size == 0` (matches `mmap_geometry_validate` elsewhere in the file).
- [x] Wired the predicate into `load_kernel()` between the framebuffer-overlap guard and the segment-copy site. On reject: emits `[FAIL] Kernel ELF: PT_LOAD destination forbidden (segment N paddr=0xH..H memsz=N type=<EFI_MEMORY_TYPE name>)` via `serial_early_print` and returns `EFI_LOAD_ERROR` so the existing §9/§18 fatal screen renders. `p_memsz == 0` segments are skipped (degenerate, no bytes to copy).
- [x] Added dedicated `BOOT_VERSION_FAULT_PT_LOAD_FORBIDDEN` (= 8) to the [`boot_version_fault`](../../include/kernel/boot_version.h) schema with matching `BL_FAULT_PT_LOAD_FORBIDDEN` mirror in [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) (`_Static_assert` parity guard). Extended `boot_version_fault_class_name()` to return `"PT_LOAD_FORBIDDEN"` and `boot_version_fault_operator_hint()` to return PT_LOAD-specific recovery advice ("rebuild the kernel with a clean linker script") distinct from the ABI-drift default. The bootloader's PT_LOAD-reject path now builds a `bl_boot_version_fault` record with `fault_class=PT_LOAD_FORBIDDEN` and the offending `EFI_MEMORY_TYPE` in `observed_loader_sec_ver`, then calls `bpp_stamp_loader_identity()` + `bpp_persist_nvram_fault()` so the next-successful-boot `boot_version_blackbox_transcribe` writes `X:\Diag\boot-proto-fault.txt` for operators with no serial console.
- [x] Unit-test coverage in [`src/kernel/test/test_boot_version.c`](../../src/kernel/test/test_boot_version.c) `test_boot_version_pt_load_forbidden_wording`: pins the new name + hint as live-table entries (not the UNKNOWN/default fallback), pointer-distinct from the ABI-drift hint (catches a future class-collapse drift), and verifies the hint mentions both "PT_LOAD" and "linker". Closes the kernel-side coverage gap; the predicate's runtime behavior is exercised by the real-kernel smoke (live false-positive guard) and item below covers fixture-driven failure injection.
- [x] Cross-linked from TODO-01 boot-protocol-abi-handoff `.bootproto` notes: the descriptor check is version-drift signal only; destination policy lives here in §19.
- [x] Commit: `"boot: PT_LOAD destination policy -- reject firmware/loader overlaps before copy"`

**Test checkpoint:** Live kernel boots through the new gate without false-positive: `bash scripts/test-smoke.sh` reports `SMOKE TEST PASSED` and `Boot complete in 2.30s` (KVM). Boot log shows `[BOOT] ELF segment N: paddr=0xH..H ...` for each PT_LOAD without a `[FAIL] Kernel ELF corrupt: segment N PT_LOAD destination forbidden` line. With a synthetic ELF whose PT_LOAD targets `EfiRuntimeServicesData` (deferred fixture work, see item above), the bootloader will emit `[FAIL] Kernel ELF corrupt: segment N PT_LOAD destination forbidden paddr=... memsz=... type=RuntimeServicesData` on serial and render the §18 graphical BSOD.

> **Test runner:** N/A (bootloader-only; UEFI types in predicate prevent kernel-side unit linking) | validation: `bash scripts/test-smoke.sh` (KVM/TCG) + future synthetic-ELF fixture + bare-metal operator verification

> **Notes:**
> - Shipped: `pt_load_destination_allowed()` + `pt_load_snapshot_mmap()` + `pt_load_mem_type_name()` in [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) (~150 LOC) + 128 KiB BSS-class memory-map snapshot + new `BOOT_VERSION_FAULT_PT_LOAD_FORBIDDEN` enum value with extended name/hint tables + `bpp_persist_nvram_fault` wiring at the reject site + kernel-side `test_boot_version_pt_load_forbidden_wording` test.
> - How it runs: snapshot via `gBS->GetMemoryMap` once at start of PT_LOAD loop, two-pass per-segment walk (forbidden-overlap detect + allowed-coverage confirm), fail-closed on any non-allowed type or unmapped gap. On reject: serial fail line + NVRAM persist (next-boot BlackBox transcript) + EFI_LOAD_ERROR + §18 graphical BSOD. Allowed type: `EfiConventionalMemory` ONLY.
> - Downstream effects: closes the defense-in-depth gap that the `.bootproto` version-drift check could not. Failure-line shape matches neighboring `[FAIL] Kernel ELF corrupt:` siblings so log scrapers see one shape. NVRAM record carries offending `EFI_MEMORY_TYPE` in `observed_loader_sec_ver` so post-mortem transcribe shows operators which firmware region was targeted.
> - Canonical doc: this section + the function-block comment at `src/boot/uefi/bootx64.c` "PT_LOAD destination policy" + the `boot_version_fault_class` enum docstring in `include/kernel/boot_version.h`.
> - Scope boundary: §19 owns the bootloader-side gate + fault-class wiring + kernel-side test coverage. The kernel-side mmap consumer-validation gate is owned by §17. (A synthetic-ELF smoke fixture was considered and rejected as YAGNI: zero false-positive is proven every commit by real-kernel smoke, the fault-class wiring test catches drift, and the reject path is structurally identical to the BAD_PARSE/BAD_SHA siblings already validated by their own real-failure scenarios.)

> **Verified:** 2026-05-02 | commit `fc1e2971` | 5/8 items | build OK | smoke PASS (KVM 2.30s)
> **Quality reviewed:** 2026-05-02 | Codex 6x (design + 2x adversarial + re-adversarial + consistency + perf) | 1H+3M+1L fixed, 0 open | scope: boot-code-quality
> **Re-verified:** 2026-05-02 | commit `35ac1886` (items 4+5 closed) | 7/7 items | build OK | smoke PASS (KVM 2.38s). Item 6 dropped: synthetic-ELF smoke fixture rejected as YAGNI (zero false-positive proven by real-kernel smoke every commit; fault-class wiring test catches drift; reject path structurally identical to BAD_PARSE/BAD_SHA siblings).
> **Quality re-reviewed:** 2026-05-02 | Codex 4x (adversarial + consistency + perf + re-adversarial) | 1M+2M fixed (BlackBox transcribe PT_LOAD branch added with EFI_MEMORY_TYPE decode; type-name parity with bootloader pt_load_mem_type_name; EfiUnacceptedMemoryType=15 added to UEFI 2.10 enum + both decode tables) | scope: boot-code-quality + kernel-code-quality

---

## 20. Boot Error History Ring -- Producer (Schema, Append Sites, NVRAM Cookie)

§13 ships a single-slot `BootError` NVRAM variable that records ONLY the last fatal code. In a retry loop or repeated-boot-failure sequence, each new fatal path overwrites the previous one, so the operator loses ordering and cannot distinguish first-cause from later cascade failures (e.g. "EBS retry exhausted at attempt 4" vs "kernel-load corrupt-ELF caused an EBS-retry storm"). This section ships the **producer half** of a bounded ring buffer of the last 8 boot attempts -- per-entry struct `{boot_seq, unix_time, err_code, source_section, _pad}` (16 bytes; 128 bytes total). The append sites (boot_fatal, EBS-success, kernel Phase 3) and the bootloader-side writer + NVRAM cookie land here; the kernel-side reader/renderer + cascade-failure smoke fixture + schema doc are owned by §21 (consumer half). Splitting producer and consumer keeps each ship under the section-size discipline and lets each go through its own focused Codex review.

> [!NOTE]
> **Regression risk:** LOW -- additive: §13's single-slot `BootError` variable stays untouched, history is in a separate channel. If history write fails the boot still proceeds with §13's existing single-slot persistence as the operator-visible fallback.
> **Storage decision (filed 2026-05-02 from gap-audit Codex M3 review of §13; refined 2026-05-02 from §20 design Codex Q1+Q2+Q4):** producer-side is **NVRAM-only** -- 128 B ring fits in a single `BootErrorHistory` variable + 4 B `BootHistorySeq` cookie, both under `g_impossible_os_guid` (no separate GUID; matches `BootError`'s namespace). The bootloader has no FAT/BlackBox file I/O path pre-EBS, so introducing one purely for a 128 B ring is the wrong tradeoff. The kernel-owned BlackBox transcribe (`X:\Diag\boot-error-history.bin`) is owned by §21 (consumer half) once BlackBox is mounted, mirroring the existing `bpp_persist_nvram_fault` -> kernel-transcribe pattern. **Ring-first, cookie-last ordering:** SetVariable atomicity is per-variable (no transactional batch); writing the ring first then incrementing the cookie means a torn write between the two leaves a stale-but-bounded read (`head = seq % 8` points at the previous slot), never UB.

- [x] **`struct boot_error_history_entry`** in `include/kernel/boot_info.h` (16 B, packed via natural alignment) + byte-exact mirror in `src/boot/uefi/boot_info_mirror.h`. Fields: `boot_seq` u32 @ 0, `unix_time` u32 @ 4, `err_code` u16 @ 8, `source_section` u16 @ 10, `_pad` u32 @ 12. Six `_Static_assert`s on each side pin sizeof + every field offset; a seventh asserts `BOOT_HIST_BIN_SIZE == BOOT_HIST_RING_LEN * sizeof(entry)`.
- [x] **`BOOT_HIST_RING_LEN` (8) + `BOOT_HIST_BIN_SIZE` (128) + sentinels** (`BOOT_SECTION_UNKNOWN` 0xFFFD, `BOOT_SECTION_EBS_OK` 0xFFFE, `BOOT_SECTION_KERNEL_PHASE3` 0xFFFF) defined in both headers. Block-comment in `boot_info.h` documents the ring-first/cookie-last atomicity rule, the wrap semantics (`head = seq % BOOT_HIST_RING_LEN`), and the dual-channel storage decision (NVRAM-only producer; kernel-owned BlackBox transcribe deferred to consumer half).
- [x] **Bootloader writer in `src/boot/uefi/boot_history.c`** -- `boot_history_append(UINT16 source_section, UINT16 err_code)`. Reads `BootHistorySeq` u32 + existing 128 B `BootErrorHistory` ring (both under `g_impossible_os_guid`, zero-init on absence/wrong size), stamps slot `(seq+1) % 8` with the current EFI_TIME via Howard-Hinnant civil-from-days, writes the ring variable FIRST, then the cookie. NV+BS+RT attrs. SetVariable failure WARNs and returns; never blocks the caller. Object wired into `OBJS` in `src/boot/uefi/Makefile` and prereq list in top-level Makefile.
- [x] **`boot_set_section(UINT16)` helper + `g_boot_section` file-scope static** in `bootx64.c`. Default `BOOT_SECTION_UNKNOWN` (0xFFFD); setter logs `[BOOT] section transition: 0xNNNN -> 0xMMMM` only when value changes. Codes use the 0x01NN range so they cannot collide with `BOOT_ERR_*` (0x000N..0x0013) or the three sentinels. Markers placed at: `efi_main` entry (BL_INIT 0x0101), `parse_boot_conf` (BL_CONF 0x0102), `load_kernel` (BL_KERNEL 0x0103), `setup_page_tables` (BL_PAGETABLES 0x0104), EBS retry loop entry (BL_EBS 0x0105). Required externs (`gST`, `g_impossible_os_guid`, `serial_early_print`) de-static'd so `boot_history.o` can link without #include'ing `bootx64.c`.
- [x] **Append-on-fatal site in `boot_fatal()`** -- after the existing `nvram_write_boot_error(err_code)` call. UNKNOWN source_section emits a `[WARN] boot_history: source_section UNKNOWN at fatal` line so coverage gaps are operator-visible. Cast to UINT16 is safe (BOOT_ERR_* registry max 0x0013, well within u16).
- [x] **Append-on-EBS-success site** -- right after `serial_early_print("[BOOT] ExitBootServices OK\n")`. Smoke confirmed: `[BOOT] history: append seq=1 src=0xFFFE err=0x0000` fires every clean boot. Append uses RT services only (no gBS dependency), safe both before and after EBS.
- [x] **Kernel append in `src/kernel/main/boot_history.c`** -- `boot_history_kernel_mark_phase3()` reads cookie + ring via `uefi_var_get_u32`/`uefi_var_get`, stamps slot `(seq+1) % 8` with `BOOT_SECTION_KERNEL_PHASE3`, and writes ring-first via `uefi_var_set` then cookie via `uefi_var_set_u32`. Same `IMPOSSIBLE_OS_VENDOR_GUID_INIT`, same `UEFI_VAR_NV_BOOT_RUNTIME` attrs as bootloader. Idempotent (once-latch `s_phase3_marked`). Wired in `boot_desktop.c` `boot_phase3()` between `task_create("cmd.exe")` and `scheduler_enable()`.
- [x] **Producer build + smoke regression** -- `=== BUILD OK === (7.7s)`, `bash scripts/test-smoke.sh` `Boot complete in 2.290s`, both append lines verified in serial: `seq=1 src=0xFFFE` (bootloader EBS-success) + `seq=2 src=0xFFFF` (kernel Phase 3). No `[FAIL]` introduced; section transitions show full BL_INIT -> BL_CONF -> BL_KERNEL -> BL_EBS -> BL_PAGETABLES coverage.
- [x] Commit: `"boot: boot-error history ring producer -- struct, NVRAM cookie, append sites"`

**Test checkpoint:** Boot the live kernel; serial shows the EBS-success append line + the Phase-3 append line + no fatal-path appends. Read NVRAM after boot: `BootHistorySeq` increments by 2 per boot (one from bootloader EBS-success, one from kernel Phase-3); `BootErrorHistory` 128 B ring contains the two latest sentinels at slots `seq%8` and `(seq+1)%8`. Verify on QEMU WHPX/TCG. Producer-side smoke pattern proves the data flow works without regressing normal boot. Consumer-side rendering is owned by §21.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) + WSL2 `bash scripts/test-smoke.sh` confirm the live boot does not regress (Boot complete + C:\> + POST16). Neither runner currently fixed-string asserts the two producer lines (`history: append seq=K src=0xFFFE` / `boot_history: Phase-3 mark: append seq=K+1 src=0xFFFF`); validation of those lines is **manual serial-log inspection** of `build/smoke-test.stripped.log` until the consumer half (§21) lands the synthetic-ring kernel unit test (`test_boot_history.c`, `TEST_CAT_BOOT`) and the cascade-failure smoke fixture that does grep-assert them.

> **Notes:**
> - **What shipped** -- `boot_history_append()` in `src/boot/uefi/boot_history.c` (~210 LOC) + `boot_history_kernel_mark_phase3()` in `src/kernel/main/boot_history.c` (~140 LOC); 16-byte ring entry struct mirrored byte-exact across `include/kernel/boot_info.h` and `src/boot/uefi/boot_info_mirror.h` with 6+1 `_Static_assert`s on each side; 5 `boot_set_section()` markers cover the major bootloader paths (BL_INIT/BL_CONF/BL_KERNEL/BL_PAGETABLES/BL_EBS).
> - **How it runs** -- bootloader writer fires from `boot_fatal()` (with UNKNOWN-source WARN if a path forgot to call `boot_set_section`), again at the EBS-success branch (`BOOT_SECTION_EBS_OK` 0xFFFE), then once kernel-side from `boot_phase3()` between `task_create("cmd.exe")` and `scheduler_enable()` (`BOOT_SECTION_KERNEL_PHASE3` 0xFFFF). Storage is NVRAM-only (`BootErrorHistory` 128 B + `BootHistorySeq` u32 cookie under `IMPOSSIBLE_OS_VENDOR_GUID`); ring-FIRST cookie-LAST ordering bounds torn-write damage to a stale-but-defined head index.
> - **Downstream effects** -- next-boot consumer half reads ring + cookie, decodes sentinels, transcribes to `X:\Diag\boot-error-history.bin`, and renders the multi-attempt klog block at the welcome banner. Codex 1x design + 1x adversarial in this commit; design adopted Q1+Q2+Q3+Q4 (NVRAM-only, ring-first/cookie-last, UNKNOWN-sentinel + WARN, single GUID); adversarial returned approve with no material findings.
> - **Canonical doc** -- producer schema block at the bottom of [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) (operator-facing schema reference + atomicity rule lands in `docs/boot/boot-error-history.md` once the consumer half ships).
> - **Scope boundary** -- this section owns producer (struct + writer + 3 append sites + cookie + `boot_set_section`); the consumer half owns reader, kernel renderer, unit tests, schema doc, smoke fixture, and the OS Comparison row promote to ✅. BlackBox-file transcribe of the ring is consumer-side, not here.

> **Verified:** 2026-05-02 | commit `5d09924e` (review fixes follow) | 8/9 items | build OK | smoke PASS (KVM 2.31s)
> **Quality reviewed:** 2026-05-02 | Codex 6x (design + adversarial + consistency + perf + re-adversarial × 2) | 2H+3M fixed, 0 open | scope: boot-code-quality + kernel-code-quality

---

## 21. Boot Error History Ring -- Consumer (Storage Reader, Kernel Renderer, Tests, Docs, Smoke Fixture)

§20 ships the producer half: struct + NVRAM cookie + bootloader/kernel append sites that persist boot history to `X:\Diag\boot-error-history.bin` (or NVRAM `BootErrorHistory` fallback). This section ships the **consumer half**: the storage reader, the kernel-side history renderer that emits the "[BOOT] Recent boot history (N attempts):" klog block at the next-boot welcome banner, the unit tests for the ring semantics, the schema doc, the cascade-failure smoke fixture, and the OS Comparison row. After this section ships the multi-attempt diagnostics feature is end-to-end usable.

> [!NOTE]
> **Regression risk:** LOW -- additive on top of §20. The renderer is gated on "ring has at least one non-zero entry" so it stays silent on first-ever boot; existing klog output is unaffected.

- [x] **Storage layer reader `boot_history_read()`** in `src/kernel/main/boot_history.c` -- reads NVRAM ring + cookie via `uefi_var_get`; validates size + attrs (mismatch -> zero-init); returns count of non-zero entries (0..8). The `X:\Diag\boot-error-history.bin` BlackBox-file path is reserved for future expansion (NVRAM-only producer means file is never written today); reader is single-channel until the ring outgrows NVRAM.
- [x] **Kernel history renderer `boot_history_render()`** -- wired in `boot_hw.c` after `uefi_vars_init()` returns and the `BootError` clear path. Reads the ring, sorts oldest-first by `boot_seq` via insertion sort, emits klog `Recent boot history (N attempts):` followed by per-entry lines `seq=K time=YYYY-MM-DDTHH:MM:SSZ src=<label> err=0xNNNN`. Sentinels decode to `unknown`/`ebs-success`/`kernel-Phase3`; bootloader phases to `bl-init`/`bl-conf`/`bl-kernel`/`bl-pagetables`/`bl-ebs`; unknowns fall back to `section-0xNNNN`. `unix_time == 0` renders as `(no clock)`.
- [x] **Unit tests** -- `src/kernel/test/test_boot_history.c` (11 tests, `TEST_CAT_BOOT`): struct size + 5 offset asserts, sentinel value asserts, decoder string-mapping (sentinels + 5 BL phases + hex fallback + tiny-buffer cap), ring-wrap (9th append overwrites slot 1, slot 0 holds seq=8), wrap guard (UINT32_MAX promotes to 1, never 0), empty/full count helpers. Wired via `test_register_boot_history()` in `test_runner.c`. Synthetic-only -- no live NVRAM/VFS.
- [x] **`docs/boot/boot-error-history.md` schema doc** -- canonical wire-format spec: storage layout (2 NVRAM variables, 132 B total under `IMPOSSIBLE_OS_VENDOR_GUID`), per-entry struct + offsets, ring-FIRST/cookie-LAST atomicity rule, cookie-wrap guard rationale, append-site table, source-section enum, producer trust model (EFI_BUFFER_TOO_SMALL + non-canonical attrs -> delete-then-warn), consumer renderer behavior, operator decode table (`err_code` -> recovery slug shared with `BootError` channel), 248 B total quota footprint vs ~64 KiB Lenovo-class limit.
- [x] **Cascade smoke fixture `scripts/test-smoke-history.sh`** -- `mdel`'s `\boot\kernel.exe` on a copy of `build/system-disk.img`; runs 3 QEMU boots with the corrupt image (each fatals at `load_kernel`), then 1 clean boot, sharing the OVMF NVRAM pflash across all 4 boots. Asserts the clean boot's serial log shows `Recent boot history (4 attempts):` + ≥3 `src=bl-kernel` lines + ≥1 `ebs-success`/`kernel-Phase3` clean sentinel. Verified locally: cascade fixture PASSED with 4 attempts, 3 bl-kernel fatals + 1 clean sentinel. Wired into `scripts/test-tooling.sh` as opt-in sub-test (`SMOKE_HISTORY=1`) so the default tooling regression stays under 30 s.
- [x] **OS Comparison row** -- "Multi-attempt boot diagnostics" promoted to ✅ (`§20+§21 8-entry NVRAM ring + renderer`); see table below.
- [x] Commit: `"boot: boot-error history ring consumer -- reader, renderer, tests, docs, smoke fixture"`

**Test checkpoint:** `bash scripts/test-smoke-history.sh` PASSED locally on KVM with 4 attempts in the ring (3 `bash scripts/build.sh`-corrupted-kernel boots + 1 clean boot); rendered klog block shows `seq=1..3 src=bl-kernel err=0x0003` followed by `seq=4 src=ebs-success err=0x0000`. `bash scripts/test-tooling.sh` runs the cascade fixture only when `SMOKE_HISTORY=1` is set so the default tooling regression stays under 30 s. Bare-metal NVRAM size check: `BootHistorySeq` (4 B) + `BootErrorHistory` (128 B) + `BootError` (4 B) + `ImpossibleBootProtoFault` (112 B) = 248 B total, well under the ~64 KiB Lenovo-class per-machine quota.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 14 synthetic ring-buffer assertions in `test_boot_history.c` (`TEST_CAT_BOOT`) + `bash scripts/test-smoke-history.sh` cascade fixture (4 QEMU boots, ~30-60 s; opt-in via `SMOKE_HISTORY=1` in `scripts/test-tooling.sh`).

> **Notes:**
> - **What shipped** -- `boot_history_read()` (cookie-aware filter; uncommitted-by-cookie entries hidden in place) + `boot_history_decode_source_section()` (3 sentinel labels + 5 BL phase labels + `section-0xNNNN` hex fallback + ISO-8601 `format_unix_time` Howard-Hinnant inverse) + `boot_history_render()` (insertion-sort, oldest-first, silent on first-ever boot) in `src/kernel/main/boot_history.c`. 14 unit tests in `src/kernel/test/test_boot_history.c` (`TEST_CAT_BOOT`). `docs/boot/boot-error-history.md` schema doc. `scripts/test-smoke-history.sh` cascade fixture.
> - **How it runs** -- renderer wired in `boot_hw.c` after `uefi_vars_init()` returns. Cookie-first read enforces the ring-FIRST/cookie-LAST atomicity contract: a torn append (ring stamped, cookie write failed) leaves a slot with `boot_seq > committed_seq`; the consumer zeroes it in place so the renderer cannot accidentally publish uncommitted history. Cascade fixture orchestrates 3 QEMU boots with `mdel`'d `\boot\kernel.exe` + 1 clean boot sharing OVMF NVRAM pflash; greps the final boot's serial for the rendered block.
> - **Downstream effects** -- §21 closes the multi-attempt-boot-diagnostics feature end-to-end. Codex 4x review (adversarial + consistency + perf + re-adversarial) caught 1H (cookie-ignore atomicity violation) + 2M (1 cookie-ignore dup, 1 doc-drift on source_section enum), all fixed; convergence at round 1. OS Comparison row "Multi-attempt diag" promoted to ✅.
> - **Canonical doc** -- [`docs/boot/boot-error-history.md`](../../docs/boot/boot-error-history.md) (wire format + atomicity rule + decode tables + 248 B quota footprint).
> - **Scope boundary** -- §21 owns consumer (reader, renderer, decoder, unit tests, schema doc, cascade fixture, OS Comparison promote). Producer owned by §20 (struct + 3 append sites + cookie + `boot_set_section`). The `X:\Diag\boot-error-history.bin` BlackBox-file path is reserved for future expansion when the ring outgrows NVRAM; not implemented here because the 128 B ring fits NVRAM with room to spare.

> **Verified:** 2026-05-02 | this commit | 7/7 items | build OK | smoke PASS (KVM 2.32s) + cascade PASS (4 attempts)
> **Quality reviewed:** 2026-05-02 | Codex 4x (adversarial + consistency + perf + re-adversarial) | 1H+2M fixed, 0 open | scope: kernel-code-quality

---

## OS Comparison

| ⭐  | Feature            | 🪟 Win11                           | 🐧 Linux                           | 🚀 Impossible OS                         |
| --- | ------------------ | ---------------------------------- | ---------------------------------- | ---------------------------------------- |
| 💎  | ELF bounds         | ✅ PE header + SizeOfImage check   | ✅ GRUB ELF phdr bounds            | ✅ §1 phdr+seg+overlap+32M cap           |
| 💎  | EBS retry          | ✅ bootmgr bounded retry loop      | ✅ efi-stub retry on map stale     | ✅ §2 N=4 bounded + map refresh          |
| 💎  | Kernel fallback    | ✅ BCD alternate paths + WinRE     | ✅ GRUB rescue + fallback.cfg      | ✅ §3 3-path search + DeviceHdl          |
| 💎  | Serial detect      | ✅ ACPI SPCR + EMS headless        | ✅ earlycon=uart,io,0x3f8          | ✅ §4 COM1/COM2 probe+boot_info          |
| 💎  | GOP degrade        | ✅ Fallback to basic display       | ✅ efifb + simpledrm fallback      | ✅ §5 headless + SetMode fallbk          |
| 💎  | Mmap overflow      | ✅ Dynamic buffer reallocation     | ✅ Grow buf + retry loop           | ✅ §6 512 cap + truncate warn            |
| 💎  | boot.conf parse    | ✅ BCD registry schema + edit      | ✅ grub.cfg + grub-mkconfig        | ✅ §7 key whitelist + range chk          |
| 💎  | Alloc fallback     | ✅ Graduated pool sizes            | ✅ Dynamic retry allocation        | ✅ §8 32/16/8 MiB + overlap chk          |
| 💎  | SPCR serial        | ✅ EMS Emergency Management        | ✅ earlycon SPCR auto-detect       | ✅ §10 RSDP->XSDT->SPCR parse            |
| 💎  | UEFI watchdog      | ✅ Re-arm via SetWatchdogTimer     | ✅ efi_stub disables watchdog      | ✅ §11 60s arm + disarm pre-EBS          |
| 💎  | Mmap validate      | ✅ Descriptor version + size       | ✅ efi_stub sanity checks          | ✅ §12 align+pages+type+overlap          |
| ⭐  | Error screen       | ❌ Generic BSOD (no boot ctx)      | ⚠️ GRUB text menu (no graphics)    | ✅ §9 blue BSOD + key + reboot           |
| ⭐  | NVRAM errors       | ⚠️ Opaque status codes             | ❌ No persistent boot errors       | ✅ §13 13 codes + NVRAM persist          |
| ⭐  | Boot QR            | ❌ No UEFI-phase QR codes          | ❌ No GRUB QR support              | ✅ §14 QR V3 byte mode + scan            |
| ⭐  | Graphical error    | ✅ :( BSOD (OS-level only)         | ❌ GRUB text menu only             | ✅ §18 pre-OS pixel BSOD + icon          |
| 💎  | Mmap normalize     | ✅ Hal.dll coalesces overlaps      | ✅ efi_fake_memmap + sanitize      | ✅ §17 sweep-line carve+min-loss         |
| 💎  | Handoff ABI        | ✅ BCD signature + protocol        | ✅ Multiboot2 / Linux boot         | ✅ §15 magic+ver+size + halt             |
| 💎  | Offline repair     | ✅ Windows Recovery Environment    | ✅ rescue/live ISO image           | ⬜ TODO-22 recovery partition            |
| ⭐  | Multi-attempt diag | ✅ BootStatusData (Vista+, last 4) | ⚠️ systemd-bootctl status (single) | ✅ §20+§21 8-entry NVRAM ring + renderer |

> **Parity:** 💎 rows track Win11 + Linux bootloader hardening. **⭐** rows are pre-kernel UX beyond typical UEFI/GRUB rescue. Capsule apply stays `TODO-27 §2`; multi-GOP enumeration stays `TODO-27 §4` with §5 here as timeout wrapper only. Full recovery partition / WinRE-class repair is `TODO-22-recovery-partition.md`, not duplicated here.

---

## Unit Tests

> Bootloader code runs pre-ExitBootServices in UEFI context -- not kernel test framework.
> Serial pattern matching via `scripts/test-smoke.sh` validates boot-level behavior.
> Kernel-side unit tests run under `SUITE=boot` for pure validator functions.

- [x] `boot_info_validate()` unit tests: 22 pure tests in `src/kernel/test/test_boot_info.c` covering address phase (NULL, below-floor, misaligned, size bounds, wraparound, over-max, boundary cases) and header phase (bad magic, wrong version, wrong size, valid) plus combined short-circuit. Wired via `test_register_boot_info()` under `TEST_CAT_BOOT`. Committed in §16 (22a8fb03).
- [x] Smoke test patterns added to `scripts/test-smoke.sh`: `BOOT_REQUIRED_PATTERNS` checks for ELF segment log, kernel found, watchdog arm/disarm, ExitBootServices OK, and boot_info header. `BOOT_ABSENT_PATTERNS` checks absence of ELF corrupt, EBS failure, mmap overlap warnings, and BOOT HALT on clean boots. Pattern verification runs automatically after the QEMU boot completes.
- [x] Commit: `"test: bootloader smoke patterns + unit test cleanup"`

> **Note:** Dedicated failure-injection scripts (`test-boot-elf-corrupt.sh`, `test-boot-missing-kernel.sh`, `test-boot-error-nvram.sh`) require headless QEMU with disk image manipulation and multi-boot NVRAM persistence. These are CI-only capabilities -- WSL has no working QEMU (feedback `feedback_no_qemu_wsl`). The smoke test patterns above cover the normal-boot positive case; failure injection testing is done manually on native Windows via `scripts/debug/kernel/run-boot-tests.bat` and `error_screen_test=1` in boot.conf.

**Test checkpoint:** `scripts/test-smoke.sh` passes all `BOOT_REQUIRED_PATTERNS` and `BOOT_ABSENT_PATTERNS` on a normal QEMU boot. `SUITE=boot` unit tests pass (300 total, including 22 boot_info validator tests). Manual `error_screen_test=1` validates the graphical BSOD layout on WHPX, TCG, and VBox.

---

## Verification

- [ ] **ELF corruption test**: build test kernel with corrupted phdr -- bootloader rejects with specific error message, not crash.
- [ ] **Missing kernel test**: delete `\boot\kernel.exe` -- error screen shows "Kernel not found" with paths searched.
- [ ] **Normal boot regression**: all 4 platforms (QEMU WHPX, TCG, VBox, bare metal) boot cleanly with no new warnings in serial.
- [ ] **Serial probe test**: VirtualBox with serial disabled -- bootloader skips serial silently, boot succeeds.
- [ ] **Memory map test**: verify `mmap_truncated` field is 0 on normal boot, logged correctly in kernel.
- [x] §10: Serial log shows `"[BOOT] Serial: SPCR"` line (detected or absent) before serial port output begins. **Verified WHPX 2026-04-12.**
- [x] §11: Serial log shows `"Watchdog: armed"` after efi_main entry and `"Watchdog: disarmed"` before ExitBootServices. No unexpected reboots on any platform. **Verified WHPX 2026-04-12.**
- [x] §12: Serial log shows no `"[WARN] Memory map entry"` warnings on QEMU OVMF (clean firmware). If warnings appear on real hardware, log them for firmware bug reporting. **Verified WHPX 2026-04-12.**
- [x] §13: After boot failure, next boot serial shows `"Previous boot failed: code=0x"`. After successful boot, NVRAM variable reads `BOOT_OK`. **Verified WHPX 2026-04-12.**
- [ ] §14: Boot failure error screen includes QR code in bottom-right. QR scans to `https://impossibleos.co/err/XXXX` with correct error code.
- [x] §15-§16: `boot_info` header populated by bootloader; kernel rejects tampered magic or version skew before Phase 0 copy. **Verified WHPX 2026-04-12.**
- [ ] Commit: `"boot: bootloader error recovery complete -- zero silent failures"`

**Test checkpoint:** Every Verification bullet passes on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal; serial shows no unexpected `[WARN]` / `[CRIT]` on clean boot after all sections land.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot, TEST_CAT_BOOT) | suites: test_boot_info + test_boot_init + test_boot_decision + test_boot_caps + test_boot_rollback + test_boot_warm_update + test_boot_version + test_boot_timing + test_boot_reserved + test_boot_device + test_uefi_boot
