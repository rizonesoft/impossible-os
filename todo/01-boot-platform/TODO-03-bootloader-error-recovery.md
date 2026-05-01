---
schema_version: 1
id: bootloader-error-recovery
domain: 01-boot-platform
status: active
title: "TODO-03 -- Bootloader Error Recovery & ELF Hardening"
---

# TODO-03 -- Bootloader Error Recovery & ELF Hardening

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

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | ELF bounds checking                            | --         |  [/]   |
| 💎  |   2   | ExitBootServices retry loop (bounded)          | --         |  [x]   |
| 💎  |   3   | Fallback kernel search (3 paths)               | --         |  [x]   |
| 💎  |   4   | Serial port probe and COM2 fallback            | --         |  [x]   |
| 💎  |   5   | GOP timeout and graceful degradation           | --         |  [x]   |
| 💎  |   6   | Memory map overflow detection (512 entries)    | --         |  [x]   |
| 💎  |   7   | boot.conf validation and version field         | --         |  [/]   |
| 💎  |   8   | Kernel load allocation fallback (32-16-8 MiB)  | --         |  [x]   |
| ⭐  |   9   | Boot failure error screen                      | §1-§8      |  [x]   |
| 💎  |  10   | ACPI SPCR serial port auto-detection           | §4         |  [x]   |
| 💎  |  11   | UEFI watchdog timer management                 | --         |  [x]   |
| 💎  |  12   | Memory map descriptor validation               | §6         |  [x]   |
| ⭐  |  13   | Boot error code registry & NVRAM persistence   | §9         |  [/]   |
| ⭐  |  14   | Error screen QR code                           | §9         |  [x]   |
| 💎  |  15   | boot_info ABI foundation moved to TODO-01      | --         |  [x]   |
| 💎  |  16   | boot_info kernel validation moved to TODO-01   | §15        |  [x]   |
| 💎  |  17   | Memory map overlap normalization (sort+carve)  | §12        |  [x]   |
| ⭐  |  18   | Graphical error screen (ChromeOS/Win11-style)  | §9, §14    |  [x]   |
| 💎  |  19   | PT_LOAD destination policy (defense-in-depth)  | §1         |  [ ]   |

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
- [ ] **Consolidate ELF typedefs with shared header** (review-todo-section 2026-05-01 Codex consistency H1): `load_kernel()` defines its own `Elf64_Ehdr` / `Elf64_Phdr` at [`bootx64.c:40-76`](../../src/boot/uefi/bootx64.c) while [`src/boot/uefi/elf_types.h`](../../src/boot/uefi/elf_types.h) carries a second copy used by the .bootproto descriptor walker. Two byte-compatible copies both need to track the on-disk ELF spec; drift between them would let one path read fields at different offsets than the other. Include `elf_types.h` from `bootx64.c`, delete the local typedefs, and add `_Static_assert(__builtin_offsetof(Elf64_Ehdr, e_phoff) == 32, ...)` etc. for every field `load_kernel()` reads (e_phoff, e_phentsize, e_phnum, p_offset, p_filesz, p_memsz, p_paddr) so a future ABI change to elf_types.h breaks the build instead of silently changing field offsets. Owner: this section.
- [ ] **Replace byte-at-a-time efi_memcpy / efi_memset with word-width or rep movsb path** (review-todo-section 2026-05-01 Codex perf M1): the helpers at [`bootx64.c:186-200`](../../src/boot/uefi/bootx64.c) are one-byte loops; `load_kernel()` calls them across multi-MiB PT_LOAD segments + BSS clears, executing millions of byte stores per boot before kernel handoff. Switch to: (a) `gBS->CopyMem` / `gBS->SetMem` while pre-EBS (UEFI firmware provides optimized implementations), with the byte-loop fallback retained for post-EBS callers, OR (b) inline-asm `rep movsb` / `rep stosb` for x86-64 (modern microarchitectures fast-path these). Measure boot-time delta on QEMU TCG (where the savings are largest). Owner: this section.
- [x] Commit: `"boot: harden ELF parser -- bounds check all headers and segments"` (3c888540)

**Test checkpoint:** Build a test kernel with `e_phoff` pointing past EOF. Bootloader must reject with `"Kernel ELF corrupt: phdr offset past EOF"` on serial. Verify on QEMU WHPX and TCG. Normal kernel must pass all checks on all 4 platforms (WHPX, TCG, VBox, bare metal).

