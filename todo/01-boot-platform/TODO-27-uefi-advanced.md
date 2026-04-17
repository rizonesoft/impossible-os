# TODO-27 -- UEFI Advanced Features

> **Goal:** Advanced UEFI boot features beyond the core boot path: UEFI capsule firmware updates, W^X memory enforcement on UEFI runtime regions, multi-GPU GOP enumeration, extended Secure Boot state variables with enforcement policy, extended SMBIOS type parsing, DBX revocation list synchronization, and advanced multi-OS menu integration. These are production polish features -- the OS boots and runs correctly without them.

> [!NOTE]
> Split from TODO-02 (UEFI Hardening & Secure Boot). TODO-02 covers §1-§8 (runtime, variables, GOP, SMBIOS, Secure Boot state, shim, boot UX polish, serial klog) plus open §9 ops backlog (SBAT doc, DB registry mirror, EBS retry). This TODO covers deferred advanced UEFI features. The general boot entry store/menu policy is owned by TODO-07; this file's §1 is only the UEFI-specific multi-OS discovery and chainload extension.

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) -- UEFI bootloader
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) -- boot info struct
- [`include/kernel/uefi_runtime.h`](../../include/kernel/uefi_runtime.h) -- UEFI runtime wrappers
- [`include/kernel/smbios.h`](../../include/kernel/smbios.h) -- SMBIOS parser
- -> XREF: `TODO-02-uefi-hardening-secureboot.md` -- core §1-§8 + §9 ops parity backlog; this TODO extends it
- -> XREF: `TODO-02-uefi-hardening-secureboot.md §9` -- SBAT/DB registry/EBS retry must not duplicate capsule write path owned here §2
- -> XREF: `TODO-03-bootloader-error-recovery.md §3` -- GOP timeout and headless fallback wrap enumeration from this file §6
- -> XREF: `TODO-04-firmware-table-platform-inventory.md §8` -- ESRT inventory mirror feeds capsule policy in this file §4
- -> XREF: `TODO-07-boot-entry-store-menu-policy.md §4` -- boot menu framework; this file §1 adds UEFI multi-OS discovery and chainload entries
- -> XREF: `04-drivers-hardware/TODO-17-gpu-display-drivers.md §6` -- multi-head display consumes `boot_info.gop_handles[]` from §1
- -> XREF: `02-kernel-core/TODO-10-kernel-security-hardening.md` -- kernel lockdown triggered by §5 enforcement policy
- -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` -- chassis type from §4 distinguishes desktop vs laptop
- -> XREF: `18-future-research/TODO-04-secureboot-tpm.md §3` -- PK/KEK/db key hierarchy builds on §5 and §1

## Outcome

- UEFI multi-OS discovery and chainload entries feed the TODO-07 boot menu.
- UEFI capsule delivery path for firmware updates via ESRT.
- W^X enforcement on UEFI runtime memory regions.
- Multi-GPU GOP enumeration with ConOut-path primary selection.
- Extended Secure Boot state (SetupMode, AuditMode, DeployedMode) exposed to kernel and registry.
- SMBIOS Type 2/3/16/19 parsed into registry for Device Manager and System Properties.
- DBX revocation list freshness check with proactive stale warning.

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On          | Status |
| --- | :---: | ---------------------------------------- | ------------------- | :----: |
| ⭐  |   1   | UEFI multi-OS detection and chainload entries | T07 §1-§4      |  [ ]   |
| 💎  |   2   | UEFI capsule update and ESRT             | T02 §2              |  [ ]   |
| 💎  |   3   | UEFI memory attributes (W^X)            | T02 §1, T24 §1      |  [ ]   |
| 💎  |   4   | Multi-GPU GOP enumeration                | T02 §3              |  [ ]   |
| 💎  |   5   | Secure Boot extended state + enforcement | T02 §5              |  [ ]   |
| 💎  |   6   | SMBIOS extended type parsing             | T02 §4              |  [ ]   |
| 💎  |   7   | DBX revocation list sync                 | T02 §2, §5          |  [ ]   |

---

## 1. UEFI Multi-OS Detection and Chainload Entries

Detect other OS partitions from GPT and contribute chainload entries to the TODO-07 boot menu when the user has multiple OSes installed.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`

- [ ] After GPT partition scan: check each partition type GUID for Linux, Windows, and other EFI system partitions
- [ ] If any non-Impossible-OS partition found: add up to 8 UEFI chainload entries to the TODO-07 boot entry store
- [ ] Let TODO-07 render the countdown boot menu and resolve keyboard input/policy
- [ ] For non-Impossible entries: chainload via `LoadImage` + `StartImage` for EFI boot paths
- [ ] Commit: `"boot: multi-OS GPT detection and countdown text-mode boot menu"`

**Test checkpoint:** QEMU with two GPT partitions: TODO-07 boot menu includes 2 entries, countdown from 3, auto-selects Impossible OS. Bare metal dual-boot: detects Windows/Linux, chainload works.

