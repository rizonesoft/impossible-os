# TODO-01 — UEFI Bootloader Hardening & Secure Boot

> **Goal:** The basic UEFI bootloader and boot splash are done. This TODO hardens the boot path with Secure Boot shim chain-loading, UEFI runtime service preservation, UEFI variable access, GOP resolution auto-detection, SMBIOS hardware info, A/B dual-slot booting, multi-OS boot menu, UEFI capsule firmware updates, and W^X memory enforcement — the complete production-quality boot experience expected of a shipped OS.

> [!IMPORTANT]
> **Secure Boot strategy:** Use the rhboot/shim chain-loading approach. MOK key pair (`MOK.key`) is generated locally and MUST NEVER be committed to the repo. Long-term goal: submit shim to Microsoft shim-review to eliminate the MOK enrollment popup for end users.

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- [`include/kernel/uefi_runtime.h`](../../include/kernel/uefi_runtime.h)
- [`include/kernel/smbios.h`](../../include/kernel/smbios.h)
- [`include/kernel/boot_splash.h`](../../include/kernel/boot_splash.h)
- [`src/kernel/boot_timing.c`](../../src/kernel/boot_timing.c)
- → XREF: `TODO-02-boot-diagnostics.md` — `boot_progress()` API consumed by §7
- → XREF: `TODO-03-interrupt-timer-arch.md` — timer calibration affects boot profiling in §7
- → XREF: `02-kernel-core/TODO-05-native-api-layer.md` — `GetFirmwareEnvironmentVariableA/W` Win32 wiring (no dedicated section yet; add to TODO-05 when Win32 firmware-variable API surface is scoped)
- → XREF: `02-kernel-core/TODO-07-time-filetime-management.md §5` — `UEFI GetTime` → FILETIME seeding (wall clock init)
- → XREF: `02-kernel-core/TODO-11-security-reference-monitor.md` — `srm_verify_kernel_signature()` (called by §5 here) has no section in TODO-11 yet; needs to be added there (suggest §10 "Win32 Security API Wrappers" — WinVerifyTrust/Authenticode path, or a new §12 for Code Integrity)
- → XREF: `02-kernel-core/TODO-13-registry-completion.md` — `HKLM\HARDWARE\*` and `HKLM\SYSTEM\SecureBoot` storage

## Outcome

- `ExitBootServices()` is followed immediately by `SetVirtualAddressMap()` so UEFI runtime pointers remain valid in kernel virtual space.
- `uefi_var_get()` / `uefi_var_set()` work correctly after boot; `GetFirmwareEnvironmentVariableA/W` are wired.
- `boot_info.fb_width/fb_height` always reflect the negotiated GOP resolution; HiDPI flag set when width ≥ 2560.
- `HKLM\HARDWARE\BIOS\*`, `HKLM\HARDWARE\CPU\*`, `HKLM\HARDWARE\Memory\*` populated from SMBIOS at first boot.
- `boot_info.secure_boot_enabled` set correctly; padlock shown in system tray when Secure Boot is active; kernel verifies `kernel.exe` signature when enforced.
- `BOOTX64.EFI` signed with MOK key; `.gitignore` entry for `MOK.key`.
- A/B slot flip works on kernel update; auto-recovery rolls back after 3 failed boots.
- Boot menu appears on dual-boot hardware; Impossible OS boots by default after 3 s.
- UEFI capsule delivery path tested end-to-end in QEMU.
- `[Boot] W^X enforced on N UEFI memory regions` appears in serial log.

## Implementation Order

| ⭐  | Order | Deliverable                        | Depends On       | Status |
| --- | :---: | ---------------------------------- | ---------------- | :----: |
| 💎  |   1   | UEFI runtime services preservation | —                |  [x]   |
| 💎  |   2   | UEFI variable services             | 1                |  [x]   |
| 💎  |   3   | GOP resolution auto-detection      | 1                |  [ ]   |
| 💎  |   4   | SMBIOS table parsing               | 1                |  [ ]   |
| 💎  |   5   | Secure Boot state detection        | 2                |  [ ]   |
| 💎  |   6   | Secure Boot shim chain-loading     | 5                |  [ ]   |
| 💎  |   7   | Boot UX polish                     | 3, 5, TODO-02 §2 |  [ ]   |
| 💎  |   8   | A/B dual-slot boot                 | 2                |  [ ]   |
| ⭐  |   9   | Multi-OS detection & boot menu     | 1                |  [ ]   |
| 💎  |  10   | UEFI capsule update & ESRT         | 2, 8             |  [ ]   |
| 💎  |  11   | UEFI memory attributes (W^X)       | 1                |  [ ]   |