> **Notes:**
> - What shipped: 8 ELF bounds checks at [`bootx64.c:3855-4020`](../../src/boot/uefi/bootx64.c) covering ehdr file-size guard, magic/class/machine, `e_phentsize == sizeof(Elf64_Phdr)`, `e_phnum <= 64`, `ELF_MAX_KERNEL_SIZE = 32 MiB`, `e_phoff` + phdr-table EOF bounds, per-PT_LOAD `p_offset + p_filesz` subtraction-based bounds, `p_memsz >= p_filesz`, address wraparound, full `sizeof(struct boot_info) = 23872`-byte boot_info overlap, `pitch * height`-with-overflow-guard framebuffer overlap.
> - How it integrates: invoked by `load_kernel()` in the split-path boot before the segment-copy loop and before any `efi_memcpy` writes; UKI fast path skips this code entirely (signed PE means the kernel bytes are already firmware-verified). Returns `EFI_LOAD_ERROR` to the caller on any failure; caller maps to `boot_fatal(BOOT_ERR_ELF_CORRUPT, ...)` for the §9 error screen.
> - Downstream effects: blocks malformed/hostile kernel images from clobbering boot_info, framebuffer, or low memory through unchecked PT_LOAD destinations. PT_LOAD destination policy (forbid firmware/loader regions) is owned by §19 below as an open `[ ]` item. Codex 3x review adoptions in commit `<hash>`.
> - Canonical doc: this section + ELF spec (TIS Tool Interface Standard 1.2 + System V ABI x86-64) + UEFI 2.10 section 13 LoadedImage handoff.
> - Scope boundary: §1 owns ELF FILE-side validation (offsets, sizes, magic, header fields). §19 owns PT_LOAD DESTINATION policy (where in physical memory segments may write). §17 owns memory-map overlap normalization for runtime regions; this section's overlap checks are static against boot_info + framebuffer only.

