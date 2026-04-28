---
schema_version: 1
id: uefi-hardening-secureboot
domain: 01-boot-platform
status: active
title: "TODO-02 -- UEFI Bootloader Hardening & Secure Boot"
---

# TODO-02 -- UEFI Bootloader Hardening & Secure Boot

> **Goal:** Harden the UEFI boot path with runtime service preservation, variable access, GOP resolution auto-detection, SMBIOS hardware inventory, Secure Boot shim chain-loading, boot UX polish, and serial log standardization. This is the complete core boot experience -- the OS boots reliably on QEMU, VirtualBox, and bare metal with Secure Boot support.

> [!IMPORTANT]
> **Secure Boot strategy:** Use the rhboot/shim chain-loading approach. MOK key pair (`MOK.key`) is generated locally and MUST NEVER be committed to the repo. Long-term goal: submit shim to Microsoft shim-review to eliminate the MOK enrollment popup for end users.
>
> **Current state (code-truth 2026-04-10):** `uefi_runtime_init()` / `call_set_virtual_address_map()`, `uefi_var_*`, `gop_negotiate_mode()`, SMBIOS registry population, `uefi_secureboot_init()`, shim signing (`scripts/sign-efi.sh`), `secureboot_keys_init()` + `tpm_init()` wiring, `uefi_crypto_agility_init()`, `uefi_capsule_init()` (query-only), and `esrt_init()` exist in `src/kernel/uefi_runtime.c`, `src/kernel/tpm.c`, `src/kernel/main/boot_hw.c`, `src/kernel/main/boot_interrupts.c`, `src/boot/uefi/bootx64.c`. `src/kernel/test/test_uefi_boot.c` and `test_register_uefi_boot()` are **not** merged; **Verification** final Commit stays open until Unit Tests land.

> [!NOTE]
> Advanced UEFI features (multi-OS boot menu, capsule **delivery** / signed UpdateCapsule path, W^X enforcement on RT pages, multi-GPU GOP, extended Secure Boot policy modes, SMBIOS extended types, DBX freshness automation) are owned by [TODO-27-uefi-advanced.md](TODO-27-uefi-advanced.md). TPM measured boot/PCR replay is owned by [TODO-13](TODO-13-tpm-measured-boot-attestation.md), and early entropy handoff is owned by [TODO-12](TODO-12-early-entropy-random-seed.md). §9 here is ops-parity backlog only.

---

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- [`include/kernel/uefi_runtime.h`](../../include/kernel/uefi_runtime.h)
- [`include/kernel/smbios.h`](../../include/kernel/smbios.h)
- [`include/kernel/boot_splash.h`](../../include/kernel/boot_splash.h)
- [`src/kernel/boot_timing.c`](../../src/kernel/boot_timing.c)
- [`src/kernel/uefi_runtime.c`](../../src/kernel/uefi_runtime.c)
- [`docs/guides/secure-boot-keys.md`](../../docs/guides/secure-boot-keys.md)
- -> XREF: `TODO-14-boot-diagnostics.md §3` -- `boot_progress()` named-stage API consumed by this file §8
- -> XREF: `TODO-11-interrupt-timer-arch.md §2` -- LAPIC timer calibration feeds boot timing used in §2
- -> XREF: `TODO-11-interrupt-timer-arch.md §3` -- boot time visualization JSON export referenced from this file §2
- -> XREF: `02-kernel-core/TODO-08-time-filetime-management.md §3` -- `UEFI GetTime` -> FILETIME seeding
- -> XREF: `02-kernel-core/TODO-14-registry-completion.md §5` -- `HKLM\HARDWARE\*` and `HKLM\SYSTEM\SecureBoot` storage via registry syscalls
- -> XREF: `TODO-27-uefi-advanced.md` -- deferred advanced features (multi-OS menu, capsule, W^X, multi-GPU, extended SB, SMBIOS ext, DBX)
- -> XREF: `TODO-13-tpm-measured-boot-attestation.md` -- measured boot, PCR replay, TPM NV baselines, sealed secrets, and attestation export
- -> XREF: `TODO-12-early-entropy-random-seed.md` -- firmware and CPU entropy collection plus boot-time seed handoff
- -> XREF: `10-platform-services/TODO-08-win32-api-surface.md §2` -- consumer for §14 `kernel32.dll` firmware variable + table exports
- -> XREF: `01-boot-platform/TODO-04-firmware-table-platform-inventory.md §1` -- ESP UUID surfaced by §13 feeds the firmware table catalog

---

## Outcome

- `ExitBootServices()` followed by `SetVirtualAddressMap()` -- UEFI runtime pointers valid in kernel.
- `uefi_var_get()` / `uefi_var_set()` work after boot.
- `boot_info.fb_width/fb_height` reflect negotiated GOP resolution; HiDPI flag set when width >= 2560.
- `HKLM\HARDWARE\BIOS\*`, `HKLM\HARDWARE\CPU\*`, `HKLM\HARDWARE\Memory\*` populated from SMBIOS.
- `boot_info.secure_boot_enabled` set correctly; kernel verifies state.
- `BOOTX64.EFI` signed with MOK key; `.gitignore` entry for `MOK.key`.
- Serial log unified format with atomic line writes.
- SBAT / revocation ops checklist documented; Secure Boot DB counts visible in-registry; `ExitBootServices()` bounded retry with visible status codes on picky firmware.
- Shim is signed by a current MS UEFI CA generation (2011 or 2023); CA generation visible in registry (`HKLM\SYSTEM\SecureBoot\ShimCA`); build-time WARN fires when only the deprecated 2011 CA is present past the 2026-04-01 safety window.
- Bootloader fails fast on corrupted / wrong-type ESP: GPT type GUID + FAT32 BPB + required-file batch are checked before kernel load; ESP UUID + size mirrored to `HKLM\HARDWARE\BOOT\ESP\*`.
- `kernel32.dll` exports `GetFirmwareEnvironmentVariableA/W`, `SetFirmwareEnvironmentVariableA/W`, `GetSystemFirmwareTable`, `EnumSystemFirmwareTables`; variable storage quota mirrored to `HKLM\SYSTEM\SecureBoot\Vars\*`.

---

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On      | Status |
| --- | :---: | ---------------------------------------- | --------------- | :----: |
| 💎  |   1   | UEFI runtime services preservation       | --              |  [x]   |
| 💎  |   2   | UEFI variable services                   | §1              |  [x]   |
| 💎  |   3   | GOP resolution auto-detection            | §1              |  [x]   |
| 💎  |   4   | SMBIOS table parsing                     | §1              |  [x]   |
| 💎  |   5   | Secure Boot state detection              | §2              |  [/]   |
| 💎  |   6   | Secure Boot shim chain-loading           | §5              |  [x]   |
| 💎  |   7   | Boot UX polish                           | §3, §5, T14 §2  |  [x]   |
| ⭐  |   8   | Serial log standardization               | --              |  [x]   |
| 💎  |   9   | SBAT ops, DB registry mirror, EBS retry  | §2, §5          |  [x]   |
| 💎  |  10   | RT sleepable lock migration              | §1              |  [x]   |
| 💎  |  11   | Unified signed boot artifact (UKI-style) | §6              |  [ ]   |
| 💎  |  12   | MS UEFI CA 2023 transition + 2011 retirement | §6          |  [ ]   |
| 💎  |  13   | EFI System Partition integrity check     | --              |  [ ]   |
| 💎  |  14   | Win32 firmware variable + table surface  | §2              |  [ ]   |

---

