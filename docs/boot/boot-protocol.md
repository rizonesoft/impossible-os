# Boot Protocol Reference

> Canonical reference for the Impossible OS bootloader to kernel
> handoff ABI. Pairs with:
>
> - [`boot-info-fields.md`](boot-info-fields.md): per-field ownership matrix (which subsystem reads each field, which consumer validates it).
> - [`boot-protocol-changelog.md`](boot-protocol-changelog.md): `BOOT_INFO_VERSION`-keyed history of every schema change with owning TODO section + commit.
> - [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h): C contract (struct layout + version macro + validator prototypes).
> - [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h): bootloader mirror of the struct, kept byte-identical with the kernel header.
>
> This document describes **what the handoff looks like, when each field is valid, and how to populate a minimal supported version**. Field-by-field semantics live in `boot-info-fields.md`.

## 1. Layout at a Glance

```
Physical address 0x10000  ────┐
                              │
        struct boot_info {    │  <-- bootloader writes here before jumping
          struct boot_info_header header;   (offset 0, 8 bytes)
          ... all ABI fields ...            (v23: see boot_info.h sizeof for total)
        }                     │
```

- **Handoff base**: `BOOT_INFO_PHYS_ADDR = 0x10000`.
- **Version gate**: `BOOT_INFO_VERSION = 23` (current; this prose is updated alongside every bump but the authoritative live value lives in [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) and [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h) -- check the changelog for the field-level history).
- **Magic gate**: `BOOT_INFO_MAGIC = 0x49504F53` (`"IPOS"`, little-endian).
- **Size gate**: `sizeof(struct boot_info)` is ABI-pinned via `_Static_assert`. The kernel refuses any handoff whose `header.size` does not equal this exact value.
- **Early map bound**: `BOOT_INFO_EARLY_MAP_END` is the bootloader's 4 GiB identity map. The kernel rejects handoff pointers outside `[0x1000 .. BOOT_INFO_EARLY_MAP_END)`.

