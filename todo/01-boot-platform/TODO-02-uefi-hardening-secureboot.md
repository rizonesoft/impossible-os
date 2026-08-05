---
schema_version: 1
id: uefi-hardening-secureboot
domain: 01-boot-platform
status: active
title: "TODO-02 -- UEFI Bootloader Hardening & Secure Boot"
---

# TODO-02 -- UEFI Bootloader Hardening & Secure Boot

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

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
- -> XREF: `01-boot-platform/TODO-04-firmware-table-platform-inventory.md §1` -- higher-level firmware_table_entry_t catalog that consumes §14's `acpi_enumerate_signatures` / `acpi_get_raw_table` / `smbios_get_raw_table` helpers
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

| ⭐  | Order | Deliverable                                              | Depends On     | Status |
| --- | :---: | -------------------------------------------------------- | -------------- | :----: |
| 💎  |   1   | UEFI runtime services preservation                       | --             |  [x]   |
| 💎  |   2   | UEFI variable services                                   | §1             |  [x]   |
| 💎  |   3   | GOP resolution auto-detection                            | §1             |  [x]   |
| 💎  |   4   | SMBIOS table parsing                                     | §1             |  [x]   |
| 💎  |   5   | Secure Boot state detection                              | §2             |  [x]   |
| 💎  |   6   | Secure Boot shim chain-loading                           | §5             |  [x]   |
| 💎  |   7   | Boot UX polish                                           | §3, §5, T14 §2 |  [x]   |
| ⭐  |   8   | Serial log standardization                               | --             |  [x]   |
| 💎  |   9   | SBAT ops, DB registry mirror, EBS retry                  | §2, §5         |  [x]   |
| 💎  |  10   | RT sleepable lock migration                              | §1             |  [x]   |
| 💎  |  11   | Unified signed boot artifact (UKI-style)                 | §6             |  [x]   |
| 💎  |  12   | MS UEFI CA 2023 transition + 2011 retirement             | §6             |  [/]   |
| 💎  |  13   | EFI System Partition integrity check                     | --             |  [x]   |
| 💎  |  14   | Win32 firmware variable + table surface                  | §2             |  [x]   |
| 💎  |  15   | Post-boot SecureBoot revalidation                        | §5             |  [/]   |
| 💎  |  16   | Signed `.initrd` / recovery / module PE sections in UKI  | §11            |  [/]   |
| 💎  |  17   | SBAT revocation metadata in boot artifacts               | §9, §11        |  [x]   |
| 🛠️  |  18   | Build idempotency: UKI SBAT survives incremental rebuild | §11, §17       |  [/]   |
| 🛠️  |  19   | Bootloader `ReadBlocks` IoAlign compliance               | §13            |  [/]   |
| 🛠️  |  20   | Post-ship follow-up backfill (2026-07-31 cohort)         | --             |  [ ]   |

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
> **Accepted:** [H] PMM bitmap unsynchronized; `pmm_alloc_contiguous` / `pmm_free_frame` race on SMP when uefi_value_alloc routes >4 KiB buffers there (reason: kernel-wide PMM concern, not §2-specific) -> XREF: 03-memory-concurrency/TODO-03 §1 (item: "PMM bitmap SMP locking" at line 103)
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
- [x] Commit: `"kernel: Secure Boot state detection, registry key"`

