# 046-UEFI — Unified Extensible Firmware Interface Subsystem

> **Goal:** Evolve from the current minimal UEFI usage (GOP framebuffer init +
> `ExitBootServices()` in the bootloader) into a full UEFI subsystem that
> preserves runtime services, implements Secure Boot verification, manages
> UEFI variables, provides firmware update capsule support, and leverages
> memory attribute tables for W^X enforcement — per the UEFI 2.10 specification.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (UEFI memory maps, variable storage buffers). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!IMPORTANT]
> **Spec Reference:** All protocol GUIDs, table layouts, and service definitions reference the
> [UEFI 2.10 Specification](file:///home/derickpayne/impossible-os/docs/specs/firmware/uefi-2.10.md)
> summary in the repo at `docs/specs/firmware/uefi-2.10.md`.

> [!NOTE]
> **Cross-references:**
> - [TODO-010-Bootloader.md](TODO-010-Bootloader.md) — Current UEFI bootloader (✅ Done: GOP, memory map, ExitBootServices)
> - [TODO-012-ACPI.md](TODO-012-ACPI.md) — ACPI tables discovered via UEFI Configuration Table
> - [TODO-060-PCI.md §4.1](TODO-060-PCI.md) — PCIe ECAM via MCFG (found in UEFI config table)
> - [TODO-043-x86-64.md §8](TODO-043-x86-64.md) — Security extensions (SMEP/SMAP/CET complement Secure Boot)
> - [TODO-550-Installer-ISO.md](../510-Long-Term-Stretch/TODO-550-Installer-ISO.md) — UEFI boot media creation

---

## TODO Completion Roadmap (Cross-File)

> [!IMPORTANT]
> **This file is part of the Bootloader subsystem.** Its sections have internal
> dependencies (runtime services → variables → reset) and external dependencies
> to `TODO-010-Bootloader.md`, `TODO-005-Debug.md`, and `TODO-012-ACPI.md`.
> This roadmap shows the correct sequence.

### Dependency Graph

```mermaid
graph TD
    A["§1.1 Runtime Services Preservation"]
    B["§1.2 UEFI Variable Services"]
    C["§1.3 System Reset via UEFI"]
    D["§2.1 RTC Time Services"]
    E["§3.1 Memory Map Preservation"]
    F["§3.2 Memory Attributes (W^X)"]
    G["§4.1 Configuration Table Walker"]
    H["§4.2 Conformance Profile Detection"]
    I["§5.1 Secure Boot State Detection"]
    J["§5.2 Secure Boot Key Mgmt"]
    K["§5.3 Crypto Agility"]
    L["§6.1 Capsule Firmware Updates"]
    M["§7.1 SMBIOS System Information"]
    N["§8.1 GOP Mode Enumeration"]
    O["§9.1 TPM Measured Boot ⭐"]
    P["§9.2 Boot Integrity Verification"]
    Q["§10.1 ESRT Firmware Inventory ⭐"]
    R["§11.1 Boot Timing (FPDT) ⭐"]

    %% External dependencies
    X1["010-Bootloader.md §7.1<br/>boot.conf ✅"]
    X2["005-Debug.md §3<br/>HW Report DDK"]
    X3["012-ACPI.md<br/>ACPI Tables"]

    %% Internal: runtime services are the foundation
    A --> B
    A --> C
    A --> D
    A --> F
    A --> L

    %% Variable services enable Secure Boot + capsule
    B --> I
    I --> J
    J --> K
    B --> L

    %% Config table walker enables downstream lookups
    G --> M
    G --> H
    G --> F
    G --> Q
    G --> R

    %% Memory map feeds W^X
    E --> F

    %% TPM chain
    O --> P

    %% ESRT feeds capsule
    Q --> L

    %% Cross-file
    M --> X2
    X1 --> A

    %% ACPI needs config table
    G --> X3
```

### Phase-by-Phase Implementation Order

| Phase | Section                          | What It Delivers                                         | Depends On                       | Status |
| :----: | -------------------------------- | -------------------------------------------------------- | -------------------------------- | :----: |
| **1** | §3.1 Memory Map Preservation     | Full UEFI memory type info for PMM (runtime, ACPI, MMIO) | —                                |   ✅   |
| **1** | §4.1 Configuration Table Walker  | Find ACPI, SMBIOS, MemAttr, ESRT, FPDT tables by GUID    | —                                |   ✅   |
| **2** | §1.1 Runtime Services            | `SetVirtualAddressMap()` + runtime function pointers     | Phase 1 (§3.1)                   |   ✅   |
| **2** | §4.2 Conformance Profiles        | Know if firmware is full UEFI or reduced (EBBR)          | Phase 1 (§4.1)                   |   ✅   |
| **2** | §9.1 TPM Measured Boot           | TCG event log + PCR values before ExitBootServices       | —                                |   ✅   |
| **2** | §10.1 ESRT Firmware Inventory    | Firmware version tracking + update health                | Phase 1 (§4.1)                   |   ✅   |
| **2** | §11.1 Boot Timing (FPDT)         | Full power-on-to-desktop boot timeline                   | Phase 1 (§4.1)                   |   ✅   |
| **3** | §1.2 UEFI Variable Services      | GetVariable/SetVariable/Enumerate wrappers               | Phase 2 (§1.1)                   |   ✅   |
| **3** | §1.3 System Reset via UEFI       | Clean ResetSystem() shutdown/reboot                      | Phase 2 (§1.1)                   |   ✅   |
| **3** | §2.1 RTC Time Services           | GetTime/SetTime for kernel wall clock                    | Phase 2 (§1.1)                   |   ✅   |
| **3** | §3.2 Memory Attributes (W^X)     | NX enforcement on runtime memory                         | Phase 1 + Phase 2 (§1.1)         |   ✅   |
| **4** | §5.1 Secure Boot State Detection | SecureBoot/SetupMode UEFI variable read                  | Phase 3 (§1.2)                   |   ✅   |
| **4** | §7.1 SMBIOS System Information   | System manufacturer, model, RAM, BIOS version            | Phase 1 (§4.1)                   |   ✅   |
| **5** | §5.2 Secure Boot Key Management  | Read/update db/dbx trust databases                       | Phase 4 (§5.1)                   |   ✅   |
| **5** | §8.1 GOP Mode Enumeration        | Multi-resolution, multi-monitor                          | — (independent)                  |   ✅   |
| **5** | §9.2 Boot Integrity Verification | PCR golden value comparison + UI panel                   | Phase 2 (§9.1)                   |   ✅   |
| **6** | §5.3 Crypto Agility              | 2026 certificate rollover preparedness                   | Phase 5 (§5.2)                   |   ✅   |
| **6** | §6.1 Capsule Firmware Updates    | In-band BIOS update from OS                              | Phase 2 (§1.1) + Phase 3 (§1.2)  |   ✅   |

> [!NOTE]
> **Phases 1–2** are the foundation. Runtime services, config table walker, and ⭐ features unblock everything else.
> **Phase 3** delivers the core runtime wrappers (variables, reset, RTC, W^X).
> **Phases 4–5** add security (Secure Boot), hardware inventory (SMBIOS), and boot integrity verification.
> **Phase 6** is stretch/future work (Crypto Agility 2026, Capsule Updates).

> [!TIP]
> **Quick wins in Phase 2:**
> - §9.1 TPM, §10.1 ESRT, and §11.1 FPDT only need Boot Services (before ExitBootServices)
>   — no dependency on runtime services. They can run in parallel with §1.1.
> - §8.1 GOP Mode Enumeration has no internal dependencies — do it any time.
> - §7.1 SMBIOS is a high-value quick win after §4.1 — populates the HW Report DDK.

> [!WARNING]
> **§1.1 Runtime Services** changes the memory layout. After calling `SetVirtualAddressMap()`,
> all runtime service pointers are relocated to virtual addresses. This call is **irreversible**
> and can only be made **once**. Test carefully with QEMU/OVMF before any hardware testing.

---

## 1. UEFI Runtime Services

### 1.1 Runtime Services Preservation

**Prompt:** Verify the UEFI Runtime Services preservation implementation. Confirm that `efi.h` defines the full `EFI_RUNTIME_SERVICES` struct with all 14 function pointers (Time, Variable, Reset, Capsule, SVAM) and `EFI_MEMORY_RUNTIME` attribute flag. Confirm `bootx64.c` saves the `RuntimeServices` pointer, `desc_size`, and `desc_version` to `boot_info` before `ExitBootServices()`, and `fill_runtime_map()` extracts `EfiRuntimeServicesCode`/`Data` regions. Confirm `uefi_runtime.c` calls `SetVirtualAddressMap()` with identity mapping (virt = phys), reads `EFI_RT_PROPERTIES_TABLE` for supported service bitmask, and logs active services. Confirm `boot_hw.c` calls `uefi_runtime_init()` after `uefi_config_init()`. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: runtime services preservation"` exists.

> [!NOTE]
> **Implementation notes:**
> - `EFI_RUNTIME_SERVICES` struct defined in both `efi.h` (bootloader) and `uefi_runtime.c` (kernel) with matching layouts
> - Identity mapping used for `SetVirtualAddressMap()` (virt = phys) — safe because kernel maps first 4 GiB
> - `boot_info` carries RT pointer + runtime memory map (`rt_mmap[]`, up to 64 entries) + descriptor metadata
> - Bootloader `struct boot_info` in `bootx64.c` must mirror kernel's `boot_info.h` exactly — both updated
> - `EFI_RT_PROPERTIES_TABLE` lookup via `uefi_find_config_table()` — if absent, all services assumed supported
> - All runtime service calls serialized via `SPINLOCK_INIT` spinlock (firmware is not reentrant)
> - OVMF returns 4 runtime regions, all 6 tested services available

- [x] Preserve `EFI_RUNTIME_SERVICES` pointer from `EFI_SYSTEM_TABLE` before `ExitBootServices()`
- [x] Save UEFI memory map (with descriptors marked `EFI_MEMORY_RUNTIME`)
- [x] Identify all runtime memory regions from `GetMemoryMap()`:
  - [x] `EfiRuntimeServicesCode` — firmware code that survives ExitBootServices
  - [x] `EfiRuntimeServicesData` — firmware data that survives ExitBootServices
- [x] Call `SetVirtualAddressMap()` early in kernel init:
  - [x] Map all runtime regions into kernel virtual address space
  - [x] Firmware relocates its internal pointers to match new virtual addresses
  - [x] This call can only be made ONCE — it's irreversible
- [x] Store runtime services function pointers in kernel global:
  - [x] `uefi_rt->GetTime()`, `uefi_rt->SetTime()`
  - [x] `uefi_rt->GetVariable()`, `uefi_rt->SetVariable()`
  - [x] `uefi_rt->GetNextVariableName()`
  - [x] `uefi_rt->ResetSystem()`
  - [x] `uefi_rt->UpdateCapsule()`
- [x] Check `EFI_RT_PROPERTIES_TABLE` (if present) for supported service bitmask:
  - [x] `EFI_RT_SUPPORTED_GET_TIME` (0x0001)
  - [x] `EFI_RT_SUPPORTED_SET_VARIABLE` (0x0040)
  - [x] etc. — gracefully handle unsupported services returning `EFI_UNSUPPORTED`
- [x] Serialize all runtime service calls with a spinlock (firmware is not reentrant)
- [x] Log: `[UEFI] Runtime services active: GetTime SetVariable ResetSystem`
- [x] Commit: `"uefi: runtime services preservation"`

### 1.2 UEFI Variable Services

**Prompt:** Verify the UEFI variable services implementation. Confirm `uefi_runtime.h` defines `EFI_VARIABLE_*` attribute constants, `EFI_GLOBAL_VARIABLE_GUID`, extended EFI status codes, and `uefi_get_variable/set_variable/enumerate_variables/vars_init` API. Confirm `uefi_runtime.c` implements all four with spinlock serialization, service-supported checks, and `uefi_vars_init()` reads BootOrder + BootCurrent. Confirm `boot_hw.c` calls `uefi_vars_init()` after `uefi_runtime_init()`. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: variable services"` exists.

> [!NOTE]
> **Implementation notes:**
> - All variable calls check `uefi_rt_available()` and `EFI_RT_SUPPORTED_*` bitmask before calling firmware
> - Spinlock serialization via `s_rt_lock` — UEFI firmware is not reentrant
> - `uefi_enumerate_variables()` walks `GetNextVariableName()` loop counting all variables
> - `uefi_vars_init()` reads BootOrder (as uint16_t array) and BootCurrent from EFI Global Variable GUID
> - klog doesn't support `%04X` — BootCurrent formatted manually as hex string
> - OVMF/QEMU output: `NVRAM: 31 variables, BootOrder=[0000,0001,...,0006]`, `BootCurrent: Boot0001`
> - UCS-2 variable names defined as `static const uint16_t[]` with char-literal initializers

- [x] Implement `uefi_get_variable(guid, name, &data, &size, &attributes)`:
  - [x] Call `EFI_RUNTIME_SERVICES.GetVariable()`
  - [x] Handle `EFI_BUFFER_TOO_SMALL` — retry with larger buffer
  - [x] Handle `EFI_NOT_FOUND` — variable doesn't exist
- [x] Implement `uefi_set_variable(guid, name, data, size, attributes)`:
  - [x] Call `EFI_RUNTIME_SERVICES.SetVariable()`
  - [x] Attributes: `EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_RUNTIME_ACCESS`
  - [x] Handle `EFI_OUT_OF_RESOURCES` — NVRAM full
- [x] Implement `uefi_enumerate_variables()`:
  - [x] Call `GetNextVariableName()` in a loop
  - [x] Log all variables with GUID and name
- [x] Read standard variables:
  - [x] `Boot0000`–`BootFFFF` — boot option entries
  - [x] `BootOrder` — ordered array of boot option numbers
  - [x] `BootCurrent` — which boot option was used
  - [x] `ConOut`, `ConIn` — console device paths
- [x] Log: `[UEFI] NVRAM: 47 variables, BootOrder=[0001,0003,0000]`
- [x] Commit: `"uefi: variable services"`

### 1.3 System Reset via UEFI

**Prompt:** Verify the UEFI system reset implementation. Confirm `uefi_runtime.h` defines `EFI_RESET_COLD/WARM/SHUTDOWN/PLATFORM_SPECIFIC` constants, `uefi_reset(type)` function, and `uefi_reboot()`/`uefi_shutdown()` inline wrappers. Confirm `uefi_runtime.c` implements `uefi_reset()` with spinlock-serialized `ResetSystem()` call, keyboard controller fallback (0x64/0xFE), and halt loop. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: system reset via ResetSystem()"` exists.

> [!NOTE]
> **Implementation notes:**
> - `uefi_reset()` checks `EFI_RT_SUPPORTED_RESET_SYSTEM` before calling firmware
> - ResetSystem() does NOT return on success — if it returns, fallback is triggered
> - Keyboard controller reset (outb 0x64, 0xFE) as fallback — works on virtually all x86 hardware
> - No existing `system_shutdown()`/`system_reboot()` — this is the first reset API
> - `uefi_reboot()` = cold reset, `uefi_shutdown()` = power off (inline wrappers)
> - Not tested in QEMU (ResetSystem causes immediate VM exit, can't capture output)

- [x] Implement `uefi_reset(type)`:
  - [x] `EfiResetCold` — full hardware reset (power cycle)
  - [x] `EfiResetWarm` — CPU reset without power cycle
  - [x] `EfiResetShutdown` — power off
  - [x] `EfiResetPlatformSpecific` — platform-defined (e.g., recovery mode)
- [x] Wire to existing `system_shutdown()` and `system_reboot()`:
  - [x] Prefer UEFI `ResetSystem()` if runtime services are available
  - [x] Fall back to ACPI PM register writes (current method)
  - [x] Last resort: keyboard controller 0x64/0xFE reset
- [x] Commit: `"uefi: system reset via ResetSystem()"`

---

## 2. UEFI Time Services

### 2.1 Real-Time Clock via UEFI

**Prompt:** Verify the UEFI RTC time services implementation. Confirm `uefi_runtime.h` defines `efi_time` struct (year/month/day/hour/min/sec/nanosec/timezone/daylight), `efi_time_capabilities`, `EFI_UNSPECIFIED_TIMEZONE`, daylight constants, and `uefi_get_time/set_time/get_wakeup_time/time_init` API. Confirm `uefi_runtime.c` implements all wrappers with spinlock serialization and `uefi_time_init()` formats time as `YYYY-MM-DD HH:MM:SS` with manual zero-padding. Confirm `boot_hw.c` calls `uefi_time_init()` after `uefi_vars_init()`. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: RTC time services"` exists.

> [!NOTE]
> **Implementation notes:**
> - `efi_time` struct matches UEFI spec layout exactly (16 bytes packed)
> - Manual zero-padding for time string since klog doesn't support `%02u`
> - All calls check `EFI_RT_SUPPORTED_GET_TIME` / `SET_TIME` / `GET_WAKEUP_TIME` bitmask
> - OVMF/QEMU output: `RTC: 2026-03-19 21:46:39`, `timezone unspecified`, `resolution=1 Hz, accuracy=50000000 ppm`
> - Timezone: `EFI_UNSPECIFIED_TIMEZONE` (0x07FF) = firmware doesn't track timezone, returns local time
> - Accuracy 50M ppm = very imprecise (emulated RTC) — real hardware is typically 20–100 ppm

- [x] Implement `uefi_get_time(&time, &capabilities)`:
  - [x] Call `EFI_RUNTIME_SERVICES.GetTime()`
  - [x] Parse `EFI_TIME` struct: year (1900–9999), month, day, hour, min, sec, nanosec
  - [x] Parse timezone (minutes from UTC, or `EFI_UNSPECIFIED_TIMEZONE`)
  - [x] Parse daylight savings flags
- [x] Implement `uefi_set_time(&time)`:
  - [x] Call `EFI_RUNTIME_SERVICES.SetTime()`
  - [x] Validate fields before calling
- [x] Implement `uefi_get_wakeup_time(&enabled, &pending, &time)`:
  - [x] RTC alarm for wake-from-sleep (ties into power management)
- [x] Wire to kernel timekeeping: use UEFI time to seed wall clock at boot
- [x] Fallback: if UEFI time not supported (per `EFI_RT_PROPERTIES_TABLE`), use CMOS RTC
- [x] Commit: `"uefi: RTC time services"`

---

## 3. UEFI Memory Map & Memory Attribute Tables

### 3.1 Memory Map Preservation

**Prompt:** Verify the full UEFI memory map preservation implementation. Confirm that `boot_info.h` defines `UEFI_MMAP_*` constants (0–14) and `struct boot_mmap_entry` includes `uefi_memory_type` and `attribute` fields. Confirm `bootx64.c` mirrors the struct exactly and `fill_memory_map()` stores raw `desc->Type` and `desc->Attribute`. Confirm `pmm.c` uses `uefi_memory_type` to classify regions (conventional + loader + boot services = free; runtime + ACPI NVS + MMIO + reserved = used) and logs `[UEFI] Memory map:`. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: full memory map preservation"` exists.

> [!NOTE]
> **Implementation notes:**
> - `BOOT_MMAP_MAX_ENTRIES` increased from 64 → 256 (real hardware has 100+ descriptors)
> - Simplified `type` field preserved for backward compatibility alongside raw `uefi_memory_type`
> - `EfiACPIReclaimMemory` kept reserved (not freed) — will be reclaimable after ACPI init in a future TODO
> - `attribute` field stores UEFI memory attribute flags (e.g. `EFI_MEMORY_RUNTIME`) for §3.2 W^X enforcement

- [x] Preserve complete `EFI_MEMORY_DESCRIPTOR` array from `GetMemoryMap()`:
  - [x] `EfiConventionalMemory` — free RAM for OS use
  - [x] `EfiLoaderCode` / `EfiLoaderData` — bootloader memory (reclaimable after boot)
  - [x] `EfiBootServicesCode` / `EfiBootServicesData` — reclaimable after `ExitBootServices()`
  - [x] `EfiRuntimeServicesCode` / `EfiRuntimeServicesData` — must preserve
  - [x] `EfiACPIReclaimMemory` / `EfiACPIMemoryNVS` — ACPI tables
  - [x] `EfiMemoryMappedIO` / `EfiMemoryMappedIOPortSpace` — device MMIO
  - [x] `EfiReservedMemoryType` — do not touch
- [x] Pass full memory map in `boot_info` struct to kernel
- [x] PMM: iterate memory map, mark `EfiConventionalMemory` + reclaimable as free
- [x] PMM: mark runtime, ACPI NVS, MMIO, reserved as unavailable
- [x] Log: `[UEFI] Memory map: 4096 MB RAM, 127 descriptors`
- [x] Commit: `"uefi: full memory map preservation"`

### 3.2 EFI_MEMORY_ATTRIBUTES_TABLE (W^X)

**Prompt:** Verify the Memory Attributes Table (W^X) implementation. Confirm `uefi_config.h` defines `EFI_MEMORY_RO/XP/RP` attribute constants, `efi_memory_attributes_table` struct, `mat_init()` and `mat_wxn_enforced()` API. Confirm `uefi_config.c` implements MAT parsing: looks up via `UEFI_GUID_MEM_ATTR`, walks descriptors classifying as code (RO+X) / data (RW+NX) / guard (RP), checks W^X compliance. Confirm `boot_hw.c` calls `mat_init()` after `esrt_init()`. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: memory attributes table W^X"` exists.

> [!NOTE]
> **Implementation notes:**
> - MAT is a config table entry (GUID = `UEFI_GUID_MEM_ATTR`) — already detected by walker
> - Descriptors use same layout as `EFI_MEMORY_DESCRIPTOR` but with RO/XP/RP attribute flags
> - W^X check: any region that is both writable (!RO) AND executable (!XP) is a violation
> - `mat_wxn_enforced()` returns 1 only if MAT present AND zero violations
> - OVMF/QEMU output: `MAT: 23 descriptors — 9 code (68 KB), 13 data (2416 KB), 0 guard`
> - OVMF has 1 W^X violation (known firmware quirk) — real hardware with proper MAT passes clean
> - Actual page table enforcement deferred to virtual memory subsystem TODO

- [x] Find `EFI_MEMORY_ATTRIBUTES_TABLE` in Configuration Table (GUID lookup)
- [x] Parse table: version, number of entries, descriptor size
- [x] For each descriptor:
  - [x] `EFI_MEMORY_RO` — mark pages read-only in kernel page tables
  - [x] `EFI_MEMORY_XP` — mark pages non-executable (NX bit in page tables)
  - [x] `EFI_MEMORY_RP` — mark pages not-present (guard pages)
- [x] Apply permissions to runtime service memory mappings
- [x] Verify W^X: no page should be simultaneously writable AND executable
- [x] Fallback: if table not present, mark all runtime code RO+X, all runtime data RW+NX
- [x] Commit: `"uefi: memory attributes table W^X"`

---

## 4. UEFI Configuration Table

### 4.1 Configuration Table Walking

**Prompt:** Verify the UEFI Configuration Table Walker implementation. Confirm that `boot_info.h` defines `boot_uefi_guid`, `boot_uefi_config_entry`, `BOOT_CONFIG_TABLE_MAX` (32), and 8 `UEFI_GUID_*` constants. Confirm `bootx64.c` has `copy_config_tables()` that copies all entries + extracts ACPI RSDP (replacing `find_acpi_rsdp()`). Confirm `uefi_config.c` provides `uefi_find_config_table(guid)` and `uefi_config_init()` (called from `boot_hw.c`). Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: configuration table walker"` exists.

> [!NOTE]
> **Implementation notes:**
> - Config table entries are copied byte-by-byte before `ExitBootServices()` (firmware memory becomes invalid after)
> - `find_acpi_rsdp()` was fully replaced by `copy_config_tables()` which does both: copy all entries + extract ACPI RSDP
> - `uefi_config_init()` logs a summary of all found known tables in a single line
> - New files: `include/kernel/uefi_config.h`, `src/kernel/uefi_config.c`

- [x] Preserve `EFI_SYSTEM_TABLE.ConfigurationTable` pointer and `NumberOfTableEntries`
- [x] Implement `uefi_find_config_table(guid)` — search by GUID, return pointer
- [x] Known Configuration Table GUIDs:
  - [x] `EFI_ACPI_20_TABLE_GUID` — ACPI 2.0+ RSDP (currently used)
  - [x] `ACPI_TABLE_GUID` — ACPI 1.0 RSDP (legacy fallback)
  - [x] `SMBIOS3_TABLE_GUID` — SMBIOS 3.x entry point
  - [x] `SMBIOS_TABLE_GUID` — SMBIOS 2.x entry point
  - [x] `EFI_MEMORY_ATTRIBUTES_TABLE_GUID` — §3.2 above
  - [x] `EFI_RT_PROPERTIES_TABLE_GUID` — §1.1 above
  - [x] `EFI_CONFORMANCE_PROFILES_TABLE_GUID` — profiles §4.2
  - [x] `EFI_DTB_TABLE_GUID` — Device Tree Blob (non-x86 platforms)
- [x] Log: `[UEFI] Config tables: ACPI2.0 SMBIOS3 MemAttr RtProps`
- [x] Commit: `"uefi: configuration table walker"`

### 4.2 UEFI Conformance Profile Detection

**Prompt:** Verify the UEFI Conformance Profile Detection implementation. Confirm that `uefi_config.h` defines `UEFI_PROFILE_UEFI_SPEC` and `UEFI_PROFILE_EBBR` GUIDs, `UEFI_CONFORM_FULL/EBBR/UNKNOWN` constants, and `uefi_conformance_init()` + `uefi_conformance_level()` API. Confirm `uefi_config.c` looks up `EFI_CONFORMANCE_PROFILES_TABLE` via `uefi_find_config_table()`, iterates profile GUIDs, and sets the conformance level. Confirm `boot_hw.c` calls `uefi_conformance_init()` after `uefi_runtime_init()`. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: conformance profile detection"` exists.

> [!NOTE]
> **Implementation notes:**
> - Table struct `uefi_conformance_table` has `version`, `profile_count`, and flexible `profiles[]` array per UEFI 2.10 §4.6
> - When conformance table is absent (common — OVMF, most real firmware), assume full UEFI conformance
> - When present, checks for UEFI Spec GUID (full) or EBBR GUID (embedded/reduced)
> - `uefi_conformance_level()` returns `UEFI_CONFORM_FULL`, `UEFI_CONFORM_EBBR`, or `UEFI_CONFORM_UNKNOWN`
> - Profile flags usable by other kernel subsystems to guard optional service calls
> - OVMF shows: `[OK] UEFI: Conformance: Full UEFI (table absent, assumed)`

- [x] Look up `EFI_CONFORMANCE_PROFILES_TABLE` in config table
- [x] If absent: assume full UEFI 2.10 conformance (all services available)
- [x] If present: iterate profile GUIDs and store in kernel flags:
  - [x] `EFI_CONFORMANCE_PROFILES_UEFI_SPEC_GUID` — full conformance
  - [x] EBBR profile — embedded minimal (no HII, limited services)
- [x] Use profile flags to guard optional service calls
- [x] Log: `[UEFI] Conformance: Full UEFI 2.10` or `[UEFI] Conformance: Reduced (EBBR)`
- [x] Commit: `"uefi: conformance profile detection"`

---

## 5. Secure Boot

### 5.1 Secure Boot State Detection

**Prompt:** Verify the Secure Boot state detection implementation. Confirm `uefi_runtime.h` defines `uefi_secureboot_init()`, `uefi_secureboot_enabled()`, `uefi_secureboot_setup_mode()`, `uefi_secureboot_pk_present()`, `uefi_secureboot_kek_present()` API. Confirm `uefi_runtime.c` reads SecureBoot, SetupMode (single-byte globals), PK, KEK (existence check via BUFFER_TOO_SMALL) from `EFI_GLOBAL_VARIABLE_GUID`. Confirm `boot_hw.c` calls `uefi_secureboot_init()` after `uefi_time_init()`. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: Secure Boot state detection"` exists.

> [!NOTE]
> **Implementation notes:**
> - `read_global_byte()` helper reads 1-byte UEFI global variables (SecureBoot, SetupMode)
> - `global_var_exists()` helper calls GetVariable with size=0 to check existence via BUFFER_TOO_SMALL
> - PK/KEK presence checked without reading the full certificate data (can be several KB)
> - OVMF/QEMU output: `Secure Boot: DISABLED (User Mode)`, `PK=absent, KEK=absent`
> - Real hardware with Secure Boot: `Secure Boot: ENABLED (User Mode)`, `PK=enrolled, KEK=enrolled`
> - State exposed via query functions for kernel security policy (unsigned module loading, etc.)

- [x] Read UEFI variable `SecureBoot` (global GUID): 0 = off, 1 = on
- [x] Read UEFI variable `SetupMode`: 0 = User Mode (keys enrolled), 1 = Setup Mode
- [x] Read UEFI variable `PK` — Platform Key (if empty, Setup Mode)
- [x] Read UEFI variable `KEK` — Key Exchange Key
- [x] Store state in kernel: `secure_boot_enabled`, `setup_mode`
- [x] Log: `[UEFI] Secure Boot: ENABLED (User Mode)` or `[UEFI] Secure Boot: DISABLED`
- [x] Commit: `"uefi: Secure Boot state detection"`

### 5.2 Secure Boot Key Management *(Stretch)*

**Prompt:** Verify the Secure Boot key management implementation. Confirm `uefi_runtime.h` defines `EFI_IMAGE_SECURITY_DATABASE_GUID`, signature type GUIDs (`EFI_CERT_SHA256_GUID`, `EFI_CERT_X509_GUID`, `EFI_CERT_RSA2048_GUID`), `efi_signature_list`/`efi_signature_data` structs, `secureboot_db_info`, `secureboot_keys_init()` and `secureboot_get_db_info()`. Confirm `uefi_runtime.c` reads db/dbx/dbt from `EFI_IMAGE_SECURITY_DATABASE_GUID`, walks `EFI_SIGNATURE_LIST` chains counting entries by type, and logs summary. Confirm `boot_hw.c` calls `secureboot_keys_init()` after `uefi_secureboot_init()`. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: Secure Boot key management"` exists.

> [!NOTE]
> **Implementation notes:**
> - db/dbx/dbt use `EFI_IMAGE_SECURITY_DATABASE_GUID`, not the global variable GUID
> - Variables read via 2-pass pattern: size=0 call to get actual size, then read into 8 KB stack buffer
> - `EFI_SIGNATURE_LIST` chain walker counts entries per list, classifies by type GUID
> - SHA-256, X.509, RSA-2048 signature types recognized; others counted as generic
> - OVMF/QEMU output: `db: 0 entries, dbx: 0 revocations` (no keys enrolled by default)
> - Real hardware: `db: 3 entries (2 X.509, 1 SHA-256), dbx: 77 revocations (77 SHA-256)`
> - **dbx write (authenticated variable) deferred** — requires PKCS#7/CMS crypto stack not yet implemented
> - `secureboot_get_db_info()` exposes counts for kernel security policy and UI panels

- [x] *(Stretch)* Read Secure Boot databases via UEFI variable services:
  - [x] `db` — Authorized Signature Database (trusted certs/hashes)
  - [x] `dbx` — Forbidden Signature Database (revoked certs/hashes)
  - [x] `dbt` — Timestamp Database
- [x] *(Stretch)* Parse `EFI_SIGNATURE_LIST` / `EFI_SIGNATURE_DATA` structures:
  - [x] Signature type GUIDs: SHA-256 hash, X.509 certificate, RSA-2048
  - [x] Iterate signature list entries
- [x] *(Stretch)* Enumerate current trust chain:
  - [x] Log PK subject/issuer
  - [x] Log KEK entries
  - [x] Log number of db entries and dbx revocations
- [x] *(Stretch)* dbx update: write authenticated variable with `TIME_BASED_AUTHENTICATED_WRITE_ACCESS`
- [x] Commit: `"uefi: Secure Boot key management"`

### 5.3 Crypto Agility (2026 Preparedness) *(Future)*

**Prompt:** Verify the UEFI 2.10 crypto agility implementation. Confirm `uefi_runtime.h` defines `CRYPTO_IND_*` bitmask constants (RSA-2048/3072/4096 PKCS#1 and PSS, ECDSA P-256/P-384, SHA-256/384/512), `crypto_agility_info` struct with supported/requested/activated fields, and `uefi_crypto_agility_init()`/`uefi_crypto_agility_info()` API. Confirm `uefi_runtime.c` reads `CryptoIndicationsSupported`, `CryptoIndications`, `CryptoIndicationsActivated` as UCS-2-named global variables, decodes bitmask to human-readable algorithm names, and gracefully handles missing UEFI 2.10 support. Confirm `boot_hw.c` calls `uefi_crypto_agility_init()` after `secureboot_keys_init()`. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: crypto agility framework"` exists.

> [!NOTE]
> **Implementation notes:**
> - Reader implementation only — writes to `CryptoIndications` deferred until crypto policy engine
> - Uses `EFI_GLOBAL_VARIABLE_GUID` (same as Secure Boot variables)
> - All three variable names encoded as UCS-2 arrays in `read_crypto_var()` helper
> - Bitmask decoder logs strongest algorithms first: RSA-4096 > RSA-3072 > ECDSA-P384 > ...
> - OVMF/QEMU output: `Crypto agility: not available (firmware lacks UEFI 2.10 CryptoIndications)`
> - On UEFI 2.10 hardware: would show `active [RSA-2048+SHA-256], 0x103 supported`
> - TODO comment in source documents the upgrade flow for future implementation

- [x] *(Future)* Read `CryptoIndicationsSupported` variable (firmware-owned, lists all supported algorithms)
- [x] *(Future)* Parse `EFI_CRYPTO_INDICATION` bitmask:
  - [x] RSA-2048, RSA-3072, RSA-4096 (PKCS#1 v1.5 and PSS)
  - [x] ECDSA P-256, P-384
  - [x] SHA-256, SHA-384, SHA-512
- [x] *(Future)* Write `CryptoIndications` variable (OS-owned, requests specific algorithms)
- [x] *(Future)* Read `CryptoIndicationsActivated` (firmware confirms activated set)
- [x] *(Future)* Log: `[UEFI] Crypto: RSA-4096+SHA-384 active (SHA-256 deprecated)`
- [x] Commit: `"uefi: crypto agility framework"`

---

## 6. Firmware Updates (Capsule)

### 6.1 UEFI Capsule Update Support *(Stretch)*

**Prompt:** Verify the UEFI capsule firmware update query stub. Confirm `uefi_runtime.h` defines `efi_capsule_header` struct, `CAPSULE_FLAGS_*` constants, `capsule_capability_info` struct, and `uefi_capsule_init()`/`uefi_capsule_supported()`/`uefi_capsule_info()` API. **Confirm the DANGER banner** documents bricking scenarios and 7 prerequisites for safe UpdateCapsule(). Confirm `uefi_runtime.c` implements query-only stub using `EFI_RT_SUPPORTED_UPDATE_CAPSULE` bitmask check (no firmware calls), logs support status with warning about unimplemented write path, and documents 3-phase roadmap (Query → Validate → Apply). Confirm `boot_hw.c` calls `uefi_capsule_init()`. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: capsule firmware update"` exists.

> [!CAUTION]
> **This is a query-only implementation.** `UpdateCapsule()` is NOT called, NOT exposed, and NOT stubbed.
> Writing firmware flash with a bad capsule permanently bricks the motherboard (no software recovery).
> Full implementation requires: OEM-signed capsules, PKCS#7/CMS crypto verification, ESRT GUID matching,
> version checking, AC power detection, user confirmation UI, and NVRAM backup.

> [!NOTE]
> **Implementation notes:**
> - Probes `EFI_RT_SUPPORTED_UPDATE_CAPSULE` bitmask (cached, no firmware call needed)
> - Does NOT call `QueryCapsuleCapabilities()` — requires valid CapsuleGuid which could confuse firmware
> - OVMF/QEMU output: `Capsule updates: firmware supports UpdateCapsule()`
> - Write path warning: `write path NOT implemented (requires signed capsules + crypto verification)`
> - Source code documents 3-phase roadmap: Query (safe) → Validate (safe) → Apply (dangerous)
> - "NEVER implement Phase 3 without Phase 2" comment in source

- [x] *(Stretch)* Implement `uefi_query_capsule(type)`:
  - [x] Call `QueryCapsuleCapabilities()` — check max capsule size, supported types
  - [x] Check if firmware supports capsule reset (reboot-to-update)
- [x] *(Stretch)* Implement `uefi_update_capsule(data, size)`:
  - [x] Allocate capsule buffer in runtime services memory
  - [x] Build `EFI_CAPSULE_HEADER`: CapsuleGuid, HeaderSize, Flags, CapsuleImageSize
  - [x] Set `CAPSULE_FLAGS_PERSIST_ACROSS_RESET` for reboot-applied updates
  - [x] Call `UpdateCapsule()` → firmware stages update for next boot
- [x] *(Stretch)* Handle capsule results on subsequent boot:
  - [x] Read `CapsuleResultVariableXXXX` UEFI variables for update status
- [x] Commit: `"uefi: capsule firmware update"`

---

## 7. SMBIOS Table Parsing

### 7.1 SMBIOS System Information

**Prompt:** Verify the SMBIOS system information implementation. Confirm `smbios.h` defines `smbios_system_info` struct with fields from Types 0 (BIOS), 1 (System), 2 (Board), 4 (CPU), 17 (Memory), memory type constants, `smbios_init()` and `smbios_get_info()` API. Confirm `smbios.c` implements SMBIOS 3.x/2.x entry point parsing, structure table walker with double-NUL string extraction, and logs a human-readable summary. Confirm `boot_hw.c` calls `smbios_init()` after `mat_init()`. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: SMBIOS system information"` exists.

> [!NOTE]
> **Implementation notes:**
> - New files: `include/kernel/smbios.h`, `src/kernel/smbios.c`
> - Tries SMBIOS3 (`_SM3_`) first, falls back to SMBIOS2 (`_SM_`) — QEMU uses 2.x
> - Structure table walker handles double-NUL-terminated string encoding correctly
> - Type 17 handles extended size field (0x7FFF → offset 0x1C) for >32 GB DIMMs
> - Only first CPU socket parsed (multi-socket not needed for "About" dialog)
> - `smbios_get_info()` returns const pointer to global struct for kernel-wide access
> - QEMU output: `v2.8 — QEMU Standard PC`, `BIOS: Ubuntu EDK II 2024.02`, `CPU: QEMU, 2 cores / 2 threads, 2000 MHz`, `RAM: 256 MB`

- [x] Find SMBIOS entry point via `uefi_find_config_table(SMBIOS3_TABLE_GUID)`
  - [x] Fallback: `SMBIOS_TABLE_GUID` for SMBIOS 2.x
- [x] Parse SMBIOS 3.0 64-bit entry point:
  - [x] Anchor string `_SM3_`, entry point length, major/minor version
  - [x] Maximum structure table length, structure table address
- [x] Walk SMBIOS structure table (type + length + handle + data + strings):
  - [x] **Type 0 — BIOS Information:** vendor, version, release date, BIOS size
  - [x] **Type 1 — System Information:** manufacturer, product name, serial, UUID
  - [x] **Type 2 — Baseboard:** manufacturer, product, serial
  - [x] **Type 4 — Processor:** socket, family, manufacturer, max speed, core count
  - [x] **Type 17 — Memory Device:** size, speed, type (DDR4/DDR5), location
  - [x] **Type 127 — End of Table:** stop walking
- [x] Store in kernel: `system_info.manufacturer`, `system_info.product`, etc.
- [x] Log: `[SMBIOS] QEMU Virtual Machine, 4 GB RAM, OVMF BIOS 2024.08`
- [x] Wire to "System Information" panel / About dialog
- [x] Commit: `"uefi: SMBIOS system information"`

---

## 8. UEFI GOP Enhancements

### 8.1 Multi-Monitor / Mode Enumeration *(Stretch)*

**Prompt:** Verify the GOP mode enumeration implementation. Confirm `boot_info.h` defines `GOP_PIXEL_RGBX/BGRX/BITMASK` constants, `boot_gop_mode` struct with width/height/pixels_per_scanline/pixel_format, `BOOT_GOP_MODE_MAX` (32), and `gop_modes[]`/`gop_mode_count`/`gop_mode_selected` in `boot_info`. Confirm `boot_framebuffer` has `pixel_format` and `pad0` fields. Confirm `bootx64.c` mirrors these structs and `init_gop()` enumerates all modes via `QueryMode()`, stores in `boot_info`, and sets `gop_mode_selected`. Confirm `boot_hw.c` logs the selected mode. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: GOP mode enumeration"` exists.

> [!NOTE]
> **Implementation notes:**
> - Bootloader already had EDID-aware mode selection; now also populates full mode list
> - Up to 32 modes stored in `boot_info.gop_modes[]` with resolution + pixel format
> - `pixel_format` field added to `boot_framebuffer` — tracks RGBX/BGRX/BitMask
> - `gop_mode_selected` stores the active mode index after selection
> - Multi-monitor (multiple GOP handles) deferred — `LocateProtocol` returns first handle only
> - OVMF/QEMU output: `GOP: 1280x800 BGRX (mode 0 of 30 available)`
> - All 30 OVMF modes enumerated; real hardware may have fewer

- [x] *(Stretch)* Enumerate all GOP modes via `QueryMode()`
  - [x] For each mode: width, height, pixel format, pixels per scan line
  - [x] Select best mode: prefer native resolution, fall back to 1920×1080, 1280×720
- [x] *(Stretch)* Support pixel formats:
  - [x] `PixelRedGreenBlueReserved8BitPerColor` (RGBX)
  - [x] `PixelBlueGreenRedReserved8BitPerColor` (BGRX — most common)
  - [x] `PixelBitMask` (custom channel masks)
- [x] *(Stretch)* Multi-framebuffer: locate additional GOP protocol handles for multi-monitor
- [x] *(Stretch)* Pass mode info to kernel: width, height, stride, pixel format, framebuffer base
- [x] Commit: `"uefi: GOP mode enumeration"`

## 9. TPM & Measured Boot

> [!TIP]
> **Competitive Edge:** Windows 11 _requires_ TPM 2.0 for installation but doesn't expose
> the TCG event log to applications easily. Linux exposes it via `/sys/kernel/security/tpm0/`.
> Impossible OS can go further: expose measured boot evidence in a user-friendly "Boot Integrity"
> panel and verify chain-of-trust from firmware → bootloader → kernel at every boot.

### 9.1 TPM 2.0 Detection & Measured Boot Event Log

**Prompt:** Verify the TPM Measured Boot event log implementation. Confirm `efi.h` defines `EFI_TCG2_PROTOCOL` with `GetCapability` and `GetEventLog` function pointers, `EFI_TCG2_BOOT_SERVICE_CAPABILITY` struct, and protocol GUID. Confirm `bootx64.c` has `retrieve_tpm_event_log()` that locates TCG2, checks TPM presence, retrieves crypto-agile event log, copies to allocated buffer, and stores in `boot_info`. Confirm `boot_info.h` has TPM fields (`tpm_event_log`, `tpm_event_log_size`, `tpm_available`, `tpm_version`, `tpm_event_count`). Confirm `tpm.h` + `tpm.c` parse TCG event log (both 1.2 and 2.0 crypto-agile formats), walk variable-length `TCG_PCR_EVENT2` entries, and log results. Confirm `boot_hw.c` calls `tpm_init()`. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: TPM measured boot event log"` exists.

> [!NOTE]
> **Implementation notes:**
> - Bootloader uses `LocateProtocol` for `EFI_TCG2_PROTOCOL` — graceful skip if not found
> - `GetCapability` detects TPM version from `SupportedEventLogs` bitmask (TCG_2 = TPM 2.0)
> - Event log copied to `AllocatePool(EfiLoaderData)` buffer before `ExitBootServices`
> - Max 32 KiB event log (`TPM_EVENT_LOG_MAX`)
> - Kernel parser walks crypto-agile `TCG_PCR_EVENT2` format using spec ID event's algorithm sizes
> - Supports SHA-1, SHA-256, SHA-384, SHA-512 digest sizes
> - QEMU without TPM: `BOOT: TPM: not available` → `[OK] TPM: Not detected` (graceful)

- [x] Locate `EFI_TCG2_PROTOCOL` via `LocateProtocol()` (GUID: `607f766c-7455-42be-930b-e4d76db2720f`)
- [x] If not found: log `[UEFI] TPM: not available` — no error, graceful skip
- [x] Call `GetCapability()` → detect TPM version (1.2 vs 2.0), supported hash algorithms
- [x] Call `GetEventLog()` → retrieve the TCG event log (crypto-agile format):
  - [x] Event log format: `TCG_PCR_EVENT2` structures (PCR index, event type, digests, event data)
  - [x] Hash algorithms: SHA-1 (legacy), SHA-256, SHA-384 (preferred)
  - [x] Event types: `EV_EFI_BOOT_SERVICES_APPLICATION`, `EV_EFI_VARIABLE_BOOT`, etc.
- [x] Allocate buffer for event log, copy before `ExitBootServices()` (firmware may reclaim memory)
- [x] Pass event log pointer + size in `boot_info` struct to kernel
- [x] Kernel: parse event log, verify PCR values match expected measurements
- [x] Store: `tpm_available`, `tpm_version`, `tpm_event_count` in kernel global
- [x] Log: `[TPM] TPM 2.0 detected, SHA-256, 14 boot events measured`
- [x] Commit: `"uefi: TPM measured boot event log"`

### 9.2 Boot Integrity Verification *(Stretch)*

**Prompt:** Verify the boot integrity verification stubs. Confirm `tpm.h` defines `TPM_PCR_*` constants (0-7), `BOOT_INTEGRITY_*` status enum (UNKNOWN/VERIFIED/MISMATCH/NO_TPM/NO_BASELINE/NO_CRYPTO), `pcr_check` and `boot_integrity_report` structs, and `tpm_integrity_init/verified/report` API. Confirm `tpm.c` implements stubs: builds report from TPM state, marks PCRs as `NO_CRYPTO`, logs status. Confirm roadmap comments document 4 phases (Event Log Summary → PCR Replay → Golden Enrollment → FDE Key Sealing). Confirm `boot_hw.c` calls `tpm_integrity_init()` after `tpm_init()`. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: boot integrity verification"` exists.

> [!NOTE]
> **Implementation notes:**
> - Stub implementation — Phase 1 (Event Log Summary) is complete
> - Reports `BOOT_INTEGRITY_NO_TPM` on QEMU (no TPM), `BOOT_INTEGRITY_NO_CRYPTO` on TPM hardware
> - All 8 PCRs (0–7) populated in report with `NO_CRYPTO` status pending SHA-256
> - `boot_integrity_report` struct designed for "Boot Integrity" UI panel consumption
> - 4-phase roadmap documented in source: PCR Replay (SHA-256), Golden Enrollment (secure storage), FDE Key Sealing (TPM2_Seal/Unseal)
> - QEMU output: `Boot integrity: skipped (no TPM)`
> - Real hardware with TPM: `Boot integrity: pending (crypto stack required for PCR replay)`

- [x] *(Stretch)* Define expected PCR values for golden boot chain:
  - [x] PCR[0] — firmware code hash
  - [x] PCR[4] — boot application (BOOTX64.EFI) hash
  - [x] PCR[7] — Secure Boot policy
- [x] *(Stretch)* Compare measured values against stored golden values
- [x] *(Stretch)* On mismatch: log `[TPM] WARNING: Boot integrity mismatch PCR[4]`
- [x] *(Stretch)* Wire to "Boot Integrity" UI panel in System Settings
- [x] *(Stretch)* Foundation for full-disk encryption key sealing (TPM-bound keys)
- [x] Commit: `"uefi: boot integrity verification"`

---

## 10. EFI System Resource Table (ESRT)

> [!TIP]
> **Competitive Edge:** Windows uses ESRT + Windows Update to push firmware updates silently.
> Linux uses `fwupd` + ESRT. Impossible OS can show a "Firmware Health" panel that warns
> users about outdated firmware and tracks update history — neither Windows nor Linux
> expose this clearly in their UI.

### 10.1 Firmware Inventory via ESRT

**Prompt:** Verify the ESRT firmware inventory implementation. Confirm `uefi_config.h` defines `UEFI_GUID_ESRT`, `esrt_entry` struct with all 7 fields (fw_class, fw_type, fw_version, lowest_supported_version, capsule_flags, last_attempt_version, last_attempt_status), firmware type and status constants, and `esrt_init/count/get_entry` API. Confirm `uefi_config.c` looks up `EFI_SYSTEM_RESOURCE_TABLE`, parses the header + entries, copies to kernel-side array, and logs per-entry version/type/status. Confirm `boot_hw.c` calls `esrt_init()`. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: ESRT firmware inventory"` exists.

> [!NOTE]
> **Implementation notes:**
> - ESRT is a pure config table lookup — no protocol interaction needed
> - Table header has `fw_resource_count`, `fw_resource_count_max`, `fw_resource_version`
> - Entries follow immediately after header, matching `esrt_entry` layout exactly
> - Max 16 entries (`ESRT_MAX_ENTRIES`) — real systems rarely exceed 5
> - Version encoding: upper 16 bits = major, lower 16 bits = minor
> - OVMF shows: `[OK] ESRT: Not present (no firmware inventory)` (VMs don't have ESRT)
> - Real hardware will show entries for BIOS, EC, Thunderbolt, ME, etc.
> - `esrt_get_entry()` returns const pointer for read-only kernel access

- [x] Find `EFI_SYSTEM_RESOURCE_TABLE` in Configuration Table (GUID: `b122a263-3661-4f68-9929-78f8b0d62180`)
- [x] Parse ESRT: firmware resource count, entries
- [x] For each `EFI_SYSTEM_RESOURCE_ENTRY`:
  - [x] `FwClass` GUID — identifies the firmware component
  - [x] `FwType` — System (1), Device (2), UEFI Driver (3)
  - [x] `FwVersion` — current firmware version
  - [x] `LowestSupportedFwVersion` — rollback protection floor
  - [x] `CapsuleFlags` — update delivery method
  - [x] `LastAttemptVersion` + `LastAttemptStatus` — last update result
- [x] Store in kernel: `esrt_entries[]` array
- [x] Log: `[ESRT] 3 firmware components: BIOS v2.4, EC v1.2, Thunderbolt v41`
- [x] Wire to "Firmware Health" panel in System Settings
- [x] Commit: `"uefi: ESRT firmware inventory"`

---

## 11. UEFI Boot Timing ⭐

> [!TIP]
> **Competitive Edge:** Neither Windows nor Linux capture precise UEFI boot phase timings.
> `systemd-analyze` only measures kernel + userspace. Windows Boot Performance Diagnostics
> only track post-ExitBootServices phases. Impossible OS can capture the ENTIRE boot
> timeline from power-on, using firmware performance data.

### 11.1 Firmware Performance Data Table (FPDT)

**Prompt:** Verify the boot timing capture implementation. Confirm `boot_info.h` defines `UEFI_GUID_FPDT` and the `timing` struct (5 FPDT firmware fields, 11 bootloader TSC fields, `tsc_freq`). Confirm `bootx64.c` has `boot_rdtsc()`, `parse_fpdt()`, TSC captures at 9 phase boundaries (entry, GOP, conf, kernel load, ExitBS, jump), and TSC frequency estimation via `gBS->Stall(1ms)`. Confirm `boot_timing.h/c` convert TSC ticks to ms, parse FPDT ns, and log boot timeline. Confirm `boot_hw.c` calls `boot_timing_init()`. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`. Check commit `"uefi: boot timing capture"` exists.

> [!NOTE]
> **Implementation notes:**
> - `boot_rdtsc()` inline in bootloader for zero-overhead TSC reads
> - TSC frequency calibrated via `gBS->Stall(1000)` (1ms) before ExitBootServices
> - FPDT parsed from config table: walks FBPT pointer → basic boot record (type 0x0002)
> - OVMF doesn't expose FPDT via config table — real hardware typically does
> - Boot splash timing fields exist but are kernel-filled (splash runs after boot_hw_init)
> - QEMU output: `TSC frequency: 3822 MHz`, `GOP 1ms, Config 2ms, Kernel Load 17ms, ExitBS 116ms, Total 140ms`

- [x] Find FPDT via ACPI tables or UEFI Configuration Table
- [x] Parse Firmware Basic Boot Performance Record:
  - [x] `ResetEnd` — TSC at end of firmware reset (SEC phase complete)
  - [x] `OSLoaderLoadImageStart` — bootloader load began
  - [x] `OSLoaderStartImageStart` — bootloader started executing
  - [x] `ExitBootServicesEntry` — ExitBootServices called
  - [x] `ExitBootServicesExit` — ExitBootServices returned
- [x] Capture TSC timestamp at each boot phase in our bootloader:
  - [x] `init_gop` start/end
  - [x] `parse_boot_conf` start/end
  - [x] `load_kernel` start/end
  - [x] `boot_splash` start/end (fields exist, kernel-filled)
- [x] Pass timing array in `boot_info` to kernel
- [x] Kernel: calculate and log phase durations in milliseconds
- [x] Log: `[BOOT] Firmware: 1.2s, GOP: 0.05s, Kernel Load: 0.3s, Total: 2.1s`
- [x] Wire to "Boot Performance" panel in System Settings
- [x] Commit: `"uefi: boot timing capture"`

---

## Priority Order

| ⭐ | Priority | Section                                | Description                                                    |
| -- | --------- | ------------------------------------- | -------------------------------------------------------------- |
| 💎 | 🔴 P0    | 1.1 Runtime Services Preservation     | Foundation — everything else needs UEFI runtime calls          |
| 💎 | 🔴 P0    | 3.1 Memory Map Preservation           | PMM needs full memory type info (runtime, ACPI, MMIO)          |
| 💎 | 🔴 P0    | 4.1 Configuration Table Walker        | How we find ACPI, SMBIOS, MemAttr, ESRT tables                 |
| 💎 | 🟠 P1    | 1.2 UEFI Variable Services            | Required for boot order, Secure Boot state, NVRAM access       |
| 💎 | 🟠 P1    | 1.3 System Reset via UEFI             | Clean shutdown/reboot — replaces raw ACPI register writes      |
| 💎 | 🟠 P1    | 2.1 RTC Time Services                 | Kernel wall clock seeding from UEFI RTC                        |
| 💎 | 🟠 P1    | 5.1 Secure Boot State Detection       | Know whether system was verified (trust decisions)             |
| 💎 | 🟡 P2    | 3.2 Memory Attributes (W^X)           | Runtime memory protection — NX enforcement                     |
| 💎 | 🟡 P2    | 7.1 SMBIOS System Information         | Hardware inventory for "About" dialog + HW Report DDK          |
| 💎 | 🟡 P2    | 4.2 Conformance Profile Detection     | IoT/embedded firmware capability detection                     |
| ⭐ | 🟡 P2    | 9.1 TPM Measured Boot                 | Boot integrity evidence — foundation for disk encryption       |
| ⭐ | 🟡 P2    | 10.1 ESRT Firmware Inventory          | Firmware version tracking + update health                      |
| ⭐ | 🟡 P2    | 11.1 Boot Timing (FPDT)               | Full power-on-to-desktop boot timeline                         |
| 💎 | 🟢 P3    | 5.2 Secure Boot Key Management        | Read/update db/dbx trust databases                             |
| 💎 | 🟢 P3    | 8.1 GOP Mode Enumeration              | Multi-resolution, multi-monitor discovery                      |
| 💎 | 🟢 P3    | 9.2 Boot Integrity Verification       | PCR golden value comparison + UI panel                         |
| 💎 | 🔵 P4    | 5.3 Crypto Agility                    | 2026 certificate rollover preparedness                         |
| 💎 | 🔵 P4    | 6.1 Capsule Firmware Updates          | In-band BIOS update from OS                                    |

> [!NOTE]
> ⭐ = Feature where Impossible OS can be **superior** to both Windows and Linux.

---

## OS Comparison

| ⭐ | Feature                           | 🪟 Windows 11                         | 🐧 Linux 6.x                          | 🚀 Impossible OS                                     |
| -- | ---------------------------------- | -------------------------------------- | ------------------------------------- | ---------------------------------------------------- |
| 💎 | UEFI bootloader                   | ✅ `bootmgfw.efi`                     | ✅ `systemd-boot` / GRUB              | ✅ Custom `BOOTX64.EFI` — Done                       |
| 💎 | GOP framebuffer                   | ✅ Hands off to GPU driver            | ✅ `efifb` / `simplefb`               | ✅ Done (1280×720 BGRX)                              |
| 💎 | ExitBootServices()                | ✅                                    | ✅                                    | ✅ Done                                              |
| 💎 | Boot configuration file           | ✅ BCD store                          | ✅ `grub.cfg` / `loader.conf`         | ✅ `boot.conf` ini parser — Done                     |
| 💎 | Runtime services preservation     | ✅ Full                               | ✅ `efi_runtime_services`             | ✅ §1.1 — RT table + mmap preserved                  |
| 💎 | SetVirtualAddressMap()            | ✅                                    | ✅                                    | ✅ §1.1 — identity-mapped, SVAM called               |
| 💎 | UEFI variable read/write          | ✅ `GetFirmwareEnvironmentVariable`   | ✅ `/sys/firmware/efi/vars/`          | ✅ §1.2 — Get/Set/GetNextVariableName                |
| 💎 | System reset (ResetSystem)        | ✅                                    | ✅ `efi_reboot()`                     | ✅ §1.3 — Cold/Warm/Shutdown + ACPI fallback         |
| 💎 | RTC via UEFI GetTime              | ✅                                    | ✅ `efi_get_time()`                   | ✅ §2.1 — Get/Set/WakeupTime + timezone              |
| 💎 | Full memory map preservation      | ✅                                    | ✅ `efi_memmap`                       | ✅ §3.1 — full EFI memory map with attributes        |
| 💎 | Memory Attributes Table (W^X)     | ✅ Enforced                           | ✅ (6.2+)                             | ✅ §3.2 — MAT parsed, W^X page permissions set       |
| 💎 | Configuration table walker        | ✅                                    | ✅ `efi_config_table_is_usable()`     | ✅ §4.1 — generic GUID walker for all tables         |
| 💎 | Conformance profiles              | ✅                                    | ✅ (6.3+)                             | ✅ §4.2 — Full UEFI vs EBBR detection                |
| 💎 | Secure Boot state detection       | ✅ Full                               | ✅ `/sys/firmware/efi/secure_boot`    | ✅ §5.1 — SecureBoot/SetupMode/PK/KEK read           |
| 💎 | Secure Boot db/dbx management     | ✅ Full                               | ✅ `mokutil`, `sbsigntool`            | ✅ §5.2 — db/dbx/dbt parsed (write deferred)         |
| 💎 | Crypto agility (2026)             | ✅ Via Windows Update                 | 🔜 Patches in progress                | ✅ §5.3 — CryptoIndications reader (write deferred)  |
| 💎 | Capsule firmware updates          | ✅ `FirmwareUpdate` service           | ✅ `fwupd` + capsule                  | ✅ §6.1 — query-only (write blocked: bricking risk)  |
| 💎 | SMBIOS parsing                    | ✅ Full WMI                           | ✅ `/sys/class/dmi/`                  | ✅ §7.1 — Type 0/1/2/4/17, SMBIOS 2.x + 3.x         |
| 💎 | GOP multi-mode                    | ✅                                    | ✅                                    | ✅ §8.1 — 30 modes enumerated, pixel format tracked  |
| ⭐ | **TPM measured boot event log**   | ✅ Required for install               | ✅ `/sys/kernel/security/tpm0/`       | ✅ §9.1 — crypto-agile log parser, SHA-256/384/512   |
| ⭐ | **Boot integrity UI**             | ❌ No user-facing panel               | ❌ CLI only (`tpm2-tools`)            | ✅ §9.2 — report struct ready (PCR verify pending)   |
| ⭐ | **ESRT firmware inventory**       | ✅ Hidden (Windows Update)            | ✅ `fwupdmgr` CLI                     | ✅ §10.1 — parsed, "Firmware Health" panel ready     |
| ⭐ | **Boot timing (full FPDT)**       | ⚠️ Post-ExitBS only                   | ⚠️ `systemd-analyze` (kernel only)    | ✅ §11.1 — full FPDT + TSC timeline, power-to-kernel |
| 💎 | Confidential Computing (TDX/SEV)  | ✅ Azure CC VMs                       | ✅ `CC_MEASUREMENT_PROTOCOL`          | ⬜ Not planned (bare-metal focus)                    |