## 1. UEFI Runtime Services Preservation

Before `ExitBootServices()`, save UEFI runtime function pointers into `boot_info` so the kernel can call them after the boot services are gone.

- [x] `uefi_runtime_init()` -> `call_set_virtual_address_map()` in kernel, after `ExitBootServices()`
- [x] `boot_info.uefi_runtime` struct: GetVariable, SetVariable, GetTime, SetTime, ResetSystem, UpdateCapsule, QueryCapsuleCapabilities
- [x] Bootloader copies `gRT->*` pointers before `ExitBootServices()`
- [x] Kernel validates each pointer non-NULL, logs OK or UNAVAILABLE
- [x] `BOOT_DEGRADED` path: `s_available = 0`; callers guard with `uefi_rt_available()`
- [x] Commit: `"boot: preserve UEFI runtime service pointers across ExitBootServices"`

**Test checkpoint:** Serial shows `[UEFI] SetVirtualAddressMap OK` and per-service OK or UNAVAILABLE lines; kernel does not fault when calling preserved RT entry points after EBS. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) -- `test_uefi_rt_available` (test_uefi_boot.c:22) | 1 §1 suite, 0 failures

> **Notes:**
> - What shipped: `src/kernel/uefi_runtime.c` (~440 lines) -- `uefi_runtime_init()` walks the bootloader-preserved RT table, validates signature + CRC32, calls `SetVirtualAddressMap`, validates the 5 critical service pointers, and exposes `uefi_rt_available()` as the canonical check.
> - How it integrates: invoked from `boot_hw.c:262` via `BOOT_STEP`; sets `kernel_subsystem_set_ready(uefi_runtime, BOOT_OK || BOOT_DEGRADED)`; the `s_rt_mutex` (mutex_t) serializes all post-init RT calls (sleepable per §10 migration).
> - Once-latch + caps invariants: `s_init_done` blocks duplicate-init re-entry; `s_svam_done` mirrors into `boot_info.uefi_runtime.svam_called`; `rt_advertise_unavailable()` enforces the present/degraded XOR contract by clearing `caps_present` AND setting `caps_degraded` for `BOOT_CAP_RUNTIME_SERVICES` in lockstep with `s_available=0` and the legacy byte.
> - Codex 4x review pipeline this pass: implementation-time adversarial (3 findings: H1 once-latch, H2 SVAM fn-pointer NULL deref, M1 caps not cleared on degrade) + consistency (M1 caps_degraded XOR invariant) + perf (H1 BOOT_RT_MMAP_MAX local bound) + re-adversarial on fix delta (verdict: approve, no findings). All 5 fixed; zero deferred / accepted-XREF.
> - Canonical doc: [`src/kernel/uefi_runtime.c`](../../src/kernel/uefi_runtime.c) is authoritative for SVAM + RT-service invariants; [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) §510-518 / §1183-1187 carries the caps_present/caps_degraded XOR contract.
> - Scope boundary: §1 owns SVAM + RT-pointer validation + once-init contract; §10 owns the sleepable mutex around post-init RT calls; §2 owns variable-service wrappers; per-CPU MSR re-program after CR3 reload is a CLAUDE.md "Bare Metal Gotchas" concern, not a §1 implementation gap.

> **Verified:** 2026-04-27 | commit `cbda05ac` | 5/5 items | build OK | tests 1 §1 suite, 0 failures (test_uefi_rt_available)
> **Quality reviewed:** 2026-04-27 | Codex 4x (adversarial + consistency + perf + re-adversarial) | 3H+2M fixed | scope: kernel-code-quality (gates walked: SMP-safe by default, memory rules, bare-metal correctness, complete error paths)

---

## 2. UEFI Variable Services

Thin wrappers around `gRT->GetVariable` / `SetVariable` with error translation.

- [x] `uefi_var_get()` / `uefi_var_set()` with NTSTATUS return
- [x] Common GUIDs: EFI_GLOBAL, EFI_IMAGE_SECURITY_DATABASE, IMPOSSIBLE_OS_VENDOR
- [x] `uefi_var_get_u32()` / `uefi_var_set_u32()` convenience wrappers
- [x] `uefi_var_enumerate(callback)` for iterating all variables
- [x] Win32 API wired: `NtQuerySystemEnvironmentValue[Ex]` (0x00D2-0x00D4) and `NtSetSystemEnvironmentValue[Ex]` (0x00D3-0x00D5) registered in SSDT, mapped to `uefi_get_variable` / `uefi_set_variable`
- [x] Commit: `"kernel: UEFI variable get/set wrappers"`
- [x] Optimization: kernel `uefi_vars_init()` now uses `g_boot_info.uefi_boot_*` (populated pre-EBS by TODO-05 §6) instead of re-reading BootOrder/BootCurrent via runtime services -- removes 2 GetVariable calls, works even if runtime services are degraded

**Test checkpoint:** `uefi_var_get(L"SecureBoot", ...)` returns success or not-found without crash; test GUID roundtrip via `uefi_var_set_u32` / `uefi_var_get_u32` survives reboot when NVRAM allows. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) -- `test_uefi_var_get_secureboot` + `test_uefi_var_u32_roundtrip` (test_uefi_boot.c:31,45) | 2 §2 suites, 0 failures
>
> **Notes:**
> - Legacy SSDT 0x00D2 / 0x00D3 (`NtQuerySystemEnvironmentValue` / `NtSetSystemEnvironmentValue`) now ship with the canonical native NT ABI: `PUNICODE_STRING` name (Length / MaximumLength / Buffer triplet), implicit `EFI_GLOBAL_VARIABLE_GUID` namespace, legacy attrs = NV|BS|RT for the setter, `USHORT*` `ReturnLength` out for the getter. Earlier wrappers that re-routed to the Ex variant via stack-args 5/6 were silently dropping those args because the SSDT dispatcher only forwards 4 (TODO-12 §4 owns extending the entry).
> - All four handlers now marshal user pointers into kernel-bounded buffers BEFORE entering `s_rt_mutex`: GUID copy + length copy + name (`marshal_user_pwstr_name` for Ex, `marshal_user_unicode_string` for legacy) + value buffer probe + kmalloc/pmm copy. Mutex hold time is now a function of firmware latency + a bounded copy, not user memory latency -- closes the cross-process stall vector.
> - Value buffers above 4 KiB route through `pmm_alloc_contiguous` per CLAUDE.md kmalloc cap; `uefi_value_alloc` / `uefi_value_free` pick the right allocator and frame-by-frame free. Caps: name 1024 B, value 64 KiB.
> - `count_sig_entries` (Secure Boot DB inventory) tightened against firmware-supplied size lies: header_total computed in uint64 to defeat wraparound; signature_size floor at `sizeof(efi_signature_data)`; data_size must be exact multiple of signature_size or list rejected.
> - Query handlers defend against firmware returning SUCCESS while inflating DataSize: if `actual_len > requested_len` after `uefi_get_variable`, downgrade to `UEFI_BUFFER_TOO_SMALL` and skip the copy-back -- protects both the kernel kvalue buffer and the user-probed range from overrun by buggy/hostile firmware.
> - Canonical doc: legacy native NT ABI spec at TODO-12 §23 (line 707). Master SSDT row at TODO-A-SSDT-Master-Table §0x00D0-0x00DF.
>
> **Verified:** 2026-04-27 | commit `7ab63775` | 7/7 items | build OK | tests 2 §2 suites, 0 failures (test_uefi_var_get_secureboot, test_uefi_var_u32_roundtrip)
> **Accepted:** [H] SSDT dispatcher forwards only 4 args; legacy handlers must not depend on stack-args 5-6 -> XREF: 02-kernel-core/TODO-12 §4 (item: "Extend INT 0x2E + SYSCALL entry to read stack arguments 5-6+" at line 212)
> **Accepted:** [H] PMM bitmap unsynchronized; `pmm_alloc_contiguous` / `pmm_free_frame` race on SMP when uefi_value_alloc routes >4 KiB buffers there (reason: kernel-wide PMM concern, not §2-specific) -> XREF: 03-memory-concurrency/TODO-03 §1 (item: "PMM bitmap SMP locking" at line 100)
> **Quality reviewed:** 2026-04-27 | Codex 5x (adversarial + consistency + perf + re-adversarial + final-adversarial) | 5H+3M fixed, 2H accepted-XREF | scope: kernel-code-quality (gates walked: SMP-safe by default, memory rules, bare-metal correctness, complete error paths)

