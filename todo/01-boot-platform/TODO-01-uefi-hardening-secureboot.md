# TODO-01 — UEFI Bootloader Hardening & Secure Boot

> **Goal:** The basic UEFI bootloader and boot splash are done. This TODO hardens the boot path with Secure Boot shim chain-loading, UEFI runtime service preservation, UEFI variable access, GOP resolution auto-detection, SMBIOS hardware info, multi-OS boot menu, UEFI capsule firmware updates, and W^X memory enforcement — the complete production-quality boot experience expected of a shipped OS.

> [!IMPORTANT]
> **Secure Boot strategy:** Use the rhboot/shim chain-loading approach. MOK key pair (`MOK.key`) is generated locally and MUST NEVER be committed to the repo. Long-term goal: submit shim to Microsoft shim-review to eliminate the MOK enrollment popup for end users.

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- [`include/kernel/uefi_runtime.h`](../../include/kernel/uefi_runtime.h)
- [`include/kernel/smbios.h`](../../include/kernel/smbios.h)
- [`include/kernel/boot_splash.h`](../../include/kernel/boot_splash.h)
- [`src/kernel/boot_timing.c`](../../src/kernel/boot_timing.c)
- → XREF: `TODO-07-boot-diagnostics.md` — `boot_progress()` API consumed by §7
- → XREF: `TODO-06-interrupt-timer-arch.md` — timer calibration affects boot profiling in §7
- → XREF: `02-kernel-core/TODO-05-native-api-ssdt.md` — `GetFirmwareEnvironmentVariableA/W` Win32 wiring (no dedicated section yet; add to TODO-05 when Win32 firmware-variable API surface is scoped)
- → XREF: `02-kernel-core/TODO-07-time-filetime-management.md §5` — `UEFI GetTime` → FILETIME seeding (wall clock init)
- → XREF: `02-kernel-core/TODO-11-security-reference-monitor.md` — `srm_verify_kernel_signature()` (called by §5 here) has no section in TODO-11 yet; needs to be added there (suggest §10 "Win32 Security API Wrappers" — WinVerifyTrust/Authenticode path, or a new §12 for Code Integrity)
- → XREF: `02-kernel-core/TODO-13-registry-completion.md` — `HKLM\HARDWARE\*` and `HKLM\SYSTEM\SecureBoot` storage
- → XREF: `04-drivers-hardware/TODO-08-gpu-display-drivers.md §8` — kernel multi-head `display_register_head()` consumes `boot_info.gop_handles[]` populated by §12 here

## Outcome

- `ExitBootServices()` is followed immediately by `SetVirtualAddressMap()` so UEFI runtime pointers remain valid in kernel virtual space.
- `uefi_var_get()` / `uefi_var_set()` work correctly after boot; `GetFirmwareEnvironmentVariableA/W` are wired.
- `boot_info.fb_width/fb_height` always reflect the negotiated GOP resolution; HiDPI flag set when width ≥ 2560.
- `HKLM\HARDWARE\BIOS\*`, `HKLM\HARDWARE\CPU\*`, `HKLM\HARDWARE\Memory\*` populated from SMBIOS at first boot.
- `boot_info.secure_boot_enabled` set correctly; padlock shown in system tray when Secure Boot is active; kernel verifies `kernel.exe` signature when enforced.
- `BOOTX64.EFI` signed with MOK key; `.gitignore` entry for `MOK.key`.
- Boot menu appears on dual-boot hardware; Impossible OS boots by default after 3 s.
- UEFI capsule delivery path tested end-to-end in QEMU.
- `[Boot] W^X enforced on N UEFI memory regions` appears in serial log.

## Implementation Order

| ⭐  | Order | Deliverable                        | Depends On      | Status |
| --- | :---: | ---------------------------------- | --------------- | :----: |
| 💎  |   1   | UEFI runtime services preservation | —               |  [x]   |
| 💎  |   2   | UEFI variable services             | §1              |  [x]   |
| 💎  |   3   | GOP resolution auto-detection      | §1              |  [x]   |
| 💎  |   4   | SMBIOS table parsing               | §1              |  [x]   |
| 💎  |   5   | Secure Boot state detection        | §2              |  [x]   |
| 💎  |   6   | Secure Boot shim chain-loading     | §5              |  [x]   |
| 💎  |   7   | Boot UX polish                     | §3, §5, T07 §2  |  [x]   |
| ⭐  |   8   | Multi-OS detection & boot menu     | §1              | defer  |
| 💎  |   9   | UEFI capsule update & ESRT         | §2              | defer  |
| 💎  |  10   | UEFI memory attributes (W^X)       | §1              | defer  |
| ⭐  |  11   | Serial log standardization         | —               |  [x]   |
| 💎  |  12   | Multi-GPU GOP enumeration          | §3              | defer  |