> 💎 = parity — Windows Boot Manager and GRUB implement these features; Impossible OS must match them.
> ⭐ = exclusive — the in-bootloader multi-OS detection with graphical countdown timer is not present in competitors.

---

## 1. UEFI Runtime Services Preservation `[Opus]`

Before `ExitBootServices()`, save UEFI runtime function pointers into `boot_info` so the kernel can call them after the boot services are gone.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`, `include/kernel/uefi_runtime.h`, `src/kernel/uefi_runtime.c`

- [x] Call `uefi_runtime_init()` → `call_set_virtual_address_map()` in kernel, after `ExitBootServices()`, to relocate all `EFI_MEMORY_RUNTIME`-flagged pages to their kernel virtual addresses (UEFI spec §7.4.2 requires post-EBS; bootloader must NOT call SVAM)
- [x] Extend `boot_info` with `uefi_runtime` struct: `GetVariable`, `SetVariable`, `GetTime`, `SetTime`, `ResetSystem`, `UpdateCapsule`, `QueryCapsuleCapabilities` function pointers
- [x] In bootloader: copy `gRT->*` pointers into `boot_info.uefi_runtime` before `ExitBootServices()`; `svam_called` stays `0` (SVAM is kernel-owned per spec)
- [x] In kernel `uefi_runtime_init()`: validate each pointer non-NULL, log `[UEFI] Runtime services: OK` or `[UEFI] Runtime services: UNAVAILABLE (firmware limitation)` and return `BOOT_DEGRADED`
- [x] `BOOT_DEGRADED` path: `s_available = 0`; callers guard with `uefi_rt_available()` before any `uefi_get_variable` / `uefi_set_variable` call
- [x] Boot-time serial log: `[UEFI] SetVirtualAddressMap OK` with count of runtime-mapped regions (`call_set_virtual_address_map()` logs region count + status via `klog`)
- [ ] Commit: `"boot: preserve UEFI runtime service pointers across ExitBootServices"`

## 2. UEFI Variable Services `[Sonnet]`

Thin wrappers around `gRT->GetVariable` / `SetVariable` with error translation, used throughout the kernel for Secure Boot key management, A/B slot state, and firmware settings.

**Files:** `include/kernel/uefi_vars.h`, `src/kernel/uefi_vars.c`

- [x] Define `uefi_var_get(const uint16_t *name, const efi_guid_t *guid, void *buf, size_t *size)` → `NTSTATUS` (`STATUS_SUCCESS`, `STATUS_NOT_FOUND`, `STATUS_BUFFER_TOO_SMALL`, `STATUS_UNSUCCESSFUL`) — `include/kernel/uefi_vars.h`, `src/kernel/uefi_vars.c`
- [x] Define `uefi_var_set(const uint16_t *name, const efi_guid_t *guid, const void *buf, size_t size, uint32_t attrs)` → `NTSTATUS`; `UEFI_VAR_NV_BOOT_RUNTIME` convenience macro covers the standard attrs combination
- [x] Define common GUIDs: `EFI_GLOBAL_VARIABLE_GUID_INIT`, `EFI_IMAGE_SECURITY_DATABASE_GUID_INIT`, `IMPOSSIBLE_OS_VENDOR_GUID_INIT` (`{6F35D3A4-C0E6-4A82-B5D8-7C9D2E4F8A13}`)
- [x] Implement `uefi_var_get_u32(name, guid, out)` / `uefi_var_set_u32(name, guid, val)` convenience wrappers
- [ ] Wire Win32 API: `GetFirmwareEnvironmentVariableA/W` → UTF-8/UTF-16 name conversion → `uefi_var_get`; `SetFirmwareEnvironmentVariableA/W` → `uefi_var_set` (→ XREF `02-kernel-core/TODO-05-native-api-layer.md` — section to be scoped when Win32 firmware-variable surface is defined)
- [x] Add `uefi_var_enumerate(callback)` for iterating all variables (used by §10 ESRT); backed by new `uefi_get_next_variable_name()` primitive added to `uefi_runtime.c`
- [x] Commit: `"kernel: UEFI variable get/set wrappers + Win32 GetFirmwareEnvironmentVariable wiring"`

## 3. GOP Resolution Auto-Detection `[Sonnet]`

Negotiate the best framebuffer resolution before `ExitBootServices()`, respecting `boot.conf` overrides and HiDPI display detection.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`

