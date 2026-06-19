---
schema_version: 1
id: boot-validation-certification-matrix
domain: 01-boot-platform
status: active
title: "TODO-28 -- Boot Validation & Hardware Certification Matrix"
---

# TODO-28 -- Boot Validation & Hardware Certification Matrix

> **Goal:** Turn the boot platform from a set of feature TODOs into a certified boot surface. Every boot promise needs an automated or manual gate: QEMU WHPX/TCG, VirtualBox, Hyper-V, USB 2/3, NVMe, SATA, Secure Boot, TPM, network boot, A/B rollback, recovery, watchdog, hibernation, and bare-metal classes.
> **Current state:** Many TODOs mention platform verification individually, and several scripts exist. There is no single boot certification matrix, no result schema, no release gate, no bare-metal lab inventory, and no consolidated support bundle.

> [!NOTE]
> **Scope boundary vs `15-installer-release/TODO-04-release-qa`.** That TODO owns WHOLE-OS release QA: cross-subsystem regression (its §1), VM certification (§2 QEMU, §3 Hyper-V, §4 VirtualBox), real-hardware checklist (§5), performance benchmarks (§6), release readiness gate (§7), crash-analytics + 30-min idle soak (§8). TODO-28 owns the BOOT-PLATFORM certification surface: the boot-cert matrix schema, boot-path gates (firmware/Secure Boot/recovery/network boot), repeat-BOOT flake reliability, and the boot support bundle. The VM launchers + bare-metal checklist are SHARED -- TODO-28 §2/§7 consume / extend the D15 TODO-04 harness rather than duplicating it (reciprocal XREF in D15 TODO-04). Boot-perf regression is owned by `TODO-29`; visual-POST validation by `TODO-15`; firmware-table validation by `TODO-04` -- §1 encodes those as owner rows that §9 consumes, not re-tests.

## Inputs

- [`scripts/debug/kernel/run-boot-tests.bat`](../../scripts/debug/kernel/run-boot-tests.bat)
- -> XREF: `15-installer-release/TODO-04-release-qa.md` -- whole-OS release QA; TODO-28 is the boot-platform certification subset (see scope-boundary note)
- -> XREF: `TODO-29-boot-perf-health-observability.md` -- boot-perf/timing regression owner; §1 consumes its boot-trend artifact
- -> XREF: `TODO-15-visual-post-display.md` -- visual POST validation owner; §1 consumes it as a gate row
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
| 💎 | 3 | Storage/media boot suite | §1, T16, T17, T06 | [ ] |
| 💎 | 4 | Security boot suite | §1, T02, T13, T12 | [ ] |
| 💎 | 5 | Recovery and rollback suite | §1, T21, T22, T23, T26 | [ ] |
| 💎 | 6 | Network boot suite | §1, T25 | [ ] |
| 💎 | 7 | Bare-metal lab inventory | §1 | [ ] |
| ⭐ | 8 | Boot support bundle collector | §2-§7, TODO-24 | [ ] |
| ⭐ | 9 | Release gate and dashboard | §1-§8 | [ ] |
| 💎 | 10 | Certification docs | §1-§9 | [ ] |
| 💎 | 11 | Firmware sanity certification gate | §1, T04 §2, §8 | [ ] |

## 1. Boot Certification Matrix Schema

- [ ] Define `boot-cert.yml` with platforms, artifacts, features, required/optional gates, and owner TODO.
- [ ] Each row encodes `required_for` (tier x platform-class), `automation_level`, `owner_todo`, and `source_artifact` (the owner output a gate consumes, e.g. TODO-29 boot-trend) so §9 computes required status from the schema
- [ ] Define result JSON schema with pass/fail/skip/degraded, logs, build id, artifact id, and machine id.
- [ ] Map every TODO-01..28 to >=1 row; the lint fails any row lacking an explicit `required_for`/platform applicability policy
- [ ] Commit: `"test: boot certification matrix schema"`

**Test checkpoint:** `boot-cert.yml` parses + each result validates against the JSON schema; a CI lint fails when any TODO-01..28 maps to zero certification rows.

---

## 2. VM Automation Suite

- [ ] Standardize QEMU WHPX, QEMU TCG, VirtualBox, and Hyper-V launchers.
- [ ] Capture serial, screenshot, BlackBox image, and exit status.
- [ ] Add timeout/hang detection.
- [ ] Support test-suite filters and boot.conf patching.
- [ ] Boot-reliability gate: N consecutive cold + warm reboots per tier/platform, per-iteration capture, flake threshold 0 for stable (catches intermittent-boot / NVRAM-ordering / storage-timing flakes a single-shot misses)
- [ ] -> XREF: `15-installer-release/TODO-04-release-qa.md §2`/`§8` -- this gate is BOOT repeat-boot flake detection; whole-OS idle/crash soak stays release-QA
- [ ] Commit: `"test: boot VM automation suite"`