> Out-of-scope work that originally lived under §5 has been migrated to its owning TODOs (2026-05-01): padlock tray icon -> [`08-graphics-ui/TODO-11-startmenu-tray-notifications.md §4`](../08-graphics-ui/TODO-11-startmenu-tray-notifications.md#4-system-tray-icons-sonnet); post-boot drift-detection wiring -> §15 below. The TODO-27 §5 ownership narrowing was already applied during the 2026-05-01 gap-audit ([TODO-27 §5](TODO-27-uefi-advanced.md#5-secure-boot-extended-state-and-enforcement-policy) consumes the canonical state via the `uefi_secureboot_*` API and is scoped to enforcement policy). §5 here is now scoped to the boot-time state read + registry publication only.

> [!NOTE]
> Kernel **code signature verification** for loaded images is owned by `02-kernel-core/TODO-10-kernel-security-hardening.md §11` (Enclave and code signing) and cross-notes in `02-kernel-core/TODO-15-security-reference-monitor.md`, not this bootloader TODO.

**Test checkpoint:** `boot_info.secure_boot_enabled` matches UEFI `SecureBoot` variable when read succeeded (`StateValid=1`); registry `HKLM\SYSTEM\SecureBoot\StateValid` is `1` if the SecureBoot variable read succeeded, `0` if it failed. `State` key is present (0 or 1) ONLY when `StateValid=1`; absent on read-failure path so legacy boolean readers cannot mistake unknown for disabled. Padlock tray rendering is verified by [TODO-11 §4](../08-graphics-ui/TODO-11-startmenu-tray-notifications.md#4-system-tray-icons-sonnet). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot, TEST_CAT_BOOT) | test_secureboot_state_matches_var + test_secureboot_db_inventory_matches_registry; smoke covers boot reach to C:\>

> **Notes:**
> - `uefi_secureboot_init()` in `src/kernel/uefi_runtime.c:825-944` reads SecureBoot/SetupMode/DeployedMode/AuditMode (UEFI 2.5+) global vars + PK/KEK existence; publishes to `g_boot_info.secure_boot_enabled` + `g_system_state.secure_boot` + `BOOT_CAP_SECURE_BOOT_STATE` cap bit ONLY when the variable read succeeded. Failure path: state stays zero-init, cap stays degraded, klog says UNKNOWN.
> - Hostile-firmware hardening (Codex 4x review 2026-04-28): `read_global_byte` now requires `status==SUCCESS && sz==sizeof(val)` (rejects firmware returning SUCCESS with sz=0/sz>1); `global_var_exists` requires `sz>0` (rejects empty PK/KEK as "enrolled"); `uefi_secureboot_populate_registry` writes `StateValid` always but `State` ONLY when valid (closes the unknown-vs-disabled ambiguity in the registry; 0xFF sentinel attempted but re-adversarial caught it polluted the bool contract for legacy readers).
> - Padlock tray icon: `g_system_state.secure_boot` flag is published here; rendering is owned by [TODO-11 §4](../08-graphics-ui/TODO-11-startmenu-tray-notifications.md#4-system-tray-icons-sonnet) (filed there 2026-05-01).
> - Canonical doc: this section + UEFI 2.10 section 8.2.1 (Secure Boot variables) + UEFI 2.10 section 3.3 (PK/KEK semantics).
> - Scope boundary: §5 owns boot-time SecureBoot state read + registry publication. Post-boot revalidation (drift detection on S3 resume + periodic) is owned by §15 of this TODO. Kernel **code signature verification** for loaded images is owned by `02-kernel-core/TODO-10-kernel-security-hardening.md §11`; SecureBoot DB/dbx inventory is owned by §9 of this TODO.

> **Verified:** 2026-04-28 | commit `35e9e17e` | 4/4 items (3 [x] + 1 [/]) | build OK | tests test_secureboot_state_matches_var + test_secureboot_db_inventory_matches_registry wired
> **Quality reviewed:** 2026-04-28 | Codex 4x (adversarial + consistency + perf + re-adversarial) | 1H+2M fixed (+1 re-adversarial regression) | scope: kernel-code-quality (gates walked: SMP-safe by default, memory rules, bare-metal correctness, complete error paths, no TODO/FIXME, Win32 surface)
> **Scope migration:** 2026-05-01 | section narrowed to boot-time state read; padlock tray icon migrated to TODO-11 §4; post-boot drift revalidation moved to §15 of this TODO; TODO-27 §5 ownership narrowing migrated to TODO-27 §5 itself. IO row flipped [/] -> [x] because remaining items are all [x].

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

> **Test runner:** `scripts\debug\kernel\run-secureboot.bat` (TCG + q35 + SMM + OVMF-secure; WHPX cannot emulate Secure Boot pflash) | manual: shim launches MokManager on first boot, enroll `\MOK.cer` from ESP root, signed `grubx64.efi` then trusts the chain. Build-time validation via `scripts/sign-efi.sh` (sbsign + sbverify on every `make sign-efi` when `keys/MOK.key` present).

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
- [x] JSON boot-timeline export via `boot_timeline_dump_json()` -> XREF: [`01-boot-platform/TODO-11 §9`](TODO-11-interrupt-timer-arch.md) (item: "`boot_timeline_dump_json()`: writes `X:\Perf\boot-timeline.json`" at line 215). Implementation lives at `src/kernel/main/boot_progress.c:203` (header decl `include/kernel/boot_progress.h:64`); writes a JSON array that begins with up to 5 FPDT firmware entries (`source:"fpdt"`, `unreliable:bool`) followed by bootloader+kernel TSC steps (`source:"tsc"`). Path moved from `X:\Boot\` to `X:\Perf\` by TODO-04 FPDT and Boot Timing Normalization. The earlier `TODO-11-interrupt-timer-arch.md §3` XREF was stale (kernel-domain TODO-11 was renamed `warm-kernel-update-runtime`; the right anchor is the `01-boot-platform` TODO-11 §9 above).
- [x] Pre-framebuffer error screen: `boot_halt(reason)` with inline 8x8 bitmap font
- [x] `boot_splash_status()` integration: live stage text below spinner
- [x] Commit: `"boot: boot-stage instrumentation, pre-framebuffer error recovery screen"`

**Test checkpoint:** `boot_progress()` stages appear in order on serial and optional `X:\Perf\boot-profile.log`; `boot_halt()` shows pre-framebuffer error text when forced; JSON timeline export matches the boot-platform Boot Time Visualization contract. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot, TEST_CAT_BOOT) -- covers `boot_progress`/`boot_timing_record_step` null-safety + POST16 uniqueness in `test_boot_init.c`; smoke (KVM 2.3s) covers boot reach to C:\>

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

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot, TEST_CAT_BOOT) -- covers `test_klog_ring_write` / `test_klog_level_drop` / `test_klog_level_pass` / `test_klog_global_level` / `test_klog_rate_limit_api` in `test_klog.c`; smoke covers boot reach to C:\>

> **Notes:**
> - What shipped: 5-level klog (`LOG_DEBUG`->`[INFO]`, `LOG_INFO`->`[ OK ]`, `LOG_WARN`->`[WARN]`, `LOG_ERROR`->`[FAIL]`, `LOG_FATAL`->`[CRIT]`); atomic per-line `serial_write` (spinlock-serialized) + atomic `klog()` builder at `src/kernel/klog.c:934 char line[512]`; recovery-replay path at `klog.c:537 char line[192]`. `printk()` rewritten 2026-04-28 to also be atomic (single bounded buffer + one `serial_write`).
> - How it integrates: `serial_putchar_raw()` static inline (no lock); `serial_write()` holds `g_serial_lock` for whole string; `klog()` snapshots a ring entry under `s_klog_lock`, releases, then formats line outside the lock and emits via single `serial_write`. ANSI color escape sequences with reserved-tail termination so the terminal cannot get stuck in color on truncation.
> - Downstream: `klog_get_ring()` is locked-snapshot-safe for `(count, head)`; entry contents are still live (no production caller iterates entries concurrently with logging).
> - Doc pointer: header at `include/kernel/klog.h` documents the level mapping and `klog()` API.
> - Scope boundary: §8 owns the format + atomic-line plumbing. Per-process console / log-level filtering / framebuffer scrollback are out of scope (separate sections).

> **Verified:** 2026-04-28 | commit `e3e138f2`-era impl + `0346727f` (re-review fixes: 1H+3M+1L) | 4/4 items | build OK | tests 276/276 PASS, audit-ai-system 7/7 PASS
> **Quality reviewed:** 2026-04-28 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+3M+1L fixed | scope: kernel-code-quality (SMP-safe spinlock + IRQ-safe + bounded buffers + ANSI state termination)

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

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | covered by file-wide `test_uefi_boot.c` (DbEntries/DbxEntries assertions); EBS-retry surface validated via smoke + serial pattern check.

> **Notes:**
> - **What shipped**: bounded `ExitBootServices` retry loop in `src/boot/uefi/bootx64.c` (`EBS_MAX_ATTEMPTS=4` -- locked per agreement with `01-boot-platform/TODO-03 §2`), each retry refetches the memory map and refills the runtime maps before re-attempting; `secureboot_get_db_info()` mirror writes 5 DWORDs (`DbEntries`, `DbxEntries`, `DbSha256`, `DbxSha256`, `DbX509`) under `HKLM\SYSTEM\SecureBoot\` via `uefi_secureboot_populate_registry()` in `src/kernel/uefi_runtime.c`; SBAT bump / shim-refresh checklist added to `docs/guides/secure-boot-keys.md`.
> - **How it runs / integrates**: registry writes piggyback on the same `RegCreateKeyEx` scope as `State` so the open-key-handle dependency is local; EBS retry path is fail-fast after 4 attempts (`boot_fatal(BOOT_ERR_EXIT_BS_FAIL, ...)`).
> - **Downstream effects**: closes the runtime-trust ops parity gap with Win11 (`HKLM\SYSTEM\CurrentControlSet\Control\SecureBoot\State`) and Linux's `/sys/firmware/efi/efivars/SecureBoot-*`. UpdateCapsule write paths remain owned by `01-boot-platform/TODO-27 §2`.
> - **Canonical doc**: [`docs/guides/secure-boot-keys.md`](../../docs/guides/secure-boot-keys.md) (5-step SBAT/shim refresh checklist).
> - **Scope boundary**: §9 owns the SBAT-doc + EBS-retry + DB-registry triple. Capsule install + ESRT-driven UX is `01-boot-platform/TODO-27 §2`. Padlock tray-icon consumer of `g_system_state.secure_boot` is `08-graphics-ui/TODO-11 §4` (filed via §5).

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

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | covered by file-wide `test_uefi_boot.c` RT-availability + variable-roundtrip suites; latency-warning + panic-trylock paths validated via smoke + serial pattern check.

> **Notes:**
> - What shipped: `s_rt_mutex` + `rt_call_enter`/`rt_call_exit` (sleepable lock with LAPIC LVT mask + TSC latency >50ms warn) + `rt_call_enter_emergency`/`rt_call_exit_emergency` (trylock for panic/reset path) in `src/kernel/uefi_runtime.c`.
> - How it integrates: all thread-context RT callers (GetVariable/SetVariable/GetTime/SetTime/GetNextVariableName/GetWakeupTime/QueryVariableInfo) acquire the mutex; `uefi_reset` uses the trylock path with a `got_lock` guard at uefi_runtime.c:649-654 so emergency unlock is balanced.
> - Downstream: closes the spinlock-with-IRQs-disabled stall on flash erase/write; matches Windows `FAST_MUTEX` and Linux `efi_runtime_lock` semaphore at parity. TSC latency log exceeds both.
> - Canonical doc: `src/kernel/uefi_runtime.c` (consumer); `src/kernel/sched/mutex.c` (primitive).
> - Scope boundary: §10 ships the UEFI consumer. The mutex primitive itself is owned by `03-memory-concurrency/TODO-08-advanced-sync` -- the 2026-04-28 re-review surfaced an SMP wait-queue race in that primitive (filed as §11 there).

> **Verified:** 2026-04-28 (re-verify; original ship 2026-04-11) | RISK_TIER=critical | 5/5 items | build OK | code-truth confirmed at uefi_runtime.c:110/438-466/469-488/649-654
> **Accepted:** [H] mutex_lock loser path mutates num_waiters/waiter_tasks[]/waiter_threads[] with plain loads/stores under SMP CAS contention; lost waiter sleeps forever -> XREF: 03-memory-concurrency/TODO-08-advanced-sync §11 (item: "Wait-queue protection: per-mutex spinlock_t wait_lock around enqueue/dequeue/wake")
> **Quality reviewed:** 2026-04-28 (re-verify) | Codex 1x (adversarial) | 0 fixed, 1H accepted-XREF | scope: N/A (re-verify of already-shipped section; original 3x dispatch on 2026-04-11; no C/H diff to re-quality-review)

---

## 11. Unified Signed Boot Artifact (UKI-style)

systemd-boot ships a Unified Kernel Image (UKI) format: a single signed UEFI PE that bundles the EFI stub, kernel, optional initrd, kernel cmdline, and other resources. Tampering with any component invalidates the whole signature. UEFI firmware can invoke the UKI directly (useful in Confidential Computing) OR a boot loader can chain into it. Impossible OS currently signs `BOOTX64.EFI` and ships `kernel.exe` + `boot.conf` as separate artifacts; a Secure Boot signature on `BOOTX64.EFI` does NOT cover the kernel or the config. A UKI-style unified artifact closes that gap.

> [!NOTE]
> The handoff-ABI side (how `boot_info` carries the UKI-origin flag, how the kernel verifies it was invoked through the unified path vs legacy split) is owned by [`01-boot-platform/TODO-01 §8`](TODO-01-boot-protocol-abi-handoff.md#8-boot-protocol-documentation-and-schema-changelog) (Boot Protocol Documentation and Schema Changelog). This section owns the signing + packaging + build-pipeline side.

- [x] Packaging: `scripts/build.sh` runs an `llvm-objcopy --add-section` step between `EFI Boot` and `EFI Signing`, producing `build/tools/BOOTX64.UKI.efi` with `.osrel` (auto-generated NAME/ID/VERSION_ID/PRETTY_NAME), `.cmdline` (copy of `resources/boot/boot.conf`), and `.linux` (the ELF kernel). UAPI Group UKI spec layout. Section virtual addresses auto-placed by objcopy past the stub's existing sections; bootloader walks the table by name so spec VAs are not load-bearing.
- [x] [`scripts/sign-efi.sh`](../../scripts/sign-efi.sh) refactored into a `sign_one()` helper that handles atomic sign + verify + replace; called for both `BOOTX64.EFI` (split path) and `BOOTX64.UKI.efi` (whole-chain). Both paths remain installable; UKI is preferred for modern Secure Boot / direct-firmware-invoke; split stays for legacy loaders.
- [x] `BOOT_FLAG_INVOKED_VIA_UKI` (1u<<3) added to [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) + [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h); `BOOT_INFO_VERSION` bumped 9 -> 10; `BOOT_FLAG_MASK_KNOWN` extended. Set by `load_kernel()` in `bootx64.c` when the UKI fast path fires (g_uki_kernel_ptr non-NULL after `detect_uki_sections()` PE walk). `parse_boot_conf()` short-circuits to the embedded `.cmdline` so cmdline is also covered by the firmware-Secure-Boot signature (Codex post-impl H1 fix: disk boot.conf is bypassed in UKI mode to preserve whole-chain signature semantics).
- [x] [`docs/guides/secure-boot-keys.md`](../../docs/guides/secure-boot-keys.md) "Unified Kernel Image (UKI)" subsection: section layout table, build-pipeline objcopy invocation, sign-efi sign_one() helper, bootloader detect_uki_sections() walker, PCR measurement order note, when-to-use matrix.
- [x] Smoke test (`bash scripts/test-smoke.sh`) reaches `Boot complete in 2.570s` + `C:\>` on the split path with the new code; the UKI artifact is built + signed alongside the split path and is invocable via direct-firmware-invoke (the existing scripts/debug/kernel/run-boot-tests.bat covers both via QEMU WHPX/TCG, VirtualBox, bare metal).
- [x] **Stamp-identity binding for key rotation** (Codex review-todo-section round-5 H1, shipped 2026-05-01): [`scripts/sign-efi.sh`](../../scripts/sign-efi.sh) now writes the SHA-256 of `MOK_CRT` to `${SIGN_FINGERPRINT_FILE}` (passed as `build/.signed-artifacts.fingerprint` from the [Makefile](../../Makefile)) AFTER both Phase 2 `mv`s succeed and the rollback trap clears -- atomic via temp+mv. The Makefile computes `SIGN_DESIRED_FP` at parse time via `sha256sum`-then-`shasum` fallback (matches sign-efi.sh portability) and `SIGN_RECORDED_FP` from the sidecar; when they differ the stamp is invalidated via `$(shell rm -f $(SIGN_STAMP))` so the next recipe expansion fires. Codex H1 (2026-05-01) fail-closed extension: when `MOK_KEY` exists but `SIGN_DESIRED_FP` cannot be computed (cert missing/unreadable/typoed, or neither `sha256sum` nor `shasum` on PATH), the stamp is force-invalidated so `sign-efi.sh` runs and surfaces the underlying error rather than the build silently shipping a stale signature. Validated: with stamp + BOOTX64.EFI fresh, swapping `MOK_CRT` to a different-content cert with OLDER mtime forced a re-sign and recorded the new fingerprint; with `MOK_CRT` set to a non-existent path the stamp was invalidated and `sign-efi.sh` failed loudly via sbsign instead of skipping.
- [x] Commit: `"boot: Unified signed boot artifact (UKI) alongside split BOOTX64.EFI"`

> Out-of-scope work that originally lived under §11 has been promoted to its own section (2026-05-01): signed `.initrd` / `.recovery` / `.modules` PE-section embedding + bootloader walk + cmdline-rejection -> §16 below. §11 here is now scoped to the UKI artifact + dual-sign pipeline + boot_info `BOOT_FLAG_INVOKED_VIA_UKI` flag only.

**Test checkpoint:** `bash scripts/build.sh` emits both `BOOTX64.EFI` (existing split path) AND `BOOTX64.UKI.efi` (new unified artifact). Signing succeeds on both when `MOK.key` is present. Booting the UKI via QEMU direct-firmware-invoke (`-drive if=pflash,format=raw,file=OVMF.fd` + `-cdrom BOOTX64.UKI.efi.iso` OR by placing the UKI at `EFI/BOOT/BOOTX64.EFI`) reaches `C:\>` cleanly with `boot_info.flags & BOOT_FLAG_INVOKED_VIA_UKI`. Split-path boot on the same firmware still works unchanged. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal (UKI is the modern-firmware path; bare metal via UEFI 2.7+ with Secure Boot should execute the UKI directly).

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 17 sub-tests, 0 failures (existing boot suite + 3 new UKI flag tests in test_uefi_boot.c)

> **Notes:**
> - What shipped: `BOOTX64.UKI.efi` (UAPI Group UKI artifact, 6.1 MB), shared `sign_one()` helper signing both PEs, `detect_uki_sections()` PE-walker + `g_uki_*` globals + `BOOT_FLAG_INVOKED_VIA_UKI` (v10 ABI), `parse_boot_conf()` UKI short-circuit, `docs/guides/secure-boot-keys.md` UKI subsection, 3 unit tests for the v10 flag bit.
> - How it integrates: `scripts/build.sh` UKI pack step runs between `EFI Boot` and `EFI Signing`; bootloader walks its own LoadedImage PE table for `.linux`/`.cmdline`/`.osrel`; UKI fast path in `load_kernel()` and `parse_boot_conf()` skips ESP filesystem entirely so whole-chain signature covers kernel + cmdline.
> - Downstream effects: closes the gap where `BOOTX64.EFI` signature didn't cover kernel.exe + boot.conf; satisfies TODO-01 §8 schema changelog v9->v10 entry; the `.osrel` section gives attestation + telemetry consumers a canonical NAME/ID/VERSION_ID surface. Codex 2x review adoptions in commit `3bc47f6f`.
> - Canonical doc: [`docs/guides/secure-boot-keys.md`](../../docs/guides/secure-boot-keys.md) "Unified Kernel Image (UKI)".
> - Scope boundary: §11 owns build/sign/bootloader/ABI surfaces. TODO-01 §8 owns the boot_info schema changelog (v10 entry); TODO-13 owns the measured-boot log + PCR replay; §6 owns the underlying EFI signing pipeline. Recovery image (`.initrd` PE section) reserved for future use; not populated today.

> **Verified:** 2026-04-29 | commit `3bc47f6f` (impl ship `6b3df1fb`) | 7/8 items, 2 deferred [/] (stamp-identity binding; signed-initrd/recovery PE sections from gap-audit 2026-05-01) | build OK | smoke PASS (KVM 2.370s) | secureboot-smoke 7/7 PASS
> **Quality reviewed:** 2026-04-29 | Codex 10x (design + adversarial-impl + adversarial + consistency + perf + 5x re-adversarial) | 8H+3M fixed, 1H deferred-XREF | scope: boot-code-quality (UEFI PE walk + EBS boundary + ABI sync + atomic signing pipeline)
> **Deferred resolved 2026-05-01:** [H] stamp identity binding for MOK key rotation -- shipped via cert SHA-256 sidecar + Makefile parse-time invalidation with fail-closed-on-uncomputable-fingerprint (Codex 1H folded in same dispatch).
> **Scope migration 2026-05-01:** signed `.initrd` / `.recovery` / `.modules` PE-section embedding promoted to §16 of this TODO (was a §11 deferred [H] from gap-audit 2026-05-01). §11 IO row flipped [/] -> [x] because remaining items are all [x] after migration; §16 IO row carries the new [ ] state.

---

## 12. MS UEFI CA 2023 Transition + 2011 Retirement Tracking

Microsoft began rotating UEFI signing certificates in 2024-2025: the original `Microsoft Corporation UEFI CA 2011` (the cert that signs every shim Microsoft has shipped) is scheduled to expire June 2026, and a replacement `Microsoft Corporation UEFI CA 2023` is being enrolled into firmware DBs via Windows Update. Devices booting a shim still signed only by 2011 CA will start failing on machines whose firmware's KEK/db has rotated to 2023-only. Any Impossible OS install that ships through 2026+ MUST track this transition and re-sign shim against the 2023 CA before the 2011 cert expires. This section is doctrine + ops; the actual signing happens in §6's pipeline.

> [!IMPORTANT]
> **2026 ship-blocker if ignored.** Without 2023-CA-signed shim, machines that received the firmware DB rotation through Windows Update will refuse to boot Impossible OS. The transition window is firm.

- [x] "MS UEFI CA Lifecycle" section in [`docs/guides/secure-boot-keys.md`](../../docs/guides/secure-boot-keys.md) covering: 2011 CA expiry date (June 2026), 2023 CA enrollment timeline, how to verify which CA your shim is signed against (`sbverify --list shim/shimx64.efi`), how to re-sign + redistribute when MS publishes the 2023-signed shim binary.
- [x] [`scripts/sign-efi.sh`](../../scripts/sign-efi.sh) post-sign block emits `[shim] signed-by: Microsoft Corporation UEFI CA YYYY` AND graduated deprecation policy (pre-2026-05-01 silent / 2026-05-01..2026-06-30 WARN / post-2026-06-30 FAIL exit 1) -- check fires BEFORE the mv that promotes signed binary so failure leaves original unchanged (Codex M1 fix). Update [`scripts/sign-efi.sh`](../../scripts/sign-efi.sh) to log which MS UEFI CA the bundled `shim/shimx64.efi` is signed by (parse `sbverify --list` output, emit `[shim] signed-by: Microsoft Corporation UEFI CA <year>`). Fail loudly if signed-by year is in the deprecated set.
- [x] Build-time check: `scripts/test-tooling.sh` `shim_ca_detection` block runs sign-efi.sh against synthetic 2011/2023 CA + pre-warn/warn/post-expiry date matrix via `SHIM_CA_TEST_TODAY` override env. Original wording continued: Add a build-time check (`scripts/build.sh` or `scripts/test-tooling.sh` sub-test): if the shim binary is signed only by the deprecated CA AND the build host's date is past 2026-04-01, emit a WARN. The 60-day pre-expiry window is the safety margin per the MS guidance.
- [x] `HKLM\SYSTEM\SecureBoot\ShimCA` (DWORD) populated by `uefi_secureboot_populate_registry()` at boot from `SHIM_CA_YEAR` (build-time-extracted from `sbverify --list shim/shimx64.efi` via new `scripts/extract-shim-ca.sh` -> `build/generated/shim_ca.h`). Sentinels: 0 (not pinned), 0xFFFFFFFF (parse fail), 2011|2023. SHA-bound at gen time (Codex H1 fix). Original wording: Surface the shim CA generation in `HKLM\SYSTEM\SecureBoot\ShimCA` (DWORD: 2011 or 2023) at boot via [`uefi_secureboot_populate_registry()`](../../src/kernel/uefi_runtime.c). Consumers (msinfo32-equivalent, audit tools) can read it.
- [x] Commit: `"boot: track MS UEFI CA 2023 transition; sign-efi.sh emits CA generation; registry surface"`

**Test checkpoint:** `sbverify --list shim/shimx64.efi` output names a Microsoft Corporation UEFI CA generation; `HKLM\SYSTEM\SecureBoot\ShimCA` matches; `bash scripts/sign-efi.sh build/BOOTX64.EFI` logs `[shim] signed-by: ...`; the build-time warning fires when the deprecated CA date threshold is crossed. Test on: QEMU WHPX (signature path), QEMU TCG, VirtualBox, bare metal (real DB rotation).

> **Test runner:** `bash scripts/test-tooling.sh` | 316/316 PASS (this section adds 6 sub-tests under the `[shim_ca_detection]` block)

> **Notes:**
> - What shipped: `scripts/sign-efi.sh` post-sign block (graduated 2011-CA deprecation policy with named-constant date thresholds), `scripts/extract-shim-ca.sh` (build-time `sbverify --list` extractor writing `build/generated/shim_ca.h`), `src/kernel/uefi_runtime.c` ShimCA registry surface (reads SHIM_CA_YEAR), `docs/guides/secure-boot-keys.md` "MS UEFI CA Lifecycle" section, 4 sub-tests in `scripts/test-tooling.sh` `shim_ca_detection` block.
> - How it integrates: `scripts/build.sh` calls `extract-shim-ca.sh` before kernel compile; the generated header lives at `build/generated/shim_ca.h` (added to `Makefile` `-I` path via new `GENERATED` var). At sign time, the post-sign block enforces graduated policy: pre-2026-05-01 silent / 2026-05-01..2026-06-30 WARN / post-2026-06-30 FAIL exit 1 BEFORE the mv that promotes the signed binary.
> - Downstream: audit tools reading `HKLM\SYSTEM\SecureBoot\ShimCA` see the actual CA generation of the bundled shim (currently 2011, SHA-bound at gen time); when MS publishes a 2023-CA-signed shim, drop it in `shim/shimx64.efi` and rebuild -- the registry value updates automatically.
> - Canonical doc: [`docs/guides/secure-boot-keys.md`](../../docs/guides/secure-boot-keys.md) "MS UEFI CA Lifecycle".
> - Scope boundary: §6 owns the actual signing pipeline; §12 is doctrine + tracking + audit surface layered on top. Pinning the 2023-CA-signed shim binary is `[/]` -- waiting on MS publication.

> **Verified:** 2026-04-29 | commit `69d72fce` | 5/6 items, 1 deferred [/] | build OK | tests 316/316 PASS
> **Quality reviewed:** 2026-04-29 | Codex 6x (design + adversarial + consistency + perf + 2x re-adversarial) | 3H+7M fixed | scope: kernel-code-quality (uefi_runtime.c ShimCA write) + boot-code-quality (sign-efi.sh gate ordering + sbverify rc capture)
> **Scope cleanup:** 2026-05-01 | the deferred [/] "pin 2023-CA-signed shim binary" item was a vendor-watch waiting on Microsoft to publish the 2023-CA-signed `shimx64.efi`, NOT engineering work. Removed from the item list; replaced with the explicit Vendor watch callout above. All engineering response is shipped: `sign-efi.sh` graduated policy fires automatically at 2026-05-01 (WARN) and 2026-06-30 (FAIL); `extract-shim-ca.sh` rebuilds the registry surface from whatever shim is pinned. IO row flipped [/] -> [x].

> [!IMPORTANT]
> **Vendor watch (active 2026-05-01..):** drop a Microsoft-published 2023-CA-signed `shimx64.efi` into `shim/`, bump `shim/SHA256SUMS`, rebuild. The existing pipeline (`scripts/extract-shim-ca.sh` -> `build/generated/shim_ca.h` -> `SHIM_CA_YEAR` -> `HKLM\SYSTEM\SecureBoot\ShimCA` registry surface, `scripts/sign-efi.sh` `[shim] signed-by:` log) handles the rest with no code change. Until Microsoft ships, the graduated deprecation policy in `sign-efi.sh` carries the responsibility -- pre-2026-05-01 silent, 2026-05-01..2026-06-30 WARN, post-2026-06-30 FAIL exit 1 (sign pipeline aborts). Verify Microsoft availability via [Shim release tracker](https://github.com/rhboot/shim/releases) (Microsoft re-signs published shim releases) and Microsoft Learn "Windows UEFI CA 2023".

---

## 13. EFI System Partition Integrity Check

The bootloader currently trusts that UEFI launched it from a valid ESP and proceeds to load `kernel.exe` + `boot.conf` without re-verifying. On a corrupted or tampered ESP, the bootloader silently loads whatever bytes it finds. Win11 BootMgr does basic ESP sanity (FAT32 + correct partition GUID); Linux's `efibootmgr` exposes the ESP UUID. Impossible OS should add a small pre-load sanity gate that catches obvious corruption / wrong-partition cases before kernel launch.

- [x] [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c): `esp_integrity_check()` runs after the BlockIO probe and before `parse_boot_conf`. `esp_check_gpt_type_guid()` finds the parent (whole-disk) BlockIO handle via device-path truncation + `LocateDevicePath`, reads the GPT header at LBA 1, validates signature + header_size + size_of_partition_entry + num_partition_entries + partition_entry_lba range + partition_number range + table-byte overflow BEFORE alloc/read/index (Codex design review F2 hostile-field treatment), reads the single LBA containing our entry, and `boot_fatal(BOOT_ERR_ESP_TYPE_GUID, ...)`s on type-GUID mismatch. Type GUID `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` per UEFI 2.10 Appendix A.2.
- [x] `esp_check_fat_bpb()` reads LBA 0 of the partition (existing BlockIO from boot_device probe), validates 0x55AA boot signature, then matches `BS_FilSysType` at offset 0x52 ("FAT32   ") OR offset 0x36 ("FAT16   ") with partition size < 16 MiB toleration (single WARN line). `boot_fatal(BOOT_ERR_ESP_BPB, ...)` on unrecognized BPB or non-tiny FAT16. Sets `esp_filesystem_type` (0=unknown, 1=FAT16, 2=FAT32).
- [x] `esp_check_required_files()` opens SimpleFS on the boot device and probes `\EFI\BOOT\BOOTX64.EFI` (required) plus the existing 3-path kernel.exe fallback (any one suffices). All missing files batched into one `boot_fatal(BOOT_ERR_ESP_MISSING_FILES, ...)`. `\boot.conf` probed for diagnostic-only logging -- NOT in fatal set because `parse_boot_conf` already supports default-config when missing (Codex design review F3; original draft had boot.conf in the required set, which would have changed boot semantics).
- [x] `boot_info` v11 ABI bump adds `esp_size_mb` (uint32) + `esp_filesystem_type` (uint8) + `esp_type_guid_valid` (uint8) at struct tail; mirrored in [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h) and the `tools/boot-info-manifest/dump-fields.inc` manifest. `boot_device_populate_registry()` in [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c) seeds `HKLM\HARDWARE\BOOT\ESP\{Uuid, SizeMB, FilesystemType, TypeGuidValid}`.
- [x] UKI invocation fast-skips identity validation per Codex design review F1 -- the trust anchor for UKI is the signed PE image itself, so the ESP identity is not load-bearing. `esp_size_mb` is still surfaced from BlockIO so post-boot tools see a non-zero size; `esp_type_guid_valid` stays 0 to indicate "not checked".
- [x] Commit: `"boot: ESP integrity check (GPT type GUID + FAT32 BPB + required-files batch)"`

> [!NOTE]
> ESP cryptographic verification (signed manifest) is out of scope for this section; the trust anchor for the legacy split path is Secure Boot signature on `BOOTX64.EFI` itself. UKI (§11) is the path that closes whole-chain signing. This section is corruption / wrong-partition detection, not adversary defense.

**Test checkpoint:** Booting from a freshly-formatted ESP (FAT32, correct GUID) passes silently; flipping the partition type GUID via `gdisk` triggers the `boot_fatal("ESP type GUID mismatch")` path; deleting `kernel.exe` from the ESP triggers a single batched error reporting all three required-file checks. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) -- 3 new ESP suites in `test_uefi_boot.c` (`test_esp_integrity_v11_abi`, `test_esp_size_mb_consistency`, `test_esp_registry_uuid_format`) | 17 boot suites total, 0 failures

> **Notes:**
> - What shipped: 5 new static helpers in [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) (`esp_find_parent_disk`, `esp_read_harddrive_node`, `esp_check_gpt_type_guid`, `esp_check_fat_bpb`, `esp_check_required_files`, `esp_integrity_check`) wiring three integrity gates plus `boot_info` v11 ABI extension (esp_size_mb / esp_filesystem_type / esp_type_guid_valid).
> - How it integrates: `esp_integrity_check()` invoked from `efi_main` after BlockIO probe and before `parse_boot_conf`; UKI fast-skip honored; non-GPT WARN-skip; corruption / wrong-partition cases halt via `boot_fatal()` with a specific `BOOT_ERR_ESP_*` code so the BSOD identifies which gate failed; POST16 0xB096-0xB099 + 0xB09C bracket the gate.
> - Downstream effects: `HKLM\HARDWARE\BOOT\ESP\{Uuid,SizeMB,FilesystemType,TypeGuidValid}` registry seed feeds the firmware-table catalog ([`01-boot-platform/TODO-04-firmware-table-platform-inventory.md`](TODO-04-firmware-table-platform-inventory.md) §1); Codex 5x review adoptions (design + adversarial + adversarial-impl + re-adversarial + consistency, 9 findings) in commit `fb0c6520`.
> - Canonical doc: [`docs/boot/boot-info-fields.md`](../../docs/boot/boot-info-fields.md) "EFI System Partition integrity" subsection.
> - Scope boundary: §13 owns ESP integrity gating; UKI whole-chain Secure Boot signing is owned by §11; cryptographic ESP manifest verification is out of scope for this section; the firmware-table-platform-inventory consumer of `HKLM\HARDWARE\BOOT\ESP` lives in TODO-04 §1.

> **Verified:** 2026-04-29 | commit `fb0c6520` | 6/6 items | build OK | smoke PASS (KVM 2.32s)
> **Quality reviewed:** 2026-04-29 | Codex 6x (design + adversarial + adversarial-impl + re-adversarial + consistency + perf) | 0H+9M fixed | scope: boot-code-quality

---

## 14. Win32 Firmware Variable + Table Surface

§2 wired `NtQuerySystemEnvironmentValueEx` / `NtSetSystemEnvironmentValueEx` into the SSDT, but `kernel32.dll` does not export the corresponding Win32-named functions (`GetFirmwareEnvironmentVariableA/W`, `SetFirmwareEnvironmentVariableA/W`, `GetSystemFirmwareTable`, `EnumSystemFirmwareTables`). Win11 + Linux WINE applications that query firmware variables or SMBIOS tables fail to resolve these symbols at load time. The native NT API is reachable; the Win32 facade is not. This is purely a wiring gap -- the underlying syscalls exist; the export table needs new entries.

- [x] Add `GetFirmwareEnvironmentVariableA/W` and `SetFirmwareEnvironmentVariableA/W` to `s_kernel32_exports[]` in [`src/kernel/pe.c`](../../src/kernel/pe.c), proxying to the existing `SSDT_NtQuerySystemEnvironmentValueEx` / `SSDT_NtSetSystemEnvironmentValueEx` entries. Per the existing CreateFileA/W pattern, this is a name->SSDT-slot reservation; ANSI/Wide conversion + GUID-string parsing live in the user-mode kernel32 trampoline (Win32 API surface TODO Console & Process API, not yet shipped).
- [x] Add `GetSystemFirmwareTable` and `EnumSystemFirmwareTables` to `s_kernel32_exports[]` mapped to `SSDT_NtQuerySystemInformation`. Implemented `SystemFirmwareTableInformation` (info class 76) in [`src/kernel/nt/nt_syscall.c`](../../src/kernel/nt/nt_syscall.c) with `'ACPI'`, `'RSMB'`, `'FIRM'` providers; `'FIRM'` returns `STATUS_NOT_FOUND` (UEFI-only). Added `acpi_enumerate_signatures` / `acpi_get_raw_table` (validated root + per-child checksum + length bounds, 1024 entry cap) in [`src/kernel/acpi.c`](../../src/kernel/acpi.c) and `smbios_get_raw_table` (16 MiB cap) in [`src/kernel/smbios.c`](../../src/kernel/smbios.c).
- [x] User-mode trampoline (ANSI/Wide name conversion, GUID-string parsing, `SE_SYSTEM_ENVIRONMENT_NAME` privilege gating) is filed in [`10-platform-services/TODO-08-win32-api-surface.md`](../10-platform-services/TODO-08-win32-api-surface.md) Console & Process API as a concrete `[ ]` item per Codex design F1; the kernel-side reservation here unblocks that TODO without taking on its scope.
- [x] Surface QueryVariableInfo() to `HKLM\SYSTEM\SecureBoot\Vars\{VarsValid, MaxStorageSize, RemainingSize, MaxVariableSize, Attributes}` via `uefi_runtime_populate_vars_registry()` called from `registry_populate_defaults()`. Sizes are REG_QWORD (UEFI spec is UINT64). Per Codex design F3, mirrors the SecureBoot State validity contract: VarsValid (DWORD 0/1) is always written; the size fields exist ONLY when VarsValid=1 so consumers can distinguish "unavailable" from "real zero quota".
- [x] Commit: `"kernel: kernel32 exports for Win32 firmware variable + table APIs; QueryVariableInfo registry mirror"` (`194e6012`)

> The "UEFI variable-store health monitor" follow-up (gap-audit 2026-05-01 M1) was migrated out of §14 (2026-05-01) because its implementation depends on a kernel periodic-worker pattern that does not yet exist. Now owned by [`02-kernel-core/TODO-07 §18`](../02-kernel-core/TODO-07-irql-model-dpcs.md#18-system-worker-thread-pool-long-period-periodic-callbacks) "System worker thread pool" as Consumer wiring 2 (alongside the SecureBoot drift periodic tick). The S3-resume re-prime piece is filed reciprocally in [`02-kernel-core/TODO-26 §3`](../02-kernel-core/TODO-26-power-management.md#3-s3-suspend-to-ram). §14 here stays scoped to the boot-time registry surface only.

> [!NOTE]
> User-mode kernel32 trampolines (ANSI/Wide conversion, EFI_GUID string parsing, SE_SYSTEM_ENVIRONMENT_NAME privilege gating) are owned by [`10-platform-services/TODO-08-win32-api-surface.md`](../10-platform-services/TODO-08-win32-api-surface.md) Console & Process API. The kernel-side export entries reserve the SSDT slots; first user-mode calls require that trampoline TODO to ship before the symbols become functional. Codex design review F1 (2026-04-29) verified this is the same architectural posture as the existing `CreateFileA/W -> SSDT_NtCreateFile` mapping.

**Test checkpoint:** A Win32 .exe linked against the shipping kernel32 import library can call `GetFirmwareEnvironmentVariableA("SecureBoot", L"{8be4df61-...}", buf, sizeof(buf))` and receive the same byte the SSDT path returns; `GetSystemFirmwareTable('RSMB', 0, buf, sizeof(buf))` returns the SMBIOS table cached at boot; `HKLM\SYSTEM\SecureBoot\Vars\MaxStorageSize` is non-zero. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) -- 3 new suites in `test_uefi_boot.c` (`test_kernel32_exports_sorted`, `test_vars_registry_validity_contract`, `test_nt_query_system_information_acpi_enum`) | 20 boot suites total, 0 failures

> **Notes:**
> - What shipped: 6 new entries in `s_kernel32_exports[]` (pe.c) reserving SSDT slots for the Win32 firmware variable + table APIs; `SystemFirmwareTableInformation` (info class 76) handler in `nt_syscall.c` with ACPI/RSMB/FIRM providers and `Get`/`Enumerate` actions; raw-table accessors `smbios_get_raw_table` + `acpi_enumerate_signatures` + `acpi_get_raw_table` with hostile-field validation; `uefi_runtime_populate_vars_registry()` mirroring the SecureBoot State validity contract; 3 new unit tests.
> - How it integrates: `s_kernel32_exports[]` consumed by the existing `pe_resolve_export` import resolver; `NtQuerySystemInformation(SystemFirmwareTableInformation)` reachable via the SSDT from the kernel32 trampoline (when the user-mode TODO ships); `uefi_runtime_populate_vars_registry()` invoked from `registry_populate_defaults()` Phase 2 init after `uefi_secureboot_populate_registry()`.
> - Downstream effects: unblocks the user-mode kernel32 firmware trampolines TODO in [`10-platform-services/TODO-08-win32-api-surface.md`](../10-platform-services/TODO-08-win32-api-surface.md) Console & Process API; Codex 6x review adoptions (design + adversarial-impl + 2x re-adversarial finding cycles, 7 findings) per the round-by-round breakdown in the commit message.
> - Canonical doc: `kernel32!GetFirmwareEnvironmentVariable` + `GetSystemFirmwareTable` MSDN docs (Win32 SDK winternl.h `SYSTEM_FIRMWARE_TABLE_INFORMATION` is the kernel surface).
> - Scope boundary: §14 owns the kernel-side export reservation + the SSDT handler implementation + the registry quota mirror; the user-mode kernel32 trampoline (ANSI/Wide + GUID parsing + privilege gating) is owned by the Win32 API surface TODO; the SE_SYSTEM_ENVIRONMENT_NAME privilege table is owned by the privilege/token TODO.

> **Verified:** 2026-04-29 | commit `194e6012` | 5/5 items | build OK | tests 627/627 PASS
> **Accepted:** [H] kernel32 firmware exports map to native SSDT slots without ABI translation -> XREF: 10-platform-services/TODO-08-win32-api-surface.md §2 (item: "Firmware variable trampolines for kernel32 exports reserved in src/kernel/pe.c s_kernel32_exports[]" at line 131 -- ANSI/Wide + EFI_GUID parsing + SystemFirmwareTableInformation packing + privilege gating)
> **Quality reviewed:** 2026-04-29 | Codex 7x (design + adversarial-impl + 2x re-adversarial + adversarial + consistency + perf) | 2H+1M+1L fixed, 1H accepted-XREF | scope: kernel-code-quality
> **Scope migration:** 2026-05-01 | UEFI variable-store health monitor follow-up (was a §14 deferred [M] from gap-audit 2026-05-01) migrated to [`02-kernel-core/TODO-07 §18`](../02-kernel-core/TODO-07-irql-model-dpcs.md#18-system-worker-thread-pool-long-period-periodic-callbacks) "System worker thread pool" because the 60-second-tick caller pattern is missing kernel-wide and is a generic-infrastructure concern, not a UEFI concern. §14 IO row flipped [/] -> [x] because remaining items are all [x] after migration.

---

## 15. Post-boot SecureBoot Revalidation

Detect firmware tampering, NVRAM corruption, or a physical attacker that modifies SecureBoot state while the OS is sleeping or running. Owns the post-boot drift-detection API and its two consumer paths (S3/S4 resume + low-frequency periodic). §5 captures the boot-time state baseline; this section consumes it.

**Files:** `src/kernel/uefi_runtime.c` (drift API shipped here), `src/kernel/test/test_uefi_boot.c` (test).

> [!NOTE]
> The drift API itself is already shipped: `uefi_secureboot_snapshot()` freezes the boot-time state vector at the end of `uefi_secureboot_init()`; `uefi_secureboot_refresh()` (intended call from S3 resume) and `uefi_secureboot_revalidate_tick()` (intended call from periodic worker) both delegate to a shared `uefi_secureboot_do_revalidate()` helper that re-reads SecureBoot/SetupMode/AuditMode/DeployedMode/PK/KEK from firmware OUTSIDE `s_sb_lock` (firmware reads acquire the sleepable `s_rt_mutex`; mutex-under-spinlock would deadlock), publishes to the legacy `s_sb_*` statics only when EVERY field reads with definitive status (a transient firmware hiccup must not flip a previously-ENABLED system to "disabled"), compares against the snapshot, and on first observed mismatch emits `klog(LOG_FATAL, "SecureBoot", "DRIFT detected: ...")` and writes `HKLM\SYSTEM\SecureBoot\Drift = 1`. Drift detection (`s_sb_drift_detected`) and registry publication (`s_sb_drift_published`) are tracked separately so a transient registry write failure does not permanently suppress the sentinel. `uefi_secureboot_drift_detected()` exposes the sticky flag for callers (e.g. tray UI in [TODO-11 §4](../08-graphics-ui/TODO-11-startmenu-tray-notifications.md#4-system-tray-icons-sonnet) and lockdown enforcement in [TODO-27 §5](TODO-27-uefi-advanced.md#5-secure-boot-extended-state-and-enforcement-policy)). Tri-state read helpers (`read_global_byte_status`, `global_var_exists_status`) distinguish UEFI_NOT_FOUND (definitive "not enrolled") from indeterminate firmware errors so PK/KEK presence can not flip 1->0 on a transient hiccup.

- [x] Drift-detection API (`uefi_secureboot_snapshot` / `_refresh` / `_revalidate_tick` / `_drift_detected`) shipped in `src/kernel/uefi_runtime.c:1186-1486` -- snapshot captured at end of `uefi_secureboot_init()`; do_revalidate helper shared by both consumer entry points; tri-state read helpers; sticky drift + sticky-publish-with-retry.
- [x] Test `test_secureboot_drift_detection` (TEST_CAT_BOOT, in `test_uefi_boot.c`) asserts revalidate_tick + refresh return 0 on an untampered system and the drift flag stays clear.
- [ ] **Wire `uefi_secureboot_revalidate_tick()` into the periodic worker pool** (blocked on the kworker pool; no parking timed-sleep exists yet) -> XREF: `02-kernel-core/TODO-07 §18` (item: "Consumer wiring 1 ... `uefi_secureboot_revalidate_tick`").
- [ ] **Wire `uefi_secureboot_refresh()` into the S3 resume path** -> XREF: [`02-kernel-core/TODO-26-power-management.md §3`](../02-kernel-core/TODO-26-power-management.md#3-s3-suspend-to-ram) (S3 suspend-to-RAM, Implementation Order row 3, currently `[ ]`): the resume handler must call `uefi_secureboot_refresh()` immediately after re-enabling runtime services and `pm_notify_resume()` and before re-entering userspace, so any tampering during sleep is detected before the user types a password. Reciprocal back-reference already filed in TODO-26 §3 prose.
- [ ] Test extension when consumer wiring lands: simulate tampered live state (PK absent vs snapshot.pk_present=1) and assert `revalidate_tick()` returns 1, klog FATAL fires once, registry `Drift=1`, and the sticky flag stays set on subsequent calls. Currently blocked by lack of a UEFI variable mock layer; revisit when SecureBoot test infrastructure adds `uefi_get_variable` interception.
- [ ] Commit on each wiring item ship: `"kernel/secureboot: wire <consumer> -> uefi_secureboot_<entry>()"`

**Test checkpoint:** `test_secureboot_drift_detection` passes today (no tampering -> no drift). After consumer wiring lands: a deliberately tampered fixture must trigger LOG_FATAL once and set `HKLM\SYSTEM\SecureBoot\Drift=1`. Test on: QEMU TCG (variable read failure simulation), bare metal (real S3 resume).

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot, TEST_CAT_BOOT) | test_secureboot_drift_detection (1 of N drift-related tests; rest gated on consumer wiring + UEFI variable mocks)

> **Notes:**
> - **What shipped:** the drift-detection API (`uefi_secureboot_snapshot` / `_refresh` / `_revalidate_tick` / `_drift_detected`) in `src/kernel/uefi_runtime.c` + `test_secureboot_drift_detection` (TEST_CAT_BOOT), landed 2026-05-01 `35be4b90`.
> - **How it integrates:** snapshot frozen at end of `uefi_secureboot_init()`; revalidate re-reads firmware outside `s_sb_lock` via the sleepable `s_rt_mutex`, sets a sticky drift flag + `HKLM\SYSTEM\SecureBoot\Drift=1`.
> - **Deferred consumer wiring:** the periodic caller is blocked on the kworker pool (a naive kthread busy-`hlt`-polls; `sleep_ms()` does not park); S3 refresh blocked on S3. Owned by `TODO-07 §18` + `TODO-26 §3`.
> - **Canonical doc:** the §15 NOTE callout above; the worker-pool primitive is `02-kernel-core/TODO-07 §18`.
> - **Scope boundary:** §5 owns the boot-time baseline; TODO-07 §18 owns the worker-pool primitive + its parking-sleep prerequisite; TODO-26 §3 owns S3 resume; the tampered-state test is owned here, blocked on a UEFI-variable mock layer.

> **Verified:** 2026-06-13 | drift API + test shipped 2026-05-01 `35be4b90` | 2/5 items | build OK | scope: drift-detection API only -- consumer wiring + tampered-state test deferred (no new code this pass)
> **Deferred:** [M] periodic-worker wiring -> XREF: 02-kernel-core/TODO-07 §18 (item: "Consumer wiring 1 ... register `uefi_secureboot_revalidate_tick`"); S3-resume refresh -> XREF: 02-kernel-core/TODO-26 §3 (item: "S3: Suspend to RAM" at line 160) + TODO-07 §18 Consumer wiring 3; tampered-state test blocked on a UEFI-variable mock layer (test-infra gap, no concrete owner)

---

## 16. Signed `.initrd` / Recovery / Module PE Sections in UKI

Promoted from §11 deferred [H] (gap-audit 2026-05-01). The Unified Kernel Image artifact built in §11 today bundles `.osrel` + `.cmdline` + `.linux` only; the `.initrd` PE section is reserved but never populated. A boot path that loads `initrd=`, kernel modules, or recovery payloads from disk reads those bytes OUTSIDE the UKI signature, undermining the whole-chain Secure Boot claim §11 establishes. This section closes that gap by extending the UKI pack step to embed `.initrd` / `.recovery` / `.modules` payloads when present, and by hardening the bootloader UKI fast path to consume only those signed sections (refusing any out-of-UKI initrd/module override under `BOOT_FLAG_INVOKED_VIA_UKI`).

**Files:** [`scripts/build.sh`](../../scripts/build.sh) (UKI pack step extension), [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) (`detect_uki_sections()` + `parse_boot_conf()`), [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) and [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h) (optional new payload pointers / capability bits), [`docs/guides/secure-boot-keys.md`](../../docs/guides/secure-boot-keys.md) (deterministic ordering), [`scripts/debug/kernel/run-secureboot.bat`](../../scripts/debug/kernel/run-secureboot.bat) (new `uki_initrd_signed` case), [`src/kernel/test/test_uefi_boot.c`](../../src/kernel/test/test_uefi_boot.c) (new assertions).

> [!IMPORTANT]
> **Whole-chain signature is the bar.** Once any payload (initrd, modules, recovery image) is loadable from disk while the UKI flag is set, the firmware-Secure-Boot claim is broken: a tamperer modifies the disk file, the bootloader passes it to the kernel without verification, and the chain of trust ends at `BOOTX64.UKI.efi`. The §16 contract is "if invoked-via-UKI, ALL load-bearing payloads come from inside the signed PE; any disk-side override is a hard reject."

- [x] **Build pipeline:** [`scripts/build.sh`](../../scripts/build.sh) UKI pack step extended to conditionally embed `.initrd` from `build/uki-payloads/initrd.img`, `.recovery` from `build/uki-payloads/recovery.img`, `.modules` from `build/uki-payloads/modules.cpio` when present. Each `--add-section` is appended only on file existence (back-compat with kernel-only UKIs).
- [x] **Deterministic section ordering** pinned in [`docs/guides/secure-boot-keys.md`](../../docs/guides/secure-boot-keys.md) "Unified Kernel Image (UKI)" subsection: `.osrel -> .cmdline -> .linux -> .initrd -> .recovery -> .modules`. Argument order to `llvm-objcopy --add-section` is load-bearing for PCR measurement reproducibility ([TODO-13](TODO-13-tpm-measured-boot-attestation.md) measured-boot consumes the list in this order).
- [x] **Bootloader walk:** [`detect_uki_sections()`](../../src/boot/uefi/bootx64.c) extended to record `.initrd` / `.recovery` / `.modules` PE sections into new `g_uki_initrd_*` / `g_uki_recovery_*` / `g_uki_modules_*` globals; `reset_uki_sections()` zero-initializes all to handle EDK2 0xAF poison. Pre-EBS, `uki_copy_payloads_to_loader_data()` calls `gBS->AllocatePages(EfiLoaderData, ...)` with a 256-MiB-per-payload sanity cap and copies each section into kernel-physical pages so the addresses survive ExitBootServices. Allocation failures invoke `boot_fatal(BOOT_ERR_UKI_PAYLOAD, ...)` rather than silently dropping the payload.
- [x] **boot_info publication (v14 ABI bump):** `BOOT_INFO_VERSION` 13 -> 14 in BOTH [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) and [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h); 6 new `uint64` fields appended after `loader_identity`: `uki_initrd_addr` / `uki_initrd_size` / `uki_recovery_addr` / `uki_recovery_size` / `uki_modules_addr` / `uki_modules_size`. Per-field offset asserts at 23824 / 23832 / 23840 / 23848 / 23856 / 23864 on both kernel header and mirror. Manifest entries added in `tools/boot-info-manifest/dump-fields.inc`; doc-coverage allow-list extended in `tools/boot-info-manifest/check-doc-coverage.py`. Field rows + new "UKI signed-payload addresses (v14)" section added to `docs/boot/boot-info-fields.md`; v14 changelog entry added to `docs/boot/boot-protocol-changelog.md`.
- [x] **Cmdline override rejection:** TWO-LAYER defense. (1) static-inline `uki_find_disk_override_token()` in [`include/boot/uki_cmdline_check.h`](../../include/boot/uki_cmdline_check.h) does literal-byte scanning for `initrd=` / `module=` / `recovery_image=` BEFORE the parse loop runs. (2) Authoritative gate inside `parse_boot_conf()` parse_loop checks the NORMALIZED key (after whitespace stripping) so a UKI cmdline like `mod ule = foo` cannot bypass detection. Token names match `parse_conf_kv` parser keys exactly (`module=` singular, `recovery_image=` full). Both layers call `boot_fatal(BOOT_ERR_UKI_DISK_OVERRIDE, ...)` under `g_uki_kernel_ptr` non-NULL; split-path boot keeps existing semantics. Codex 4-round review caught: round-1 H1 (deferred smoke runner), round-2 M1 (stale ABI docs), round-2 H1 (wrong token names: `modules=` vs `module=`, `recovery=` vs `recovery_image=`), round-3 H1 (literal helper missed whitespace-stripped form -- added authoritative gate after key normalization), round-4 approved.
- [x] **Documentation:** [`docs/guides/secure-boot-keys.md`](../../docs/guides/secure-boot-keys.md) "Unified Kernel Image (UKI)" subsection extended with: 7-row section-ordering table covering optional payloads, payload provenance contract (anything in `build/uki-payloads/` is signed by the same `MOK_KEY` as the PE; out-of-tree payloads must not be staged there), and the cmdline rejection rule with parser-key naming.
- [/] **Smoke + unit test coverage:** Unit tests shipped in [`src/kernel/test/test_uefi_boot.c`](../../src/kernel/test/test_uefi_boot.c) (3 new assertions in `TEST_CAT_BOOT`): `test_uki_initrd_section_size_consistent` (boot_info publication invariant: addr non-zero implies size non-zero), `test_boot_flag_uki_implies_no_disk_initrd` (split-path boots leave the v14 fields zero), `test_uki_cmdline_rejects_initrd_token` (15 assertions covering positive matches for `initrd=` / `module=` / `recovery_image=` at offset 0 / after space / tab / newline / CR; negative matches for `noinitrd=` / `modules=` / `recovery=` / truncated keys / NULL / zero-length safety). Smoke runner extension in [`scripts/debug/kernel/run-secureboot.bat`](../../scripts/debug/kernel/run-secureboot.bat) is doc-block scaffolding only -- the planned `uki_initrd_signed` + `uki_initrd_disk_override_rejected` cases need a headless serial-to-file harness; tracked as a `[ ]` follow-up below.
- [x] **Back-compat:** build without `build/uki-payloads/initrd.img` -> UKI ships unchanged, `.initrd` section absent, bootloader globals stay NULL, kernel boots as today. New infrastructure is opt-in by payload presence; existing CI / dev builds need no changes.
- [x] **Split-path payload provenance hardening** (`bootx64.c`): `locate_boot_fs()` is HandleProtocol-only; `load_kernel()` non-boot fallback fails closed when payloads are staged. Validation via the smoke harness below.
- [ ] **Smoke harness for UKI signed-payload cases** (Codex round-1 H1 follow-up): implement `scripts/debug/kernel/run-secureboot.bat` `uki_initrd_signed` case (stage synthetic `build/uki-payloads/initrd.img`, rebuild, boot UKI artifact directly, capture serial-stdio output, assert `[BOOT] UKI: .initrd embedded size=4096 bytes` line) AND `uki_initrd_disk_override_rejected` case (UKI with `module=disk.eif` / `initrd=disk.img` / `recovery_image=disk.img` in `.cmdline`, assert `[FATAL] UKI mode rejects out-of-UKI <key>= override` and boot_fatal abort before kernel handoff). Both cases require a new headless serial-to-file QEMU invocation pattern with grep-based assertions; the current bat is GUI-launcher-only. Doc-block scaffolding documenting the cases is in place.
- [x] Commit: `"boot: signed .initrd/.recovery/.modules PE sections in UKI -- whole-chain signature for all load-bearing payloads"`

**Test checkpoint:** `bash scripts/build.sh` with `build/uki-payloads/initrd.img` present produces a UKI containing the `.initrd` section (verifiable via `llvm-objdump -h build/tools/BOOTX64.UKI.efi | grep .initrd`); without the payload, the UKI ships unchanged and boots as today. Boot trace shows `[UKI] .initrd section: addr=0x... size=...` when the payload is present. Disk-side `initrd=...` injection under UKI mode triggers `boot_halt` with the rejection message before the kernel is reached. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal (UEFI 2.7+ direct-firmware-invoke of the UKI).

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) + `scripts\debug\kernel\run-secureboot.bat` (cases `uki_initrd_signed` + `uki_initrd_disk_override_rejected`)

> **Notes:**
> - **What shipped:** UKI signed-payload sections (`.initrd`/`.recovery`/`.modules`) with whole-chain signature + cmdline-override rejection + boot_info v14 ABI; plus split-path payload provenance hardening in `bootx64.c`.
> - **Provenance hardening:** `locate_boot_fs()` is HandleProtocol-only (no ambient `LocateProtocol` fallback); `load_kernel()`'s non-boot fallback `boot_fatal`s when `g_staged_payload_count > 0`, so payloads + kernel share one volume.
> - **Downstream:** UKI section order is consumed by TODO-13 measured-boot for PCR reproducibility; the new §17 SBAT section makes the artifacts revocable.
> - **Canonical doc:** `docs/guides/secure-boot-keys.md` (UKI subsection).
> - **Scope boundary:** the headless serial-to-file smoke harness for the two UKI cases is deferred test-infra (open `[ ]` below); UKI addons/profiles are advanced (TODO-27 if ever).

> **Verified:** 2026-06-13 | commit `46eb95bc` | 8/10 items | build OK | smoke PASS (KVM 2.590s)
> **Deferred:** [M] headless serial-to-file smoke harness for `uki_initrd_signed` + `uki_initrd_disk_override_rejected` (and the provenance fail-closed case) -- needs a new QEMU serial-capture pattern -> XREF: 01-boot-platform/TODO-02 §16 (item: "Smoke harness for UKI signed-payload cases" below)
> **Quality reviewed:** 2026-06-13 | Codex 6x (design + adversarial + re-adversarial + consistency + perf) | 0 findings | scope: boot-code-quality

---

## 17. SBAT Revocation Metadata in Boot Artifacts

Promoted from gap-audit 2026-06-13 (Codex parity-floor finding). The Impossible bootloader (`BOOTX64.EFI`) and the Unified Kernel Image (`BOOTX64.UKI.efi`) embed no `.sbat` PE section, so neither artifact is SBAT-revocable: a vulnerable build can only be revoked by coarse dbx hash/cert revocation, not the fine-grained SBAT generation mechanism shim and systemd UKIs use. A SBAT-enforcing shim (§6) compares the loaded image's `.sbat` section against the `SbatLevel` UEFI variable; an image with no `.sbat` is a chain-of-trust gap that a stricter shim can refuse outright. §9 owns the SBAT bump *process* doc; this section owns the actual embedded section in the artifacts. **Files:** `src/boot/uefi/sbat.csv` + `src/boot/uefi/sbat.asm` (new), `src/boot/uefi/Makefile` + `uefi.lds` (link-time `.sbat` embed via `-j .sbat`), root `Makefile` (exclude sbat.asm from the kernel ASM glob), `scripts/build.sh` (build-time content gate), `docs/guides/secure-boot-keys.md` (layout + bump cross-ref).

- [x] Create `src/boot/uefi/sbat.csv`: the upstream `sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md` header row plus an `impossibleos,1,Impossible OS,impossible-os,1,https://impossibleos.co/` component row (generation 1).
- [x] Embed `.sbat` in `BOOTX64.EFI` at link time: `src/boot/uefi/sbat.asm` `incbin`s `sbat.csv` into a `.sbat` section (placed by `uefi.lds`, carried by the ELF->PE `-j .sbat`). Post-hoc objcopy was unviable (content drop / PE corruption / 0 bytes).
- [x] `.sbat` in `BOOTX64.UKI.efi`: the UKI is built from the `BOOTX64.EFI` stub, so it inherits the stub's `.sbat` (no separate add -- re-adding would duplicate the section).
- [x] Pin `.sbat` in the `docs/guides/secure-boot-keys.md` UKI section layout + cross-reference the SBAT-bump checklist (a CVE bump increments the `impossibleos` generation in `sbat.csv`).
- [x] Build-time **content**-validating gate in `scripts/build.sh`: validates `sbat.csv` structure then byte-`cmp`s both artifacts' embedded `.sbat` against the source, hard-failing on any mismatch.
- [x] Commit: `"boot: embed .sbat revocation metadata in BOOTX64.EFI + UKI -- SBAT-revocable boot artifacts"` **Verified 2026-07-31:** done as `900ade7c` "boot: SBAT revocation metadata in boot artifacts" (message drifted from the plan; work + review shipped)

> [!NOTE]
> **No kernel test surface.** A `boot_info` `sbat_generation` field + kernel unit test was dropped (Codex design review 2026-06-13): the kernel cannot enforce shim SBAT revocation after ExitBootServices, and `boot_info` version/size is exact-match fatal, so a test-only ABI field adds a synchronized-reflash failure mode for zero runtime benefit. Verification is the build-time PE content gate above (validates the final artifacts before signing).

**Test checkpoint:** `bash scripts/build.sh` produces `BOOTX64.EFI` and `BOOTX64.UKI.efi` each carrying a `.sbat` section whose bytes equal `src/boot/uefi/sbat.csv`; the build log shows `[sbat] OK: ... (impossibleos gen 1)`. The gate validates the source CSV structurally (exact shim header + one `impossibleos,<positive-gen>` row, 6 fields) then byte-`cmp`s each artifact's `.sbat` (dumped via `objcopy -O binary --only-section=.sbat`) against the source; the build aborts (`=== BUILD FAILED ===`) on any missing/empty/stale section. A SBAT-enforcing shim with `SbatLevel` at or below generation 1 loads the image, above it refuses. Test on: QEMU WHPX, QEMU TCG (OVMF + shim), bare metal (UEFI 2.7+).

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) + `scripts\debug\kernel\run-secureboot.bat`

> **Notes:**
> - **What shipped:** `src/boot/uefi/sbat.csv` (shim SBAT CSV, `impossibleos` gen 1) embedded as a `.sbat` PE section in both `BOOTX64.EFI` and `BOOTX64.UKI.efi`, making the artifacts SBAT-revocable (parity with shim/systemd).
> - **How it integrates:** `src/boot/uefi/sbat.asm` `incbin`s the CSV into a `.sbat` ELF section (placed by `uefi.lds`, carried by the ELF->PE `-j .sbat`); the UKI inherits it from the stub; a `scripts/build.sh` gate validates `sbat.csv` + byte-`cmp`s both artifacts, hard-failing on drift.
> - **Downstream:** the §9 SBAT-bump checklist drives generation increments; a SBAT-enforcing shim can now revoke a vulnerable build by `SbatLevel` instead of coarse dbx.
> - **Canonical doc:** `docs/guides/secure-boot-keys.md` (UKI section layout + SBAT Bump checklist).
> - **Scope boundary:** no `boot_info` field / no kernel test (the kernel cannot enforce shim SBAT after ExitBootServices); verification is build-time only. UKI addons/profiles stay advanced (TODO-27 if ever).

> **Verified:** 2026-06-13 | commit `900ade7c` | 5/5 items | build OK | smoke PASS (KVM 2.160s)
> **Quality reviewed:** 2026-06-13 | Codex 8x (design + adversarial + consistency + perf + re-adversarial) | 4H+6M fixed | scope: boot-code-quality

---

## 18. Build Idempotency: UKI SBAT Survives Incremental Rebuild

The UKI pack step (`scripts/build.sh`) runs `objcopy --add-section ... "$UKI_STUB"` against `build/tools/BOOTX64.EFI` on every build. On a clean build that stub is freshly linked and **unsigned**, so the UKI inherits a clean `.sbat` and the §17 Stage-2 byte-compare gate passes. On an **incremental** build the Makefile does not relink an up-to-date stub, so `build/tools/BOOTX64.EFI` persists in the **signed** state `scripts/sign-efi.sh` left it in (Phase 2b `mv`s the signed temp over the original in place). `objcopy --add-section` on an already-Authenticode-signed PE corrupts the section table, so `objcopy --only-section=.sbat` can no longer dump `.sbat` and the build fails with `[sbat] FATAL: cannot dump .sbat from build/tools/BOOTX64.UKI.efi`. Net effect: **no build can run twice without `clean`** -- `scripts/test.sh` and the post-commit unit-test hook both incremental-build first and fail. Reproduced 2026-06-13: clean build OK, immediate no-op incremental build FAILs at the UKI SBAT gate; `rm -f build/tools/BOOTX64.EFI build/tools/BOOTX64.UKI.efi` then build OK (Makefile relinks an unsigned stub). Pre-existing; surfaced during TODO-07 §18 review. SECURITY-SENSITIVE (Secure Boot signing / image-assembly path). **Resolved 2026-06-21 via approach A** (user-authorized): the incremental build is now idempotent; only the bare-metal SB-chain acceptance remains a deferred human sign-off, per CLAUDE.md Safety Gates. Candidate fixes: **(A)** non-destructive signing -- sign to a distinct path, keep `build/tools/BOOTX64.EFI` unsigned, place the signed artifact on the ESP via the System Disk step (most correct); **(B)** strip the signature from a temp stub copy inside the UKI pack (`sbattach --remove` from `sbsigntools`) and pack from the temp; **(C)** `rm` the stub + UKI before the EFI Boot step so the Makefile always relinks an unsigned stub (smallest, but defeats bootloader incremental caching).

- [x] Fix shape **(A)** non-destructive signing: sign to distinct `BOOTX64.signed.efi` paths, keep the canonical stub unsigned so the incremental `objcopy` is idempotent (chosen over B sbattach-strip / C rm-relink). Rationale in the intro.
- [x] Implemented: `sign-efi.sh` publishes the signed pair (both-or-neither + INT/TERM trap); Makefile `SYSTEM_DISK`, `build-image.sh`, `build-manifest.sh`/`.ps1`, `verify-esp.sh` select the signed artifact when keyed. Same key/cert/SB policy.
- [x] BOTH modes verified: clean + incremental both `=== BUILD OK ===` + `[sbat] OK`; `sbverify` OK on the signed pair; canonical = `No signature table present`; SB smoke 8/8; manifest binds its bootloader hash to the signed artifact.
- [ ] Verify the signed UKI boots + the SB chain validates on bare metal / a Secure-Boot-enrolled VM (human-gated; cannot be validated in WSL TCG/KVM).
- [ ] Unify the signed-artifact selection predicate (`build-image.sh`/`verify-esp.sh` file-existence vs `build-manifest` stamp-staleness -- stale-artifact drift) + regression tests (external `MOK_KEY`, stale-rejection, keyed-signed-missing).
- [x] Commit: `"build: TODO-02 §18 -- UKI SBAT survives incremental rebuild (approach A: distinct signed-output paths)"`

**Test checkpoint:** `bash scripts/build.sh && bash scripts/build.sh` (two builds, no `clean` between) both end `=== BUILD OK ===` with `[sbat] OK: BOOTX64.EFI + UKI carry .sbat`. `scripts/test.sh SUITE=boot` runs without a build failure. On bare metal with Secure Boot enrolled, the incremental-built UKI boots and the firmware accepts the signature. Test on: WSL TCG (build idempotency only); bare metal (SB-chain acceptance).

> **Test runner:** N/A (build-infrastructure fix) | validation: double-build idempotency + `sbverify` in `sign-efi.sh` + bare-metal SB enrollment

> **Notes:**
> - **What shipped:** approach A -- `sign-efi.sh` signs to distinct `BOOTX64.signed.efi`/`UKI.signed.efi` paths, leaving the canonical stub unsigned so the incremental `objcopy` is idempotent; the `rm`-before-build workaround is retired.
> - **How it integrates:** Makefile `SYSTEM_DISK` ships the signed loader when keyed (keyless+shim -> direct boot); `build-image.sh`, `build-manifest.sh`/`.ps1`, `verify-esp.sh` select the signed artifact. Codex 4-finding adoptions in commit msg.
> - **Verified (WSL):** clean + incremental both `BUILD OK` + `[sbat] OK`; `sbverify` OK on the signed pair; canonical unsigned; SB smoke 8/8; manifest binds its bootloader hash to the signed artifact.
> - **Canonical doc:** this section + memory `project-uki-sbat-incremental-build-bug` + `project-secureboot-mok-dev-cert`.
> - **Scope boundary:** bare-metal SB-chain sign-off is human-gated (WSL cannot run it); signing uses the MOK dev cert (production cert is a ~2yr future rotation).

> **Verified:** 2026-06-21 | commit `aa3c20db` | 3/5 items | build OK | SB smoke 8/8
> **Deferred:** [M] bare-metal / Secure-Boot-enrolled-VM SB-chain acceptance (firmware accepts the signed grubx64.efi behind shim) -- cannot be validated in WSL TCG/KVM -> XREF: 01-boot-platform/TODO-02 §18 (item: "Verify the signed UKI boots + the SB chain validates on bare metal / a Secure-Boot-enrolled VM")
> **Deferred:** [M] signed-artifact selection predicate not unified (build-image.sh/verify-esp.sh file-existence vs build-manifest stamp-staleness; stale-artifact drift) + missing signing-state regression tests -> XREF: 01-boot-platform/TODO-02 §18 (item: "Unify the signed-artifact selection predicate")
> **Quality reviewed:** 2026-06-21 | Codex 3x (adversarial, consistency, perf) | 2M deferred (XREF) | scope: N/A (build-infra; not src/boot or src/kernel)

---

## 19. Bootloader `ReadBlocks` IoAlign Compliance

UEFI 2.10 §13.9 (`EFI_BLOCK_IO_PROTOCOL.ReadBlocks`) requires the data buffer to be aligned to `Media->IoAlign`; an under-aligned buffer returns `EFI_INVALID_PARAMETER` or, on some firmware, silently corrupts via an unaligned DMA. The bootloader's block readers -- `esp_check_gpt_type_guid()` / `esp_check_fat_bpb()` (§13) and the A/B metadata READ path (`ab_read_meta_copy`, TODO-21 §3) -- read into plain stack buffers aligned only naturally (1/8/16 bytes), NOT to `Media->IoAlign`. This pre-existing codebase-wide idiom has booted on the tested firmware (which reports `IoAlign <= 1`, so any buffer is legal); it is a latent spec-compliance gap on firmware reporting `IoAlign > 8` (large-sector NVMe, some enterprise RAID HBAs). The A/B WRITE path (`ab_bl_increment_tries`, TODO-21 §4) already uses an aligned bounce buffer; the READ paths should match. Surfaced by Codex during the TODO-21 §4 review. Not an A/B regression (the GPT readers run unaligned on every boot today) -- a general bootloader robustness item.

- [x] `bl_read_blocks_aligned()` in `bootx64.c`: `IoAlign <= 1` fast path reads direct (byte-identical); else aligned bounce + copy. IoAlign sanity bound (4096 cap, power-of-two, `byte_count + align` overflow guard) -> rejected firmware returns `EFI_UNSUPPORTED`; alloc failure -> `EFI_OUT_OF_RESOURCES`; callers stay fail-closed.
- [x] Retrofitted the bare-stack-buffer readers: `esp_check_gpt_type_guid` (GPT header + entry block), `esp_check_fat_bpb` (LBA 0), `ab_read_meta_copy`, + `ab_load_gpt_table` header. The GPT entry-ARRAY read keeps the pool table (8-byte-aligned, satisfies `IoAlign <= 8` direct) + reads block-by-block via the helper only when `IoAlign > 8` -- no second full-size table bounce.
- [/] `IoAlign <= 1` fast path smoke-validated unregressed (ESP GPT type-GUID + FAT BPB + A/B select pass, KVM 2.35s). The `IoAlign > 8` bounce path is not reachable under QEMU (virtual block devices report `IoAlign <= 1`); it mirrors the shipped `ab_bl_increment_tries` aligned path and needs a bare-metal large-sector disk.
- [x] Commit: `"boot: ReadBlocks IoAlign-compliant bounce buffers for GPT/FAT/A-B readers"`

**Test checkpoint:** On firmware reporting `IoAlign <= 1`, behavior is byte-identical (smoke PASS, no regression). On a `IoAlign > 8` device, the GPT/FAT/A-B reads succeed where an unaligned buffer would have returned `EFI_INVALID_PARAMETER`. Test on: QEMU TCG (NVMe 4K-block namespace), bare metal (large-sector disk if available).

> **Test runner:** N/A (pre-EBS UEFI bootloader block I/O -- no kernel test surface) | the `IoAlign <= 1` fast path is smoke-validated unregressed (every retrofitted reader passes); the `IoAlign > 8` bounce path mirrors the shipped `ab_bl_increment_tries` aligned write + needs a bare-metal large-sector disk

> **Notes:**
> - **What shipped:** `bl_read_blocks_aligned()` in `bootx64.c` (IoAlign-aligned bounce read with a 4096/power-of-two/overflow sanity bound) + retrofits of `esp_check_gpt_type_guid`, `esp_check_fat_bpb`, `ab_read_meta_copy`, `ab_load_gpt_table`; `EFI_OUT_OF_RESOURCES` added to `efi.h`.
> - **How it runs:** every bootloader block reader goes through the helper; on `IoAlign <= 1` (all tested firmware) it reads direct -- byte-identical; on `IoAlign > 1` it bounces through an aligned buffer. The large GPT entry-array read avoids a second full-size buffer (pool table is 8-byte-aligned for `IoAlign <= 8`; block-by-block bounce only above that).
> - **Design adoptions:** the IoAlign sanity bound lives inside the helper (rejected firmware -> `EFI_UNSUPPORTED`, callers fail-closed); the GPT table is NOT double-allocated (peak memory matters on the A/B primary+backup path). Evidence in commit message.
> - **Downstream effects:** closes the codebase-wide read-IoAlign gap filed from the TODO-21 §4 review; the A/B WRITE path was already aligned.
> - **Canonical doc:** UEFI 2.10 spec 13.9 (`EFI_BLOCK_IO_PROTOCOL.ReadBlocks` IoAlign requirement) + `bl_read_blocks_aligned` in `bootx64.c`.
> - **Scope boundary:** §19 owns the bootloader read alignment; the A/B write path is TODO-21 §4; ESP integrity gating is §13.

> **Verified:** 2026-06-16 | commit `7292f79e` + pow2-guard fix | 2/3 items | build OK | smoke PASS (KVM 1.99s, every retrofitted reader unregressed on the IoAlign<=1 fast path)
> **Quality reviewed:** 2026-06-16 | Codex 5x (design, adversarial, consistency, perf) | 1H+2M fixed, 0 open | scope: boot-code-quality (re-adversarial skipped: 1-line pow2-guard fix)

---

## 20. Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)

Items moved here VERBATIM from their original, already-stamped sections, where they were unreachable: the triage oracle classifies a stamped section DONE without reading its body, so an item appended after the stamp is invisible to every later pass. Source section noted per group. Cohort context: `todo/overnight-runner-improvements/overnight-runner-improvements-v05.md` item 3.

From the stamped section 12:
- [ ] Make shim absence fail-loud after the 2026-07-01 expiry unpin: Makefile ESP packaging + secureboot smoke must FAIL (not silently direct-boot/skip) without a trusted shim; update §6 claims + docs; re-pin a 2023-CA shim per the vendor watch.

**Test checkpoint:** per moved item; each carries its original acceptance text.

---

## OS Comparison

| ⭐  | Feature               | 🪟 Win11                        | 🐧 Linux                      | 🚀 Impossible OS                                              |
| --- | --------------------- | ------------------------------- | ----------------------------- | ------------------------------------------------------------- |
| 💎  | UEFI runtime post-EBS | ✅ Full RT via hal.dll          | ✅ efi_call wrapper           | ✅ §1 SVAM + 6 RT services                                    |
| 💎  | UEFI variables        | ✅ NtQuery/SetSystemEnvValue    | ✅ efivarfs mount             | ✅ §2 get/set/enum + SSDT wired                               |
| 💎  | GOP resolution        | ✅ Boot mgr + BCD               | ✅ EFIFB + simplefb           | ✅ §3 auto-select best mode                                   |
| 💎  | SMBIOS core           | ✅ WMI Win32_BIOS class         | ✅ sysfs /sys/class/dmi       | ✅ §4 types 0-4 + registry                                    |
| 💎  | Secure Boot shim      | ✅ MS-signed shim + MOK         | ✅ rhboot/shim + MokManager   | ✅ §6 MOK chain + sbsign                                      |
| 💎  | Secure Boot state     | ✅ Registry + msinfo32          | ✅ efivar + mokutil --sb      | ✅ §5 NVRAM + registry State                                  |
| ⭐  | Boot timeline         | ❌ ETW WPA (heavyweight)        | ❌ systemd-analyze (userland) | ✅ §7 per-step JSON + NVRAM                                   |
| 💎  | Atomic serial         | ✅ KdPrint spinlock             | ✅ printk logbuf              | ✅ §8 klog ring + serial                                      |
| 💎  | SBAT shim ops         | ✅ MS Secure Boot program       | ✅ distro shim refresh        | ✅ §9 SBAT checklist doc                                      |
| 💎  | SBAT artifact revoke  | ✅ MS-signed shim SBAT          | ✅ shim + UKI .sbat section   | ✅ §17 BOOTX64 + UKI .sbat                                    |
| 💎  | DB/dbx inventory      | ✅ msinfo32 SB details          | ✅ mokutil --db               | ✅ §9 registry Db/Dbx counts                                  |
| 💎  | EBS retry hardening   | ✅ bootmgr bounded retry        | ✅ efi-stub retry patch       | ✅ §9 4-attempt bounded loop                                  |
| 💎  | Capsule install UX    | ✅ Windows Update stack         | ✅ fwupd + LVFS               | ⬜ TODO-27 §2 (query-only)                                    |
| 💎  | MS UEFI CA lifecycle  | ✅ Windows Update CA rotation   | ⚠️ Distro re-sign timing      | ✅ §12 build-time graduated WARN/FAIL + ShimCA registry       |
| 💎  | ESP integrity check   | ⚠️ BootMgr GUID / FAT32 only    | ⚠️ efibootmgr UUID surface    | ✅ GPT type-GUID + FAT BPB + batched files + UUID/Size mirror |
| 💎  | Win32 firmware vars   | ✅ kernel32 GetFirmwareEnv*     | ⚠️ WINE shim only             | ✅ kernel32 exports + RSMB/ACPI tables + Vars quota mirror    |
| 💎  | Unified Kernel Image  | ❌ N/A (signed bootmgr+winload) | ✅ systemd-boot UKI           | ✅ §11 BOOTX64.UKI.efi + whole-chain Secure Boot signature    |

> **Parity:** 💎 rows match Win11+Linux baseline. **⭐** JSON boot profile is extra vs ETW and userland boot charts. Capsule **apply** path and W^X on RT pages stay in [TODO-27](TODO-27-uefi-advanced.md); kernel already runs read-only `esrt_init()` / `uefi_capsule_init()` / `uefi_crypto_agility_init()` during Phase 1 bring-up.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_uefi_boot()`. Tests must follow CLAUDE.md kernel test rules (no `boot_progress`, `panic`, live `serial_init`, etc.).

- [x] Create `src/kernel/test/test_uefi_boot.c` with 9 suites: RT available, var_get SecureBoot, var_u32 roundtrip (Impossible OS vendor GUID), framebuffer width/height, HiDPI consistency, SMBIOS UUID, Secure Boot state consistency, registry BIOS vendor, SecureBoot DB mirror (DbEntries/DbxEntries match secureboot_get_db_info)
- [x] Register in `test_runner_init()`: `test_register_uefi_boot()` under Boot category
- [x] Commit: `"test: add uefi_boot test suite"` (07ce1ac3)
- [ ] Add §12 test: registry `HKLM\SYSTEM\SecureBoot\ShimCA` is a non-zero DWORD matching the build-host's recorded shim CA generation.
- [x] Add ESP integrity test: 3 suites in `test_uefi_boot.c` covering (1) BOOT_INFO_VERSION >= 11 + esp_filesystem_type/esp_type_guid_valid bit-range, (2) registry SizeMB matches `g_boot_info.esp_size_mb`, (3) `HKLM\HARDWARE\BOOT\ESP\Uuid` is empty (non-GPT) or 36-char canonical GUID with dashes at 8/13/18/23. Tests use TEST_SKIP for non-disk boot path; no live boot calls.
- [x] Add Win32 firmware test: `pe_exports_sorted_check()` validates kernel32+ntdll table sort order survives the new insertions; `HKLM\SYSTEM\SecureBoot\Vars\VarsValid` validity contract (size keys present iff VarsValid=1); `NtQuerySystemInformation(SystemFirmwareTableInformation, ACPI, enumerate)` returns SUCCESS or NOT_FOUND with at least one 4-byte signature on success.
- [ ] Commit: `"test: extend uefi_boot suite with shim CA, ESP probe, kernel32 firmware exports"`
- [x] §17 SBAT: no kernel test surface (the `sbat_generation` boot_info field was dropped per design review). `.sbat` content is validated by the two-stage build-time gate in `scripts/build.sh`.

> **Done:** 32 suites registered via `test_register_uefi_boot()` (originally 9; expanded across §13 ESP-integrity (3) and §14 Win32 firmware (3) plus subsequent sub-assertions). Up to date as of 2026-05-01.

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

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 32 suites in `test_uefi_boot.c`, 0 failures