- [ ] Implement `gop_negotiate_mode()` in bootloader: call `gop->QueryMode(i, &size, &info)` for all `i` in `[0, gop->Mode->MaxMode)`
- [ ] Score each mode: exact match to `boot.conf` `Resolution=WxH` = max score; otherwise pick highest `width × height` the firmware offers
- [ ] Set `boot_info.hidpi = 1` if negotiated `width >= 2560`; caller (boot splash) scales logo and font by 2×
- [ ] If `boot.conf` `Resolution=` is absent or `auto`, pick highest native resolution
- [ ] Call `gop->SetMode(best_mode)` before reading framebuffer base address into `boot_info.fb.*`
- [ ] Serial log: `[Boot] GOP: {width}x{height} 32bpp (mode {idx})` after negotiation
- [ ] Fallback: if `SetMode` fails for best mode, retry with current mode; log `[Boot] GOP: using firmware default {W}x{H}`
- [ ] Commit: `"boot: GOP resolution auto-detection with HiDPI flag and boot.conf override"`

## 4. SMBIOS Table Parsing `[Sonnet]`

Walk SMBIOS 3.x structures and populate Registry hardware keys for System Properties, Device Manager, and diagnostics tools.

**Files:** `include/kernel/smbios.h`, `src/kernel/smbios.c`

- [ ] In bootloader: scan `EFI_CONFIGURATION_TABLE` for SMBIOS3 GUID (`{F2FD1544-9794-4A2C-992E-E5BBCF20E394}`); store `smbios3_entry_point` pointer in `boot_info.smbios_base`; fall back to SMBIOS 2.x GUID (`{EB9D2D31-...}`) if 3.x absent
- [ ] In `smbios_init()`: validate anchor string (`"_SM3_"` or `"_SM_"`), walk structure chain by following `(char *)hdr + hdr->length` then skipping two zero bytes for the string heap
- [ ] Extract Type 0 (BIOS info): vendor string, BIOS version, release date → `HKLM\HARDWARE\BIOS\BIOSVendor`, `BIOSVersion`, `BIOSReleaseDate`
- [ ] Extract Type 1 (System): manufacturer, product name, version, serial number, UUID → `HKLM\HARDWARE\System\SystemManufacturer`, `SystemProductName`, `SystemSerial`, `SystemUUID`
- [ ] Extract Type 4 (Processor, may repeat per socket): socket designation, family, speed, core count, thread count → `HKLM\HARDWARE\CPU\{idx}\*`
- [ ] Extract Type 17 (Memory device, repeats per DIMM): size MB, speed MHz, type (DDR4/DDR5), manufacturer, part number, bank/device locator → `HKLM\HARDWARE\Memory\{idx}\*`
- [ ] `smbios_get_system_uuid(uint8_t uuid[16])` — used by licensing and telemetry
- [ ] Expose via System Properties dialog (`sysdm.cpl`) and `msinfo32` shell command
- [ ] Commit: `"kernel: SMBIOS 3.x table parse → Registry HARDWARE hives"`

## 5. Secure Boot State Detection `[Sonnet]`

Read the UEFI `SecureBoot` variable and expose the state to the kernel and user mode.

