---
schema_version: 1
id: uefi-advanced
domain: 01-boot-platform
status: active
title: "TODO-27 -- UEFI Advanced Features"
---

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
| ⭐  |   1   | UEFI multi-OS detection and chainload entries | T07 §1-§4      |  [x]   |
| 💎  |   2   | Firmware update advisor (read-only LVFS) | T04 §6              |  [x]   |
| 💎  |   3   | UEFI memory attributes (W^X)            | T02 §1, T24 §1      |  [ ]   |
| 💎  |   4   | Multi-GPU GOP enumeration                | T02 §3              |  [ ]   |
| 💎  |   5   | Secure Boot extended state + enforcement | T02 §5              |  [ ]   |
| 💎  |   6   | SMBIOS extended type parsing             | T02 §4              |  [ ]   |
| 💎  |   7   | DBX revocation list sync                 | T02 §2, §5          |  [ ]   |

---

## 1. UEFI Multi-OS Detection and Chainload Entries

Detect other OS partitions from GPT and contribute chainload entries to the TODO-07 boot menu when the user has multiple OSes installed.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`

- [x] `chainload_detect()` (`bootx64.c`, Step 1a' pre-EBS): `LocateHandleBuffer(SFS)` enumerates non-boot volumes and read-only-probes the well-known foreign bootloader paths into `g_chainload_targets[8]`.
- [x] Direct bootloader-file detection supersedes the GPT-type-GUID scan: the chainload target is the loader file on the ESP, not a data-partition type.
- [x] `chainload_synthesize(parse)` appends up to 8 `BOOT_ENTRY_KIND_CHAINLOAD` envelopes (id `chainload-N`, title=OS, sort_key `zzz-` so they sort after IPOS) to the TODO-07 parse result, bounded by `BOOT_ENTRIES_MAX_ENTRIES`.
- [x] TODO-07 §6 renders the countdown menu over the combined entries; IPOS stays the policy default (foreign entries carry no policy tags).
- [x] `chainload_exec()` builds the volume device path + MEDIA/FILEPATH node, `LoadImage(BootPolicy=FALSE)` (firmware Secure-Boot-verifies; `EFI_SECURITY_VIOLATION` refused) + `StartImage`; a failed chainload demotes to the IPOS fallback.
- [x] `efi.h` types `LoadImage`/`StartImage`/`UnloadImage` (`EFI_IMAGE_LOAD`/`_START`/`_UNLOAD`) for the chainload path.
- [x] Commit: `"boot: multi-OS GPT detection and countdown text-mode boot menu"`

**Test checkpoint:** On QEMU/bare-metal with a foreign UEFI bootloader on another volume, serial logs `[MULTIBOOT] detected <OS>`, the TODO-07 menu shows the foreign entry after the IPOS default, the countdown auto-selects IPOS, and selecting the foreign entry chainloads it (`[MULTIBOOT] chainloading <OS>`); with no foreign loader, serial logs `[MULTIBOOT] no foreign OS bootloaders found` and boots IPOS. Verify on QEMU WHPX/TCG + bare-metal dual-boot.

> **Test runner:** N/A (UEFI bootloader-only; no kernel test surface) | validation: serial `[MULTIBOOT]` lines on QEMU + bare-metal dual-boot

> **Notes:**
> - Shipped `chainload_detect`/`_probe_volume`/`_build_devpath`/`_exec`/`_synthesize` (`bootx64.c`) + typed `LoadImage`/`StartImage`/`UnloadImage` (`efi.h`); detect at Step 1a', synthesize after the parse, dispatch after the menu.
> - Foreign loaders found by read-only ESP bootloader-path probe (not GPT type); trust = firmware Secure Boot at `LoadImage` (dbx = `EFI_SECURITY_VIOLATION` refused); a failed chainload demotes to the IPOS fallback.
> - Scope boundary: the JSON-store chainload entry-kind validator stays TODO-07 §13 (synthesized entries built trusted, bypass it); W^X/multi-GPU/SecureBoot/SMBIOS/dbx are §3-§7.

## 2. Firmware Update Advisor (read-only LVFS-style)

Surface what firmware updates exist for the host and tell the operator how to apply them via the **vendor's** update path. Impossible OS does NOT call `UpdateCapsule()`, does NOT write the `OsIndications` capsule bit, does NOT stage capsule images on the ESP, and does NOT trigger reboot-and-flash. Firmware writes are the single failure mode that turns a laptop into a paperweight; the OS-side risk surface for an actual capsule path is wider than its hobby-OS value, so this section is deliberately scoped as advisory only.

**Files:** `src/kernel/firmware_advisor.c` (new), `include/kernel/firmware_advisor.h` (new), `user/sysinfo/firmware_cmd.c` (new)

> [!NOTE]
> **Option B scope decision (2026-05-02):** the original §2 plan covered actual capsule delivery (UpdateCapsule + OsIndications + ESP staging + submission journal + torn-write recovery). That work is **out of scope and intentionally unowned**. Operators update firmware via the vendor's tool (BIOS Setup, Lenovo Vantage, Dell Command Update, fwupd from a Linux live USB, etc.); Impossible OS only tells them *what* to update and *why*. Removed deliverables: `capsule_update_request`, `capsule_check_result`, `OsIndications` write, ESP `\EFI\UpdateCapsule\` staging, capsule submission journal, torn-write recovery. **Stance change condition:** revisit only if (a) Impossible OS becomes the user's primary daily-driver OS AND (b) a vendor-signing path with brick-test coverage on real hardware is in place.

- [x] **ESRT consumer** (no re-parse): `firmware_advisor_init` consumes the `uefi_config.h` ESRT API (count, entry, decode_status, decode_type, rollback_floor_ok). Read-only.
- [x] **Offline LVFS cache reader** (`X:\Diag\lvfs-metadata.json`, schema_version=1) via `kernel/json.h`. Live HTTPS fetch + GPG/PKCS7 verify + gzip + XML parse owned by §8 (needs TCP/TLS/crypto/XML).
- [x] **Version-comparison advisor** (`fa_classify`): emits `up_to_date` / `update_available` / `unknown` per FwClass; severity `critical` iff cache row carries non-empty CVE; `LastAttemptStatusName` propagated to registry + JSON for operator UX.
- [x] **Rollback gate (advisory only)**: `RollbackFloorOk` mirrored to registry + JSON; sysinfo prints downgrade-blocked note when clear. Never enforced.
- [x] **`sysinfo.exe firmware-updates` CLI** (`user/sysinfo/sysinfo.c` + cmd.exe `sysinfo` builtin): reads `X:\Diag\firmware-advisor.json` (kernel-published), renders per-component table, closes with canonical refusal line.
- [x] **Registry mirror** at `HKLM\SOFTWARE\Impossible\FirmwareAdvisor\<GUID>\` (12 per-component values + `_Header` sibling). Idempotent via `RegDeleteTree`.
- [x] **Refusal sentinel** (`firmware_capsule_refused.c`): compile-time `_Static_assert` (gated `CAPSULE_REFUSAL_ENABLED`) + 4 linker symbols in `.firmware_capsule_refused` section.
- [x] **JSON publish to `X:\Diag\firmware-advisor.json`** (`fa_publish_json`): denormalized verdict, schema_version=1 pinned, fail-closed on truncation. Avoids user-mode registry SSDT plumbing.
- [x] Commit: `"kernel: firmware-update advisor (read-only LVFS metadata + ESRT join)"`

**Test checkpoint:** `sysinfo firmware-updates` at the C:\> prompt on QEMU OVMF (no ESRT) prints `No firmware components reported by ESRT...` followed by the canonical refusal line. Smoke confirms `[OK] advisor: cache: missing` + `[OK] advisor: JSON: wrote X:\Diag\firmware-advisor.json (187 bytes)`. Synthetic-ESRT + synthetic-cache fixture coverage filed as the §8 follow-up. **No `OsIndications` variable is touched on any boot.** Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 5 advisor sub-tests / 16 assertions, 0 failures
>
> **Notes:**
> - **What shipped** -- `firmware_advisor.[ch]` + `firmware_capsule_refused.c` + `user/sysinfo/sysinfo.c` + cmd.exe `sysinfo` builtin + 5 unit tests in `test_firmware_advisor.c`.
> - **How it runs** -- BSP-only Phase 3 init in `boot_desktop.c` after VFS / ESRT / registry. Idempotent (`RegDeleteTree`); cache absent / malformed degrades to `status=unknown` with one `LOG_INFO`.
> - **Downstream effects** -- closes the TODO-04 §6 deferral; `HKLM\SOFTWARE\Impossible\FirmwareAdvisor\` is the canonical desktop-notification source; `X:\Diag\firmware-advisor.json` is the canonical user-mode source.
> - **Canonical doc** -- [`include/kernel/firmware_advisor.h`](../../include/kernel/firmware_advisor.h).
> - **Scope boundary** -- §2 owns offline cache reader + ESRT join + registry + JSON publish + sysinfo CLI + refusal sentinel; live HTTPS fetch + GPG/PKCS7 verify + gzip + XML parse owned by §8.

> **Verified:** 2026-05-03 | this commit | 9/9 items | build OK | smoke PASS (KVM 2.55s) + 187-byte advisor JSON + sentinel symbols present
> **Quality reviewed:** 2026-05-03 | Codex 7x (design + adversarial x2 + consistency x2 + perf + re-adversarial) | 1H+5M+1L fixed | scope: kernel-code-quality + userland-code-quality

## 3. UEFI Memory Attributes (W^X)

Enforce write-XOR-execute on UEFI runtime memory regions by walking the `EFI_MEMORY_ATTRIBUTES_TABLE`.

**Files:** `src/kernel/uefi_runtime.c`, `src/kernel/mm/vmm.c`

> [!NOTE]
> `01-boot-platform/TODO-04 §5 UEFI Memory Attributes and Runtime Properties Inventory` shipped the consumer API: `mat_get_count()` / `mat_get_entry(idx, *out)` returning `mat_entry_t {phys_addr, num_pages, attribute, cls}` with `cls` ∈ `{GUARD, CODE, DATA, RODATA, WX_VIOLATION}` from `include/kernel/uefi_config.h`. Iterate that instead of re-walking firmware tables. `mat_classify_attr()` is a pure test helper.

- [x] Locate `EFI_MEMORY_ATTRIBUTES_TABLE` in UEFI config tables -- shipped by TODO-04 §1 catalog + §5 inventory; consume via `mat_get_count()`/`mat_get_entry()`.
- [ ] Walk entries: for `MAT_CLASS_DATA` regions call `vmm_set_nx(virt, size)`; for `MAT_CLASS_CODE` regions call `vmm_set_ro(virt, size)`. (Iteration source switched to `mat_get_entry()` from `include/kernel/uefi_config.h`.)
- [ ] Prerequisite: implement `vmm_set_nx()` and `vmm_set_ro()` in vmm.c (do not exist yet)
- [ ] Graceful degradation: if table absent, log warning and continue
- [ ] **EFI_MEMORY_ATTRIBUTE_PROTOCOL runtime sync** (gap-audit 2026-05-01 H2, surfaced from TODO-02 review pipeline): the static `EFI_MEMORY_ATTRIBUTES_TABLE` (UEFI 2.6) freezes attributes at boot; UEFI 2.10 adds the runtime-callable `EFI_MEMORY_ATTRIBUTE_PROTOCOL` with `GetMemoryAttributes` / `SetMemoryAttributes` / `ClearMemoryAttributes` so firmware-backed permission flips can stay in sync with OS page-table flips. Linux 6.7+ uses this for the EFI stub. Add: (a) protocol discovery via `LocateProtocol(EFI_MEMORY_ATTRIBUTE_PROTOCOL_GUID, ...)` BEFORE ExitBootServices; cache the function pointers via `boot_info.uefi_runtime` mirror (UEFI 2.10 protocol survives EBS like the rest of RT). (b) When `vmm_set_nx`/`vmm_set_ro` modify a UEFI runtime page, ALSO call the protocol's `SetMemoryAttributes(EFI_MEMORY_XP)` / `EFI_MEMORY_RO` so firmware-internal page-table state matches the OS view -- avoids drift on systems where firmware re-asserts permissions after `SetVirtualAddressMap`. (c) Graceful degradation: when the protocol is absent (UEFI < 2.10 or stripped firmware), log `[UEFI] memory-attribute protocol absent; falling back to static MAT enforcement only` and continue. Tests: synthetic UEFI 2.10 fixture (protocol present) asserts both paths fire; synthetic UEFI 2.6 fixture (protocol absent) asserts the absence is logged and the static path still works. Owner: this section.
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

## 5. Secure Boot Enforcement Policy

Provide a kernel-lockdown enforcement policy gated on the canonical Secure Boot state. The state itself (SetupMode/AuditMode/DeployedMode + drift detection) is owned by [TODO-02 §5](TODO-02-uefi-hardening-secureboot.md#5-secure-boot-state-detection); this section is enforcement policy that CONSUMES the canonical state via the `uefi_secureboot_*` API, not duplicate variable reads.

**Files:** `src/kernel/uefi_runtime.c`, `include/kernel/uefi_runtime.h`, `include/kernel/boot_info.h`

> [!NOTE]
> Gap-audit 2026-05-01 narrowed this section's scope: the original "Read SetupMode/AuditMode/DeployedMode" item was duplicated work -- TODO-02 §5 already publishes those values via `uefi_secureboot_init()`. Removed to avoid drift between two readers; this section now consumes the canonical state.

- [ ] `boot.conf` key `SecureBootEnforce=0`: when 1, read `g_system_state.secure_boot_enforced` (canonical state from TODO-02 §5) and trigger kernel lockdown (-> XREF: `02-kernel-core/TODO-10-kernel-security-hardening.md`)
- [ ] Audit-mode trap-and-log: when `g_system_state.audit_mode == 1`, install a kernel hook that logs every Secure Boot policy violation (failed signature verify, MOK miss, etc.) to `HKLM\SYSTEM\SecureBoot\AuditLog\` without halting the boot. Lets operators dry-run enforcement.
- [ ] DeployedMode lockdown: when `g_system_state.deployed_mode == 1`, refuse to clear PK / KEK / db / dbx via `uefi_var_set()` from kernel space (returns `STATUS_ACCESS_DENIED`). Production fleets stay locked.
- [ ] Drift consumer: poll `uefi_secureboot_drift_detected()` (canonical API shipped in [TODO-02 §15](TODO-02-uefi-hardening-secureboot.md#15-post-boot-securebootrevalidation)) from the same kernel worker that fires `uefi_secureboot_revalidate_tick()`, and trigger immediate lockdown when the sticky flag transitions 0->1 between ticks. Alternatively register a callback via a future `uefi_secureboot_register_drift_listener()` if one is added when the periodic worker pattern lands.
- [ ] Serial log: `[SecureBoot] policy: enforce=%u audit=%u deployed=%u` (no longer logs the raw variable values -- those are TODO-02 §5's surface).
- [ ] Commit: `"kernel: Secure Boot enforcement policy consuming canonical state"`

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
| ⭐ | In-bootloader OS menu     | ❌ Separate BCD/bootmgr    | ❌ GRUB is separate        | ✅ §1 detect + chainload menu |
| ⭐ | Firmware update advisor   | ⚠️ silent WU push only     | ⚠️ fwupd writes flash      | ⬜ §2 read-only LVFS; no UpdateCapsule |
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