---

## 3. GOP Resolution Auto-Detection

Negotiate the best framebuffer resolution before `ExitBootServices()`.

- [x] `gop_negotiate_mode()`: query all modes, score by resolution, respect `boot.conf` override
- [x] HiDPI flag: `boot_info.hidpi = 1` when width >= 2560
- [x] `gop->SetMode(best_mode)` with fallback to current mode
- [x] Commit: `"boot: GOP resolution auto-detection with HiDPI flag and boot.conf override"`

**Test checkpoint:** Serial shows `[Boot] GOP: {W}x{H} 32bpp (mode N)` with W/H > 0; `boot_info.hidpi` is 1 when width >= 2560 else 0. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot, TEST_CAT_BOOT) | smoke PASS (KVM 2.3s; GOP enumerated 30 modes, mode 0 selected at 1280x800 BGRX)

> **Notes:**
> - `gop_negotiate_mode()` and `init_gop()` in `src/boot/uefi/bootx64.c:1664-1900` score every 32bpp mode by pixel count under boot.conf overrides or auto-1080p cap; `boot_info.hidpi` flips at width >= 2560; HiDPI consumers (boot splash, desktop scaling) read the flag.
> - Hostile-firmware hardening (Codex 4x review 2026-04-27): both QueryMode loops capped at `BOOT_GOP_MODE_MAX` + 100-error abort; every successful info buffer FreePool'd per UEFI 12.9.2.4; required_bytes checked for UINTN multiplication overflow before any clear; duplicate VRAM clear in init_gop removed (efi_main owns the visible clear); `gop_mode_selected` switched to ordinal-into-`gop_modes[]` with `gop_mode_count` sentinel for off-table.
> - Test wiring at `src/kernel/test/test_uefi_boot.c` (test_boot_info_hidpi + GOP fb width/height assertions, registered TEST_CAT_BOOT). Smoke test (`bash scripts/test-smoke.sh`) covers end-to-end boot reaching `C:\>` with serial verifying the GOP log line.
> - Canonical doc: this section + `docs/boot/boot-info-fields.md` "Framebuffer + GOP" subsection.
> - Scope boundary: §3 owns GOP mode negotiation + framebuffer publication into boot_info; multi-GPU and DisplayPort hot-plug are owned by `01-boot-platform/TODO-27-uefi-advanced.md`; HiDPI scaling rules are owned by the desktop compositor (TODO under `09-desktop-shell/`).

> **Verified:** 2026-04-27 | commit `909e5eef` | 4/4 items | build OK | smoke PASS (KVM 2.3s)
> **Quality reviewed:** 2026-04-27 | Codex 4x (adversarial + consistency + perf + re-adversarial) | 5H+1M fixed | scope: boot-code-quality (gates walked: UEFI types, error handling, framebuffer safety, boot_info ABI, EBS boundary, fallback chains, spec compliance)

---

## 4. SMBIOS Table Parsing

Walk SMBIOS 3.x structures and populate Registry hardware keys.

- [x] Scan `EFI_CONFIGURATION_TABLE` for SMBIOS3 GUID; fallback to 2.x
- [x] Type 0 (BIOS): vendor, version, release date -> `HKLM\HARDWARE\BIOS\*`
- [x] Type 1 (System): manufacturer, product, serial, UUID -> `HKLM\HARDWARE\System\*`
- [x] Type 4 (Processor): socket, family, speed, cores, threads -> `HKLM\HARDWARE\CPU\*`
- [x] Type 17 (Memory): size, speed, type, manufacturer -> `HKLM\HARDWARE\Memory\*`
- [x] `smbios_get_system_uuid()`
- [x] Commit: `"kernel: SMBIOS 3.x table parse -> Registry HARDWARE hives"`

> [!NOTE]
> System Properties UI for SMBIOS fields is **not** boot-loader code. Track in XREF `09-desktop-shell/TODO-11-control-panel.md` when that TODO is ready.

**Test checkpoint:** Registry keys under `HKLM\HARDWARE\BIOS\*`, `HKLM\HARDWARE\System\*`, `HKLM\HARDWARE\CPU\*`, and `HKLM\HARDWARE\Memory\*` are populated after boot; `smbios_get_system_uuid()` returns non-zero UUID on real hardware. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot, TEST_CAT_BOOT) | test_smbios_uuid registered; smoke covers boot reach to C:\>

> **Notes:**
> - `smbios.c` parses SMBIOS 3.x (preferred) and 2.x entry points found via UEFI config table; walks structures, extracts BIOS / System / Baseboard / Type 4 CPU / Type 17 DIMM data, populates `HKLM\HARDWARE\{BIOS,System,CPU\<n>,Memory\<n>}` via `smbios_populate_registry()` from `registry.c:1851`.
> - Hostile-firmware hardening (Codex 4x review 2026-04-27): new `smbios_copy_string()` bounds copies by both max-1 AND `s_table_end` (closes overread that escaped through `str_copy` at 18 call sites); entry-point length validated against explicit wire-format constants (`SMBIOS3_EP_LEN=0x18`, `SMBIOS2_EP_LEN_MIN=0x1E`, `SMBIOS2_EP_LEN_MAX=0x1F`) BEFORE checksum (closes the `ep->length=0` checksum-bypass and oversized-length overread); `walk_structures` uses `__builtin_add_overflow` to reject `table_addr + max_len` wrap and now returns `int`; callers gate `s_info.valid` on the walker's return so a rejected table cannot pose as parsed.
> - `smbios_get_system_uuid()` and `smbios_get_info()` consumed by `test_smbios_uuid` (`src/kernel/test/test_uefi_boot.c:94-101`, `TEST_CAT_BOOT`) and registry hardware-key population. Boot integration validates end-to-end via the existing smoke test reaching `C:\>`.
> - Canonical doc: this section + DMTF DSP0134 3.7.0 (referenced in code header comments at `smbios.c:30-40`).
> - Scope boundary: §4 owns table parse + Registry HARDWARE hive population; the Control Panel UI surfaces these keys to the user (owned by `09-desktop-shell/TODO-11-control-panel.md` per the section's existing NOTE).

> **Verified:** 2026-04-27 | commit `fce434c1` | 7/7 items | build OK | lint CLEAN | tests test_smbios_uuid wired
> **Quality reviewed:** 2026-04-27 | Codex 5x (design + adversarial + consistency + perf + re-adversarial) | 2H+2M fixed | scope: kernel-code-quality (gates walked: SMP-safe by default, memory rules, bare-metal correctness, complete error paths, no TODO/FIXME, Win32 surface)

---

## 5. Secure Boot State Detection

Read the UEFI `SecureBoot` variable and expose the state to the kernel.

- [x] `uefi_secureboot_init()`: read `SecureBoot` variable, set `boot_info.secure_boot_enabled`
- [x] Write `HKLM\SYSTEM\SecureBoot\State` = 0 or 1
- [/] Padlock icon in system tray when Secure Boot active -- `g_system_state.secure_boot` flag published; rendering not implemented. -> XREF: `08-graphics-ui/TODO-11-startmenu-tray-notifications.md §4` (System tray icons -- `tray_icon` struct + register/unregister) is the owner; padlock-when-secure-boot-on is a §4 consumer of `g_system_state.secure_boot`.
- [x] Commit: `"kernel: Secure Boot state detection, registry key"`

> [!NOTE]
> Kernel **code signature verification** for loaded images is owned by `02-kernel-core/TODO-10-kernel-security-hardening.md §11` (Enclave and code signing) and cross-notes in `02-kernel-core/TODO-15-security-reference-monitor.md`, not this bootloader TODO.

**Test checkpoint:** `boot_info.secure_boot_enabled` matches UEFI `SecureBoot` variable when read succeeded (`StateValid=1`); registry `HKLM\SYSTEM\SecureBoot\StateValid` is `1` if the SecureBoot variable read succeeded, `0` if it failed. `State` key is present (0 or 1) ONLY when `StateValid=1`; absent on read-failure path so legacy boolean readers cannot mistake unknown for disabled. Padlock tray icon matches state when shell is running. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot, TEST_CAT_BOOT) | test_secureboot_state_matches_var + test_secureboot_db_inventory_matches_registry; smoke covers boot reach to C:\>

