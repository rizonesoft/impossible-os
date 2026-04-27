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

> **Verified:** 2026-04-11 -- Codex adversarial review found 2 valid issues (SVAM failure not propagated, descriptor stride mismatch on extended firmware), both fixed. 1 unit test wired (`test_uefi_rt_available`), build passes.
> **Quality reviewed:** 2026-04-11 -- 1 spec violation fixed (SVAM attribute preservation), 2 best practices applied (centralized RT timer masking, table signature+CRC32 validation). Accepted: spinlock->mutex migration (-> 01-boot-platform/TODO-02 §10).

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

> **Verified:** 2026-04-11 -- Codex review found u32 size validation bug (fixed), SSDT pointer probing + privilege gaps accepted out-of-scope (tracked in 02-kernel-core/TODO-23). 2 unit tests wired, build passes.
> **Quality reviewed:** 2026-04-11 -- 2 spec violations fixed (expanded efi_to_ntstatus to 9 codes + unified duplicate, enumerate BUFFER_TOO_SMALL handling), 2 best practices (attrs output on uefi_var_get, unified mapper), 1 dead code removed (uefi_enumerate_variables -> callback), 1 parity gap closed (QueryVariableInfo exposed). Accepted: duplicate GUID (circular include), parameter order (Win32 vs UEFI convention), SSDT probing/privilege (-> 02-kernel-core/TODO-23), spinlock migration (-> 01-boot-platform/TODO-02 §10).

---

## 3. GOP Resolution Auto-Detection

Negotiate the best framebuffer resolution before `ExitBootServices()`.

- [x] `gop_negotiate_mode()`: query all modes, score by resolution, respect `boot.conf` override
- [x] HiDPI flag: `boot_info.hidpi = 1` when width >= 2560
- [x] `gop->SetMode(best_mode)` with fallback to current mode
- [x] Commit: `"boot: GOP resolution auto-detection with HiDPI flag and boot.conf override"`

**Test checkpoint:** Serial shows `[Boot] GOP: {W}x{H} 32bpp (mode N)` with W/H > 0; `boot_info.hidpi` is 1 when width >= 2560 else 0. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Verified:** 2026-04-11 -- Codex found FrameBufferBase==0 crash path after mode 0 retry (fixed: headless fallback). Auto 1080p cap is intentional VBox safety (documented in code). 2 unit tests wired, build passes.
> **Quality reviewed:** 2026-04-11 -- 1 spec violation fixed (FrameBufferSize bounds check before VRAM clear), 3 best practices (Mode/Info NULL guard, pitch validation in mode scoring, mode 0 retry failure logging), 1 simplification (extracted gop_pixel_format_code helper). Accepted: VRAM clear pixel loop (cold path, runs once), double mode enumeration (different purposes).

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

> **Verified:** 2026-04-11 -- Codex found 3 parser safety issues: bounded string scan (smbios_get_string overread), walker truncation check (hdr->length > remaining), Type 17 0x7FFF sentinel without extended field. All fixed. Accepted: none.
> **Quality reviewed:** 2026-04-11 -- 1 spec violation fixed (entry point checksum validation for 3.x and 2.x), 1 best practice (anchor string validation "_SM3_"/"_SM_"), 2 parity gaps closed (Type 4 2-byte core/thread counts at 0x2A/0x2E for SMBIOS 3.0+, Type 17 configured speed at 0x20 preferred over max speed). Accepted: none.

---

## 5. Secure Boot State Detection

Read the UEFI `SecureBoot` variable and expose the state to the kernel.

- [x] `uefi_secureboot_init()`: read `SecureBoot` variable, set `boot_info.secure_boot_enabled`
- [x] Write `HKLM\SYSTEM\SecureBoot\State` = 0 or 1
- [/] Padlock icon in system tray when Secure Boot active -- `g_system_state.secure_boot` flag published; rendering not implemented. -> XREF: `08-graphics-ui/TODO-11-startmenu-tray-notifications.md §4` (System tray icons -- `tray_icon` struct + register/unregister) is the owner; padlock-when-secure-boot-on is a §4 consumer of `g_system_state.secure_boot`.
- [x] Commit: `"kernel: Secure Boot state detection, registry key"`

> [!NOTE]
> Kernel **code signature verification** for loaded images is owned by `02-kernel-core/TODO-10-kernel-security-hardening.md §11` (Enclave and code signing) and cross-notes in `02-kernel-core/TODO-15-security-reference-monitor.md`, not this bootloader TODO.