**Files:** `src/kernel/uefi_runtime.c`, `include/kernel/uefi_runtime.h`, `src/kernel/main/boot_hw.c`

- [ ] Call `uefi_var_get_u32(L"SecureBoot", &EFI_GLOBAL_VARIABLE_GUID, &val)` in `uefi_secureboot_init()`; set `boot_info.secure_boot_enabled = (val == 1)`
- [ ] Write `HKLM\SYSTEM\SecureBoot\State` = 0 or 1 after registry is up (→ XREF `02-kernel-core/TODO-13-registry-completion.md`)
- [ ] If `secure_boot_enabled`: call `srm_verify_kernel_signature("C:\\boot\\kernel.exe")` (→ XREF `02-kernel-core/TODO-11-security-reference-monitor.md`); on failure: log `[SecureBoot] kernel.exe signature INVALID` + `BOOT_FATAL`
- [ ] Display padlock icon (🔒) in system tray status bar when Secure Boot is active (desktop integration hook — set flag in `g_system_state.secure_boot` readable by tray renderer)
- [ ] Serial log: `[SecureBoot] state=ENABLED` or `[SecureBoot] state=DISABLED (firmware or user override)`
- [ ] Commit: `"kernel: Secure Boot state detection, registry key, and kernel.exe signature check"`

## 6. Secure Boot Shim Chain-Loading `[Opus]`

Set up the MOK key pair, sign `BOOTX64.EFI`, and integrate shim into the build for hardware-compatible Secure Boot.

**Files:** `scripts/build.sh`, `scripts/sign-efi.sh`, `src/boot/uefi/`, `.gitignore`