> **Notes:**
> - `uefi_secureboot_init()` in `src/kernel/uefi_runtime.c:825-944` reads SecureBoot/SetupMode/DeployedMode/AuditMode (UEFI 2.5+) global vars + PK/KEK existence; publishes to `g_boot_info.secure_boot_enabled` + `g_system_state.secure_boot` + `BOOT_CAP_SECURE_BOOT_STATE` cap bit ONLY when the variable read succeeded. Failure path: state stays zero-init, cap stays degraded, klog says UNKNOWN.
> - Hostile-firmware hardening (Codex 4x review 2026-04-28): `read_global_byte` now requires `status==SUCCESS && sz==sizeof(val)` (rejects firmware returning SUCCESS with sz=0/sz>1); `global_var_exists` requires `sz>0` (rejects empty PK/KEK as "enrolled"); `uefi_secureboot_populate_registry` writes `StateValid` always but `State` ONLY when valid (closes the unknown-vs-disabled ambiguity in the registry; 0xFF sentinel attempted but re-adversarial caught it polluted the bool contract for legacy readers).
> - Padlock tray icon: `g_system_state.secure_boot` flag published; rendering owned by `08-graphics-ui/TODO-11-startmenu-tray-notifications.md §4` (System tray icons -- `tray_icon` struct + register/unregister). The flag is the consumer-side signal; no further plumbing needed by this section.
> - Canonical doc: this section + UEFI 2.10 §8.2.1 (Secure Boot variables) + UEFI 2.10 §3.3 (PK/KEK semantics).
> - Scope boundary: §5 owns SecureBoot state read + registry publication; kernel **code signature verification** for loaded images is owned by `02-kernel-core/TODO-10-kernel-security-hardening.md §11`; SecureBoot DB/dbx inventory is owned by §9 of this TODO.

> **Verified:** 2026-04-28 | commit `35e9e17e` | 4/4 items (3 [x] + 1 [/]) | build OK | tests test_secureboot_state_matches_var + test_secureboot_db_inventory_matches_registry wired
> **Quality reviewed:** 2026-04-28 | Codex 4x (adversarial + consistency + perf + re-adversarial) | 1H+2M fixed (+1 re-adversarial regression) | scope: kernel-code-quality (gates walked: SMP-safe by default, memory rules, bare-metal correctness, complete error paths, no TODO/FIXME, Win32 surface)

---

## 6. Secure Boot Shim Chain-Loading

Set up MOK key pair, sign `BOOTX64.EFI`, and integrate shim into the build.

- [x] `.gitignore`: `MOK.key`, `*.signed.EFI`
- [x] Key generation documented in `docs/guides/secure-boot-keys.md`
- [x] `scripts/sign-efi.sh` + `sign-efi` Makefile target
- [x] Shim: `shim/shimx64.efi` + `shim/mmx64.efi` committed (SHA256 verified)
- [x] Shipped shim: Ubuntu `shim-signed` 1.58 (Microsoft-signed; trusted by firmware via MS UEFI CA out of the box). Shim validates `grubx64.efi` via the MOK list, populated by MokManager on first boot from `\\MOK.cer` at the ESP root (DER-encoded). The earlier self-built/embedded-MOK.cer flow was retired when we switched to the MS-signed binary.
- [x] Commit: `"boot: Secure Boot shim chain-loading, MOK key signing pipeline"`

**Test checkpoint:** With `MOK.key` present, signed `BOOTX64.EFI` builds; without keys, signing is skipped silently; first boot can complete MOK enrollment path on real firmware. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Notes:**
> - What shipped: signed `BOOTX64.EFI` (sbsign + atomic temp+verify+mv via `scripts/sign-efi.sh`); committed Microsoft-signed Ubuntu shim 1.58 (`shim/shimx64.efi` + `mmx64.efi` + `SHA256SUMS`); MOK enrollment via MokManager from ESP-root `\MOK.cer`.
> - How it integrates: `make sign-efi` calls `scripts/sign-efi.sh` (env-override-friendly: `MOK_KEY`, `MOK_CRT`, `EFI_BIN`); `make disk` writes DER-encoded `\MOK.cer` to ESP root; firmware -> shim (MS-signed) -> grubx64.efi (MOK-signed) -> kernel.
> - Downstream: first-boot MokManager flow per `docs/guides/secure-boot-keys.md`; SBAT bumps + shim refreshes track Ubuntu shim-signed releases (NOT rebuilt locally).
> - Doc pointer: [`docs/guides/secure-boot-keys.md`](../../docs/guides/secure-boot-keys.md).
> - Scope boundary: §6 owns the chain-loading wiring + signing pipeline + key-management docs. SBAT operations + Secure Boot DB count exposure are §7/§8 in this TODO.

> **Verified:** 2026-04-28 | commit `e3e138f2` (impl) + `6839936c` (atomic-sign fix) + `87f068b1` (MS-shim swap) + `47db7913` (post-impl re-review) | 6/6 items | build OK
> **Quality reviewed:** 2026-04-28 | Codex 3x (adversarial, consistency, perf) | 2H+1M fixed | scope: N/A (host-side build/sign pipeline + docs)

---

## 7. Boot UX Polish

Structured boot profiling and pre-framebuffer error recovery screen.

- [x] Boot profiling: `boot_progress()` at every major event; `boot_timing_write_report()` writes to `X:\Perf\boot-profile.log`
- [x] JSON boot-timeline export via `boot_timeline_dump_json()` -> XREF: [`01-boot-platform/TODO-11 §9`](TODO-11-interrupt-timer-arch.md) (item: "`boot_timeline_dump_json()`: writes `X:\Boot\boot-timeline.json`" at line 215). Implementation lives at `src/kernel/main/boot_progress.c:203` (header decl `include/kernel/boot_progress.h:64`); writes JSON array of `{stage, phase, post, start_ms, duration_ms}` per step. The earlier `TODO-11-interrupt-timer-arch.md §3` XREF was stale (kernel-domain TODO-11 was renamed `warm-kernel-update-runtime`; the right anchor is the `01-boot-platform` TODO-11 §9 above).
- [x] Pre-framebuffer error screen: `boot_halt(reason)` with inline 8x8 bitmap font
- [x] `boot_splash_status()` integration: live stage text below spinner
- [x] Commit: `"boot: boot-stage instrumentation, pre-framebuffer error recovery screen"`

