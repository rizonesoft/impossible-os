<!-- docs: covers=todo/01-boot-platform/TODO-27-uefi-advanced.md sources=src/boot/uefi/bootx64.c,include/kernel/boot_info.h,src/kernel/firmware_advisor.c,include/kernel/firmware_advisor.h,src/kernel/firmware_capsule_refused.c,user/sysinfo/sysinfo.c,src/kernel/uefi_runtime.c,src/kernel/mm/vmm.c,src/kernel/smbios.c,include/kernel/smbios.h,include/kernel/uefi_config.h reviewed=2026-09-28 order=27 -->
# Advanced UEFI Features

## What is it?

These are the UEFI features beyond the core boot path: finding other operating systems on the disk and chainloading them, telling the operator which firmware updates exist without ever writing firmware, enforcing write-XOR-execute (W^X) on UEFI runtime memory, enumerating every graphics output instead of one, and reading more SMBIOS tables into the Registry. None of them is needed to boot.

Four pieces ship: multi-OS chainload, the read-only firmware update advisor, static W^X enforcement, and multi-GPU GOP enumeration, plus SMBIOS Type 2, 3, 16 and 19 parsing. Secure Boot enforcement policy and DBX revocation freshness are not implemented. W^X has a deferred runtime extension, and the advisor's live metadata refresh and fixture tests are open follow-ups.

## How does it work?

**Multi-OS chainload.** Before `ExitBootServices()`, `chainload_detect()` scans the other volumes for well-known foreign bootloader files, recording up to eight. `chainload_synthesize()` adds each as a chainload entry in the boot menu, after the Impossible OS default. Choosing one runs `chainload_exec()`, which loads and starts the target through firmware. Secure Boot verifies the target as usual; a Secure Boot refusal is logged and boot falls back to Impossible OS.

**Firmware update advisor.** `firmware_advisor_init()` reads the ESRT (through the `uefi_config.h` API) and an offline metadata cache at `X:\Diag\lvfs-metadata.json`, joins them by firmware class GUID, and classifies each component as up to date, update available or unknown (`fa_classify()`). Results go to the Registry under `HKLM\SOFTWARE\Impossible\FirmwareAdvisor\` and to `X:\Diag\firmware-advisor.json` (`fa_publish_json()`). The advisor never calls `UpdateCapsule()`, never sets the `OsIndications` capsule bit and never stages a capsule; `firmware_capsule_refused.c` pins that refusal with a compile-time assertion and linker symbols.

**W^X enforcement.** `uefi_runtime_enforce_wx()` walks the parsed `EFI_MEMORY_ATTRIBUTES_TABLE` (`mat_get_count()`, `mat_get_entry()`), marking data regions non-executable with `vmm_set_nx()` and code regions read-only with `vmm_set_ro()`. Those functions change one page-table bit and flush only the local TLB, so enforcement refuses to run once more than one CPU is online.

**Multi-GPU GOP.** `gop_enumerate_and_select()` finds every Graphics Output Protocol handle and records up to four heads' geometry in `boot_info.gop_handles[]`. The primary is the head attached to the firmware console output, then a console-device tie-breaker, then the largest resolution. A secondary head that fails validation keeps its geometry without a framebuffer address.

**SMBIOS extended types.** The SMBIOS walker parses Types 2, 3, 16 and 19 into `HKLM\HARDWARE\Baseboard`, `HKLM\HARDWARE\Chassis` and `HKLM\HARDWARE\MemoryArray`. `smbios_chassis_is_laptop()` reports whether the chassis type is a portable one.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `chainload_detect()`, `chainload_synthesize()`, `chainload_exec()` | Find foreign OS loaders, add menu entries, chainload on selection ([`bootx64.c`](../../src/boot/uefi/bootx64.c)) |
| `firmware_advisor_init()`, `fa_classify()`, `fa_publish_json()` | ESRT and cache join, per-component classification, JSON publish ([`firmware_advisor.c`](../../src/kernel/firmware_advisor.c), [`firmware_advisor.h`](../../include/kernel/firmware_advisor.h)) |
| `firmware_capsule_refused.c` | Pins the no-capsule-write refusal ([`firmware_capsule_refused.c`](../../src/kernel/firmware_capsule_refused.c)) |
| `sysinfo firmware-updates` | Prints the advisor's table and the refusal line ([`sysinfo.c`](../../user/sysinfo/sysinfo.c)) |
| `uefi_runtime_enforce_wx()` | Applies NX and read-only per Memory Attributes Table region ([`uefi_runtime.c`](../../src/kernel/uefi_runtime.c)) |
| `vmm_set_nx()`, `vmm_set_ro()` | Single-bit page-table updates used by the W^X walk ([`vmm.c`](../../src/kernel/mm/vmm.c)) |
| `mat_get_count()`, `mat_get_entry()` | Memory Attributes Table consumer API ([`uefi_config.h`](../../include/kernel/uefi_config.h)) |
| `gop_enumerate_and_select()`, `struct boot_gop_handle` | Enumerate GOP heads and hand their geometry to the kernel ([`bootx64.c`](../../src/boot/uefi/bootx64.c), [`boot_info.h`](../../include/kernel/boot_info.h)) |
| `smbios_chassis_is_laptop()` | Chassis classification from SMBIOS Type 3 ([`smbios.c`](../../src/kernel/smbios.c), [`smbios.h`](../../include/kernel/smbios.h)) |

## How do I use it?

Chainload detection, W^X, GOP enumeration and SMBIOS parsing run on every boot. Their serial lines:

- `[MULTIBOOT] detected <name>`, or `[MULTIBOOT] no foreign OS bootloaders found`; choosing a foreign entry logs `[MULTIBOOT] chainloading <name>`, and a Secure Boot refusal logs `[MULTIBOOT] chainload REFUSED -- Secure Boot dbx`.
- `W^X enforced on <n> runtime memory regions (static MAT)`, a `W^X DEGRADED: ...` line when a region failed, or `W^X enforcement refused: SMP active (no TLB shootdown)` when other CPUs are already online.
- `[BOOT] GOP: <n> handle found` (or `handles found`), or `[BOOT] GOP: none found, headless boot`.
- `Registry populated: BIOS, System, <n> CPU(s), <n> DIMM(s)` followed by the extra tables that were present.

At the `C:\>` prompt:

```
sysinfo firmware-updates
```

On QEMU with OVMF, which reports no ESRT entries, this prints `No firmware components reported by ESRT (firmware does not advertise updatable resources).` Every run ends with `Apply via the vendor's BIOS update tool; Impossible OS does not write firmware.`