## 2. UEFI Capsule Update and ESRT

Parse the ESRT firmware resource table and implement the UEFI capsule delivery path for firmware updates.

**Files:** `src/kernel/uefi_capsule.c` (new), `include/kernel/uefi_capsule.h` (new)

- [ ] `esrt_init()`: locate `EFI_SYSTEM_RESOURCE_TABLE` in UEFI config tables; walk entries; extract firmware type, GUID, current/last-attempt version, status
- [ ] Write to Registry: `HKLM\HARDWARE\Firmware\{GUID}\FwType`, `FwVersion`, `LastAttemptVersion`, `LastAttemptStatus`
- [ ] `capsule_update_request(path)`: read capsule, write to ESP `\EFI\UpdateCapsule\`, set `OsIndications` bit 0, reboot
- [ ] `capsule_check_result()`: read `OsIndicationsSupported` and `CapsuleReportGuid` at boot
- [ ] Commit: `"kernel: ESRT firmware table parse + UEFI capsule update delivery"`

**Test checkpoint:** QEMU: `[ESRT] N firmware entries found`. Registry `HKLM\HARDWARE\Firmware\{GUID}\FwVersion` populated.

## 3. UEFI Memory Attributes (W^X)

Enforce write-XOR-execute on UEFI runtime memory regions by walking the `EFI_MEMORY_ATTRIBUTES_TABLE`.

**Files:** `src/kernel/uefi_runtime.c`, `src/kernel/mm/vmm.c`

- [ ] Locate `EFI_MEMORY_ATTRIBUTES_TABLE` in UEFI config tables
- [ ] Walk entries: for RW pages, set NX bit via `vmm_set_nx(virt, size)`; for RT code regions, set RO via `vmm_set_ro(virt, size)`
- [ ] Prerequisite: implement `vmm_set_nx()` and `vmm_set_ro()` in vmm.c (do not exist yet)
- [ ] Graceful degradation: if table absent, log warning and continue
- [ ] Commit: `"kernel: UEFI runtime W^X enforcement via EFI_MEMORY_ATTRIBUTES_TABLE"`

**Regression risk:** Modifies page table permissions on UEFI runtime regions. Rollback: skip enforcement (BOOT_DEGRADED path).

**Test checkpoint:** Serial shows `[UEFI] W^X enforced on N runtime memory regions`. `uefi_get_time()` still works after enforcement.

## 4. Multi-GPU GOP Handle Enumeration

Enumerate all GOP handles via `LocateHandleBuffer` and select the active display using ConOut device path.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`

> [!NOTE]
> -> XREF: `04-drivers-hardware/TODO-17-gpu-display-drivers.md §6` -- kernel multi-head `display_register_head()` will consume `boot_info.gop_handles[]`.

- [ ] Add `boot_gop_handle_t` struct and `gop_handles[4]` array to `boot_info.h`
- [ ] Replace `LocateProtocol` with `LocateHandleBuffer` in `init_gop()`
- [ ] Primary selection: compare each handle's device path against `ConOut` device path; fallback to largest resolution
- [ ] Call `gop_negotiate_mode()` only on the selected primary handle; `boot_info.fb` unchanged
- [ ] Commit: `"boot: enumerate all GOP handles -- LocateHandleBuffer, ConOut-path primary selection"`

**Test checkpoint:** QEMU (single GPU): `[BOOT] GOP: 1 handle(s)`, unchanged. Bare metal iGPU+dGPU: `[BOOT] GOP: 2 handle(s)`.

## 5. Secure Boot Extended State and Enforcement Policy

Read additional UEFI Secure Boot variables and provide an enforcement policy toggle.

**Files:** `src/kernel/uefi_runtime.c`, `include/kernel/uefi_runtime.h`, `include/kernel/boot_info.h`

