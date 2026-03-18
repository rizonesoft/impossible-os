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
> [UEFI 2.10 Specification](file:///home/derickpayne/impossible-os/specs/uefi-2.10.md)
> summary in the repo at `specs/uefi-2.10.md`.

> [!NOTE]
> **Cross-references:**
> - [TODO-010-Bootloader.md](TODO-010-Bootloader.md) — Current UEFI bootloader (✅ Done: GOP, memory map, ExitBootServices)
> - [TODO-042-ACPI.md](TODO-042-ACPI.md) — ACPI tables discovered via UEFI Configuration Table
> - [TODO-044-PCI.md §4.1](TODO-044-PCI.md) — PCIe ECAM via MCFG (found in UEFI config table)
> - [TODO-043-x86-64.md §8](TODO-043-x86-64.md) — Security extensions (SMEP/SMAP/CET complement Secure Boot)
> - [TODO-550-Installer-ISO.md](../510-Long-Term-Stretch/TODO-550-Installer-ISO.md) — UEFI boot media creation

---

## 1. UEFI Runtime Services

### 1.1 Runtime Services Preservation

**Prompt:** After `ExitBootServices()`, UEFI Runtime Services remain callable by the OS kernel. The current bootloader calls `ExitBootServices()` but discards the runtime services function pointers. We need to preserve the `EFI_RUNTIME_SERVICES` table, call `SetVirtualAddressMap()` to remap runtime memory into the kernel's virtual address space, and expose runtime services to the kernel. This is the foundation for UEFI variable access, RTC, firmware updates, and system reset. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"uefi: runtime services preservation"`. Add notes directly in this TODO section.

- [ ] Preserve `EFI_RUNTIME_SERVICES` pointer from `EFI_SYSTEM_TABLE` before `ExitBootServices()`
- [ ] Save UEFI memory map (with descriptors marked `EFI_MEMORY_RUNTIME`)
- [ ] Identify all runtime memory regions from `GetMemoryMap()`:
  - [ ] `EfiRuntimeServicesCode` — firmware code that survives ExitBootServices
  - [ ] `EfiRuntimeServicesData` — firmware data that survives ExitBootServices
- [ ] Call `SetVirtualAddressMap()` early in kernel init:
  - [ ] Map all runtime regions into kernel virtual address space
  - [ ] Firmware relocates its internal pointers to match new virtual addresses
  - [ ] This call can only be made ONCE — it's irreversible
- [ ] Store runtime services function pointers in kernel global:
  - [ ] `uefi_rt->GetTime()`, `uefi_rt->SetTime()`
  - [ ] `uefi_rt->GetVariable()`, `uefi_rt->SetVariable()`
  - [ ] `uefi_rt->GetNextVariableName()`
  - [ ] `uefi_rt->ResetSystem()`
  - [ ] `uefi_rt->UpdateCapsule()`
- [ ] Check `EFI_RT_PROPERTIES_TABLE` (if present) for supported service bitmask:
  - [ ] `EFI_RT_SUPPORTED_GET_TIME` (0x0001)
  - [ ] `EFI_RT_SUPPORTED_SET_VARIABLE` (0x0040)
  - [ ] etc. — gracefully handle unsupported services returning `EFI_UNSUPPORTED`
- [ ] Serialize all runtime service calls with a spinlock (firmware is not reentrant)
- [ ] Log: `[UEFI] Runtime services active: GetTime SetVariable ResetSystem`
- [ ] Commit: `"uefi: runtime services preservation"`

### 1.2 UEFI Variable Services

**Prompt:** UEFI Variables are key-value pairs stored in firmware NVRAM, identified by a GUID namespace + name string. They're used for boot order, Secure Boot keys, and OS-firmware communication. The kernel needs `GetVariable()`, `SetVariable()`, and `GetNextVariableName()` wrappers. Variables have attributes: `NON_VOLATILE`, `BOOTSERVICE_ACCESS`, `RUNTIME_ACCESS`, `TIME_BASED_AUTHENTICATED_WRITE_ACCESS`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"uefi: variable services"`. Add notes directly in this TODO section.

- [ ] Implement `uefi_get_variable(guid, name, &data, &size, &attributes)`:
  - [ ] Call `EFI_RUNTIME_SERVICES.GetVariable()`
  - [ ] Handle `EFI_BUFFER_TOO_SMALL` — retry with larger buffer
  - [ ] Handle `EFI_NOT_FOUND` — variable doesn't exist
- [ ] Implement `uefi_set_variable(guid, name, data, size, attributes)`:
  - [ ] Call `EFI_RUNTIME_SERVICES.SetVariable()`
  - [ ] Attributes: `EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_RUNTIME_ACCESS`
  - [ ] Handle `EFI_OUT_OF_RESOURCES` — NVRAM full
- [ ] Implement `uefi_enumerate_variables()`:
  - [ ] Call `GetNextVariableName()` in a loop
  - [ ] Log all variables with GUID and name
- [ ] Read standard variables:
  - [ ] `Boot0000`–`BootFFFF` — boot option entries
  - [ ] `BootOrder` — ordered array of boot option numbers
  - [ ] `BootCurrent` — which boot option was used
  - [ ] `ConOut`, `ConIn` — console device paths
- [ ] Log: `[UEFI] NVRAM: 47 variables, BootOrder=[0001,0003,0000]`
- [ ] Commit: `"uefi: variable services"`

### 1.3 System Reset via UEFI

**Prompt:** The current shutdown/reboot uses direct ACPI register writes or keyboard controller reset. UEFI provides a clean `ResetSystem()` runtime service that handles all platform-specific details. Implement kernel wrappers that prefer UEFI reset when available. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"uefi: system reset via ResetSystem()"`. Add notes directly in this TODO section.

- [ ] Implement `uefi_reset(type)`:
  - [ ] `EfiResetCold` — full hardware reset (power cycle)
  - [ ] `EfiResetWarm` — CPU reset without power cycle
  - [ ] `EfiResetShutdown` — power off
  - [ ] `EfiResetPlatformSpecific` — platform-defined (e.g., recovery mode)
- [ ] Wire to existing `system_shutdown()` and `system_reboot()`:
  - [ ] Prefer UEFI `ResetSystem()` if runtime services are available
  - [ ] Fall back to ACPI PM register writes (current method)
  - [ ] Last resort: keyboard controller 0x64/0xFE reset
- [ ] Commit: `"uefi: system reset via ResetSystem()"`

---

## 2. UEFI Time Services

### 2.1 Real-Time Clock via UEFI

**Prompt:** UEFI provides `GetTime()` and `SetTime()` runtime services that abstract RTC hardware differences. More reliable than direct CMOS RTC access (port 0x70/0x71) because UEFI handles platform-specific quirks. The `EFI_TIME` structure includes year, month, day, hour, minute, second, nanosecond, timezone, and daylight savings. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"uefi: RTC time services"`. Add notes directly in this TODO section.

- [ ] Implement `uefi_get_time(&time, &capabilities)`:
  - [ ] Call `EFI_RUNTIME_SERVICES.GetTime()`
  - [ ] Parse `EFI_TIME` struct: year (1900–9999), month, day, hour, min, sec, nanosec
  - [ ] Parse timezone (minutes from UTC, or `EFI_UNSPECIFIED_TIMEZONE`)
  - [ ] Parse daylight savings flags
- [ ] Implement `uefi_set_time(&time)`:
  - [ ] Call `EFI_RUNTIME_SERVICES.SetTime()`
  - [ ] Validate fields before calling
- [ ] Implement `uefi_get_wakeup_time(&enabled, &pending, &time)`:
  - [ ] RTC alarm for wake-from-sleep (ties into power management)
- [ ] Wire to kernel timekeeping: use UEFI time to seed wall clock at boot
- [ ] Fallback: if UEFI time not supported (per `EFI_RT_PROPERTIES_TABLE`), use CMOS RTC
- [ ] Commit: `"uefi: RTC time services"`

---

## 3. UEFI Memory Map & Memory Attribute Tables

### 3.1 Memory Map Preservation

**Prompt:** The UEFI memory map (from `GetMemoryMap()`) describes all physical memory: conventional, reserved, ACPI, MMIO, runtime, and firmware reserved regions. The current bootloader passes a simplified memory map to the kernel. Preserve the full UEFI memory map with all type annotations for the physical memory manager and VMM. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"uefi: full memory map preservation"`. Add notes directly in this TODO section.

- [ ] Preserve complete `EFI_MEMORY_DESCRIPTOR` array from `GetMemoryMap()`:
  - [ ] `EfiConventionalMemory` — free RAM for OS use
  - [ ] `EfiLoaderCode` / `EfiLoaderData` — bootloader memory (reclaimable after boot)
  - [ ] `EfiBootServicesCode` / `EfiBootServicesData` — reclaimable after `ExitBootServices()`
  - [ ] `EfiRuntimeServicesCode` / `EfiRuntimeServicesData` — must preserve
  - [ ] `EfiACPIReclaimMemory` / `EfiACPIMemoryNVS` — ACPI tables
  - [ ] `EfiMemoryMappedIO` / `EfiMemoryMappedIOPortSpace` — device MMIO
  - [ ] `EfiReservedMemoryType` — do not touch
- [ ] Pass full memory map in `boot_info` struct to kernel
- [ ] PMM: iterate memory map, mark `EfiConventionalMemory` + reclaimable as free
- [ ] PMM: mark runtime, ACPI NVS, MMIO, reserved as unavailable
- [ ] Log: `[UEFI] Memory map: 4096 MB RAM, 127 descriptors`
- [ ] Commit: `"uefi: full memory map preservation"`

### 3.2 EFI_MEMORY_ATTRIBUTES_TABLE (W^X)

**Prompt:** UEFI 2.10 introduces `EFI_MEMORY_ATTRIBUTES_TABLE` to declare fine-grained memory permissions for runtime regions. Each descriptor annotates sub-regions as read-only, writable, or executable — enforcing W^X (Write XOR Execute). The OS should read this table to set correct page permissions for runtime services memory, preventing code injection into firmware runtime regions. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"uefi: memory attributes table W^X"`. Add notes directly in this TODO section.

- [ ] Find `EFI_MEMORY_ATTRIBUTES_TABLE` in Configuration Table (GUID lookup)
- [ ] Parse table: version, number of entries, descriptor size
- [ ] For each descriptor:
  - [ ] `EFI_MEMORY_RO` — mark pages read-only in kernel page tables
  - [ ] `EFI_MEMORY_XP` — mark pages non-executable (NX bit in page tables)
  - [ ] `EFI_MEMORY_RP` — mark pages not-present (guard pages)
- [ ] Apply permissions to runtime service memory mappings
- [ ] Verify W^X: no page should be simultaneously writable AND executable
- [ ] Fallback: if table not present, mark all runtime code RO+X, all runtime data RW+NX
- [ ] Commit: `"uefi: memory attributes table W^X"`

---

## 4. UEFI Configuration Table

### 4.1 Configuration Table Walking

**Prompt:** The `EFI_SYSTEM_TABLE.ConfigurationTable` is an array of `{GUID, VendorTable}` pairs pointing to platform-specific data (ACPI tables, SMBIOS, device tree, etc.). The current bootloader extracts the ACPI RSDP from this table. Formalize this into a proper configuration table walker that finds and exposes all relevant entries. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"uefi: configuration table walker"`. Add notes directly in this TODO section.

- [ ] Preserve `EFI_SYSTEM_TABLE.ConfigurationTable` pointer and `NumberOfTableEntries`
- [ ] Implement `uefi_find_config_table(guid)` — search by GUID, return pointer
- [ ] Known Configuration Table GUIDs:
  - [ ] `EFI_ACPI_20_TABLE_GUID` — ACPI 2.0+ RSDP (currently used)
  - [ ] `ACPI_TABLE_GUID` — ACPI 1.0 RSDP (legacy fallback)
  - [ ] `SMBIOS3_TABLE_GUID` — SMBIOS 3.x entry point
  - [ ] `SMBIOS_TABLE_GUID` — SMBIOS 2.x entry point
  - [ ] `EFI_MEMORY_ATTRIBUTES_TABLE_GUID` — §3.2 above
  - [ ] `EFI_RT_PROPERTIES_TABLE_GUID` — §1.1 above
  - [ ] `EFI_CONFORMANCE_PROFILES_TABLE_GUID` — profiles §4.2
  - [ ] `EFI_DTB_TABLE_GUID` — Device Tree Blob (non-x86 platforms)
- [ ] Log: `[UEFI] Config tables: ACPI2.0 SMBIOS3 MemAttr RtProps`
- [ ] Commit: `"uefi: configuration table walker"`

### 4.2 UEFI Conformance Profile Detection

**Prompt:** UEFI 2.10 introduces Conformance Profiles that allow firmware to declare which subset of UEFI it implements. If the `EFI_CONFORMANCE_PROFILES_TABLE` is absent, assume full UEFI conformance. If present, read the profile GUIDs to understand firmware capabilities. This matters for IoT/embedded platforms that omit heavy features. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"uefi: conformance profile detection"`. Add notes directly in this TODO section.

- [ ] Look up `EFI_CONFORMANCE_PROFILES_TABLE` in config table
- [ ] If absent: assume full UEFI 2.10 conformance (all services available)
- [ ] If present: iterate profile GUIDs and store in kernel flags:
  - [ ] `EFI_CONFORMANCE_PROFILES_UEFI_SPEC_GUID` — full conformance
  - [ ] EBBR profile — embedded minimal (no HII, limited services)
- [ ] Use profile flags to guard optional service calls
- [ ] Log: `[UEFI] Conformance: Full UEFI 2.10` or `[UEFI] Conformance: Reduced (EBBR)`
- [ ] Commit: `"uefi: conformance profile detection"`

---

## 5. Secure Boot

### 5.1 Secure Boot State Detection

**Prompt:** Detect whether the system booted with Secure Boot enabled by reading UEFI variables. The kernel should know whether it was verified by firmware, which affects trust decisions (e.g., whether to allow unsigned kernel modules). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"uefi: Secure Boot state detection"`. Add notes directly in this TODO section.

- [ ] Read UEFI variable `SecureBoot` (global GUID): 0 = off, 1 = on
- [ ] Read UEFI variable `SetupMode`: 0 = User Mode (keys enrolled), 1 = Setup Mode
- [ ] Read UEFI variable `PK` — Platform Key (if empty, Setup Mode)
- [ ] Read UEFI variable `KEK` — Key Exchange Key
- [ ] Store state in kernel: `secure_boot_enabled`, `setup_mode`
- [ ] Log: `[UEFI] Secure Boot: ENABLED (User Mode)` or `[UEFI] Secure Boot: DISABLED`
- [ ] Commit: `"uefi: Secure Boot state detection"`

### 5.2 Secure Boot Key Management *(Stretch)*

**Prompt:** The Secure Boot trust chain: Platform Key (PK) → Key Exchange Key (KEK) → Authorized Database (db) / Forbidden Database (dbx). For key management (enrolling/revoking keys), the OS writes authenticated variables with time-based signatures. This enables the OS to update the dbx (revocation list) without firmware reflash — critical for patching vulnerabilities like BootHole. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"uefi: Secure Boot key management"`. Add notes directly in this TODO section.

- [ ] *(Stretch)* Read Secure Boot databases via UEFI variable services:
  - [ ] `db` — Authorized Signature Database (trusted certs/hashes)
  - [ ] `dbx` — Forbidden Signature Database (revoked certs/hashes)
  - [ ] `dbt` — Timestamp Database
- [ ] *(Stretch)* Parse `EFI_SIGNATURE_LIST` / `EFI_SIGNATURE_DATA` structures:
  - [ ] Signature type GUIDs: SHA-256 hash, X.509 certificate, RSA-2048
  - [ ] Iterate signature list entries
- [ ] *(Stretch)* Enumerate current trust chain:
  - [ ] Log PK subject/issuer
  - [ ] Log KEK entries
  - [ ] Log number of db entries and dbx revocations
- [ ] *(Stretch)* dbx update: write authenticated variable with `TIME_BASED_AUTHENTICATED_WRITE_ACCESS`
- [ ] Commit: `"uefi: Secure Boot key management"`

### 5.3 Crypto Agility (2026 Preparedness) *(Future)*

**Prompt:** UEFI 2.10 introduces `CryptoIndicationsSupported`, `CryptoIndications`, and `CryptoIndicationsActivated` variables for dynamic algorithm negotiation between OS and firmware. This enables transition from SHA-256/RSA-2048 to stronger algorithms (SHA-384/512, RSA-3072/4096, ECDSA P-384) without firmware reflash. Critical for the 2026 Secure Boot certificate expiry. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"uefi: crypto agility framework"`. Add notes directly in this TODO section.

- [ ] *(Future)* Read `CryptoIndicationsSupported` variable (firmware-owned, lists all supported algorithms)
- [ ] *(Future)* Parse `EFI_CRYPTO_INDICATION` bitmask:
  - [ ] RSA-2048, RSA-3072, RSA-4096 (PKCS#1 v1.5 and PSS)
  - [ ] ECDSA P-256, P-384
  - [ ] SHA-256, SHA-384, SHA-512
- [ ] *(Future)* Write `CryptoIndications` variable (OS-owned, requests specific algorithms)
- [ ] *(Future)* Read `CryptoIndicationsActivated` (firmware confirms activated set)
- [ ] *(Future)* Log: `[UEFI] Crypto: RSA-4096+SHA-384 active (SHA-256 deprecated)`
- [ ] Commit: `"uefi: crypto agility framework"`

---

## 6. Firmware Updates (Capsule)

### 6.1 UEFI Capsule Update Support *(Stretch)*

**Prompt:** UEFI provides `UpdateCapsule()` and `QueryCapsuleCapabilities()` runtime services for in-band firmware updates. The OS packages a firmware update into a capsule (binary blob), passes it to the firmware, and the firmware applies it on next reboot. This enables BIOS/UEFI updates from within the OS without manual BIOS flashing. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"uefi: capsule firmware update"`. Add notes directly in this TODO section.

- [ ] *(Stretch)* Implement `uefi_query_capsule(type)`:
  - [ ] Call `QueryCapsuleCapabilities()` — check max capsule size, supported types
  - [ ] Check if firmware supports capsule reset (reboot-to-update)
- [ ] *(Stretch)* Implement `uefi_update_capsule(data, size)`:
  - [ ] Allocate capsule buffer in runtime services memory
  - [ ] Build `EFI_CAPSULE_HEADER`: CapsuleGuid, HeaderSize, Flags, CapsuleImageSize
  - [ ] Set `CAPSULE_FLAGS_PERSIST_ACROSS_RESET` for reboot-applied updates
  - [ ] Call `UpdateCapsule()` → firmware stages update for next boot
- [ ] *(Stretch)* Handle capsule results on subsequent boot:
  - [ ] Read `CapsuleResultVariableXXXX` UEFI variables for update status
- [ ] Commit: `"uefi: capsule firmware update"`

---

## 7. SMBIOS Table Parsing

### 7.1 SMBIOS System Information

**Prompt:** SMBIOS (System Management BIOS) tables provide hardware inventory information: system manufacturer, model, serial number, BIOS version, CPU sockets, memory DIMMs, etc. Found via UEFI Configuration Table. Useful for the "About This PC" dialog and hardware detection. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"uefi: SMBIOS system information"`. Add notes directly in this TODO section.

- [ ] Find SMBIOS entry point via `uefi_find_config_table(SMBIOS3_TABLE_GUID)`
  - [ ] Fallback: `SMBIOS_TABLE_GUID` for SMBIOS 2.x
- [ ] Parse SMBIOS 3.0 64-bit entry point:
  - [ ] Anchor string `_SM3_`, entry point length, major/minor version
  - [ ] Maximum structure table length, structure table address
- [ ] Walk SMBIOS structure table (type + length + handle + data + strings):
  - [ ] **Type 0 — BIOS Information:** vendor, version, release date, BIOS size
  - [ ] **Type 1 — System Information:** manufacturer, product name, serial, UUID
  - [ ] **Type 2 — Baseboard:** manufacturer, product, serial
  - [ ] **Type 4 — Processor:** socket, family, manufacturer, max speed, core count
  - [ ] **Type 17 — Memory Device:** size, speed, type (DDR4/DDR5), location
  - [ ] **Type 127 — End of Table:** stop walking
- [ ] Store in kernel: `system_info.manufacturer`, `system_info.product`, etc.
- [ ] Log: `[SMBIOS] QEMU Virtual Machine, 4 GB RAM, OVMF BIOS 2024.08`
- [ ] Wire to "System Information" panel / About dialog
- [ ] Commit: `"uefi: SMBIOS system information"`

---

## 8. UEFI GOP Enhancements

### 8.1 Multi-Monitor / Mode Enumeration *(Stretch)*

**Prompt:** The current bootloader picks the first available GOP mode. UEFI GOP supports multiple modes (resolutions) and potentially multiple framebuffers (multi-monitor). Enumerate all available modes, select the best resolution, and expose mode information to the kernel for display management. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"uefi: GOP mode enumeration"`. Add notes directly in this TODO section.

- [ ] *(Stretch)* Enumerate all GOP modes via `QueryMode()`
  - [ ] For each mode: width, height, pixel format, pixels per scan line
  - [ ] Select best mode: prefer native resolution, fall back to 1920×1080, 1280×720
- [ ] *(Stretch)* Support pixel formats:
  - [ ] `PixelRedGreenBlueReserved8BitPerColor` (RGBX)
  - [ ] `PixelBlueGreenRedReserved8BitPerColor` (BGRX — most common)
  - [ ] `PixelBitMask` (custom channel masks)
- [ ] *(Stretch)* Multi-framebuffer: locate additional GOP protocol handles for multi-monitor
- [ ] *(Stretch)* Pass mode info to kernel: width, height, stride, pixel format, framebuffer base
- [ ] Commit: `"uefi: GOP mode enumeration"`

---

## Priority Order

| Priority | Section                           | Description                                                  |
|----------|-----------------------------------|--------------------------------------------------------------|
| 🔴 P0    | 1.1 Runtime Services Preservation | Foundation — everything else needs UEFI runtime calls         |
| 🔴 P0    | 3.1 Memory Map Preservation       | PMM needs full memory type info (runtime, ACPI, MMIO)         |
| 🔴 P0    | 4.1 Configuration Table Walker    | How we find ACPI, SMBIOS, MemAttr tables                      |
| 🟠 P1    | 1.2 UEFI Variable Services        | Required for boot order, Secure Boot state, NVRAM access       |
| 🟠 P1    | 1.3 System Reset via UEFI         | Clean shutdown/reboot — replaces raw ACPI register writes      |
| 🟠 P1    | 2.1 RTC Time Services             | Kernel wall clock seeding from UEFI RTC                        |
| 🟠 P1    | 5.1 Secure Boot State Detection   | Know whether system was verified (trust decisions)             |
| 🟡 P2    | 3.2 Memory Attributes (W^X)       | Runtime memory protection — NX enforcement                     |
| 🟡 P2    | 7.1 SMBIOS System Information     | Hardware inventory for "About" dialog                          |
| 🟡 P2    | 4.2 Conformance Profile Detection | IoT/embedded firmware capability detection                     |
| 🟢 P3    | 5.2 Secure Boot Key Management    | Read/update db/dbx trust databases                             |
| 🟢 P3    | 8.1 GOP Mode Enumeration          | Multi-resolution, multi-monitor discovery                      |
| 🔵 P4    | 5.3 Crypto Agility                | 2026 certificate rollover preparedness                         |
| 🔵 P4    | 6.1 Capsule Firmware Updates      | In-band BIOS update from OS                                    |

---

## OS Comparison

| Feature                               | 🪟 Windows 11                         | 🐧 Linux 6.x                          | 🚀 Impossible OS                          |
| ------------------------------------- | ------------------------------------- | -------------------------------------- | ----------------------------------------- |
| UEFI bootloader                       | ✅ `bootmgfw.efi`                      | ✅ `systemd-boot` / GRUB               | ✅ Custom `BOOTX64.EFI` — Done            |
| GOP framebuffer                       | ✅ (hands off to GPU driver)            | ✅ `efifb` / `simplefb`                | ✅ Done (1280×720 BGRX)                   |
| ExitBootServices()                    | ✅                                      | ✅                                      | ✅ Done                                    |
| Runtime services preservation         | ✅ Full                                 | ✅ `efi_runtime_services`               | ⬜ §1.1 P0 — discarded after exit         |
| SetVirtualAddressMap()                | ✅                                      | ✅                                      | ⬜ §1.1 P0                                |
| UEFI variable read/write              | ✅ `GetFirmwareEnvironmentVariable`     | ✅ `/sys/firmware/efi/vars/`            | ⬜ §1.2 P1                                |
| System reset (ResetSystem)            | ✅                                      | ✅ `efi_reboot()`                       | ⬜ §1.3 P1 — uses ACPI/KB reset           |
| RTC via UEFI GetTime                  | ✅                                      | ✅ `efi_get_time()`                     | ⬜ §2.1 P1 — uses CMOS RTC                |
| Full memory map preservation          | ✅                                      | ✅ `efi_memmap`                         | ⚠️ Simplified map — §3.1 P0              |
| EFI_MEMORY_ATTRIBUTES_TABLE (W^X)    | ✅ Enforced                             | ✅ (6.2+)                               | ⬜ §3.2 P2                                |
| Configuration table walker            | ✅                                      | ✅ `efi_config_table_is_usable()`       | ⚠️ ACPI-only — §4.1 P0                   |
| Conformance profiles                  | ✅                                      | ✅ (6.3+)                               | ⬜ §4.2 P2                                |
| Secure Boot state detection           | ✅ Full                                 | ✅ `/sys/firmware/efi/secure_boot`      | ⬜ §5.1 P1                                |
| Secure Boot db/dbx management         | ✅ Full                                 | ✅ `mokutil`, `sbsigntool`              | ⬜ §5.2 P3                                |
| Crypto agility (2026)                 | ✅ (via Windows Update)                 | 🔜 (patches in progress)               | ⬜ §5.3 P4                                |
| Capsule firmware updates              | ✅ `FirmwareUpdate` service             | ✅ `fwupd` + capsule                    | ⬜ §6.1 P4                                |
| SMBIOS parsing                        | ✅ Full WMI                             | ✅ `/sys/class/dmi/`                    | ⬜ §7.1 P2                                |
| GOP multi-mode                        | ✅                                      | ✅                                      | ⬜ §8.1 P3 — single mode                  |
| **Confidential Computing (TDX/SEV)** | ✅ Azure CC VMs                         | ✅ `CC_MEASUREMENT_PROTOCOL`            | ⬜ Not planned (bare-metal focus)          |