**Test checkpoint:** `boot_progress()` stages appear in order on serial and optional `X:\Perf\boot-profile.log`; `boot_halt()` shows pre-framebuffer error text when forced; JSON timeline export matches `TODO-11-interrupt-timer-arch.md §3` contract. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Notes:**
> - What shipped: `boot_progress(phase, name, post16)` instrumented at every major boot event (8+ call sites in `src/kernel/main/boot_hw.c`); `boot_timing_write_report()` writes plain-text profile to `X:\Perf\boot-profile.log` (BlackBox) or `C:\Impossible\System\Logs\boot-profile.log` (fallback); `boot_timeline_dump_json()` writes the structured JSON timeline at `src/kernel/main/boot_progress.c:203`.
> - How it integrates: `boot_desktop.c:275` invokes both writers at desktop-ready (after VFS is up); `boot_halt(reason)` renders an inline 8x8-bitmap-font error screen via the framebuffer (with pitch validation per Codex H1 fix) and falls back to serial-only when fb is unavailable or pitch is malformed; `boot_splash_status(msg)` updates live stage text under the spinner.
> - Downstream: BlackBox partition at TODO-17 is the canonical landing surface for both files; JSON contract owned by `01-boot-platform/TODO-11 §9` (`boot_timeline_dump_json()` row at line 215).
> - Doc pointer: header docs at `include/kernel/boot_timing.h` and `include/kernel/boot_progress.h` carry the path-resolution rules; `01-boot-platform/TODO-11 §9` carries the JSON schema.
> - Scope boundary: §7 owns instrumentation + pre-framebuffer error screen + JSON-timeline plumbing. Timeline viewer / diff tooling lives in `01-boot-platform/TODO-14-boot-diagnostics.md`; the JSON schema itself is owned by TODO-11.

> **Verified:** 2026-04-28 | commit `ede0e2d3` (impl + post-impl Codex 3 fixes 2026-04-11) + `807b491f` (re-review fixes: H1 fb pitch + 2M XREF/path doc) + `99d5eb71` (re-adversarial H2 width*4 overflow fix) | 5/5 items | build OK
> **Quality reviewed:** 2026-04-28 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2H+2M fixed | scope: boot-code-quality (UEFI handoff + framebuffer safety)

---

## 8. Serial Log Standardization and Race Fix

Unified log format and atomic serial writes.

- [x] **Unified format:** `klog.h` defines five levels: `LOG_DEBUG`->`[INFO]`, `LOG_INFO`->`[ OK ]`, `LOG_WARN`->`[WARN]`, `LOG_ERROR`->`[FAIL]`, `LOG_FATAL`->`[CRIT]`; all callers migrated from `printk()` to `klog()`; deprecated `log.h` + `log.c` deleted
- [x] **Line race fix:** `serial_putchar_raw()` static helper (no lock); `serial_write()` holds spinlock for entire string; `klog()` builds full line in `char line[256]`, calls `serial_write()` atomically
- [x] Commit: `"kernel: Serial Output Phase 1 -- unified klog format"`
- [x] Commit: `"kernel: fix serial line mangling by making klog write atomically"`

**Test checkpoint:** Under concurrent IRQ logging, serial shows no interleaved partial lines; tags `[ OK ]`, `[WARN]`, `[FAIL]` appear as documented. Stress: rapid `klog()` from timer tick + main thread. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Verified:** 2026-04-11 -- Codex found truncation without marker (fixed: `~` appended on overflow). printk bypass and ring flush race accepted by design (emergency path + lockless producer). Accepted: printk emergency bypass (intentional), ring flush lockless design (same trade-off as Linux printk).
> **Quality reviewed:** 2026-04-11 -- no findings. Matches Windows DbgPrint (spinlock-serialized per-line) and Linux printk (lockless ring + console_lock). ANSI color output exceeds both. Accepted: none.

---

## 9. SBAT Ops, Secure Boot DB Registry Mirror, and ExitBootServices Retry

Win11 and major Linux distros surface firmware trust inventory (db/dbx counts), track SBAT-driven shim revocations, and retry `ExitBootServices()` when the memory map changes. Kernel already parses db/dbx via `secureboot_keys_init()` and logs counts (`src/kernel/uefi_runtime.c`); bootloader already performs **one** remap+retry around `gBS->ExitBootServices` (`src/boot/uefi/bootx64.c`). This section closes the remaining **ops parity** gaps without taking ownership of `UpdateCapsule` write paths (-> XREF: `TODO-27-uefi-advanced.md §2`).

> [!WARNING]
> **Regression risk:** Bootloader `ExitBootServices` retry must stay strictly bounded (no spin on broken firmware). Registry DWORD writes must run after `SYSTEM\SecureBoot` exists and must not clobber `State`.