- [ ] Read `SetupMode`, `AuditMode`, `DeployedMode` UEFI variables; store in `boot_info`; write to `HKLM\SYSTEM\SecureBoot\`
- [ ] `boot.conf` key `SecureBootEnforce=0`: when 1, trigger kernel lockdown (-> XREF: `02-kernel-core/TODO-10-kernel-security-hardening.md`)
- [ ] Serial log: `[SecureBoot] SetupMode=%u AuditMode=%u DeployedMode=%u Enforce=%u`
- [ ] Commit: `"kernel: Secure Boot extended state variables + SecureBootEnforce policy"`

**Test checkpoint:** QEMU Setup Mode: `SetupMode=1`. Enrolled PK: `SetupMode=0`. `SecureBootEnforce=1`: `g_system_state.secure_boot_enforced == 1`.

## 6. SMBIOS Extended Type Parsing

Parse SMBIOS Type 2 (Baseboard), Type 3 (Chassis), Type 16 (Memory Array), Type 19 (Memory Mapped Address).

**Files:** `src/kernel/smbios.c`, `include/kernel/smbios.h`

- [ ] Type 2: manufacturer, product, version, serial, asset tag -> `HKLM\HARDWARE\Baseboard\*`
- [ ] Type 3: manufacturer, type code, serial, asset tag -> `HKLM\HARDWARE\Chassis\*`; `smbios_get_chassis_type()` for desktop vs laptop distinction
- [ ] Type 16: location, use, max capacity, device count -> `HKLM\HARDWARE\MemoryArray\*`
- [ ] Type 19: start/end address -> `HKLM\HARDWARE\MemoryArray\StartAddr`, `EndAddr`
- [ ] Commit: `"kernel: SMBIOS Type 2/3/16/19 extended parsing -> Registry HARDWARE hives"`

**Test checkpoint:** QEMU: `HKLM\HARDWARE\Baseboard\Manufacturer` non-empty. Bare metal laptop: `Chassis\Type` = 9 (Laptop).

## 7. DBX Revocation List Sync

Synchronize the UEFI dbx with the latest revocation list shipped with OS updates.

**Files:** `src/kernel/secureboot_dbx.c` (new)

> [!TIP]
> **Competitive edge:** Neither Win11 nor Linux proactively validates dbx freshness from within the OS. Impossible OS can log a boot warning when dbx is stale and offer one-click update.

- [ ] `secureboot_dbx_init()`: read `dbx` variable, parse `EFI_SIGNATURE_LIST`, count entries
- [ ] Ship `resources/secureboot/dbx-latest.bin` with each release; compare installed vs shipped
- [ ] If stale: log warning, set `HKLM\SYSTEM\SecureBoot\DbxStale = 1`
- [ ] `secureboot_dbx_apply(path)`: write signed dbx update via `uefi_var_set` with `APPEND_WRITE`
- [ ] Commit: `"kernel: DBX revocation list freshness check and update path"`

**Test checkpoint:** QEMU with OVMF: `secureboot_dbx_init()` reads dbx (may be empty). After apply: entry count updated.

---

## OS Comparison

| ⭐ | Feature                   | 🪟 Win11                   | 🐧 Linux                  | 🚀 Impossible OS              |
|----|---------------------------|-----------------------------|----------------------------|--------------------------------|
| ⭐ | In-bootloader OS menu     | ❌ Separate BCD/bootmgr    | ❌ GRUB is separate        | ⬜ §1 -- integrated countdown |
| 💎 | UEFI capsule update       | ✅ WU UEFI capsules        | ✅ fwupd                   | ⬜ §2                         |
| 💎 | UEFI memory W^X           | ✅ Since Win10 1607        | ✅ EFI_MEMORY_ATTRIBUTES   | ⬜ §3                         |
| 💎 | Multi-GPU GOP             | ✅ LocateHandleBuffer      | ✅ grub handle buffer      | ⬜ §4                         |
| 💎 | Secure Boot extended vars | ✅ SetupMode + Deployed    | ✅ efivarfs all SB vars    | ⬜ §5                         |
| 💎 | Secure Boot enforcement   | ✅ HVCI lockdown           | ✅ kernel lockdown mode    | ⬜ §5                         |
| 💎 | SMBIOS extended types     | ✅ WMI BaseBoard/Enclosure | ✅ /sys/firmware/dmi full  | ⬜ §6                         |
| 💎 | DBX revocation sync       | ✅ WU silent dbx push      | ✅ fwupd/dbxtool           | ⬜ §7                         |
| ⭐ | Proactive dbx stale alert | ❌ Silent WU push only     | ❌ Requires manual fwupdmgr| ⬜ §7 -- boot warning         |

## Unit Tests

> Wire into `test_runner_init()` via `test_register_uefi_advanced()`.

- [ ] Create `src/kernel/test/test_uefi_advanced.c` with:
  - §5: `HKLM\SYSTEM\SecureBoot\SetupMode` exists and is 0 or 1
  - §6: `HKLM\HARDWARE\Chassis\Type` is valid (1-36) on real hardware
  - §6: `HKLM\HARDWARE\MemoryArray\MaxCapacityMB` > 0 on real hardware
  - §7: `secureboot_dbx_init()` does not crash when dbx variable is empty
- [ ] Register in `test_runner_init()`: `test_register_uefi_advanced()`
- [ ] Commit: `"test: add uefi_advanced test suite"`

## Verification

- [ ] Boot menu appears when two GPT partitions present; countdown works; default boots without input
- [ ] `[UEFI] W^X enforced on N UEFI memory regions` in serial log (N > 0 on OVMF)
- [ ] `[SecureBoot] SetupMode=N AuditMode=N DeployedMode=N Enforce=N` in serial log
- [ ] `[SMBIOS] Baseboard: ... Chassis: ... MemArray: ...` in serial log
- [ ] `[SecureBoot] dbx: N entries` in serial log
- [ ] Commit: `"boot: uefi-advanced verified"`
