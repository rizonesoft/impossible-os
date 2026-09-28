<!-- docs: covers=todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md sources=tools/boot-cert/boot-cert.yml,tools/boot-cert/boot-cert.schema.json,tools/boot-cert/result.schema.json,tools/boot-cert/reliability.schema.json,tools/boot-cert/lint.py,tools/boot-cert/boot_reliability.py,tools/boot-cert/test_lint.py,tools/boot-cert/test_boot_reliability.py,scripts/machines/boot-test-qemu.sh,scripts/test-smoke-matrix.sh reviewed=2026-09-28 order=28 -->
# Boot Validation and Certification Matrix

## What is it?

The certification matrix answers one question: is this build boot-certified across QEMU, VirtualBox, Hyper-V and real hardware? It is a schema-checked table (`tools/boot-cert/boot-cert.yml`) that maps every boot-platform roadmap file to at least one certification row, a result format that automated suites and manual bare-metal runs both use, and a lint that fails when the matrix, a row or a result is malformed or incomplete.

The schemas, the lint and the automated reboot-reliability suite ship and run in the tooling tests. The storage, security, recovery, network-boot and firmware-sanity suites that would fill the matrix with real results are open, as is the release dashboard, so today the matrix proves its own shape and coverage rather than certifying a release.

## How does it work?

`boot-cert.yml` has 29 rows, nine platform classes (QEMU and VM classes plus five bare-metal classes) and three tiers (`nightly`, `rc`, `stable`). Each row names its owning roadmap file, the platforms it runs on, the tiers that require it, how automated it is, and the artifact the release gate reads.

`lint.py` validates the file against `boot-cert.schema.json` and then checks two rules a schema cannot: a row required by any tier must name its evidence, and every boot-platform roadmap file must have at least one row. It reads the roadmap file list from the tree on each run, so a new roadmap file without a row fails immediately. A single run's result must match `result.schema.json` (row, status of pass, fail, skip or degraded, build, machine, tier, platform class, timestamp, optional logs and reason).

The automated suite is `boot_reliability.py`. It keeps a launcher per platform class: `qemu-tcg` uses `scripts/machines/boot-test-qemu.sh`, `virtualbox` uses `scripts/release/boot-test-vbox.sh`, and `qemu-whpx` and `hyperv` have no Linux-host launcher yet. Every boot is a fresh VM launch (QEMU runs with `-no-reboot`), so no guest-initiated reboot is exercised. On QEMU, a cold boot starts from fresh firmware variables and the warm series shares one variables file, seeded fresh on its first boot and reused by the later ones. VirtualBox has no cold and warm distinction: its launcher creates and destroys a throwaway VM on every run. It runs a number of cold and warm boots, classifies each captured serial log, and reduces the series to a decision against a per-tier flake threshold (one flake for `nightly` and `rc`, none for `stable`). The aggregate matches `reliability.schema.json`. Its `--self-check` mode validates the launcher table and schemas without booting anything, which is what the tooling suite runs.

The manual half is the [Bare-Metal Boot Lab](../hardware/boot-lab.md): five hardware classes mapped to the bare-metal platform classes, a per-machine inventory, and a run record in the `result.schema.json` shape.

`scripts/test-smoke-matrix.sh` is related but separate. It is the four-leg smoke gate (KVM and TCG, one and two CPUs) run for boot-path changes; it does not produce matrix results.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `boot-cert.yml` | The certification matrix ([`boot-cert.yml`](../../tools/boot-cert/boot-cert.yml)) |
| `boot-cert.schema.json` | Schema for the matrix and its rows ([`boot-cert.schema.json`](../../tools/boot-cert/boot-cert.schema.json)) |
| `result.schema.json` | Shape of one certification run, automated or manual ([`result.schema.json`](../../tools/boot-cert/result.schema.json)) |
| `reliability.schema.json` | Shape of a cold and warm boot series and its decision ([`reliability.schema.json`](../../tools/boot-cert/reliability.schema.json)) |
| `lint.py` | Schema, evidence and coverage checks ([`lint.py`](../../tools/boot-cert/lint.py)) |
| `boot_reliability.py` | Reboot-reliability driver, classifier and aggregator ([`boot_reliability.py`](../../tools/boot-cert/boot_reliability.py)) |
| `boot-test-qemu.sh` | One QEMU boot with `--disk`, `--timeout`, `--accel`, `--vars`, `--serial-out` and `--screenshot` ([`boot-test-qemu.sh`](../../scripts/machines/boot-test-qemu.sh)) |
| `test-smoke-matrix.sh` | Separate four-leg boot smoke gate ([`test-smoke-matrix.sh`](../../scripts/test-smoke-matrix.sh)) |

