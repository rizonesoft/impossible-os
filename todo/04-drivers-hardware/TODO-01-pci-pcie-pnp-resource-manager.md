---
schema_version: 1
id: pci-pcie-pnp-resource-manager
domain: 04-drivers-hardware
status: active
title: "TODO-01 -- PCI/PCIe, PnP & Resource Manager"
---

# TODO-01 -- PCI/PCIe, PnP & Resource Manager

> **Goal:** Own the full PCI/PCIe and Plug and Play substrate that every hardware driver depends on: bus enumeration, bridge windows, BAR sizing, resource assignment, IRQ routing, ACPI device correlation, driver binding, hot-plug, power state coordination, and persistent device identity. This replaces scattered assumptions and the stale cross-reference to a non-existent kernel-core PCI TODO.
> **Current state:** `src/kernel/drivers/pci.c` performs legacy CF8/CFC scans and simple `pci_find_device()` lookups. TODO-08 adds ECAM, capability scanning, MSI/MSI-X and hot-plug pieces, while TODO-05 adds driver match tables. There is no authoritative PnP device tree, no resource allocator, no bridge-window manager, no driver bind/unbind state machine, and no ACPI `_ADR`/`_PRT` correlation layer.

## Inputs

- [`src/kernel/drivers/pci.c`](../../src/kernel/drivers/pci.c)
- [`include/kernel/drivers/pci.h`](../../include/kernel/drivers/pci.h)
- [`src/kernel/acpi.c`](../../src/kernel/acpi.c)
- -> XREF: `TODO-05-kernel-module-system.md §3` -- driver model consumes device enumeration and bind/unbind callbacks
- -> XREF: `TODO-08-core-driver-enhancements.md §1-§6` -- ECAM, capabilities, MSI/MSI-X and PCIe hot-plug are folded into the resource manager
- -> XREF: `TODO-02-apic-interrupt-routing.md §3` -- MSI vector allocation
- -> XREF: `TODO-03-acpi-power-management.md §1` -- ACPI namespace and `_PRT`/`_ADR` data
- -> XREF: `TODO-07-device-manager.md` -- user-visible device tree and diagnostics

## Outcome

- PCI/PCIe devices are represented by stable `device_node_t` objects with bus path, ACPI path, resources, state, and bound driver.
- BAR sizing, bridge-window decoding, resource conflicts, and hot-plug changes are handled centrally.
- Driver probe/remove/suspend/resume has one state machine for built-in and loadable drivers.
- Device Manager, registry, and diagnostics consume the same device tree.

## Implementation Order

| Priority  | Order | Deliverable                                              | Depends On     | Status |
| --------- | :---: | -------------------------------------------------------- | -------------- | :----: |
| Parity    |   1   | Canonical device node and bus path schema                | TODO-05 §3     |  [ ]   |
| Parity    |   2   | Full PCI/PCIe enumeration with multifunction and bridges | §1             |  [ ]   |
| Parity    |   3   | BAR sizing and resource window allocator                 | §2, TODO-08 §1 |  [ ]   |
| Parity    |   4   | ACPI `_ADR` / `_PRT` / `_CRS` correlation                | §1, TODO-03 §1 |  [ ]   |
| Parity    |   5   | Central IRQ routing and MSI/MSI-X handoff                | §4, TODO-02 §3 |  [ ]   |
| Parity    |   6   | Driver bind/unbind/probe/remove state machine            | §1, TODO-05 §3 |  [ ]   |
| Parity    |   7   | PCIe hot-plug and surprise-removal events                | §3, TODO-08 §6 |  [ ]   |
| Parity    |   8   | Device power states and wake capabilities                | §1, TODO-03    |  [ ]   |
| Exclusive |   9   | Resource conflict diagnostics and recovery               | §1-§8          |  [ ]   |
| Parity    |  10   | Unit, VM, and hardware PCI matrix                        | §1-§9          |  [ ]   |

## 1. Canonical Device Node and Bus Path Schema

- [ ] Define `device_node_t` with stable id, bus type, parent, ACPI path, PCI BDF, class, resources, state, health, and bound driver name.
- [ ] Add `device_tree_init()` before non-boot driver probing.
- [ ] Store boot-critical devices discovered earlier as pre-existing nodes instead of duplicate ad hoc records.
- [ ] Commit: `"drivers: define canonical device node and PCI bus path"`

