# TODO-25 -- Driver Hardware Certification Matrix

> **Goal:** Turn driver support into a certifiable hardware surface. Every driver promise needs a VM test, fixture, or bare-metal checklist covering enumeration, probe, I/O, suspend/resume, hot-plug, error recovery, diagnostics, and removal. This is the release gate for the `04-drivers-hardware` domain.
> **Current state:** Individual TODOs list tests, but there is no single matrix mapping drivers to VM platforms, hardware classes, required devices, destructive tests, firmware needs, or release-blocking criteria.

## Inputs

- [INDEX.md](INDEX.md)
- [GAP-ANALYSIS.md](GAP-ANALYSIS.md)
- -> XREF: `TODO-07-device-manager.md` -- diagnostics consumed during certification
- -> XREF: `TODO-01-pci-pcie-pnp-resource-manager.md` -- device inventory and topology
- -> XREF: `../01-boot-platform/TODO-28-boot-validation-certification-matrix.md` -- boot/platform certification companion
- -> XREF: `15-installer-release` -- release QA consumes this gate

## Outcome

- Every driver TODO maps to at least one certification row.
- Hardware support tiers are explicit: required, best-effort, experimental, unsupported.
- Release images cannot claim a hardware class without passing its gate.

## Implementation Order

| Priority | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| Parity | 1 | Certification schema and support tiers | -- | [ ] |
| Parity | 2 | VM driver test matrix | §1 | [ ] |
| Parity | 3 | Desktop/laptop bare-metal matrix | §1 | [ ] |
| Parity | 4 | USB/peripheral matrix | §1 | [ ] |
| Parity | 5 | Network/wireless/Bluetooth matrix | §1 | [ ] |
| Parity | 6 | Suspend/resume and hot-plug matrix | §1 | [ ] |
| Parity | 7 | Firmware and license matrix | §1, TODO-06 | [ ] |
| Parity | 8 | Failure injection and recovery suite | §1 | [ ] |
| Exclusive | 9 | Support bundle and hardware fingerprint | §1-§8 | [ ] |
| Parity | 10 | Release gate automation | §1-§9 | [ ] |

## 1. Certification Schema and Support Tiers

- [ ] Define matrix columns: TODO, device class, required hardware, VM coverage, bare-metal coverage, suspend, hot-plug, firmware, diagnostics, status.
- [ ] Define tiers: required, supported, best-effort, experimental, unsupported.
- [ ] Commit: `"docs: driver hardware certification schema"`

## 2. VM Driver Test Matrix

- [ ] Cover QEMU, VirtualBox, Hyper-V, and VMware-compatible devices where applicable.
- [ ] Include virtio, e1000, AHCI, NVMe, USB, display, input, and audio fixtures.
- [ ] Commit: `"test: VM driver certification matrix"`

## 3. Desktop/Laptop Bare-Metal Matrix

- [ ] Define minimum desktop and laptop hardware samples.
- [ ] Include ACPI, battery, touchpad, WiFi, Bluetooth, audio, GPU, USB-C, and sensors.
- [ ] Commit: `"docs: bare-metal driver certification matrix"`

## 4. USB/Peripheral Matrix

- [ ] Test hubs, storage, HID, audio, camera, printer, serial, controller, Bluetooth dongle, and WiFi dongle.
- [ ] Include hot-unplug and slow-device tests.
- [ ] Commit: `"test: USB peripheral certification matrix"`

## 5. Network/Wireless/Bluetooth Matrix

- [ ] Cover wired NICs, WiFi chip families, Bluetooth profiles, firmware presence/absence, and reconnect.
- [ ] Commit: `"test: network wireless bluetooth matrix"`

## 6. Suspend/Resume and Hot-Plug Matrix

- [ ] Require S3/S4 or documented unsupported state for each hardware class.
- [ ] Test surprise removal where physically safe.
- [ ] Commit: `"test: driver suspend hotplug matrix"`

## 7. Firmware and License Matrix

- [ ] Ensure every firmware-using device has source, license, hash, and redistribution status.
- [ ] Block release on unknown firmware provenance.
- [ ] Commit: `"docs: firmware certification matrix"`

## 8. Failure Injection and Recovery

- [ ] Simulate probe failure, IRQ storm, DMA allocation failure, firmware missing, hot-unplug during I/O, and reset timeout.
- [ ] Commit: `"test: driver failure injection suite"`

## 9. Support Bundle and Hardware Fingerprint

- [ ] Generate a driver support bundle with PCI/USB/I2C topology, firmware hashes, interrupt mode, driver versions, and last errors.
- [ ] Commit: `"drivers: hardware support bundle"`

## 10. Release Gate Automation

- [ ] Add CI/manual checklist gate that fails unsupported required hardware classes.
- [ ] Commit: `"release: driver certification gate"`

## OS Comparison

| Priority | Feature | Windows | Linux | Impossible OS |
| --- | --- | --- | --- | --- |
| Parity | Hardware certification | HLK/WHQL | distro hardware QA | TODO-25 |
| Parity | Support bundle | msinfo/Event logs | sosreport/hw-probe | TODO-25 §9 |
| Exclusive | TODO-owner matrix | internal | scattered | TODO-25 §1 |

