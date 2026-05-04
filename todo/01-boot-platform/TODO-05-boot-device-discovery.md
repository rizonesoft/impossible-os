---
schema_version: 1
id: boot-device-discovery
domain: 01-boot-platform
status: active
title: "TODO-05 -- Boot Device Discovery & Fallback Chain"
---

# TODO-05 -- Boot Device Discovery & Fallback Chain

> **Goal:** The bootloader must correctly identify which device it booted from, load the kernel and `boot.conf` from that device (not a random filesystem), and support a priority-based fallback chain across local SATA, NVMe, and USB devices. **Today** `parse_boot_conf()` still uses the first `LocateProtocol(SIMPLE_FILE_SYSTEM)` handle, and there is no cross-volume fallback -- risks wrong-disk config on multi-disk systems even though `load_kernel()` already prefers LoadedImage. Windows uses the BCD store + Loaded Image device path; GRUB uses device enumeration + search; Linux exposes boot entries via `efibootmgr`. This TODO finishes boot device identification, fallback, UEFI boot variables (BootOrder, BootCurrent, BootNext, optional Boot#### decode), partition GUID validation, removable media detection, boot device Registry population, and a pre-boot device health check so the OS boots reliably on any local hardware configuration and exposes complete boot provenance to the kernel. Network/PXE/HTTP boot is owned by [TODO-25](TODO-25-network-pxe-http-boot.md).

> [!IMPORTANT]
> **Current state (code-truth 2026-04-12; gap-audit 2026-05-03):** §1--§12 shipped. Gap audit (Codex 1x, 8 findings) added §13 (boot variable capability surface: `EFI_LOAD_OPTION.Attributes`, `BootOptionSupport`, `OsIndicationsSupported`, Boot#### Description in registry) and §14 (NVMe NSID + EUI-64, PCI device/function, SD/eMMC classification) -- both `[ ]`. `BOOT_INFO_VERSION=5` today; bumps to 6 when §13 lands.

---

## Inputs

- `src/boot/uefi/bootx64.c` -- `load_kernel()` and `parse_boot_conf()` filesystem access
- `src/boot/uefi/efi.h` -- UEFI protocol definitions (EFI_LOADED_IMAGE_PROTOCOL, EFI_DEVICE_PATH_PROTOCOL, EFI_BLOCK_IO_PROTOCOL)
- `include/kernel/boot_info.h` -- boot_info struct (needs boot device info)
- -> XREF: `TODO-01-boot-protocol-abi-handoff.md §1,§10` -- canonical owner map for `boot_device_*`, Boot####, and related handoff fields that this TODO already populates
- → XREF: `TODO-02-uefi-hardening-secureboot.md §2` -- UEFI variable services (`uefi_var_get` / `uefi_var_set`); this file reads BootOrder/BootCurrent pre-ExitBootServices
- → XREF: `TODO-02-uefi-hardening-secureboot.md §5` -- SMBIOS/registry hardware hive population (boot provenance complements device discovery)
- → XREF: `TODO-27-uefi-advanced.md §1` -- multi-OS detection and boot menu (not TODO-02 §8 -- that is serial klog)
- → XREF: `TODO-03-bootloader-error-recovery.md §5` -- fallback kernel search paths
- → XREF: `TODO-17-xhci-usb-boot.md §2` -- USB device handover
- → XREF: `TODO-25-network-pxe-http-boot.md` -- owns PXE/TFTP/HTTP boot protocol discovery, asset loading, and network provenance
- → XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §1` -- vfs_probe consumes boot device info for C: assignment
- → XREF: `02-kernel-core/TODO-14-registry-completion.md` -- §10 `HKLM\SYSTEM\Boot\Device\*` requires `registry_init()` and write APIs from the registry engine (coordinate field names with kernel registry)

---

## Outcome

- Bootloader uses `LoadedImage->DeviceHandle` to identify the actual boot device.
- Kernel and boot.conf are always loaded from the boot device's filesystem, not a random one.
- `boot_info` contains the boot device type (SATA, NVMe, USB, network), device path, removable flag, and partition GUID.
- If boot device fails, fallback chain tries other devices in priority order.
- UEFI boot variables (BootOrder, BootCurrent, BootNext) are read, logged, and passed to the kernel; optional **Boot####** `EFI_LOAD_OPTION` decode matches `efibootmgr -v` / firmware boot manager detail.
- Partition GUID from the boot device's GPT entry is validated and stored for stable device identification.
- Removable media (USB sticks, external drives) are flagged so the kernel can apply write-caching policy.
- Boot device info populated in Registry: `HKLM\SYSTEM\Boot\Device\*` with type, path, GUID, removable flag.
- Pre-boot device health check uses `EFI_BLOCK_IO_PROTOCOL.Media` (present/read-only/capacity) plus optional SATA link sanity reads -- not full SMART, but catches absent media and obvious read-only fixed disks early.

---

## Implementation Order

| ⭐  | Order | Deliverable                                        | Depends On     | Status |
| --- | :---: | -------------------------------------------------- | -------------- | :----: |
| 💎  |   1   | Boot device identification via LoadedImage         | --             |  [x]   |
| 💎  |   2   | Filesystem access scoped to boot device            | §1             |  [x]   |
| 💎  |   3   | Boot device info in boot_info struct               | §1             |  [x]   |
| 💎  |   4   | Boot device type detection (SATA/NVMe/USB/Net)     | §3             |  [x]   |
| 💎  |   5   | Device fallback chain (priority-based)             | §2, §4         |  [x]   |
| 💎  |   6   | UEFI boot variable reading (BootOrder/Current/Next)| §1             |  [x]   |
| 💎  |   7   | Partition GUID extraction and validation           | §1             |  [x]   |
| 💎  |   8   | Removable media detection                          | §1, §4         |  [x]   |
| 💎  |   9   | Boot device Registry population                    | §3, §4, §7, §8 |  [x]   |
| ⭐  |  10   | Boot device logging and diagnostics                | §1--§9, §12    |  [x]   |
| ⭐  |  11   | Pre-boot device health check                       | §1, §7         |  [x]   |
| 💎  |  12   | Boot#### `EFI_LOAD_OPTION` decode (diagnostics)    | §6             |  [x]   |
| 💎  |  13   | Extended boot variable capability surface          | §6, §9, §12    |  [ ]   |
| 💎  |  14   | Local boot device path detail capture              | §3, §4, §9     |  [ ]   |

> 💎 = parity -- Windows (BCD + device path) and Linux (GRUB device search) both do this.
> ⭐ = exclusive -- detailed boot device diagnostics with full enumeration, and proactive disk health check before kernel load.

---

## 1. Boot Device Identification via LoadedImage

Use UEFI `EFI_LOADED_IMAGE_PROTOCOL` to find which device the bootloader was loaded from.

> [!NOTE]
> **Code-truth:** `load_kernel()` already resolves the boot filesystem through LoadedImage (`bootx64.c` ~970--980). This section finishes the contract: store the handle globally, add POST16 + serial proof, and feed §2--§12. **Downgrade scope:** treat "implement HandleProtocol chain" as **verify + unify** with any duplicate logic.

- [x] After `efi_main()` entry: call `HandleProtocol(ImageHandle, EFI_LOADED_IMAGE_PROTOCOL_GUID, &loaded_image)` -- `bootx64.c` efi_main §1 block, before `parse_boot_conf()`
- [x] Extract `loaded_image->DeviceHandle` -- stored in `g_boot_device_handle` global
- [x] Store `DeviceHandle` in a single bootloader global (or `boot_info` handoff struct) for `parse_boot_conf()` and for §3--§8 consumers -- `load_kernel()` refactored to use `g_boot_device_handle` instead of its own local LoadedImage lookup
- [x] Log: `"[BOOT] Boot device: handle=0x%x"` on serial -- full 64-bit hex printed
- [x] `POST16(0xB090)` on entry, `POST16(0xB091)` on success -- `POST16_BL_BOOT_DEV` / `POST16_BL_BOOT_DEV_OK` in `bootx64.c`
- [x] Add matching `#define POST16_BL_*` names in `src/boot/uefi/bootx64.c` in the `0xB000` bootloader range -- `POST16_BL_BOOT_DEV` (0xB090) / `POST16_BL_BOOT_DEV_OK` (0xB091) added; no collision with 0xB080-0xB085 USB/xHCI range. Note: the 0xB0xx range is owned by `bootx64.c` (source of truth for serial-emitted codes); the previously-mirrored `POST16_BOOT_DEV*` defines in `include/kernel/boot_init.h` were removed as part of TODO-01 §10 POST16 manifest work (had 0 kernel callers and carried misleading semantic labels).
- [x] Commit: `"boot: identify boot device via EFI_LOADED_IMAGE_PROTOCOL"` (6b2aa73e)

**Test checkpoint:** Serial output shows `"Boot device: handle=0x..."` on all platforms (QEMU WHPX, TCG, VirtualBox, bare metal). Handle is non-zero. Verify on bare metal -- firmware LoadedImage behavior may differ from emulated.

> **Notes:**
> - `g_boot_device_handle` is the static `EFI_HANDLE` global consumed by 11 sites across §1-§12; reset to NULL on both LoadedImage-failure paths so consumers gate cleanly.
> - POST16 0xB090 entry / 0xB091 success live in `bootx64.c` (sole owner of the 0xB0xx range); kernel-side mirror was retired with the TODO-01 §10 POST16 manifest work.
> - Re-reviewed 2026-05-04 (Codex 3x, 0H+0M+2L fixed): L1 stale stamp wording about `boot_init.h` mirror corrected; L2 tangential xHCI DMA hex16 truncation fixed in same commit (bootx64.c:6342, 6364 now use `serial_early_print_hex64`).

> **Verified:** 2026-04-12 ship | re-verified 2026-05-04 | commit `6b2aa73e` (ship) + this commit (re-review) | 6/6 items | build OK
> **Quality reviewed:** 2026-05-04 | Codex 3x (adversarial, consistency, perf) | 0H+0M+2L fixed | scope: boot-code-quality

---

## 2. Filesystem Access Scoped to Boot Device

Replace `LocateProtocol(SIMPLE_FILE_SYSTEM)` with `HandleProtocol(DeviceHandle, SIMPLE_FILE_SYSTEM)` everywhere boot configuration and kernel loads must stay on the same volume.

- [x] `parse_boot_conf()` opens FS via `HandleProtocol(g_boot_device_handle, SimpleFS)`; **no** LocateProtocol fallback (intentional cross-volume safety)
- [x] `load_kernel()` and `load_staged_payloads()` use the `locate_boot_fs()` helper (HandleProtocol + LocateProtocol fallback) -- §5 kernel-search chain accepts cross-volume kernel discovery
- [x] On boot device SimpleFS miss: `parse_boot_conf` fails closed with `boot_config` defaults (no random-disk read); `load_kernel` / staged payloads continue via fallback
- [x] Logs: `"[BOOT] Using boot device filesystem"` (parse success) or `"[WARN] Boot device has no SimpleFS for boot.conf -- skipping parse, using boot_config defaults"` (fail-closed)
- [x] `POST16(0xB092)` before filesystem open, `POST16(0xB093)` after success -- `POST16_BL_BOOT_FS` / `POST16_BL_BOOT_FS_OK` in `bootx64.c` (source of truth for 0xB0xx range; `boot_init.h` mirror removed in TODO-01 §10)
- [x] Commit: `"boot: scope filesystem access to boot device -- no more random disk"` (d7ee0bcc)

**Test checkpoint:** On QEMU with single disk, behavior unchanged. On multi-disk (SATA + NVMe test), kernel loads from correct device. Verify on bare metal multi-disk system -- firmware filesystem handle order differs from QEMU. If crash, check POST code: 0xB092 = filesystem open failed.

**Regression risk:** MEDIUM -- changes how filesystem is located. If `DeviceHandle` is wrong, falls back to old behavior.

> **Notes:**
> - Two distinct fallback policies: `parse_boot_conf` fails closed (no LocateProtocol -- cross-volume safety); `load_kernel` / `load_staged_payloads` fall back via `locate_boot_fs()`.
> - POST16 0xB092 entry / 0xB093 success-after-OpenVolume in `bootx64.c` (sole owner of 0xB0xx range).
> - Re-reviewed 2026-05-04 (Codex 4x: adv+cons+perf+re-adv, 1H+1M+1L fixed): NULL-fs guard, doc/code drift corrected, vpd.c mirror extended.

> **Verified:** 2026-04-12 ship | re-verified 2026-05-04 | commit `d7ee0bcc` (ship) + this commit (re-review) | 6/6 items | build OK
> **Quality reviewed:** 2026-05-04 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+1M+1L fixed | scope: boot-code-quality

---

## 3. Boot Device Info in boot_info

Pass boot device information to the kernel so it knows which device it booted from.

- [x] Add to `boot_info`: `uint8_t boot_device_type` (0=unknown, 1=SATA, 2=NVMe, 3=USB, 4=network) -- `boot_info.h:486`, `bootx64.c:286`; type set to 0 (unknown) by default, §4 will classify
- [x] Add to `boot_info`: `uint8_t boot_device_path[128]` -- `boot_info.h:488`, `bootx64.c:288`; UCS-2 to ASCII conversion with 127-char cap
- [x] Populate from `DevicePathToText()` UEFI protocol (if available) -- `bootx64.c` efi_main §3 block; `EFI_DEVICE_PATH_TO_TEXT_PROTOCOL` + `EFI_DEVICE_PATH_PROTOCOL` added to `efi.h`
- [x] Kernel logs: `"[BOOT] Booted from: %s (type=%u)"` during boot_info parsing -- `boot_hw.c` after last_boot_error check
- [x] Commit: `"boot: pass boot device type and path in boot_info"` (6c3e723f)

**Test checkpoint:** Kernel serial output shows `"Booted from: PciRoot(0x0)/Pci(0x2,0x0)/Sata(0x0,0xFFFF,0x0)"` or similar. Path format varies by firmware. Verify on bare metal -- real firmware produces longer device paths than QEMU.

> **Notes:**
> - `boot_device_type` (uint8) at offset 21924, `boot_device_path[128]` at 21928 -- pinned via `_Static_assert` in both kernel header and bootloader mirror.
> - DevicePathToText chain: `HandleProtocol(DevicePath)` -> `LocateProtocol(DevicePathToText)` -> `ConvertDevicePathToText` -> printable-ASCII sanitizer (127-cap) -> `FreePool`.
> - Re-reviewed 2026-05-04 (Codex 4x: adv+cons+perf+re-adv, 1H+1M fixed): H added missing offset pins; M replaced lossy `(text[j] & 0x7F)` mask with explicit ASCII sanitizer (printable 0x20-0x7E unchanged, everything else -> '?').

> **Verified:** 2026-04-12 ship | re-verified 2026-05-04 | commit `6c3e723f` (ship) + this commit (re-review) | 5/5 items | build OK
> **Quality reviewed:** 2026-05-04 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+1M fixed | scope: boot-code-quality

---

## 4. Boot Device Type Detection

Classify the boot device as SATA, NVMe, USB, or network based on the device path.

- [x] Add DP node constants to `efi.h`: `EFI_DP_TYPE_MESSAGING/MEDIA/END` + Messaging subtypes (`SATA 0x12`, `NVME 0x17`, `USB 0x05`, `IPV4 0x0C`, `IPV6 0x0D`) + `MEDIA_HARDDRIVE 0x01`
- [x] Classify type by substring scan of `DevicePathToText` ASCII output for `Sata(` / `NVMe(` / `USB(` / `IPv4(` / `IPv6(` -- `bootx64.c:7809-7836`
- [x] Why text not nodes: `HandleProtocol` returns a partition-rooted path that omits parent Messaging nodes; `DevicePathToText` walks to root and emits them
- [x] Bounded scan `pi + 5 <= sizeof(boot_device_path)` so the 4-byte lookahead never over-reads the 128-byte buffer
- [x] If `DevicePathToText` unavailable or path empty: type stays 0; Messaging-node constants are forward-reserved for §14 (NSID / PCI BDF / SD / eMMC)
- [x] Store result in `boot_info.boot_device_type` -- written directly in the substring loop
- [x] Commit: `"boot: detect boot device type from UEFI device path"` (50049dd7)

**Test checkpoint:** USB boot → `boot_device_type=3`, SATA boot → `boot_device_type=1`. Verify on bare metal USB and SATA -- device path node structure varies by firmware vendor.

> **Notes:**
> - Classifier is text-substring on `DevicePathToText` ASCII output (`bootx64.c:7809-7836`); raw-node walk would miss parent Messaging nodes that `HandleProtocol` strips on partition handles.
> - Constants in `efi.h:790-800` are forward-reserved for §14 node-walk classifier (NSID / PCI BDF / SD / eMMC).
> - Re-reviewed 2026-05-04 (Codex 4x, 1H+1M fixed): bounded the 5-byte lookahead, rewrote stamp/checklist to match actual implementation.

> **Verified:** 2026-04-12 ship | re-verified 2026-05-04 | commit `50049dd7` (ship) + this commit (re-review) | 6/6 items | build OK
> **Quality reviewed:** 2026-05-04 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+1M fixed | scope: boot-code-quality

---

## 5. Device Fallback Chain

If the boot device's kernel is missing or corrupt, try other devices in priority order.

> [!NOTE]
> **CLAUDE.md:** QEMU WHPX emulated NVMe can be flaky; run SATA+NVMe fallback scenarios on **QEMU TCG**, VirtualBox, or bare metal so failures are not misread as bootloader bugs.

- [x] Enumerate all `SIMPLE_FILE_SYSTEM_PROTOCOL` handles using `LocateHandleBuffer()` -- `bootx64.c` load_kernel §5 block, after boot device path search fails
- [x] For each handle: check if `\boot\kernel.exe` (or `\kernel.exe`, `\EFI\ImpossibleOS\kernel.exe`) exists via Open+Close -- reuses the existing 3-path search per handle
- [x] Priority order: boot device first (tried before fallback), then firmware enumeration order -- SATA/NVMe/USB ordering is firmware-dependent; boot device is always tried first via `g_boot_device_handle`, skipped in fallback loop
- [x] If kernel found on non-boot device: `"[WARN] Kernel found on non-boot device at \boot\kernel.exe"` -- logs which path matched
- [x] If no device has kernel: returns `EFI_NOT_FOUND` which triggers `boot_fatal()` error screen in efi_main (→ XREF `TODO-03 §2`)
- [x] `POST16(0xB094)` on fallback entry, `POST16(0xB095)` on fallback exit -- `POST16_BL_FALLBACK` / `POST16_BL_FALLBACK_OK` in `bootx64.c` + `boot_init.h`
- [x] Commit: `"boot: device fallback chain -- search all filesystems for kernel"` (c5eacf9f)

**Test checkpoint:** Remove kernel from SATA disk, leave it on USB. Boot from SATA → bootloader finds kernel on USB with warning. Verify on bare metal -- firmware `LocateHandleBuffer` may return handles in different order than QEMU.

**Regression risk:** MEDIUM -- iterates all filesystem handles. If enumeration is slow on firmware, adds boot time.

> **Notes:**
> - Fallback chain at `bootx64.c:4325-4443` runs only when boot-device kernel search fails: LocateHandleBuffer + 3-path search per non-boot handle, security-violation halts whole search, media errors skip volume.
> - POST16 0xB094 fallback-entry / 0xB095 fallback-success in `bootx64.c` (sole owner of 0xB0xx range).
> - Re-reviewed 2026-05-04 (Codex 4x, 1H fixed): added EFI_SUCCESS+NULL postcondition guard on the fallback HandleProtocol call -- mirrors the boot-FS-scoping `locate_boot_fs` defense. Recovery path no longer crashes on misbehaving firmware.

> **Verified:** 2026-04-12 ship | re-verified 2026-05-04 | commit `c5eacf9f` (ship) + this commit (re-review) | 7/7 items | build OK
> **Quality reviewed:** 2026-05-04 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H fixed | scope: boot-code-quality

---

## 6. UEFI Boot Variable Reading (BootOrder, BootCurrent, BootNext)

Read UEFI global boot variables so the bootloader knows which firmware boot entry was selected and whether a one-shot boot was requested. Windows reads these via BCD abstractions; Linux reads them via efibootmgr/efivarfs. Every production bootloader must honor BootNext for firmware update reboot flows.

- [x] Add `EFI_GLOBAL_VARIABLE` GUID to `efi.h`: `EFI_GLOBAL_VARIABLE_GUID {8BE4DF61-93CA-11D2-AA0D-00E098032B8C}` at `efi.h:762`
- [x] Read `BootCurrent` (UINT16): `rt->GetVariable(u"BootCurrent", ...)` -> `boot_info.uefi_boot_current` -- `bootx64.c` efi_main §6 block
- [x] Read `BootOrder` (UINT16 array, up to 16): `rt->GetVariable(u"BootOrder", ...)` -> `boot_info.uefi_boot_order[16]` with count -- capped at 16 entries
- [x] Read `BootNext` (UINT16, optional): `rt->GetVariable(u"BootNext", ...)` -> `boot_info.uefi_boot_next` with `uefi_boot_next_valid=1` if present
- [x] If `BootNext` is set: `"[BOOT] BootNext=0x%04x (one-shot override)"` logged
- [x] Log: `"[BOOT] BootCurrent=0x%04x"` and `"[BOOT] BootOrder=[0x%04x,0x%04x,...]"` logged separately for clarity
- [x] Commit: `"boot: read UEFI boot variables -- BootOrder, BootCurrent, BootNext"` (77a4823c)

**Test checkpoint:** Serial shows `"BootCurrent=0x0000"` (or valid entry number) on all UEFI platforms. `boot_info.uefi_boot_order_count > 0` on real firmware. On QEMU OVMF: BootOrder may be empty (acceptable).

> **Notes:**
> - 3 GetVariable calls (BootCurrent/BootOrder/BootNext) at `bootx64.c:8046-8113` with size validation (`sz==sizeof(...)`) and 128-entry BootOrder stack buffer; truncates to first 16 in boot_info storage.
> - Defaults: 0xFFFF for current/next, valid=0, order_count=0; gates whole block on `(rt && rt->GetVariable)`.
> - Re-reviewed 2026-05-04 (Codex 4x, 1H fixed): added per-field offset pins for uefi_boot_next (22058), uefi_boot_next_valid (22060), uefi_boot_order_count (22061), uefi_boot_order (22064) in both kernel header and bootloader mirror.

> **Verified:** 2026-04-12 ship | re-verified 2026-05-04 | commit `77a4823c` (ship) + this commit (re-review) | 7/7 items | build OK
> **Quality reviewed:** 2026-05-04 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H fixed | scope: boot-code-quality

---

## 7. Partition GUID Extraction and Validation

Extract the GPT partition GUID from the boot device's media device path node. This provides a stable, cross-boot identifier for the boot partition -- more reliable than device path text which can change when PCI topology changes. Windows BCD uses disk signature + partition GUID for stable boot volume identification; Linux uses PARTUUID.

- [x] Walk the boot device's device path: look for `MEDIA_DEVICE_PATH` (Type 0x04) / `MEDIA_HARDDRIVE_DP` (SubType 0x01) node -- integrated into §4 walk loop with `node_len >= 42` guard
- [x] Extract `PartitionSignature` (16 bytes at offset 24 for GPT) and `MBRType` (offset 40: 0x02=GPT, 0x01=MBR) -- per UEFI spec Table 10-58
- [x] Store in `boot_info.boot_partition_guid[16]` and `boot_info.boot_partition_style` (0=unknown, 1=MBR, 2=GPT) -- `boot_info.h` + `bootx64.c` mirror, BOOT_INFO_VERSION bumped to 4
- [x] If GPT: validate GUID is non-zero; if all-zero: `"[WARN] Boot partition GUID is zero -- firmware may not support GPT"` logged
- [x] Log: `"[BOOT] Boot partition: GUID=XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX (GPT)"` or `"(MBR sig=0xXXXXXXXX)"` -- UEFI mixed-endian GUID format
- [x] Commit: `"boot: extract and validate boot partition GUID from device path"` (d125a47c)

**Test checkpoint:** QEMU SATA boot: serial shows GPT partition GUID matching the EFI system partition GUID in the disk image. `boot_info.boot_partition_style == 2` (GPT) on all modern hardware. MBR systems (if any) show `boot_partition_style == 1`.

> **Verified:** 2026-04-12 -- all 6 items confirmed. HardDrive DP node extraction in shared walk at `bootx64.c:4740-4760`. Both MBRType and SignatureType validated (GPT: mbr_type==0x02 && sig_type==0x02; MBR: mbr_type==0x01 && sig_type==0x01). node_len>=42 guard. `walked+node_len<=1024` cap prevents reading past walk boundary. GUID formatted in UEFI mixed-endian. All-zero GUID warned. Codex adversarial: walk-cap overread fixed, SignatureType check added. Accepted: none.
> **Quality reviewed:** 2026-04-12 -- boot-code-quality gates walked. EFI_DP_SUBTYPE_END_ENTIRE kept as forward-reserve for §4's single-instance rationale. No dead code. Shared walk is single O(n) pass. UEFI spec Table 10-58 compliance. Accepted: none.

---

## 8. Removable Media Detection

Determine whether the boot device is removable (USB stick, external drive) or fixed (internal SATA/NVMe). Windows uses `DriveType` (removable/fixed) to adjust write-caching policy; Linux checks `/sys/block/sdX/removable`. Knowing removability at boot time lets the kernel apply safe cache defaults and warn about ejection.

- [x] After obtaining boot `DeviceHandle` from §1: query `EFI_BLOCK_IO_PROTOCOL` on the handle via `HandleProtocol()` -- `bootx64.c` efi_main §8 block, NULL checks on bio and bio->Media
- [x] Read `BlockIo->Media->RemovableMedia` (BOOLEAN): converted to 0/1 via ternary
- [x] Read `BlockIo->Media->MediaPresent` (BOOLEAN): converted to 0/1, default 1 (assume present if BlockIO unavailable)
- [x] Store in `boot_info.boot_device_removable` and `boot_info.boot_media_present` -- carved from `_part_pad[3]` (same struct size), BOOT_INFO_VERSION bumped to 5
- [x] If removable: `"[BOOT] Boot device is removable -- write-caching will be disabled by default"`, else `"[BOOT] Boot device is fixed"`. If !MediaPresent: `"[WARN] Boot media not present"`
- [x] Commit: `"boot: detect removable media via EFI_BLOCK_IO_PROTOCOL"` (e4bd76be)

**Test checkpoint:** USB boot on QEMU (`-device usb-storage`): `boot_device_removable == 1`. SATA boot: `boot_device_removable == 0`. Both: `boot_media_present == 1`. Verify on bare metal USB stick.

> **Verified:** 2026-04-12 -- all 6 items confirmed. HandleProtocol(BlockIO) with NULL checks on bio and bio->Media at `bootx64.c:4835-4847`. BOOLEAN->0/1 via ternary. USB default removable when BlockIO unavailable. Fields carved from _part_pad (same struct size). BOOT_INFO_VERSION=5 both sides. Codex adversarial: approved, no findings. Codex quality: approved.
> **Quality reviewed:** 2026-04-12 -- boot-code-quality 13 gates walked. G13 spec compliance: UEFI BOOLEAN handled per spec (non-zero=TRUE). No dead _part_pad references. O(1) single HandleProtocol call. Parity: matches Windows DriveType + Linux sysfs removable. Accepted: none.

---

## 9. Boot Device Registry Population

Populate `HKLM\SYSTEM\Boot\Device\` Registry keys with boot device information so user-mode applications and diagnostics tools can query how the system booted. Windows populates `HKLM\SYSTEM\CurrentControlSet\Enum\` with boot device details; Linux exposes boot device info via `/sys/firmware/efi/`. This gives Impossible OS a central, queryable boot provenance record.

- [x] After `registry_init()` completes in Phase 2: `boot_device_populate_registry()` called from `registry_populate_defaults()` in `registry.c:1720` (not boot_hw.c -- Registry unavailable in Phase 0)
- [x] Write `HKLM\SYSTEM\Boot\Device\Type` (REG_DWORD): via `RegSetDword(hKey, "Type", ...)` in `boot_hw.c`
- [x] Write `HKLM\SYSTEM\Boot\Device\Path` (REG_SZ): via `RegSetString(hKey, "Path", ...)` from `g_boot_info.boot_device_path`
- [x] Write `HKLM\SYSTEM\Boot\Device\PartitionGUID` (REG_SZ): GUID formatted as `XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX` in UEFI mixed-endian; MBR as 8-char hex
- [x] Write `HKLM\SYSTEM\Boot\Device\PartitionStyle` (REG_DWORD): 1=MBR, 2=GPT
- [x] Write `HKLM\SYSTEM\Boot\Device\Removable` (REG_DWORD): 0 or 1
- [x] Write `HKLM\SYSTEM\Boot\Device\BootCurrent` (REG_DWORD): from `g_boot_info.uefi_boot_current`
- [x] Write `HKLM\SYSTEM\Boot\Device\BootNext` (REG_DWORD): from `g_boot_info.uefi_boot_next` (0xFFFF if not set)
- [x] Log: `"Boot device Registry populated: %s type=%u removable=%u"` via klog
- [x] Commit: `"boot: populate HKLM\SYSTEM\Boot\Device\ Registry with boot provenance"` (8522c805)

**Test checkpoint:** After boot, Registry query for `HKLM\SYSTEM\Boot\Device\Type` returns 1 (SATA) on QEMU default. `Path` is non-empty. `PartitionGUID` is formatted as `XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX`. Shell `reg query HKLM\SYSTEM\Boot\Device` (when shell reg command exists) shows all keys. Verify on bare metal -- all values should reflect real hardware.

> **Verified:** 2026-04-12 -- all 10 items confirmed. `boot_device_populate_registry()` at `boot_hw.c:426`, called from `registry.c:1722` after registry_init(). 8 values: Type, Path, PartitionGUID (GPT/MBR/empty), PartitionStyle, Removable, BootCurrent, BootNext. RegCreateKeyEx failure returns early. GUID mixed-endian matches §7. Codex adversarial: unchecked RegSet returns rejected (matches smbios pattern, pool has capacity). Accepted: test_boot_device.c registry assertion (Unit Tests section scope).
> **Quality reviewed:** 2026-04-12 -- kernel-code-quality 11 gates walked. No dead code. GUID byte order consistent with §7 bootloader. 8 RegSet calls O(1) each. Parity: matches Windows HKLM\\Enum + Linux /sys/firmware/efi. Accepted: none.

---

## 10. Boot Device Logging and Diagnostics

Log the full boot device enumeration to serial for debugging. This is the comprehensive diagnostic section that ties together all previous sections.

- [x] At boot start: enumerate all `SIMPLE_FILE_SYSTEM_PROTOCOL` handles via `LocateHandleBuffer()` -- `bootx64.c` efi_main §10 block after §6 boot variables
- [x] For each: log device path text (80-char truncated), kernel presence (`[kernel]` tag), removable status (`[removable]`/`[fixed]`), boot device marked `*BOOT*` -- per-handle Open+Close for kernel check, BlockIO for removable, DevicePathToText for path
- [x] Log UEFI `BootCurrent` variable -- already logged in §6 block above (`[BOOT] BootCurrent=0x...`)
- [x] Log `BootOrder` variable -- already logged in §6 block above (`[BOOT] BootOrder=[...]`)
- [x] If `BootNext` was set -- already logged in §6 block above (`[BOOT] BootNext=0x... (one-shot override)`)
- [x] Log partition GUID for boot device -- already logged in §7 block above (`[BOOT] Boot partition: GUID=...`)
- [x] Summary line: `"[BOOT] Device summary: N devices found, boot=SATA (fixed)"` with device count and type/removable status
- [x] Commit: `"boot: comprehensive boot device enumeration logging"` (93005d23)

**Test checkpoint:** Serial output shows numbered list of all available boot devices with kernel presence status, device type, and removable flag. Summary line present. Verify on QEMU multi-disk and bare metal.

> **Verified:** 2026-04-12 -- all 8 items confirmed. LocateHandleBuffer enumeration at `bootx64.c:4946`. Per-handle: Open/Close kernel check, BlockIO tri-state removable, DevicePathToText 80-char. Boot device marked *BOOT*. Summary with nb[20] buffer. BootCurrent/BootOrder/BootNext from §6. Partition GUID from §7. Codex adversarial: approved, no findings. Accepted: unconditional probe (watchdog guards, gatable later if needed).
> **Quality reviewed:** 2026-04-12 -- boot-code-quality 13 gates walked. No dead GUIDs. O(n) enumeration. Tri-state removable per G13 spec compliance. Exclusive feature -- neither Windows nor Linux logs full device enumeration at bootloader level. Accepted: none.

---

## 11. Pre-Boot Device Health Check

Read basic health indicators from the boot device before loading the kernel. Neither Windows nor Linux performs a proactive disk health check at the bootloader stage -- both rely on post-boot kernel-mode SMART monitoring. An early warning at boot time gives the user a chance to back up before a failing disk causes data loss.

> [!TIP]
> **Competitive advantage:** A pre-kernel boot device health check is unique. Windows relies on `storport.sys` SMART polling after boot; Linux relies on `smartd` in user space. Neither warns at boot time. Impossible OS can show `"[WARN] Boot disk reports errors -- back up your data"` before the kernel even loads, giving the user maximum lead time to react.

- [x] Query `EFI_BLOCK_IO_PROTOCOL.Media` on boot device: check `MediaPresent`, `ReadOnly`, `LogicalPartition` -- `bootx64.c` §11 block after §8 removable detection
- [x] Read `BlockSize` and `LastBlock`: compute capacity as `(LastBlock+1)*BlockSize / 1MiB`; log `"[BOOT] Boot disk: NNN MiB (SATA)"` -- BlockSize==0 guarded
- [x] If `ReadOnly == TRUE` on a non-USB device: `"[WARN] Boot disk is read-only -- possible hardware failure"` -- USB excluded (USB sticks with write-protect switch are normal)
- [x] If `MediaPresent == FALSE`: `"[WARN] Boot media not present (reported by firmware -- may be stale)"` -- warning only (boot_fatal would false-trigger on partition handles where we already loaded from the device)
- [x] SATA link status via PCI: skipped -- requires AHCI BAR MMIO access from UEFI bootloader which is risky on real firmware (BAR may not be mapped, MMIO access could crash). Media fields cover critical health indicators. Bare-metal SATA link errors are caught by the kernel's AHCI driver at Port 0 init.
- [x] Commit: `"boot: pre-boot device health check -- early warning for failing disks"` (a9a6e5c3)

**Test checkpoint:** Normal boot: serial shows `"Boot disk: NNN MiB (SATA)"` with correct capacity. QEMU with read-only disk (`-drive ...,readonly=on`): serial shows `"read-only"` warning. Verify on bare metal -- SATA link status bits should be clean on healthy hardware.

**Regression risk:** LOW -- read-only queries. No writes to disk, no modification of boot path. If SATA status check fails (unsupported firmware), skip silently.

> **Verified:** 2026-04-12 -- all 6 items confirmed. §8+§11 merged into single HandleProtocol(BlockIO) block. Capacity with overflow guard (LastBlock+blocks*BlockSize). ReadOnly on non-USB. MediaPresent warning-only (not fatal -- partition handle stale state). LogicalPartition diagnostic. SATA link status skipped (AHCI BAR MMIO risk). Codex adversarial: MediaPresent reverted from fatal to warn, capacity overflow guarded. Codex quality: duplicate HandleProtocol refactored to single call. Accepted: SATA link status (kernel AHCI driver handles this).
> **Quality reviewed:** 2026-04-12 -- boot-code-quality 13 gates walked. Single BlockIO lookup shared between §8+§11. Overflow-safe UINT64 capacity math. Exclusive feature -- neither Windows nor Linux checks disk health at bootloader stage. Accepted: none.

---

## 12. Boot#### Load Option Decode (Diagnostics)

Firmware boot entry **`Boot####`** variables hold an **`EFI_LOAD_OPTION`**: attributes, description, and the file/device path list for that menu entry. Linux **`efibootmgr -v`** and Windows **BCD** tooling expose this; it is the authoritative link between **BootCurrent** and the path the firmware *intended* to run. Decoding it catches mismatches when **`bootx64.efi`** was launched from a fallback path while **BootCurrent** points at another entry.

- [x] After reading `BootCurrent` (§6): format variable name `Boot####` with lowercase hex via UCS-2 CHAR16 array -- `bootx64.c` §12 block inside §6's `if (rt && rt->GetVariable)` scope
- [x] Call `GetVariable("Boot####", EFI_GLOBAL_VARIABLE_GUID, ...)`; `EFI_NOT_FOUND` is silent (some VMs have minimal NVRAM) -- 512-byte stack buffer, `lo_sz > 6` guard
- [x] Parse `EFI_LOAD_OPTION`: Attributes at [0..3], FilePathListLength at [4..5] (LE), Description at [6..] (NUL-terminated CHAR16), FilePathList after Description NUL -- bounds-checked against `lo_sz`
- [x] Log: `"[BOOT] Boot%04x: <description>"` (80-char truncated ASCII) and `"[BOOT]   Path: <device path>"` (120-char truncated via DevicePathToText) -- FreePool on fp_txt
- [x] FilePath comparison: skipped -- diagnostic-only and the device path is already visible in §3's log and §12's Path line for manual comparison.
- [x] Commit: `"boot: decode Boot#### EFI_LOAD_OPTION for BootCurrent diagnostics"` (74b9072f)

**Test checkpoint:** On firmware with a populated `Boot0000` (or current entry), serial shows description + device path text. On OVMF with empty entries, skip is silent (no hang). Verify on bare metal -- description strings are UTF-16 vendor strings.

> **Verified:** 2026-04-12 -- all 6 items confirmed. Boot#### name formatted with uppercase hex (UEFI spec Section 3.1.2) at `bootx64.c:4975`. GetVariable into 2048-byte buffer, EFI_BUFFER_TOO_SMALL logged, EFI_NOT_FOUND silent. EFI_LOAD_OPTION parsed with bounds checks. Device path validated (END_ENTIRE within fp_len) before ConvertDevicePathToText. Codex adversarial: lowercase hex fixed to uppercase (spec compliance). Accepted: FilePath comparison (manual via §3+§12 output).
> **Quality reviewed:** 2026-04-12 -- boot-code-quality 13 gates walked. G13: uppercase hex per UEFI spec, device path validated before ConvertDevicePathToText. No dead code. O(1) per boot. Parity: matches Windows BCD + Linux efibootmgr -v. Accepted: none.

---

## 13. Extended Boot Variable Capability Surface

§6 captured `BootCurrent` / `BootOrder` / `BootNext`; §12 logs Boot#### description and FilePathList. The remaining `EFI_LOAD_OPTION` Attributes word, two firmware-side capability variables (`BootOptionSupport`, `OsIndicationsSupported`), and the human-readable Boot#### description text are still log-only or untouched. Win11 BCDEdit and Linux `efibootmgr -v` both surface these. Read-only `OsIndicationsSupported` is **distinct** from TODO-27 §2's write-path ban on `OsIndications` -- the read side carries no capsule risk.

- [ ] Decode `EFI_LOAD_OPTION.Attributes` (UINT32 at offset 0) for `BootCurrent`'s Boot#### per UEFI 2.10 §3.1.3 (ACTIVE / FORCE_RECONNECT / HIDDEN / CATEGORY mask)
- [ ] Read `BootOptionSupport` (UINT32) global per UEFI 2.10 §3.1.4 (KEY/APP/SYSPREP/COUNT bits; absent => 0)
- [ ] Read `OsIndicationsSupported` (UINT64) global per UEFI 2.10 §3.3 -- READ-only firmware capability, distinct from TODO-27 §2 write-path ban
- [ ] Add `boot_info` fields and bump `BOOT_INFO_VERSION` 5 -> 6 in kernel header + bootloader mirror; refresh manifest dump-fields.inc; offset pin via `_Static_assert`
- [ ] Persist in `boot_device_populate_registry()`: `BootCurrentAttributes` + `BootOptionSupport` + `OsIndicationsSupportedLo/Hi` + `Description` (REG_SZ <=64) under `HKLM\SYSTEM\Boot\Device\`
- [ ] Serial log: `attrs=0x%08x (active=%u hidden=%u cat=%s)` plus `BootOptionSupport / OsIndicationsSupported` line
- [ ] `POST16(0xB0A0)` entry / `POST16(0xB0A1)` ok -- `POST16_BL_BOOT_VAR_EXT` / `_OK` (0xB096-0xB09C reserved by TODO-02 §13 ESP-integrity + rollback)
- [ ] Unit tests in `test_boot_device.c`: reserved-bit-zero on `boot_option_support`, NUL-termination of `boot_description`, ACTIVE bit consistency (TEST_SKIP if firmware NVRAM empty)
- [ ] Commit: `"boot: extended boot variable capability surface (Attributes + BootOptionSupport + OsIndicationsSupported + Description)"`

**Test checkpoint:** Serial shows `BootCurrent attrs=0x00000001` (LOAD_OPTION_ACTIVE) on standard QEMU OVMF, `BootOptionSupport=0x00000311` on firmware reporting KEY+APP+COUNT, `Description="UEFI ImpossibleOS"` on firmware with populated entries. Registry returns the four new values. Verify on bare metal -- vendor firmware sets additional capability bits and richer descriptions.

---

## 14. Local Boot Device Path Detail Capture

§4 classifies the boot device by Messaging-node subtype (SATA/NVMe/USB/network) but discards the per-bus identifiers Win11 (`MSFT_Disk.UniqueId` / `BusType`) and Linux (`/sys/class/nvme/nvmeX/nsid`, sysfs PCI topology) both expose. NVMe boots lose the namespace ID and EUI-64; PCI device/function never reaches `boot_info`; SD card and eMMC boots (modern laptops, tablets) classify as `boot_device_type=0` because the enum stops at four bus types. Capture the cheap detail at parse time, extend the enum, and persist to registry.

- [ ] Add subtype constants to `efi.h`: `EFI_DP_MSG_SD` (0x1A) per UEFI 2.10 §10.3.4.24, `EFI_DP_MSG_EMMC` (0x1D) per §10.3.4.27
- [ ] Extend `boot_device_type` enum: 5=SD, 6=eMMC; update §4 log strings + `test_boot_device.c` range to 0..6
- [ ] In §4 walk: when NVMe SubType (0x17) and `node_len >= 20`, copy NSID (UINT32 +4) + EUI-64 (8 BE +8) into `boot_info.boot_nvme_nsid` + `boot_nvme_eui64[8]` per UEFI 2.10 §10.3.4.21
- [ ] In §4 walk: capture last PCI node Device + Function (UINT8 each) per §10.3.2.1 into `boot_pci_device` / `_function`; sentinel 0xFF when not on PCI
- [ ] Bump `BOOT_INFO_VERSION` (piggyback on §13's bump if same release, else 7)
- [ ] Persist in `boot_device_populate_registry()`: `NamespaceId` (REG_DWORD), `NamespaceEui64` (REG_BINARY 8B; omit if NVMe + all-zero), `PciDevice` / `PciFunction` (REG_DWORD; 0xFF sentinel)
- [ ] Serial log: `[BOOT] Boot device: NVMe NSID=%u EUI-64=XX:..:XX` / `Boot device type: SD` or `eMMC`
- [ ] Unit tests: enum range 0..6, NSID nonzero => EUI-64 populated (or sentinel), PCI device <= 31 + function <= 7 OR both 0xFF
- [ ] Commit: `"boot: capture NVMe NSID/EUI-64 + PCI device/function + SD/eMMC classification"`

**Test checkpoint:** NVMe boot on QEMU `-device nvme`: serial shows `Boot device: NVMe NSID=1 EUI-64=...`, registry `NamespaceId=1`. SATA boot: `NamespaceId=0`, no `NamespaceEui64`. SD/eMMC boot on bare-metal tablet: `boot_device_type=5` or `6`. Verify on bare metal -- vendor PCIe NVMe + embedded eMMC Surface-class devices cover the modern boot surface.

---

## OS Comparison

| ⭐  | Feature                     | 🪟 Win11                      | 🐧 Linux                  | 🚀 Impossible OS              |
| --- | --------------------------- | ------------------------------ | ------------------------- | ------------------------------ |
| 💎  | Boot device identification  | ✅ BCD + device path           | ✅ GRUB search command    | ✅ §1-§2 done                 |
| 💎  | Multi-device fallback       | ⚠️ BCD recovery only           | ⚠️ GRUB menu only         | ✅ §5 cross-volume kernel scan |
| 💎  | Boot device type in kernel  | ✅ HKLM Enum BusType           | ⚠️ sysfs (post-boot only) | ✅ §3-§4 done                 |
| 💎  | Boot variable reading       | ✅ via UEFI Runtime API        | ✅ efibootmgr/efivarfs    | ✅ §6 done                    |
| 💎  | Boot#### option decode      | ✅ BCD / bcdedit               | ✅ efibootmgr -v          | ✅ §12 done                   |
| 💎  | BootNext one-shot boot      | ✅ SetFirmwareEnvVar           | ✅ efibootmgr -n          | ✅ §6 done                    |
| 💎  | Partition GUID validation   | ✅ BCD disk signature          | ✅ root=PARTUUID=         | ✅ §7 done                    |
| 💎  | Removable media detection   | ✅ DriveType removable         | ✅ sysfs removable flag   | ✅ §8 done                    |
| 💎  | Boot device Registry        | ✅ HKLM Enum + MountedDevices  | ✅ /sys/firmware/efi      | ✅ §9 done                    |
| 💎  | Boot#### attrs + caps       | ✅ BCDEdit metadata            | ✅ efibootmgr -v          | ⬜ §13 planned                |
| 💎  | Boot device bus topology    | ✅ MSFT_Disk UniqueId/BusType  | ✅ sysfs nsid + PCI BDF   | ⬜ §14 planned                |
| ⭐  | Full device enumeration log | ❌ Hidden in Event Log         | ❌ Not logged             | ✅ §10 done                   |
| ⭐  | Pre-boot disk health check  | ❌ Post-boot SMART only        | ❌ Post-boot smartd only  | ✅ §11 done                   |

> **After parity items:** Impossible OS matches Windows and Linux on all boot device discovery fundamentals: device identification via LoadedImage, UEFI boot variable reading, partition GUID validation, removable media detection, and Registry population. The exclusive items push beyond: comprehensive serial logging of the full device enumeration (neither competitor exposes this), and a pre-boot disk health check at the UEFI stage that gives users early warning of failing hardware before the kernel even loads.

---

## Unit Tests

> Boot device discovery runs in UEFI bootloader context -- not kernel test framework.
> Use `scripts/test-smoke.sh` serial pattern matching for boot-level validation.
> Kernel-side boot_info fields can be validated via kernel unit tests.
> Test registration: `src/kernel/test/test_runner.c`, `include/kernel/test/test.h`

- [x] Smoke test patterns: skipped per feedback -- don't add patterns to test-smoke.sh (breaks Windows QEMU tests)
- [x] Create `src/kernel/test/test_boot_device.c` with 9 test suites: type valid (0--4), path non-empty, type known (not UNKNOWN on QEMU), partition style GPT, partition GUID non-zero, SATA not removable (with SKIP for non-SATA), media present, boot order count bounded, Registry Type matches boot_info
- [x] Register in `test_runner_init()`: `test_register_boot_device()` under `/* Boot */` category at `test_runner.c:231`
- [x] Commit: `"test: add boot device discovery unit tests"` (9d230ab5)

> **Done:** 9 suites, 11 assertions -- registered in `test_runner_init()` as `test_register_boot_device()` (2026-04-12)

**Test checkpoint:** `scripts/test-smoke.sh` matches all listed `[BOOT]` serial patterns on a reference QEMU boot; `bash scripts/test.sh SUITE=boot` (or `make test-boot`) passes after `test_register_boot_device()` and `test_boot_device.c` land. Tests avoid CLAUDE.md forbidden boot-path side effects in kernel tests.

---

## Verification

- [ ] **Multi-disk test**: QEMU with SATA + NVMe -- kernel loads from correct device. Verify on bare metal multi-disk system.
- [ ] **USB boot test**: kernel on USB only -- bootloader finds it, logs device type. `boot_device_removable == 1`. Verify on bare metal USB stick.
- [ ] **Fallback test**: kernel missing from boot device -- fallback finds it on another device with warning.
- [ ] **Boot variables test**: `BootCurrent` and `BootOrder` logged on serial. `BootNext` logged when set (test with `efibootmgr -n` if available).
- [ ] **Boot#### decode test** (§12): on firmware with NVRAM boot entries, serial shows `Boot####` description + device path lines; skip path stays non-fatal on minimal OVMF.
- [ ] **Partition GUID test**: serial shows valid GPT GUID for boot partition. Registry `PartitionGUID` matches.
- [ ] **Removable detection test**: USB boot shows `removable=1`; SATA boot shows `removable=0`.
- [ ] **Registry population test**: after boot, all `HKLM\SYSTEM\Boot\Device\*` keys present with correct values.
- [ ] **Health check test**: serial shows `"Boot disk: NNN MiB"` with correct capacity on all platforms. Read-only disk shows warning.
- [ ] **Normal boot regression**: all platforms (QEMU WHPX, TCG, VirtualBox, bare metal) boot cleanly with new diagnostics in serial. No regressions.
- [ ] Commit: `"boot: boot device discovery complete -- device, fallback, BootOrder/BootCurrent/BootNext/Boot####, Registry, health check"`

**Test checkpoint:** Every Verification bullet above passes on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal; POST16 codes `0xB090`--`0xB095` localize bootloader failures as documented in §1--§5.

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 9 suites, 0 failures