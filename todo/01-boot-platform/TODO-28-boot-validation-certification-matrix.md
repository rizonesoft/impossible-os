---
schema_version: 1
id: boot-validation-certification-matrix
domain: 01-boot-platform
status: active
title: "TODO-28 -- Boot Validation & Hardware Certification Matrix"
---

# TODO-28 -- Boot Validation & Hardware Certification Matrix

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

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

| ⭐  | Order | Deliverable                        | Depends On             | Status |
| --- | :---: | ---------------------------------- | ---------------------- | :----: |
| 💎  |   1   | Boot certification matrix schema   | GAP-ANALYSIS           |  [x]   |
| 💎  |   2   | VM automation suite                | §1                     |  [x]   |
| 💎  |   3   | Storage/media boot suite           | §1, T16, T17, T06      |  [/]   |
| 💎  |   4   | Security boot suite                | §1, T02, T13, T12      |  [/]   |
| 💎  |   5   | Recovery and rollback suite        | §1, T21, T22, T23, T26 |  [/]   |
| 💎  |   6   | Network boot suite                 | §1, T25                |  [/]   |
| 💎  |   7   | Bare-metal lab inventory           | §1                     |  [x]   |
| ⭐  |   8   | Boot support bundle collector      | §2-§7, TODO-24         |  [/]   |
| ⭐  |   9   | Release gate and dashboard         | §1-§8                  |  [/]   |
| 💎  |  10   | Certification docs                 | §1-§9                  |  [/]   |
| 💎  |  11   | Firmware sanity certification gate | §1, T04 §2, §8         |  [/]   |

## 1. Boot Certification Matrix Schema

- [x] `tools/boot-cert/boot-cert.yml` (29 rows, every boot-platform TODO-01..29) + `boot-cert.schema.json` (row shape, JSON Schema draft 2020-12)
- [x] Each row encodes `required_for` (tier x platform-class), `automation_level`, `owner_todo`, `source_artifact` (e.g. firmware-tables.json, boot-trend.json) so the release gate computes required status from the schema
- [x] `tools/boot-cert/result.schema.json` -- per-run result (pass/fail/skip/degraded, logs, build/artifact/machine id, tier, platform_class, ts)
- [x] `tools/boot-cert/lint.py` (release gate, wired into `scripts/test-tooling.sh`): maps every TODO via the live dir listing (not the cache); fails zero-row TODOs, required rows missing evidence, `required_for` classes outside `platforms[]`
- [x] Commit: `"test: boot certification matrix schema"`

**Test checkpoint:** `bash scripts/test-tooling.sh` runs `tools/boot-cert/lint.py` (exit 0: 29 rows, 29 TODOs covered) + `test_lint.py` (18 assertions, each asserting the SPECIFIC diagnostic: coverage gap, evidence-less required row, required_for-subset, undeclared row platform, undeclared tier, duplicate YAML key incl. inline merge-source, cyclic merge alias, duplicate JSON key, out-of-matrix result platform, deeply-nested JSON, empty + present-but-empty `--results`; merge+override loads; result JSON valid/invalid).

> **Test runner:** `bash scripts/test-tooling.sh` | boot-cert lint + self-test (3 aggregate assertions; 398/398 tooling tests PASS as of §2) -- host-side certification harness, no kernel `TEST_CAT_*` surface

> **Notes:**
> - **What shipped** -- `tools/boot-cert/`: `boot-cert.yml` (29-row matrix, every boot-platform TODO), `boot-cert.schema.json` + `result.schema.json` (draft 2020-12), `lint.py` (release gate), `test_lint.py` (6-assertion self-test).
> - **How it runs** -- `scripts/test-tooling.sh` invokes `lint.py --quiet` (exit non-zero on schema/coverage/evidence failure) + `test_lint.py`; coverage set is the live `todo/01-boot-platform/TODO-NN-*.md` listing, never the cache.
> - **Downstream effects** -- §2-§8 emit results validated against `result.schema.json`; §9 release gate reads `required_for`/`source_artifact`; §11 firmware gate is the `firmware-tables` row.
> - **Canonical doc** -- [`tools/boot-cert/boot-cert.schema.json`](../../tools/boot-cert/boot-cert.schema.json) + the `boot-cert.yml` header.
> - **Scope boundary** -- §1 owns the schema + coverage/evidence lint; the per-suite runners that PRODUCE results are §2-§8; the release dashboard is §9.
> - **Codex** -- design 2x + adversarial/consistency/perf + re-adversarial x9 convergence; finding evidence in commit history.