## 2. Full PCI/PCIe Enumeration with Bridges

- [ ] Enumerate all buses, devices, functions, and secondary/subordinate buses.
- [ ] Detect PCI-to-PCI bridges, PCIe root ports, downstream ports, and multifunction devices.
- [ ] Preserve legacy CF8/CFC fallback and use ECAM when TODO-08 exposes it.
- [ ] Commit: `"drivers: enumerate PCIe bridges and multifunction devices"`

## 3. BAR Sizing and Resource Window Allocator

- [ ] Implement 32-bit/64-bit BAR sizing with disabled-memory decode guard.
- [ ] Decode bridge memory/prefetchable/IO windows.
- [ ] Assign resources only when firmware left a device unassigned or hot-plug requires it.
- [ ] Commit: `"drivers: central PCI resource allocator"`

## 4. ACPI Correlation

- [ ] Map PCI BDF to ACPI namespace via `_ADR`.
- [ ] Import `_PRT` routing and `_CRS` resource windows.
- [ ] Publish ACPI device ids and compatible ids on `device_node_t`.
- [ ] Commit: `"drivers: correlate PCI devices with ACPI namespace"`

## 5. IRQ Routing and MSI/MSI-X Handoff

- [ ] Route legacy INTx through `_PRT`/IOAPIC when MSI is unavailable -- replaces the PCI_INTERRUPT_LINE-as-GSI assumption in the `irq_request_gsi_ex` consumers (`ahci_core.c`, `vbox_mouse.c`, `rtl8139.c`, `virtio/input.c`; filed from `01-boot-platform/TODO-11` §5 review).
- [ ] Call APIC vector allocator for MSI/MSI-X.
- [ ] Store active interrupt mode on device nodes.
- [ ] Commit: `"drivers: centralize PCI interrupt routing"`

## 6. Driver Bind/Unbind State Machine

- [ ] Add `driver_probe_device()`, `driver_bind()`, `driver_unbind()`, and remove callback ordering.
- [ ] Support built-in drivers and `.kmod` drivers through the same API.
- [ ] Prevent duplicate binding when a boot-critical built-in driver already owns a device.
- [ ] Commit: `"drivers: PnP bind and unbind state machine"`

## 7. Hot-Plug and Surprise Removal

- [ ] Convert PCIe hot-plug signals into device-tree add/remove events.
- [ ] Notify bound drivers before remove when possible.
- [ ] Mark surprise-removed devices unhealthy and block new I/O.
- [ ] Publish arrival/removal via `knf_publish` on the `Device/*` catalog states (not a bespoke queue); KNF owns the state names + payload schema, this section owns the producer side (-> XREF: D02 T16 §5).
- [ ] Commit: `"drivers: PCIe hot-plug device events"`

## 8. Device Power States

- [ ] Track D0-D3cold support, PME wake, and runtime idle policy.
- [ ] Coordinate suspend/resume callbacks with ACPI power management.
- [ ] Commit: `"drivers: PCI power state coordination"`

## 9. Resource Conflict Diagnostics

- [ ] Emit conflict reports when BAR windows, IRQs, or ACPI resources overlap.
- [ ] Add BlackBox and Device Manager views for failed probes.
- [ ] Commit: `"drivers: PCI resource conflict diagnostics"`

## 10. Tests

- [ ] Unit-test BAR sizing helpers and bridge-window math.
- [ ] QEMU matrix: PCI bridges, multifunction devices, hot-plug, MSI and INTx fallback.
- [ ] Bare-metal checklist: laptop, desktop, and workstation with at least one PCIe bridge.
- [ ] Commit: `"test: PCI/PNP resource manager"`

## OS Comparison

| Priority  | Feature         | Windows              | Linux       | Impossible OS |
| --------- | --------------- | -------------------- | ----------- | ------------- |
| Parity    | PnP device tree | PnP Manager          | driver core | TODO-01       |
| Parity    | PCI resources   | PnP/ACPI arbiter     | pci core    | TODO-01 §3    |
| Parity    | Hot-plug        | PCI bus driver       | pciehp      | TODO-01 §7    |
| Exclusive | Conflict report | Device Manager codes | dmesg/sysfs | TODO-01 §9    |

