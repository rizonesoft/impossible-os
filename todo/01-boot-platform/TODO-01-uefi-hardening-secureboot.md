# TODO-01 -- UEFI Bootloader Hardening & Secure Boot

> **Goal:** Harden the UEFI boot path with runtime service preservation, variable access, GOP resolution auto-detection, SMBIOS hardware inventory, Secure Boot shim chain-loading, boot UX polish, and serial log standardization. This is the complete core boot experience -- the OS boots reliably on QEMU, VirtualBox, and bare metal with Secure Boot support.

> [!IMPORTANT]
> **Secure Boot strategy:** Use the rhboot/shim chain-loading approach. MOK key pair (`MOK.key`) is generated locally and MUST NEVER be committed to the repo. Long-term goal: submit shim to Microsoft shim-review to eliminate the MOK enrollment popup for end users.

> [!NOTE]
> Advanced UEFI features (multi-OS boot menu, capsule updates, W^X enforcement, multi-GPU GOP, extended Secure Boot state, SMBIOS extended types, DBX revocation sync) are in [TODO-18-uefi-advanced.md](TODO-18-uefi-advanced.md).

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- [`include/kernel/uefi_runtime.h`](../../include/kernel/uefi_runtime.h)
- [`include/kernel/smbios.h`](../../include/kernel/smbios.h)
- [`include/kernel/boot_splash.h`](../../include/kernel/boot_splash.h)
- [`src/kernel/boot_timing.c`](../../src/kernel/boot_timing.c)
- -> XREF: `TODO-07-boot-diagnostics.md` -- `boot_progress()` API consumed by §7
- -> XREF: `TODO-06-interrupt-timer-arch.md` -- timer calibration affects boot profiling in §7
- -> XREF: `02-kernel-core/TODO-07-time-filetime-management.md §5` -- `UEFI GetTime` -> FILETIME seeding
- -> XREF: `02-kernel-core/TODO-13-registry-completion.md` -- `HKLM\HARDWARE\*` and `HKLM\SYSTEM\SecureBoot` storage
- -> XREF: `TODO-18-uefi-advanced.md` -- deferred advanced features (multi-OS menu, capsule, W^X, multi-GPU, extended SB, SMBIOS ext, DBX)

## Outcome

- `ExitBootServices()` followed by `SetVirtualAddressMap()` -- UEFI runtime pointers valid in kernel.
- `uefi_var_get()` / `uefi_var_set()` work after boot.
- `boot_info.fb_width/fb_height` reflect negotiated GOP resolution; HiDPI flag set when width >= 2560.
- `HKLM\HARDWARE\BIOS\*`, `HKLM\HARDWARE\CPU\*`, `HKLM\HARDWARE\Memory\*` populated from SMBIOS.
- `boot_info.secure_boot_enabled` set correctly; kernel verifies state.
- `BOOTX64.EFI` signed with MOK key; `.gitignore` entry for `MOK.key`.
- Serial log unified format with atomic line writes.

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On      | Status |
| --- | :---: | ---------------------------------------- | --------------- | :----: |
| 💎  |   1   | UEFI runtime services preservation       | --              |  [x]   |
| 💎  |   2   | UEFI variable services                   | §1              |  [x]   |
| 💎  |   3   | GOP resolution auto-detection            | §1              |  [x]   |
| 💎  |   4   | SMBIOS table parsing                     | §1              |  [x]   |
| 💎  |   5   | Secure Boot state detection              | §2              |  [x]   |
| 💎  |   6   | Secure Boot shim chain-loading           | §5              |  [x]   |
| 💎  |   7   | Boot UX polish                           | §3, §5, T07 §2  |  [x]   |
| ⭐  |   8   | Serial log standardization               | --              |  [x]   |

---

## 1. UEFI Runtime Services Preservation

Before `ExitBootServices()`, save UEFI runtime function pointers into `boot_info` so the kernel can call them after the boot services are gone.

- [x] `uefi_runtime_init()` -> `call_set_virtual_address_map()` in kernel, after `ExitBootServices()`
- [x] `boot_info.uefi_runtime` struct: GetVariable, SetVariable, GetTime, SetTime, ResetSystem, UpdateCapsule, QueryCapsuleCapabilities
- [x] Bootloader copies `gRT->*` pointers before `ExitBootServices()`
- [x] Kernel validates each pointer non-NULL, logs OK or UNAVAILABLE
- [x] `BOOT_DEGRADED` path: `s_available = 0`; callers guard with `uefi_rt_available()`
- [x] Commit: `"boot: preserve UEFI runtime service pointers across ExitBootServices"`