> **Verified:** 2026-05-01 | review-todo-section re-verify | 8/10 items, 2 new follow-up [ ] | build OK | smoke PASS (KVM 2.450s). All 8 original checks confirmed at file:line. Prior 2026-04-29 + 2026-04-11 verifications retained.
> **Accepted:** [H] PT_LOAD destination policy below 1 MiB floor not enforced -> XREF: 01-boot-platform/TODO-03 §19 (item: "Define `bool pt_load_destination_allowed(...)` predicate" at line 508 -- EfiConventionalMemory/EfiLoaderData allowlist naturally rejects p_paddr < 0x100000)
> **Accepted:** [M] BOOT_INFO_PHYS_ADDR macro duplicated across bootx64.c + kernel/mm/boot_reserved.c + kernel/main/boot_payload.c -> XREF: 01-boot-platform/TODO-01 §1 (item: "Single source of truth for `BOOT_INFO_PHYS_ADDR` macro" filed 2026-05-01 -- move to boot_info.h, mirror in boot_info_mirror.h, delete three local #defines)
> **Quality reviewed:** 2026-05-01 | Codex 3x (adversarial + consistency + perf) | 1H accepted-XREF (PT_LOAD floor -> §19), 1H + 1M deferred-§1-followup (ELF typedef consolidation, byte-loop memcpy/memset), 1M accepted-XREF (BOOT_INFO_PHYS_ADDR -> TODO-01) | scope: boot-code-quality (gates walked: UEFI types, error handling, EBS boundary, boot_info ABI sync (Gate 6 finding -> Accept-XREF), parse buffer dynamic alloc).

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

> **Notes:**
> - Three hardcoded kernel paths (`\boot\kernel.exe` -> `\kernel.exe` -> `\EFI\ImpossibleOS\kernel.exe`) tried in order on the boot device's SimpleFS, with LocateHandleBuffer-based all-volumes fallback when the kernel is absent from the boot device.
> - `LoadedImage->DeviceHandle` resolved at efi_main into `g_boot_device_handle`; degraded paths (NULL handle or no SimpleFS) defer to the explicit all-volumes search with `[WARN]` diagnostics rather than silently calling `LocateProtocol` (rebuilt 2026-05-01).
> - Primary loop and all-volumes inner loop share identical error policy: `EFI_NOT_FOUND` continues, every other Open status closes resources, `FreePool(fs_handles)`, and returns the underlying status.
> - POST16 sequence: `POST16_BL_FALLBACK` (0xB094) on entry to all-volumes search; `POST16_BL_FALLBACK_OK` (0xB095) only on success; left at 0xB094 on total failure so a POST card shows fallback-failed.
> - Scope boundary: `parse_boot_conf()` and `locate_boot_fs()` still use the silent LocateProtocol pattern; consistency follow-ups filed in §7 and TODO-02 §16 to mirror the §3 hardening.
> **Verified:** 2026-05-01 | commit `<pending>` | 5/5 items | build OK | smoke PASS (KVM 2.46s)
> **Accepted:** [H] Silent LocateProtocol fallback in `parse_boot_conf()` (reason: scope -- §7 owns boot.conf reads) -> XREF: 01-boot-platform/TODO-03 §7 (item: "Eliminate silent LocateProtocol fallback in `parse_boot_conf()`" at line 249)
> **Accepted:** [H] Silent LocateProtocol fallback in `locate_boot_fs()` (reason: scope -- TODO-02 §16 owns UKI staged-payload disk reads) -> XREF: 01-boot-platform/TODO-02 §16 (item: "Eliminate silent LocateProtocol fallback in `locate_boot_fs()`" at line 540)
> **Quality reviewed:** 2026-05-01 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+1M fixed, 2H accepted-XREF | scope: boot-code-quality

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

> **Verified:** 2026-04-11 -- all 4 items confirmed. QueryMode 100-error abort, SetMode->mode0->firmware fallback, headless boot on no GOP, logging at each stage. Additional hardening from 01-boot-platform/TODO-02 §4 quality review (FrameBufferSize bounds, Mode NULL guard, pitch validation). Accepted: none.
> **Quality reviewed:** 2026-04-11 -- no additional findings. Shared code with 01-boot-platform/TODO-02 §4 which received full quality review (FrameBufferSize bounds, Mode/Info NULL guard, pitch validation, gop_pixel_format_code dedup). Defensive wrappers match UEFI best practice for GOP error recovery. Accepted: none.

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

- [x] Dynamic file-size probing via `GetInfo(&EFI_FILE_INFO_ID, ...)` + `gBS->AllocatePool(FileSize+1)`. Sanity cap `BOOT_CONF_SANITY_CAP = 1 MiB` triggers `boot_fatal(BOOT_ERR_CONF_INVALID, ...)` on overflow rather than the previous silent-truncate behavior. Original 4096-byte fixed buffer + truncation warning was retired in the 2026-04-21 incident fix (CLAUDE.md "Bare Metal Gotchas: Dynamic parse buffers + hard-fail on overflow") -- a `test=1` line at EOF was being silently dropped when boot.conf grew past 4096 bytes.
- [x] After parsing each key=value: unknown keys produce `"[WARN] boot.conf: unknown key 'X' (ignored)"`
- [x] Known-key whitelist: all existing keys + new `config_version` key
- [x] `config_version` field added to `boot_config` struct (1 byte from `_reserved[]`). `config_version=1` in boot.conf sets it.
- [x] Range validation: `debug`/`verbose`/`test` clamped to 0-1, `splash_timeout` clamped to 0-60 (default 3). Out-of-range logs `"[WARN] boot.conf: X out of range, using N"`
- [x] Commit: `"boot: validate boot.conf -- warn on unknown keys and out-of-range values"` (83b2ccb8)
- [ ] **Eliminate silent LocateProtocol fallback in `parse_boot_conf()`** (filed 2026-05-01 from §3 review Codex consistency H2): `parse_boot_conf()` at `src/boot/uefi/bootx64.c:2477-2495` still calls `gBS->LocateProtocol(&fs_guid, ...)` when `g_boot_device_handle` is NULL or lacks SimpleFS, picking the first SimpleFS volume firmware enumerates. After §3's hardening, `load_kernel()` no longer does this -- so on a degraded boot-device path, boot.conf can come from a different ESP than the kernel, defeating the trust boundary. Implementation: mirror the §3 pattern -- on degraded paths, log `[WARN] Boot device has no SimpleFS for boot.conf` and skip the parse entirely (boot_config defaults already populated by `boot_config_defaults()`); OR enter an explicit all-volumes search with `[WARN]` per-volume diagnostics that matches `load_kernel()`'s fallback chain. Test: with QEMU `-drive` set up so the bootloader's LoadedImage->DeviceHandle has no SimpleFS protocol but a second SimpleFS handle exists, assert serial shows the explicit warn (not silent first-volume selection) and that boot.conf is either skipped or selected with a logged volume identity.

**Test checkpoint:** Add `bogus_key=42` to boot.conf. Serial must show `"unknown key 'bogus_key'"`. Set `splash_timeout=999` -- serial must show `"out of range, using default 3"`. A boot.conf > 1 MiB must trigger `boot_fatal(BOOT_ERR_CONF_INVALID)` (no silent truncation).

> **Verified:** 2026-04-11 -- all 5 items confirmed. 4096-byte buffer with truncation warning, unknown key logging, config_version field, range validation for debug/verbose/test/splash_timeout. Accepted: none.
> **Quality reviewed:** 2026-04-11 -- no findings. More user-friendly than Windows BCD (human-readable + warnings) and more defensive than GRUB (range clamping vs silent accept). Accepted: none.
> **Drift fix:** 2026-05-01 (gap-audit Codex M2) -- bullet 1 rewritten from "4096-byte buffer + truncation warning" to dynamic AllocatePool + 1 MiB hard-fail to match the post-2026-04-21 implementation; original wording would let a future validator accept the silent-truncation failure mode the incident eliminated.

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

> **Verified:** 2026-04-11 -- all 4 items confirmed. 6 validation checks (overflow, wrap, zero, align, type, overlap) + desc_size geometry guard at caller. Codex found NumberOfPages overflow (fixed) and desc_size=0 path (fixed with boot_fatal at caller level). Accepted: overlap normalization (-> XREF: 01-boot-platform/TODO-03 §17).
> **Quality reviewed:** 2026-04-11 -- boot-code-quality 10/10 gates passed. Codex quality found desc_size geometry + map_size remainder (fixed: boot_fatal guard before both fill functions). EfiMaxMemoryType off-by-one fixed (>= not >). Accepted: overlap priority normalization (-> XREF: 01-boot-platform/TODO-03 §17).

---

## 13. Boot Error Code Registry & NVRAM Persistence

Windows has BootStatusPolicy but error codes are opaque hex values without context. Linux has no bootloader-level error persistence. This section creates a structured error code system where each failure has a unique code, and the last-boot error is saved in UEFI NVRAM for next-boot diagnostics.

> [!TIP]
> **Competitive advantage:** Neither Windows nor Linux persists structured bootloader error codes in NVRAM. The next boot can display "Last boot failed: 0x0003 -- Kernel not found at \boot\kernel.exe" before trying again. Combined with TODO-21 A/B rollback and TODO-14 §6 panic forensics, this gives a complete cross-boot diagnostic chain.
> **Regression risk:** LOW -- NVRAM writes are non-destructive (single variable). If NVRAM is full or read-only, write silently fails and boot continues.

- [x] Define `BOOT_ERR_*` codes as `#define` constants in `efi.h` (13 codes: 0x0000-0x000C covering §1-§12 failure modes)
- [x] On fatal error: `boot_fatal()` writes error code to UEFI NVRAM variable `BootError` (Impossible OS vendor GUID, non-volatile + boot-service-access + runtime-access) via `nvram_write_boot_error()` helper
- [x] On successful boot: kernel clears `BootError` NVRAM variable in `boot_phase0()` after `uefi_vars_init()` succeeds -- clearing in the kernel (not bootloader) ensures a crash between EBS and Phase 0 preserves the error evidence
- [x] At boot entry: `nvram_read_boot_error()` reads NVRAM -- if non-zero, logs `"[BOOT] Previous boot failed: code=0x%04x"` on serial
- [x] Pass `boot_info.last_boot_error` to kernel -- field added to both bootloader struct (bootx64.c) and kernel header (boot_info.h) after `serial_baud`; kernel logs `"Previous boot failed: code=0x%04X"` via klog in `boot_hw.c`
- [x] `boot_fatal()` includes the error code in on-screen display: `"Error code: 0xNNNN"` line on BSOD screen, plus hex code in serial log
- [ ] **Bounded boot-error history ring** (gap-audit 2026-05-01 Codex M3): the current `BootError` NVRAM variable records ONLY the last fatal code. In a retry loop or repeated-boot-failure sequence, each new fatal path overwrites the previous one, so the operator loses ordering and cannot distinguish first-cause from later cascade failures. Ship a bounded ring buffer of the last 8 boot attempts: per-entry struct `{boot_seq, unix_time, err_code, source_section, _pad}` (16 bytes, 128 bytes total) stored in either (a) a new `BootErrorHistory` NVRAM variable with a 128-byte hard cap to stay well under the per-machine NVRAM quota (Lenovo class 64 KiB total), OR (b) X:\Diag\boot-error-history.bin in the BlackBox partition with a single NVRAM-resident pointer/seq counter (preferred -- no NVRAM pressure). Bootloader appends one entry on each fatal exit + one entry on successful EBS handoff (so the kernel can prove "boot reached this point"); kernel clears the in-flight slot on successful Phase 3 reach. Provides Win11 BootStatusData-equivalent multi-attempt diagnostics. Owner: this section; reuses §13's `nvram_write_boot_error` plumbing.
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
- [x] Standalone encoder (TODO-14 §7 not yet implemented) -- when §7 lands, the kernel-side encoder can be factored from this implementation
- [x] `error_screen_test=1` boot.conf key triggers `boot_fatal()` before kernel load for QR/BSOD testing (halts instead of rebooting so screen stays visible)
- [x] Commit: `"boot: QR code on boot error screen -- scan for recovery instructions"` (7e588e7e)

**Test checkpoint:** Run `scripts/debug/kernel/run-error-screen-test.bat` (sets `error_screen_test=1`). Error screen shows QR code in bottom-right. Scan with phone -- URL `https://impossibleos.co/err/0003` appears. Verify QR is scannable at 1280x720 and 1920x1080 resolutions.

> **Verified:** 2026-04-11 -- Codex adversarial found RS coefficients wrong + format placement wrong (both fixed). Post-fix Codex clean. Real-hardware testing found 3 additional bugs: (1) pixel colors 0xFF000000=blue in BGRX, (2) format bits MSB/LSB reversed (Codex was wrong about this), (3) V2 alphanumeric uppercase URL -- switched to V3 byte mode for lowercase. Accepted: none.
> **Quality reviewed:** 2026-04-11 -- Switched to QR V3 byte mode after user feedback. Three V3 bugs fixed: zigzag loop skipped column pairs after timing column, zigzag direction formula wrong for cols < 6, byte padding added zero codeword when already aligned. All verified against segno: 0 differences. Accepted: none.

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

> **Verified:** 2026-04-11 -- all 8 items confirmed with code evidence. Header defined at offset 0 in both `include/kernel/boot_info.h:47` and `src/boot/uefi/bootx64.c:199`. Static asserts for offset/size/<=65535 present on both sides. Bootloader populates at `bootx64.c:3748` as the last step before `jump_to_kernel()`. Kernel validates at `boot_hw.c:70-96` with observed vs expected LOG_ERROR diagnostics before `boot_halt()`. Accepted: pre-memcpy pointer/bounds validation (`boot_info_validate()`) and cross-build layout fingerprinting -> XREF: 01-boot-platform/TODO-03 §16.
> **Quality reviewed:** 2026-04-11 -- Codex adversarial round 1 flagged LOG_FATAL-before-boot_halt regression (klog LOG_FATAL is no-return); fixed with LOG_ERROR. Round 2 and 3 clean. Dead code + consistency + performance pass -- no findings. Win11 uses BCD signature + Boot Services Protocol GUID; Linux Multiboot2 uses 0x36D76289 + tag format; our magic+version+size + runtime validation is comparable scope. Accepted: 2 HIGH/MED findings accepted as -> XREF: 01-boot-platform/TODO-03 §16 (pre-copy validation, mirror struct drift detection).

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

> **Verified:** 2026-04-11 -- all 9 items confirmed. Three validators at [boot_info.c](../../src/kernel/main/boot_info.c) lines 31 / 77 / 95. Phase 0 call sites at [boot_hw.c:72](../../src/kernel/main/boot_hw.c#L72) and [boot_hw.c:86](../../src/kernel/main/boot_hw.c#L86) with `BOOT_INFO_EARLY_MAP_END` and LOG_ERROR failure paths. Ten mirrored `_Static_assert` offset checks at [boot_info.h:518-529](../../include/kernel/boot_info.h#L518) and [bootx64.c:306-315](../../src/boot/uefi/bootx64.c#L306). Two overlap guards at [bootx64.c:2017](../../src/boot/uefi/bootx64.c#L2017) and [bootx64.c:2128](../../src/boot/uefi/bootx64.c#L2128). 22 pure tests registered via `test_register_boot_info()` at [test_runner.c:227](../../src/kernel/test/test_runner.c#L227). Accepted: none.
> **Quality reviewed:** 2026-04-11 -- Codex round 1 caught the bootloader `AllocatePages(AllocateAnyPages)` free-pool gap (boot_info not reserved via `AllocateAddress`) and round 2 cleared after the reservation fix at [bootx64.c:3556-3583](../../src/boot/uefi/bootx64.c#L3556). Dead code + consistency + performance pass -- ten offset asserts match on both sides, validators are all referenced, removed post-copy re-checks have no dangling callers, `cmdline` ASCII check reachable, no hot-path allocations. Parity: Linux efi-stub reserves the kernel decompression area via `AllocateAddress` for the same reason; Windows `bootmgfw.efi` uses `BlMmAllocatePhysicalPages` with `AllocateAddress` equivalent. Accepted: none.

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

> **Verified:** 2026-04-12 -- all 8 items confirmed with code evidence. Priority table at [bootx64.c:2319](../../src/boot/uefi/bootx64.c#L2319) with Runtime promoted above ACPI Reclaim. Sweep-line `mmap_normalize()` at [bootx64.c:2504](../../src/boot/uefi/bootx64.c#L2504) with event sort, active set, and coalescing emit. BSS work buffers ~34 KiB. `mmap_evict_for_incoming()` min-loss ranking at [bootx64.c:2584](../../src/boot/uefi/bootx64.c#L2584). `fill_memory_map()` three-phase rewrite at [bootx64.c:2670](../../src/boot/uefi/bootx64.c#L2670) with post-normalize `total_mem` recompute. Accepted: none.
> **Quality reviewed:** 2026-04-12 -- five Codex rounds. Design review flagged Runtime priority ordering, stale `mem_upper_kb`, and cap truncation blindness; all fixed before first build. Impl round 1 flagged a 2*n+16 iterative resolver guard that could exhaust (1291 passes for n=512); replaced with the sweep-line algorithm. Impl round 2 flagged unstable tiebreak under swap-remove reordering; fixed with descriptor-index tiebreak. Review round 1 flagged arbitrary-large-RAM eviction; fixed with overlap-preference eviction. Review round 2 flagged that overlap-preference still discarded non-overlapping fragments of a large victim; fixed with min-loss ranking (`loss = victim.length - overlap_length`). Review round 3 cleared. Quality pass clean: no dead code, priority/eviction/coalescing consistent with `efi.h`, O(n^2) bounded at n=512. Parity: Linux `efi_memmap_insert()` and `e820_update_range()` use similar carve-by-priority logic; Windows HAL applies EFI memory-type precedence internally. Accepted: none.

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

> **Verified:** 2026-04-12 -- all 10 items confirmed. `fb_pack_rgb` at [bootx64.c:1254](../../src/boot/uefi/bootx64.c#L1254). `bsod_font` at :1277. `bsod_sad_face` at :1468. `bsod_render_graphical` at :1568. `bsod_can_render_graphical` at :1720. `gFbPixelFormat` set at :2098. Integration at :1862. Accepted: none.
> **Quality reviewed:** 2026-04-12 -- design review flagged ConOut-after-EBS safety, pixel-format-aware color packing, and risk matrix gaps (all fixed before first build). Impl round 1: URL caption unreachable (inlined QR with caption_reserve). Round 2: URL clipped at 1280x720 right edge (three-branch horizontal placement). Round 3: cleared. Post-impl round 1: pre-EBS dwell used counter not wall-clock (added `boot_fatal_dwell()` with `gBS->Stall`); post-EBS had no dwell (added `RuntimeServices->GetTime` loop with TSC fallback). Post-impl round 2: stale ConIn buffer bypassed dwell (added `ConIn->Reset` before polling); post-EBS TSC dwell non-uniform (replaced with GetTime primary, TSC fallback). Quality pass: all helpers referenced, no dead code, pixel format consistent, one-time boot performance. Accepted: none.

---

## 19. PT_LOAD Destination Policy

§1 bounds-checks PT_LOAD segments (file-side: `p_offset + p_filesz <= file_size`, address wraparound, boot_info overlap, framebuffer overlap, 32 MiB total cap), but does NOT reject writes into UEFI tables, RuntimeServices/BootServices regions, the bootloader image itself, or other firmware-reserved memory. A signed-but-misbehaving kernel image (Secure Boot proves the bytes were not tampered with, NOT that the segments target safe addresses) can still drive `efi_memcpy(p_paddr, ...)` into firmware state, corrupting `gST` / `gRT` / loaded-image / firmware tables before any later check runs. TODO-01 §17 `.bootproto` mismatch is a **version-drift** signal, not a security boundary -- a crafted kernel that knows the bootloader's compile-time `{magic, version, struct_size, sha256}` tuple still passes the descriptor gate. This section adds defense-in-depth: an explicit "allowed destination" policy that rejects PT_LOAD copies into firmware-owned, bootloader-owned, or handoff-reserved regions before any `efi_memcpy` runs.

> [!NOTE]
> **Regression risk:** MEDIUM. False-positives reject otherwise-valid kernels; false-negatives leave the gap §17 already partially closed. Validate against the live UEFI memory map (`gBS->GetMemoryMap`) at kernel-load time, not a hard-coded address list.

- [ ] Snapshot the UEFI memory map immediately before `load_kernel()` returns (after the existing `gBS->GetMemoryMap` call site OR a fresh one); cache the entries that mark `EfiRuntimeServicesCode`, `EfiRuntimeServicesData`, `EfiBootServicesCode` (where the bootloader image lives), `EfiLoaderCode` (bootloader image fallback), `EfiACPIReclaimMemory`, `EfiACPIMemoryNVS`, and any `EfiReserved` ranges. Walk this list per PT_LOAD segment in `load_kernel()` and reject any segment whose `[p_paddr, p_paddr + p_memsz)` overlaps a forbidden region.
- [ ] Define `bool pt_load_destination_allowed(UINT64 dst_start, UINT64 dst_end, const EFI_MEMORY_DESCRIPTOR *map, UINTN entries, UINTN desc_size)` in `src/boot/uefi/bootx64.c` (or a new `src/boot/uefi/load_policy.c` if it grows past ~80 LOC). Returns false on any overlap with the forbidden classes above; returns true only when every byte of the destination range falls inside `EfiConventionalMemory` or `EfiLoaderData` (the kernel's intended landing pages).
- [ ] Wire the predicate into `load_kernel()` BEFORE the `efi_memcpy(dst, src, copy_size)` at the existing PT_LOAD copy site (currently `src/boot/uefi/bootx64.c:3744-3756`, search for "Copy segment to its physical address"). On reject: emit `[FAIL] Kernel ELF: PT_LOAD destination forbidden (paddr=0xH..H, type=<EFI_MEMORY_TYPE name>)\n` via `serial_early_print` AND return `EFI_LOAD_ERROR` so the existing §9 fatal screen renders. Do NOT attempt mitigation -- a kernel that wants to write firmware addresses is broken or hostile.
- [ ] Add a fault class to the §17 `boot_version_fault` schema (or a new sibling NVRAM record) so the operator-visible error names "PT_LOAD destination forbidden" rather than the generic boot-load failure. XREF the §17 producer/consumer to add the new fault class.
- [ ] Unit test in `src/kernel/test/test_boot_proto.c` (or a new `test_load_policy.c`): synthesize EFI_MEMORY_DESCRIPTOR fixtures covering each forbidden class and assert `pt_load_destination_allowed()` rejects them; assert it accepts a destination fully inside `EfiConventionalMemory`. Cannot test the live `gBS->GetMemoryMap` path under unit tests (lives outside boot infrastructure per `feedback_test_no_live_boot_calls`); the policy predicate is the testable surface.
- [ ] Smoke test extension in `scripts/test-smoke.sh`: build a synthetic kernel ELF with a PT_LOAD segment targeting `EfiRuntimeServicesData` (the SystemTable region) and confirm the bootloader rejects it via the new fail message before any copy occurs. Owner: §19 fixture; can re-use the `tools/test-bootproto/` fixture infrastructure.
- [ ] On §19 ship: cross-link from TODO-01 §17 Notes to record that the §17 .bootproto check is "version drift only", and that destination policy lives here.
- [ ] Commit: `"boot: PT_LOAD destination policy -- reject firmware/loader overlaps before copy"`

**Test checkpoint:** With a synthetic ELF whose PT_LOAD targets `EfiRuntimeServicesData`, bootloader emits `[FAIL] Kernel ELF: PT_LOAD destination forbidden ... type=RuntimeServicesData` on serial and renders the §9 error screen. Verified on QEMU WHPX + TCG; bare-metal validation is operator-driven (requires a hand-built malformed kernel).

> **Test runner:** N/A (not yet shipped) | validation: synthetic-ELF fixture + smoke pattern check

---

## OS Comparison

| ⭐ | Feature         | 🪟 Win11                         | 🐧 Linux                        | 🚀 Impossible OS                 |
| -- | --------------- | --------------------------------- | ------------------------------- | --------------------------------- |
| 💎 | ELF bounds      | ✅ PE header + SizeOfImage check | ✅ GRUB ELF phdr bounds         | ✅ §1 phdr+seg+overlap+32M cap   |
| 💎 | EBS retry       | ✅ bootmgr bounded retry loop    | ✅ efi-stub retry on map stale  | ✅ §2 N=4 bounded + map refresh  |
| 💎 | Kernel fallback | ✅ BCD alternate paths + WinRE   | ✅ GRUB rescue + fallback.cfg   | ✅ §3 3-path search + DeviceHdl  |
| 💎 | Serial detect   | ✅ ACPI SPCR + EMS headless      | ✅ earlycon=uart,io,0x3f8       | ✅ §4 COM1/COM2 probe+boot_info  |
| 💎 | GOP degrade     | ✅ Fallback to basic display     | ✅ efifb + simpledrm fallback   | ✅ §5 headless + SetMode fallbk  |
| 💎 | Mmap overflow   | ✅ Dynamic buffer reallocation   | ✅ Grow buf + retry loop        | ✅ §6 512 cap + truncate warn    |
| 💎 | boot.conf parse | ✅ BCD registry schema + edit    | ✅ grub.cfg + grub-mkconfig     | ✅ §7 key whitelist + range chk  |
| 💎 | Alloc fallback  | ✅ Graduated pool sizes          | ✅ Dynamic retry allocation     | ✅ §8 32/16/8 MiB + overlap chk  |
| 💎 | SPCR serial     | ✅ EMS Emergency Management      | ✅ earlycon SPCR auto-detect    | ✅ §10 RSDP->XSDT->SPCR parse    |
| 💎 | UEFI watchdog   | ✅ Re-arm via SetWatchdogTimer   | ✅ efi_stub disables watchdog   | ✅ §11 60s arm + disarm pre-EBS  |
| 💎 | Mmap validate   | ✅ Descriptor version + size     | ✅ efi_stub sanity checks       | ✅ §12 align+pages+type+overlap  |
| ⭐ | Error screen    | ❌ Generic BSOD (no boot ctx)    | ⚠️ GRUB text menu (no graphics) | ✅ §9 blue BSOD + key + reboot   |
| ⭐ | NVRAM errors    | ⚠️ Opaque status codes           | ❌ No persistent boot errors    | ✅ §13 13 codes + NVRAM persist  |
| ⭐ | Boot QR         | ❌ No UEFI-phase QR codes        | ❌ No GRUB QR support           | ✅ §14 QR V3 byte mode + scan    |
| ⭐ | Graphical error | ✅ :( BSOD (OS-level only)       | ❌ GRUB text menu only          | ✅ §18 pre-OS pixel BSOD + icon  |
| 💎 | Mmap normalize  | ✅ Hal.dll coalesces overlaps    | ✅ efi_fake_memmap + sanitize   | ✅ §17 sweep-line carve+min-loss |
| 💎 | Handoff ABI     | ✅ BCD signature + protocol      | ✅ Multiboot2 / Linux boot      | ✅ §15 magic+ver+size + halt     |
| 💎 | Offline repair  | ✅ Windows Recovery Environment  | ✅ rescue/live ISO image        | ⬜ TODO-22 recovery partition    |

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