> 💎 = parity — Windows Boot Manager and GRUB implement these features; Impossible OS must match them.
> ⭐ = exclusive — the in-bootloader multi-OS detection with graphical countdown timer is not present in competitors.

---

## 1. UEFI Runtime Services Preservation
Before `ExitBootServices()`, save UEFI runtime function pointers into `boot_info` so the kernel can call them after the boot services are gone.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`, `include/kernel/uefi_runtime.h`, `src/kernel/uefi_runtime.c`

- [x] Call `uefi_runtime_init()` → `call_set_virtual_address_map()` in kernel, after `ExitBootServices()`, to relocate all `EFI_MEMORY_RUNTIME`-flagged pages to their kernel virtual addresses (UEFI spec §7.4.2 requires post-EBS; bootloader must NOT call SVAM)
- [x] Extend `boot_info` with `uefi_runtime` struct: `GetVariable`, `SetVariable`, `GetTime`, `SetTime`, `ResetSystem`, `UpdateCapsule`, `QueryCapsuleCapabilities` function pointers
- [x] In bootloader: copy `gRT->*` pointers into `boot_info.uefi_runtime` before `ExitBootServices()`; `svam_called` stays `0` (SVAM is kernel-owned per spec)
- [x] In kernel `uefi_runtime_init()`: validate each pointer non-NULL, log `[UEFI] Runtime services: OK` or `[UEFI] Runtime services: UNAVAILABLE (firmware limitation)` and return `BOOT_DEGRADED`
- [x] `BOOT_DEGRADED` path: `s_available = 0`; callers guard with `uefi_rt_available()` before any `uefi_get_variable` / `uefi_set_variable` call
- [x] Boot-time serial log: `[UEFI] SetVirtualAddressMap OK` with count of runtime-mapped regions (`call_set_virtual_address_map()` logs region count + status via `klog`)
- [ ] Commit: `"boot: preserve UEFI runtime service pointers across ExitBootServices"`

## 2. UEFI Variable Services
Thin wrappers around `gRT->GetVariable` / `SetVariable` with error translation, used throughout the kernel for Secure Boot key management, A/B slot state, and firmware settings.

**Files:** `include/kernel/uefi_vars.h`, `src/kernel/uefi_vars.c`

- [x] Define `uefi_var_get(const uint16_t *name, const efi_guid_t *guid, void *buf, size_t *size)` → `NTSTATUS` (`STATUS_SUCCESS`, `STATUS_NOT_FOUND`, `STATUS_BUFFER_TOO_SMALL`, `STATUS_UNSUCCESSFUL`) — `include/kernel/uefi_vars.h`, `src/kernel/uefi_vars.c`
- [x] Define `uefi_var_set(const uint16_t *name, const efi_guid_t *guid, const void *buf, size_t size, uint32_t attrs)` → `NTSTATUS`; `UEFI_VAR_NV_BOOT_RUNTIME` convenience macro covers the standard attrs combination
- [x] Define common GUIDs: `EFI_GLOBAL_VARIABLE_GUID_INIT`, `EFI_IMAGE_SECURITY_DATABASE_GUID_INIT`, `IMPOSSIBLE_OS_VENDOR_GUID_INIT` (`{6F35D3A4-C0E6-4A82-B5D8-7C9D2E4F8A13}`)
- [x] Implement `uefi_var_get_u32(name, guid, out)` / `uefi_var_set_u32(name, guid, val)` convenience wrappers
- [ ] Wire Win32 API: `GetFirmwareEnvironmentVariableA/W` → UTF-8/UTF-16 name conversion → `uefi_var_get`; `SetFirmwareEnvironmentVariableA/W` → `uefi_var_set` (→ XREF `02-kernel-core/TODO-05-native-api-ssdt.md` — section to be scoped when Win32 firmware-variable surface is defined)
- [x] Add `uefi_var_enumerate(callback)` for iterating all variables (used by §9 ESRT); backed by new `uefi_get_next_variable_name()` primitive added to `uefi_runtime.c`
- [x] Commit: `"kernel: UEFI variable get/set wrappers + Win32 GetFirmwareEnvironmentVariable wiring"`

## 3. GOP Resolution Auto-Detection
Negotiate the best framebuffer resolution before `ExitBootServices()`, respecting `boot.conf` overrides and HiDPI display detection.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`