> **Verified:** 2026-06-19 | this commit | 5/5 items | build OK | test_lint.py 18/18 + test-tooling 396/396 PASS
> **Quality reviewed:** 2026-06-19 | Codex 20x (design, adversarial, consistency, perf, re-adversarial) | 2H+13M fixed | scope: N/A (host-side Python tooling; no domain code-quality skill)

---

## 2. VM Automation Suite

- [x] Standardized launcher registry (`LAUNCHERS` in `boot_reliability.py`) for QEMU WHPX/TCG + VirtualBox + Hyper-V; `boot-test-qemu.sh` is the single-shot QEMU primitive (cold/warm NVRAM). WHPX/Hyper-V have no Linux runner (XREF below).
- [x] Capture serial + screenshot (HMP `screendump`) + exit status; BlackBox advertised per-launcher in `caps`; per-iteration artifacts under `--out` (`serial-NNN-{cold,warm}.log` / `screen-NNN-*.ppm`) recorded in each result's `logs[]`.
- [x] Timeout/hang detection: launcher polls captured serial each second; gate hard-walls each launcher at `timeout+30s` (`_run_once`), classified `fail` "boot timed out".
- [x] Test-suite filter + boot.conf patching via existing `scripts/patch-boot-conf.sh` (`--test-suite`, repeatable `--boot-conf-patch key=value`), applied on the cold-series boundary and reset after.
- [x] Boot-reliability gate `boot_reliability.py`: N cold + M warm reboots; `classify_boot` (fail/nonzero-exit beat PASS, missing milestone->degraded); flake decision via `aggregate_runs` (stable=0); incomplete runs rejected; schema-valid output.
- [x] `tools/boot-cert/reliability.schema.json` -- aggregate shape (cold/warm counts, flake_count/threshold, decision, classifier_version, iterations[]) for release-gate audit; result.schema.json (S1) stays the frozen single-run shape.
- [/] -> XREF: `15-installer-release/TODO-04-release-qa.md §2`/`§8` -- this gate is BOOT repeat-boot flake detection; whole-OS idle/crash soak stays release-QA
- [x] Commit: `"test: boot VM automation suite"`

**Test checkpoint:** `bash scripts/test-tooling.sh` runs `boot_reliability.py --self-check` (registry + both schemas load) + `test_boot_reliability.py` (63 assertions: classify fail-precedence/timeout/exit-code-authoritative/missing-prompt/ISO-no-prompt/missing-signal/ANSI; tier thresholds; aggregate stable-0/nightly-budget/skip-not-flake/flake+skip-fail; result + reliability schema conformance; incomplete-run invariants; registry + launcher-source consistency; plus mock-launcher orchestration: cold+warm bootstrap, all-skip + partial-skip are failures, exit-code-only path). Live launch (host with QEMU): `boot_reliability.py --platform qemu-tcg --tier stable --cold 3 --warm 2` boots `build/system-disk.img` 5x, each iteration captures serial + exit; a wedged boot trips the timeout->fail; `stable` red-lights on any single flake.

> **Test runner:** `bash scripts/test-tooling.sh` | boot-reliability self-check + 63-assertion self-test (398/398 tooling tests PASS) -- host-side certification harness, no kernel `TEST_CAT_*` surface

> **Notes:**
> - **What shipped** -- `boot_reliability.py` (registry + classify/aggregate/build + cold/warm driver), `reliability.schema.json`, `boot-test-qemu.sh` (single-shot QEMU launcher w/ cold/warm NVRAM), `test_boot_reliability.py` (63 assertions).
> - **How it runs** -- `boot_reliability.py --platform <pc> --tier <t> --cold N --warm M` drives the registry launcher N+M times, classifies each captured serial, writes `reliability.json` + `results.json`; `test-tooling.sh` runs the no-QEMU self-test.
> - **Downstream effects** -- §9 release gate consumes `decision`/`flake_count`/`flake_threshold`; §3-§6 suites drive their platform rows through this same gate; reuses `patch-boot-conf.sh` + `boot-test-{vbox,vhdx}.sh`.
> - **Canonical doc** -- [`tools/boot-cert/reliability.schema.json`](../../tools/boot-cert/reliability.schema.json) + the `boot_reliability.py` module docstring.
> - **Scope boundary** -- §2 owns VM launch + repeat-boot flake detection; the Windows-side WHPX/Hyper-V live runner is owned by TODO-06 (`boot-test-whpx.ps1`); whole-OS idle/crash soak is TODO-04 §2/§8; bare-metal lab is §7.
> - **Codex** -- design + test-coverage + adversarial-impl x4 + adversarial + re-adversarial x7 + consistency + perf, all converged clean; ~16 fail-open cert holes closed pre-commit + 4H in review (POST16 core gate, concurrent-disk flock, serial cap, disk-path canon). Per-finding evidence in commit history.