**Test checkpoint:** `boot_info.secure_boot_enabled` matches UEFI `SecureBoot` variable; registry `HKLM\SYSTEM\SecureBoot\State` is 0 or 1 accordingly; padlock tray icon matches state when shell is running. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Quality reviewed:** 2026-04-11 -- 4 parity gaps closed (SetupMode, DeployedMode, AuditMode, PK/KEK enrollment all written to HKLM\SYSTEM\SecureBoot registry). Accepted: registry path convention (intentionally simpler than Windows CurrentControlSet).

---

## 6. Secure Boot Shim Chain-Loading

Set up MOK key pair, sign `BOOTX64.EFI`, and integrate shim into the build.

- [x] `.gitignore`: `MOK.key`, `*.signed.EFI`
- [x] Key generation documented in `docs/guides/secure-boot-keys.md`
- [x] `scripts/sign-efi.sh` + `sign-efi` Makefile target
- [x] Shim: `shim/shimx64.efi` + `shim/mmx64.efi` committed (SHA256 verified)
- [x] Shim validates `grubx64.efi` via embedded `MOK.cer`; MokManager for first-boot enrollment
- [x] Commit: `"boot: Secure Boot shim chain-loading, MOK key signing pipeline"`

**Test checkpoint:** With `MOK.key` present, signed `BOOTX64.EFI` builds; without keys, signing is skipped silently; first boot can complete MOK enrollment path on real firmware. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Verified:** 2026-04-11 -- all 5 items verified (gitignore, docs, scripts, shim binaries, build integration). Codex found 4 signing/packaging safety issues, all fixed. Accepted: none.
> **Quality reviewed:** 2026-04-11 -- 4 fixes: atomic signing (temp+verify+mv), shim hash verification (SHA256SUMS), partial shim set error, consistent error handling. Accepted: none.

---

## 7. Boot UX Polish

Structured boot profiling and pre-framebuffer error recovery screen.

- [x] Boot profiling: `boot_progress()` at every major event; `boot_timing_write_report()` writes to `X:\Perf\boot-profile.log`
- [x] JSON boot-timeline export (-> XREF: `TODO-11-interrupt-timer-arch.md §3`)
- [x] Pre-framebuffer error screen: `boot_halt(reason)` with inline 8x8 bitmap font
- [x] `boot_splash_status()` integration: live stage text below spinner
- [x] Commit: `"boot: boot-stage instrumentation, pre-framebuffer error recovery screen"`

**Test checkpoint:** `boot_progress()` stages appear in order on serial and optional `X:\Perf\boot-profile.log`; `boot_halt()` shows pre-framebuffer error text when forced; JSON timeline export matches `TODO-11-interrupt-timer-arch.md §3` contract. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Verified:** 2026-04-11 -- Codex found 3 issues: JSON buffer overflow on long step names (fixed: clamped pos + escaped quotes), profile report stack overflow (fixed: bounded step name copy), forbidden boot_progress call in test (fixed: replaced with pure boot_timing_record_step). Accepted: none.
> **Quality reviewed:** 2026-04-11 -- no findings. JSON timeline format exceeds Win11 ETW (binary, requires WPA) and Linux systemd-analyze (text only). Pre-fb error screen, boot_post_write16 I/O safety, and splash integration all industry-compliant. Accepted: none.

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

> **Parity:** 💎 rows match Win11+Linux baseline. **⭐** JSON boot profile is extra vs ETW and userland boot charts. Capsule **apply** path and W^X on RT pages stay in [TODO-27](TODO-27-uefi-advanced.md); kernel already runs read-only `esrt_init()` / `uefi_capsule_init()` / `uefi_crypto_agility_init()` during Phase 1 bring-up.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_uefi_boot()`. Tests must follow CLAUDE.md kernel test rules (no `boot_progress`, `panic`, live `serial_init`, etc.).

- [x] Create `src/kernel/test/test_uefi_boot.c` with 9 suites: RT available, var_get SecureBoot, var_u32 roundtrip (Impossible OS vendor GUID), framebuffer width/height, HiDPI consistency, SMBIOS UUID, Secure Boot state consistency, registry BIOS vendor, SecureBoot DB mirror (DbEntries/DbxEntries match secureboot_get_db_info)
- [x] Register in `test_runner_init()`: `test_register_uefi_boot()` under Boot category
- [x] Commit: `"test: add uefi_boot test suite"` (07ce1ac3)

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