- [ ] Add `.gitignore` entries: `MOK.key`, `MOK.pem`, `*.signed.EFI`, `keys/` — private key MUST NEVER be committed
- [ ] Document key generation in `docs/guides/secure-boot-keys.md`: `openssl genrsa -out MOK.key 2048` + `openssl req -new -x509 -key MOK.key -out MOK.crt -days 3650 -subj "/CN=Impossible OS MOK/"`
- [ ] Update `scripts/sign-efi.sh`: if `keys/MOK.key` and `keys/MOK.crt` exist, run `sbsign --key keys/MOK.key --cert keys/MOK.crt --output build/BOOTX64.signed.EFI build/BOOTX64.EFI`; skip silently if keys absent (dev builds)
- [ ] Bundle pre-compiled `shim.efi` (from rhboot release, or build from source with our vendor cert embedded): place at `resources/boot/shim.efi`; installed to ESP as `EFI\BOOT\BOOTX64.EFI`; our signed `BOOTX64.EFI` installed as `EFI\BOOT\grub.efi` (shim default fallback name)
- [ ] Shim validates our `BOOTX64.EFI` via MOK; on first boot without enrolled MOK: shim shows blue MOK Manager screen → user selects Enroll MOK → reboots → Impossible OS boots
- [ ] Add `sign-efi` Makefile target: `make sign-efi` invokes `scripts/sign-efi.sh`; CI sets `MOK_KEY` + `MOK_CRT` env vars from secrets vault
- [ ] Long-term tracker (no code): open issue to submit shim to [rhboot/shim-review](https://github.com/rhboot/shim-review) once first release candidate is tagged
- [ ] Commit: `"boot: Secure Boot shim chain-loading, MOK key signing pipeline, .gitignore"`

## 7. Boot UX Polish `[Sonnet]`

Fade-in transition, structured boot profiling, and a pre-framebuffer error recovery screen.

**Files:** `src/boot/uefi/bootx64.c`, `src/kernel/boot_timing.c`, `include/kernel/boot_timing.h`

- [ ] Bootloader fade-in: during the 300 ms between `ExitBootServices()` and kernel jump, ramp the GOP framebuffer from black to the accent color gradient from `boot.conf` key `AccentColor=RRGGBB`; use PIT-tick loop for timing; fallback to instant-black if `AccentColor` absent
- [ ] Boot profiling: `boot_timing_record_step()` (already in `boot_timing.c` §1) is the API; ensure it is called for every major event: UEFI init, ELF load, kernel entry, GDT/IDT, PMM, VMM, VFS, scheduler, desktop ready; write human-readable report to `C:\Impossible\System\Logs\boot-profile.log` at desktop-ready (`boot_timing_print_steps()`)
- [ ] JSON boot-timeline export and `boot-timeline` shell command are owned by → XREF: `TODO-03-interrupt-timer-arch.md §9`; this section's `boot_timing_record_step()` calls produce the data that §9 exports and visualizes.
- [ ] Pre-framebuffer error recovery screen: in `boot_halt()` (→ XREF `02-kernel-core/TODO-01-kernel-init-sequencing.md §7`) when called before `fb_init()`: write a solid red rectangle across the top 40 px of the framebuffer directly via `boot_info.fb.base`; draw error text using the 8×8 boot font; print error text + recovery URL to serial
- [ ] `boot_splash_status()` integration: each `boot_progress()` call (→ XREF `TODO-02-boot-diagnostics.md §2`) also calls `boot_splash_status()` with a human-readable stage string; splash shows live progress text below the spinner
- [ ] Commit: `"boot: fade-in transition, boot-stage instrumentation, pre-framebuffer error recovery screen"`

## 8. A/B Dual-Slot Boot `[Opus]`

Reliable kernel update delivery with automatic rollback on repeated boot failure.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`, `src/shell/update-slot.c`

- [ ] `boot.conf` keys: `BootSlot=A` (or `B`), `MaxBootAttempts=3`; bootloader reads these via FAT32 file open before loading kernel
- [ ] Bootloader loads `\boot\kernel-A.exe` when `BootSlot=A`, `\boot\kernel-B.exe` when `BootSlot=B`; store active slot in `boot_info.boot_slot`
- [ ] Bootloader increments `BootAttempts` counter in `boot.conf` (read-modify-write via UEFI SimpleFileSystem) on every boot attempt
- [ ] Kernel success path: call `boot_slot_mark_good()` which resets `BootAttempts=0` in `boot.conf` via VFS after VFS is up; must be called from Phase 3 after `vfs_init()` succeeds
- [ ] Auto-recovery: if `BootAttempts > MaxBootAttempts` at bootloader time → flip `BootSlot` (A↔B), reset `BootAttempts=0`, reboot; log `[Boot] Rollback: slot A failed 3× — switching to slot B`
- [ ] `HKLM\SYSTEM\BootSlot` = `"A"` or `"B"` (written by kernel after slot confirmed good)
- [ ] `update-slot` shell command: shows active slot, pending slot, attempt count; `update-slot --apply <kernel_path>` writes new kernel to inactive slot + flips `BootSlot`
- [ ] QEMU test: build two kernel images, set `BootSlot=A MaxBootAttempts=1`; corrupt kernel-A.exe; verify bootloader switches to kernel-B.exe on second boot
- [ ] Commit: `"boot: A/B dual-slot boot with automatic rollback on repeated failure"`

## 9. Multi-OS Detection & Boot Menu `[Sonnet]`

Detect other OS partitions from GPT and show a countdown boot menu when the user has multiple OSes installed.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`

- [ ] After GPT partition scan: check each partition type GUID: `{0FC63DAF-...}` = Linux data/ext4; `{EBD0A0A2-...}` = Windows NTFS/Basic Data; `{C12A7328-...}` = EFI System (other OS); `{4F68BCE3-...}` = Linux root x86-64
- [ ] If any non-Impossible-OS partition found: store up to 8 `boot_menu_entry_t` in `boot_info.boot_entries[]` (label, partition index, type)
- [ ] Render text-mode boot menu on framebuffer (white text on black): `"1. Impossible OS [default]"`, `"2. Windows Boot Manager"`, `"3. Linux"` etc.; countdown timer in top-right corner (`3... 2... 1...`) using PIT ticks; configurable timeout: `boot.conf` key `BootMenuTimeout=3`
- [ ] Input: PS/2 keyboard polling via `in al, 0x60`; number key or arrow key + Enter selects; ESC or no input → boot Impossible OS immediately
- [ ] For non-Impossible entries: chainload the selected partition's VBR (read first 512 bytes of partition, copy to `0x7C00`, jump) or launch `\EFI\Microsoft\Boot\bootmgfw.efi` / `\EFI\ubuntu\grubx64.efi` via `LoadImage` + `StartImage`
- [ ] `boot.conf` key `DefaultOS=0` (0-indexed; 0 = Impossible OS always default)
- [ ] Commit: `"boot: multi-OS GPT detection and countdown text-mode boot menu"`

## 10. UEFI Capsule Update & ESRT `[Sonnet]`

Parse the ESRT firmware resource table and implement the UEFI capsule delivery path for firmware updates.

**Files:** `src/kernel/uefi_capsule.c`, `include/kernel/uefi_capsule.h`

- [ ] `esrt_init()`: locate `EFI_SYSTEM_RESOURCE_TABLE` in UEFI configuration tables (GUID `{B122A263-...}`); walk `EFI_SYSTEM_RESOURCE_ENTRY` array; extract firmware type, GUID, current version, last-attempt version, last-attempt status
- [ ] Write to Registry: `HKLM\HARDWARE\Firmware\{GUID}\FwType`, `FwVersion`, `LastAttemptVersion`, `LastAttemptStatus` for each entry
- [ ] `capsule_update_request(path)`: read capsule file from `path` on the VFS; write to `\EFI\UpdateCapsule\` on the ESP via `uefi_var_set`; set UEFI variable `OsIndications` bit 0 (`EFI_OS_INDICATIONS_FILE_CAPSULE_DELIVERY_SUPPORTED`); reboot
- [ ] `capsule_check_result()`: called at boot — read `OsIndicationsSupported` to confirm capsule delivery is supported; read `CapsuleReportGuid` variable to check last update result
- [ ] Serial log: `[ESRT] {N} firmware entries found` at boot; `[ESRT] BIOS version 0x{ver}`
- [ ] Commit: `"kernel: ESRT firmware table parse + UEFI capsule update delivery"`

## 11. UEFI Memory Attributes (W^X) `[Opus]`

Enforce write-XOR-execute on UEFI runtime memory regions by walking the `EFI_MEMORY_ATTRIBUTES_TABLE`.

**Files:** `src/kernel/uefi_runtime.c`, `src/kernel/mm/vmm.c`

- [ ] Locate `EFI_MEMORY_ATTRIBUTES_TABLE` in UEFI config tables (GUID `{DCFA911D-...}`)
- [ ] Walk entries: for any page with both `EFI_MEMORY_RW` and `EFI_MEMORY_XP` attributes → clear execute permission (`NX` bit) in kernel page tables via `vmm_set_nx(virt, size)` (requires EFER.NXE already set)
- [ ] For any EFI runtime code region (`EFI_MEMORY_RUNTIME | EFI_MEMORY_RT_CODE`): ensure mapped `RX` only (no write); call `vmm_set_ro(virt, size)` to remove write permission
- [ ] `BOOT_DEGRADED` path: if `EFI_MEMORY_ATTRIBUTES_TABLE` is absent (older firmware), log `[UEFI] W^X: attributes table not present — skipping enforcement` and continue
- [ ] Serial log: `[UEFI] W^X enforced on {N} runtime memory regions` on success
- [ ] Commit: `"kernel: UEFI runtime W^X enforcement via EFI_MEMORY_ATTRIBUTES_TABLE"`

---

## OS Comparison

| ⭐  | Feature                             | 🪟 Windows 11                                   | 🐧 Linux (GRUB/systemd-boot)                     | 🚀 Impossible OS                                       |
| --- | ----------------------------------- | ------------------------------------------------ | ------------------------------------------------ | ------------------------------------------------------- |
| 💎  | Secure Boot shim chain-loading      | ✅ Microsoft-signed shim + WHQL                 | ✅ rhboot shim (distro-signed)                   | ⬜ Planned — §6; MOK enrollment path                   |
| 💎  | Secure Boot state in kernel         | ✅ `HKLM\SYSTEM\SecureBoot` + WinVerifyTrust    | ✅ `/sys/firmware/efi/efivars/SecureBoot`        | ⬜ Planned — §5                                        |
| 💎  | UEFI runtime services after boot    | ✅ Full EFI runtime preserved                   | ✅ `efi_call_*` wrappers post-ExitBootServices   | ✅ Done — §1; RT pointers copied pre-EBS, SVAM called in `uefi_runtime_init()` post-EBS  |
| 💎  | UEFI variable read/write            | ✅ `GetFirmwareEnvironmentVariable` Win32 API   | ✅ `efivarfs` + `efivar` library                 | ✅ Done — §2; `uefi_var_get/set`, NTSTATUS translation, `uefi_var_enumerate(callback)`, `IMPOSSIBLE_OS_VENDOR_GUID`  |
| 💎  | GOP resolution negotiation          | ✅ Boot manager negotiates GOP mode             | ✅ GRUB `gfxmode` + EFIFB                        | ⬜ Planned — §3                                        |
| 💎  | SMBIOS hardware inventory           | ✅ WMI Win32_BIOS/Win32_ComputerSystem          | ✅ `/sys/firmware/dmi/entries/`                  | ⬜ Planned — §4                                        |
| 💎  | A/B dual-slot kernel update         | ✅ Windows Update dual-partition recovery       | ✅ `grub-reboot` + BTRFS snapshots               | ⬜ Planned — §8                                        |
| 💎  | UEFI capsule firmware update        | ✅ Windows Update delivers UEFI capsules        | ✅ `fwupd` + `fwupdmgr update`                   | ⬜ Planned — §10                                       |
| 💎  | UEFI memory W^X enforcement         | ✅ Enforced since Windows 10 1607               | ✅ `CONFIG_EFI_MEMORY_ATTRIBUTES_TABLE`          | ⬜ Planned — §11                                       |
| ⭐  | In-bootloader multi-OS menu         | ❌ Separate BCD / bootmgr UI                    | ❌ GRUB is a separate bootloader                 | ⬜ **Planned — §9 — integrated countdown menu**        |
| ⭐  | Bootloader fade-in accent gradient  | ❌ Fixed black → logo, no user color            | ❌ Not implemented                               | ⬜ **Planned — §7 — accent color from boot.conf**      |
| ⭐  | JSON boot profiling timeline        | ❌ ETW boot trace (binary, needs WPA to decode) | ❌ `systemd-analyze` (post-boot, not bootloader) | ⬜ **§7 produces timing data; export + chart owned by TODO-03 §9**  |

> **After parity items:** Impossible OS will fully match Windows and Linux on Secure Boot, UEFI runtime, SMBIOS, capsule updates, and W^X enforcement. The exclusive items push beyond: the integrated countdown boot menu eliminates the need for a separate bootloader for dual-boot, the accent fade-in gives a branded first impression, and the structured JSON boot timeline makes performance regression testing trivial compared to WPA or systemd-analyze.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [x] Serial log shows `[UEFI] SetVirtualAddressMap OK` and `[UEFI] Runtime services: OK`
- [ ] `uefi_var_get(L"SecureBoot", ...)` returns 0 (disabled) in QEMU; returns 1 on Secure Boot–enabled hardware
- [ ] GOP negotiation log shows `[Boot] GOP: {W}x{H} 32bpp (mode N)` matching QEMU display resolution
- [ ] SMBIOS data appears in Registry under `HKLM\HARDWARE\BIOS\*` and `HKLM\HARDWARE\System\*`
- [ ] `BOOTX64.EFI` signed build present when `keys/MOK.key` + `keys/MOK.crt` exist; skipped silently when absent
- [ ] A/B rollback: corrupt kernel-A.exe in QEMU disk image; after `MaxBootAttempts+1` boots, kernel-B.exe boots successfully
- [ ] Boot menu appears when two GPT partitions are present; timer counts down; default boots without input
- [ ] `[UEFI] W^X enforced on N UEFI memory regions` in serial log (N > 0 on QEMU with OVMF)
- [ ] Commit: `"boot: uefi-hardening verified — runtime services, Secure Boot, GOP, SMBIOS, A/B slots, boot menu"`