The mirror in `src/boot/uefi/boot_info_mirror.h` uses UEFI typedefs (`UINT8`, `UINT64`, etc.) while the kernel header uses freestanding `kernel/types.h` typedefs (`uint8_t`, `uint64_t`). The physical byte layout is identical; the `_Static_assert`s on both sides enforce this, and the [Generated ABI Manifest](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#2-generated-abi-manifest-and-offset-fingerprint) tooling (`tools/boot-info-manifest/`) emits a canonical SHA-256 fingerprint that must match between the two compilation units.

## 2. Lifecycle

```
  UEFI firmware
      │
      ▼
  [BOOTX64.EFI entry]  ─── parse boot.conf, load kernel ELF, populate
                             g_boot_info at 0x10000, write header magic
                             + version + size last
      │
      ▼
  [ExitBootServices]  ─── UEFI Boot Services protocol handles invalidate;
                             RuntimeServices and EfiLoaderData regions
                             survive
      │
      ▼
  [Kernel entry (`boot_phase0` in `src/kernel/main/boot_hw.c`)]
      │       │
      │       ├── boot_info_validate_addr(mbi, size, BOOT_INFO_EARLY_MAP_END)
      │       │       reject NULL, misaligned, out-of-map, wrap
      │       │
      │       ├── boot_version_classify(header, sizeof(struct boot_info), &fault)
      │       │       reject magic/version/size drift with a structured
      │       │       fault record that feeds boot_version_render_fatal
      │       │       (LOG_FATAL on serial + framebuffer). NVRAM
      │       │       persistence is NOT attempted here today because
      │       │       uefi_runtime_init runs later in Phase 0 -- the
      │       │       Phase 3 BlackBox transcription path only emits
      │       │       X:\Diag\boot-proto-fault.txt when a FUTURE caller
      │       │       with uefi_runtime already live has persisted a
      │       │       record (bootloader pre-jump work or section 13
      │       │       anti-rollback).
      │       │
      │       ├── Copy header.size bytes from bootloader's mbi into the
      │       │   kernel's own g_boot_info (single-copy, trusted after validation)
      │       │
      │       ├── boot_payload_validate(&g_boot_info)
      │       │       reject overlapping / malformed payload_descriptors[]
      │       │
      │       ├── (later in phase 0) uefi_runtime_init()
      │       │       SetVirtualAddressMap, wire GetVariable/SetVariable
      │       │
      │       └── pmm_init()
      │               walk mmap, mark free, then
      │               boot_reserved_populate_from_info(&g_boot_info, &err)
      │               boot_reserved_check_payloads_disjoint(...) x4
      │               boot_reserved_apply() (apply reservations to bitmap)
      │
      ▼
  [Phase 1/2/3 hardware bring-up + subsystem init]
      │
      └── late Phase 3 in `src/kernel/main/boot_desktop.c`:
            hw_dump_write_file()              --> X:\Diag\hwdump.txt
            boot_reserved_blackbox_dump()     --> X:\Diag\boot-reserved.json
            boot_version_blackbox_transcribe() --> X:\Diag\boot-proto-fault.txt
                                                     (only if a PRIOR caller
                                                      with uefi_runtime
                                                      already live persisted
                                                      a fault record to
                                                      NVRAM; today that is
                                                      section 13 anti-
                                                      rollback when it
                                                      ships -- the Phase 0
                                                      stale-loader path
                                                      does not produce
                                                      such a record)
```

Every validator above is defined in [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) or [`include/kernel/boot_version.h`](../../include/kernel/boot_version.h). The validators run BEFORE any consumer reads `g_boot_info` fields beyond the header, so downstream code can rely on invariants (no overlap, no range wrap, packed prefix, etc.).

## 3. Pointer Validity

Pointer fields inside `struct boot_info` fall into four classes:

| Class | Examples | Validity window |
|---|---|---|
| **Bootloader-owned transient** | Addresses below 1 MiB, `BootServices`-era handles | Dead at ExitBootServices. `struct boot_info` MUST NOT store any of these. |
| **RuntimeServices-survived** | `uefi_runtime_services`, every `rt_mmap[].phys_addr` | Valid across EBS. The kernel calls `uefi_runtime_init()` then `SetVirtualAddressMap()` to rebind RT pointers to kernel virtual addresses. |
| **EfiLoaderData** | USB DMA pages, TPM event log copy, typed payload descriptors, scratchpad base | Survives EBS but is reclaimable by PMM unless added to the `boot_reserved` table (see the [Handoff Memory Ownership section](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#6-handoff-memory-ownership-and-pmm-reservation-table)). Bootloader sets `BOOT_PAYLOAD_FLAG_RESERVED` on any descriptor whose bytes must not be reclaimed. |
| **Framebuffer MMIO** | `fb.addr` | Survives EBS. Kernel maps as UC via `vmm_map_mmio_uc()` after PMM init; bootloader identity map has it as WB. |

The kernel never trusts a pointer until the matching validator has run. `boot_info_validate_addr()` covers the `struct boot_info` itself; `boot_payload_validate()` covers every payload descriptor range; `boot_reserved_populate_from_info()` covers every retained region (including USB DMA pages, TPM log, framebuffer, runtime memory regions, and payload descriptors with `FLAG_RESERVED`).

## 4. Minimum Supported Handoff

A third-party bootloader that wants to invoke Impossible OS must populate the fields below at `0x10000` BEFORE jumping to the kernel ELF entry point. Anything not in these tables can be left zero (the kernel's validators accept zero as "absent" and reject only structurally-invalid values), with one important exception below: the capability classification in `caps_present` / `caps_degraded` is a Phase 0 hard requirement for v7+ handoffs because `boot_caps_validate` halts on any known capability bit left unclassified.

### Required (kernel halts via Phase 0 validator if wrong)

| Field | Type | Requirement |
|---|---|---|
| `header.magic` | `uint32_t` | Must equal `BOOT_INFO_MAGIC = 0x49504F53`. |
| `header.version` | `uint16_t` | Must equal the `BOOT_INFO_VERSION` the kernel was built against. |
| `header.size` | `uint16_t` | Must equal `sizeof(struct boot_info)` for that version. |
| `caps_present`, `caps_degraded` (v7+) | `uint64_t` each | Every bit in `BOOT_CAP_MASK_KNOWN` MUST be classified as present (the loader populated the underlying field) OR degraded (the loader could not populate it and the kernel must use a fallback). A bit set in NEITHER halts via `boot_caps_validate`. A bit set in BOTH also halts. `caps_required` may be zero; if non-zero, every required bit must be in `caps_present`. The native UEFI loader at `src/boot/uefi/bootx64.c` populates this block explicitly; alternate adapters that leave it zero will halt regardless of the rest of their handoff. |
| `esp_size_mb`, `esp_filesystem_type`, `esp_type_guid_valid` (v11+) | `uint32_t`, `uint8_t`, `uint8_t` | Bootloader's pre-load ESP integrity attestation. The native UEFI loader populates these via `esp_integrity_check()` before `load_kernel()`. The kernel currently logs them but does not halt on zero today; future attestation consumers WILL halt on `esp_type_guid_valid=0`, so adapters SHOULD populate honestly even if the values are best-effort. |

### Required for full UEFI parity (degrades cleanly if absent)

These fields are required by a native UEFI loader that wants every Impossible OS feature (GOP splash, desktop, `GetVariable`/`SetVariable` for POST-codes and anti-rollback NVRAM, etc.). A non-UEFI adapter may zero any of them; the kernel degrades each one separately with a klog warning instead of halting.

| Field | Type | Purpose | Degraded behavior when zeroed |
|---|---|---|---|
| `mmap[]` + `mmap_count` | `struct boot_mmap_entry[]` | Physical memory layout. PMM walks this to find free RAM. | `pmm_init` logs `memory map truncated` warning and falls back to whatever is populated; zero entries means no free RAM is recovered and subsequent allocations fail (effectively degraded, not halted). |
| `fb.addr`, `fb.pitch`, `fb.width`, `fb.height`, `fb.bpp`, `fb.pixel_format` + `fb_available=1` | `struct boot_framebuffer` | Linear GOP framebuffer for the kernel splash and desktop. | `fb_available=0` puts the kernel in headless mode: no splash, no desktop, serial still works. Supported path. |
| `uefi_runtime_services` + `uefi_rt_available=1` | `uintptr_t`/`uint8_t` | EFI RuntimeServices table pointer (must point at a RUNTSERV-signature table). Needed for `GetVariable` / `SetVariable` / `GetTime`. | `uefi_runtime_init()` returns `BOOT_DEGRADED` and logs the limitation. POST-code NVRAM writes, anti-rollback reads, and RT time calls become no-ops; kernel boots without them. |

### Optional (consumed if populated)

| Field | Purpose | If absent |
|---|---|---|
| `rt_mmap[]` + `rt_mmap_count` | UEFI runtime memory regions for SVAM. | Kernel degrades RT services; logs warning. |
| `tpm_event_log` + `tpm_event_log_size` + `tpm_available` | Measured boot event log from `EFI_TCG2_PROTOCOL`. | TPM measurement is unavailable. |
| `usb_controller` + `usb_devices[]` | xHCI handover state (DCBAA, scratchpad, DMA pages). | Kernel re-enumerates USB with higher latency. |
| `payload_descriptors[]` + `payload_count` | Typed payloads (module / initrd / recovery_image; see [Module and initrd Handoff Contract](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#5-module-and-initrd-handoff-contract)). Must pass `boot_payload_validate()`: packed prefix, no overlap, no range wrap. | No modules loaded. |
| `boot_device_*`, `uefi_boot_*` | Boot device identification and BootOrder inspection. | Kernel uses the default drive search path. |
| `timing.*` | FPDT and TSC timestamps for boot timeline diagnostics. | Timeline starts at kernel entry. |
| `config` (boot.conf parsed form) | Runtime configuration knobs: debug, verbose, test_suite, etc. | The kernel has NO fallback for this field today: `boot_phase0` consumes whatever bytes were handed off. A zeroed `config` gives `serial_debug=0`, `splash_timeout=0`, `deferred=0`, `utest_isolation=0`, etc., which differs from the native UEFI loader's `boot_config_defaults()` (applied in-loader BEFORE parsing boot.conf). Third-party loaders SHOULD populate `config` with the same defaults the UEFI loader emits (see `src/boot/uefi/bootx64.c::boot_config_defaults`) unless they deliberately want the zeroed values. |

### Writer contract

The bootloader MUST:

1. Allocate `struct boot_info` at physical address `0x10000` (either a fixed allocation or ensure it lands there via AllocatePages + memmove).
2. `efi_memset(g_boot_info_ptr, 0, sizeof(struct boot_info))` before populating any field. This guarantees every unused slot is zero (a required precondition for `boot_payload_validate()`'s NONE-empty-slot invariant, and for every `*_count` field that is read without a prior store).
3. Populate the fields above in any order. The header (`magic`, `version`, `size`) MUST be written as a block AFTER every validator-relevant field is final. The canonical producer at `src/boot/uefi/bootx64.c` around line 5691 writes the block as `header.magic = BOOT_INFO_MAGIC;` then `header.version = BOOT_INFO_VERSION;` then `header.size = sizeof(struct boot_info);`. Post-header telemetry writes are explicitly allowed: the canonical loader then writes `timing.kernel_jump = boot_rdtsc();` and jumps. The kernel validator (`boot_version_classify`) reads only the header fields; it never consults `timing.*`, so recording jump-timing after the header cannot invalidate the handoff. A third-party loader SHOULD write the header as the last block before any validator-relevant work, but MAY continue to touch telemetry-only fields (`timing.*`) between the header block and `jump_to_kernel`.
4. Call `ExitBootServices()` and then jump to the kernel ELF entry point with the handoff pointer in `rsi` and the magic (`UEFI_BOOT_MAGIC`) in `rdi`, matching the Multiboot2-style ABI the kernel's early assembly expects.

Failure modes: magic drift, version drift, and size drift each halt with a dedicated fault-class line via `boot_version_render_fatal()`. Structural field drift (e.g. rt_mmap entry with wrapping length) halts via the downstream validator with its own class-specific klog. See [boot-protocol-changelog.md](boot-protocol-changelog.md) for which version introduced each field.

## 5. Example: Native UEFI path

The canonical bootloader is [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c). Follow the flow from `efi_main` through to `jump_to_kernel` for a concrete reference implementation. The mirror struct at [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h) is the bootloader-side view; it is regenerated by hand when the kernel header gains a field (the [ABI manifest and mirror drift checker](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#3-full-bootloaderkernel-mirror-drift-checker) tooling catches any drift before build completes).

## 6. Compatibility Matrix for Alternate Adapters

| Adapter | Required payloads | Required capability flags | Required provenance fields | Supported |
|---|---|---|---|---|
| **Native UEFI (`BOOTX64.EFI`)** | All of `mmap`, `fb`, `uefi_runtime_services` | none (all v6+ features) | `boot_device_*`, `uefi_boot_*`, `timing.*`, `config` | ✅ Canonical |
| **Multiboot2 / GRUB legacy path** | (parser deleted) | (parser deleted) | (parser deleted) | ❌ Retired -- policy = `unsupported` per [`docs/boot/alt-boot.md`](alt-boot.md). Parser source (`src/kernel/multiboot2_parse.c`), header (`include/kernel/multiboot2.h`), 32-bit entry stub (`src/boot/entry.asm`), Multiboot2 header asm (`src/boot/multiboot2_header.asm`), and `src/boot/grub.cfg` were deleted under the [Alternate Boot Protocols TODO-08](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md) deprecation gate. Re-opening requires restoring files from git history. |
| **PXE / HTTP boot** | `payload_descriptors[]` with `BOOT_PAYLOAD_NETWORK_CONFIG` | reserved for [Network PXE/HTTP boot](../../todo/01-boot-platform/TODO-25-network-pxe-http-boot.md) | [Network provenance section](../../todo/01-boot-platform/TODO-25-network-pxe-http-boot.md#6-boot_info-network-provenance) | ⬜ Planned; alternate adapters should target the [Alternate Boot Protocols](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md) TODO for this path. |
| **Secure Launch (TrenchBoot / TXT / SKINIT)** | `payload_descriptors[]` with `BOOT_PAYLOAD_TPM_EVENT_LOG` | reserved for `drtm_entry_pcr` etc. (see [Attestation Report Export](../../todo/01-boot-platform/TODO-13-tpm-measured-boot-attestation.md#9-attestation-report-export)) | DRTM measurement metadata | ⬜ Planned; forward-compat fields are reserved in the attestation report. |
| **Warm kernel update (KHO-style)** | `payload_descriptors[]` preserving in-memory state | [Warm kernel update handoff ABI](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#14-warm-kernel-update-handoff-abi) slot | KHO state region | ⬜ ABI reserved. |

Every adapter column above references the owning TODO by capability name and anchor link rather than numeric section reference. The matrix is deliberately short. Failure semantics differ by layer:

- **Header drift** (magic / version / size mismatch): the kernel halts via `boot_version_render_fatal` with a full observed-vs-expected diagnostic. No degraded path.
- **Payload drift** (overlapping / wrapping / unknown-required payload descriptors): the kernel halts via `boot_payload_validate` with a per-descriptor error class.
- **Optional field absence** (framebuffer, runtime services, USB state, TPM log, payloads): the kernel degrades per subsystem with a klog warning. The Multiboot2 row above is retired -- the parser was deleted under the alt-boot `unsupported` policy.

An adapter that emits a well-formed header + mmap + classified `caps_present`/`caps_degraded` but zeros everything else is a supported degraded boot; the "fail closed" behavior is scoped to the header / capability / payload-integrity validators (header drift, unclassified caps bits, overlapping payloads) and NOT to the full feature surface (framebuffer, runtime services, USB state, TPM log -- each degrades independently with a klog warning when its capability bit is set degraded).

## 7. See Also

- [`boot-info-fields.md`](boot-info-fields.md): per-field matrix (ownership, consumer, validator, version-added).
- [`boot-protocol-changelog.md`](boot-protocol-changelog.md): schema changelog keyed by `BOOT_INFO_VERSION`.
- TODO-01 domain index: [`todo/01-boot-platform/INDEX.md`](../../todo/01-boot-platform/INDEX.md) (implementation-tracking source of truth).
- [`tools/boot-info-manifest/`](../../tools/boot-info-manifest/): kernel + mirror manifest emitters and drift detector.

This protocol reference is stable: schema-layer changes bump `BOOT_INFO_VERSION` and get a changelog row; documentation-only edits (new examples, clarified prose) do NOT bump the version.