- [x] Extend `docs/guides/secure-boot-keys.md` with SBAT bump / shim refresh checklist: 5-step process covering advisory monitoring (rhboot/shim + Microsoft + UEFI Forum), rebuild/resign workflow, `.sbat` CSV generation bump, key hygiene, and post-refresh testing.
- [x] Bootloader `src/boot/uefi/bootx64.c`: bounded `ExitBootServices()` retry loop (EBS_MAX_ATTEMPTS=4, locked per TODO-03 S2 agreement). Each retry logs to serial, re-fetches memory map via `get_memory_map()`, refills memory+runtime maps, then retries EBS. Fatal halt after all attempts exhausted.
- [x] After `secureboot_keys_init()`, mirror `secureboot_get_db_info()` fields under `HKLM\SYSTEM\SecureBoot\` as DWORD values (`DbEntries`, `DbxEntries`, `DbSha256`, `DbxSha256`, `DbX509`) via `RegSetDword()` in `uefi_secureboot_populate_registry()`. Writes after `State` in the same `RegCreateKeyEx` scope -- does not clobber `State`.
- [x] -> XREF: `02-kernel-core/TODO-14-registry-completion.md §5` -- confirmed: `uefi_secureboot_populate_registry()` is called from `registry_populate_defaults()` after `SYSTEM\SecureBoot` key exists. DB writes use the same open key handle as `State` -- no ordering regression.
- [x] -> XREF: `TODO-27-uefi-advanced.md §2` -- capsule install + ESRT-driven UX remains advanced scope (query-only init already in `uefi_capsule_init()` / `esrt_init()`). Acknowledged, no action needed.
- [x] Commit: `"boot: SBAT ops doc, Secure Boot DB counts in registry, EBS retry"` (3c552022)

**Test checkpoint:** Serial shows `[BOOT] ExitBootServices OK` after deliberate map churn (QEMU OVMF multi-disk attach); when db/dbx exist, `klog` / registry reflects non-zero counts consistent with `secureboot_get_db_info()`; SBAT section present in `docs/guides/secure-boot-keys.md`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Verified:** 2026-04-11 -- all items verified: SBAT docs (5 refs in secure-boot-keys.md), EBS retry (EBS_MAX_ATTEMPTS=4), db/dbx registry mirror (uefi_secureboot_populate_registry writes 5 DWORD values after State), XREF to 02-kernel-core/TODO-14 confirmed, XREF to 01-boot-platform/TODO-18 acknowledged. Accepted: none.
> **Quality reviewed:** 2026-04-11 -- no additional findings. Registry writes already enhanced in §5 quality review (SetupMode, DeployedMode, AuditMode added). EBS retry bounded and logged. SBAT documentation complete. Accepted: none.

---

## 10. UEFI Runtime Services Sleepable Lock Migration

UEFI firmware SetVariable can take 10-100ms+ for flash erase/write cycles. The current `s_rt_lock` spinlock holds with interrupts disabled across the entire firmware call, stalling all contending CPUs. Windows uses `FAST_MUTEX` (sleepable), Linux uses a semaphore (`efi_runtime_lock`). Migrate to a sleepable lock for thread-context RT callers, with a trylock/emergency path for panic writes.

> [!NOTE]
> **Prerequisite (now satisfied):** required a kernel mutex primitive (sleepable lock with ownership tracking + `mutex_lock()` / `mutex_trylock()` / `mutex_unlock()`). Already shipped in [`src/kernel/sched/mutex.c`](../../src/kernel/sched/mutex.c) with full waitqueue-based sleep, deadlock detection, and lock ordering. -> XREF: `03-memory-concurrency/TODO-08-advanced-sync.md` for further synchronization primitive work.

- [x] Implement kernel mutex primitive: `mutex_init()`, `mutex_lock()`, `mutex_trylock()`, `mutex_unlock()` with ownership tracking and scheduler integration -- already existed in `src/kernel/sched/mutex.c` with full waitqueue-based sleep, deadlock detection, and lock ordering
- [x] Replace `s_rt_lock` (spinlock) with a mutex in `uefi_runtime.c` for all thread-context RT calls (GetVariable, SetVariable, GetTime, SetTime, GetNextVariableName, GetWakeupTime, QueryVariableInfo) -- `s_rt_mutex = MUTEX_INIT("uefi_rt")`, `rt_call_enter()` uses `mutex_lock()`
- [x] Add emergency trylock path for panic/reset: `uefi_reset()` uses `rt_call_enter_emergency()` with `mutex_trylock()` -- if contended, logs warning and calls ResetSystem without lock
- [x] Verify ResetSystem still works from NMI/panic context -- emergency path uses trylock (non-blocking), no sleep, LAPIC masking still applied
- [x] Log latency warnings: `rt_call_exit()` measures TSC elapsed time; if > 50ms, klog warns with service name and duration
- [x] Commit: `"kernel: migrate UEFI RT serialization from spinlock to mutex"`

**Test checkpoint:** Serial shows normal RT calls with no latency warnings on QEMU (fast emulated flash). On bare metal with real NVRAM, SetVariable calls should complete without stalling other CPUs. Verify `uefi_reset()` works from panic context (NMI handler test). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Verified:** 2026-04-11 -- Codex found critical mutex_lock CAS race (fixed: atomic_cmpxchg instead of atomic_set). Emergency ResetSystem trade-off documented (trylock failure -> unlocked call, reboot attempt > hang). All 5 items verified. Accepted: none.
> **Quality reviewed:** 2026-04-11 -- no findings. Matches Windows FAST_MUTEX and Linux efi_runtime_lock semaphore. Priority inheritance, emergency trylock, LAPIC masking all at parity. TSC latency monitoring exceeds both (neither Win11 nor Linux log slow RT calls inline). Accepted: none.

---

## 11. Unified Signed Boot Artifact (UKI-style)

systemd-boot ships a Unified Kernel Image (UKI) format: a single signed UEFI PE that bundles the EFI stub, kernel, optional initrd, kernel cmdline, and other resources. Tampering with any component invalidates the whole signature. UEFI firmware can invoke the UKI directly (useful in Confidential Computing) OR a boot loader can chain into it. Impossible OS currently signs `BOOTX64.EFI` and ships `kernel.exe` + `boot.conf` as separate artifacts; a Secure Boot signature on `BOOTX64.EFI` does NOT cover the kernel or the config. A UKI-style unified artifact closes that gap.

> [!NOTE]
> The handoff-ABI side (how `boot_info` carries the UKI-origin flag, how the kernel verifies it was invoked through the unified path vs legacy split) is owned by [`01-boot-platform/TODO-01 §8`](TODO-01-boot-protocol-abi-handoff.md#8-boot-protocol-documentation-and-schema-changelog) (Boot Protocol Documentation and Schema Changelog). This section owns the signing + packaging + build-pipeline side.

- [ ] Investigate packaging: `scripts/build.sh` produces `BOOTX64.UKI.efi` combining `BOOTX64.EFI` + `kernel.exe` + `boot.conf` + (optional) recovery image into one PE, following the UAPI Group UKI specification (PE `.linux` / `.initrd` / `.cmdline` / `.osrel` sections).
- [ ] Extend [`scripts/sign-efi.sh`](../../scripts/sign-efi.sh) to sign the UKI in the same pass as the current split artifacts; both paths remain installable (UKI for modern Secure Boot + direct-firmware-invoke; split for backwards compatibility with legacy boot loaders that expect a separate kernel file).
- [ ] Add a `boot.conf` flag + `boot_info.flags` bit `BOOT_FLAG_INVOKED_VIA_UKI` so the kernel can tell whether it was launched through the unified artifact (whole-chain signed) or the split path (per-file signed). Informs §9 `BootReason` enum and any future attestation report.
- [ ] Document the UKI layout in `docs/guides/secure-boot-keys.md` alongside the existing MOK flow; include the `objcopy --add-section` invocation and the per-section `measure-for-pcr` order so the measured-boot log (TODO-13) reconstructs the same PCR values regardless of which artifact was booted.
- [ ] Add a regression to [`scripts/debug/kernel/run-boot-tests.bat`](../../scripts/debug/kernel/run-boot-tests.bat) that verifies both paths boot cleanly on QEMU; the UKI path is what modern Secure Boot with `shim + systemd-boot` / direct-firmware-invoke scenarios will use.
- [ ] Commit: `"boot: Unified signed boot artifact (UKI) alongside split BOOTX64.EFI"`

**Test checkpoint:** `bash scripts/build.sh` emits both `BOOTX64.EFI` (existing split path) AND `BOOTX64.UKI.efi` (new unified artifact). Signing succeeds on both when `MOK.key` is present. Booting the UKI via QEMU direct-firmware-invoke (`-drive if=pflash,format=raw,file=OVMF.fd` + `-cdrom BOOTX64.UKI.efi.iso` OR by placing the UKI at `EFI/BOOT/BOOTX64.EFI`) reaches `C:\>` cleanly with `boot_info.flags & BOOT_FLAG_INVOKED_VIA_UKI`. Split-path boot on the same firmware still works unchanged. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal (UKI is the modern-firmware path; bare metal via UEFI 2.7+ with Secure Boot should execute the UKI directly).

---

## 12. MS UEFI CA 2023 Transition + 2011 Retirement Tracking

Microsoft began rotating UEFI signing certificates in 2024-2025: the original `Microsoft Corporation UEFI CA 2011` (the cert that signs every shim Microsoft has shipped) is scheduled to expire June 2026, and a replacement `Microsoft Corporation UEFI CA 2023` is being enrolled into firmware DBs via Windows Update. Devices booting a shim still signed only by 2011 CA will start failing on machines whose firmware's KEK/db has rotated to 2023-only. Any Impossible OS install that ships through 2026+ MUST track this transition and re-sign shim against the 2023 CA before the 2011 cert expires. This section is doctrine + ops; the actual signing happens in §6's pipeline.

> [!IMPORTANT]
> **2026 ship-blocker if ignored.** Without 2023-CA-signed shim, machines that received the firmware DB rotation through Windows Update will refuse to boot Impossible OS. The transition window is firm.

- [ ] Add a "MS UEFI CA Lifecycle" section to [`docs/guides/secure-boot-keys.md`](../../docs/guides/secure-boot-keys.md) covering: 2011 CA expiry date (June 2026), 2023 CA enrollment timeline, how to verify which CA your shim is signed against (`sbverify --list shim/shimx64.efi`), how to re-sign + redistribute when MS publishes the 2023-signed shim binary.
- [ ] Update [`scripts/sign-efi.sh`](../../scripts/sign-efi.sh) to log which MS UEFI CA the bundled `shim/shimx64.efi` is signed by (parse `sbverify --list` output, emit `[shim] signed-by: Microsoft Corporation UEFI CA <year>`). Fail loudly if signed-by year is in the deprecated set.
- [ ] Pin the canonical 2023-CA-signed shim binary into `shim/` once Microsoft publishes it; bump `shim/SHA256SUMS` accordingly. Until then, document the 2011-CA-signed binary's expiry exposure in the docs section above.
- [ ] Add a build-time check (`scripts/build.sh` or `scripts/test-tooling.sh` sub-test): if the shim binary is signed only by the deprecated CA AND the build host's date is past 2026-04-01, emit a WARN. The 60-day pre-expiry window is the safety margin per the MS guidance.
- [ ] Surface the shim CA generation in `HKLM\SYSTEM\SecureBoot\ShimCA` (DWORD: 2011 or 2023) at boot via [`uefi_secureboot_populate_registry()`](../../src/kernel/uefi_runtime.c). Consumers (msinfo32-equivalent, audit tools) can read it.
- [ ] Commit: `"boot: track MS UEFI CA 2023 transition; sign-efi.sh emits CA generation; registry surface"`

**Test checkpoint:** `sbverify --list shim/shimx64.efi` output names a Microsoft Corporation UEFI CA generation; `HKLM\SYSTEM\SecureBoot\ShimCA` matches; `bash scripts/sign-efi.sh build/BOOTX64.EFI` logs `[shim] signed-by: ...`; the build-time warning fires when the deprecated CA date threshold is crossed. Test on: QEMU WHPX (signature path), QEMU TCG, VirtualBox, bare metal (real DB rotation).

---

## 13. EFI System Partition Integrity Check

The bootloader currently trusts that UEFI launched it from a valid ESP and proceeds to load `kernel.exe` + `boot.conf` without re-verifying. On a corrupted or tampered ESP, the bootloader silently loads whatever bytes it finds. Win11 BootMgr does basic ESP sanity (FAT32 + correct partition GUID); Linux's `efibootmgr` exposes the ESP UUID. Impossible OS should add a small pre-load sanity gate that catches obvious corruption / wrong-partition cases before kernel launch.

- [ ] [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c): after `gBS->OpenProtocol(EFI_LOADED_IMAGE_PROTOCOL_GUID)`, call `EFI_BLOCK_IO_PROTOCOL` to read the GPT partition entry for the device that launched us. Verify partition type GUID equals `EFI_PARTITION_TYPE_SYSTEM_PARTITION_GUID` (`C12A7328-F81F-11D2-BA4B-00A0C93EC93B`). Halt with `boot_halt("ESP type GUID mismatch")` on failure.
- [ ] Verify the ESP filesystem is FAT32 by reading the BPB's `BS_FilSysType` field (offset 0x52, "FAT32   "). Tolerate FAT16 only on tiny test ESPs (< 16 MB) with a one-line WARN to serial.
- [ ] Sanity-check that `\EFI\BOOT\BOOTX64.EFI`, `\kernel.exe`, and `\boot.conf` exist before attempting their full load, so missing-file errors are reported in one batch instead of cascading through the load chain.
- [ ] Surface the ESP UUID + size into `HKLM\HARDWARE\BOOT\ESP\{Uuid, SizeMB}` via `boot_info` so post-boot tools can identify the boot disk without re-reading firmware.
- [ ] Commit: `"boot: ESP integrity check (GPT type GUID + FAT32 BPB + required-files batch)"`

> [!NOTE]
> ESP cryptographic verification (signed manifest) is out of scope for this section; the trust anchor for the legacy split path is Secure Boot signature on `BOOTX64.EFI` itself. UKI (§11) is the path that closes whole-chain signing. This section is corruption / wrong-partition detection, not adversary defense.

**Test checkpoint:** Booting from a freshly-formatted ESP (FAT32, correct GUID) passes silently; flipping the partition type GUID via `gdisk` triggers the `boot_halt("ESP type GUID mismatch")` path; deleting `kernel.exe` from the ESP triggers a single batched error reporting all three required-file checks. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 14. Win32 Firmware Variable + Table Surface

§2 wired `NtQuerySystemEnvironmentValueEx` / `NtSetSystemEnvironmentValueEx` into the SSDT, but `kernel32.dll` does not export the corresponding Win32-named functions (`GetFirmwareEnvironmentVariableA/W`, `SetFirmwareEnvironmentVariableA/W`, `GetSystemFirmwareTable`, `EnumSystemFirmwareTables`). Win11 + Linux WINE applications that query firmware variables or SMBIOS tables fail to resolve these symbols at load time. The native NT API is reachable; the Win32 facade is not. This is purely a wiring gap -- the underlying syscalls exist; the export table needs new entries.

- [ ] Add `GetFirmwareEnvironmentVariableA/W` and `SetFirmwareEnvironmentVariableA/W` to `s_kernel32_exports[]` in [`src/kernel/pe.c`](../../src/kernel/pe.c), proxying to the existing `SSDT_NtQuerySystemEnvironmentValueEx` / `SSDT_NtSetSystemEnvironmentValueEx` entries.
- [ ] Add `GetSystemFirmwareTable` and `EnumSystemFirmwareTables` to `s_kernel32_exports[]`, returning the cached ACPI / SMBIOS tables from Phase 1 init (the kernel already parses these in §4 SMBIOS + ACPI init).
- [ ] Implement the Win32-style ANSI/Wide name conversion + privilege check (`SE_SYSTEM_ENVIRONMENT_NAME` privilege required for Set, per Win32 doc) in the export trampoline, then dispatch to the NT syscall.
- [ ] Surface variable storage quota -- Win32 has no canonical export, but the registry should mirror `QueryVariableInfo` results: `HKLM\SYSTEM\SecureBoot\Vars\{MaxStorageSize, RemainingSize, MaxVariableSize}` (DWORDs, in bytes), refreshed at boot. Documents the 64 KB / per-machine total cap that production firmware enforces.
- [ ] Commit: `"kernel: kernel32 exports for Win32 firmware variable + table APIs; QueryVariableInfo registry mirror"`

> [!NOTE]
> SE_SYSTEM_ENVIRONMENT_NAME privilege gating is partial in this section's scope (the TODO-23 referenced under §2 owns the privilege table). Drop a comment in the trampoline pointing at TODO-23's privilege check, and gate the Set path on a TODO marker until the privilege table lands.

**Test checkpoint:** A Win32 .exe linked against the shipping kernel32 import library can call `GetFirmwareEnvironmentVariableA("SecureBoot", L"{8be4df61-...}", buf, sizeof(buf))` and receive the same byte the SSDT path returns; `GetSystemFirmwareTable('RSMB', 0, buf, sizeof(buf))` returns the SMBIOS table cached at boot; `HKLM\SYSTEM\SecureBoot\Vars\MaxStorageSize` is non-zero. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐   | Feature               | 🪟 Win11                       | 🐧 Linux                     | 🚀 Impossible OS                |
| --- | ---------------------- | ------------------------------- | ---------------------------- | -------------------------------- |
| 💎   | UEFI runtime post-EBS | ✅ Full RT via hal.dll         | ✅ efi_call wrapper          | ✅ §1 SVAM + 6 RT services      |
| 💎   | UEFI variables        | ✅ NtQuery/SetSystemEnvValue   | ✅ efivarfs mount            | ✅ §2 get/set/enum + SSDT wired |
| 💎   | GOP resolution        | ✅ Boot mgr + BCD              | ✅ EFIFB + simplefb          | ✅ §3 auto-select best mode     |
| 💎   | SMBIOS core           | ✅ WMI Win32_BIOS class        | ✅ sysfs /sys/class/dmi      | ✅ §4 types 0-4 + registry      |
| 💎   | Secure Boot shim      | ✅ MS-signed shim + MOK        | ✅ rhboot/shim + MokManager  | ✅ §6 MOK chain + sbsign        |
| 💎   | Secure Boot state     | ✅ Registry + msinfo32         | ✅ efivar + mokutil --sb     | ✅ §5 NVRAM + registry State    |
| ⭐   | Boot timeline         | ❌ ETW WPA (heavyweight)       | ❌ systemd-analyze (userland)| ✅ §7 per-step JSON + NVRAM     |
| 💎   | Atomic serial         | ✅ KdPrint spinlock            | ✅ printk logbuf             | ✅ §8 klog ring + serial        |
| 💎   | SBAT shim ops         | ✅ MS Secure Boot program      | ✅ distro shim refresh       | ✅ §9 SBAT checklist doc        |
| 💎   | DB/dbx inventory      | ✅ msinfo32 SB details         | ✅ mokutil --db              | ✅ §9 registry Db/Dbx counts    |
| 💎   | EBS retry hardening   | ✅ bootmgr bounded retry       | ✅ efi-stub retry patch      | ✅ §9 4-attempt bounded loop    |
| 💎   | Capsule install UX    | ✅ Windows Update stack        | ✅ fwupd + LVFS              | ⬜ TODO-27 §2 (query-only)      |
| 💎   | MS UEFI CA lifecycle  | ✅ Windows Update CA rotation  | ⚠️ Distro re-sign timing     | ⬜ §12 build-time CA WARN + registry surface |
| 💎   | ESP integrity check   | ⚠️ BootMgr GUID / FAT32 only   | ⚠️ efibootmgr UUID surface   | ⬜ §13 GUID + BPB + batch + UUID mirror |
| 💎   | Win32 firmware vars   | ✅ kernel32 GetFirmwareEnv*    | ⚠️ WINE shim only            | ⬜ §14 kernel32 exports + quota mirror |

> **Parity:** 💎 rows match Win11+Linux baseline. **⭐** JSON boot profile is extra vs ETW and userland boot charts. Capsule **apply** path and W^X on RT pages stay in [TODO-27](TODO-27-uefi-advanced.md); kernel already runs read-only `esrt_init()` / `uefi_capsule_init()` / `uefi_crypto_agility_init()` during Phase 1 bring-up.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_uefi_boot()`. Tests must follow CLAUDE.md kernel test rules (no `boot_progress`, `panic`, live `serial_init`, etc.).