- [x] Implement `gop_negotiate_mode()` in bootloader: call `gop->QueryMode(i, &size, &info)` for all `i` in `[0, gop->Mode->MaxMode)`
- [x] Score each mode: exact match to `boot.conf` `Resolution=WxH` = max score; otherwise pick highest `width × height` the firmware offers
- [x] Set `boot_info.hidpi = 1` if negotiated `width >= 2560`; caller (boot splash) scales logo and font by 2×
- [x] If `boot.conf` `Resolution=` is absent or `auto`, pick highest native resolution
- [x] Call `gop->SetMode(best_mode)` before reading framebuffer base address into `boot_info.fb.*`
- [x] Serial log: `BOOT: GOP: {width}x{height} 32bpp (mode {idx})` after negotiation
- [x] Fallback: if `SetMode` fails for best mode, retry with current mode; log `BOOT: GOP: using firmware default {W}x{H}`
- [x] Commit: `"boot: GOP resolution auto-detection with HiDPI flag and boot.conf override"`

## 4. SMBIOS Table Parsing
Walk SMBIOS 3.x structures and populate Registry hardware keys for System Properties, Device Manager, and diagnostics tools.

**Files:** `include/kernel/smbios.h`, `src/kernel/smbios.c`

- [x] In bootloader: scan `EFI_CONFIGURATION_TABLE` for SMBIOS3 GUID (`{F2FD1544-9794-4A2C-992E-E5BBCF20E394}`); store `smbios3_entry_point` pointer in `boot_info.smbios_base`; fall back to SMBIOS 2.x GUID (`{EB9D2D31-...}`) if 3.x absent
- [x] In `smbios_init()`: validate anchor string (`"_SM3_"` or `"_SM_"`), walk structure chain by following `(char *)hdr + hdr->length` then skipping two zero bytes for the string heap
- [x] Extract Type 0 (BIOS info): vendor string, BIOS version, release date → `HKLM\HARDWARE\BIOS\BIOSVendor`, `BIOSVersion`, `BIOSReleaseDate`
- [x] Extract Type 1 (System): manufacturer, product name, version, serial number, UUID → `HKLM\HARDWARE\System\SystemManufacturer`, `SystemProductName`, `SystemSerial`, `SystemUUID`
- [x] Extract Type 4 (Processor, may repeat per socket): socket designation, family, speed, core count, thread count → `HKLM\HARDWARE\CPU\{idx}\*`
- [x] Extract Type 17 (Memory device, repeats per DIMM): size MB, speed MHz, type (DDR4/DDR5), manufacturer, part number, bank/device locator → `HKLM\HARDWARE\Memory\{idx}\*`
- [x] `smbios_get_system_uuid(uint8_t uuid[16])` — used by licensing and telemetry
- [ ] Expose via System Properties dialog (`sysdm.cpl`) and `msinfo32` shell command (deferred — depends on desktop shell)
- [x] Commit: `"kernel: SMBIOS 3.x table parse → Registry HARDWARE hives"`

## 5. Secure Boot State Detection
Read the UEFI `SecureBoot` variable and expose the state to the kernel and user mode.

**Files:** `src/kernel/uefi_runtime.c`, `include/kernel/uefi_runtime.h`, `src/kernel/main/boot_hw.c`

- [x] Call `uefi_var_get_u32(L"SecureBoot", &EFI_GLOBAL_VARIABLE_GUID, &val)` in `uefi_secureboot_init()`; set `boot_info.secure_boot_enabled = (val == 1)`
- [x] Write `HKLM\SYSTEM\SecureBoot\State` = 0 or 1 after registry is up (→ XREF `02-kernel-core/TODO-13-registry-completion.md`)
- [ ] *(deferred)* If `secure_boot_enabled`: call `srm_verify_kernel_signature("C:\\boot\\kernel.exe")` — blocked until `02-kernel-core/TODO-11-security-reference-monitor.md` adds a Code Integrity section; log `[SecureBoot] kernel.exe signature INVALID` + `BOOT_FATAL` on failure
- [x] Display padlock icon (🔒) in system tray status bar when Secure Boot is active (desktop integration hook — set flag in `g_system_state.secure_boot` readable by tray renderer)
- [x] Serial log: `[SecureBoot] state=ENABLED` or `[SecureBoot] state=DISABLED (firmware or user override)`
- [x] Commit: `"kernel: Secure Boot state detection, registry key, and kernel.exe signature check"`