**Test checkpoint:** each launcher (QEMU WHPX/TCG, VirtualBox, Hyper-V) boots to `C:\>` or times out with a captured serial + screenshot + BlackBox image + exit status; hang detection fires within the timeout.

---

## 3. Storage/Media Boot Suite

- [ ] Test SATA/AHCI, NVMe, USB MSC, ISO, raw disk, VHD/VHDX, and VDI.
- [ ] Validate boot device registry and BlackBox logs for each run.
- [ ] Include slow-media tests for USB 2.0.
- [ ] Archive artifact manifest and partition map.
- [ ] Commit: `"test: boot storage media suite"`

**Test checkpoint:** each storage class (SATA/AHCI, NVMe, USB MSC, ISO, raw disk, VHD/VHDX, VDI) boots and archives its boot-device registry entry + BlackBox log + artifact manifest + partition map; USB 2.0 slow-media case completes.

---

## 4. Security Boot Suite

- [ ] Test Secure Boot off/on, signed/unsigned bootloader, dbx mismatch, SBAT generation.
- [ ] Test TPM present/absent, PCR replay, entropy sources, and manifest validation.
- [ ] Capture attestation and entropy reports.
- [ ] Fail release gate on insecure unexpected success.
- [ ] Commit: `"test: boot security suite"`

**Test checkpoint:** Secure Boot off/on, signed/unsigned bootloader, dbx mismatch, SBAT generation, and TPM present/absent each produce the expected pass/fail with attestation + entropy reports captured; an insecure unexpected-success fails the release gate.

---

## 5. Recovery and Rollback Suite

- [ ] Test A/B success, failed slot rollback, recovery partition fallback, watchdog reboot, hibernation failure fallback.
- [ ] Assert reason codes and counters.
- [ ] Verify no infinite loops.
- [ ] Capture VPD/recovery screenshots.
- [ ] Commit: `"test: boot recovery rollback suite"`

**Test checkpoint:** A/B success, failed-slot rollback, recovery-partition fallback, watchdog reboot, and hibernation-failure fallback each hit the expected reason code + counter with no infinite loop; VPD/recovery screenshots captured.

---

## 6. Network Boot Suite

- [ ] Start local DHCP/TFTP/HTTP fixtures.
- [ ] Test PXE, HTTP Boot, signed manifest, missing file, bad hash, timeout, and local fallback.
- [ ] Capture `network-boot.json`.
- [ ] Mark bare-metal PXE as manual unless lab supports automation.
- [ ] Commit: `"test: network boot certification suite"`

**Test checkpoint:** with local DHCP/TFTP/HTTP fixtures, PXE + HTTP Boot + signed-manifest + missing-file + bad-hash + timeout + local-fallback each produce the expected result and capture `network-boot.json`; bare-metal PXE flagged manual when the lab lacks automation.

---

## 7. Bare-Metal Lab Inventory

- [ ] Create `docs/hardware/boot-lab.md` with CPU, chipset, firmware, GPU, storage, USB controller, TPM, Secure Boot status.
- [ ] Define minimum certification classes: desktop SATA, laptop NVMe, USB-only, Secure Boot+TPM, no-TPM.
- [ ] Add manual run template and evidence checklist.
- [ ] Track firmware quirks back to TODO-04 §9.
- [ ] Commit: `"docs: boot bare-metal lab inventory"`

**Test checkpoint:** `docs/hardware/boot-lab.md` lists the 5 certification classes (desktop SATA, laptop NVMe, USB-only, Secure Boot+TPM, no-TPM) with per-machine inventory + a manual run template + evidence checklist; firmware quirks link back to TODO-04 §9.

---

## 8. Boot Support Bundle Collector

- [ ] Collect serial log, boot timeline, firmware report, BlackBox files, artifact manifest, screenshots, and result JSON.
- [ ] Compress into `boot-support-{build}-{machine}.zip`.
- [ ] Redact serial numbers and secrets.
- [ ] Add host tool command.
- [ ] Commit: `"tools: boot support bundle collector"`

**Test checkpoint:** the collector produces `boot-support-{build}-{machine}.zip` containing serial log + boot timeline + firmware report + BlackBox files + artifact manifest + screenshots + result JSON, with serial numbers and secrets redacted.

---

## 9. Release Gate and Dashboard