## How do I use it?

```bash
python3 tools/boot-cert/lint.py
python3 tools/boot-cert/boot_reliability.py --self-check --platform qemu-tcg --tier stable
python3 tools/boot-cert/test_boot_reliability.py
```

The self-check prints `boot-reliability self-check OK`. `scripts/test-tooling.sh` runs all three and reports `boot-cert lint passes (schema + coverage + evidence)` among its checks.

On a host with QEMU, a live reliability run boots the image repeatedly:

```bash
python3 tools/boot-cert/boot_reliability.py --platform qemu-tcg --tier stable --row-id boot-cert-matrix \
    --build-id <build> --machine-id <host> --cold 3 --warm 2
```

Pass `--row-id` with a row that exists in `boot-cert.yml`: the default, `boot-reliability`, is not a matrix row, so `python3 tools/boot-cert/lint.py --results <run-dir>/results.json` rejects results written with it.

For the separate smoke gate, `bash scripts/test-smoke-matrix.sh` prints one line per leg and ends with `SMOKE MATRIX PASSED` or `SMOKE MATRIX FAILED`.

## What is not implemented yet?

- Storage and media boot suite: [Storage/Media Boot Suite](../../todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md#3-storagemedia-boot-suite).
- Security boot suite (Secure Boot on and off, signed and unsigned, dbx, SBAT, TPM present and absent): [Security Boot Suite](../../todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md#4-security-boot-suite).
- Recovery and rollback suite (A/B rollback, recovery fallback, watchdog reboot, hibernation fallback): [Recovery and Rollback Suite](../../todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md#5-recovery-and-rollback-suite).
- Network boot suite (PXE, HTTP Boot, local fixtures): [Network Boot Suite](../../todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md#6-network-boot-suite).
- A share-safe boot support bundle with redaction: [Boot Support Bundle Collector](../../todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md#8-boot-support-bundle-collector).
- The release gate and dashboard that consume results: [Release Gate and Dashboard](../../todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md#9-release-gate-and-dashboard).
- An operator guide for certification runs: [Certification Docs](../../todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md#10-certification-docs).
- A firmware-sanity gate that fails a release on degraded firmware tables: [Firmware Sanity Certification Gate](../../todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md#11-firmware-sanity-certification-gate).
- A pixel check on captured error screens, so a screen drawn in the wrong place fails: [Error-Screen Pixel Oracle](../../todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md#12-error-screen-pixel-oracle----assert-what-was-drawn-not-what-the-loader-says-it-drew).
- Only QEMU warm boots after the first reuse firmware NVRAM, VirtualBox reuses none, and a guest-initiated reboot (reset path, device state across reset) is not tested: [VM Automation Suite](../../todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md#2-vm-automation-suite).
- WHPX and Hyper-V have no launcher on a Linux host; they wait on the Windows-side runner: [CI Boot Matrix for Every Artifact](../../todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md#9-ci-boot-matrix-for-every-artifact).

## How does it compare with Windows 11 and Linux?

Windows 11 hardware certification runs through the Windows Hardware Lab Kit, a closed test suite vendors run against published requirements, including reliability testing. Linux has no single equivalent: each distribution runs its own CI, and the Firmware Test Suite (FWTS) checks ACPI and UEFI tables separately. Impossible OS has an open, roadmap-mapped matrix and a repeat-boot flake gate, but without the remaining suites and the release gate it does not yet certify a build end to end.

## See also

- [Boot Validation and Certification Matrix roadmap](../../todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md)
- [Bare-Metal Boot Lab](../hardware/boot-lab.md), the manual deep dive
- [A/B Dual-Slot Boot and Automatic Rollback](ab-boot-rollback.md)
- [Network Boot (PXE and HTTP)](network-boot.md)