- [x] Create `src/kernel/test/test_uefi_boot.c` with 9 suites: RT available, var_get SecureBoot, var_u32 roundtrip (Impossible OS vendor GUID), framebuffer width/height, HiDPI consistency, SMBIOS UUID, Secure Boot state consistency, registry BIOS vendor, SecureBoot DB mirror (DbEntries/DbxEntries match secureboot_get_db_info)
- [x] Register in `test_runner_init()`: `test_register_uefi_boot()` under Boot category
- [x] Commit: `"test: add uefi_boot test suite"` (07ce1ac3)
- [ ] Add §12 test: registry `HKLM\SYSTEM\SecureBoot\ShimCA` is a non-zero DWORD matching the build-host's recorded shim CA generation.
- [ ] Add §13 test: ESP probe returns valid GPT type GUID match (constant compare via `RtlCompareMemory(&type_guid, &EFI_PARTITION_TYPE_SYSTEM_PARTITION_GUID, sizeof(EFI_GUID))` returning the full 16); ESP UUID surfaced under `HKLM\HARDWARE\BOOT\ESP\Uuid` is non-zero.
- [ ] Add §14 test: `pe_resolve_export("kernel32.dll", "GetFirmwareEnvironmentVariableA")` returns a non-NULL function pointer; calling it with `L"SecureBoot"` returns the same byte the SSDT path returns; `HKLM\SYSTEM\SecureBoot\Vars\MaxStorageSize` is a non-zero DWORD.
- [ ] Commit: `"test: extend uefi_boot suite with shim CA, ESP probe, kernel32 firmware exports"`

