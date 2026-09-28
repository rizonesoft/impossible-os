# Boot Platform

The UEFI bootloader, the boot_info handoff to the kernel, boot entries and policy, firmware tables, CPU bringup and bare-metal hardening.

## Roadmap Overviews

One page per boot-platform roadmap file, each following the [page contract](../contributing/docs-page-contract.md).

| Document                                                                | Topics                                                           |
| ----------------------------------------------------------------------- | ---------------------------------------------------------------- |
| [Boot Protocol ABI Handoff](boot-protocol-abi-overview.md)              | boot_info contract, versioning, ABI manifest drift gate          |
| [UEFI Bootloader Hardening and Secure Boot](uefi-hardening-overview.md) | Runtime services, UEFI variables, shim chain, SBAT, UKI          |
| [Bootloader Error Recovery](bootloader-error-recovery.md)               | Fatal-boot screens, NVRAM error codes, history ring, ELF checks  |
| [Firmware Table Inventory](firmware-platform-inventory.md)              | ACPI, SMBIOS and ESRT catalog, validation, Registry and JSON     |
| [Boot Device Discovery](boot-device-discovery.md)                       | Boot device identity, type, partition GUID, fallback chain       |
| [Boot Media and Installer Handoff](boot-media-image-pipeline.md)        | Media role detection and its handoff to the kernel               |
| [Boot Entries, Menu and Policy](boot-entries-menu-policy.md)            | Entry store, policy merge, boot menu, audit, loader variables    |
| [Alternate Boot Protocols](alternate-boot-protocols.md)                 | Why UEFI/GPT is the only supported boot path                     |
| [CPU Boot Sequencing and AP Bringup](cpu-boot-sequencing.md)            | INIT/SIPI, per-CPU hardening, online mask lifecycle              |
| [Bare Metal Boot Hardening](bare-metal-hardening.md)                    | IST stacks, graceful degradation, guarded early-boot stacks      |
| [Interrupt Architecture and Timers](interrupt-timer-architecture.md)    | MADT, LAPIC/IOAPIC, IDT vectors, IRQ API, unified timer HAL      |
| [Early Entropy and Random Seed](early-entropy-random-seed.md)           | Boot entropy sources, seed handoff, seed file, CSPRNG first key  |
| [TPM Measured Boot and Attestation](tpm-measured-boot.md)               | Event log replay, golden baseline, sealing, signed quotes        |
| [Boot Diagnostics](boot-diagnostics.md)                                 | Named stages, POST codes, cross-boot panic evidence, load log    |
| [Visual POST Display](visual-post-display.md)                           | Pre-splash stage table, postbars modes, last-boot banner         |
| [NVMe Storage Driver](nvme-boot-storage.md)                             | NVMe bringup, polled I/O queue, flush and shutdown               |
| [USB Boot Storage (xHCI)](xhci-usb-boot.md)                             | xHCI controller, device enumeration, USB mass storage            |
| [USB HID Keyboard and Mouse](usb-hid-boot-protocol.md)                  | Boot-protocol HID reports, dedicated event ring, input merge     |
| [USB Boot Hardening](usb-boot-hardening.md)                             | SCSI retry, stall recovery, bounded timeouts, slow-media logging |
| [USB Zero-Delay Handover](usb-zero-delay-handover.md)                   | Bootloader-allocated xHCI DMA and kernel inherit                 |
| [A/B Dual-Slot Boot and Rollback](ab-boot-rollback.md)                  | Slot metadata, try counting, rollback, mark-good                 |
| [Recovery Partition](recovery-partition.md)                             | Read-only recovery slot, kernel backup, IXFS checker             |
| [Boot Watchdog](boot-watchdog.md)                                       | ACPI WDAT arm, pet and verified disarm                           |
| [BlackBox Service Partition](blackbox-service-partition.md)             | `X:\` log partition, dirty-bit repair, cleanup, retention        |
| [Network Boot (PXE and HTTP)](network-boot.md)                          | Network launch discovery, DHCP capture, TFTP and HTTP probes     |
| [Hibernation Resume Handoff](hibernation-resume-handoff.md)             | Hibernation image format; boot-side resume not built             |
| [Advanced UEFI Features](uefi-advanced.md)                              | Chainload, firmware advisor, W^X, multi-GPU GOP, SMBIOS          |
| [Boot Validation Matrix](boot-validation-matrix.md)                     | Certification matrix, lint, reboot reliability suite             |
| [Boot Performance and Health](boot-performance-health.md)               | Step budgets, boot-health.json, boot-trend.json                  |

## Reference Documents

| Document                                                 | Topics                                                            |
| -------------------------------------------------------- | ----------------------------------------------------------------- |
| [Boot Protocol Reference](boot-protocol.md)              | The boot_info handoff contract between BOOTX64.EFI and the kernel |
| [boot_info Field Ownership](boot-info-fields.md)         | Which side writes and reads each boot_info field                  |
| [Boot Protocol Changelog](boot-protocol-changelog.md)    | Every `BOOT_INFO_VERSION` bump and what it changed                |
| [Boot Error History Ring](boot-error-history.md)         | Persisted boot error records and how to read them                 |
| [Firmware Tables Wire Format](firmware-tables-schema.md) | `firmware-tables.json` schema                                     |
| [Boot Entry Store Schema](boot-entry-schema.md)          | On-disk boot entry records                                        |
| [Bootstrap and First-Install Seeding](bootstrap.md)      | How the first boot entries are created                            |
| [bootcfg](bootcfg.md)                                    | The boot entry store editor                                       |
| [Boot Menu](boot-menu.md)                                | Menu renderer, hotkeys and indicators                             |
| [Boot Policy Merge Order](boot-policy.md)                | How boot policy sources combine                                   |
| [Boot Policy Audit Schema](boot-history-schema.md)       | Per-boot policy audit and entry-store mutation logs               |
| [Boot Health Gate](boot-health.md)                       | Per-entry health gate that marks a boot entry good                |
| [Boot Health Wire Format](boot-health-schema.md)         | `boot-health.json` schema                                         |
| [Boot Timeline Wire Format](boot-timeline-schema.md)     | `boot-timeline.json` schema                                       |
| [Boot Trend Wire Format](boot-trend-schema.md)           | `boot-trend.json` schema                                          |
| [OS-Visible Loader Variables](loader-vars.md)            | UEFI variables the loader publishes to the OS                     |
| [PCR Allocation and Policy Masks](pcr-allocation.md)     | Which TPM PCRs measure what                                       |
| [BlackBox Diagnostic Artifacts](black-box-artifacts.md)  | Diagnostic files written under `X:\Diag\`                         |
| [Alternate Boot Protocol Policy](alt-boot.md)            | Why only UEFI boot is supported, and the compatibility boundary   |

## See Also

- [Secure Boot Key Management](../guides/secure-boot-keys.md)
- [Bare Metal Gotchas](../infrastructure/bare-metal-gotchas.md)
- [Boot Artifacts](../release/boot-artifacts.md)