> **Verified:** 2026-06-19 | this commit | 6/7 items | build OK | 63-assertion self-test + 398/398 tooling PASS
> **Quality reviewed:** 2026-06-19 | Codex 16x (design, test-coverage, adversarial-impl, adversarial, re-adversarial, consistency, perf) | 4H fixed (POST16 core gate, concurrent-disk flock, serial cap, disk-path canon) | scope: N/A (host-side Python/bash tooling; no domain code-quality skill)

---

## 3. Storage/Media Boot Suite

> [!NOTE]
> Plan sharpened by Codex design review (2026-06-19). §3 is NOT host-tooling-only: it needs a kernel boot-path readback marker, typed QEMU storage topologies, per-format serial standardization, and a coverage schema. Deferred this pass (multi-commit + boot-path scope); the items below are the vetted implementation plan.

- [ ] `boot-test-qemu.sh` typed `--storage` enum (sata-ahci, nvme, usb-msc, usb2-slow, raw-disk) with fixed QEMU device-arg sets (current launcher is AHCI-only, so untyped runs would all exercise one path).
- [ ] Standardize `--serial-out` on `boot-test-{iso,vhdx,vbox}.sh` so the suite owns serial capture (today ISO writes serial to a private mktemp and deletes it).
- [ ] Kernel boot-device readback gate in `src/kernel/main/boot_hw.c`: reopen `HKLM\SYSTEM\Boot\Device`, verify populated values, emit a dedicated pass/fail serial marker so the suite validates registry PERSISTENCE, not the populate-attempt log. -> XREF boot-device-discovery owner.
- [ ] `tools/boot-cert/storage_suite.schema.json` -- coverage manifest enforcing the exact required storage-class set per row (result.schema.json stays the frozen single-run shape).
- [ ] `tools/boot-cert/storage_suite.py` -- STORAGE_CLASSES registry driving the §2 launchers/gate per class; validates boot-device readback marker + BlackBox (`X:\`) presence; USB 2.0 slow-media case; archives artifact manifest + partition map (sgdisk/parted); emits per-class `result.schema.json` + the coverage manifest.
- [ ] `tools/boot-cert/test_storage_suite.py` self-test (mock launchers, no live QEMU) wired into `scripts/test-tooling.sh`.
- [ ] Commit: `"test: boot storage media suite"`

**Test checkpoint:** each storage class (SATA/AHCI, NVMe, USB MSC, USB2-slow, ISO, raw disk, VHD/VHDX, VDI) boots via its typed launcher and archives a boot-device readback-marker PASS + BlackBox log + artifact manifest + partition map; the coverage manifest fails if any required class is missing; `storage_suite.py --self-check` + `test_storage_suite.py` pass under `scripts/test-tooling.sh`.

> **Deferred:** [H] Storage suite needs a kernel boot-device readback serial marker (boot-path work + smoke-test/kernel-quality gates), typed `--storage` QEMU topologies, per-format `--serial-out` standardization, and a coverage schema -- a multi-commit effort beyond this pass; Codex-vetted plan captured in the checklist above. -> XREF: 01-boot-platform/TODO-06-boot-media-image-installer-handoff.md §9 (item: "QEMU VHDX boot (KVM/TCG): delegates to scripts/release/boot-test-vhdx.sh") for the per-format launcher/serial standardization.

---

## 4. Security Boot Suite

- [ ] Test Secure Boot off/on, signed/unsigned bootloader, dbx mismatch, SBAT generation.
- [ ] Test TPM present/absent, PCR replay, entropy sources, and manifest validation.
- [ ] Capture attestation and entropy reports.
- [ ] Fail release gate on insecure unexpected success.
- [ ] Commit: `"test: boot security suite"`

**Test checkpoint:** Secure Boot off/on, signed/unsigned bootloader, dbx mismatch, SBAT generation, and TPM present/absent each produce the expected pass/fail with attestation + entropy reports captured; an insecure unexpected-success fails the release gate.

> **Deferred:** [H] Security suite needs the §3 storage-suite driver foundation (deferred) PLUS Secure Boot signing infra (signed/unsigned/dbx/SBAT artifacts) and TPM test infra -- crypto/signing infrastructure not yet available this pass. -> XREF: 01-boot-platform/TODO-28-boot-validation-certification-matrix.md §3 (item: "tools/boot-cert/storage_suite.py" -- shared suite-driver foundation); 01-boot-platform/TODO-02-uefi-hardening-secureboot.md (Secure Boot / dbx / SBAT signing); 01-boot-platform/TODO-13-tpm-measured-boot.md (TPM present/absent + PCR replay).

---

## 5. Recovery and Rollback Suite

- [ ] Test A/B success, failed slot rollback, recovery partition fallback, watchdog reboot, hibernation failure fallback.
- [ ] Assert reason codes and counters.
- [ ] Verify no infinite loops.
- [ ] Capture VPD/recovery screenshots.
- [ ] Commit: `"test: boot recovery rollback suite"`

**Test checkpoint:** A/B success, failed-slot rollback, recovery-partition fallback, watchdog reboot, and hibernation-failure fallback each hit the expected reason code + counter with no infinite loop; VPD/recovery screenshots captured.

> **Deferred:** [H] Recovery suite needs the §3 storage-suite driver foundation (deferred) PLUS the A/B-slot, recovery-partition, watchdog, and hibernation runtime owned by the recovery/dual-slot/watchdog domains. -> XREF: 01-boot-platform/TODO-28-boot-validation-certification-matrix.md §3 (item: "tools/boot-cert/storage_suite.py" -- shared suite-driver foundation); 01-boot-platform/TODO-21-ab-slot-dual-boot.md, TODO-22, TODO-23 (A/B + recovery + watchdog runtime).

---

## 6. Network Boot Suite

- [ ] Start local DHCP/TFTP/HTTP fixtures.
- [ ] Test PXE, HTTP Boot, signed manifest, missing file, bad hash, timeout, and local fallback.
- [ ] Capture `network-boot.json`.
- [ ] Mark bare-metal PXE as manual unless lab supports automation.
- [ ] Commit: `"test: network boot certification suite"`

**Test checkpoint:** with local DHCP/TFTP/HTTP fixtures, PXE + HTTP Boot + signed-manifest + missing-file + bad-hash + timeout + local-fallback each produce the expected result and capture `network-boot.json`; bare-metal PXE flagged manual when the lab lacks automation.

> **Deferred:** [H] Network-boot suite needs the §3 storage-suite driver foundation (deferred) PLUS the UEFI PXE/HTTP-Boot client runtime + local DHCP/TFTP/HTTP fixtures owned by the network-boot domain. -> XREF: 01-boot-platform/TODO-28-boot-validation-certification-matrix.md §3 (item: "tools/boot-cert/storage_suite.py" -- shared suite-driver foundation); 01-boot-platform/TODO-25-uefi-network-boot.md (PXE/HTTP Boot client + manifest verification).

---

## 7. Bare-Metal Lab Inventory

- [x] `docs/hardware/boot-lab.md` per-machine inventory schema (CPU, chipset, firmware vendor/version, GPU, storage controller, USB controller, TPM, Secure Boot) + a reference Haswell-laptop block.
- [x] 5 certification classes (desktop-SATA, laptop-NVMe, USB-only, SecureBoot+TPM, no-TPM) mapped to the `boot-cert.yml` `baremetal-*` platform classes + `boot_device_type` values.
- [x] Manual run template (build/machine/class/result/artifacts) + per-run evidence checklist (serial, screenshot, BlackBox, firmware report, signed-image + PCR checks).
- [x] Firmware-quirk tracking links back to `TODO-04 §9` (Firmware Quirk Database) feeding the §11 firmware-sanity gate.
- [x] Commit: `"docs: boot bare-metal lab inventory"`

**Test checkpoint:** `docs/hardware/boot-lab.md` lists the 5 certification classes (desktop SATA, laptop NVMe, USB-only, Secure Boot+TPM, no-TPM) with per-machine inventory + a manual run template + evidence checklist; firmware quirks link back to TODO-04 §9.

> **Test runner:** N/A (docs-only) | validation: `docs/hardware/boot-lab.md` lists the 5 classes mapped to `boot-cert.yml` baremetal-* + run template + evidence checklist + TODO-04 §9 quirk XREF

> **Notes:**
> - **What shipped** -- `docs/hardware/boot-lab.md`: the manual-evidence half of the boot cert matrix (per-machine inventory schema, 5 cert classes, run template, per-run evidence checklist, firmware-quirk tracking).
> - **Structure / consumers** -- the §9 release gate consumes per-run `result`; the §8 support bundle packages the named artifacts; the 5 classes mirror `tools/boot-cert/boot-cert.yml` `baremetal-*` + `boot_device_type` (boot-info-fields.md).
> - **Canonical doc** -- [`docs/hardware/boot-lab.md`](../../docs/hardware/boot-lab.md).
> - **Scope boundary** -- §7 is the manual bare-metal evidence inventory; firmware quirk registry is TODO-04 §9; the firmware-sanity gate that consumes it is §11; automated VM suites are §2-§6.

> **Verified:** 2026-06-19 | this commit | 4/4 items | build OK | docs-only (concrete run example validates vs result.schema.json; 5 classes match boot-cert.yml baremetal-* enum)
> **Quality reviewed:** 2026-06-19 | Codex 6x (adversarial-impl, adversarial, consistency, perf, re-adversarial) | 5M fixed | scope: N/A (docs-only)

---

## 8. Boot Support Bundle Collector

> [!NOTE]
> Plan sharpened by Codex design review (2026-06-19). §8 is a SECURITY-bearing collector (a shared bundle must leak nothing), so the design below is the vetted plan. Deferred this pass (depends on §2-§7 outputs + multi-commit redaction effort).

- [ ] `tools/boot-cert/support_bundle.py`: discover serial logs + screenshots + reliability.json/results.json (§2) + BlackBox `X:\` export + firmware report + boot timeline from a `--in` dir.
- [ ] Share-safe by DEFAULT: redact serials/secrets (SMBIOS/disk serial, NVMe EUI-64, UEFI key bytes, token-like blobs) in TEXT/JSON; QUARANTINE/omit opaque binaries (BlackBox blobs, screenshots) by default; `--private` opt-in for raw; manifest marks each omitted/private artifact + why.
- [ ] Fail-closed validation: reject when `reliability.json`/`results.json` fail their schemas OR build/machine/tier/platform identities disagree with the CLI inputs; warn-only for optional missing artifacts (recorded in the manifest).
- [ ] Tamper-evidence: in-bundle `bundle-manifest.json` (per-file sha256 + expected/omitted sections) PLUS an out-of-band top-level bundle digest printed to stdout; reproducible zip (sorted entries, fixed mtime) asserted byte-identical in the self-test.
- [ ] `tools/boot-cert/test_support_bundle.py` self-test (mock `--in` dir, no live QEMU) wired into `scripts/test-tooling.sh`.
- [ ] Commit: `"tools: boot support bundle collector"`

**Test checkpoint:** `support_bundle.py --in <dir> --build-id B --machine-id M` produces a share-safe `boot-support-B-M.zip` (redacted text/JSON, opaque binaries omitted unless `--private`) + `bundle-manifest.json` with per-file sha256 + an out-of-band digest; invalid/identity-mismatched result JSON fails closed; `test_support_bundle.py` asserts redaction + fail-closed + reproducible bytes.

> **Deferred:** [H] Support-bundle collector is a security-bearing share-safe/redaction effort (binary quarantine + private opt-in + fail-closed schema/identity validation + out-of-band tamper digest + reproducible zip) and consumes §2-§7 outputs (§3-§6 deferred) -- a multi-commit effort beyond this pass; Codex-vetted plan above. -> XREF: 01-boot-platform/TODO-28-boot-validation-certification-matrix.md §3 (item: "tools/boot-cert/storage_suite.py" -- produces packaged inputs); 01-boot-platform/TODO-24-blackbox-service-partition.md (`X:\` BlackBox exports the bundle packages).

---

## 9. Release Gate and Dashboard

- [ ] Generate `build/reports/boot-cert.html`.
- [ ] Define required gates for nightly, release candidate, and stable.
- [ ] Fail release packaging when required gates fail.
- [ ] Keep history so regressions are visible.
- [ ] Commit: `"ci: boot certification release gate"`

**Test checkpoint:** `build/reports/boot-cert.html` is generated from result JSON; a required-gate failure (per nightly / RC / stable tier) fails release packaging; prior-run history is retained so regressions are visible.

> **Deferred:** [H] Release gate + dashboard consumes §1-§8 (the per-suite result JSON + support bundle); §3-§6 + §8 are deferred, so the gate has no complete result set to aggregate yet. Implement after the suites produce results. -> XREF: 01-boot-platform/TODO-28-boot-validation-certification-matrix.md §8 (item: "tools/boot-cert/support_bundle.py" -- bundle + manifest the dashboard links); §1 boot-cert.yml `required_for` drives the per-tier gate.

---

## 10. Certification Docs

- [ ] Add `docs/testing/boot-certification.md`.
- [ ] Explain how to run VM and bare-metal certification.
- [ ] Link common failure signatures to TODO owners.
- [ ] Include support-bundle upload instructions.
- [ ] Commit: `"docs: boot certification guide"`

**Test checkpoint:** `docs/testing/boot-certification.md` documents running VM + bare-metal certification, maps common failure signatures to owner TODOs, and gives support-bundle upload instructions.

> **Deferred:** [M] Certification guide caps §1-§9 (it documents running every suite + the release gate + support-bundle upload); writing the final guide while §3-§9 are deferred would document unshipped surface. Write once the suites + gate land. -> XREF: 01-boot-platform/TODO-28-boot-validation-certification-matrix.md §9 (item: "Generate build/reports/boot-cert.html" -- the gate the guide explains); §8 support-bundle upload instructions.

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

> **Deferred:** [M] Firmware-sanity gate consumes TODO-04 `firmware-tables.json` (producer = TODO-04 §2/§8) + the §9 release-gate pattern + the §8 support bundle for report capture; the producer artifact + §8/§9 are not yet shipped this pass. TODO-04 §9 Firmware Quirk Database is `[x]`, but the `firmware-tables.json` decoder/report it needs is TODO-04 §2/§8 (still open). -> XREF: 01-boot-platform/TODO-04-firmware-table-platform-inventory.md §2/§8 (firmware-tables.json producer); 01-boot-platform/TODO-28-boot-validation-certification-matrix.md §9 (release-gate consumer).

---

## OS Comparison

| ⭐  | Feature                  | 🪟 Win11              | 🐧 Linux                 | 🚀 Impossible OS                |
| --- | ------------------------ | --------------------- | ------------------------ | ------------------------------- |
| 💎  | WHQL-style boot matrix   | ✅ WHQL/HLK           | ⚠️ per-distro CI          | ⬜ §1 cert matrix + gate        |
| 💎  | VM boot automation       | ⚠️ internal only       | ⚠️ per-distro tests       | ✅ §2 4-class launcher registry |
| ⭐  | BlackBox support bundle  | ⚠️ WER/event logs      | ⚠️ journal/sosreport      | ⬜ §8 one-zip bundle            |
| ⭐  | TODO-owner mapped matrix | ❌ internal, opaque   | ❌ ad hoc                | ✅ §1 boot-cert.yml + lint gate |
| 💎  | Firmware sanity gate     | ✅ HLK firmware tests | ✅ FWTS (ACPI/UEFI)      | ⬜ §11 consumes T04 inventory   |
| 💎  | Bare-metal cert lab      | ✅ WHQL/HLK lab       | ⚠️ per-distro HW labs     | ✅ §7 5-class lab inventory     |
| 💎  | Repeat-boot reliability  | ✅ HLK MTBF           | ⚠️ KernelCI boot-to-shell | ✅ §2 cold/warm flake gate      |

---

## Unit Tests

> Host-side certification-harness tests (schema + JSON + redaction), not kernel `TEST_CAT_*`. Wire as a `tools/boot-cert/` test target run by the §9 release gate.

- [ ] `test_boot_cert_schema_valid` -- `boot-cert.yml` validates against the schema
- [ ] `test_boot_cert_every_todo_has_gate` -- every TODO-01..28 maps to >=1 row
- [ ] `test_boot_result_json_parse` -- result JSON round-trips pass/fail/skip/degraded
- [x] `test_boot_reliability.py` (35 assertions, §2) -- classify_boot precedence, flake aggregation, reliability/result schema conformance, incomplete-run guard
- [ ] `test_support_bundle_redaction` -- serials/secrets absent from the bundle

---

## Verification

> **Test runner:** N/A (host-side certification harness; no kernel `TEST_CAT_*` surface) | validation: `tools/boot-cert/` schema+JSON+redaction tests run by the §9 release gate; VM/bare-metal evidence bundles per §2-§8

- [ ] QEMU WHPX and TCG automated run
- [ ] VirtualBox automated or semi-automated run
- [ ] Hyper-V VHDX run
- [ ] Bare-metal manual evidence bundle (manual -- run on lab hardware)