> **Done:** 9 suites, 13 assertions -- registered in `test_runner_init()` (2026-04-10)

**Test checkpoint:** `SUITE=boot` run shows new `test_uefi_boot` cases PASS; no forbidden boot/VPD calls from test body. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## Verification

- [x] Serial log shows `[UEFI] SetVirtualAddressMap OK` and `[UEFI] Runtime services: OK`
- [x] `uefi_var_get(L"SecureBoot", ...)` returns 0 (disabled) in QEMU
- [x] GOP negotiation log shows `[Boot] GOP: {W}x{H} 32bpp (mode N)`
- [x] SMBIOS data appears in Registry under `HKLM\HARDWARE\BIOS\*` and `HKLM\HARDWARE\System\*`
- [x] Signed build present when MOK keys exist; skipped silently when absent
- [x] No interleaved serial lines under concurrent IRQ logging

**Test checkpoint:** Repeat Verification bullets on a clean build after Unit Tests land; serial matches expected markers above on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 9 suites, 0 failures

---

## History

| Date | Action | Summary |
| --- | --- | --- |
| 2026-04-10 | validate | validate-todo-file: Inputs `---` + XREF section refs (TODO-14 §3, TODO-11 §2/§3, TODO-20 §4); Impl T14 fixed; removed `### 8.1/8.2`; §1-§8 Commit+Test checkpoint+platforms; §4/§5 deferred items as NOTE; OS table compact; Unit Tests NOTE+checkpoint; Verification runner+checkpoint; History added. |
| 2026-04-10 | gap-analysis | gap-analysis-todo: Current state merged into IMPORTANT; new §9 (SBAT doc, EBS retry, DB registry) + Impl Order row 9 `[ ]`; OS rows + Sources; Unit Tests §9 hook; cross-TODO XREF repairs in TODO-05/17/18/02-memory-security/09-desktop; code-truth note for existing `secureboot_keys_init`/`tpm_init`/capsule query init. |
| 2026-04-10 | validate | validate-todo-file: continuation-line rg clean; Inputs + `uefi_runtime.c` + `secure-boot-keys.md`; §9 registry path fix + `[!WARNING]` regression callout; OS capsule row + parity note; XREF §2/§4/§5/§7/§9 verified; `run-boot-tests.bat` present; optional note: OS row 206 Linux cell may deserve `systemd-analyze` nuance. |
