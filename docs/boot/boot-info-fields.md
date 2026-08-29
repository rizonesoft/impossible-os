# struct boot_info -- Canonical Field Ownership Matrix

> **Owner:** [Boot Protocol ABI & Handoff Contract roadmap -- Canonical boot_info Field Ownership Table section](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#1-canonical-boot_info-field-ownership-table).
> **Single source of truth.** [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) has a file-top comment that points here rather than duplicating policy inline. Every field of `struct boot_info` (and its nested structs) is listed below with producer, first valid phase, consumer(s), lifetime, owning roadmap, and validation rule. Add a field -> add a row.
>
> **Machine-readable complement.** Every build emits [`build/boot-info-abi.kernel.json`](../../build/boot-info-abi.kernel.json) and [`build/boot-info-abi.mirror.json`](../../build/boot-info-abi.mirror.json) (struct size + SHA-256 fingerprint + the per-field row set; current count printed by `make boot-info-doc-coverage`) via the `boot_info ABI` step of `bash scripts/build.sh`. The two JSONs are diffed by [`tools/boot-info-manifest/compare.sh`](../../tools/boot-info-manifest/compare.sh); any field / offset / size drift between `include/kernel/boot_info.h` and [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h) fails the build with the first mismatching field named. The row set includes indexed leaves for every array-of-struct member (`mmap[0].*`, `gop_modes[0].*`, `config_table[0].*`, `rt_mmap[0].*`, `usb_devices[0].*` plus nested `endpoints[0].*`) and element-size sentinels for every scalar array (`usb_controller.dma_pages[0]`, `uefi_boot_order[0]`, `boot_device_path[0]`, `hv_vendor[0]`, ...), so the manifest also catches same-size element-type changes the six pinned `_Static_assert` offsets miss.

## How to read this doc

- **Producer** -- who writes the value. `bootx64` is the UEFI bootloader ([`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)). `mb2` is the legacy GRUB/Multiboot2 path ([`src/kernel/multiboot2_parse.c`](../../src/kernel/multiboot2_parse.c), [`src/boot/entry.asm`](../../src/boot/entry.asm)). `kernel` marks fields written by kernel code AFTER boot_info handoff.
- **First valid phase** -- the earliest point a consumer can safely read the field. `P0` = entry into `boot_phase0`, after `boot_info_validate*` succeeds; `P1` = after `boot_phase1` init order completes; `P2` = after `boot_phase2`; `P3` = after `boot_phase3` / user-mode; `K` = set later by kernel code (not populated on entry).
- **Consumer(s)** -- primary kernel files or subsystems that read the field. Non-exhaustive; the column names representative callers.
- **Lifetime** -- how long the value remains meaningful. `boot` = valid only during boot, stale once the consumer has caught up. `runtime` = required for the whole OS lifetime (e.g., runtime services, hypervisor flags). `handoff` = valid only until PMM reclaims the underlying memory.
- **Owning roadmap** -- the TODO section whose scope includes this field's producer/consumer contract.
- **Validation** -- the check that rejects malformed data. `boot_info_validate_*` covers the header; individual subsystems own deeper validation (e.g., `vmm_map_mmio_uc` for MMIO ranges).

ABI invariants (enforced by `_Static_assert` in the header):
- `boot_info_header` is at offset 0, exactly 8 bytes.
- `sizeof(struct boot_info)` fits in `uint16_t` (header.size).
- Six critical count fields are pinned by offset between kernel and bootloader mirror: `mmap_count`, `gop_mode_count`, `config_table_count`, `rt_mmap_count`, `usb_device_count`, `uefi_boot_current`.
- `boot_config.cmdline` is at byte offset 32 (stable across versions); `boot_config.config_found` at 288; `sizeof(struct boot_config) == 512`.

Every version bump goes in both [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) and [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h) (the mirror header included by both [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) and the host manifest dumper [`tools/boot-info-manifest/dump-mirror.c`](../../tools/boot-info-manifest/dump-mirror.c)). Current: `BOOT_INFO_VERSION = 24`.

## Top-level `struct boot_info` fields

### ABI header

| Field     | Producer | First valid | Consumer                                | Lifetime | Owning roadmap | Validation |
| --------- | -------- | ----------- | --------------------------------------- | -------- | -------------- | ---------- |
| `header.magic`   | bootx64 / mb2 | P0 | `boot_info_validate_header` | runtime | boot-protocol §1 | `== BOOT_INFO_MAGIC` (`"IPOS"`). |
| `header.version` | bootx64 / mb2 | P0 | `boot_info_validate_header` | runtime | boot-protocol §1 | `== BOOT_INFO_VERSION` exact match (stale-loader detector). |
| `header.size`    | bootx64 / mb2 | P0 | `boot_info_validate_header` | runtime | boot-protocol §1 | `== sizeof(struct boot_info)` kernel-side. |

### Memory map

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `mmap[BOOT_MMAP_MAX_ENTRIES]` | bootx64 (UEFI mmap pass) | P0 | `pmm_init`, `vmm_apply_nx_policy`, runtime services map | boot | boot-protocol §6 (reservation table) | Each entry walked against `BOOT_MMAP_MAX_ENTRIES`; per-entry sanity checked in PMM ingestion. |
| `mmap_count`                  | bootx64 | P0 | same | boot | boot-protocol §6 | `<= BOOT_MMAP_MAX_ENTRIES`; offset pinned (§3 mirror fingerprint). |
| `mmap_truncated`              | bootx64 | P0 | kernel boot log (warn if 1) | boot | boot-protocol §6 | Advisory flag; non-zero prints a serial warning. |
| `mmap_quirks`                 | bootx64 | P0 | kernel boot log | boot | boot-protocol §6 | Advisory. |
| `_mmap_pad[2]`                | bootx64 (zero fill) | P0 | none (alignment) | -- | -- | Zero. |

### Basic memory (legacy)

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `mem_lower_kb` | bootx64 / mb2 | P0 | **none** (deprecate) | boot | boot-protocol §1 (retain alias) | None. See **Legacy / Multiboot2-only** block below. |
| `mem_upper_kb` | bootx64 / mb2 | P0 | **none** (deprecate) | boot | boot-protocol §1 (retain alias) | None. |

### Framebuffer

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `fb` (see nested table below) | bootx64 / mb2 | P0 | `framebuffer.c`, `vmm_map_mmio_uc` | runtime | graphics-asset-foundation | `fb_available == 1` gate; individual geometry checks in `fb_init`. |
| `fb_available`                | bootx64 / mb2 | P0 | same | runtime | graphics-asset-foundation | `0 | 1`. |
| `hidpi`                       | bootx64       | P0 | desktop / DPI scaling | runtime | graphics-asset-foundation | `0 | 1` (negotiated width >= 2560). |
| `fb_pad[2]`                   | bootx64       | P0 | none (alignment) | --   | --             | Reserved; zero. |

### GOP mode list

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `gop_modes[BOOT_GOP_MODE_MAX]` | bootx64 | P0 | display mode switcher (future) | boot | graphics-asset-foundation | `gop_mode_count <= BOOT_GOP_MODE_MAX`; offset pinned. |
| `gop_mode_count`               | bootx64 | P0 | same | boot | graphics-asset-foundation | `<= BOOT_GOP_MODE_MAX`. Producer enumerates while `i < BOOT_GOP_MODE_MAX`, so a full table legitimately equals the constant. |
| `gop_mode_selected`            | bootx64 | P0 | same | boot | graphics-asset-foundation | `<= gop_mode_count`. Indexed access only when `< gop_mode_count`; the bootloader writes `gop_mode_selected = gop_mode_count` as an off-table sentinel when the active firmware mode was not captured in the bounded `gop_modes[]` array, and the kernel consumer treats `sel >= mc` as "active mode unknown" (`bootx64.c` near line 1955 + `boot_interrupts.c` near line 282). |

### ACPI

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `acpi_rsdp_addr` | bootx64 / mb2 | P0 | `acpi_init` | runtime | kernel-init-sequencing | Non-zero when `acpi_available == 1`; `acpi_init` walks RSDP signature. |
| `acpi_version`   | bootx64 / mb2 | P0 | `acpi_init` | runtime | kernel-init-sequencing | `1` (RSDP v1) or `2` (RSDP v2). |
| `acpi_available` | bootx64 / mb2 | P0 | gate for ACPI path | runtime | kernel-init-sequencing | `0 | 1`. |

### Module (GRUB legacy)

These fields predate the typed payload descriptor array in [boot-protocol roadmap -- Optional Payload Descriptor Array section](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#4-optional-payload-descriptor-array). §4 explicitly directs "Keep legacy single `module_start/module_end` as compatibility aliases until consumers migrate"; no kernel code outside the Multiboot2 parser reads these today.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `module_start`     | mb2 only (zero on native UEFI) | P0 | none today; retained as compatibility alias | handoff | boot-protocol §4 / §5 | None. `module_available` gates reads. Live writer: `src/kernel/multiboot2_parse.c` lines 110-113 (only). The native `bootx64` does NOT populate this; UKI/native consumers should use the typed payload descriptor array (§4) instead. |
| `module_end`       | mb2 only (zero on native UEFI) | P0 | none today; retained as compatibility alias | handoff | boot-protocol §4 / §5 | None. Same producer truth as `module_start`. |
| `module_available` | mb2 only (zero on native UEFI) | P0 | gate for legacy consumers | boot | boot-protocol §4 / §5 | `0 | 1`. Zero on native UEFI means "no MB2 module"; consumers must check this gate before reading the alias fields. |

See **Legacy / Multiboot2-only** block below for the full retain/deprecate breakdown.

### Boot configuration (`boot.conf`)

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `config` (see nested table below) | bootx64 (parses `\EFI\ImpossibleOS\boot.conf`) | P0 | boot init, test runner, splash, debug paths | runtime | boot-entry-store-menu-policy | Size + offsets pinned at compile time; `config_found` gates fallbacks. |

### UEFI Configuration Table

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `config_table[BOOT_CONFIG_TABLE_MAX]` | bootx64 | P0 | ACPI, SMBIOS, FPDT, MEM_ATTR lookups | runtime | kernel-init-sequencing | `config_table_count <= BOOT_CONFIG_TABLE_MAX`; offset pinned. GUID match per consumer. |
| `config_table_count`                   | bootx64 | P0 | same | runtime | kernel-init-sequencing | `<= BOOT_CONFIG_TABLE_MAX`. |

### UEFI Runtime Services

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `uefi_runtime_services`    | bootx64 | P0 | `uefi_runtime.c` | runtime | kernel-init-sequencing | Gated by `uefi_rt_available == 1`. |
| `uefi_rt_available`        | bootx64 | P0 | same | runtime | kernel-init-sequencing | `0 | 1`. |
| `rt_mmap[BOOT_RT_MMAP_MAX]` | bootx64 | P0 | `SetVirtualAddressMap`, vmm identity map survival | runtime | kernel-init-sequencing | `rt_mmap_count <= BOOT_RT_MMAP_MAX`; offset pinned. |
| `rt_mmap_count`            | bootx64 | P0 | same | runtime | kernel-init-sequencing | `<= BOOT_RT_MMAP_MAX`. |
| `uefi_mmap_desc_size`      | bootx64 | P0 | SVAM call shape | runtime | kernel-init-sequencing | Non-zero when `uefi_rt_available == 1`. |
| `uefi_mmap_desc_version`   | bootx64 | P0 | SVAM call shape | runtime | kernel-init-sequencing | Non-zero when `uefi_rt_available == 1`. |
| `uefi_runtime` (see nested table) | bootx64 | P0 | per-function dispatcher | runtime | kernel-init-sequencing | Individual function pointers non-NULL when `svam_called == 1`. |

### TPM measured boot

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `tpm_event_log`      | bootx64 | P0 | TPM event-log parser | runtime | tpm-measured-boot-attestation | Non-zero when `tpm_available == 1`; `tpm_event_log_size` bounded. |
| `tpm_event_log_size` | bootx64 | P0 | same | runtime | tpm-measured-boot-attestation | `> 0` when `tpm_available == 1`. |
| `tpm_available`      | bootx64 | P0 | gate for TPM path | runtime | tpm-measured-boot-attestation | `0 | 1`. |
| `tpm_version`        | bootx64 | P0 | TPM 1.2 vs 2.0 dispatch | runtime | tpm-measured-boot-attestation | `0` (none), `1` (1.2), `2` (2.0). |
| `tpm_event_count`    | bootx64 | P0 | event-log walker | runtime | tpm-measured-boot-attestation | Matches events parsed from `tpm_event_log`. |

### USB discovery

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `usb_devices[BOOT_USB_MAX_DEVICES]` | bootx64 | P0 | xHCI driver, USB enumeration | handoff | usb-zero-delay-handover | Per-entry `active == 1`; `usb_device_count <= BOOT_USB_MAX_DEVICES`; offset pinned. |
| `usb_device_count`                   | bootx64 | P0 | same | handoff | usb-zero-delay-handover | `<= BOOT_USB_MAX_DEVICES`. |
| `usb_discovery_ok`                   | bootx64 | P0 | gate for zero-delay path | handoff | usb-zero-delay-handover | `0 | 1`. |
| `usb_handover_complete`              | bootx64 | P0 | PMM reservation pass | handoff | usb-zero-delay-handover | `0 | 1` (gate for `usb_controller` reads). |
| `usb_pad[2]`                         | bootx64 | P0 | none (alignment) | -- | -- | Reserved. |
| `usb_controller` (see nested table)  | bootx64 | P0 | xHCI driver + PMM reservation | handoff | usb-zero-delay-handover | Gated by `usb_handover_complete == 1`; each phys addr checked in PMM reservation. |

### Boot timing (FPDT + TSC)

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `timing.*` (see nested table) | bootx64 + firmware FPDT | P0 | boot perf dashboard, `X:\Perf\` staging | runtime | kernel-init-sequencing | `fpdt_available == 1` gates FPDT fields; TSC fields non-zero if `tsc_freq > 0`. |

### Serial port

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `serial_port`   | bootx64 | P0 | `serial_init` | runtime | kernel-init-sequencing | One of 0 (absent), 0x3F8 (COM1), 0x2F8 (COM2). |
| `serial_source` | bootx64 | P0 | serial probe logs | boot | kernel-init-sequencing | `0` none, `1` SPCR, `2` I/O probe. |
| `_serial_pad`   | bootx64 | P0 | none (alignment) | -- | -- | Reserved. |
| `serial_baud`   | bootx64 | P0 | `serial_init` | runtime | kernel-init-sequencing | `0` means "use default 38400"; otherwise baud rate from SPCR. |

### Last-boot error (NVRAM)

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `last_boot_error` | bootx64 (reads `ImpossibleOS-LastBootError` NVRAM var) | P0 | boot recovery UX | runtime | bootloader-error-recovery | `0` = last boot OK; otherwise panic/error code consumed by QR/BSOD path. |

### Boot device identity

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `boot_device_type`        | bootx64 | P0 | HKLM\SYSTEM\Boot\Device populator | runtime | boot-device-discovery | `0` unknown, `1` SATA, `2` NVMe, `3` USB, `4` network, `5` SD, `6` eMMC. |
| `_boot_dev_pad[3]`        | bootx64 | P0 | none (alignment) | -- | -- | Reserved. |
| `boot_device_path[128]`   | bootx64 | P0 | same | runtime | boot-device-discovery | Null-terminated UEFI device-path text. |

### UEFI boot variables

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `uefi_boot_current`        | bootx64 | P0 | boot-entry store | runtime | boot-entry-store-menu-policy | Offset pinned. |
| `uefi_boot_next`           | bootx64 | P0 | one-shot override consumer | runtime | boot-entry-store-menu-policy | `0xFFFF` when `uefi_boot_next_valid == 0`. |
| `uefi_boot_next_valid`     | bootx64 | P0 | same | runtime | boot-entry-store-menu-policy | `0 | 1`. |
| `uefi_boot_order_count`    | bootx64 | P0 | boot-order walker | runtime | boot-entry-store-menu-policy | `<= 16`. |
| `_boot_var_pad[2]`         | bootx64 | P0 | none (alignment) | -- | -- | Reserved. |
| `uefi_boot_order[16]`      | bootx64 | P0 | boot-order walker | runtime | boot-entry-store-menu-policy | First 16 entries; bounded. |

### Boot partition

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `boot_partition_guid[16]` | bootx64 | P0 | HKLM\SYSTEM\Boot\Device populator | runtime | boot-device-discovery | Raw GPT GUID or first 4 bytes = MBR signature. |
| `boot_partition_style`    | bootx64 | P0 | same | runtime | boot-device-discovery | `0` unknown, `1` MBR, `2` GPT. |

### Removable media

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `boot_device_removable` | bootx64 | P0 | recovery / safe-boot heuristics | runtime | boot-device-discovery | `0 | 1`. |
| `boot_media_present`    | bootx64 | P0 | same | runtime | boot-device-discovery | `0 | 1`. |
| `_rem_pad`              | bootx64 | P0 | none (alignment) | -- | -- | Reserved. |

## Kernel-populated fields

These fields live inside `struct boot_info` but the **bootloader never writes them**. Kernel code fills them after its own subsystems come up; readers must gate on the corresponding producer completing. Including them in `boot_info` keeps diagnostic dumps and the registry populator simple, at the cost of a weaker ABI shape (changing the set of kernel-populated fields does not need a `BOOT_INFO_VERSION` bump as long as layout does not change).

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `secure_boot_enabled` | kernel (`uefi_secureboot_init`) | K (after UEFI runtime init) | registry populator, boot log | runtime | uefi-hardening-secureboot | `0 | 1`. |
| `_kp_pad[3]`          | kernel (zero init) | K | none (alignment) | -- | -- | Reserved. |
| `degraded_mask`       | kernel (`boot_degrade_*`) | K (set incrementally across phases) | health orchestrator, BSOD context | runtime | system-health-recovery-orchestrator | Bitmask; individual bits are `SUBSYS_*` from `kernel_subsystems.h`. |
| `hv_flags`            | kernel (CPUID hypervisor leaf + per-vendor probe) | K (after CPUID init in Phase 0) | platform detection, MSR trap workarounds | runtime | kernel-init-sequencing | `HV_FLAG_*` bits. |
| `hv_vendor[16]`       | kernel (CPUID 0x40000000) | K | platform detection, boot log | runtime | kernel-init-sequencing | Null-terminated ASCII; zero if none. |

### Typed payload descriptor array

Bootloader publishes typed physical payloads that are consumed by kernel subsystems after handoff. The packed-prefix invariant + overlap / range / unknown-required validator lives in [`src/kernel/main/boot_payload.c`](../../src/kernel/main/boot_payload.c) and is wired from [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c) immediately after the boot_info copy.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `payload_descriptors[BOOT_PAYLOAD_MAX]` | bootx64 (cold-boot path) OR outgoing kernel (warm-update path, type=`BOOT_PAYLOAD_WARM_UPDATE_STATE`) | P0 | §5 (module/initrd), §6 (PMM reservation), §13 (TPM log), §20 (USB handover), §22 (recovery), §25 (network), §26 (hibernation), §12 (random seed), §14 (`boot_warm_update_consume` for type-9 entries; gated on `BOOT_CAP_PAYLOAD_DESCRIPTORS` + `BOOT_FLAG_WARM_UPDATE`) | handoff | boot-protocol-abi-handoff §4, §5, §6, §14 | Indices `[0, payload_count)` are the packed prefix; slot N>=payload_count with `length!=0 || type!=NONE` fails validation. Every occupied slot must pass overlap checks against `boot_info` / `rt_mmap[]` / `usb_controller.dma_pages[]` / framebuffer. Type-9 (warm-update) entries with `BOOT_PAYLOAD_FLAG_REQUIRED` may carry `BOOT_WARM_UPDATE_CONT_*` continuation bits at positions 8..13; the §4 validator's REQUIRED+unknown-flags check is type-aware and accepts those bits in addition to `BOOT_PAYLOAD_FLAG_MASK_KNOWN`. |
| `payload_count` | bootx64 | P0 | validator, consumers | handoff | boot-protocol-abi-handoff §4 | `<= BOOT_PAYLOAD_MAX` (32). |
| `payload_overflow` | bootx64 | P0 | boot log | handoff | boot-protocol-abi-handoff §4 | `0 | 1`; set to 1 when the producer had more typed payloads than the array can hold. |
| `payload_total_bytes` | bootx64 | P0 | validator | handoff | boot-protocol-abi-handoff §4 | Must equal the sum of `length` for every occupied slot in the packed prefix; validator rejects a mismatch. |

### Capability negotiation

Tri-word bitmask negotiation between bootloader and kernel. `caps_required` asserts "producer could not have booted without this feature"; `caps_present` advertises "producer populated or supplied this feature"; `caps_degraded` advertises "producer intentionally skipped this feature and substituted a safe fallback". The classify+halt validator [`boot_caps_validate`](../../src/kernel/main/boot_caps.c) is wired from [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c) after `boot_payload_validate` and BEFORE any capability-gated consumer runs. Known-bit mask is `BOOT_CAP_MASK_KNOWN`; bits outside the mask in `caps_required` reject (stale-kernel-on-newer-loader); unknown bits in `caps_present` / `caps_degraded` are tolerated for forward compat.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `caps_required` | bootx64 | P0 | validator (`boot_caps_validate`) | handoff | boot-protocol-abi-handoff §11 | All set bits MUST be in `BOOT_CAP_MASK_KNOWN`; halts with "unknown required bits" otherwise. No bit may be set in both `caps_required` and `caps_degraded`. |
| `caps_present` | bootx64 | P0 | capability-gated consumers (runtime services, TPM log, USB handover, etc.) | handoff | boot-protocol-abi-handoff §11 | Unknown bits tolerated (forward compat). No bit may be set in both `caps_present` and `caps_degraded`. Every bit in `BOOT_CAP_MASK_KNOWN` must appear in `caps_present OR caps_degraded` (no unclassified known bits). |
| `caps_degraded` | bootx64 | P0 | boot log, health orchestrator | handoff | boot-protocol-abi-handoff §11 | Unknown bits tolerated. Must not intersect `caps_required` or `caps_present`. Complements `caps_present` to cover every known bit. |

#### `BOOT_CAP_*` bit catalog (v7)

`BOOT_CAP_MASK_KNOWN` is the OR of every entry below; the validator's "unknown required" check rejects any `caps_required` bit outside this set, and the "every known bit must be classified" check requires each of these to appear in `caps_present` OR `caps_degraded`. The list is the single source of truth -- it drives `BOOT_CAP_MASK_KNOWN` in [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h), the bit-name switch in [`src/kernel/main/boot_caps.c`](../../src/kernel/main/boot_caps.c), the table-driven test in [`src/kernel/test/test_boot_caps.c`](../../src/kernel/test/test_boot_caps.c), and this catalog. Adding a new bit requires updating all four.

| Bit | Name (`BOOT_CAP_*` suffix) | Companion `boot_info` fields | Producer rule |
| --- | --- | --- | --- |
| 0 | `PAYLOAD_DESCRIPTORS` | `payload_descriptors[]`, `payload_count`, `payload_total_bytes` | `present` when array populated + validated; `degraded` if zero typed payloads delivered (legacy single-module fallback). |
| 1 | `RUNTIME_SERVICES` | `uefi_runtime_services`, `uefi_rt_available` | `present` when ExitBootServices preserved RT pointer + variable services callable; `degraded` for non-UEFI adapters or revoked RT post-EBS. |
| 2 | `SECURE_BOOT_STATE` | `secure_boot_enabled` | `degraded` at handoff -- bootloader cannot read the variable pre-RT; kernel reads it after `uefi_runtime_init` and calls `boot_caps_mark_present(BOOT_CAP_SECURE_BOOT_STATE)`. |
| 3 | `TPM_EVENT_LOG` | `tpm_event_log`, `tpm_event_log_size`, `tpm_available`, `tpm_version`, `tpm_event_count` | `present` when EFI_TCG2_PROTOCOL returned a buffer; `degraded` when no TPM detected or `EFI_TCG2_PROTOCOL` unavailable. |
| 4 | `USB_HANDOVER` | `usb_controller.active`, `usb_controller.dma_*`, `usb_devices[]` | `present` when bootloader allocated controller DMA + completed enumeration; `degraded` when no xHCI / EHCI handed off (UEFI-only USB stack remains, but kernel cannot resume from that). |
| 5 | `MEDIA_ROLE` | `boot_device.*`, removable / fixed media flags | `present` when boot media classification + identity captured; `degraded` for adapters that cannot supply the path device. |
| 6 | `NETWORK_PROVENANCE` | reserved for PXE / HTTPboot / iSCSI metadata | always `degraded` today (no network-boot producer); reserved for the future PXE adapter. |
| 7 | `RESUME_METADATA` | reserved for hibernation / S4 resume handoff | always `degraded` today (no resume producer); reserved for the future suspend-to-disk path. |
| 8 | `ALT_PROTOCOL_ADAPTER` | -- (informational) | `present` iff the producer is a non-native-UEFI adapter (Multiboot2 GRUB, etc.); native UEFI bootloader reports it `degraded`. Used by validator to log "adapter=alternate" vs "adapter=native-UEFI" on degraded summaries. |

### Boot-path provenance

One shared decision record answering "what path did this boot take and why". Consumers (HKLM\SYSTEM\Boot\Decision populator, BlackBox transcription, recovery orchestrator, attestation narrative) read these four fields BEFORE inspecting per-path descriptors (recovery image, resume metadata, network provenance) so they know which descriptor to trust. Validator: [`boot_decision_validate`](../../src/kernel/main/boot_decision.c) wired from `boot_hw.c` after `boot_caps_validate`. Bumping any enum value or flag bit requires a `BOOT_INFO_VERSION` bump.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `boot_path` | bootx64 | P0 | Registry (`SYSTEM\\Boot\\Decision\\Path`), BlackBox transcription | handoff | boot-protocol-abi-handoff section 12 | `enum boot_path_type` in `[0, BOOT_PATH_TYPE_MAX]`; out-of-range halts with "stale loader emitted unknown flow". |
| `boot_reason` | bootx64 | P0 | Registry (`Reason`), recovery orchestrator, attestation | handoff | boot-protocol-abi-handoff section 12 | `enum boot_reason_code` in `[0, BOOT_REASON_CODE_MAX]`; out-of-range halts with "unknown code". |
| `boot_source_flags` | bootx64 | P0 | Registry (`SourceFlags`), BlackBox | handoff | boot-protocol-abi-handoff section 12 | Every set bit MUST be in `BOOT_SOURCE_FLAG_MASK_KNOWN`; unknown bits halt with "unknown flag" (unlike capability negotiation, forward-compat tolerance does NOT apply -- every flag maps to a kernel-side policy). |
| `boot_fallback_depth` | bootx64 | P0 | Registry (`FallbackDepth`), BlackBox | handoff | boot-protocol-abi-handoff section 12 | `<= BOOT_FALLBACK_DEPTH_MAX` (16); greater halts with "probable fallback loop". |

#### `BOOT_PATH_*` catalog (v12)

`enum boot_path_type` -- which boot flow ran. Value 0 is the UNSET sentinel (BSS-zero from a producer that never populated the field; validator rejects). The single source of truth lives in [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h); this catalog must stay in sync.

| Value | Name | Description |
| ----- | ---- | ----------- |
| 0 | `BOOT_PATH_UNSET` | sentinel (rejected by validator) |
| 1 | `BOOT_PATH_NORMAL` | normal cold boot from primary boot device |
| 2 | `BOOT_PATH_INSTALLER` | installer image handoff |
| 3 | `BOOT_PATH_RECOVERY` | recovery partition / Windows RE equivalent |
| 4 | `BOOT_PATH_NETWORK` | PXE / HTTP boot |
| 5 | `BOOT_PATH_RESUME` | S4 hibernation resume (validated) |
| 6 | `BOOT_PATH_FAST_STARTUP` | Windows-style fast startup (hybrid boot) |
| 7 | `BOOT_PATH_DIAGNOSTIC` | operator-triggered diagnostic mode |

#### `BOOT_REASON_*` catalog (v12)

`enum boot_reason_code` -- the policy reason for the selected path. Value 0 is the UNSET sentinel. Each reason has a row in the validator's per-reason policy table (`reason_policy[]` in [`src/kernel/main/boot_decision.c`](../../src/kernel/main/boot_decision.c)) that pins required path, forbidden paths, fallback-class membership, and the trigger flag the reason demands. Adding a reason without updating the policy table is a compile-time error (`_Static_assert` on table length).

| Value | Name | Required path | Fallback class | Trigger flag |
| ----- | ---- | ------------- | -------------- | ------------ |
| 0 | `BOOT_REASON_UNSET` | -- | -- | -- (rejected) |
| 1 | `BOOT_REASON_NORMAL` | any | no | -- |
| 2 | `BOOT_REASON_USER_SELECTED` | any | no | -- |
| 3 | `BOOT_REASON_ROLLBACK` | any | yes | `ROLLBACK_TRIGGERED` |
| 4 | `BOOT_REASON_RESUME_VALIDATED` | `RESUME` | no | -- |
| 5 | `BOOT_REASON_RESUME_INVALIDATED` | any (forbids `RESUME`) | yes | `RESUME_INVALIDATED` |
| 6 | `BOOT_REASON_NETWORK_INSECURE` | `NETWORK` | no | `NETWORK_INSECURE` |
| 7 | `BOOT_REASON_MANIFEST_FAILURE` | any (forbids `INSTALLER`) | yes | `MANIFEST_FAILED` |
| 8 | `BOOT_REASON_MEASURED_BOOT_FAIL` | any | yes | `MEASURED_BOOT_FAILED` |
| 9 | `BOOT_REASON_RECOVERY_TRIGGER` | `RECOVERY` | no | `RECOVERY_TRIGGERED` |
| 10 | `BOOT_REASON_FAST_STARTUP_HIT` | `FAST_STARTUP` | no | -- |
| 11 | `BOOT_REASON_DIAGNOSTIC_REQUEST` | `DIAGNOSTIC` | no | -- |
| 12 | `BOOT_REASON_FALLBACK` | any | yes | -- |

#### `BOOT_SOURCE_FLAG_*` catalog (v12)

Closed bitmask of inputs that drove the decision. `BOOT_SOURCE_FLAG_MASK_KNOWN` is the OR of every entry below; unknown bits halt the validator (no forward-compat tolerance). Trigger flags participate in Rule 7a (reason demands flag) and Rule 7b (flag demands matching reason); informational flags carry no reason constraint.

| Bit | Name | Class | Producer rule |
| --- | ---- | ----- | ------------- |
| 0 | `BOOT_NEXT_SET` | informational | `BootNext` UEFI variable was populated |
| 1 | `BOOT_CURRENT_MISMATCH` | informational | `BootCurrent` != expected primary entry |
| 2 | `MEDIA_REMOVABLE` | informational | boot media flagged removable by `EFI_BLOCK_IO_PROTOCOL` |
| 3 | `MEDIA_PRESENT` | informational | boot media actually present (BlockIO confirmed) |
| 4 | `ROLLBACK_TRIGGERED` | trigger | rollback counter asserted; reason MUST be `ROLLBACK` |
| 5 | `RECOVERY_TRIGGERED` | trigger | recovery trigger asserted; reason MUST be `RECOVERY_TRIGGER` |
| 6 | `RESUME_INVALIDATED` | trigger | hibernation image invalidated; reason MUST be `RESUME_INVALIDATED` |
| 7 | `NETWORK_INSECURE` | trigger | network boot with insecure-channel flag; reason MUST be `NETWORK_INSECURE` |
| 8 | `MANIFEST_FAILED` | trigger | image manifest check failed; reason MUST be `MANIFEST_FAILURE` |
| 9 | `MEASURED_BOOT_FAILED` | trigger | measured-boot/TPM check failed; reason MUST be `MEASURED_BOOT_FAIL` |

### Anti-rollback and security-version binding

Monotonic anti-rollback counter bound to UEFI NVRAM variable `IPOSRequiredSecVersion`. The bootloader reads it early and refuses to jump when the kernel's `os_loader_security_version` is below the stored `required_security_version`. The kernel ships with a build-time signed value; after a successful Phase 3 proof-of-life (POST16_BOOT_OK), an opt-in policy may raise the NVRAM counter via UEFI RT `SetVariable` so future boots refuse older-signed kernels. Write timing is load-bearing: a pre-jump update would brick the system on a crash before POST16_BOOT_OK. Validator: [`boot_rollback_validate`](../../src/kernel/main/boot_rollback.c) wired from `boot_hw.c` after `boot_decision_validate`.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `flags` | bootx64 | P0 | validator (`boot_rollback_validate`), `boot_warm_update_consume`, UKI consumers, crash-recorder | handoff | [boot-protocol section 13 / 14](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#13-anti-rollback-and-security-version-binding) + [UKI signed boot](../../todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md#11-unified-signed-boot-artifact-uki-style) | Every set bit MUST be in `BOOT_FLAG_MASK_KNOWN` = `BOOT_FLAG_ROLLBACK_REFUSAL` (1u<<0, downgrade halt) + `BOOT_FLAG_ROLLBACK_READ_FAILED` (1u<<1, NVRAM read/validation halt) + `BOOT_FLAG_WARM_UPDATE` (1u<<2, warm-kernel-update handoff) + `BOOT_FLAG_INVOKED_VIA_UKI` (1u<<3, set by `bootx64.c` when the UKI fast path fires). Unknown bits halt. Closed mask -- no forward-compat tolerance. |
| `os_loader_security_version` | kernel build (Makefile-baked constant read by bootloader) | P0 | validator, Registry export, Phase 3 NVRAM raise | handoff | boot-protocol-abi-handoff section 13 | `<= BOOT_SECURITY_VERSION_MAX` (0x7FFFFFFF); out-of-range halts. Shipped value baked into `kernel.exe` at build. |
| `required_security_version` | bootx64 (UEFI variable `IPOSRequiredSecVersion`) | P0 | validator, Registry export, Phase 3 raise-decision | handoff | boot-protocol-abi-handoff section 13 | `<= BOOT_SECURITY_VERSION_MAX`. Zero on first-ever boot (variable absent); raised by kernel Phase 3 opt-in. |
| `_rollback_pad` | bootx64 | P0 | reserved | handoff | boot-protocol-abi-handoff section 13 | Must be zero; reserved for future anti-rollback policy word. |

### EFI System Partition integrity

Pre-load sanity gate written by `esp_integrity_check()` in [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c). Catches obvious corruption / wrong-partition cases before the bootloader trusts the disk for kernel.exe + BOOTX64.EFI. Mirrors what Win11 BootMgr surfaces (FAT32 + GPT type GUID) and what Linux exposes via `efibootmgr` UUID. The trust anchor for the legacy split path is the Secure Boot signature on `BOOTX64.EFI` itself; this gate is corruption / wrong-partition detection, not adversary defense. Surfaced post-boot at `HKLM\HARDWARE\BOOT\ESP\{Uuid, SizeMB, FilesystemType, TypeGuidValid}` via `boot_device_populate_registry` in [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c).

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `esp_size_mb` | bootx64 (`esp_integrity_check` from BlockIO->Media on partition handle) | P0 | Registry (`HKLM\HARDWARE\BOOT\ESP\SizeMB`) | handoff | uefi-hardening-secureboot ESP integrity | `0` when BlockIO unavailable; otherwise partition `(LastBlock+1)*BlockSize` in MiB. UKI invocation populates this from BlockIO before skipping identity validation. |
| `esp_filesystem_type` | bootx64 (BS_FilSysType BPB read at LBA 0) | P0 | Registry (`FilesystemType`) | handoff | uefi-hardening-secureboot ESP integrity | `0`=unknown / not checked, `1`=FAT16 (only on partitions < 16 MiB; warned), `2`=FAT32. Non-tiny FAT16 or unrecognized BPB halts with `BOOT_ERR_ESP_BPB`. Zero in UKI mode. |
| `esp_type_guid_valid` | bootx64 (parent-disk GPT entry table read) | P0 | Registry (`TypeGuidValid`) | handoff | uefi-hardening-secureboot ESP integrity | `1` only when GPT partition type GUID matched `EFI_PARTITION_TYPE_SYSTEM_PARTITION_GUID`. `0` for UKI mode (skipped), non-GPT boot media, or parent-disk handle unavailable. Mismatch halts with `BOOT_ERR_ESP_TYPE_GUID`. |
| `_esp_pad` | bootx64 | P0 | reserved | handoff | uefi-hardening-secureboot ESP integrity | Must be zero; reserved for future ESP integrity flags (e.g., backup-GPT validity, secondary-LBA agreement). |

### Bootloader build identity

64-byte packed `struct boot_loader_identity` populated by the UEFI bootloader from compile-time constants emitted by [`tools/boot-info-manifest/gen-loader-identity.sh`](../../tools/boot-info-manifest/gen-loader-identity.sh) into `build/boot_loader_identity.h`. Surfaces the producer's git commit + commit time + label so triage tools (kernel fatal screen, BlackBox transcript, NVRAM fault record, `HKLM\SYSTEM\Boot\Decision\Loader{GitSha,BuildTime,BuildLabel}`) can attribute drift / rollback / stale-loader faults to a specific bootloader image. Zero `git_sha[]` is the "loader did not populate" sentinel for back-compat with stale bootloaders against fresh kernels.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `loader_identity` | bootx64 (compile-time constants from `build/boot_loader_identity.h`) | P0 | `HKLM\SYSTEM\Boot\Decision\Loader*` registry, `boot_version_render_fatal`, `boot_version_blackbox_transcribe` | handoff | boot-protocol-abi-handoff Bootloader Build Identity | 64 bytes packed; populated immediately before `header.magic` write so a stale-bootloader detection on this field is race-free. All-zero `git_sha[]` means bootloader did not populate (legacy bootloader against fresh kernel). |
| `loader_identity.git_sha` | bootx64 | P0 | fatal screen + transcript + registry | handoff | boot-protocol-abi-handoff Bootloader Build Identity | 20 raw bytes from `git rev-parse HEAD`; cannot be all-zero on a live build (zero is the populate sentinel). |
| `loader_identity.build_unix_time` | bootx64 | P0 | fatal screen + transcript + registry | handoff | boot-protocol-abi-handoff Bootloader Build Identity | `git log -1 --format=%ct` (commit unix time, little-endian). 0 on no-git fallback. |
| `loader_identity.build_label` | bootx64 | P0 | fatal screen + transcript + registry | handoff | boot-protocol-abi-handoff Bootloader Build Identity | `git describe --dirty --always --tags` truncated to 23 bytes + NUL. 24-byte char array, NUL-terminated. |
| `loader_identity._pad` | bootx64 | P0 | reserved | handoff | boot-protocol-abi-handoff Bootloader Build Identity | Must be zero; reserved for future identity fields (signing-cert SKID, build-host hash, etc.). |

## UKI signed-payload addresses (v14)

Six `uint64` fields appended after `loader_identity`. Populated by `detect_uki_sections()` + `uki_copy_payloads_to_loader_data()` in `bootx64.c` after walking the LoadedImage PE section table for `.initrd` / `.recovery` / `.modules`. Each payload is copied out of LoadedImage memory into AllocatePages-allocated `EfiLoaderData` pages so the kernel-physical address survives ExitBootServices. Zero `addr` means the section was absent in the UKI (back-compat with UKI builds shipping no payload sections). Set ONLY when `boot_info.flags & BOOT_FLAG_INVOKED_VIA_UKI`; ignored on the legacy split path. Provides whole-chain Secure Boot signature coverage of load-bearing payloads.

| Field | Producer | Phase | Consumer | Lifetime | Owning TODO | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `uki_initrd_addr` | bootx64 (`uki_copy_payload .initrd`) | P0 | kernel `initrd` consumer (when wired) | handoff | uefi-hardening-secureboot UKI signed payloads | kernel-physical pointer to `EfiLoaderData` copy of the `.initrd` PE section, or 0 if absent. |
| `uki_initrd_size` | bootx64 | P0 | kernel `initrd` consumer | handoff | uefi-hardening-secureboot UKI signed payloads | original PE section `VirtualSize` in bytes; 0 if absent. |
| `uki_recovery_addr` | bootx64 (`uki_copy_payload .recovery`) | P0 | recovery image loader (when wired) | handoff | uefi-hardening-secureboot UKI signed payloads | kernel-physical pointer to `EfiLoaderData` copy of the `.recovery` PE section, or 0 if absent. |
| `uki_recovery_size` | bootx64 | P0 | recovery image loader | handoff | uefi-hardening-secureboot UKI signed payloads | size in bytes; 0 if absent. |
| `uki_modules_addr` | bootx64 (`uki_copy_payload .modules`) | P0 | kernel module loader (when wired) | handoff | uefi-hardening-secureboot UKI signed payloads | kernel-physical pointer to `EfiLoaderData` copy of the `.modules` cpio archive, or 0 if absent. |
| `uki_modules_size` | bootx64 | P0 | kernel module loader | handoff | uefi-hardening-secureboot UKI signed payloads | size in bytes; 0 if absent. |

## Extended UEFI boot-variable capability surface (v15)

Four read-only firmware capability fields appended after the v14 UKI cluster. Populated by the UEFI bootloader pre-EBS from `BootCurrent`'s Boot####.Attributes (UEFI 2.10 spec 3.1.3), the `BootOptionSupport` and `OsIndicationsSupported` globals (UEFI 2.10 spec 3.1.4 + 8.5.4), and the Boot#### Description string. Persisted in the registry under `HKLM\SYSTEM\Boot\Device\` for diagnostics / `BCDEdit`-equivalent surfacing. Read-only; `OsIndicationsSupported` is the firmware capability variable, distinct from the `OsIndications` write-path capsule trigger banned by the UEFI hardening TODO.

| Field | Producer | Phase | Consumer | Lifetime | Owning TODO | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `boot_current_attrs` | bootx64 (extended boot vars) | P0 | `boot_device_populate_registry()` | handoff | boot-device-discovery extended-boot-vars | EFI_LOAD_OPTION.Attributes for the firmware-selected Boot#### entry; 0 if BootCurrent absent. |
| `boot_option_support` | bootx64 | P0 | `boot_device_populate_registry()` | handoff | boot-device-discovery extended-boot-vars | BootOptionSupport global capability bits (KEY/APP/SYSPREP/COUNT); 0 if absent. |
| `os_indications_supported` | bootx64 | P0 | `boot_device_populate_registry()` | handoff | boot-device-discovery extended-boot-vars | OsIndicationsSupported global capability bitmap; 0 if absent. Read-only -- distinct from the OsIndications write-path. |
| `boot_description` | bootx64 | P0 | `boot_device_populate_registry()` | handoff | boot-device-discovery extended-boot-vars | Boot#### Description ASCII-truncated to 63 chars + NUL terminator; empty if BootCurrent absent. |

## Local boot device path detail (v16)

Per-bus identifiers extracted from the boot device's UEFI device path: NVMe NamespaceId + NamespaceUuid (EUI-64) per UEFI 2.10 spec 10.3.4.21, leaf PCI Device + Function per UEFI 2.10 spec 10.3.2.1. Win11 surfaces these via `MSFT_Disk.UniqueId` + `BusType`; Linux exposes them via `/sys/class/nvme/nvmeX/nsid` + sysfs PCI BDF. Persisted in `HKLM\SYSTEM\Boot\Device\` as `NamespaceId`, `NamespaceEui64`, `PciDevice`, `PciFunction` for diagnostics + storage-stack provenance. Sentinels: `boot_nvme_nsid=0` and `boot_nvme_eui64` all-zero when not NVMe; `boot_pci_device=0xFF` and `boot_pci_function=0xFF` when not on PCI.

| Field | Producer | Phase | Consumer | Lifetime | Owning TODO | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `boot_nvme_nsid` | bootx64 (DP node walk) | P0 | `boot_device_populate_registry()` | handoff | boot-device-discovery boot-device-detail | NVMe NamespaceId from Messaging-NVMe DP node (UEFI 2.10 spec 10.3.4.21); 0 if not NVMe. |
| `boot_nvme_eui64` | bootx64 | P0 | `boot_device_populate_registry()` | handoff | boot-device-discovery boot-device-detail | 8-byte NamespaceUuid (EUI-64 byte order); all-zero if not NVMe or firmware reports zero. |
| `boot_pci_device` | bootx64 | P0 | `boot_device_populate_registry()` | handoff | boot-device-discovery boot-device-detail | leaf PCI Device number (0..31); 0xFF if not on PCI. |
| `boot_pci_function` | bootx64 | P0 | `boot_device_populate_registry()` | handoff | boot-device-discovery boot-device-detail | leaf PCI Function number (0..7); 0xFF if not on PCI. |
| `_v16_pad` | bootx64 | P0 | -- | handoff | boot-device-discovery boot-device-detail | 2-byte alignment padding after `boot_pci_function`. |

## Local boot device path detail (v17)

Media role detected from `/IPOS/role.txt` on the ESP and on the BlackBox service partition before kernel load (boot-media role-detection feature, owning TODO `01-boot-platform/TODO-06`). The bootloader reads role.txt from both partitions on the same physical disk as the booted ESP and cross-checks the contents; mismatch falls back to `BOOT_MEDIA_ROLE_NORMAL` and sets `boot_media_role_mismatch=1`. Absent BlackBox marker is NOT a mismatch (single-partition media is legal). When the role is `INSTALLER`, `RECOVERY`, or `DIAGNOSTICS`, the bootloader also sets `boot_path` to the matching enum and `boot_reason = BOOT_REASON_MEDIA_ROLE_MARKER` so Registry / recovery / attestation consumers see a coherent decision record.

**Source precedence (3-source contract):** The bootloader resolves `boot_media_role` from at most three sources in order. The first source that returns a non-UNSET value wins; later sources are not consulted.

1. **UKI `.cmdline` token** -- when `boot_info.flags & BOOT_FLAG_INVOKED_VIA_UKI`, parse the `media_role=NAME` token from the signed `.cmdline` PE section via `uki_cmdline_extract_media_role()`. UKI cmdline is covered by the firmware-Secure-Boot whole-chain signature, so this is the most authoritative source. A UKI legitimately staged on top of an existing on-disk role marker (recovery UKI on a normal install) is the EXPECTED case; UKI override is CLEAN, no `boot_media_role_mismatch` flag is set.
2. **DHCP option** (network boot, planned) -- PXE/HTTP-served kernel will read a vendor DHCP option carrying the role. Currently blocked on `01-boot-platform/TODO-25` (network boot infrastructure).
3. **ESP `/IPOS/role.txt`** -- the disk-side fallback documented above. Cross-checked against `BlackBox/role.txt` on the same disk.

Default `BOOT_MEDIA_ROLE_NORMAL` when all three sources report UNSET.

| Field | Producer | Phase | Consumer | Lifetime | Owning TODO | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `boot_media_role` | bootx64 | P0 | kernel media-role consumer | handoff | boot-media role-detection | `enum boot_media_role` (UNSET=0, normal=1, installer=2, live=3, recovery=4, manufacturing=5, diagnostics=6). UNSET is the producer-must-overwrite sentinel: the validator (`boot_decision_validate` Rule 3.5) hard-rejects UNSET on every boot, matching the existing Rule 1+2 BSS-zero doctrine for `boot_path` / `boot_reason`. Absent / unrecognized markers are published as `BOOT_MEDIA_ROLE_NORMAL` by the bootloader (and cross-check disagreement is tracked separately via `boot_media_role_mismatch`); UNSET reaching the kernel means an alternate firmware adapter or partial v17 wiring left BSS zero. |
| `boot_media_role_mismatch` | bootx64 | P0 | kernel media-role consumer | handoff | boot-media role-detection | 1 = ESP and BlackBox markers disagreed; loader fell back to `NORMAL`. 0 = agreed or BlackBox absent. |
| `_v17_pad` | bootx64 | P0 | -- | handoff | boot-media role-detection | 3-byte alignment padding after `boot_media_role_mismatch`. |

## Firmware trust landscape (v18)

Trust signals read by the kernel from UEFI runtime services after `ExitBootServices`. Populated by `uefi_secureboot_init()` in [`src/kernel/uefi_runtime.c`](../../src/kernel/uefi_runtime.c) alongside the existing `secure_boot_enabled` / setup-mode reads. v18 is the partial-ship subset of the artifact-signing / manifest-verification feature (owning TODO `01-boot-platform/TODO-06`); the bootloader-side manifest signature path is blocked on crypto vendor work tracked in `09-desktop-shell/TODO-07-cng-crypto.md` (primitives) and `15-installer-release/TODO-01-release-artifacts.md` (release-side signing). The boot_decision degraded-trust rule emits one `[WARN]` klog line per set bit (advisory, non-fatal); `boot_device_populate_registry` surfaces these to `HKLM\SYSTEM\Boot\Trust\*`.

| Field | Producer | Phase | Consumer | Lifetime | Owning TODO | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `sbat_level_size` | kernel `uefi_secureboot_init` | P2 | boot_decision, registry populator | persistent | trust-landscape surface | firmware-reported byte count of the `SbatLevel` UEFI variable (Microsoft Secure Boot Advanced Targeting). 0 = absent / unreadable / empty / oversized (>1 KiB pathological cap); 1..63 = full payload fits in `sbat_level[]`; >=64 = payload truncated to first 63 bytes (consumers detect truncation by comparing against `sizeof(sbat_level)-1`). |
| `sbat_level` | kernel `uefi_secureboot_init` | P2 | boot_decision, registry populator | persistent | trust-landscape surface | first up-to-63 bytes of the raw `SbatLevel` payload, always NUL-terminated. Empty string = SBAT absent. To read the full payload size, use `sbat_level_size`. |
| `dbx_size` | kernel `uefi_secureboot_init` | P2 | boot_decision, registry populator | persistent | trust-landscape surface | bytes in the `dbx` variable under `EFI_IMAGE_SECURITY_DATABASE_GUID` (forbidden signature DB). 0 = absent or unreadable. Contents are NOT mirrored into `boot_info` (dbx can be tens of KiB); only presence and size are surfaced. |
| `degraded_trust_flags` | kernel `uefi_secureboot_init` | P2 | boot_decision, registry populator | persistent | trust-landscape surface | `BOOT_DEGRADED_TRUST_*` bitmask. SECURE_BOOT_OFF / SECURE_BOOT_UNREADABLE / SETUP_MODE / SBAT_ABSENT / DBX_ABSENT. Closed mask: any bit outside `BOOT_DEGRADED_TRUST_MASK_KNOWN` is a producer bug. |

## Boot policy selection (v19)

Result of the boot-entry policy ladder (boot-entry policy feature, owning TODO `01-boot-platform/TODO-07`). The bootloader runs the precedence ladder (hotkey > watchdog > A/B > recovery > store-default > fallback) over the parsed `bootentries.json` envelope set and writes the chosen entry id + reason here, plus the per-entry rejected list. Parallel to `boot_path` / `boot_reason`: those are the path-flow axis (where the kernel loads from); these are the policy-ladder axis (why this specific entry beat others). When the chosen entry implies a path change (recovery, diagnostics), the loader updates `boot_path` / `boot_reason` as well.

| Field | Producer | Phase | Consumer | Lifetime | Owning TODO | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `selected_entry_id` | bootx64 boot policy | P0 | kernel registry (`HKLM\SYSTEM\Boot\Selection\*`), policy-audit, loader-variables | persistent | boot-entry policy | NUL-terminated kebab-case id of the entry chosen by `boot_policy_decide()`. Empty string when the parser rejected the store outright (`reason = FALLBACK_STORE_INVALID`) AND the synthesized fallback envelope is in use. Length capped by `BOOT_ENTRIES_MAX_ID_LEN` (47) + NUL + slack. |
| `selection_reason` | bootx64 boot policy | P0 | kernel registry, policy-audit | persistent | boot-entry policy | `enum boot_selection_reason` (see [`include/boot/boot_policy.h`](../../include/boot/boot_policy.h)). One of `STORE_DEFAULT`, `BOOTNEXT_HINT`, `HOTKEY`, `WATCHDOG_ROLLBACK`, `AB_TRY_STATE`, `RECOVERY_REQUEST`, `FALLBACK_NO_VIABLE`, `FALLBACK_STORE_INVALID`, `UNKNOWN_BOOTCURRENT`. Producer must overwrite `BOOT_SELECTION_UNSET` (0). |
| `_selection_pad` | bootx64 boot policy | P0 | kernel | persistent | boot-entry policy | reserved zero; alignment to 8-byte boundary for the `rejected_entries` array. |
| `rejected_entries` | bootx64 boot policy | P0 | kernel registry (`HKLM\SYSTEM\Boot\Rejected\*`), policy-audit | persistent | boot-entry policy | packed-prefix array of (id, reason) pairs for entries the ladder filtered OUT. Each row: `id[64]` (NUL-terminated kebab-case) plus `uint32_t reason` (`enum boot_reject_reason`: `KIND_SKIPPED`, `NOT_ACTIVE`, `HIDDEN`, `MACHINE_ID_MISMATCH`, `TRIES_EXHAUSTED`, `PATH_ESCAPE`, `KIND_UNAVAILABLE`). Parser-level rejects (CRC mismatch, schema version, etc.) fail the whole store; this list is for per-entry filter misses after the store passed parsing. Cap = `BOOT_ENTRIES_MAX_ENTRIES` (64). |
| `rejected_entry_count` | bootx64 boot policy | P0 | kernel | persistent | boot-entry policy | 0..64; packed-prefix length of `rejected_entries`. 0 = no entries filtered out. |
| `rejected_entry_overflow` | bootx64 boot policy | P0 | kernel | persistent | boot-entry policy | 1 if the bootloader observed more than 64 rejects (extras dropped after recording the `[WARN]` line). |

## Policy audit surface (v20)

Surface for the per-decision boot policy audit trail (boot-entry audit feature, owning TODO `01-boot-platform/TODO-07`). The bootloader reads the `ImpossibleOS-BootSticky` UEFI variable (single 256-byte record, exceptional-only -- per-boot history goes to `X:\Boot\history.jsonl`) and surfaces the relevant bits here. The kernel's `boot_audit_publish()` consumes this block plus the v19 selection block to compose one JSONL line per boot, then acks consumed triggers via `uefi_var_set` post-publish. Bootloader is read-only on the sticky var.

| Field | Producer | Phase | Consumer | Lifetime | Owning TODO | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `audit_degraded` | bootx64 boot policy | P0 | kernel `boot_audit_publish()` | persistent | boot-entry audit | 1 if the sticky read failed (missing / wrong attrs / wrong size / bad CRC). Consumers MUST treat `sticky_*` fields as zeros and emit `BOOT_AUDIT_EVENT_AUDIT_DEGRADED` in the JSONL line. |
| `sticky_present` | bootx64 boot policy | P0 | kernel `boot_audit_publish()` | persistent | boot-entry audit | 1 if a valid sticky record was read; 0 means absent or untrusted. |
| `sticky_recovery_trigger` | bootx64 boot policy | P0 | kernel `boot_audit_publish()` | persistent | boot-entry audit | 1 if the sticky carried `recovery_trigger=1`. The bootloader maps this to `boot_policy_inputs.recovery_request` for the ladder; the kernel acks (clears the bit) post-publish. |
| `sticky_watchdog_rollback_request` | bootx64 boot policy | P0 | kernel `boot_audit_publish()` | persistent | boot-entry audit | 1 if the sticky carried `watchdog_rollback_request=1`. Same two-phase ack as `sticky_recovery_trigger`. |
| `sticky_last_outcome` | bootx64 boot policy | P0 | kernel `boot_audit_publish()` | persistent | boot-entry audit | 1 if the previous boot reached mark-good. Carried for diagnostics only; does not feed the ladder. |
| `sticky_audit_degraded_last_boot` | bootx64 boot policy | P0 | kernel `boot_audit_publish()` | persistent | boot-entry audit | 1 if the previous boot's NVRAM/BlackBox path was degraded (chain-of-degradation visibility). |
| `sticky_last_event_code` | bootx64 boot policy | P0 | kernel `boot_audit_publish()` | persistent | boot-entry audit | `enum boot_audit_event_code` from previous boot. See [`include/boot/boot_audit_codes.h`](../../include/boot/boot_audit_codes.h). |
| `sticky_last_boot_seq` | bootx64 boot policy | P0 | kernel `boot_audit_publish()` | persistent | boot-entry audit | Previous-record diagnostic / ack-context field. The JSONL `boot_seq` comes from the BlackBox-side dual-file counter (`X:\Boot\sequence` + `sequence.new`), NOT from this field. `sticky_last_boot_seq` is preserved across boots so consumers can correlate ack acknowledgments with the boot that acked them. |
| `sticky_consumed_trigger_seq` | bootx64 boot policy | P0 | kernel `boot_audit_publish()` | persistent | boot-entry audit | Boot seq of the last successful trigger ack (helps the kernel detect torn ack windows -- if the bootloader observes a trigger AND `sticky_consumed_trigger_seq == sticky_last_boot_seq`, the previous boot acked but this boot saw the trigger anyway, which means the previous boot also saw it but never cleared -- fail-safe). |
| `_audit_pad` | bootx64 boot policy | P0 | none | persistent | boot-entry audit | reserved zero; alignment to 8-byte boundary. |

## OS-visible Loader UEFI variables (v21)

Surface for the systemd-boot Boot Loader Interface compatibility feature (boot-entry loader-vars feature, owning TODO `01-boot-platform/TODO-07`). The bootloader publishes 11 read-only `Loader*` UEFI variables under vendor GUID `4a67b082-0a4c-41cf-b6c7-440b29bb8c4f` after the policy decision and consumes 2 one-shot vars (`LoaderEntryOneShot`, `LoaderConfigTimeoutOneShot`) early in policy invoke. See [`docs/boot/loader-vars.md`](loader-vars.md) for the full variable inventory + feature bitmap.

| Field | Producer | Phase | Consumer | Lifetime | Owning TODO | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `loader_vars_degraded` | bootx64 boot policy | P0 | kernel `boot_audit_publish()` + diagnostics | persistent | boot-entry loader-vars | 1 if any `Loader*` SetVariable call failed (NVRAM quota, firmware refusal). Userland still boots; the kernel surfaces the flag in the boot audit JSONL so operators see degraded publication state. |
| `active_slot` | bootx64 `select_active_slot()` | P0 | kernel `partition_mount_filesystems()` + mark-boot-successful | persistent | A/B dual-slot boot | A/B slot the bootloader selected pre-EBS from the on-disk `ab_boot_metadata` record. 0 = Slot A, 1 = Slot B (encoding mirrors `include/boot/ab_boot_metadata.h`). Default 0 -- single-slot/non-A/B/error paths resolve to Slot A. Kernel mounts EXACTLY this slot's IXFS as C: and refuses to mark a mismatched slot good. |
| `ab_select_reason` | bootx64 `select_active_slot()` | P0 | kernel VPD slot diagnostics | persistent | A/B dual-slot boot | A/B selection outcome as decided pre-EBS: 0=normal, 1=rollback, 2=both-exhausted (matches `enum ab_boot_select_reason`). Snapshot at selection time because the on-disk record mutates during boot. Carved from `_loader_vars_pad`; no version bump. |
| `ab_from_slot` | bootx64 `select_active_slot()` | P0 | kernel VPD slot diagnostics | persistent | A/B dual-slot boot | Slot rolled away from (rollback) or the recorded active slot (both-exhausted); 0 when normal. |
| `ab_slot_tries` | bootx64 `select_active_slot()` | P0 | kernel VPD slot diagnostics | persistent | A/B dual-slot boot | Per-slot tries consumed as selected (`[0]`=A, `[1]`=B; 0..`AB_BOOT_MAX_TRIES`). VPD derives bootable (tries<MAX) vs exhausted from this, NOT from the successful flag. |
| `ab_slot_flags` | bootx64 `select_active_slot()` | P0 | kernel VPD slot diagnostics | persistent | A/B dual-slot boot | bit0 = Slot A successful (verified), bit1 = Slot B successful. Separate from bootable/exhausted. |
| `ab_status_valid` | bootx64 `select_active_slot()` | P0 | kernel VPD slot diagnostics | persistent | A/B dual-slot boot | Producer-valid marker for the A/B snapshot: set to `AB_STATUS_VALID_MAGIC` (0xAB) when this loader publishes the snapshot. The kernel gates detailed slot/rollback rendering on it so an older same-version (v22) loader that left these reserved bytes zero renders "snapshot unavailable" instead of a false healthy zero-state. Consumes the last `_loader_vars_pad` byte; no version bump. |
| `ab_meta_lba` | bootx64 `select_active_slot()` | P0 | kernel A/B metadata write path | persistent | A/B dual-slot boot | First absolute LBA of the reconciled A/B-metadata partition on the boot/parent disk (parent block units; matches `partition_info.start_lba`). 0 when no A/B metadata partition. Lets the kernel write path locate the partition WITHOUT re-deriving GPT state via the weaker kernel `gpt_parse`. v22. |
| `ab_meta_block_count` | bootx64 `select_active_slot()` | P0 | kernel A/B metadata write path | persistent | A/B dual-slot boot | Size of the A/B-metadata partition in parent blocks (0 when absent). v22. |
| `_ab_meta_pad` | bootx64 boot policy | P0 | none | persistent | A/B dual-slot boot | reserved zero; aligns the v22 A/B-metadata range to 8 bytes. |

## Multi-GPU GOP handles (v23)

Per-GOP-handle enumeration appended at the struct tail (TODO-27 sec4). `init_gop()` enumerates every `EFI_GRAPHICS_OUTPUT_PROTOCOL` handle via `LocateHandleBuffer` (iGPU + dGPU, or multiple panels), records one `boot_gop_handle` per handle, and selects the PRIMARY by device-path match against `gST->ConsoleOutHandle` (then `EFI_CONSOLE_OUT_DEVICE_GUID` marker, then largest resolution). The primary drives `boot_info.fb` unchanged; secondaries carry geometry for the kernel multi-head driver (`04-drivers-hardware/TODO-17 sec6` `display_register_head()`). `gop_handle_count==0` ONLY when no GOP handle was exported (headless / no display / enumeration failed); a successful single-GPU boot publishes `gop_handle_count==1` with `gop_handles[0]` primary. A `count==0` consumer falls back to `boot_info.fb` (zeroed on headless) and must NOT treat `count==0` as the single-head case -- that is `count==1`.

| Field | Producer | Phase | Consumer | Lifetime | Owning TODO | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `gop_handles` | bootx64 `gop_enumerate_and_select()` | P0 | kernel multi-head `display_register_head()` | handoff | multi-GPU GOP enumeration | One `boot_gop_handle` per enumerated GOP handle (max `BOOT_GOP_HANDLE_MAX`=4). Exactly one has `is_primary==1`. v23. |
| `gop_handle_count` | bootx64 `gop_enumerate_and_select()` | P0 | kernel multi-head driver | handoff | multi-GPU GOP enumeration | Number of valid `gop_handles[]` entries; 0 on headless / single-`LocateProtocol` boots. v23. |
| `_gop_handle_pad` | bootx64 | P0 | none | handoff | multi-GPU GOP enumeration | reserved zero; aligns the v23 cluster to 8 bytes. |

## Nested struct: `boot_payload_desc`

48-byte ABI-pinned descriptor. Kernel enum `boot_payload_type` maps well-known values (MODULE, INITRD, RECOVERY_IMAGE, HIBERNATION_META, TPM_EVENT_LOG, NETWORK_CONFIG, RANDOM_SEED, USB_HANDOVER); unknown values are SKIPPED for type-specific validation unless `BOOT_PAYLOAD_FLAG_REQUIRED` is set on the descriptor (required-unknown forces a fatal boot failure).

| Field | Size | Semantics | Validation |
| --- | --- | --- | --- |
| `type` | u32 | `enum boot_payload_type` | `NONE` (0) or a known enum value or an unknown value handled by the REQUIRED policy. |
| `flags` | u32 | `BOOT_PAYLOAD_FLAG_*` bitmask | Unknown bits are accepted if `REQUIRED` is clear; `REQUIRED | <unknown bit>` fails validation. |
| `phys_start` | u64 | Physical base of payload | Must not overlap retained regions. |
| `length` | u64 | Size in bytes | `phys_start + length` must not wrap `u64`; `0` marks an empty slot. |
| `alignment` | u64 | Required natural alignment | `0` (no requirement) or a power of 2. |
| `checksum` | u64 | CRC-32C in low 32 bits when `CHECKSUMMED` set | Computed by consumer when they process the payload. |
| `producer_id` | u32 | `enum boot_payload_producer` | `BOOT_PRODUCER_UEFI` or `_KERNEL_TEST` (fixture buffers). `_MULTIBOOT2 = 2` is RESERVED -- alt-boot policy is `unsupported`; the value is kept for ABI stability but no producer emits it. |
| `_reserved` | u32 | Pad to 48 bytes | Zero. |

## Legacy / Multiboot2-only fields (historical)

The repo currently supports ONE boot path: UEFI (`src/boot/uefi/bootx64.c`). The Multiboot2 parser, header asm, 32-bit entry stub, and `grub.cfg` were deleted under the alt-boot `unsupported` policy ([TODO-08](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md) closed; see [`docs/boot/alt-boot.md`](alt-boot.md)).

The fields below remain in `struct boot_info` for ABI stability -- removing them would shift offsets and force a `BOOT_INFO_VERSION` bump. The UEFI bootloader fills them when applicable; no kernel consumer reads them today.

| Field | Status | Replacement owner | Notes |
| --- | --- | --- | --- |
| `mem_lower_kb` | **deprecate** | `mmap[]` (UEFI memory map) | No kernel consumer. UEFI mmap supersedes lower/upper-memory reporting. Removal candidate for a future `BOOT_INFO_VERSION` bump. |
| `mem_upper_kb` | **deprecate** | `mmap[]` | Same reason. |
| `module_start`     | **retain as compatibility alias** | boot-protocol §4 typed payload descriptors | Kept as a compatibility alias until consumers migrate to typed descriptors. |
| `module_end`       | **retain as compatibility alias** | boot-protocol §4 typed payload descriptors | Same. |
| `module_available` | **retain as compatibility alias** | boot-protocol §4 typed payload descriptors | Same. |

These fields no longer reflect any active code path; removing them is gated on a future ABI version bump.

## Nested struct: `boot_mmap_entry`

Per-region entry in `boot_info.mmap[]`. Producer and consumer are the same for every row; the table records per-field validation.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `base_addr`          | bootx64 / mb2 | P0 | PMM ingestion | boot | boot-protocol §6 | Per-entry checked for overlap in PMM. |
| `length`             | bootx64 / mb2 | P0 | same | boot | boot-protocol §6 | `> 0`; no wrap with `base_addr`. |
| `type`               | bootx64 / mb2 | P0 | same | boot | boot-protocol §6 | Simplified: `1` available, `2` reserved, `3` ACPI, `4` NVS, `5` bad. |
| `uefi_memory_type`   | bootx64 | P0 | runtime services map | boot | boot-protocol §6 | One of `UEFI_MMAP_*` (0-14). |
| `attribute`          | bootx64 | P0 | runtime memory walker | boot | kernel-init-sequencing | EFI memory attribute bitmask (`EFI_MEMORY_RUNTIME` etc.). |

## Nested struct: `boot_uefi_guid`

Used wherever a UEFI GUID needs to be carried over (config tables, well-known GUID constants).

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `data1`    | bootx64 | P0 | GUID matcher | runtime | kernel-init-sequencing | None per-field; whole-GUID match against `UEFI_GUID_*`. |
| `data2`    | bootx64 | P0 | same | runtime | kernel-init-sequencing | Same. |
| `data3`    | bootx64 | P0 | same | runtime | kernel-init-sequencing | Same. |
| `data4[8]` | bootx64 | P0 | same | runtime | kernel-init-sequencing | Same. |

## Nested struct: `boot_uefi_config_entry`

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `guid`       | bootx64 | P0 | ACPI / SMBIOS / FPDT / MEM_ATTR / RT_PROPS / CONFORMANCE / DTB lookups | runtime | kernel-init-sequencing | Exact match against `UEFI_GUID_*` constants. |
| `table_addr` | bootx64 | P0 | same | runtime | kernel-init-sequencing | Non-zero when `guid` matches a consumed well-known GUID. |

## Nested struct: `boot_rt_mem_entry`

Region record for `SetVirtualAddressMap`.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `phys_addr` | bootx64 | P0 | SVAM call | runtime | kernel-init-sequencing | Page-aligned. |
| `num_pages` | bootx64 | P0 | SVAM call | runtime | kernel-init-sequencing | `> 0`. |
| `attribute` | bootx64 | P0 | SVAM call | runtime | kernel-init-sequencing | `UEFI_MEMORY_ATTR_RUNTIME` bit set. |
| `type`      | bootx64 | P0 | SVAM call | runtime | kernel-init-sequencing | `UEFI_MMAP_RUNTIME_CODE` or `_DATA`. |
| `reserved`  | bootx64 | P0 | none (alignment) | -- | -- | Zero. |

## Nested struct: `boot_uefi_runtime`

EFI runtime-service function pointers + state flag. Bootloader uses PE/COFF ms_abi; kernel casts at use time to sysv calling convention.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `get_time`                   | bootx64 | P0 | `uefi_runtime.c` time path | runtime | kernel-init-sequencing | Non-NULL when `svam_called == 1`. |
| `set_time`                   | bootx64 | P0 | same | runtime | kernel-init-sequencing | Same. |
| `get_variable`               | bootx64 | P0 | NVRAM access | runtime | kernel-init-sequencing | Same. |
| `set_variable`               | bootx64 | P0 | NVRAM access | runtime | kernel-init-sequencing | Same. |
| `get_next_variable_name`     | bootx64 | P0 | NVRAM walker | runtime | kernel-init-sequencing | Same. |
| `reset_system`               | bootx64 | P0 | `reset_system` wrapper | runtime | kernel-init-sequencing | Same. |
| `update_capsule`             | bootx64 | P0 | firmware update path | runtime | kernel-init-sequencing | Same. |
| `query_capsule_capabilities` | bootx64 | P0 | firmware update path | runtime | kernel-init-sequencing | Same. |
| `query_variable_info`        | bootx64 | P0 | NVRAM quota | runtime | kernel-init-sequencing | Same. |
| `get_wakeup_time`            | bootx64 | P0 | wake path | runtime | kernel-init-sequencing | Same. |
| `set_wakeup_time`            | bootx64 | P0 | wake path | runtime | kernel-init-sequencing | Same. |
| `svam_called`                | bootx64 | P0 | gate for all above pointers | runtime | kernel-init-sequencing | `0 | 1`; `1` only after `SetVirtualAddressMap` returned EFI_SUCCESS. |
| `pad[7]`                     | bootx64 | P0 | none (alignment) | -- | -- | Zero. |

## Nested struct: `boot_framebuffer`

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `addr`         | bootx64 / mb2 | P0 | `framebuffer.c`, `vmm_map_mmio_uc` | runtime | graphics-asset-foundation | Non-zero when `fb_available == 1`; must be mapped WC. |
| `pitch`        | bootx64 / mb2 | P0 | blit paths | runtime | graphics-asset-foundation | `> 0`; may differ from `width * (bpp / 8)`. |
| `width`        | bootx64 / mb2 | P0 | compositor, desktop | runtime | graphics-asset-foundation | `> 0` when `fb_available == 1`. |
| `height`       | bootx64 / mb2 | P0 | compositor, desktop | runtime | graphics-asset-foundation | `> 0`. |
| `bpp`          | bootx64 / mb2 | P0 | blit paths | runtime | graphics-asset-foundation | `32` on GOP. |
| `type`         | bootx64 / mb2 | P0 | mode dispatch | runtime | graphics-asset-foundation | `0` indexed, `1` RGB, `2` EGA text (MB2 legacy). |
| `pixel_format` | bootx64       | P0 | SIMD blit paths | runtime | graphics-asset-foundation | `GOP_PIXEL_RGBX | BGRX | BITMASK`. |
| `pad0`         | bootx64       | P0 | none (alignment) | -- | -- | Zero. |

## Nested struct: `boot_gop_mode`

One row per GOP mode enumerated pre-ExitBootServices.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `width`               | bootx64 | P0 | (future) mode switcher | boot | graphics-asset-foundation | `> 0`. |
| `height`              | bootx64 | P0 | same | boot | graphics-asset-foundation | `> 0`. |
| `pixels_per_scanline` | bootx64 | P0 | same | boot | graphics-asset-foundation | `>= width`. |
| `pixel_format`        | bootx64 | P0 | same | boot | graphics-asset-foundation | `GOP_PIXEL_*`. |
| `pad[3]`              | bootx64 | P0 | none (alignment) | -- | -- | Zero. |

## Nested struct: `boot_gop_handle`

32-byte ABI-pinned record (TODO-27 sec4). One per enumerated GOP handle. `fb_addr`/`fb_size`/`fb_valid` are set ONLY when the handle's framebuffer passed the full primary-publish validation (non-zero base, supported pixel format, `pitch>=width`, no `height*pitch*4` overflow, `FrameBufferSize` covers the surface); a secondary head whose mode was never SetMode-configured records geometry with `fb_addr=0`/`fb_valid=0` so the kernel never maps a stale or non-display MMIO range.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `fb_addr`      | bootx64 | P0 | kernel multi-head driver | boot | multi-GPU GOP enumeration | FrameBufferBase; 0 unless `fb_valid==1`. |
| `fb_size`      | bootx64 | P0 | same | boot | multi-GPU GOP enumeration | FrameBufferSize reported by firmware; 0 if unknown. |
| `width`        | bootx64 | P0 | same | boot | multi-GPU GOP enumeration | `> 0` when present. |
| `height`       | bootx64 | P0 | same | boot | multi-GPU GOP enumeration | `> 0` when present. |
| `pitch`        | bootx64 | P0 | same | boot | multi-GPU GOP enumeration | bytes per scanline = `PixelsPerScanLine * 4`. |
| `pixel_format` | bootx64 | P0 | same | boot | multi-GPU GOP enumeration | `GOP_PIXEL_*`. |
| `is_primary`   | bootx64 | P0 | same | boot | multi-GPU GOP enumeration | 1 = selected primary head (drives `boot_info.fb`). |
| `fb_valid`     | bootx64 | P0 | same | boot | multi-GPU GOP enumeration | 1 = `fb_addr` passed full validation, safe to map. |
| `pad`          | bootx64 | P0 | none (alignment) | -- | -- | Zero. |

## Nested struct: `boot_config`

Mirror struct in [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c). `cmdline` at offset 32, `config_found` at 288, total size 512 -- all three pinned by `_Static_assert` in the header. Each flag below is `0 | 1` unless noted.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `debug`             | bootx64 (parses `boot.conf`) | P0 | boot logging (`klog` debug path) | runtime | boot-entry-store-menu-policy | `0 | 1`. |
| `verbose`           | bootx64 | P0 | text-mode boot / skip splash | runtime | boot-entry-store-menu-policy | `0 | 1`. |
| `serial_debug`      | bootx64 | P0 | serial COM1 debug output | runtime | boot-entry-store-menu-policy | `0 | 1`. |
| `boot_mode`         | bootx64 | P0 | init-sequencing dispatch | runtime | boot-entry-store-menu-policy | `0` normal, `1` safe, `2` recovery. |
| `splash_timeout`    | bootx64 | P0 | splash subsystem | runtime | boot-entry-store-menu-policy | Seconds; `0` = no timeout. |
| `heartbeat`         | bootx64 | P0 | heartbeat subsystem | runtime | kernel-init-sequencing | `0` off, `1` auto, `2` always. |
| `postcode`          | bootx64 | P0 | POST code emission | runtime | kernel-init-sequencing | `0` off, `1` auto, `2` always. |
| `postbars`          | bootx64 | P0 | Visual POST Display | runtime | kernel-init-sequencing | `0` off, `1` on (integrated), `2` diag (full). |
| `test`              | bootx64 | P0 | unit-test runner | runtime | kernel-test-harness | `0 | 1`. |
| `test_suite`        | bootx64 | P0 | test category filter | runtime | kernel-test-harness | Category: `0`=mm, `1`=fs, `2`=sched, `3`=ob, `4`=security, `5`=ipc, `6`=boot, `7`=abi, `8`=storage, `9`=exec, `10`=x86, `11`=desktop; `0xFF` = all (default). Parsed from `boot.conf` `test_suite=<name>` in `src/boot/uefi/bootx64.c`. |
| `test_quiet`        | bootx64 | P0 | test runner output | runtime | kernel-test-harness | `0 | 1`. |
| `diag_delay`        | bootx64 | P0 | boot diagnostics | runtime | bootloader-error-recovery | Seconds to pause on each diag screen; `0` skip. |
| `diag_splash`       | bootx64 | P0 | boot diagnostics | runtime | bootloader-error-recovery | `0 | 1`. |
| `deferred`          | bootx64 | P0 | init-sequencing strategy | runtime | kernel-init-sequencing | `0 | 1`; default `1`. |
| `async_init`        | bootx64 | P0 | parallel subsystem init on APs | runtime | kernel-init-sequencing | `0 | 1`. |
| `crash_test`        | bootx64 | P0 | deliberate BSOD: early Phase 3, or pre-first-composite | boot | bsod-ux-enhancements | `0` off, `1` early Phase 3, `2` inside the compositor loop before the first composite. |
| `ob_handle_trace`   | bootx64 | P0 | Object Manager tracing | runtime | object-manager | `0 | 1`. |
| `config_version`    | bootx64 | P0 | parser metadata | runtime | boot-entry-store-menu-policy | `0` unversioned (legacy), `1+` versioned. |
| `error_screen_test` | bootx64 | P0 | deliberate `boot_fatal` before kernel load | boot | bootloader-error-recovery | `0 | 1`. |
| `compositor`          | bootx64 | P0 | desktop compositor init | runtime | desktop-ui-test-framework | `0` normal display + VSYNC; `1` headless (no `fb_swap`, no VSYNC, test-driven frames via `compositor_step_frames(N)`). Bare-metal boot rejects `1`. |
| `test_monitors_count` | bootx64 | P0 | parsed/stored; no runtime consumer today | runtime | desktop-ui-test-framework | `0` = no assertion (default); `1..3` = expected output count for future multi-monitor matrix. Parser-only today; the display-stack hard-fail path lands with the virtio-gpu multi-output driver + [`desktop-ui-test-framework`](../../todo/00-infrastructure/TODO-05-desktop-ui-test-framework.md) matrix section. |
| `anti_rollback_raise` | bootx64 | P0 | NVRAM `IPOSRequiredSecVersion` advance after `POST16_BOOT_OK` | runtime | secure-boot-anti-rollback | `0 | 1`; default `0` (never advance). `1` allows kernel to raise the monotonic counter to `os_loader_security_version` for shipped release images. |
| `firmware_quirk_disable` | bootx64 | P1 | suppress firmware quirk auto-detection | runtime | firmware-table-platform-inventory | Bitmask of `FW_QUIRK_*` values. Default `0`. Bootloader parses `firmware_quirk_disable=broken_fpdt,bogus_mat` (comma-separated quirk names) into the mask; `firmware_quirks_init()` then ANDs `~mask` against the auto-detected set. |
| `firmware_rng`      | bootx64 | P0 | EFI_RNG_PROTOCOL collection gate | runtime | early-entropy-random-seed | `0 | 1`; default `1` (collect). `firmware_rng=off` in boot.conf skips the firmware RNG entirely -- escape hatch for firmware whose `GetRNG` hangs (no pre-EBS preemption exists to recover a non-returning call). |
| `seed_file`         | bootx64 | P3 | random-seed carryover file lifecycle gate | runtime | early-entropy-random-seed | `0 | 1`; default `1` (read + rotate `X:\Boot\random-seed.bin` in Phase 3 via `seed_file_phase3()`). `seed_file=off` skips the lifecycle entirely -- escape hatch for damaged or read-only BlackBox media. |
| `tpm_enroll`        | bootx64 | P1 | measured-boot baseline enroll opt-in | runtime | tpm-measured-boot-attestation | `0 | 1`; default `0`. `tpm_enroll=1` lets the kernel enroll/rotate the golden baseline this boot, honored ONLY when `boot_mode=recovery` so a normal boot never silently becomes the golden baseline (no first-boot trust-on-first-use). |
| `_reserved[5]`      | bootx64 (zero fill) | P0 | none (room for new flags) | -- | boot-entry-store-menu-policy | Zero; new flags consume bytes here without shifting `cmdline`. |
| `cmdline[256]`      | bootx64 | P0 | kernel command line parser | runtime | boot-entry-store-menu-policy | Null-terminated; offset 32 is pinned ABI. |
| `config_found`      | bootx64 | P0 | fallback gate | runtime | boot-entry-store-menu-policy | `0 | 1`. |
| `tap`               | bootx64 | P0 | user-mode test launcher | runtime | usermode-test-framework | `0 | 1`; emit TAP (`ok`/`not ok`/`1..N`) lines around each binary. |
| `_pad_utest[2]`     | bootx64 (zero fill) | P0 | alignment (`uint16_t` follows) | -- | usermode-test-framework | Zero. |
| `utest_timeout_ms`  | bootx64 | P0 | user-mode test launcher | runtime | usermode-test-framework | Per-binary wall-clock timeout in ms; `0` = default (10 s). |
| `utest_filter[64]`  | bootx64 | P0 | user-mode test launcher | runtime | usermode-test-framework | Null-terminated glob/literal filter; empty = run every `test_*.exe`. |
| `utest_isolation`   | bootx64 | P0 | user-mode test launcher | runtime | usermode-test-framework | `0` disable scratch-dir + Registry wipe + handle-leak check; default `1`. Production runs must stay `1`. |
| `_pad_utest2[3]`    | bootx64 (zero fill) | P0 | alignment | -- | usermode-test-framework | Zero. |
| `xml`               | bootx64 | P0 | user-mode test launcher | runtime | usermode-test-framework | `0 | 1`; emit `[UTEST-XML]` JUnit XML lines. Orthogonal to `tap`/`json`. |
| `json`              | bootx64 | P0 | user-mode test launcher | runtime | usermode-test-framework | `0 | 1`; emit `[UTEST-JSON]` lines (per-binary + summary). |
| `stress_iters`      | bootx64 | P0 | reserved (future env-passing syscall) | runtime | usermode-test-framework | `uint16_t`; parsed and plumbed through `test_usermode_set_stress_iters`, no consumer today. `0` = default. |
| `test_kernel_skip`  | bootx64 | P0 | test-runner skip knob | runtime | usermode-test-framework | `0 | 1`; skip kernel `TEST_CAT_*` suites under `test=1`. Default `0`; set by `run-all-usermode-tests.bat` + per-binary bats. |
| `test_usermode_skip`| bootx64 | P0 | test-runner skip knob | runtime | usermode-test-framework | `0 | 1`; skip user-mode launcher under `test=1`. Default `0`; set by `run-all-kernel-tests.bat`. |
| `_pad_test_skip[2]` | bootx64 (zero fill) | P0 | alignment | -- | usermode-test-framework | Zero. |
| `_pad[142]`         | bootx64 (zero fill) | P0 | none (sector alignment) | -- | boot-entry-store-menu-policy | Zero; pads struct to 512 bytes. |

## Nested struct: `boot_usb_endpoint`

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `address`     | bootx64 | P0 | xHCI driver endpoint setup | handoff | usb-zero-delay-handover | `bEndpointAddress` from descriptor; bit 7 = IN/OUT direction. |
| `attributes`  | bootx64 | P0 | same | handoff | usb-zero-delay-handover | `bmAttributes`; bits 1:0 = transfer type (control/iso/bulk/int). |
| `max_packet`  | bootx64 | P0 | same | handoff | usb-zero-delay-handover | `wMaxPacketSize`. |
| `interval`    | bootx64 | P0 | same | handoff | usb-zero-delay-handover | `bInterval` polling interval. |
| `pad[3]`      | bootx64 | P0 | none (alignment) | -- | -- | Zero. |

## Nested struct: `boot_usb_device`

One row per USB device discovered via `EFI_USB_IO_PROTOCOL` pre-ExitBootServices. Consumer (xHCI driver) skips re-enumeration after taking over the controller.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `active`          | bootx64 | P0 | per-device gate | handoff | usb-zero-delay-handover | `0 | 1`; only `1` rows are valid. |
| `port`            | bootx64 | P0 | xHCI port mapping | handoff | usb-zero-delay-handover | Root hub port number, 1-based. |
| `speed`           | bootx64 | P0 | xHCI speed selection | handoff | usb-zero-delay-handover | `1` FS, `2` LS, `3` HS, `4` SS. |
| `device_class`    | bootx64 | P0 | device class dispatch | handoff | usb-zero-delay-handover | `bDeviceClass` from descriptor. |
| `iface_class`     | bootx64 | P0 | interface class dispatch | handoff | usb-zero-delay-handover | `bInterfaceClass` of primary interface. |
| `iface_subclass`  | bootx64 | P0 | same | handoff | usb-zero-delay-handover | `bInterfaceSubClass`. |
| `iface_protocol`  | bootx64 | P0 | same | handoff | usb-zero-delay-handover | `bInterfaceProtocol`. |
| `num_endpoints`   | bootx64 | P0 | endpoint walker | handoff | usb-zero-delay-handover | `<= BOOT_USB_MAX_ENDPOINTS`. |
| `vendor_id`       | bootx64 | P0 | device identification | handoff | usb-zero-delay-handover | `idVendor`. |
| `product_id`      | bootx64 | P0 | same | handoff | usb-zero-delay-handover | `idProduct`. |
| `is_msc`          | bootx64 | P0 | MSC BOT path | handoff | usb-zero-delay-handover | `0 | 1`; when `1`, `block_size`/`block_count` valid. |
| `is_hid`          | bootx64 | P0 | HID path | handoff | usb-zero-delay-handover | `0 | 1`. |
| `pad0`            | bootx64 | P0 | none (alignment) | -- | -- | Zero. |
| `block_size`      | bootx64 | P0 | MSC BOT path | handoff | usb-zero-delay-handover | Bytes per sector (from `EFI_BLOCK_IO_MEDIA`); valid iff `is_msc == 1`. |
| `block_count`     | bootx64 | P0 | MSC BOT path | handoff | usb-zero-delay-handover | Total sectors (`LastBlock + 1`); valid iff `is_msc == 1`. |
| `endpoints[BOOT_USB_MAX_ENDPOINTS]` | bootx64 | P0 | xHCI setup | handoff | usb-zero-delay-handover | Per `boot_usb_endpoint` table above; first `num_endpoints` entries valid. |

## Nested struct: `boot_usb_controller`

xHCI DMA state allocated by the bootloader in `EfiLoaderData`. Survives ExitBootServices if the kernel calls `pmm_mark_region_used` for each non-zero physical address.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `pci_bus`                    | bootx64 | P0 | xHCI controller match | handoff | usb-zero-delay-handover | Matches PCI enumeration on kernel side. |
| `pci_dev`                    | bootx64 | P0 | same | handoff | usb-zero-delay-handover | Same. |
| `pci_func`                   | bootx64 | P0 | same | handoff | usb-zero-delay-handover | Same. |
| `active`                     | bootx64 | P0 | gate for all other fields | handoff | usb-zero-delay-handover | `0 | 1`; gated by `usb_handover_complete == 1`. |
| `mmio_phys`                  | bootx64 | P0 | xHCI driver MMIO map | runtime | usb-zero-delay-handover | Page-aligned; kernel remaps via `vmm_map_mmio_uc`. |
| `mmio_size`                  | bootx64 | P0 | same | runtime | usb-zero-delay-handover | `> 0`. |
| `cap_length`                 | bootx64 | P0 | xHCI capability cache | runtime | usb-zero-delay-handover | Matches CAPLENGTH register. |
| `hci_version`                | bootx64 | P0 | xHCI version dispatch | runtime | usb-zero-delay-handover | BCD (e.g., `0x0100`). |
| `max_slots`                  | bootx64 | P0 | slot allocator | runtime | usb-zero-delay-handover | `>= 1`. |
| `max_intrs`                  | bootx64 | P0 | interrupter allocator | runtime | usb-zero-delay-handover | `>= 1`. |
| `max_ports`                  | bootx64 | P0 | port walker | runtime | usb-zero-delay-handover | `>= 1`. |
| `db_offset`                  | bootx64 | P0 | doorbell register access | runtime | usb-zero-delay-handover | Offset into MMIO. |
| `rts_offset`                 | bootx64 | P0 | runtime register access | runtime | usb-zero-delay-handover | Offset into MMIO. |
| `ac64`                       | bootx64 | P0 | xHCI addressing mode | runtime | usb-zero-delay-handover | `0 | 1`; 1 = 64-bit DMA addressing. |
| `csz`                        | bootx64 | P0 | xHCI context struct size | runtime | usb-zero-delay-handover | `0` = 32B context, `1` = 64B context. |
| `max_scratchpads`            | bootx64 | P0 | scratchpad allocator | runtime | usb-zero-delay-handover | Raw `HCSParams2.Max Scratchpad Buffers` from xHCI capability registers (not clamped against `BOOT_USB_MAX_SCRATCHPADS` today; the constant is not a real ABI bound for this field). Kernel must reserve exactly `scratchpad_page_count` pages via `pmm_mark_region_used`. Clamp + truncation policy owned by the USB zero-delay-handover roadmap (see "USB scratchpad clamp" item). |
| `dcbaa_phys`                 | bootx64 | P0 | xHCI DCBAA register | runtime | usb-zero-delay-handover | 64B-aligned; sized `(max_slots + 1) * 8`. |
| `scratchpad_array_phys`      | bootx64 | P0 | xHCI scratchpad setup | runtime | usb-zero-delay-handover | Non-zero when `max_scratchpads > 0`. |
| `scratchpad_base_phys`       | bootx64 | P0 | same | runtime | usb-zero-delay-handover | Base of contiguous scratchpad pages. |
| `scratchpad_page_count`      | bootx64 | P0 | same | runtime | usb-zero-delay-handover | Equals `max_scratchpads` today (bootloader allocates one page per scratchpad). |
| `scratchpad_pad`             | bootx64 | P0 | none (alignment) | -- | -- | Zero. |
| `cmd_ring_phys`              | bootx64 | P0 | xHCI command ring register | runtime | usb-zero-delay-handover | 64-byte aligned; 4 KiB (256 TRBs * 16B). |
| `evt_ring_phys`              | bootx64 | P0 | xHCI event ring (via ERST) | runtime | usb-zero-delay-handover | 64-byte aligned; 4 KiB. |
| `erst_phys`                  | bootx64 | P0 | xHCI ERST register | runtime | usb-zero-delay-handover | 64-byte aligned; 16B single entry. |
| `dma_pages[BOOT_USB_MAX_DMA_PAGES]` | bootx64 | P0 | PMM reservation pass | handoff | usb-zero-delay-handover | Each non-zero entry `pmm_mark_region_used`'d. |
| `dma_page_count`             | bootx64 | P0 | same | handoff | usb-zero-delay-handover | `<= BOOT_USB_MAX_DMA_PAGES`. |
| `alloc_fail_status`          | bootx64 (debug) | P0 | boot log diagnostics | boot | usb-zero-delay-handover | Low 32 bits of last `AllocatePages` failure `EFI_STATUS`; `0` on success. |
| `alloc_fail_page`            | bootx64 (debug) | P0 | boot log diagnostics | boot | usb-zero-delay-handover | Page index that failed to allocate; `0` on success. |

## Nested struct: `timing`

FPDT-sourced fields come from the firmware performance record (nanoseconds since some firmware-defined epoch). TSC-sourced fields are raw `rdtsc` ticks captured by the bootloader. Consumer is the boot performance dashboard / `X:\Perf\` staging; all TSC timestamps must be monotonically non-decreasing.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `reset_end`               | bootx64 (from FPDT) | P0 | perf dashboard | runtime | kernel-init-sequencing | Non-zero when `fpdt_available == 1`. |
| `os_loader_load_start`    | bootx64 (FPDT) | P0 | same | runtime | kernel-init-sequencing | Same. |
| `os_loader_start_start`   | bootx64 (FPDT) | P0 | same | runtime | kernel-init-sequencing | Same. |
| `exit_bs_entry`           | bootx64 (FPDT) | P0 | same | runtime | kernel-init-sequencing | Same. |
| `exit_bs_exit`            | bootx64 (FPDT) | P0 | same | runtime | kernel-init-sequencing | Same. |
| `fpdt_available`          | bootx64 | P0 | gate for FPDT fields above | runtime | kernel-init-sequencing | `0 | 1`. |
| `bl_entry`                | bootx64 (`rdtsc`) | P0 | perf dashboard | runtime | kernel-init-sequencing | `> 0` if `tsc_freq > 0`. |
| `gop_start`               | bootx64 (`rdtsc`) | P0 | same | runtime | kernel-init-sequencing | Monotonically `>= bl_entry`. |
| `gop_end`                 | bootx64 (`rdtsc`) | P0 | same | runtime | kernel-init-sequencing | `>= gop_start`. |
| `conf_start`              | bootx64 (`rdtsc`) | P0 | same | runtime | kernel-init-sequencing | `>= gop_end`. |
| `conf_end`                | bootx64 (`rdtsc`) | P0 | same | runtime | kernel-init-sequencing | `>= conf_start`. |
| `kernel_load_start`       | bootx64 (`rdtsc`) | P0 | same | runtime | kernel-init-sequencing | `>= conf_end`. |
| `kernel_load_end`         | bootx64 (`rdtsc`) | P0 | same | runtime | kernel-init-sequencing | `>= kernel_load_start`. |
| `splash_start`            | bootx64 (`rdtsc`) | P0 | same | runtime | kernel-init-sequencing | `>= kernel_load_end`. |
| `splash_end`              | bootx64 (`rdtsc`) | P0 | same | runtime | kernel-init-sequencing | `>= splash_start`. |
| `exit_bs`                 | bootx64 (`rdtsc`) | P0 | same | runtime | kernel-init-sequencing | `>= splash_end`. |
| `kernel_jump`             | bootx64 (`rdtsc`) | P0 | same | runtime | kernel-init-sequencing | `>= exit_bs`. |
| `tsc_freq`                | bootx64 | P0 | perf dashboard (convert ticks to seconds) | runtime | kernel-init-sequencing | Hz; `0` means unknown (gates TSC-based fields). |

## Kernel boot stack (v24)

The run the kernel executes on from the handoff onward. The bootloader `AllocatePages` it below 4 GiB pre-`ExitBootServices`, fills the whole run with `BOOT_KSTACK_POISON`, and switches `RSP` to its top in `jump_to_kernel` before calling the kernel entry. Before this existed the kernel ran on the firmware's own `EfiLoaderData` stack, which `pmm_init` frees into the allocator while it is in use. The kernel validates the bounds in `boot_stack_init()` (Phase 0, before `pmm_init`), confirms its own `RSP` lies inside them, reserves the run through `boot_reserved` (`BOOT_RESERVED_BOOT_STACK`), guards the lowest page once `vmm_init` is up, and recovers the high-water mark from the poison.

**It is not a boot-only run.** `task.c:592-608` keeps it as PID 0's permanent kernel stack (`stack_base = 0`, "boot stack, don't free"), and PID 0 goes on to run the compositor loop, which never returns. Size it for that lifetime, not for early boot. Measured peaks (QEMU, 2026-08-28): 9,528 bytes at the end of Phase 1; 38,368 bytes in compositor steady state on a production boot; 99,472 bytes cumulative on a `test=1` boot, where the in-kernel test runner is the deepest path.

| Field | Producer | First valid | Consumer | Lifetime | Owning roadmap | Validation |
| --- | --- | --- | --- | --- | --- | --- |
| `kstack_base`       | bootx64 | P0 | `boot_stack_init` / `boot_reserved` | handoff | bare-metal-hardening | Non-zero, 4 KiB aligned; `base + size <= 4 GiB`; live `RSP` must fall inside. Zero halts the boot. |
| `kstack_size`       | bootx64 | P0 | same | handoff | bare-metal-hardening | 4 KiB multiple, non-zero, `<= BOOT_KSTACK_MAX_SIZE`, and `kstack_size - kstack_guard_size >= BOOT_KSTACK_MIN_USABLE` (128 KiB). The floor is on the USABLE span, not the total: guard bytes are not stack. |
| `kstack_guard_size` | bootx64 | P0 | `boot_stack_install_guard` | handoff | bare-metal-hardening | EXACTLY one 4 KiB page (`BOOT_KSTACK_GUARD_BYTES`). A wider guard is rejected: the installer unmaps `kstack_base`, so a multi-page guard would leave the page the stack actually grows through still mapped. |

## Update protocol

When adding a field:

1. Decide producer + consumer + first-valid phase before touching `boot_info.h`. Update this doc first.
2. Add the field to `struct boot_info` (or the appropriate nested struct) in [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h). Add the matching field (same name, same type, same order) to [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h) in the same commit. Bootloader uses `UINT8/16/32/64` spellings; kernel uses `uint8_t/16/32/64_t`.
3. Add a matching `F(new_field)` line to [`tools/boot-info-manifest/dump-fields.inc`](../../tools/boot-info-manifest/dump-fields.inc). If the new field is an array-of-struct, also add representative `F(new_field[0].leaf)` rows for every element leaf. If it is a scalar array with multi-byte elements, add `F(new_field[0])` as an element-size sentinel. Without this step the manifest compare passes while the struct definitions disagree.
4. Bump `BOOT_INFO_VERSION` in BOTH files unless the field fits into an existing `_reserved`/`_pad` region without shifting any documented offset.
5. Extend the `_Static_assert` offset pins in both headers if the field shifts a mirrored count field.
6. Wire the producer, wire the consumer, add a unit test that reads the field through the real layout (not a synthetic fixture).
7. Add a row here.
8. `bash scripts/build.sh` invokes the `boot_info ABI` stage automatically -- a clean build proves kernel/bootloader manifests agree.

When a field becomes stale:

1. Mark it **deprecate** in the Legacy block above. Include the replacement owner roadmap.
2. Do NOT remove it until the version bump that drops the Multiboot2 path (or the replacement feature fully displaces it), so existing bootloaders keep parsing clean.
3. Prefix removed-candidate fields with `/* DEPRECATED: ... */` in the header when the window opens.