- [ ] Generate `build/reports/boot-cert.html`.
- [ ] Define required gates for nightly, release candidate, and stable.
- [ ] Fail release packaging when required gates fail.
- [ ] Keep history so regressions are visible.
- [ ] Commit: `"ci: boot certification release gate"`

**Test checkpoint:** `build/reports/boot-cert.html` is generated from result JSON; a required-gate failure (per nightly / RC / stable tier) fails release packaging; prior-run history is retained so regressions are visible.

---

## 10. Certification Docs

- [ ] Add `docs/testing/boot-certification.md`.
- [ ] Explain how to run VM and bare-metal certification.
- [ ] Link common failure signatures to TODO owners.
- [ ] Include support-bundle upload instructions.
- [ ] Commit: `"docs: boot certification guide"`

**Test checkpoint:** `docs/testing/boot-certification.md` documents running VM + bare-metal certification, maps common failure signatures to owner TODOs, and gives support-bundle upload instructions.

---

## 11. Firmware Sanity Certification Gate

Gate releases on firmware-table sanity, not just Secure Boot/TPM: malformed or degraded ACPI/SMBIOS/MAT/RT tables are exactly the bare-metal failure class this matrix must catch (FWTS does the same for Linux). Consumes the TODO-04 firmware inventory rather than re-validating.

- [ ] Consume TODO-04 `firmware-tables.json` + `HKLM\HARDWARE\Firmware\Tables\*` (range/checksum validation from TODO-04 §2; report from §8) as a certification input
- [ ] Pass/degraded/fail per platform class: a degraded ACPI/SMBIOS/MAT/RT validation result fails the required gate for stable; degraded is recorded (not failed) on nightly
- [ ] Encode the TODO-04 owner row + `firmware-tables.json` `source_artifact` in `boot-cert.yml`
- [ ] Capture the firmware report + degraded-flag accounting in the support bundle (§8)
- [ ] -> XREF: `TODO-04-firmware-table-platform-inventory.md §2`/`§8` -- TODO-04 owns table validation + report; §11 owns the cert gate consuming it
- [ ] Commit: `"test: firmware sanity certification gate"`

**Test checkpoint:** a release whose `firmware-tables.json` marks an ACPI/SMBIOS table degraded fails the stable required gate and is recorded (not failed) on nightly; a clean firmware report passes.

---

## OS Comparison

| ⭐ | Feature                   | 🪟 Win11                  | 🐧 Linux                 | 🚀 Impossible OS              |
|----|---------------------------|----------------------------|---------------------------|-------------------------------|
| 💎 | WHQL-style boot matrix    | ✅ WHQL/HLK               | ⚠️ per-distro CI          | ⬜ §1 cert matrix + gate      |
| 💎 | VM boot automation        | ⚠️ internal only          | ⚠️ per-distro tests       | ⬜ §2 WHPX/TCG/VBox/Hyper-V   |
| ⭐ | BlackBox support bundle   | ⚠️ WER/event logs         | ⚠️ journal/sosreport      | ⬜ §8 one-zip bundle          |
| ⭐ | TODO-owner mapped matrix  | ❌ internal, opaque        | ❌ ad hoc                 | ⬜ §1 every TODO -> gate row  |
| 💎 | Firmware sanity gate      | ✅ HLK firmware tests     | ✅ FWTS (ACPI/UEFI)       | ⬜ §11 consumes T04 inventory |
| 💎 | Repeat-boot reliability   | ✅ HLK MTBF               | ⚠️ KernelCI boot-to-shell | ⬜ §2 N-boot flake gate       |

---

## Unit Tests

> Host-side certification-harness tests (schema + JSON + redaction), not kernel `TEST_CAT_*`. Wire as a `tools/boot-cert/` test target run by the §9 release gate.

- [ ] `test_boot_cert_schema_valid` -- `boot-cert.yml` validates against the schema
- [ ] `test_boot_cert_every_todo_has_gate` -- every TODO-01..28 maps to >=1 row
- [ ] `test_boot_result_json_parse` -- result JSON round-trips pass/fail/skip/degraded
- [ ] `test_support_bundle_redaction` -- serials/secrets absent from the bundle

---

## Verification

> **Test runner:** N/A (host-side certification harness; no kernel `TEST_CAT_*` surface) | validation: `tools/boot-cert/` schema+JSON+redaction tests run by the §9 release gate; VM/bare-metal evidence bundles per §2-§8

- [ ] QEMU WHPX and TCG automated run
- [ ] VirtualBox automated or semi-automated run
- [ ] Hyper-V VHDX run
- [ ] Bare-metal manual evidence bundle (manual -- run on lab hardware)