## 2. UEFI Variable Services

Thin wrappers around `gRT->GetVariable` / `SetVariable` with error translation.

- [x] `uefi_var_get()` / `uefi_var_set()` with NTSTATUS return
- [x] Common GUIDs: EFI_GLOBAL, EFI_IMAGE_SECURITY_DATABASE, IMPOSSIBLE_OS_VENDOR
- [x] `uefi_var_get_u32()` / `uefi_var_set_u32()` convenience wrappers
- [x] `uefi_var_enumerate(callback)` for iterating all variables
- [x] Win32 API wired: `NtQuerySystemEnvironmentValue[Ex]` (0x00D2-0x00D4) and `NtSetSystemEnvironmentValue[Ex]` (0x00D3-0x00D5) registered in SSDT, mapped to `uefi_get_variable` / `uefi_set_variable`
- [x] Commit: `"kernel: UEFI variable get/set wrappers"`

## 3. GOP Resolution Auto-Detection

Negotiate the best framebuffer resolution before `ExitBootServices()`.

- [x] `gop_negotiate_mode()`: query all modes, score by resolution, respect `boot.conf` override
- [x] HiDPI flag: `boot_info.hidpi = 1` when width >= 2560
- [x] `gop->SetMode(best_mode)` with fallback to current mode
- [x] Commit: `"boot: GOP resolution auto-detection with HiDPI flag and boot.conf override"`

## 4. SMBIOS Table Parsing

Walk SMBIOS 3.x structures and populate Registry hardware keys.

- [x] Scan `EFI_CONFIGURATION_TABLE` for SMBIOS3 GUID; fallback to 2.x
- [x] Type 0 (BIOS): vendor, version, release date -> `HKLM\HARDWARE\BIOS\*`
- [x] Type 1 (System): manufacturer, product, serial, UUID -> `HKLM\HARDWARE\System\*`
- [x] Type 4 (Processor): socket, family, speed, cores, threads -> `HKLM\HARDWARE\CPU\*`
- [x] Type 17 (Memory): size, speed, type, manufacturer -> `HKLM\HARDWARE\Memory\*`
- [x] `smbios_get_system_uuid()`
- [ ] Expose via System Properties dialog (-> XREF: `09-desktop-shell/TODO-11-control-panel.md`)
- [x] Commit: `"kernel: SMBIOS 3.x table parse -> Registry HARDWARE hives"`

## 5. Secure Boot State Detection

Read the UEFI `SecureBoot` variable and expose the state to the kernel.

- [x] `uefi_secureboot_init()`: read `SecureBoot` variable, set `boot_info.secure_boot_enabled`
- [x] Write `HKLM\SYSTEM\SecureBoot\State` = 0 or 1
- [ ] Kernel signature verification (-> XREF: TODO-11 Security Reference Monitor, deferred until Code Integrity section added)
- [x] Padlock icon in system tray when Secure Boot active
- [x] Commit: `"kernel: Secure Boot state detection, registry key"`

## 6. Secure Boot Shim Chain-Loading

Set up MOK key pair, sign `BOOTX64.EFI`, and integrate shim into the build.

- [x] `.gitignore`: `MOK.key`, `*.signed.EFI`
- [x] Key generation documented in `docs/guides/secure-boot-keys.md`
- [x] `scripts/sign-efi.sh` + `sign-efi` Makefile target
- [x] Shim: `shim/shimx64.efi` + `shim/mmx64.efi` committed (SHA256 verified)
- [x] Shim validates `grubx64.efi` via embedded `MOK.cer`; MokManager for first-boot enrollment
- [x] Commit: `"boot: Secure Boot shim chain-loading, MOK key signing pipeline"`

## 7. Boot UX Polish

Structured boot profiling and pre-framebuffer error recovery screen.

- [x] Boot profiling: `boot_progress()` at every major event; `boot_timing_write_report()` writes to `X:\Perf\boot-profile.log`
- [x] JSON boot-timeline export (-> XREF: TODO-06 §9)
- [x] Pre-framebuffer error screen: `boot_halt(reason)` with inline 8x8 bitmap font
- [x] `boot_splash_status()` integration: live stage text below spinner
- [x] Commit: `"boot: boot-stage instrumentation, pre-framebuffer error recovery screen"`

## 8. Serial Log Standardization and Race Fix

Unified log format and atomic serial writes.

### 8.1 Unified log format