## What is not implemented yet?

- Secure Boot enforcement policy (the enforcement switch, audit mode, deployed-mode protection) moved to the kernel lockdown work, which is not built: [Secure Boot Enforcement Policy](../../todo/01-boot-platform/TODO-27-uefi-advanced.md#5-secure-boot-enforcement-policy).
- DBX revocation freshness is not built; it needs an official revocation list bound to a compiled-in digest or signed release manifest: [DBX Revocation List Sync](../../todo/01-boot-platform/TODO-27-uefi-advanced.md#7-dbx-revocation-list-sync).
- W^X covers only the static table at boot; keeping firmware page permissions in step at runtime through `EFI_MEMORY_ATTRIBUTE_PROTOCOL` is deferred: [UEFI Memory Attributes (W^X)](../../todo/01-boot-platform/TODO-27-uefi-advanced.md#3-uefi-memory-attributes-wx).
- The advisor only reads the offline cache; a signed, fetched refresh waits on the kernel HTTPS client, and fixture tests with real ESRT rows are open: [Firmware Advisor Live Metadata Refresh and Fixture Coverage](../../todo/01-boot-platform/TODO-27-uefi-advanced.md#8-firmware-advisor-live-metadata-refresh-and-fixture-coverage).

## How does it compare with Windows 11 and Linux?

The roadmap rates chainload discovery ahead: Windows Boot Manager and GRUB list other systems through separate configuration (BCD entries, os-prober), while Impossible OS detects them itself at boot. The firmware advisor is deliberately narrower than Windows Update and fwupd, which both apply firmware; Impossible OS only reports and points the operator at the vendor's tool, because a failed firmware write can make a machine unbootable. Static W^X, multi-GPU GOP and the SMBIOS tables are at parity. Secure Boot enforcement and DBX freshness exist on both other systems (Windows lockdown and silent DBX updates; Linux kernel lockdown, fwupd and dbxtool) and are missing here.

## See also

- [UEFI Advanced Features roadmap](../../todo/01-boot-platform/TODO-27-uefi-advanced.md)
- [UEFI Bootloader Hardening and Secure Boot](uefi-hardening-overview.md)
- [OS-Visible Loader Variables](loader-vars.md)
- [Boot Entries, Menu and Policy](boot-entries-menu-policy.md)
- [boot_info Field Ownership](boot-info-fields.md)
