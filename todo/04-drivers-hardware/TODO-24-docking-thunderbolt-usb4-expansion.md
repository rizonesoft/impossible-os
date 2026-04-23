---
schema_version: 1
id: docking-thunderbolt-usb4-expansion
domain: 04-drivers-hardware
status: active
title: "TODO-24 -- Docking, Thunderbolt, USB4 & External Expansion"
---

# TODO-24 -- Docking, Thunderbolt, USB4 & External Expansion

> **Goal:** Support modern external expansion: USB-C docks, Thunderbolt/USB4 topology, alternate-mode display handoff, eGPU boundaries, PCIe tunneling security, device authorization, hot-plug storms, dock power/events, and external-device diagnostics. This TODO does not implement every tunneled device driver; it owns the expansion fabric and policy.
> **Current state:** USB, PCIe hot-plug, GPU, power, and security TODOs cover pieces, but no TODO owns dock topology, Thunderbolt/USB4 security levels, external authorization, alt-mode display coordination, or eGPU attach/detach policy.

## Inputs

- -> XREF: `TODO-10-usb-stack.md` -- USB-C devices and hubs
- -> XREF: `TODO-01-pci-pcie-pnp-resource-manager.md §7` -- PCIe hot-plug
- -> XREF: `TODO-17-gpu-display-drivers.md` -- external displays and eGPU handoff
- -> XREF: `TODO-04-security-hardware.md §4` -- IOMMU required for external PCIe DMA safety
- -> XREF: `TODO-03-acpi-power-management.md` -- dock power, wake, and ACPI events

## Outcome

- External expansion topology is visible and policy-controlled.
- Thunderbolt/USB4 tunneled PCIe devices require authorization and IOMMU protection.
- Dock attach/detach updates display, network, audio, USB, and power state safely.

## Implementation Order

| Priority | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| Parity | 1 | External expansion topology model | TODO-01 | [ ] |
| Parity | 2 | USB-C dock and hub classification | §1, TODO-10 | [ ] |
| Parity | 3 | Thunderbolt/USB4 security policy | §1, TODO-04 | [ ] |
| Parity | 4 | PCIe tunneling and authorization | §3, TODO-01 | [ ] |
| Parity | 5 | DisplayPort alt-mode/eGPU boundary | §1, TODO-17 | [ ] |
| Parity | 6 | Dock power, wake, and button events | §1, TODO-03 | [ ] |
| Parity | 7 | Hot-plug storm resilience | §1-§6 | [ ] |
| Parity | 8 | Device Manager dock topology | §1, TODO-07 | [ ] |
| Exclusive | 9 | External DMA risk report | §3, TODO-04 | [ ] |
| Parity | 10 | Dock/Thunderbolt certification matrix | §1-§9 | [ ] |

## 1. External Expansion Topology Model

- [ ] Define `expansion_node_t` for docks, ports, tunneled buses, and child devices.
- [ ] Link USB, PCIe, display, audio, and network children under physical dock nodes.
- [ ] Commit: `"drivers/dock: expansion topology model"`

## 2. USB-C Dock and Hub Classification

- [ ] Detect USB hubs that are docks and classify downstream ports.
- [ ] Store vendor/model/serial and power capabilities.
- [ ] Commit: `"drivers/dock: USB-C dock classification"`

## 3. Thunderbolt/USB4 Security Policy

- [ ] Define supported security levels: disabled, user-authorized, secure-connect, internal-only.
- [ ] Require IOMMU before authorizing external PCIe tunneling.
- [ ] Commit: `"drivers/tbt: security policy"`

## 4. PCIe Tunneling Authorization

- [ ] Block tunneled PCIe devices until authorized.
- [ ] Revoke and surprise-remove safely on unplug.
- [ ] Commit: `"drivers/tbt: PCIe tunnel authorization"`

## 5. Display Alt-mode and eGPU Boundary

- [ ] Notify display stack of external display topology changes.
- [ ] Define eGPU support boundary and safe failure mode.
- [ ] Commit: `"drivers/dock: display alt-mode handoff"`

## 6. Dock Power and Wake Events

- [ ] Surface dock power button, lid-like events, and wake capabilities.
- [ ] Commit: `"drivers/dock: power and wake events"`

## 7. Hot-Plug Storm Resilience

- [ ] Coalesce large attach/detach event bursts.
- [ ] Ensure child devices probe after fabric stabilization.
- [ ] Commit: `"drivers/dock: hotplug storm resilience"`

## 8. Device Manager Topology

- [ ] Show dock tree, ports, authorization, bandwidth, and child health.
- [ ] Commit: `"drivers/dock: Device Manager topology"`

## 9. External DMA Risk Report

- [ ] Report devices blocked due to missing IOMMU or policy.
- [ ] Persist authorization denials to BlackBox.
- [ ] Commit: `"drivers/dock: external DMA risk report"`

## 10. Certification Matrix

- [ ] Test USB-C hub, laptop dock, Thunderbolt dock, external display, and eGPU-denied path.
- [ ] Commit: `"test: docking and Thunderbolt matrix"`

## OS Comparison

| Priority | Feature | Windows | Linux | Impossible OS |
| --- | --- | --- | --- | --- |
| Parity | Dock topology | USB/TBT stack | bolt/sysfs | TODO-24 |
| Parity | TBT security | Kernel DMA Protection | thunderbolt security | TODO-24 §3 |
| Exclusive | DMA risk report | Security Center partial | scattered | TODO-24 §9 |