- [x] `klog.h` defines five levels: `LOG_DEBUG`->`[INFO]`, `LOG_INFO`->`[ OK ]`, `LOG_WARN`->`[WARN]`, `LOG_ERROR`->`[FAIL]`, `LOG_FATAL`->`[CRIT]`
- [x] All callers migrated from `printk()` to `klog()`
- [x] Deprecated `log.h` + `log.c` deleted
- [x] Commit: `"kernel: Serial Output Phase 1 -- unified klog format"`

### 8.2 Serial line-mangling race fix

- [x] `serial_putchar_raw()` static helper (no lock)
- [x] `serial_write()` holds spinlock for entire string
- [x] `klog()` builds full line in `char line[256]`, calls `serial_write()` atomically
- [x] Commit: `"kernel: fix serial line mangling by making klog write atomically"`

---

## OS Comparison

| ⭐ | Feature               | 🪟 Win11                   | 🐧 Linux                  | 🚀 Impossible OS              |
|----|-----------------------|-----------------------------|----------------------------|--------------------------------|
| 💎 | UEFI runtime post-EBS | ✅ Full RT preserved       | ✅ efi_call wrappers       | ✅ §1 -- SVAM + RT pointers   |
| 💎 | UEFI variable access  | ✅ GetFirmwareEnvVar       | ✅ efivarfs + efivar       | ✅ §2 -- get/set/enumerate    |
| 💎 | GOP resolution        | ✅ Boot manager negotiates | ✅ GRUB gfxmode + EFIFB    | ✅ §3 -- auto + HiDPI         |
| 💎 | SMBIOS core inventory | ✅ WMI Win32_BIOS          | ✅ /sys/firmware/dmi       | ✅ §4 -- Type 0/1/4/17        |
| 💎 | Secure Boot shim      | ✅ MS-signed shim + WHQL   | ✅ rhboot shim (distro)    | ✅ §6 -- shimx64 + MOK        |
| 💎 | Secure Boot state     | ✅ Registry + WinVerify    | ✅ efivarfs SecureBoot     | ✅ §5 -- NVRAM + registry     |
| ⭐ | Boot profile timeline | ❌ ETW binary (WPA)        | ❌ systemd-analyze         | ✅ §7 -- boot-profile.log     |
| 💎 | Atomic serial log     | ✅ KdPrint serialized      | ✅ printk cont flag        | ✅ §8 -- spinlock per line    |

> All core UEFI boot features at parity with Win11 and Linux. Boot profile timeline is a competitive edge -- human-readable JSON vs binary ETW or post-boot systemd-analyze. Advanced features (multi-GPU, extended SB, capsule, W^X, SMBIOS extended, DBX) deferred to [TODO-18](TODO-18-uefi-advanced.md).

## Unit Tests

> Wire into `test_runner_init()` via `test_register_uefi_boot()`.

- [ ] Create `src/kernel/test/test_uefi_boot.c` with:
  - `uefi_rt_available()` returns 1 (runtime services preserved)
  - `uefi_var_get()` for `L"SecureBoot"` returns STATUS_SUCCESS or STATUS_NOT_FOUND (never crashes)
  - `uefi_var_get_u32()` / `uefi_var_set_u32()` roundtrip: write test GUID variable, read back, values match
  - `boot_info.fb.width > 0` and `boot_info.fb.height > 0` (GOP negotiated)
  - `boot_info.hidpi == 1` when `boot_info.fb.width >= 2560`, else 0
  - `smbios_get_system_uuid()` returns non-zero UUID on real hardware
  - `boot_info.secure_boot_enabled` matches UEFI `SecureBoot` variable
  - `HKLM\HARDWARE\BIOS\BIOSVendor` is non-empty string
- [ ] Register in `test_runner_init()`: `test_register_uefi_boot()`
- [ ] Commit: `"test: add uefi_boot test suite"`

## Verification

- [x] Serial log shows `[UEFI] SetVirtualAddressMap OK` and `[UEFI] Runtime services: OK`
- [x] `uefi_var_get(L"SecureBoot", ...)` returns 0 (disabled) in QEMU
- [x] GOP negotiation log shows `[Boot] GOP: {W}x{H} 32bpp (mode N)`
- [x] SMBIOS data appears in Registry under `HKLM\HARDWARE\BIOS\*` and `HKLM\HARDWARE\System\*`
- [x] Signed build present when MOK keys exist; skipped silently when absent
- [x] No interleaved serial lines under concurrent IRQ logging
- [ ] Commit: `"boot: uefi-hardening core verified"`
