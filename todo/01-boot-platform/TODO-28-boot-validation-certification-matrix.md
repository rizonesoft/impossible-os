# TODO-28 -- Boot Validation & Hardware Certification Matrix

> **Goal:** Turn the boot platform from a set of feature TODOs into a certified boot surface. Every boot promise needs an automated or manual gate: QEMU WHPX/TCG, VirtualBox, Hyper-V, USB 2/3, NVMe, SATA, Secure Boot, TPM, network boot, A/B rollback, recovery, watchdog, hibernation, and bare-metal classes.
> **Current state:** Many TODOs mention platform verification individually, and several scripts exist. There is no single boot certification matrix, no result schema, no release gate, no bare-metal lab inventory, and no consolidated support bundle.

## Inputs

- [`scripts/debug/run-boot-tests.bat`](../../scripts/debug/run-boot-tests.bat)
- [`scripts/machines`](../../scripts/machines)
- [`scripts/deploy`](../../scripts/deploy)
- [`todo/01-boot-platform`](.)
- -> XREF: `TODO-06-boot-media-image-installer-handoff.md §9` -- artifact matrix
- -> XREF: `TODO-24-blackbox-service-partition.md` -- logs and diagnostics captured by tests

## Outcome

- Every boot-platform TODO contributes test cases to one certification matrix.
- Release builds cannot claim boot support without passing required gates.
- Manual bare-metal results are structured and comparable over time.
- BlackBox logs, serial output, VPD state, and artifact manifests are archived per run.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| 💎 | 1 | Boot certification matrix schema | GAP-ANALYSIS | [ ] |
| 💎 | 2 | VM automation suite | §1 | [ ] |
| 💎 | 3 | Storage/media boot suite | §1, TODO-16, TODO-17, TODO-06 | [ ] |
| 💎 | 4 | Security boot suite | §1, TODO-02, TODO-13, TODO-12 | [ ] |
| 💎 | 5 | Recovery and rollback suite | §1, TODO-21, TODO-22, TODO-23, TODO-26 | [ ] |
| 💎 | 6 | Network boot suite | §1, TODO-25 | [ ] |
| 💎 | 7 | Bare-metal lab inventory | §1 | [ ] |
| ⭐ | 8 | Boot support bundle collector | §2-§7, TODO-24 | [ ] |
| ⭐ | 9 | Release gate and dashboard | §1-§8 | [ ] |
| 💎 | 10 | Certification docs | §1-§9 | [ ] |

## 1. Boot Certification Matrix Schema

- [ ] Define `boot-cert.yml` with platforms, artifacts, features, required/optional gates, and owner TODO.
- [ ] Define result JSON schema with pass/fail/skip/degraded, logs, build id, artifact id, and machine id.
- [ ] Map every TODO-01 through TODO-28 to at least one certification row.
- [ ] Commit: `"test: boot certification matrix schema"`

## 2. VM Automation Suite

- [ ] Standardize QEMU WHPX, QEMU TCG, VirtualBox, and Hyper-V launchers.
- [ ] Capture serial, screenshot, BlackBox image, and exit status.
- [ ] Add timeout/hang detection.
- [ ] Support test-suite filters and boot.conf patching.
- [ ] Commit: `"test: boot VM automation suite"`

## 3. Storage/Media Boot Suite

- [ ] Test SATA/AHCI, NVMe, USB MSC, ISO, raw disk, VHD/VHDX, and VDI.
- [ ] Validate boot device registry and BlackBox logs for each run.
- [ ] Include slow-media tests for USB 2.0.
- [ ] Archive artifact manifest and partition map.
- [ ] Commit: `"test: boot storage media suite"`

## 4. Security Boot Suite

- [ ] Test Secure Boot off/on, signed/unsigned bootloader, dbx mismatch, SBAT generation.
- [ ] Test TPM present/absent, PCR replay, entropy sources, and manifest validation.
- [ ] Capture attestation and entropy reports.
- [ ] Fail release gate on insecure unexpected success.
- [ ] Commit: `"test: boot security suite"`

## 5. Recovery and Rollback Suite

- [ ] Test A/B success, failed slot rollback, recovery partition fallback, watchdog reboot, hibernation failure fallback.
- [ ] Assert reason codes and counters.
- [ ] Verify no infinite loops.
- [ ] Capture VPD/recovery screenshots.
- [ ] Commit: `"test: boot recovery rollback suite"`

## 6. Network Boot Suite

- [ ] Start local DHCP/TFTP/HTTP fixtures.
- [ ] Test PXE, HTTP Boot, signed manifest, missing file, bad hash, timeout, and local fallback.
- [ ] Capture `network-boot.json`.
- [ ] Mark bare-metal PXE as manual unless lab supports automation.
- [ ] Commit: `"test: network boot certification suite"`

## 7. Bare-Metal Lab Inventory

- [ ] Create `docs/hardware/boot-lab.md` with CPU, chipset, firmware, GPU, storage, USB controller, TPM, Secure Boot status.
- [ ] Define minimum certification classes: desktop SATA, laptop NVMe, USB-only, Secure Boot+TPM, no-TPM.
- [ ] Add manual run template and evidence checklist.
- [ ] Track firmware quirks back to TODO-04 §9.
- [ ] Commit: `"docs: boot bare-metal lab inventory"`

## 8. Boot Support Bundle Collector

- [ ] Collect serial log, boot timeline, firmware report, BlackBox files, artifact manifest, screenshots, and result JSON.
- [ ] Compress into `boot-support-{build}-{machine}.zip`.
- [ ] Redact serial numbers and secrets.
- [ ] Add host tool command.
- [ ] Commit: `"tools: boot support bundle collector"`

## 9. Release Gate and Dashboard

- [ ] Generate `build/reports/boot-cert.html`.
- [ ] Define required gates for nightly, release candidate, and stable.
- [ ] Fail release packaging when required gates fail.
- [ ] Keep history so regressions are visible.
- [ ] Commit: `"ci: boot certification release gate"`

## 10. Certification Docs

- [ ] Add `docs/testing/boot-certification.md`.
- [ ] Explain how to run VM and bare-metal certification.
- [ ] Link common failure signatures to TODO owners.
- [ ] Include support-bundle upload instructions.
- [ ] Commit: `"docs: boot certification guide"`

## OS Comparison

| ⭐ | Feature | Windows | Linux | Impossible OS |
| --- | --- | --- | --- | --- |
| 💎 | WHQL-style boot matrix | WHQL/HLK | distro CI | TODO-28 |
| 💎 | VM boot automation | internal | distro tests | TODO-28 §2 |
| ⭐ | BlackBox support bundle | WER/event logs | journal/sosreport | TODO-28 §8 |
| ⭐ | TODO-owner mapped matrix | internal | ad hoc | TODO-28 §1 |

## Unit Tests

- [ ] `test_boot_cert_schema_valid`
- [ ] `test_boot_cert_every_todo_has_gate`
- [ ] `test_boot_result_json_parse`
- [ ] `test_support_bundle_redaction`

## Verification

- [ ] QEMU WHPX and TCG automated run
- [ ] VirtualBox automated or semi-automated run
- [ ] Hyper-V VHDX run
- [ ] Bare-metal manual evidence bundle

