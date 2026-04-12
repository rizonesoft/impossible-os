# TODO-03 -- Boot Device Discovery & Fallback Chain

> **Goal:** The bootloader must correctly identify which device it booted from, load the kernel and `boot.conf` from that device (not a random filesystem), and support a priority-based fallback chain across SATA, NVMe, USB, and network devices. **Today** `parse_boot_conf()` still uses the first `LocateProtocol(SIMPLE_FILE_SYSTEM)` handle, and there is no cross-volume fallback -- risks wrong-disk config on multi-disk systems even though `load_kernel()` already prefers LoadedImage. Windows uses the BCD store + Loaded Image device path; GRUB uses device enumeration + search; Linux exposes boot entries via `efibootmgr`. This TODO finishes boot device identification, fallback, UEFI boot variables (BootOrder, BootCurrent, BootNext, optional Boot#### decode), partition GUID validation, removable media detection, boot device Registry population, and a pre-boot device health check so the OS boots reliably on any hardware configuration and exposes complete boot provenance to the kernel.

> [!IMPORTANT]
> **Current state (code-truth 2026-04-12):** §1-§2 done -- `efi_main()` resolves `LoadedImage->DeviceHandle` into `g_boot_device_handle` global. Both `parse_boot_conf()` and `load_kernel()` use the global with SimpleFS fallback to LocateProtocol. POST16 0xB090-0xB093 cover both steps. **Not implemented:** `boot_info` has no `boot_device_*` / UEFI boot-variable / partition-GUID fields yet (§3-§8). **Not implemented:** multi-volume fallback chain (§5), Boot#### decode (§12), Registry §9 population, §10 enumeration log, §11 health check.

---

## Inputs

- `src/boot/uefi/bootx64.c` -- `load_kernel()` and `parse_boot_conf()` filesystem access
- `src/boot/uefi/efi.h` -- UEFI protocol definitions (EFI_LOADED_IMAGE_PROTOCOL, EFI_DEVICE_PATH_PROTOCOL, EFI_BLOCK_IO_PROTOCOL)
- `include/kernel/boot_info.h` -- boot_info struct (needs boot device info)
- → XREF: `TODO-01-uefi-hardening-secureboot.md §2` -- UEFI variable services (`uefi_var_get` / `uefi_var_set`); this file reads BootOrder/BootCurrent pre-ExitBootServices
- → XREF: `TODO-01-uefi-hardening-secureboot.md §4` -- SMBIOS/registry hardware hive population (boot provenance complements device discovery)
- → XREF: `TODO-18-uefi-advanced.md §1` -- multi-OS detection and boot menu (not TODO-01 §8 -- that is serial klog)
- → XREF: `TODO-02-bootloader-error-recovery.md §3` -- fallback kernel search paths
- → XREF: `TODO-10-xhci-usb-boot.md §5` -- USB device handover
- → XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §1` -- vfs_probe consumes boot device info for C: assignment
- → XREF: `02-kernel-core/TODO-13-registry-completion.md` -- §9 `HKLM\SYSTEM\Boot\Device\*` requires `registry_init()` and write APIs from the registry engine (coordinate field names with kernel registry)

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
| 💎  |   2   | Filesystem access scoped to boot device            | §1             |  [/]   |
| 💎  |   3   | Boot device info in boot_info struct               | §1             |  [ ]   |
| 💎  |   4   | Boot device type detection (SATA/NVMe/USB/Net)     | §3             |  [ ]   |
| 💎  |   5   | Device fallback chain (priority-based)             | §2, §4         |  [ ]   |
| 💎  |   6   | UEFI boot variable reading (BootOrder/Current/Next)| §1             |  [ ]   |
| 💎  |   7   | Partition GUID extraction and validation           | §1             |  [ ]   |
| 💎  |   8   | Removable media detection                          | §1, §4         |  [ ]   |
| 💎  |   9   | Boot device Registry population                    | §3, §4, §7, §8 |  [ ]   |
| ⭐  |  10   | Boot device logging and diagnostics                | §1--§9, §12    |  [ ]   |
| ⭐  |  11   | Pre-boot device health check                       | §1, §7         |  [ ]   |
| 💎  |  12   | Boot#### `EFI_LOAD_OPTION` decode (diagnostics)    | §6             |  [ ]   |

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
- [x] Add matching `#define POST16_BL_DEV_*` names in `include/kernel/boot_init.h` in the `0xB000` bootloader range (and mirror in `bootx64.c` if locals remain) -- `POST16_BOOT_DEV` (0xB090) / `POST16_BOOT_DEV_OK` (0xB091) added; no collision with 0xB080-0xB085 USB/xHCI range
- [x] Commit: `"boot: identify boot device via EFI_LOADED_IMAGE_PROTOCOL"` (6b2aa73e)

**Test checkpoint:** Serial output shows `"Boot device: handle=0x..."` on all platforms (QEMU WHPX, TCG, VirtualBox, bare metal). Handle is non-zero. Verify on bare metal -- firmware LoadedImage behavior may differ from emulated.

> **Verified:** 2026-04-12 -- all 6 items confirmed. `g_boot_device_handle` global set in `efi_main()` before `parse_boot_conf()`; `load_kernel()` uses global with LocateProtocol fallback when device handle lacks SimpleFS; POST16 0xB090/0xB091 in `boot_init.h` + `bootx64.c`; smoke test pattern `"Boot device:"` matches both success and fallback. Codex adversarial: `parse_boot_conf` LocateProtocol gap deferred to §2 (explicit scope). Accepted: none.
> **Quality reviewed:** 2026-04-12 -- boot-code-quality 12 gates walked (all applicable pass). Codex quality: POST16 naming mismatch rejected (established `BL_*`/kernel convention, bootloader cannot include kernel headers per Gate 1). Dead code: old `load_kernel` local LoadedImage variables (`li_guid`, `loaded_image`) fully removed. Parity: matches Windows BCD DeviceHandle extraction + GRUB search. Accepted: none.

---

## 2. Filesystem Access Scoped to Boot Device

Replace `LocateProtocol(SIMPLE_FILE_SYSTEM)` with `HandleProtocol(DeviceHandle, SIMPLE_FILE_SYSTEM)` everywhere boot configuration and kernel loads must stay on the same volume.

- [x] In `parse_boot_conf()`: open filesystem from boot `DeviceHandle`, not global `LocateProtocol` -- `bootx64.c:2327-2345` uses `g_boot_device_handle` with HandleProtocol, fallback to LocateProtocol
- [x] In `load_kernel()`: same change -- already done in §1 (`bootx64.c:2450-2465`)
- [x] If `DeviceHandle` doesn't have `SIMPLE_FILE_SYSTEM_PROTOCOL`: fall back to `LocateProtocol` with warning -- both `parse_boot_conf()` and `load_kernel()` have the same fallback pattern
- [x] Log: `"[BOOT] Using boot device filesystem"` or `"[WARN] Boot device has no filesystem, using fallback"` -- both messages present
- [x] `POST16(0xB092)` before filesystem open, `POST16(0xB093)` after success -- `POST16_BL_BOOT_FS` / `POST16_BL_BOOT_FS_OK` in `bootx64.c`, `POST16_BOOT_FS` / `POST16_BOOT_FS_OK` in `boot_init.h`
- [ ] Commit: `"boot: scope filesystem access to boot device -- no more random disk"`

**Test checkpoint:** On QEMU with single disk, behavior unchanged. On multi-disk (SATA + NVMe test), kernel loads from correct device. Verify on bare metal multi-disk system -- firmware filesystem handle order differs from QEMU. If crash, check POST code: 0xB092 = filesystem open failed.

**Regression risk:** MEDIUM -- changes how filesystem is located. If `DeviceHandle` is wrong, falls back to old behavior.

---

## 3. Boot Device Info in boot_info

Pass boot device information to the kernel so it knows which device it booted from.

- [ ] Add to `boot_info`: `uint8_t boot_device_type` (0=unknown, 1=SATA, 2=NVMe, 3=USB, 4=network)
- [ ] Add to `boot_info`: `uint8_t boot_device_path[128]` -- UEFI device path as text string
- [ ] Populate from `DevicePathToText()` UEFI protocol (if available)
- [ ] Kernel logs: `"[BOOT] Booted from: %s (type=%u)"` during boot_info parsing
- [ ] Commit: `"boot: pass boot device type and path in boot_info"`

**Test checkpoint:** Kernel serial output shows `"Booted from: PciRoot(0x0)/Pci(0x2,0x0)/Sata(0x0,0xFFFF,0x0)"` or similar. Path format varies by firmware. Verify on bare metal -- real firmware produces longer device paths than QEMU.

---

## 4. Boot Device Type Detection

Classify the boot device as SATA, NVMe, USB, or network based on the device path.

- [ ] Add device path node type/subtype constants to `efi.h`: `MESSAGING_DEVICE_PATH` (0x03), `MSG_SATA_DP` (0x12), `MSG_NVME_NAMESPACE_DP` (0x17), `MSG_USB_DP` (0x05), `MSG_IPv4_DP` (0x0C), `MSG_IPv6_DP` (0x0D), `MEDIA_DEVICE_PATH` (0x04), `MEDIA_HARDDRIVE_DP` (0x01); add `EFI_DEVICE_PATH_PROTOCOL` struct (Type, SubType, Length[2])
- [ ] Parse UEFI device path nodes: `MESSAGING_DEVICE_PATH/MSG_SATA_DP` → SATA, `MSG_NVME_NAMESPACE_DP` → NVMe, `MSG_USB_DP` → USB, `MSG_IPv4_DP/MSG_IPv6_DP` → network
- [ ] If device path parsing unavailable: check PCI class code via `PciIo` protocol on the device handle
- [ ] Store result in `boot_info.boot_device_type`
- [ ] Commit: `"boot: detect boot device type from UEFI device path"`

**Test checkpoint:** USB boot → `boot_device_type=3`, SATA boot → `boot_device_type=1`. Verify on bare metal USB and SATA -- device path node structure varies by firmware vendor.

---

## 5. Device Fallback Chain

If the boot device's kernel is missing or corrupt, try other devices in priority order.

> [!NOTE]
> **CLAUDE.md:** QEMU WHPX emulated NVMe can be flaky; run SATA+NVMe fallback scenarios on **QEMU TCG**, VirtualBox, or bare metal so failures are not misread as bootloader bugs.

- [ ] Enumerate all `SIMPLE_FILE_SYSTEM_PROTOCOL` handles using `LocateHandleBuffer()`
- [ ] For each handle: check if `\boot\kernel.exe` exists (try to open, close immediately)
- [ ] Priority order: boot device first → SATA/NVMe → USB → other
- [ ] If kernel found on non-boot device: `"[WARN] Kernel not on boot device, using %s"` with device path
- [ ] If no device has kernel: trigger boot failure screen (→ XREF `TODO-02-bootloader-error-recovery.md §9`)
- [ ] `POST16(0xB094)` on fallback entry, `POST16(0xB095)` on fallback success or failure -- localizes fallback hangs on slow firmware
- [ ] Commit: `"boot: device fallback chain -- search all filesystems for kernel"`

**Test checkpoint:** Remove kernel from SATA disk, leave it on USB. Boot from SATA → bootloader finds kernel on USB with warning. Verify on bare metal -- firmware `LocateHandleBuffer` may return handles in different order than QEMU.

**Regression risk:** MEDIUM -- iterates all filesystem handles. If enumeration is slow on firmware, adds boot time.

---

## 6. UEFI Boot Variable Reading (BootOrder, BootCurrent, BootNext)

Read UEFI global boot variables so the bootloader knows which firmware boot entry was selected and whether a one-shot boot was requested. Windows reads these via BCD abstractions; Linux reads them via efibootmgr/efivarfs. Every production bootloader must honor BootNext for firmware update reboot flows.

- [ ] Add `EFI_GLOBAL_VARIABLE` GUID to `efi.h`: `{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}` (needed for all UEFI global variable reads)
- [ ] Read `BootCurrent` (UEFI global variable, UINT16): identifies which `Boot####` entry firmware selected for this boot; store in `boot_info.uefi_boot_current`
- [ ] Read `BootOrder` (UEFI global variable, UINT16 array): firmware priority list; store first 16 entries in `boot_info.uefi_boot_order[16]` with `boot_info.uefi_boot_order_count`
- [ ] Read `BootNext` (UEFI global variable, UINT16): if present, this is a one-shot boot override; store in `boot_info.uefi_boot_next` with `boot_info.uefi_boot_next_valid = 1`
- [ ] If `BootNext` is set: log `"[BOOT] BootNext=0x%04x (one-shot override)"` -- firmware deletes this variable after reading it, but log its presence for diagnostics
- [ ] Log: `"[BOOT] BootCurrent=0x%04x, BootOrder=[0x%04x, 0x%04x, ...]"` with variable contents
- [ ] Commit: `"boot: read UEFI boot variables -- BootOrder, BootCurrent, BootNext"`

**Test checkpoint:** Serial shows `"BootCurrent=0x0000"` (or valid entry number) on all UEFI platforms. `boot_info.uefi_boot_order_count > 0` on real firmware. On QEMU OVMF: BootOrder may be empty (acceptable).

---

## 7. Partition GUID Extraction and Validation

Extract the GPT partition GUID from the boot device's media device path node. This provides a stable, cross-boot identifier for the boot partition -- more reliable than device path text which can change when PCI topology changes. Windows BCD uses disk signature + partition GUID for stable boot volume identification; Linux uses PARTUUID.

- [ ] Walk the boot device's device path: look for `MEDIA_DEVICE_PATH` (Type 0x04) / `MEDIA_HARDDRIVE_DP` (SubType 0x01) node
- [ ] Extract `PartitionSignature` (16 bytes = GUID for GPT) and `MBRType` (0x02 = GPT, 0x01 = MBR)
- [ ] Store in `boot_info.boot_partition_guid[16]` (raw GUID bytes) and `boot_info.boot_partition_style` (0=unknown, 1=MBR, 2=GPT)
- [ ] If GPT: validate GUID is non-zero; if all-zero, log `"[WARN] Boot partition GUID is zero -- firmware may not support GPT"`
- [ ] Log: `"[BOOT] Boot partition: GUID=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x (GPT)"` or `"(MBR sig=0x%08x)"`
- [ ] Commit: `"boot: extract and validate boot partition GUID from device path"`

**Test checkpoint:** QEMU SATA boot: serial shows GPT partition GUID matching the EFI system partition GUID in the disk image. `boot_info.boot_partition_style == 2` (GPT) on all modern hardware. MBR systems (if any) show `boot_partition_style == 1`.

---

## 8. Removable Media Detection

Determine whether the boot device is removable (USB stick, external drive) or fixed (internal SATA/NVMe). Windows uses `DriveType` (removable/fixed) to adjust write-caching policy; Linux checks `/sys/block/sdX/removable`. Knowing removability at boot time lets the kernel apply safe cache defaults and warn about ejection.

- [ ] After obtaining boot `DeviceHandle` from §1: query `EFI_BLOCK_IO_PROTOCOL` on the handle via `HandleProtocol()`
- [ ] Read `BlockIo->Media->RemovableMedia` (BOOLEAN): TRUE for USB sticks, external drives, SD cards
- [ ] Read `BlockIo->Media->MediaPresent` (BOOLEAN): TRUE if media is currently inserted (relevant for card readers)
- [ ] Store in `boot_info.boot_device_removable` (0=fixed, 1=removable) and `boot_info.boot_media_present` (0/1)
- [ ] If removable: log `"[BOOT] Boot device is removable -- write-caching will be disabled by default"`
- [ ] Commit: `"boot: detect removable media via EFI_BLOCK_IO_PROTOCOL"`

**Test checkpoint:** USB boot on QEMU (`-device usb-storage`): `boot_device_removable == 1`. SATA boot: `boot_device_removable == 0`. Both: `boot_media_present == 1`. Verify on bare metal USB stick.

---

## 9. Boot Device Registry Population

Populate `HKLM\SYSTEM\Boot\Device\` Registry keys with boot device information so user-mode applications and diagnostics tools can query how the system booted. Windows populates `HKLM\SYSTEM\CurrentControlSet\Enum\` with boot device details; Linux exposes boot device info via `/sys/firmware/efi/`. This gives Impossible OS a central, queryable boot provenance record.

- [ ] After `registry_init()` completes in Phase 2 (`boot_storage.c`): call `boot_device_registry_init()` from `boot_storage.c` (not `boot_hw.c` -- Registry is unavailable in Phase 0)
- [ ] Write `HKLM\SYSTEM\Boot\Device\Type` (REG_DWORD): boot_device_type (1=SATA, 2=NVMe, 3=USB, 4=network)
- [ ] Write `HKLM\SYSTEM\Boot\Device\Path` (REG_SZ): boot_device_path text from boot_info
- [ ] Write `HKLM\SYSTEM\Boot\Device\PartitionGUID` (REG_SZ): formatted GUID string from boot_info
- [ ] Write `HKLM\SYSTEM\Boot\Device\PartitionStyle` (REG_DWORD): 1=MBR, 2=GPT
- [ ] Write `HKLM\SYSTEM\Boot\Device\Removable` (REG_DWORD): 0 or 1
- [ ] Write `HKLM\SYSTEM\Boot\Device\BootCurrent` (REG_DWORD): UEFI BootCurrent value
- [ ] Write `HKLM\SYSTEM\Boot\Device\BootNext` (REG_DWORD): UEFI BootNext value (0xFFFF if not set)
- [ ] Log: `"[BOOT] Boot device Registry populated: %s type=%u removable=%u"` with path and type
- [ ] Commit: `"boot: populate HKLM\SYSTEM\Boot\Device\ Registry with boot provenance"`

**Test checkpoint:** After boot, Registry query for `HKLM\SYSTEM\Boot\Device\Type` returns 1 (SATA) on QEMU default. `Path` is non-empty. `PartitionGUID` is formatted as `XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX`. Shell `reg query HKLM\SYSTEM\Boot\Device` (when shell reg command exists) shows all keys. Verify on bare metal -- all values should reflect real hardware.

---

## 10. Boot Device Logging and Diagnostics

Log the full boot device enumeration to serial for debugging. This is the comprehensive diagnostic section that ties together all previous sections.

- [ ] At boot start: enumerate all `SIMPLE_FILE_SYSTEM_PROTOCOL` handles using `LocateHandleBuffer()`
- [ ] For each: log device path text, device type (SATA/NVMe/USB), whether `\boot\kernel.exe` exists, and removable status
- [ ] Log UEFI `BootCurrent` variable: which boot entry firmware selected (from §6)
- [ ] Log `BootOrder` variable: full priority list (from §6)
- [ ] If `BootNext` was set: log the one-shot override (from §6)
- [ ] Log partition GUID for boot device (from §7)
- [ ] Summary line: `"[BOOT] Device summary: %u devices found, boot=%s (type=%u, %s, GUID=%s)"`
- [ ] Commit: `"boot: comprehensive boot device enumeration logging"`

**Test checkpoint:** Serial output shows numbered list of all available boot devices with kernel presence status, device type, and removable flag. Summary line present. Verify on QEMU multi-disk and bare metal.

---

## 11. Pre-Boot Device Health Check

Read basic health indicators from the boot device before loading the kernel. Neither Windows nor Linux performs a proactive disk health check at the bootloader stage -- both rely on post-boot kernel-mode SMART monitoring. An early warning at boot time gives the user a chance to back up before a failing disk causes data loss.

> [!TIP]
> **Competitive advantage:** A pre-kernel boot device health check is unique. Windows relies on `storport.sys` SMART polling after boot; Linux relies on `smartd` in user space. Neither warns at boot time. Impossible OS can show `"[WARN] Boot disk reports errors -- back up your data"` before the kernel even loads, giving the user maximum lead time to react.

- [ ] Query `EFI_BLOCK_IO_PROTOCOL.Media` on boot device: check `MediaPresent`, `ReadOnly`, `LogicalPartition`
- [ ] Read `BlockSize` and `LastBlock`: compute total capacity; log `"[BOOT] Boot disk: %u MiB (%s)"` with device type
- [ ] If `ReadOnly == TRUE` on a non-USB device: log `"[WARN] Boot disk is read-only -- possible hardware failure"` (read-only on fixed disk is abnormal)
- [ ] If `MediaPresent == FALSE`: log `"[CRIT] Boot media not present -- check cable connections"` and trigger error screen (→ XREF `TODO-02-bootloader-error-recovery.md §9`)
- [ ] If AHCI attached: attempt to read SATA Status register via PCI config space to check for link errors (DET field in PxSSTS); log `"[WARN] SATA link errors detected"` if non-zero error bits
- [ ] Commit: `"boot: pre-boot device health check -- early warning for failing disks"`

**Test checkpoint:** Normal boot: serial shows `"Boot disk: NNN MiB (SATA)"` with correct capacity. QEMU with read-only disk (`-drive ...,readonly=on`): serial shows `"read-only"` warning. Verify on bare metal -- SATA link status bits should be clean on healthy hardware.

**Regression risk:** LOW -- read-only queries. No writes to disk, no modification of boot path. If SATA status check fails (unsupported firmware), skip silently.

---

## 12. Boot#### Load Option Decode (Diagnostics)

Firmware boot entry **`Boot####`** variables hold an **`EFI_LOAD_OPTION`**: attributes, description, and the file/device path list for that menu entry. Linux **`efibootmgr -v`** and Windows **BCD** tooling expose this; it is the authoritative link between **BootCurrent** and the path the firmware *intended* to run. Decoding it catches mismatches when **`bootx64.efi`** was launched from a fallback path while **BootCurrent** points at another entry.

- [ ] After reading `BootCurrent` (§6): format variable name `Boot####` with lowercase hex (e.g. `Boot0000`) per UEFI naming rules
- [ ] Call `GetVariable("Boot####", EFI_GLOBAL_VARIABLE_GUID, ...)`; if `EFI_NOT_FOUND`, log once and skip (some VMs have minimal NVRAM)
- [ ] Parse `EFI_LOAD_OPTION`: `Attributes` (UINT32), `FilePathListLength` (UINT16), description UTF-16 string, then packed `EFI_DEVICE_PATH_PROTOCOL` nodes for `FilePathListLength` bytes
- [ ] Log: `"[BOOT] Boot%04x: %ls"` (description) and a second line with `DevicePathToText` for the file path list (truncate if > 256 chars for serial)
- [ ] Optional sanity: compare the loaded **`EFI_LOADED_IMAGE_PROTOCOL.FilePath`** to the boot entry file path -- if they diverge, log `"[WARN] LoadedImage path differs from BootCurrent entry (fallback boot?)"`
- [ ] Commit: `"boot: decode Boot#### EFI_LOAD_OPTION for BootCurrent diagnostics"`

**Test checkpoint:** On firmware with a populated `Boot0000` (or current entry), serial shows description + device path text. On OVMF with empty entries, skip is silent (no hang). Verify on bare metal -- description strings are UTF-16 vendor strings.

---

## OS Comparison

| ⭐   | Feature                     | 🪟 Win11                      | 🐧 Linux                 | 🚀 Impossible OS |
| --- | --------------------------- | ---------------------------- | ----------------------- | --------------- |
| 💎   | Boot device identification  | ✅ BCD + device path          | ✅ GRUB search command   | ✅ §1-§2 done |
| 💎   | Multi-device fallback       | ✅ BCD boot order             | ✅ GRUB menu entries     | ⬜ §5            |
| 💎   | Boot device type in kernel  | ✅ Registry boot info         | ✅ /proc/cmdline root=   | ⬜ §3--§4        |
| 💎   | Boot variable reading       | ✅ BCD reads BootOrder        | ✅ efibootmgr/efivarfs   | ⬜ §6            |
| 💎   | Boot#### option decode      | ✅ BCD / bcdedit              | ✅ efibootmgr -v         | ⬜ §12           |
| 💎   | BootNext one-shot boot      | ✅ SetFirmwareEnvVar          | ✅ efibootmgr -n         | ⬜ §6            |
| 💎   | Partition GUID validation   | ✅ BCD disk signature         | ✅ root=PARTUUID=        | ⬜ §7            |
| 💎   | Removable media detection   | ✅ DriveType removable        | ✅ sysfs removable flag  | ⬜ §8            |
| 💎   | Boot device Registry        | ✅ HKLM Enum + MountedDevices | ✅ /sys/firmware/efi     | ⬜ §9            |
| ⭐   | Full device enumeration log | ❌ Hidden in Event Log        | ❌ Not logged            | ⬜ §10 🚀         |
| ⭐   | Pre-boot disk health check  | ❌ Post-boot SMART only       | ❌ Post-boot smartd only | ⬜ §11 🚀         |

> **After parity items:** Impossible OS matches Windows and Linux on all boot device discovery fundamentals: device identification via LoadedImage, UEFI boot variable reading, partition GUID validation, removable media detection, and Registry population. The exclusive items push beyond: comprehensive serial logging of the full device enumeration (neither competitor exposes this), and a pre-boot disk health check at the UEFI stage that gives users early warning of failing hardware before the kernel even loads.

---

## Unit Tests

> Boot device discovery runs in UEFI bootloader context -- not kernel test framework.
> Use `scripts/test-smoke.sh` serial pattern matching for boot-level validation.
> Kernel-side boot_info fields can be validated via kernel unit tests.
> Test registration: `src/kernel/test/test_runner.c`, `include/kernel/test/test.h`

- [ ] Add smoke test patterns to `scripts/test-smoke.sh`:
  - Serial line `"[BOOT] Boot device: handle="` present (§1 -- LoadedImage identification)
  - Serial line `"[BOOT] Using boot device filesystem"` present (§2 -- scoped filesystem access)
  - Serial line `"[BOOT] Booted from:"` present (§3 -- device path in boot_info)
  - Serial line `"[BOOT] BootCurrent="` present (§6 -- UEFI boot variables read)
  - Serial line `"[BOOT] Boot"` with 4-digit hex and description present (§12 -- `EFI_LOAD_OPTION` decode) when firmware exposes `Boot####`
  - Serial line `"[BOOT] Boot partition:"` present (§7 -- partition GUID extracted)
  - Serial line `"[BOOT] Boot disk:"` present (§11 -- health check ran)
- [ ] Create `src/kernel/test/test_boot_device.c` with:
  - `boot_info.boot_device_type` is a valid enum value (0--4, not out of range)
  - `boot_info.boot_device_path` is non-empty (at least 1 character)
  - `boot_info.boot_device_type != 0` (UNKNOWN) when booted from a real device (QEMU always has SATA)
  - `boot_info.boot_partition_style == 2` (GPT) on QEMU (§7)
  - `boot_info.boot_partition_guid` is non-zero (§7)
  - `boot_info.boot_device_removable == 0` on SATA boot (§8)
  - `boot_info.boot_media_present == 1` (§8)
  - `boot_info.uefi_boot_order_count <= 16` (§6 -- array bounds)
  - Registry key `HKLM\SYSTEM\Boot\Device\Type` exists and matches `boot_info.boot_device_type` (§9)
- [ ] Register in `test_runner_init()`: `test_register_boot_device()`
- [ ] Commit: `"test: add boot device discovery smoke and unit tests"`

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

**Test runner:** `scripts\debug\run-boot-tests.bat` (SUITE=boot)

---

## History

| Date | Action | Summary |
| --- | --- | --- |
| 2026-04-10 | validate | validate-todo-file: Inputs `→ XREF`; §5/§11 error-screen refs use full `TODO-02` filename; Inputs XREF TODO-13 for §9 Registry keys; Unit Tests + Verification closed with **Test checkpoint**; **Test runner** `run-boot-tests.bat`; History added; reciprocal Inputs on TODO-02 (§5,§11) and TODO-13 (§9 keys). Flags: §9 blocked until registry init + TODO-13 APIs; `test_boot_device.c` not in tree yet (planned). |
| 2026-04-10 | gap-analysis | Web research: UEFI Loaded Image + Boot Manager (`uefi.org` specs); Linux `efibootmgr`/BootOrder/BootNext; GRUB `search`/UUID; systemd-boot multi-ESP. Code-truth: `load_kernel` uses LoadedImage; `parse_boot_conf` still `LocateProtocol`; no `boot_info` device fields; POST16 `0xB090`--`0xB095` not in `boot_init.h`. Added **§12** Boot#### `EFI_LOAD_OPTION` decode; IMPORTANT + §1/§2 notes; Impl row 12 + OS row; §10 depends on §12; Unit Tests smoke line; POST16 define bullet §1. |
| 2026-04-10 | validate | validate-todo-file: §1 NOTE range §2--§12; POST16 XREF pinned to `TODO-07 §1`; Outcome health-check wording matches §11 (no SMART claim); Verification + Commit cover §12; 12 flat `##` sections, Commit+Test-last OK; Inputs paths + `run-boot-tests.bat` exist; OS parity rows map to §1--§12. Flags: §9 still 10 bullets; external blockers TODO-01 §2, TODO-13 for Registry. |