## 6. Secure Boot Shim Chain-Loading
Set up the MOK key pair, sign `BOOTX64.EFI`, and integrate shim into the build for hardware-compatible Secure Boot.

**Files:** `scripts/build.sh`, `scripts/sign-efi.sh`, `src/boot/uefi/`, `.gitignore`

- [x] Add `.gitignore` entries: `MOK.key`, `*.signed.EFI` — private key MUST NEVER be committed (`keys/` is tracked for `MOK.cer`/`MOK.der`; `MOK.pem` unused — we use `.cer`/`.der`)
- [x] Document key generation in `docs/guides/secure-boot-keys.md` (`keys/README.md` covers same content; dedicated guide created at `docs/guides/secure-boot-keys.md`)
- [x] `scripts/sign-efi.sh` created (thin wrapper + CI entry point); `sign-efi` Makefile target (lines 138–151) is the primary build path — signs in-place, skips silently if `keys/MOK.key` absent
- [x] Bundle pre-compiled `shim.efi`: `shim/shimx64.efi` + `shim/mmx64.efi` committed (SHA256 verified); built via `scripts/secure-boot/build-shim.sh` with `VENDOR_CERT_FILE=keys/MOK.cer`; Makefile installs to `EFI/BOOT/BOOTX64.EFI`; signed bootloader installed as `EFI/BOOT/grubx64.efi`
- [x] Shim validates `grubx64.efi` via embedded `MOK.cer` (`VENDOR_CERT_FILE`); first-boot without enrolled MOK: `mmx64.efi` (MokManager) shows enrollment UI; pipeline verified by `scripts/secure-boot/build-sb-test-disk.sh`
- [x] `sign-efi` Makefile target exists (`make sign-efi`); CI env var overrides `MOK_KEY`/`MOK_CRT` supported via `scripts/sign-efi.sh`
- [x] Long-term tracker (no code): submit shim to [rhboot/shim-review](https://github.com/rhboot/shim-review) once first release candidate is tagged — tracked in `shim/README.md`
- [x] Commit: `"boot: Secure Boot shim chain-loading, MOK key signing pipeline, .gitignore"`

## 7. Boot UX Polish
Fade-in transition, structured boot profiling, and a pre-framebuffer error recovery screen.

**Files:** `src/boot/uefi/bootx64.c`, `src/kernel/boot_timing.c`, `include/kernel/boot_timing.h`,
`src/kernel/main/boot_init.c`, `src/kernel/main/boot_hw.c`, `src/kernel/main/boot_interrupts.c`,
`src/kernel/main/boot_storage.c`, `src/kernel/main/boot_desktop.c`,
`src/kernel/main/boot_halt.c`, `include/kernel/boot_halt.h`

- [x] Bootloader fade-in: removed — a brief accent color pulse between ExitBootServices and kernel jump added visual noise without benefit; the DRAW_BAR strips in the bootloader itself are sufficient for pre-kernel diagnostics; the screen stays black until boot_splash_init()
- [x] Boot profiling: `boot_progress()` now called at every major event (SERIAL, BOOT_INFO, PMM, VMM, HEAP, CPUID_SIMD, STORAGE_DRV, GDT, IDT, ACPI, LAPIC_IOAPIC, TIMER, RTC, KEYBOARD, FB, VFS, SMP, REGISTRY, DESKTOP_READY); `boot_timing_write_report()` added to `boot_timing.c` — writes human-readable `+NNNms [PHASEx] 0xNN step` lines to `C:\Impossible\System\Logs\boot-profile.log` via VFS; called at desktop-ready in `boot_desktop.c`
- [x] JSON boot-timeline export and `boot-timeline` shell command are owned by → XREF: `TODO-06-interrupt-timer-arch.md §9`; this section's `boot_timing_record_step()` calls produce the data that §9 exports and visualizes.
- [x] Pre-framebuffer error recovery screen: `boot_halt(reason)` created in `src/kernel/main/boot_halt.c` with inline 8×8 bitmap font (full printable ASCII 0x20–0x7F); draws solid dark-red banner (top 40 px), white title + reason text, recovery URL; always prints to serial; halts with `hlt`; `include/kernel/boot_halt.h` exposes the symbol
- [x] `boot_splash_status()` integration: `boot_progress()` in `boot_init.c` now calls `boot_splash_status(step)` after `boot_timing_record_step()`; splash shows live stage text below the spinner during every instrumented event
- [x] Commit: `"boot: fade-in transition, boot-stage instrumentation, pre-framebuffer error recovery screen"`

## 8. Multi-OS Detection & Boot Menu *(deferred — dual-boot UX, not needed during development)*
Detect other OS partitions from GPT and show a countdown boot menu when the user has multiple OSes installed.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`

- [ ] After GPT partition scan: check each partition type GUID: `{0FC63DAF-...}` = Linux data/ext4; `{EBD0A0A2-...}` = Windows NTFS/Basic Data; `{C12A7328-...}` = EFI System (other OS); `{4F68BCE3-...}` = Linux root x86-64
- [ ] If any non-Impossible-OS partition found: store up to 8 `boot_menu_entry_t` in `boot_info.boot_entries[]` (label, partition index, type)
- [ ] Render text-mode boot menu on framebuffer (white text on black): `"1. Impossible OS [default]"`, `"2. Windows Boot Manager"`, `"3. Linux"` etc.; countdown timer in top-right corner (`3... 2... 1...`) using PIT ticks; configurable timeout: `boot.conf` key `BootMenuTimeout=3`
- [ ] Input: PS/2 keyboard polling via `in al, 0x60`; number key or arrow key + Enter selects; ESC or no input → boot Impossible OS immediately
- [ ] For non-Impossible entries: chainload the selected partition's VBR (read first 512 bytes of partition, copy to `0x7C00`, jump) or launch `\EFI\Microsoft\Boot\bootmgfw.efi` / `\EFI\ubuntu\grubx64.efi` via `LoadImage` + `StartImage`
- [ ] `boot.conf` key `DefaultOS=0` (0-indexed; 0 = Impossible OS always default)
- [ ] Commit: `"boot: multi-OS GPT detection and countdown text-mode boot menu"`

**Test checkpoint:** QEMU with two GPT partitions (Impossible OS + dummy Linux partition GUID): boot menu appears with 2 entries, countdown from 3, auto-selects Impossible OS. Bare metal dual-boot laptop: detects Windows/Linux partitions, menu renders, chainload works.

## 9. UEFI Capsule Update & ESRT *(deferred — firmware update infrastructure, not needed during development)*
Parse the ESRT firmware resource table and implement the UEFI capsule delivery path for firmware updates.

**Files:** `src/kernel/uefi_capsule.c`, `include/kernel/uefi_capsule.h`

- [ ] `esrt_init()`: locate `EFI_SYSTEM_RESOURCE_TABLE` in UEFI configuration tables (GUID `{B122A263-...}`); walk `EFI_SYSTEM_RESOURCE_ENTRY` array; extract firmware type, GUID, current version, last-attempt version, last-attempt status
- [ ] Write to Registry: `HKLM\HARDWARE\Firmware\{GUID}\FwType`, `FwVersion`, `LastAttemptVersion`, `LastAttemptStatus` for each entry
- [ ] `capsule_update_request(path)`: read capsule file from `path` on the VFS; write to `\EFI\UpdateCapsule\` on the ESP via `uefi_var_set`; set UEFI variable `OsIndications` bit 0 (`EFI_OS_INDICATIONS_FILE_CAPSULE_DELIVERY_SUPPORTED`); reboot
- [ ] `capsule_check_result()`: called at boot — read `OsIndicationsSupported` to confirm capsule delivery is supported; read `CapsuleReportGuid` variable to check last update result
- [ ] Serial log: `[ESRT] {N} firmware entries found` at boot; `[ESRT] BIOS version 0x{ver}`
- [ ] Commit: `"kernel: ESRT firmware table parse + UEFI capsule update delivery"`

**Test checkpoint:** QEMU: serial shows `[ESRT] N firmware entries found`. Registry `HKLM\HARDWARE\Firmware\{GUID}\FwVersion` populated. Bare metal: ESRT parsed from real firmware, version logged.

## 10. UEFI Memory Attributes (W^X) *(deferred — needs vmm_set_nx/vmm_set_ro which don't exist yet)*
Enforce write-XOR-execute on UEFI runtime memory regions by walking the `EFI_MEMORY_ATTRIBUTES_TABLE`.

**Files:** `src/kernel/uefi_runtime.c`, `src/kernel/mm/vmm.c`

- [ ] Locate `EFI_MEMORY_ATTRIBUTES_TABLE` in UEFI config tables (GUID `{DCFA911D-...}`)
- [ ] Walk entries: for any page with both `EFI_MEMORY_RW` and `EFI_MEMORY_XP` attributes → clear execute permission (`NX` bit) in kernel page tables via `vmm_set_nx(virt, size)` (requires EFER.NXE already set)
- [ ] For any EFI runtime code region (`EFI_MEMORY_RUNTIME | EFI_MEMORY_RT_CODE`): ensure mapped `RX` only (no write); call `vmm_set_ro(virt, size)` to remove write permission
- [ ] `BOOT_DEGRADED` path: if `EFI_MEMORY_ATTRIBUTES_TABLE` is absent (older firmware), log `[UEFI] W^X: attributes table not present — skipping enforcement` and continue
- [ ] Serial log: `[UEFI] W^X enforced on {N} runtime memory regions` on success
- [ ] Prerequisite: implement `vmm_set_nx(virt, size)` and `vmm_set_ro(virt, size)` in `src/kernel/mm/vmm.c` — walk PTEs and set/clear NX and R/W bits respectively. These do not exist yet.
- [ ] Commit: `"kernel: UEFI runtime W^X enforcement via EFI_MEMORY_ATTRIBUTES_TABLE"`

**Regression risk:** Modifies page table permissions on UEFI runtime regions. A bug could make UEFI runtime calls fault (GetTime, SetVariable, ResetSystem all stop working). Rollback: skip W^X enforcement entirely (BOOT_DEGRADED path).

**Test checkpoint:** QEMU: serial shows `[UEFI] W^X enforced on N runtime memory regions`. After enforcement, `uefi_get_time()` still works (runtime calls not broken). Bare metal: same serial output, RTC time reads correctly post-enforcement.

---

## 11. Serial Log Standardization & Race Fix ✅

**Goal:** Replace inconsistent ad-hoc serial output (`[Boot]`, `BOOT:`, `[!!]`, `[OK]`, bare `printk`) with a single unified log format and eliminate serial line mangling caused by concurrent IRQ handlers.

**Files:** `src/kernel/klog.c`, `src/kernel/drivers/serial.c`, `include/kernel/klog.h`, and all callers migrated from `printk`

### 11.1 Unified log format *(completed)* ✅

Standard line format used everywhere:

```
[  X.XXX] [LEVEL] subsystem: message
```

| Token | Values |
| ----- | ------ |
| Timestamp | `[  0.000]` — seconds space-padded to 3 digits, ms zero-padded to 3 |
| Level | `[INFO]` `[ OK ]` `[WARN]` `[FAIL]` `[CRIT]` |
| Subsystem | short lowercase tag, e.g. `uefi`, `mm`, `acpi`, `test` |

- [x] `klog.h` defines five levels: `LOG_DEBUG→[INFO]`, `LOG_INFO→[ OK ]`, `LOG_WARN→[WARN]`, `LOG_ERROR→[FAIL]`, `LOG_FATAL→[CRIT]`
- [x] All pre-`klog` callers migrated: `printk()` calls in `gfx_text.c`, `icon_store.c`, `image.c`, `image_scale.c`, `desktop.c`, `boot_desktop.c`, `terminal.c`, `gallery.c`, `ixfs_format.c`, `ixfs_cow.c`, `test_threads.c`, `version.c`, `boot_hw.c`, boot splash
- [x] Stale `#include "kernel/printk.h"` removed from ~41 files that no longer called `printk()`
- [x] Deprecated `include/kernel/log.h` + `src/kernel/log.c` deleted
- [x] Commit: `"kernel: Serial Output Phase 1 — unified klog format, remove stale printk includes"`

### 11.2 Serial line-mangling race fix *(completed)* ✅

**Root cause:** `klog` emitted a log line as ~20+ individual `serial_putchar` calls (one per character of timestamp, level, subsystem, message). The per-character spinlock released between every byte, allowing a timer IRQ handler's `klog` call to inject a full line mid-message.

**Fix:**
1. `serial.c` — extracted `serial_putchar_raw()` (unlocked); `serial_write()` now holds the spinlock for the **entire string**.
2. `klog.c` — formats the complete log line (`[ts] [LEVEL] sub: msg\n`) into a 256-byte stack buffer, then calls `serial_write()` once. Removed `vformat_emit()` and the old per-character serial helpers; framebuffer output reuses `e->message` from the ring buffer.

- [x] `serial_putchar_raw()` static helper, no lock — called only from within a held lock
- [x] `serial_write()` acquires `g_serial_lock` once, writes all bytes via `serial_putchar_raw()`, releases
- [x] `klog()` builds full serial line in `char line[256]`, calls `serial_write(line)` atomically
- [x] Verified: no interleaved lines in serial output; IRQ-sourced `[WARN]` lines no longer split ioapic route messages
- [x] Commit: `"kernel: fix serial line mangling by making klog write atomically"`

---

## 12. Multi-GPU GOP Handle Enumeration *(deferred — single-GPU works, multi-GPU renders to wrong display but won't crash)*
The current `init_gop()` uses `LocateProtocol()` which returns a single GOP handle — whichever the firmware happens to expose first. On machines with iGPU + dGPU, a Thunderbolt dock, or any secondary adapter, this may select the wrong display. Replace with `LocateHandleBuffer()` to enumerate all GOP handles, select the active display using the UEFI `ConOut` console path as a tiebreaker, record all framebuffers in `boot_info` so the kernel's future multi-head support can consume them, and leave the existing `boot_info.fb` (primary framebuffer) untouched so `framebuffer_init()` needs no changes.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`

> [!NOTE]
> → XREF: `04-drivers-hardware/TODO-08-gpu-display-drivers.md §8` — kernel multi-head `display_register_head()` will consume `boot_info.gop_handles[]` populated here. Bootloader identifies the primary and records all handles; the kernel decides what to do with each at runtime.

- [ ] Add to `boot_info.h`:
  ```c
  #define BOOT_GOP_MAX_HANDLES 4
  typedef struct {
      uint64_t fb_base;
      uint32_t width;
      uint32_t height;
      uint32_t pixel_format;  /* 0 = RGBX, 1 = BGRX */
  } boot_gop_handle_t;
  /* in struct boot_info: */
  uint32_t          gop_count;                          /* total GOP handles found */
  boot_gop_handle_t gop_handles[BOOT_GOP_MAX_HANDLES];  /* all found, [0] = primary */
  ```
- [ ] Replace `gBS->LocateProtocol(&gop_guid, NULL, (VOID **)&gop)` with `gBS->LocateHandleBuffer(ByProtocol, &gop_guid, NULL, &handle_count, &handles)` in `init_gop()`
- [ ] For each handle open GOP via `gBS->OpenProtocol(..., EFI_OPEN_PROTOCOL_GET_PROTOCOL)`; record `{FrameBufferBase, HorizontalResolution, VerticalResolution, pixel_format}` into `boot_info.gop_handles[i]` (up to `BOOT_GOP_MAX_HANDLES`); set `boot_info.gop_count`
- [ ] Primary selection: walk the device path of each handle and compare against `gST->ConsoleOutHandle`'s device path (via `EFI_DEVICE_PATH_PROTOCOL`); the handle whose device path is a prefix of, or equal to, the ConOut device path is the active display — use it as the primary; if no match, fall back to the handle with the largest `Width × Height`
- [ ] Call existing `gop_negotiate_mode()` only on the selected primary handle; `boot_info.fb` continues to describe the primary's framebuffer unchanged — `framebuffer_init()` requires no modification
- [ ] Free handle buffer: `gBS->FreePool(handles)` after all handles are recorded
- [ ] Serial log: `[BOOT] GOP: %u handle(s); primary handle %u (%ux%u, ConOut match=%s)` when `gop_count > 1`; single-handle path logs unchanged (`[BOOT] GOP: %ux%u 32bpp (mode N of M)`)
- [ ] Commit: `"boot: enumerate all GOP handles — LocateHandleBuffer, ConOut-path primary selection, boot_info.gop_handles[]"`

**Test checkpoint:** QEMU (single GPU): serial shows `[BOOT] GOP: 1 handle(s)`, existing behavior unchanged. Bare metal with iGPU + dGPU: serial shows `[BOOT] GOP: 2 handle(s); primary handle N (WxH, ConOut match=yes)`. `boot_info.gop_count` > 1, `boot_info.gop_handles[0]` = primary display.

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11                              | 🐧 Linux                              | 🚀 Impossible OS                          |
|----|----------------------------|------------------------------------|------------------------------------|-----------------------------------------|
| 💎 | Secure Boot shim           | ✅ MS-signed shim + WHQL          | ✅ rhboot shim (distro)            | ✅ §6 — shimx64 + MOK                  |
| 💎 | Secure Boot state          | ✅ Registry + WinVerifyTrust      | ✅ efivarfs SecureBoot             | 🔄 §5 — NVRAM reads; SRM deferred      |
| 💎 | UEFI runtime post-EBS      | ✅ Full RT preserved              | ✅ efi_call wrappers               | ✅ §1 — SVAM + RT pointers             |
| 💎 | UEFI variable access       | ✅ GetFirmwareEnvVar Win32        | ✅ efivarfs + efivar               | ✅ §2 — get/set/enumerate              |
| 💎 | GOP resolution             | ✅ Boot manager negotiates        | ✅ GRUB gfxmode + EFIFB            | ✅ §3 — auto + HiDPI + boot.conf       |
| 💎 | SMBIOS inventory           | ✅ WMI Win32_BIOS                 | ✅ /sys/firmware/dmi               | ✅ §4 — Type 0/1/4/17 → Registry       |
| 💎 | UEFI capsule update        | ✅ WU UEFI capsules               | ✅ fwupd                           | ⬜ §9                                  |
| 💎 | UEFI memory W^X            | ✅ Since Win10 1607               | ✅ EFI_MEMORY_ATTRIBUTES           | ⬜ §10                                 |
| ⭐ | In-bootloader OS menu      | ❌ Separate BCD/bootmgr           | ❌ GRUB is separate                | ⬜ §8 — integrated countdown           |
| ⭐ | Boot profile timeline      | ❌ ETW (binary, WPA)              | ❌ systemd-analyze (post-boot)     | ✅ §7 — boot-profile.log + JSON        |
| 💎 | Multi-GPU GOP              | ✅ LocateHandleBuffer             | ✅ grub handle buffer              | ⬜ §12 — ConOut primary select         |

> **After parity items:** Impossible OS will fully match Windows and Linux on Secure Boot, UEFI runtime, SMBIOS, capsule updates, and W^X enforcement. The exclusive items push beyond: the integrated countdown boot menu eliminates the need for a separate bootloader for dual-boot, the accent fade-in gives a branded first impression, and the structured JSON boot timeline makes performance regression testing trivial compared to WPA or systemd-analyze.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_uefi_boot()` (-> XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_uefi_boot.c` with:
  - `uefi_rt_available()` returns 1 after `uefi_runtime_init()` completes (runtime services preserved)
  - `uefi_var_get()` for `L"SecureBoot"` returns `STATUS_SUCCESS` or `STATUS_NOT_FOUND` (never crashes)
  - `uefi_var_get_u32()` / `uefi_var_set_u32()` roundtrip: write a test GUID variable, read back, values match
  - `boot_info.fb.width > 0` and `boot_info.fb.height > 0` (GOP negotiated a valid resolution)
  - `boot_info.hidpi == 1` when `boot_info.fb.width >= 2560`, else `0`
  - `smbios_get_system_uuid()` returns non-zero UUID on real hardware (all-zero acceptable on QEMU)
  - `boot_info.secure_boot_enabled` matches value read from UEFI `SecureBoot` variable
  - SMBIOS registry keys exist: `HKLM\HARDWARE\BIOS\BIOSVendor` is non-empty string
- [ ] Register in `test_runner_init()`: `test_register_uefi_boot()`
- [ ] Commit: `"test: add uefi_boot test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [x] Serial log shows `[UEFI] SetVirtualAddressMap OK` and `[UEFI] Runtime services: OK`
- [ ] `uefi_var_get(L"SecureBoot", ...)` returns 0 (disabled) in QEMU; returns 1 on Secure Boot–enabled hardware
- [ ] GOP negotiation log shows `[Boot] GOP: {W}x{H} 32bpp (mode N)` matching QEMU display resolution
- [ ] SMBIOS data appears in Registry under `HKLM\HARDWARE\BIOS\*` and `HKLM\HARDWARE\System\*`
- [ ] `BOOTX64.EFI` signed build present when `keys/MOK.key` + `keys/MOK.crt` exist; skipped silently when absent
- [ ] Boot menu appears when two GPT partitions are present; timer counts down; default boots without input
- [ ] `[UEFI] W^X enforced on N UEFI memory regions` in serial log (N > 0 on QEMU with OVMF)
- [ ] Commit: `"boot: uefi-hardening verified — runtime services, Secure Boot, GOP, SMBIOS, boot menu"`
