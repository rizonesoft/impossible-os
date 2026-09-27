<!-- docs: covers=todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md -->
# Bare-Metal Boot Lab Inventory

> Owner: [`todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md`](../../todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md) §7. This is the manual-evidence half of the boot certification matrix: the VM suites (§2-§6) automate what they can; real hardware is validated here by an operator and the structured results are attached to the release gate (§9).

Bare metal is the acceptance platform (CLAUDE.md "Development Strategy"). "Verified on QEMU/WHPX" is necessary but not sufficient -- a build is boot-certified only when each required certification class below has a current, passing manual evidence record.

## Certification classes

The five minimum classes mirror the `baremetal-*` platform classes in [`tools/boot-cert/boot-cert.yml`](../../tools/boot-cert/boot-cert.yml) (validated by `tools/boot-cert/boot-cert.schema.json`). `boot_device_type` values are the `struct boot_info` enum documented in [boot-info-fields.md](../boot/boot-info-fields.md#boot-device-identity).

| Class | `boot-cert.yml` platform class | Boot device | `boot_device_type` | Why it is a distinct class |
|---|---|---|---|---|
| Desktop SATA | `baremetal-desktop-sata` | Internal SATA/AHCI disk, GPT | `1` (SATA) | AHCI is the baseline boot path; most forgiving firmware. |
| Laptop NVMe | `baremetal-laptop-nvme` | M.2 NVMe SSD, GPT | `2` (NVMe) | NVMe namespace/EUI-64 identity + laptop firmware quirks (XUSB2PR USB routing, modern-standby). |
| USB-only | `baremetal-usb-only` | Removable USB MSC, no internal disk | `3` (USB) | EHCI/xHCI handoff, removable-media + slow-media timing, recovery from USB. |
| Secure Boot + TPM | `baremetal-secureboot-tpm` | Any, Secure Boot ON, TPM 2.0 present | varies | Signed-image enforcement (dbx/SBAT) + PCR measured boot must pass. |
| No-TPM | `baremetal-no-tpm` | Any, TPM absent/disabled | varies | Boot must degrade gracefully when measured boot is unavailable -- no hard dependency on a TPM. |

A build cannot claim a class on the release gate (§9) without a current passing evidence record for it. A class a given lab machine does not satisfy is recorded with `status: skip` (the `result.schema.json` value), never `pass` and never a free-form `n/a`.

## Per-machine inventory schema

Record one block per physical machine in the lab. Keep entries current; firmware updates change quirk behavior.

```
machine_id:        <stable short id, e.g. lab-haswell-laptop>
cpu:               <vendor model, microarch, cores/threads>
chipset:           <PCH / SoC>
firmware_vendor:   <AMI / Insyde / Phoenix / coreboot / EDK2>
firmware_version:  <version string + release date>
gpu:               <integrated / discrete model; GOP driver vendor>
storage_controller:<AHCI / NVMe / RAID-mode; model>
usb_controller:    <EHCI / xHCI; XUSB2PR routing notes>
tpm:               <none / fTPM / dTPM; spec 1.2 or 2.0>
secure_boot:       <available? default state; custom-keys supported?>
classes_satisfied: <subset of the 5 classes above>
```

### Reference machine

| Field | Value |
|---|---|
| `machine_id` | `lab-haswell-laptop` |
| `cpu` | Intel Core i5-4210U (Haswell, 2C/4T) |
| `usb_controller` | EHCI + xHCI; requires XUSB2PR routing to hand USB2 ports to xHCI |
| `classes_satisfied` | `baremetal-laptop-nvme` (if NVMe fitted), `baremetal-usb-only` |

> [!NOTE]
> The Haswell laptop is the recurring physical test machine. Its EHCI+xHCI split and XUSB2PR handoff are why USB boot is a separate certification class -- see [bare-metal-gotchas.md](../infrastructure/bare-metal-gotchas.md) and the USB-handover lessons.

## Manual run template

For each (build, machine, class), the operator records a **`result.schema.json`-shaped** object (`additionalProperties: false`, so field names must match exactly). Field rules:

- `status` is exactly ONE of `pass`, `fail`, `skip`, `degraded` -- a class this machine does not satisfy is `skip` (never a free-form `n/a`). `tier` is exactly ONE of `nightly`, `rc`, `stable`. `platform_class` is the bare-metal class from the table above.
- `row_id` MUST name a `tools/boot-cert/boot-cert.yml` row whose `platforms[]` includes the recorded `platform_class`, or `tools/boot-cert/lint.py` rejects the result. Valid rows per class: desktop-SATA -> `bare-metal-hardening`; laptop-NVMe -> `nvme-storage`; USB-only -> `xhci-usb-boot`; SecureBoot+TPM -> `tpm-measured-boot` or `uefi-secureboot-state`; no-TPM -> `bare-metal-hardening`.

Additionally, `platform_class` must be one of the selected machine's `classes_satisfied` (the schema/lint layer does NOT enforce this -- it only checks the `row_id` row lists the class -- so the operator owns the machine-satisfies-class invariant).

Concrete example (laptop-NVMe cold boot on `lab-haswell-laptop`, which lists `baremetal-laptop-nvme` in `classes_satisfied`, using `nvme-storage` whose `platforms[]` includes that class):

```json
{
  "row_id": "nvme-storage",
  "status": "pass",
  "build_id": "1c2e9ccd",
  "artifact_id": null,
  "machine_id": "lab-haswell-laptop",
  "tier": "stable",
  "platform_class": "baremetal-laptop-nvme",
  "ts": "2026-06-19T00:00:00Z",
  "logs": ["serial.log", "screen.ppm", "blackbox/", "firmware-report.json"],
  "reason": null
}
```

This is the manual counterpart to the `tools/boot-cert/result.schema.json` object the VM suites emit -- identical shape, hand-authored. The §8 support-bundle collector packages the `logs` artifacts and the §9 release gate consumes `status` keyed by `row_id` + `platform_class` + `tier`.

## Evidence checklist (per run)

- [ ] Serial log captured and attached (shows `Boot complete in` + reaches the shell, or the documented class contract).
- [ ] Screenshot of the desktop / first-boot state.
- [ ] BlackBox `X:\` export pulled (postcode log, crash, diag) where the partition mounts.
- [ ] Firmware report captured (firmware vendor/version, Secure Boot state, TPM presence) and cross-checked against this machine's inventory block.
- [ ] `boot_device_type` / partition style observed matches the expected class.
- [ ] For Secure Boot + TPM class: signed-image enforcement verified (an unsigned/`dbx`-listed image is rejected) and PCRs are populated.
- [ ] For no-TPM class: boot completes with measured-boot gracefully skipped (no hard TPM dependency).
- [ ] Any firmware quirk observed is filed per the section below.

## Firmware quirk tracking

Bare-metal firmware quirks (GOP timing, NVMe identify oddities, USB routing, ACPI/SMBIOS table defects, Secure Boot key handling) are owned by the firmware-table platform inventory. File each observed quirk there with the `machine_id`, firmware vendor/version, and the observed-vs-expected behavior, and link the run's evidence record.

-> XREF: [`TODO-04-firmware-table-platform-inventory.md`](../../todo/01-boot-platform/TODO-04-firmware-table-platform-inventory.md) §9 (Firmware Quirk Database -- the `s_quirks[]` SMBIOS-keyed descriptor table). File a newly observed lab quirk there. The §11 firmware-sanity certification gate consumes the resulting `firmware-tables.json` (`quirks_active[]`); this lab inventory is where the human-observed quirks that feed §9 are first recorded.
