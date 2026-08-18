---
schema_version: 1
id: tpm-measured-boot-attestation
domain: 01-boot-platform
status: active
title: "TODO-13 -- TPM Measured Boot, PCR Replay & Attestation"
---

# TODO-13 -- TPM Measured Boot, PCR Replay & Attestation

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Complete the measured-boot trust chain from firmware through bootloader and kernel. The bootloader retrieves the TCG event log and the kernel parses it, but true integrity requires PCR replay, TPM2 PCR reads, baseline enrollment, sealed storage, and diagnostics. This TODO turns the current TPM presence/event-count code into a full boot integrity feature.
> **Current state (2026-06-12):** `retrieve_tpm_event_log()` copies the event log into `boot_info`, `tpm_init()` parses enough to count events and identify hash algorithms, and `tpm_integrity_init()` is a stub that reports `BOOT_INTEGRITY_NO_CRYPTO`. §2 shipped the TPM2 command transport (`tpm2_submit()` in `src/kernel/tpm_transport.c`: TIS/CRB, ACPI TPM2 discovery, conditional Startup, Phase 1 init). Still missing: PCR read (§3), PCR replay (§4), baseline enrollment (§6), TPM NV storage (§7), attestation report (§9).

## Inputs

- [`include/kernel/tpm.h`](../../include/kernel/tpm.h)
- [`src/kernel/tpm.c`](../../src/kernel/tpm.c)
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- -> XREF: `TODO-02-uefi-hardening-secureboot.md §3-§6` -- Secure Boot state and signed bootloader
- -> XREF: `TODO-01-boot-protocol-abi-handoff.md §4` -- TPM event log as typed boot payload
- -> XREF: `../02-kernel-core/TODO-03-kernel-libraries.md §3` -- cryptographic primitives/CSPRNG dependency
- -> XREF: `../02-kernel-core/TODO-19-code-integrity-trust-policy.md` -- image trust policy consumes measured boot
- -> XREF: `TODO-07-boot-entry-store-menu-policy.md §3, §9` -- §9 attestation export carries the boot-entry selected id and selection-reason code from TODO-07 §3

## Outcome

- TPM 1.2/2.0 event logs are parsed robustly and preserved for diagnostics.
- TPM2 PCR values can be read and compared against replayed event-log values.
- First-boot baseline enrollment stores golden measurements securely.
- Boot integrity status is available in Registry, BlackBox, system settings, and policy gates.
- Disk unlock, code integrity, and recovery can use measured-boot state.

## Implementation Order

| ⭐  | Order | Deliverable                                                   | Depends On                                                | Status |
| --- | :---: | ------------------------------------------------------------- | --------------------------------------------------------- | :----: |
| 💎  |   1   | Harden TCG event-log parser                                   | --                                                        |  [/]   |
| 💎  |   2   | TPM2 command transport                                        | §1 (ordering-only; transport does not consume the parser) |  [x]   |
| 💎  |   3   | PCR read API                                                  | §2                                                        |  [x]   |
| 💎  |   4   | PCR replay engine                                             | §1, ../02-kernel-core/TODO-03 §3                          |  [x]   |
| 💎  |   5   | Secure Boot variable measurement reconciliation (structural)  | §1, TODO-02 §3                                            |  [x]   |
| 💎  |   6   | Baseline enrollment and storage                               | §3, §4, §7                                                |  [/]   |
| 💎  |   7   | TPM NV index support                                          | §2 (transport); §12 (baseline mask)                       |  [/]   |
| ⭐  |   8   | Sealed-secret boot policy hooks                               | §7, §12                                                   |  [x]   |
| 💎  |   9   | Attestation report export                                     | §3-§6, §12, §13                                           |  [/]   |
| ⭐  |  10   | Recovery and mismatch UX                                      | §6, TODO-22                                               |  [/]   |
| 💎  |  11   | TPM tests and event-log fixtures                              | §1-§10, §12, §13                                          |  [x]   |
| 💎  |  12   | PCR allocation table and policy masks                         | (foundational; consumed by §6/§8/§13)                     |  [x]   |
| 💎  |  13   | Attestation key provisioning and TPM2 quote                   | §3, §7, §12                                               |  [x]   |
| 💎  |  14   | Post-ship follow-up backfill (2026-07-31 cohort)              | --                                                        |  [x]   |
| 💎  |  15   | Trusted enrollment provenance                                 | §6, §14                                                   |  [/]   |
| 💎  |  16   | Baseline ABI-manifest identity (populate reserved digest)     | §6, §14                                                   |  [x]   |
| 💎  |  17   | Write-locked and monotonic NV index primitives                | §6, §7, §14                                               |  [x]   |
| 💎  |  18   | Atomic boot-integrity report publication                      | §6, §12, §14                                              |  [x]   |
| 💎  |  19   | Versioned baseline growth and NV index migration              | §16, §21, §27, §28                                        |  [ ]   |
| 💎  |  20   | BOOTX64.EFI on-disk self-measurement                          | §16, TODO-01 (boot_info ABI)                              |  [/]   |
| 💎  |  21   | Authenticated NV index lifecycle and index identity           | §17, §6, §15                                              |  [/]   |
| 💎  |  22   | Bootloader-side NV floor read (EFI_TCG2 adapter)              | §27, §28, TODO-21 §3 (selection)                          |  [ ]   |
| 💎  |  23   | Headless enrollment authorization escape hatch                | §21, §27, §15                                             |  [ ]   |
| 💎  |  24   | Bounded sequence + verified teardown for seal and attestation | §17, §8, §13                                              |  [ ]   |
| 💎  |  25   | Field-level baseline mismatch attribution + status scoping    | §16, §18, §19                                             |  [ ]   |
| 💎  |  26   | Baseline-blob NV fake: wrapper-level verify coverage          | §11, §18, §6                                              |  [ ]   |
| 💎  |  27   | Authorized NV record transitions                              | §21, §15, §17                                             |  [x]   |
| 💎  |  28   | Crash-consistent record pairing, floor APIs, read budget      | §27, §17, §6                                              |  [ ]   |

## 1. Harden TCG Event-Log Parser

- [x] Parse TPM 1.2 + TPM 2.0 crypto-agile logs with unaligned-safe byte loads (`tpm_le16`/`tpm_le32`); pure `tpm_evlog_parse()` in `tpm.c`, no pointer-cast reads.
- [x] Preserve per-event metadata (PCR index, type, digest count, primary digest off/len/alg, payload off/size) in `struct tpm_event` (offset-based into the retained log, no copy).
- [x] Reject truncation/corruption with `tpm_evlog_status_t` + exact `fail_offset`; partial-tail, cap-overflow, and unsupported-alg are distinct; event count stays 0 on any non-OK status.
- [x] Export `X:\Diag\tpm-events.json` as a TCG CEL-JSON subset via a streaming writer (`tpm_evlog_export_cel`, one bounded `vfs_write` per event), hooked post-mount in `boot_desktop.c` (parse runs Phase 0, before the filesystem).
- [x] Bootloader copies the EXACT log (`tpm_compute_log_size`, not the `+256` estimate) or degrades `BOOT_CAP_TPM_EVENT_LOG`; retention via the legacy `tpm_event_log` boot_reserved path (no descriptor: would double-reserve + fatal).
- [x] Bound the bootloader TPM read by the UEFI memory-map descriptor ceiling (`tpm_log_mmap_extent()`); copy only the record-derived `tpm_compute_log_size()` length, else degrade via `tpm_publish_log_degraded()`.
- [ ] Validate the record chain reaches `log_last_entry` exactly before the bootloader copy (a forward bogus in-descriptor LastEntry can retain gap bytes); kernel `tpm_evlog_parse()` validates today.
- [x] Commit: `"tpm: harden measured boot event log parser"`

**Test checkpoint:** Kernel unit fixtures parse a TPM 1.2 SHA-1 log and a TPM 2.0 crypto-agile (multi-bank) log with zero unaligned reads; a truncated log is rejected with an explicit byte offset + status code (not a silent stop); `X:\Diag\tpm-events.json` lists per-event PCR index / type / digest count; the page-aligned bootloader copy is retained by the legacy `tpm_event_log` boot_reserved reservation (no double-reserve fatal at boot). Platforms: kernel unit tests (fixtures) + QEMU swtpm KVM smoke; bare metal (test laptop fTPM).

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 9 tpm-evlog suites in `test_tpm_event_log.c`, 0 failures

> **Notes:**
> - **What shipped:** pure `tpm_evlog_parse()` (unaligned-safe byte loads, per-event `struct tpm_event` metadata, structured status + fail offset) + streaming CEL-JSON exporter; bootloader `tpm_compute_log_size()` exact-size copy or cap-degrade.
> - **How it integrates:** parse runs Phase 0 from `tpm_init` (which wraps the pure walker); CEL export runs post-mount from `boot_desktop.c`; the log is retained by the legacy `tpm_event_log` boot_reserved reservation.
> - **Downstream:** §4 PCR replay + §9 attestation/CEL consume the preserved metadata + offsets. Codex 4x (design + adversarial + re-adversarial) adoptions in the commit message.
> - **Canonical doc:** `include/kernel/tpm.h` (parse contract); TCG CEL-JSON for the export format.
> - **Scope boundary:** the UEFI mem-map-extent read bound shipped (descriptor ceiling + record-derived copy); the open `[ ]` item is proving the record chain reaches `log_last_entry` (kernel `tpm_evlog_parse()` validates today). Retention is legacy-fields-only.

> **Verified:** 2026-06-14 | commit `53e24f76` (impl) + read-bound (mmap-descriptor ceiling + record-derived copy + degrade contract) | 6/7 items | build OK | 394 kernel tests PASS + smoke PASS (KVM 2.580s)
> **Deferred:** [M] bootloader does not prove the event-log record chain reaches `log_last_entry` before the copy (a forward in-descriptor bogus LastEntry can retain gap bytes; kernel `tpm_evlog_parse()` is the authoritative validation) -> XREF: 01-boot-platform/TODO-13 §1 (item: "Validate the record chain reaches `log_last_entry` exactly before the bootloader copy")
> **Quality reviewed:** 2026-06-14 | Codex 11x (design, adversarial, re-adversarial, consistency, perf) | prior 1C+1H+3M + read-bound 2H+2M fixed, 1M deferred | scope: kernel-code-quality + boot-code-quality

---

## 2. TPM2 Command Transport

- [x] Discovery (`tpm_transport.c` `tpm_transport_init()`): ACPI TPM2 table start method 6/7, TIS fixed-base fallback, UC MMIO; methods 2/8 degrade -> XREF: `04-drivers-hardware/TODO-03` §1 (item: "TPM2 ACPI start method (2/8) via ACPICA").
- [x] Locality 0 held from init; TIS FIFO write AND read chunked by live burstCount with Expect/dataAvail checks; CRB cmdReady/START/goIdle handshake with size-capped mapped buffers.
- [x] PTP TIMEOUT_A-D TSC poll deadlines (iteration fallback when freq unknown); timeout = sticky transport failure, boot continues; Phase 1 init after `acpi_init()` per design review.
- [x] TPM2_Startup never perturbs firmware-owned state: GetCapability probe first, Startup(CLEAR) only on TPM_RC_INITIALIZE; vendor logged.
- [x] `tpm2_submit()` whole-transaction serialized (concurrent caller gets TPM_T_ERR_BUSY); response bounded by caller cap + TPM_T_MAX_RESPONSE; response tag vocabulary enforced.
- [x] Unblocked `01-boot-platform/TODO-12` §4 (TPM2_GetRandom) -- consumer note updated in the same commit.
- [x] Commit: `"tpm: add TPM2 command transport"`

**Test checkpoint:** With QEMU `-tpm` (swtpm) serial shows `TPM2 transport up (TIS|CRB, vendor XXXX)`; without a TPM the log shows `no TPM2 table and no firmware TPM; transport not started` and boot continues (verified live on KVM smoke, POST16 0x1055). Known TCG TPM-init freeze (`project_tcg_tpm_freeze`) must not regress: transport init is Phase 1, after the freeze point. QEMU WHPX, QEMU TCG, VirtualBox, bare metal (test laptop fTPM).

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 2 transport suites (marshaling + fake-TIS submit) in `test_tpm_transport.c`, 0 failures

> **Notes:**
> - Shipped `include/kernel/tpm_transport.h` + `src/kernel/tpm_transport.c` (TIS/FIFO burst-chunked + CRB, ACPI TPM2 discovery, conditional Startup) + Phase 1 wiring in `boot_interrupts.c` (POST16 0x1054/0x1055).
> - Runs once in Phase 1 after ACPI + boot timing; `tpm2_submit()` is the single public command path, whole-transaction serialized, sticky-failed on wedge.
> - Downstream: TODO-12 §4 (TPM RNG) consumes `tpm2_submit()`; §3 PCR read and §7 NV index build on the same API; Codex design + test-coverage adoptions in the section commit.
> - Canonical contract doc: `include/kernel/tpm_transport.h` header comment (phase + concurrency contracts).
> - Scope boundary: ACPI-start methods (2/8) degrade until ACPICA lands (owner: `04-drivers-hardware/TODO-03` §1); event-log parsing stays in `tpm.c` (§1); PCR commands are §3.

> **Verified:** 2026-06-12 | commit `8ae51bf1` | 6/6 items | build OK | smoke PASS (KVM 2.530s)
> **Accepted:** [M] ACPI start methods 2/8 degrade-with-WARN until an AML interpreter exists -> XREF: 04-drivers-hardware/TODO-03 §1 (item: "TPM2 ACPI start method (2/8)" at line 103)
> **Deferred:** [L] CRB submit path lacks a fake-buffer unit seam (validated live via swtpm checkpoint) -> XREF: 01-boot-platform/TODO-13 §11 (item: "Fake-CRB buffer seam + unit suite for crb_submit()" at line 107) (RESOLVED 2026-06-14 by §11 commit c40c4eee: `tpm_t_test_install_crb_buffers()` + `test_tpm_crb_submit_fake`)
> **Quality reviewed:** 2026-06-12 | Codex 9x (design, adversarial x2, test-coverage, consistency, perf, re-adversarial x3) | 1Crit+6H+6M fixed, 0 open, 1M+1L accepted-XREF | scope: kernel-code-quality

## 3. PCR Read API

- [x] SINGLE-PCR `tpm2_pcr_read` via `TPM2_CC_PCR_Read`; pure marshal/parse seam (fixture-tested) + `tpm2_submit` wrapper. Parser binds the reply to the requested `(alg, pcr_index)` so a desynced TPM can't cache a foreign digest.
- [x] SHA-1/256/384/512 banks; inactive bank -> `out_len == 0` via `tpm_pcr_status_t` (OK / INACTIVE / BADARG / TRANSPORT / BUSY, each distinct). Undersized `out_cap` is BADARG on both paths; transport contention is the retryable BUSY, not TRANSPORT.
- [x] PCR cache populated EAGERLY in Phase 1 (`tpm_pcr_cache_init`, BSP write-once, no lock); `tpm_pcr_get` reads lock-free, falls back to uncached `tpm2_pcr_read` for non-cached PCRs; PCR-0 probe reused. Wired in `boot_interrupts.c`.
- [x] `TPM_ALG_SHA1/256/384/512` + `tpm_alg_digest_len_pub()` moved from `tpm.c` to `include/kernel/tpm.h` (public parser/`tpm_pcr_get` inputs).
- [x] Commit: `"tpm: read PCR values"`

**Test checkpoint:** `tpm2_pcr_read(alg, pcr_index, out, out_cap, *out_len)` against QEMU swtpm returns the SHA-256 PCR0 value matching `swtpm`'s state; SHA-1/384/512 banks read where active, absent banks report INACTIVE (not error); a second `tpm_pcr_get(index, alg)` for a Phase-1-cached value hits the cache (no second TPM transaction). Unit fixtures (`test_tpm_transport.c`) cover marshal byte-layout + parse binding (wrong alg, wrong PCR bit, multi-selection, wrong digest size, undersized out_cap, inactive, malformed). Platforms: QEMU swtpm KVM; bare metal (test laptop fTPM).
> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 412 kernel + 16 user-mode, 0 failures
> **Notes:**
> - Codex design adoptions (single-PCR API, status enum, eager Phase-1 cache, public `TPM_ALG_*`) baked in pre-code; per-finding trail in the commit message.
> - Parser binds reply to requested `(alg, pcr_index)`; undersized buffer is BADARG on both paths; BUSY is a distinct retryable status; PCR-0 probe reused.
> - Wedge bound: a dead/absent TPM trips sticky `s_failed` on the first read, so the eager batch costs one timeout, not N.
> **Verified:** 2026-06-14 | commit `8a8685e8` | 4/4 items | build OK | tests 412 kernel + 16 user-mode PASS
> **Quality reviewed:** 2026-06-14 | Codex 6x (design + adversarial + consistency + perf + re-adversarial x2) | 1H+4M+1L fixed, 0 open | scope: kernel-code-quality

## 4. PCR Replay Engine

> **Unblocked (2026-06-14):** the SHA prerequisites are DONE. `src/kernel/crypto/` now ships SHA-1, SHA-256, and SHA-384 (NIST-KAT-validated, Codex-reviewed); SHA-512 remains available via monocypher. All TPM PCR replay banks are therefore covered. `09-desktop-shell/TODO-07 §1`'s `cng_sha256` can consume the kernel `sha256` rather than reimplement. Remaining §4 work (no longer blocked): the `pcr_extend` helper + the replay engine itself, consuming §1's parsed event log + §3's `tpm_pcr_get` hardware reads.

- [x] **Prerequisite:** kernel SHA-256 shipped -- `src/kernel/crypto/sha256.{c,h}` (FIPS 180-4), NIST KAT-validated; reusable by `cng_sha256`. -> XREF: 09-desktop-shell/TODO-07 §1 (item: `cng_sha256` primitive).
- [x] **Prerequisite:** kernel SHA-1 (legacy PCR bank) shipped -- `src/kernel/crypto/sha1.{c,h}` (FIPS 180-4), NIST KAT-validated (`test_sha1.c`).
- [x] **Prerequisite:** kernel SHA-384 shipped -- `src/kernel/crypto/sha384.{c,h}` (SHA-384 IVs over monocypher SHA-512, layout pinned by `_Static_assert`), NIST KAT-validated. All replay banks now available (SHA-1/256/384 + SHA-512).
- [x] **Prerequisite:** `tpm_pcr_extend(alg, pcr, digest)` shipped -- `src/kernel/tpm_replay.{c,h}`, `pcr := H_bank(pcr || digest)` dispatching on SHA-1/256/384/512; self-consistency + chaining + bad-arg tests (`test_tpm_replay.c`).
- [x] **Enabler:** `digests_off` added to `struct tpm_event` (`offset+12` for EVENT2; SHA-1 digest offset for legacy), populated at all 3 §1 record sites; replay iterates `TPML_DIGEST_VALUES` from there. Fixture-asserted in `test_tpm_event_log.c`.
- [x] Replay PCR extends from the parsed event log via `tpm_replay_pcr`/`_from` (`tpm_replay.c`): per-bank digest extract (legacy raw SHA-1 + EVENT2 list, bounded), skip EV_NO_ACTION, extend in log order. Known-vector + legacy + EV_NO_ACTION tests.
- [x] Compare replayed values against hardware PCR reads via `tpm_replay_verify` (replays measured PCRs 0-7/11, reads `tpm_pcr_get`, memcmp).
- [x] `TPM_REPLAY_TAMPER` (replay != hardware) distinct from `UNVERIFIABLE` (degraded log / incomplete coverage -- never false tamper or false verified). Baseline-vs-golden mismatch owned by enrollment. -> XREF: §6.
- [x] First mismatch in diagnostics: `tpm_replay_report.first_mismatch_pcr` (lowest mismatching PCR) + `mismatch_count`.
- [x] Commit: `"tpm: replay measured boot PCRs"`

**Test checkpoint:** On a clean boot, replayed PCR values (extended from the parsed event log, every active bank) equal the hardware PCR reads from §3; a known-vector unit test replays a fixed log to expected digests; injecting one tampered event-log entry flags `event-log tamper` distinctly from `baseline mismatch` and diagnostics name the exact first-mismatch PCR index + offset. Unit tests cover the SHA-1/256/384 NIST KATs, `pcr_extend`, the replay known-vector (== independent H-chain) + legacy + EV_NO_ACTION cases, and the verify/finalize verdict logic; the live clean-boot replay==hardware comparison is QEMU-swtpm/bare-metal validation. Platforms: kernel unit tests + QEMU swtpm KVM; bare metal.
> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 530 kernel + 16 user-mode, 0 failures
> **Notes:**
> - Shipped the measured-boot PCR replay engine: `src/kernel/crypto/{sha256,sha1,sha384}.c` (FIPS 180-4) + `src/kernel/tpm_replay.c` (`tpm_pcr_extend`, the event-log replay walk, replay-vs-hardware verify) + the `digests_off` parser enabler.
> - Pure cores (KAT/known-vector/branch tested) with thin live wrappers: `tpm_replay_pcr`/`tpm_replay_verify` over `g_boot_info` + §3 `tpm_pcr_get`; handles legacy + EVENT2 log formats, skips EV_NO_ACTION, gates on a clean parse.
> - Verdict taxonomy: VERIFIED / `TPM_REPLAY_TAMPER` (replay != hardware) / `UNVERIFIABLE` (degraded log or incomplete coverage). Per-finding review trail in the piece commit messages.
> - Crypto trio also unblocks §6 image hashes + `09-desktop-shell/TODO-07 §1` `cng_sha256` (reuse, don't reimplement).
> - Scope boundary: §4 runs the replay verify at Phase 1 (`boot_interrupts.c`) + publishes the verdict to `boot_integrity_report.replay_verdict` (TAMPER escalates `overall_status`); §10 surfaces it in VPD/UI and §6 owns the baseline-vs-golden comparison.
> **Verified:** 2026-06-14 | commit `72aaf2c2` | 9/9 items | build OK | smoke PASS (KVM 2.64s) + tests 530/530
> **Quality reviewed:** 2026-06-14 | Codex 4x (adversarial + consistency + perf + re-adversarial) | 1H+2M fixed, 0 open | scope: kernel-code-quality

## 5. Secure Boot Variable Measurement Reconciliation

- [x] Decode EV_EFI_VARIABLE_DRIVER_CONFIG/AUTHORITY (PCR7) for PK/KEK/db/dbx/SecureBoot/SetupMode via the pure `uefi_var_data_parse` seam (TCG UEFI_VARIABLE_DATA, subtraction-form bounds) in `tpm_sb_reconcile.c`.
- [x] Byte-compare each measured DRIVER_CONFIG payload against the live UEFI variable (`uefi_get_variable`), tri-state per-variable live-read status; bounded 16 KiB scratch reports PATHOLOGY (never a false match) on oversize.
- [x] Flag impossible combinations via pure `sb_reconcile_classify` (enabled+no-PCR7 ONLY on a clean log, enabled+SetupMode, enabled+no-PK, state-unknown); a degraded/unavailable log is reported via `evlog_status`, never a false impossibility.
- [x] Feed results to a SEPARATE versioned `sb_reconcile_report` (own accessor); honest `secure_boot`/`secure_boot_valid` land in `boot_integrity_report` (NEVER sets BOOT_INTEGRITY_VERIFIED -- structural/unauthenticated until PCR replay).
- [x] Commit: `"tpm: reconcile Secure Boot measurements"`

**Test checkpoint:** `EV_EFI_VARIABLE_*` DRIVER_CONFIG payloads for PK/KEK/db/dbx/SecureBoot/SetupMode decode and byte-equal the live UEFI variables (`uefi_get_variable`); an injected "SecureBoot active + clean log + no PCR7 policy events" is flagged impossible while the SAME state with a degraded log is NOT; the reconciliation lands in the versioned `sb_reconcile_report` (marked unauthenticated) and the honest SB state in `boot_integrity_report`. Unit fixtures (`test_tpm_sb_reconcile.c`) cover the parser (valid, short, name/data overflow, uint64-wrap, >UINT32 data, cap-truncation) + the classify truth table. Platforms: QEMU swtpm + OVMF Secure Boot KVM; bare metal.
> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 442 kernel + 16 user-mode, 0 failures
> **Notes:**
> - `tpm_sb_reconcile.c`: pure `uefi_var_data_parse` + `sb_reconcile_classify` seam + Phase-1 BSP driver `tpm_secureboot_reconcile` (write-once report, lock-free read like `s_events`); wired after `tpm_pcr_cache_init` in `boot_interrupts.c`.
> - STRUCTURAL + UNAUTHENTICATED: byte-reconciles measured payloads vs live UEFI vars, never recomputes digests (no kernel SHA yet) and never sets BOOT_INTEGRITY_VERIFIED; the digest-honesty proof is owned by PCR replay.
> - Codex design + adversarial adoptions (unauthenticated framing, honest tri-state SB state, separate versioned report, clean-log gating, subtraction-form bounds, zero-length-measurement presence flag) -- per-finding trail in the commit messages.
> - Scope boundary: PCR replay (digest re-hash + hardware compare) is blocked on the absent SHA primitives; this section owns the structural diagnostic only.
> **Verified:** 2026-06-14 | commit `147ffd42` | 4/4 items | build OK | smoke PASS (KVM 2.67s) + tests 442/442
> **Quality reviewed:** 2026-06-14 | Codex 6x (design + adversarial + consistency + perf + re-adversarial) | 3H+4M fixed, 0 open | scope: kernel-code-quality

## 6. Baseline Enrollment and Storage

> **Design (2026-06-14, Codex 3H+1M adopted pre-code):** §6 is UNBLOCKED (§4 SHA/replay + §7 NV storage landed) and owns the baseline CONTENT. Adopted constraints for the implementor: (1) the baseline VERIFY is a Phase-1 step in `boot_interrupts` AFTER `tpm_transport_init`/`tpm_pcr_cache_init`/SB-reconcile/`tpm_replay_verify` -- NOT `tpm_integrity_init` (Phase 0, before the transport/cache exist; the `tpm.c` "read golden values" placeholder is misleading). (2) Store the golden baseline in an OWNER-auth DATA index (`tpm_nv_define_data`/`tpm_nv_write`/`tpm_nv_read`), readable every boot -- NEVER the PCR-policy-sealed index (circular: you'd need the good PCR state to read the values that define it). (3) Enrollment is gated by an explicit recovery-authorized `tpm_enroll` config bit, NOT silent first-boot TOFU (a wrong/compromised first boot must not auto-become golden). (4) `.bootproto` sha is the ABI-manifest hash, not the kernel image -- store available identity (per-bank PCR digests + SB state + firmware-version hash + `abi_manifest_sha256`); real image hashes + console confirmation + NV anti-rollback are tracked follow-ups below. -> XREF: §7 (`tpm_nv_*`; the UEFI-var -> TPM-NV migration item there is deferred to §6's baseline schema).

- [x] Versioned baseline blob (`struct tpm_baseline`, `tpm_baseline.{c,h}`): golden PCR digests {0-7,11} + SB state + firmware-version hash + `abi_manifest`(present=0) + generation + `gpt_crc32`; pure `tpm_baseline_finalize/validate/compare`.
- [x] Enrollment gated by a recovery-authorized `tpm_enroll` config bit (NOT silent first-boot TOFU): when set, assemble + store the blob via `tpm_nv_define_data`/`tpm_nv_write`; report it as config/recovery-authorized enrollment.
- [x] Phase-1 baseline verify step in `boot_interrupts` AFTER replay verify (NOT `tpm_integrity_init`): read the blob, compare current PCRs/SB/firmware-hash to golden -> set `boot_integrity_report.overall_status` VERIFIED / MISMATCH / NO_BASELINE.
- [x] Baseline rotation under the recovery gate + monotonic generation counter (overwrite the blob; reject a lower generation).
- [x] Follow-up: consolidate the measured PCR set into one `tpm_pcr_baseline_pcrs()` iterator (from the allocation-table BASELINE mask); cache/replay/baseline call it instead of hard-coding `{0-7,11}`.
- [x] Commit: `"tpm: enroll measured boot baseline"` (c7032e45)

**Test checkpoint:** With the recovery-authorized `tpm_enroll` config bit set, enrollment assembles + stores the versioned baseline blob (golden PCRs per bank + Secure Boot state + firmware-version hash + `abi_manifest_sha256`) in the owner-auth NV DATA index; a normal second boot (no enroll bit) reads it via the Phase-1 verify step and reports `verified`; an injected PCR/SB change reports `baseline-mismatch` (not a false `verified`); a NO_BASELINE boot does NOT auto-enroll; a recovery-gated rotation overwrites the blob and rejects a lower generation. The marshal/compare/generation logic is fixture-tested; the live swtpm enroll->reboot->verify->rotate cycle is QEMU-swtpm/bare-metal validation. Platforms: QEMU swtpm KVM; bare metal (test laptop fTPM).

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 3 baseline suites, 0 failures

> **Notes:**
> - Shipped `src/kernel/tpm_baseline.{c,h}`: versioned `struct tpm_baseline` (PCR digests + SB state + fw-hash + generation + `gpt_crc32`); pure `finalize/validate/compare/rotation_ok` + live `snapshot/enroll/verify`. 3 suites in `test_tpm_baseline.c`.
> - Wired in `boot_interrupts.c` Phase-1 after `tpm_replay_verify`: recovery-gated enroll (`boot_mode==recovery && config.tpm_enroll`) else verify; `tpm_enroll` is a boot_config reserved-slot field (kernel+mirror+bootloader+manifest+doc).
> - Consumes §7 NV (owner-auth data index) + §3 PCR cache + §5 SB state + §4 replay precedence; fills the old `tpm.c` "read golden values" gap as a Phase-1 step (not Phase-0).
> - Canonical doc: the section "Design (2026-06-14...)" note above + `docs/boot/boot-info-fields.md` (`tpm_enroll`).
> - Scope: §6 owns baseline CONTENT/enroll/verify/rotate; console confirmation is §15, image-digest carriage is §16, NV write-lock anti-rollback is §17 (primitives) and §21 (trust anchor), moved there by the §14 split and the 2026-08-17 §17 split ( "infra-blocked" was wrong for the console half, which is buildable).
> **Verified:** 2026-06-14 | commit `c7032e45` (impl) + review fixes | 4/9 items | build OK | tests 668 security PASS, smoke PASS (KVM 2.50s)
> **Accepted:** [H] enrollment gate is boot.conf config (`boot_mode==recovery && tpm_enroll`), not loader-validated recovery provenance -- config-spoofable -> XREF: 01-boot-platform/TODO-13 §15 (item: "Gated enrollment on `tpm_enroll_gate_evaluate()`") (RETARGETED 2026-08-17: the follow-up item moved out of this section when §14 split the backfill cohort; RESOLVED 2026-08-17 by §15 -- the gate now anchors on the NVRAM sticky trigger plus `selection_reason` and requires console confirmation, and the config-only path is reported as `esp-config-only` authority which never authorizes a write)
> **Accepted:** [H] baseline verify leaves stale per-PCR `NO_CRYPTO` + omits PCR11 in `boot_integrity_report` (self-contradicts VERIFIED) -> XREF: 01-boot-platform/TODO-13 §18 (item: "`tpm_integrity_publish_baseline()` replaces `tpm_integrity_set_overall_status()`") (RETARGETED 2026-08-17: the follow-up item moved out of this section when §14 split the backfill cohort) (RESOLVED 2026-08-17 by §18: the array is sized `BOOT_INTEGRITY_MAX_PCRS` so PCR 11 has a slot, and every per-PCR status is refreshed in the same publication as the verdict)
> **Accepted:** [M] measured PCR set `{0-7,11}` duplicated across `tpm.c`/`tpm_replay.c`/`tpm_baseline.c`/test -> XREF: 01-boot-platform/TODO-13 §6 (item: "Follow-up: consolidate the measured PCR set" at line 192) (RESOLVED 2026-06-14 by §6 commit 2b0dd4ed: `tpm_pcr_baseline_pcrs()` derives the set from the allocation-table mask; the 3 consumers migrated)
> **Quality reviewed:** 2026-06-14 | Codex 7x (design + adversarial + consistency + perf + re-adversarial) | 4H+3M fixed, 2H+1M accepted-XREF | scope: kernel-code-quality

## 7. TPM NV Index Support

> **Design (2026-06-14, Codex 2H+1M adopted pre-code):** §7 owns the NV storage + PCR-policy MECHANISM; §6 owns the baseline CONTENT/enrollment that consumes it (resolves the prior circular §6<->§7 ownership). Constraints for the implementor: a policy-protected index MUST use POLICYREAD/POLICYWRITE, NOT OWNERREAD/OWNERWRITE (owner auth is an operational bypass of any PCR policy; owner auth is for define/undefine only). NV response-code classification MUST branch by FORMAT first -- the NV warnings 0x148-0x14C are format-0 (RC_VER1) and must be exact-compared, never run through a format-1 handle/parameter mask (which would corrupt them and hide locked/no-space states). Do NOT mark §7 complete after CRUD-only; policy protection is part of §7's contract.

- [x] NV CRUD for one OWNER-auth OS data index: pure `tpm2_build_nv_{define,undefine,write,read,read_public}` + `tpm_nv_*` wrappers (`TPM_RS_PW` password auth) in `tpm_nv.{c,h}`; owner-auth data index distinct from the policy baseline.
- [x] Degraded states: format-first `tpm_nv_classify_rc` exact-comparing format-0 warnings (`NV_LOCKED` 0x148, `NV_SPACE` 0x14B, `NV_DEFINED` 0x14C) to `tpm_nv_status_t`; never wedges. Raw 0x148-0x14C fixtures in `test_tpm_nv.c`.
- [x] PCR-policy-protected baseline index: `TPMA_NV_POLICYREAD|POLICYWRITE` + `TPM2_StartAuthSession`/`TPM2_PolicyPCR` (trial computes authPolicy, real session satisfies r/w); owner auth only for define/undefine. -> XREF: §8.

**Test checkpoint:** Define / read / write / undefine round-trip on one OS-owned NV index against QEMU swtpm; where the TPM supports it the index is PCR-policy-protected (a read under the wrong PCR state is denied); no-space and locked-NV return explicit degraded states (not a wedge); the UEFI-variable -> TPM-NV migration path moves an existing baseline without loss. Platforms: QEMU swtpm KVM; bare metal (test laptop fTPM). The PCR-state-deny + live round-trip are swtpm/bare-metal validation; the kernel unit suites cover marshal/parse/classifier + session-lifecycle teardown via the fake-TIS seam.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 10 NV suites, 0 failures

> **Notes:**
> - Shipped `src/kernel/tpm_nv.c` + `tpm_nv.h`: pure TPM2 NV builders/parsers, format-first `tpm_nv_classify_rc`, `tpm_nv_*` wrappers, plus `tpm2_rsp_params()` (session-aware response locator) in the transport. 9 suites in `test_tpm_nv.c`.
> - Phase-1 transport via `tpm2_submit` (not ISR-safe); the trial/real PolicyPCR flow uses a single-cleanup teardown that best-effort `FlushContext`es the session on every post-start path (no handle leak on error/BUSY).
> - Unblocks §6 baseline storage (define/read/write + policy-protected index) and §8 (reuses the policy-session machinery). Codex design + test-coverage adoptions in the section commit.
> - Baseline policy is pinned to the allocation table: `tpm_nv_baseline_pcr_select()` derives the PolicyPCR selection from `tpm_pcr_baseline_mask()` (§12), no hard-coded PCR set.
> - Scope: §7 owns the NV storage MECHANISM only; §6 owns baseline CONTENT/enrollment. The UEFI-var -> TPM-NV migration item was closed not-applicable by §14 (§6 never used a UEFI variable); write-lock/monotonic index primitives are §17 and the authenticated lifecycle is §21.
> **Verified:** 2026-06-14 | commit `b7a9ec35` (impl) + review fixes | 3/4 items | build OK | tests 641 security + 4965 full PASS, smoke PASS (KVM 2.51s)
> **Quality reviewed:** 2026-06-14 | Codex 11x (design + test-coverage + adversarial + consistency + perf + re-adversarial) | 5H+8M fixed, 0 open | scope: kernel-code-quality

## 8. Sealed-Secret Boot Policy Hooks

- [x] Prerequisite: shared policy-session seam in `tpm_nv.{c,h}` -- `tpm_policy_session_run` + `tpm_policy_pcr_digest` + `tpm_pcr_mask_to_select` + exposed `tpm_session_cmd_exec`; NV wrappers now thin, no behavior change (§6/§7 suites green).
- [x] Seal/unseal to `tpm_pcr_seal_mask()` (PCR 7, PCR 11 excluded) in `tpm_seal.{c,h}`: deterministic ECC-P256 SRK + KEYEDHASH object (PolicyPCR authPolicy, no userWithAuth) + Load + Unseal via real POLICY session; single-cleanup teardown.
- [x] FDE key-unlock hook (`tpm_seal_fde_key`/`tpm_unseal_fde_key`, domain-tagged forward-API; stub consumer until storage-encryption lands).
- [x] Code-integrity policy seal hook (`tpm_seal_ci_policy`/`tpm_unseal_ci_policy`, forward-API; stub consumer until CI-policy lands).
- [x] Structured unseal-failure reporting (`struct tpm_unseal_result`: status + raw rc + domain) + callable recovery handoff (`tpm_seal_set_recovery_handler`, fired on every failure).
- [ ] Follow-up: live-console recovery prompt on unseal failure (needs the Phase-1 console-input path). -> shared owner with the §6 console-input infra follow-up.
- [ ] Follow-up: salted/bound HMAC sessions + parameter encryption (response-HMAC authenticity + sealed-key bus confidentiality). `tpm_session_auth_response_ok` is structural-only; transport-wide (§7/§8/§13). Parity: BitLocker bus protection.
- [x] Commit: `"tpm: sealed boot policy hooks"`

**Test checkpoint:** Seal a small secret to the SEAL PCR policy (`tpm_pcr_seal_mask()`, PCR 7), then unseal succeeds while PCR 7 matches and is DENIED (`TPM_RC_POLICY_FAIL`, with structured failure reporting + a callable recovery handoff) after PCR 7 changes; a PCR 11 / kernel-manifest-only change does NOT break unseal (seal excludes PCR 11); the FDE key-unlock + code-integrity seal hooks are present + callable (stub consumers OK). The Create/Load/Unseal marshal/parse + policy selection are fixture-tested; the live seal->PCR-change->deny cycle is QEMU-swtpm/bare-metal validation. Platforms: QEMU swtpm KVM; bare metal.
> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 761 kernel + 16 user-mode, 0 failures
> **Notes:**
> - `tpm_seal.{c,h}`: pure builders/parsers (CreatePrimary SRK, Create KEYEDHASH, Load, Unseal) + `tpm_seal_secret`/`tpm_unseal_secret` over the shared `tpm_nv.c` policy seam, single-cleanup teardown of primary/object/session on every path.
> - Seal binds PCR 7 only (`tpm_pcr_seal_mask()`, PCR 11 excluded) so a kernel-ABI-manifest change does not brick FDE; a negative test asserts PCR 11 absent from the mask.
> - Forward-API: FDE + code-integrity domain-tagged hooks (stub consumers) plus a structured `tpm_unseal_result` + `tpm_seal_set_recovery_handler` recovery handoff (live-console prompt is the open follow-up, shared with §6 console infra).
> - Session-response validation is STRUCTURAL (shared `tpm_session_auth_response_ok`, keyed off the command tag): a malformed / truncated / wrong-tag success can never return secret bytes or a bogus handle as OK. Cryptographic authenticity vs a bus interposer (response HMAC) is the filed HMAC-session follow-up.
> - Scope boundary: this section owns the seal/unseal mechanism + forward hooks; the storage-encryption / CI-policy consumers and the live recovery prompt are tracked elsewhere.
> **Verified:** 2026-06-14 | commit `f0712b77` | 5/7 items | build OK | tests 777/777
> **Deferred:** [H] cryptographic response authenticity + parameter encryption vs a physical bus interposer (forged well-formed success / key sniffing); structural validation only today -> XREF: 01-boot-platform/TODO-13 §8 (item: "salted/bound HMAC sessions + parameter encryption" at line 240)
> **Quality reviewed:** 2026-06-14 | Codex 11x (design + adversarial + adversarial-impl + re-adversarial + consistency + perf) | 5H+5M fixed, 1H deferred | scope: kernel-code-quality

## 9. Attestation Report Export

> **Design (2026-06-14, Codex 3H+1M adopted pre-code):** build the report from §13's query APIs (`tpm2_quote`/`tpm_ak_public_get`/`tpm_ek_cert_read`) + §6's `boot_integrity_report`, reusing §1's streaming one-`vfs_write`-per-record JSON pattern; put the struct + builder in a NEW `tpm_attest_report.c` (`tpm.c`/`tpm_attest.c` are already 33-35 KB). Adopted constraints for the implementor: (1) snapshot the handoff triple as an IMMUTABLE Phase-0 copy taken right after `boot_info` validation, BEFORE any kernel caps refinement (`boot_caps_mark_present`, runtime-services degradation) -- the builder consumes that snapshot, NOT live `g_boot_info`, so the report reflects loader evidence not later kernel policy. (2) `BOOT_CAP_MANIFEST_PCR_BOUND` (1u<<10) is an IGNORED unknown bit today -- make it a validated capability FIRST (add to `BOOT_CAP_LIST` + bootloader mirror + `BOOT_CAP_MASK_KNOWN`, classify present/degraded in `bootx64.c`, regression that it validates exactly once) before the report's "PCR bound" claim can gate. -> XREF: TODO-01 §11. (3) `tpm2_quote` signs ONLY the SHA-256 bank -- the report schema MUST mark SHA-256 PCRs as quoted (TPM-signed) and all other banks diagnostic-only; never a mixed-trust report. (4) the bootloader PCR-11 manifest extend MUST run BEFORE `retrieve_tpm_event_log()` so the copied log includes it (else §4 replay reports false tamper), with a canonical binary event payload (magic/version + PCR index + manifest sha256 + label) and a typed `HashLogExtendEvent` prototype.

- [ ] Prerequisite: make `BOOT_CAP_MANIFEST_PCR_BOUND` (1u<<10) a validated capability (`BOOT_CAP_LIST` + mirror + `BOOT_CAP_MASK_KNOWN` + classify in `bootx64.c` + regression). -> XREF: TODO-01 §11.
- [x] Immutable Phase-0 handoff snapshot: `struct boot_attest_handoff` + latched write-once `tpm_attest_handoff_snapshot_init`/`_get` in new `tpm_attest_report.c`, captured in `boot_hw.c` pre-caps-refinement (design constraint 1).
- [x] `boot_attestation_report_t` + `tpm_attest_report_build()`: Phase-0 snapshot + `.bootproto` manifest sha256 + integrity verdict + SHA-256-quoted PCR bank + Quote/AK/EK; pcrDigest coherence recompute; AK-EK binding UNVERIFIED; nonce MIN..MAX.
- [x] Export to `X:\Diag\attestation.json` via `tpm_attest_report_export()` -> streaming `tpm_attest_report_to_json()` (bounded sink, O_TRUNC, length-validated, no escaping); CSPRNG nonce; wired at the boot diagnostics dump.
- [x] Sibling stale-tail fixed: `tpm_evlog_export_cel` (tpm.c) now opens `tpm-events.json` with `VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC` and closes the parent dir handle, so a shorter event log cannot leave previous-boot tail bytes.
- [ ] Add native query API for user-mode system settings.
- [ ] Add remote-attestation placeholder for platform services.
- [ ] Extend a TPM PCR (target PCR 11, vendor-policy-extensible) with the kernel `.bootproto` manifest sha256 from the bootloader pre-jump path, BEFORE the bootloader transfers control to the kernel. Concretely: in `src/boot/uefi/bootx64.c` after `bootproto_verify_or_reset()` succeeds, walk the EFI_TCG2_PROTOCOL via `LocateProtocol(EFI_TCG2_PROTOCOL_GUID, ...)` and call `Tcg2Protocol->HashLogExtendEvent` with the 32-byte sha256 from the descriptor + a stable event-log entry name (e.g. `IMPOSSIBLE_OS_KERNEL_ABI_MANIFEST`). On success, the bootloader sets `boot_info.caps_present |= BOOT_CAP_MANIFEST_PCR_BOUND` (bit reserved by TODO-01 §11). Failure to extend (no TPM, locality denied, command failure) MUST leave the bit clear and continue boot -- this is a measurement, not a gate. Replay path: §4 PCR replay must include the manifest extend event so a baseline mismatch flags a different kernel-image-vs-manifest pairing, not just a different kernel image. -> XREF: [`01-boot-platform/TODO-01 §11`](TODO-01-boot-protocol-abi-handoff.md#11-capability-negotiation-and-degraded-feature-flags) (item: "Forward-reserve `BOOT_CAP_MANIFEST_PCR_BOUND` (1u<<10) bit").
- [ ] Include the bootloader-to-kernel handoff triple in `boot_attestation_report_t`: ABI manifest sha256 (from `boot_proto_descriptor.sha256`), capability words (`caps_required` / `caps_present` / `caps_degraded`), boot-path provenance (`boot_path` / `boot_reason` / `boot_source_flags` / `boot_fallback_depth`), and bootloader build identity (when TODO-01 §20 ships). Today downstream attestation consumers re-read `g_boot_info` directly which couples them to the live struct layout; the report API decouples by snapshotting the triple at attestation-build time. -> XREF: [`01-boot-platform/TODO-01 §11`](TODO-01-boot-protocol-abi-handoff.md#11-capability-negotiation-and-degraded-feature-flags) caps fields, [`§12`](TODO-01-boot-protocol-abi-handoff.md#12-common-boot-path-provenance-and-decision-record) decision record, [`§17`](TODO-01-boot-protocol-abi-handoff.md#17-bootloader-pre-jump-abi-mismatch-screen) `.bootproto` sha, [`§20`](TODO-01-boot-protocol-abi-handoff.md#20-bootloader-build-identity) bootloader build identity.
- [x] DRTM forward-compat slots reserved + round-tripped: `drtm_entry_pcr`/`drtm_acm_status`/`drtm_measurement_type` in `boot_attestation_report_t` (zeroed today, serialized in attestation.json) for a future Secure Launch adapter.
- [ ] Commit: `"tpm: export boot attestation report"`

**Test checkpoint:** A signed `boot_attestation_report_t` carries PCRs + event digest + Secure Boot state + nonce and exports to `X:\Diag\attestation.json`; the native query API returns the same snapshot to user-mode; the bootloader manifest PCR-extend (PCR 11) ran pre-jump and set `BOOT_CAP_MANIFEST_PCR_BOUND` (clear + boot-continue when no TPM); the report snapshots the handoff triple (manifest sha256, caps words, boot-path provenance) and round-trips the zeroed DRTM forward-compat slots. Platforms: QEMU swtpm KVM; bare metal.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 982 kernel + 16 user-mode, 0 failures
> **Notes:**
> - Shipped: Phase-0 handoff snapshot + `boot_attestation_report_t`/`tpm_attest_report_build()` (manifest + handoff + SHA-256-quoted PCR bank + quote/AK/EK + coherence) + streaming JSON export to `X:\Diag\attestation.json` (schema v2).
> - Trust model: only the SHA-256 bank is TPM-quoted; AK<->EK binding stays UNVERIFIED; an EK-cert read failure is UNKNOWN not ABSENT; a malformed nonce is BADARG not a degraded report.
> - Tests: `test_tpm_attest_report.c` (3 suites): snapshot write-once, builder field/coherence/trust + nonce contract, JSON serializer via in-memory sink (schema/freshness fields, overlong-length refusal).
> - Canonical doc: the trust/coherence/DRTM schema lives in the `boot_attestation_report` struct header comment.
> - Scope boundary: native query API + remote placeholder + bootloader PCR-11 extend + cap-bit are Deferred (stop-and-ask ABI / firmware HW-validation); they keep §9 at `[/]`.
> **Verified:** 2026-06-14 | commit `2355ba5f` (report+export) + review fixes | 5/10 items | build OK | security 982 PASS, smoke PASS (KVM 2.490s)
> **Deferred:** [M] native attestation query API for user-mode (new syscall/SSDT ABI exposing attestation evidence + an access-control decision; stop-and-ask) -> XREF: 01-boot-platform/TODO-13 §9 (item: "Add native query API for user-mode system settings" at line 264)
> **Deferred:** [M] bootloader PCR-11 manifest extend + `BOOT_CAP_MANIFEST_PCR_BOUND` cap-bit (firmware TCG2 HashLogExtendEvent; real-HW validation; cap-bit ABI owned by TODO-01 §11) -> XREF: 01-boot-platform/TODO-13 §9 (item: "Extend a TPM PCR (target PCR 11..." at line 269)
> **Deferred:** [L] remote-attestation placeholder needs a remote-attest protocol design decision -> XREF: 01-boot-platform/TODO-13 §9 (item: "Add remote-attestation placeholder for platform services" at line 265)
> **Deferred:** [L] handoff-triple bootloader build-identity field pends TODO-01 §20 -> XREF: 01-boot-platform/TODO-13 §9 (item: "Include the bootloader-to-kernel handoff triple..." at line 271)
> **Quality reviewed:** 2026-06-14 | Codex 5x (adversarial + consistency + perf + re-adversarial) | 1H+5M fixed | scope: kernel-code-quality

## 10. Recovery and Mismatch UX

- [x] Add VPD/boot diagnostics status for verified, no TPM, no baseline, mismatch, and event-log tamper -- `tpm_integrity_status_label()` maps the report to one status word (tamper before baseline; UNKNOWN->`unknown`), wired to a Phase-1 `klog` line.
- [ ] In recovery, explain whether firmware, bootloader, kernel, Secure Boot db, or baseline changed.
- [ ] Allow trusted baseline reset only from recovery mode with local confirmation.
- [ ] Integrate with A/B rollback if kernel measurement changed unexpectedly.
- [ ] Commit: `"recovery: measured boot mismatch UX"`

**Test checkpoint:** VPD / boot diagnostics show distinct status for verified / no-TPM / no-baseline / mismatch / event-log-tamper; recovery mode names which layer changed (firmware vs bootloader vs kernel vs Secure Boot db vs baseline); a trusted baseline reset is accepted ONLY from recovery mode with local confirmation; an unexpected kernel-measurement change triggers A/B rollback integration (TODO-22). Platforms: manual recovery-mode walkthrough (QEMU swtpm + injected mismatch); bare metal.

> **Test runner:** N/A (no new §10 code surface; `tpm_integrity_status_label()` shipped + reviewed with the integrity work) | validation: Phase-1 klog status line
> **Notes:**
> - Shipped: VPD/boot-diagnostics status labels via `tpm_integrity_status_label()` (verified / no-TPM / no-baseline / mismatch / tamper / unknown) on a Phase-1 klog line.
> - Scope boundary: the 3 remaining items (recovery layer-change explanation, baseline reset, A/B rollback) need a recovery-mode environment that does not exist yet (A/B owned by TODO-22); Deferred, §10 stays `[/]`.
> - No new code shipped this pass: §10 is a blocked-section defer-stamp; the lone done item's helper was reviewed in the integrity sections.
> **Verified:** 2026-06-14 | commit `c9ce7bcd` (status-label helper, prior) | 1/4 items | build OK | manual (Phase-1 klog status line)
> **Deferred:** [M] recovery names which layer changed (firmware/bootloader/kernel/SB-db/baseline) -- needs a recovery-mode UI/environment, none exists yet -> XREF: 01-boot-platform/TODO-13 §10 (item: "In recovery, explain whether firmware, bootloader, kernel, Secure Boot db, or baseline changed")
> **Deferred:** [M] trusted baseline reset only from recovery mode + local confirmation (security-sensitive; recovery-mode + confirmation UX) -> XREF: 01-boot-platform/TODO-13 §10 (item: "Allow trusted baseline reset only from recovery mode with local confirmation")
> **Deferred:** [M] A/B rollback on an unexpected kernel-measurement change (blocked on TODO-22 A/B rollback infra) -> XREF: 01-boot-platform/TODO-13 §10 (item: "Integrate with A/B rollback if kernel measurement changed unexpectedly")
> **Quality reviewed:** 2026-06-14 | Codex 0x (blocked section; no new code surface to review) | 0 fixed, 3 deferred | scope: N/A (recovery-mode infra + TODO-22 not yet available)

## 11. TPM Tests and Event-Log Fixtures

- [x] Event-log parser fixtures for TPM 1.2 + TPM 2.0 -- `test_tpm_event_log.c` (`tpm12_two_events`, `tpm20_event2`) parse full known-good logs; 9 suites incl. truncation/cap/bad-header/partial-tail.
- [x] PCR replay known-vector tests -- `test_tpm_replay.c` `test_replay_known_vector` (log -> independently-computed `H(H(0||d1)||d2)`) + `test_replay_legacy_sha1`.
- [x] QEMU swtpm test path -- `scripts/test-swtpm.sh`: swtpm 2.0 on a unix socket + QEMU `tpm-crb`/`tpm-tis`; asserts boot-complete + TPM transport up; skips clean (exit 0) when swtpm absent.
- [x] Fake-CRB buffer seam + unit suite for `crb_submit()` -- `tpm_t_test_install_crb_buffers()` + `test_tpm_crb_submit_fake` (ready/START/goIdle handshake, response copy, oversized + sub-header reject, stuck-START timeout, over-cap cmd -> ERR_ARG).
- [x] Degraded tests -- transport-level no-TPM (`test_tpm_transport_no_tpm` -> ERR_NODEV) + existing truncated-log (event-log) + inactive-bank (replay absent-bank) coverage.
- [x] Test-infra hardened: `tpm_t_test_install` returns a full `tpm_t_test_state` snapshot + new `tpm_t_test_restore()` (iface/available/failed/fast/busy); every `test_tpm_*` suite captures+restores so a real-fTPM host is not left mis-routed.
- [x] Commit: `"test: TPM measured boot coverage"`

**Test checkpoint:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) runs the new TPM suites with 0 failures: TPM 1.2 + 2.0 event-log parser fixtures, PCR-replay known-vector tests, the fake-CRB buffer seam suite for `crb_submit()`, and degraded cases (no TPM, truncated log, inactive banks); the QEMU swtpm path boots green on KVM. Platforms: kernel unit tests + QEMU swtpm KVM; bare metal (test laptop fTPM).
> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 787 kernel + 16 user-mode, 0 failures
> **Notes:**
> - Event-log fixtures (TPM 1.2 + 2.0) and PCR-replay known-vectors already existed in `test_tpm_event_log.c` / `test_tpm_replay.c`; this section adds the missing coverage.
> - Fake-CRB seam: `tpm_t_test_install_crb_buffers()` points the CRB cmd/rsp buffers at test memory so `test_tpm_crb_submit_fake` exercises the CRB control-area handshake (the prior fake covered the TIS FIFO interface only).
> - `scripts/test-swtpm.sh` is the live-validation path: swtpm 2.0 + QEMU `tpm-crb`/`tpm-tis`, asserts boot-complete + TPM transport up; exits 0 (skipped) when swtpm is not installed.
> - Degraded coverage: added a transport-level no-TPM test (`ERR_NODEV`); truncated-log + inactive-bank cases already covered by the event-log / replay suites.
> - Scope boundary: this section is test coverage for already-shipped TPM features (transport, event-log, replay); the swtpm script is the host/CI live-validation entry point, not a WSL unit test.
> **Verified:** 2026-06-14 | commit `c40c4eee` | 5/6 items | build OK | tests 789/789
> **Deferred:** [M] `tpm_t_test_install` teardown leaves a real-fTPM transport mis-routed after a TPM suite (cross-cutting test-infra) -> XREF: 01-boot-platform/TODO-13 §11 (item: "Follow-up (test-infra): harden `tpm_t_test_install`" at line 314) (RESOLVED 2026-06-14 by §11 commit 45054685: `tpm_t_test_install` returns a full `tpm_t_test_state` snapshot + new `tpm_t_test_restore()`; all `test_tpm_*` suites capture+restore)
> **Quality reviewed:** 2026-06-14 | Codex 5x (adversarial-impl + re-adversarial + consistency + perf) | 4H+2M fixed, 1M deferred | scope: kernel-code-quality

---

## 12. PCR Allocation and Policy Mask Table

A single source of truth for which boot events extend which PCR, and which PCR masks the sealed-secret / baseline / quote consumers use. Without it the §9 manifest-extend into PCR 11 collides with established conventions (BitLocker seals to PCR7+PCR11; systemd-stub/pcrlock use PCR 11 for UKI kernel-boot; Keylime may exclude PCR 11 from runtime masks), so a benign kernel-ABI-manifest or event-order change could trigger FDE unseal failure, a recovery prompt, or a false attestation mismatch instead of being reported as the intended layer.

> **Design (2026-06-14, Codex 2H+1M adopted pre-code):** the source of truth must be EVENT-centric, not PCR-centric -- a per-PCR `tpm_pcr_owner(index)` returning one owner/event cannot classify PCR 11's multiple events (kernel-ABI-manifest vs UKI kernel-boot), so manifest-only drift would still read as a generic mismatch. Model an ordered event allocation table (entries: event_name, pcr, producer, layer, ordering, policy flags) allowing multiple entries per PCR; `tpm_pcr_owner(index)` is a view over it. Masks MUST be DERIVED from per-event policy flags (`seal_ok`/`quote_ok`/`baseline_ok`/`volatile`) or static-asserted against them -- a subset-of-owners check does not catch a seal mask wrongly including PCR 11. The manifest event constant (PCR 11 + `IMPOSSIBLE_OS_KERNEL_ABI_MANIFEST`) MUST live in a bootloader-safe shared header consumed by BOTH the §9 bootloader extend and the kernel table (no producer/consumer drift).

- [x] `docs/boot/pcr-allocation.md`: event-centric ownership table (PCR 0-7 firmware/SB, PCR 11 UKI kernel-boot + kernel-ABI-manifest) + seal/quote/baseline rationale + BitLocker/systemd/Keylime conventions.
- [x] `tpm_pcr_alloc.h`/`.c`: ordered EVENT allocation table (`struct tpm_pcr_event_alloc`), two entries on PCR 11; `tpm_pcr_owner(index)` is a pure lowest-ordering-layer view (no TPM transaction).
- [x] Masks DERIVED from per-event policy flags (`tpm_pcr_{seal,quote,baseline}_mask` scan the table -- no constant to drift); seal default = PCR 7 only, PCR 11 EXCLUDED. Negative test asserts PCR 11 not in seal.
- [x] Manifest-event constant in dependency-free `include/boot/pcr_manifest.h` (`TPM_PCR_MANIFEST_INDEX`/`_EVENT`); kernel table includes it, bootloader-consumable, one definition. -> XREF: §9 consumes it + owns the drift check.
- [x] Commit: `"tpm: PCR allocation table and policy masks"`

**Test checkpoint:** A manifest-only change (rebuild kernel with a new `.bootproto` sha) is reported as the kernel-ABI layer in diagnostics and does NOT spuriously fail FDE unseal (§8) or remote-attestation verification (§13); the allocation table in `docs/boot/pcr-allocation.md` matches the runtime extends (a unit test asserts each owned PCR's event name). Platforms: kernel unit tests + QEMU swtpm KVM; bare metal.
> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 465 kernel + 16 user-mode, 0 failures
> **Notes:**
> - `tpm_pcr_alloc.h`/`.c` + `docs/boot/pcr-allocation.md`: event-centric allocation table (10 entries, two on PCR 11) + `tpm_pcr_owner()` view + derived seal/quote/baseline masks; pure data, no TPM transaction or global state.
> - Consumed by later sections: §6 baseline mask, §8 seal mask, §13 quote mask call the derived-mask helpers instead of hard-coding bitmaps; the manifest event constant is the bootloader producer's source of truth.
> - Codex design adoptions (event-centric table over per-PCR owner, masks derived from policy flags, seal excludes PCR 11, centralized manifest constant) baked in pre-code; per-finding trail in the commit message.
> - Scope boundary: this section is the authoritative table only; §9 owns the bootloader PCR-11 manifest extend + the producer-side drift check; §6/§8/§13 own the policy consumers.
> **Verified:** 2026-06-14 | commit `026400d6` | 4/4 items | build OK | tests 465/465
> **Quality reviewed:** 2026-06-14 | Codex 5x (design + adversarial + consistency + perf) | 2H+2M fixed, 0 open | scope: kernel-code-quality

---

## 13. Attestation Key Provisioning and TPM2 Quote

The TPM-rooted signing mechanism §9's report needs. A software-signed JSON cannot prove the PCRs came from this machine's TPM; both Win11 Device Health Attestation and Linux Keylime rely on a TPM-resident Attestation Key (AK) bound to the Endorsement Key, and `TPM2_Quote` (the TPM signs the PCR digest + a verifier nonce). This section provisions the AK and produces verifiable quotes; §9 consumes them. -> XREF: supersedes the research-spike `18-future-research/TODO-04 §2` (item: "TPM Quote (remote attestation)") for the active implementation.

- [x] Retrieve the EK cert from NV (RSA `0x01c00002`, ECC `0x01c0000a`) -- `tpm_ek_cert_read()`: NV_ReadPublic for size then chunked `tpm_nv_read`; `NO_EK_CERT` when absent (vTPM/fTPM degrade).
- [x] Provision the AK (`tpm_attest.c`): deterministic ECC-P256 EK `CreatePrimary` (endorsement) + `Create`/`Load` a restricted ECDSA-P256 signing AK under it; `EvictControl` builder present; AK recreated + cached each boot.
- [ ] Support `TPM2_MakeCredential` / `TPM2_ActivateCredential` for the credential-activation challenge (verifier confirms the AK is bound to a trusted EK). -> follow-up (not yet implemented).
- [x] `tpm2_quote()` via `TPM2_CC_Quote`: signs the selected-PCR digest + nonce with the AK (ECDSA); parses + returns `struct tpm_quote_attest` + raw signature. Fixture + fake-TIS lifecycle tested.
- [x] Anti-replay: rejects a too-short/absent nonce, returns `NONCE_STALE` unless the attest extraData echoes it; surfaces `clockInfo` (clock/resetCount/restartCount/safe) in `struct tpm_quote_attest`.
- [x] Bind the quote response to the request: reject unless the parsed PCR selection (single SHA-256 bank, 32-byte digest) matches the mask, the scheme is ECDSA+SHA-256, and ECDSA r/s <= 32 bytes; out-of-range `pcr_mask` -> `BADARG`.
- [ ] Bind `TPMS_ATTEST.qualifiedSigner` to the AK Qualified Name before returning `OK` (needs EK-pub capture + TPM Name-algebra); today the remote verifier binds the signer via the exported AK public. -> follow-up (Codex re-adversarial [H]).
- [ ] Wire the quote + AK public + EK-cert chain into §9's report (§13 exposes the query APIs; §9 owns integration). -> XREF: [`§9`](#9-attestation-report-export) (item: "Build the TPM-rooted `boot_attestation_report_t`"). Blocked on §9.
- [x] Degraded: no TPM / no EK cert / AK fail -> classified `tpm_attest_status_t`; every entry point returns a status, NEVER gates boot. Fixture-tested.
- [x] Commit: `"tpm: attestation key provisioning and TPM2 quote"`

**Test checkpoint:** Against QEMU swtpm, `tpm2_quote(mask, nonce, ...)` returns a `TPMS_ATTEST` + signature that verifies under the AK public key and carries the supplied nonce; a credential-activation round-trip (`MakeCredential`/`ActivateCredential`) succeeds; a replayed quote (stale nonce) is rejected; with no TPM the path reports `attestation_unavailable` and boot continues. Platforms: QEMU swtpm KVM (quote + verify); bare metal (test laptop fTPM, real EK cert).
> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 903 kernel + 16 user-mode, 0 failures (11 `test_tpm_attest.c` suites incl. signature-structure + request/response bind rejection)
> **Notes:**
> - `tpm_attest.{c,h}`: pure-marshal builders/parsers (EK CreatePrimary, PolicySecret, AK Create, Quote, TPMS_ATTEST parse, EvictControl) + live wrappers (EK-cert read, AK provision, `tpm2_quote`, AK-pub) over the `tpm_nv.c` policy seam.
> - AK provision: deterministic ECC-P256 EK (endorsement) + restricted ECDSA-P256 signing AK; the policy-auth EK needs `PolicySecret` re-run before EACH EK-authorized command (Create + Load). AK cached per boot; atomic SMP gate, no lock over TPM I/O.
> - `tpm2_quote` binds the response to the request (PCR selection == mask, SHA-256 bank, ECDSA+SHA-256 scheme) + the verifier nonce (anti-replay -> `NONCE_STALE`); every entry point degrades to a classified status and NEVER gates boot.
> - Query APIs (`tpm2_quote`/`tpm_ak_public_get`/`tpm_ek_cert_read`) are what §9 consumes; report integration + the `MakeCredential`/`ActivateCredential` challenge are tracked follow-ups.
> - Scope boundary: §13 owns the TPM-rooted signing mechanism; §9 owns the report export; live quote+verify + credential activation are swtpm/bare-metal validation.
> **Verified:** 2026-06-14 | commit `c9ce7bcd` (ship) + review (request/response binding) | 6/9 items | build OK | tests 903/903 PASS (security)
> **Deferred:** [H] `TPMS_ATTEST.qualifiedSigner` not bound to the AK Qualified Name -> XREF: 01-boot-platform/TODO-13 §13 (item: "Bind `TPMS_ATTEST.qualifiedSigner` to the AK Qualified Name" at line 366) (reason: needs EK-pub capture + Name-algebra; verifier binds the signer via the exported AK pub)
> **Quality reviewed:** 2026-06-14 | Codex 8x (adversarial, consistency, perf, re-adversarial) | 4H+4M fixed, 1H deferred | scope: kernel-code-quality

---

## 14. Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)

Items moved here VERBATIM from their original, already-stamped sections, where they were unreachable: the triage oracle classifies a stamped section DONE without reading its body, so an item appended after the stamp is invisible to every later pass. Source section noted per group. Cohort context: `todo/overnight-runner-improvements/overnight-runner-improvements-v05.md` item 3.

**Disposition (2026-08-17).** `section-manifest.py` scored this section SPLIT-RECOMMENDED (6 work items + ABI impact), and it is right: the cohort is six unrelated deliverables that happen to share an origin story. One is closed here as never-applicable; the other five moved to sections of their own, because they touch different files, carry different risk, and one of them is an ABI change that must not ride along with unrelated work. Nothing was dropped and nothing was re-created: each migrated item names its new owner below, and each new section states what it does NOT re-cover.

From the stamped section 7:
- [x] Migration from UEFI authenticated-variable storage to TPM NV (lossless) -- NOT APPLICABLE TO THIS TODO's BASELINE: §6's blob was never stored in a UEFI variable, so there is no source side here to migrate from.
  - The item was filed while §6's storage backend was still undecided, and §7 recorded the migration as "deferred to §6's schema". §6 then shipped straight to TPM NV: `tpm_baseline_enroll`/`_verify` go through `tpm_nv_define_data`/`tpm_nv_write`/`tpm_nv_read` against the owner-auth data index `TPM_NV_INDEX_BASELINE` (`src/kernel/tpm_baseline.c:222-309`; index at `include/kernel/tpm_nv.h:93`).
  - Verified absent rather than assumed: `src/kernel/tpm_baseline.c` contains no `uefi_set_variable`/`SetVariable` call, and no other writer stores THIS baseline blob in a UEFI variable.
  - **Deliberately NOT a corpus-wide verdict, and the first draft of this line wrongly was one.** A different golden record IS specified against UEFI Runtime Services and is still open: `04-drivers-hardware/TODO-04 §7` stores a kernel PCR[10] baseline in the `ImpossibleOS-KernelPCR` UEFI variable. That record is not §6's blob and this section does not close it; whether it should move to TPM NV is that section's call. -> XREF: [`04-drivers-hardware/TODO-04 §7`](../04-drivers-hardware/TODO-04-security-hardware.md) (item: "First boot (no baseline): write current PCR value to `ImpossibleOS-KernelPCR` UEFI variable via Runtime Services").
  - The chunked NV read/write the item expected to consume is also not needed yet: `struct tpm_baseline` is 412 bytes against `TPM_NV_MAX_DATA` 512 (`include/kernel/tpm_nv.h:105`), so one transaction carries it -- with only 100 bytes of headroom, which is why §16's schema growth has to plan its NV index rather than assume it fits. A chunked path belongs with whatever first outgrows the bound.
- [x] Commit: `"tpm: measured boot NV index storage"` -- superseded; the cohort ships as sections 15-18, each with its own commit.

Migrated, each now owned by its own section (records, not tasks -- the work is `- [ ]` at the new owner):

- Trusted enrollment PROVENANCE, from the stamped section 6 -> §15.
- Real bootloader + kernel image SHA-256 in the baseline, from the stamped section 6 -> §16.
- TPM NV write-lock / monotonic-counter anti-rollback for baseline rotation, from the stamped section 6 -> §17 (primitives) + §21 (index lifecycle) + §27 (record authorization).
- Monotonic / write-locked NV index for the A/B per-slot anti-rollback floor, from the stamped section 7 -> §21 + §27, the same mechanism with a second consumer, whose loader-side read is §22.
- Atomic `boot_integrity_report` per-PCR publication, from the stamped section 6 -> §18.

**Test checkpoint:** no code ships from this section. The not-applicable verdict is checkable by grep -- `src/kernel/tpm_baseline.c` has no UEFI variable call and its enroll/verify paths go through `tpm_nv_*`. The migrated items are covered by their new sections' checkpoints.

> **Notes:**
> - **What shipped:** a disposition, not code -- every cohort item traced to current source before routing, so the split rests on what the tree does rather than on the items' own descriptions.
> - **How it runs:** one item closed as not-applicable to §6's blob (narrowly, not corpus-wide); five migrated to §15-§18, which own the work and state what they do not re-cover.
> - **Downstream effects:** TODO-21 §8's Critical deferral now resolves against §21 + §27 + §22; reciprocal items filed in `04-drivers-hardware/TODO-04 §7` and `18-future-research/TODO-04 §2`, which produce the kernel digests §16 consumes.
> - **Canonical doc:** the per-section bodies of §15-§18; the review trail behind their current shape is in the stamp commit message.
> - **Scope boundary:** §14 owns the cohort record only. §17 and its 2026-08-17 split (§21, §22, §23, §27) carry the two items that are one mechanism; §16's NV-index migration is where §14's deferred chunked-NV question comes due.

> **Verified:** 2026-08-17 | split disposition, no code | 2/2 items | build OK (`=== BUILD OK ===`, unchanged tree) | lint rc 0 | todo-graph 10/10 | section-order clean | evidence: `tpm_baseline.c:222-309` enrolls/verifies through `tpm_nv_*` against `TPM_NV_INDEX_BASELINE` with no `SetVariable` in the file; the other five items are open `[ ]` at §15-§18
> **Quality reviewed:** 2026-08-17 | Codex 8x (adversarial, consistency, perf, re-adversarial x5) | 16H+8M fixed, 0 open | round 8 closed on an explicit approve | scope: N/A (roadmap text, no code surface; parity research skipped for the same reason)

---

## 15. Trusted Enrollment Provenance

> **Spawned-by:** §14 (split)

§6 gates baseline enrollment on `config.boot_mode == 2 && config.tpm_enroll` (`src/kernel/main/boot_interrupts.c:548-550`). Both fields are parsed verbatim from `boot.conf` on the ESP, so the gate proves ESP-write authority and nothing more: anyone who can add a line to a text file can make their own tampered boot the golden baseline that every later boot is measured against. §6's review recorded this as an accepted [H] finding, not a design choice.

Half the substrate exists, which the cohort item's own text ("owner: recovery-kind + console-input infra") did not know and which is why this is a section rather than a park. The bootloader publishes a recovery signal it DECIDED rather than read back: `BOOT_PATH_RECOVERY` (`include/kernel/boot_info.h:673`), `BOOT_REASON_RECOVERY_TRIGGER` (`boot_info.h:713`), `BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED` (`boot_info.h:804`) and `sticky_recovery_trigger` (`boot_info.h:1855`). Nothing gates enrollment on any of them today.

**The console half does NOT exist, and an earlier draft of this section wrongly claimed it did.** `boot_recovery.c` polls PS/2 scancodes (`kbd_poll_char`, lines 81-94), but `kbd_poll_begin` (lines 49-68) executes `cli` and masks the keyboard GSI with NO restore counterpart anywhere in the tree, and every caller runs `boot_recovery_act()` straight into reboot / power-off / halt. That lifecycle is TERMINAL by construction: it never has to give the machine back. A confirmation prompt must return and continue booting on all three outcomes, so the reusable primitive is a prerequisite this section builds, not substrate it inherits.

**Ownership split with §10, which owns an overlapping open item.** §10's `- [ ]` "Allow trusted baseline reset only from recovery mode with local confirmation" covers RESET / rotation UX. This section covers the ENROLLMENT authority question -- which signal is trusted to authorize a write at all. They share the console primitive below and nothing else. -> XREF: §10 (item: "Allow trusted baseline reset only from recovery mode with local confirmation").

- [x] Built a RETURNING console-confirmation primitive (`boot_confirm_prompt` / `boot_confirm_wait_keys` in `src/kernel/main/boot_confirm.c`) bounded by a calibrated-TSC deadline rather than a spin count.
  - A spin budget is a different duration on every machine, which is why the old recovery poll could not be reused as-is for a bounded prompt.
  - Two IRQ modes, and picking the wrong one is the bug the header exists to prevent. `BOOT_CONFIRM_IRQ_TAKE_OVER` saves the interrupt flag plus the IOAPIC *and* PIC keyboard routes and restores each to what it ACTUALLY was on every exit path; the new read-back accessors `ioapic_irq_masked()` / `pic_irq_masked()` are what make that possible, because unmasking blindly would enable a line that was already masked before the call ran.
  - `BOOT_CONFIRM_IRQ_LEAVE_ALONE` touches nothing, which is correct at the Phase 1 enrollment site and is the design the review corrected. IOAPIC entries start masked (`src/kernel/drivers/ioapic.c:295`) and `keyboard_init()` claims IRQ1 only at `src/kernel/main/boot_interrupts.c:588`, AFTER the gate, so nothing races for port 0x60 and leaving interrupts ENABLED keeps the splash spinner animating and the watchdog petted through the prompt.
  - Prompt bounded at 15s, well under the 60s WDAT floor (`src/kernel/drivers/watchdog.c:30-33`), and it pets the watchdog while polling. `boot_recovery.c` was refactored onto the same primitive rather than copied, so the tree has ONE PS/2 poll lifecycle; its terminal callers still halt by choosing to, not because the primitive cannot return.
  - A `boot_confirm_clock_t` policy splits the two callers, which round 2 of the adversarial review forced: an uncalibrated TSC is a supported state (`src/kernel/boot_timing.c:247-250` warns and continues), and applying the security rule to both made the degraded-boot recovery menu halt instantly on exactly the machines that need it. Security uses `CLOCK_REQUIRED` (refuse rather than guess a duration); recovery uses `CLOCK_BEST_EFFORT` (a coarse iteration bound beats no menu).
  - The yes/no prompt accepts ANY decoded key and treats every non-affirmative one as a decline. Passing only `{yes,no}` was a real defect: Escape and the menu keys fell outside the set, so a present operator pressing them was ignored until the deadline and then logged as `CONFIRM_TIMEOUT` -- fail-closed either way, but the security log said a human was absent when they had actively refused. There is deliberately no unbounded-wait mode, and `timeout_ms == 0` is rejected as an invalid argument BEFORE the clock policy is consulted, so best-effort cannot silently swallow it.
  - The confirmation lands on key RELEASE, not the press, which round 7 forced and which is a security property rather than a UI choice: draining the buffer proves nothing about whether a key is DOWN, and PS/2 typematic repeat keeps emitting fresh make codes, so a taped or pre-held `y` could authorize an enrollment nobody answered. Requiring the matching break of a key whose make we saw makes a stuck key unable to confirm at all. Residual stated in the source rather than hidden: a key held when the prompt appears and then released still answers, which is a human action in the prompt window.
  - The question is rendered on the boot SPLASH as well as serial, which round 3 forced and which the feature could not work without: a confirmation exists for an operator standing at the machine, and a serial-only prompt is invisible to exactly that person -- they press nothing and are logged as an absent timeout. The 15s bound covers the key wait, not the output before it; unbounded boot-path serial is a pre-existing whole-path property and is parked below rather than papered over here.
- [x] Gated enrollment on `tpm_enroll_gate_evaluate()` (`src/kernel/tpm_enroll_gate.c`), a pure total predicate, replacing the `boot_mode == 2 && tpm_enroll` test at `src/kernel/main/boot_interrupts.c:548`.
  - The loader-decided `boot_path` / `boot_reason` / `boot_source_flags` are checked but are NOT the anchor, which is the correction the design review forced. All three are derived from the selected boot ENTRY KIND by `boot_policy_kind_to_path()` (`src/boot/uefi/boot_policy.c:268-296`), and the entry store is the ESP file `\EFI\ImpossibleOS\bootentries.json`, so gating on them alone would have swapped one ESP-spoofable gate for another.
  - The anchor is the one thing an ESP writer cannot forge: `sticky_present` + `sticky_recovery_trigger`, read from the UEFI NVRAM variable `ImpossibleOS-BootSticky` and written into `boot_info` by `boot_sticky.c:143` independently of the ladder. `audit_degraded` refuses first, honouring boot_info.h's rule that a degraded sticky read must be treated as all-zero.
  - `selection_reason` is deliberately NOT required to be `BOOT_SELECTION_RECOVERY_REQUEST`, and the first implementation that required it was a live defect the adversarial review caught: that reason is assigned at exactly one site (`src/boot/uefi/boot_policy.c:470`) inside a ladder branch whose RECOVERY candidates the kind filter has already rejected, so it can never be published today and the gate would have made enrollment IMPOSSIBLE on every supported boot path -- an insecure capability replaced by a dead one.
  - Instead the derived record is a CONDITIONAL coherence check: when the ladder did publish `RECOVERY_REQUEST`, `boot_path` / `boot_reason` / the source flag must agree with it or the boot is refused. Relaxing it costs nothing against the spoof, because a store-default recovery entry sets those three from the entry KIND but cannot set the NVRAM trigger, so it is already refused upstream. Asserted both ways in the suite.
  - The replay check is a WHITELIST (`replay_known && verdict == TPM_REPLAY_VERIFIED`), also a review fix: the first version only inspected the verdict inside `if (replay_known)`, so a boot whose replay never ran -- exactly what the caller leaves when `tpm_replay_verify()` returns non-OK -- reached the admitting arm, as did any out-of-range encoding.
  - `whole_chain_verified` is a SEPARATE input from `secure_boot_enabled`, which round 4 raised as Critical and which is the sharpest correction in the section. Secure Boot being on does not mean the RUNNING KERNEL was covered by it: on the legacy split path the firmware verifies `BOOTX64.EFI` and the loader then reads an unsigned `kernel.exe` off the ESP (`src/boot/uefi/bootx64.c:7894-7896`, `12281-12283`). Checking only Secure Boot would have stamped an ESP-swapped kernel `local-console-on-trusted-chain` -- precisely the lie the authority value exists to prevent. The caller sets it from `BOOT_FLAG_INVOKED_VIA_UKI`, and a split boot now refuses with the distinct `kernel-unverified`.
- [x] Required console confirmation before any NV write, fail-closed: timeout, wrong key and no-usable-console are three DISTINCT refusals, and an unknown confirm value is read as a decline so the predicate has no admitting default.
  - Also refuses `BOOT_CHAIN_UNTRUSTED` when Secure Boot is inactive, and `REPLAY_TAMPER` / `REPLAY_UNVERIFIABLE` from the replay verdict computed immediately above the gate: enrolling a boot whose event log already disagrees with the hardware PCRs would promote the tampered measurement to golden.
  - The prompt is drawn ONLY on `needs_confirm`, which the predicate sets after every non-operator condition has already passed, so an ordinary boot stays silent and never waits on a keypress.
- [x] Reported the enrollment AUTHORITY beside every verdict: `none` / `esp-config-only` / `loader-signal-only` / `local-console-on-trusted-chain`. Only the last authorizes, so a config-only path can never read as trusted.
  - Named `local-console-on-trusted-chain` rather than anything claiming physical presence, deliberately: a keypress proves console INPUT, and firmware, a hypervisor or a BMC KVM can inject one. Real physical presence needs the TCG Physical Presence Interface.
- [x] Unit-tested the gate as a pure predicate: 17 suites in `src/kernel/test/test_tpm_enroll_gate.c` (TEST_CAT_SECURITY) asserting every refusal reason, both spoof directions, refusal PRECEDENCE, exact label mapping, and NULL-input fail-closed.
  - Wiring proven end-to-end rather than assumed: an assertion was deliberately broken and the suite went `FAIL: 1 of 1291 failed`, then reverted. A green suite that never ran the new tests would have looked identical without that control.
  - Two suites exist because the Codex test-coverage pass showed the original set was constant-tolerant. Single-fault fixtures could not pin the documented refusal PRECEDENCE (swapping two adjacent checks passed everything), and the label test would have passed an implementation returning one arbitrary string for every valid value -- which would make every security-log refusal indistinguishable while staying green. Dual-failure and exact-mapping tables close both.
- [x] Annotated §6's accepted [H] provenance stamp RESOLVED (kept, not deleted). -> XREF: §6 (the "Accepted: [H] enrollment gate is boot.conf config" stamp).
- [/] PARKED, owner §10 (recovery UX): make the recovery power-off flush storage before cutting power, now that the poll primitive restores the caller's interrupt state instead of guaranteeing `cli`.
  - `boot_recovery_act()` uses the quiesce-free `acpi_poweroff_now()` / `acpi_reset_now()` because `acpi_storage_quiesce()` sleeps via `hlt` and hangs forever with interrupts disabled. That constraint is now softer but NOT gone: the restore puts back whatever the failing boot happened to have, which is not guaranteed to be IF=1. The call belongs to the recovery UX owner, so it is filed rather than smuggled into this section. -> XREF: §10 (item: "Allow trusted baseline reset only from recovery mode with local confirmation").
- [/] PARKED, owner §10 (recovery UX): give the boot path a BOUNDED serial-output call, so a wedged UART cannot stall a prompt or status line that advertises a timeout.
  - Found by adversarial review round 3. `serial_write()` spins unbounded waiting on THRE (`src/kernel/drivers/serial.c:41` says so explicitly; the loop is at `serial.c:699`), so the confirmation prompt's 15s bound covers the key WAIT and not the output before it. Deliberately NOT patched locally: every klog and splash call on the boot path shares the property, so a bound inside one function would be theatre while the boot still hangs a line later. `serial_write_emergency()` is bounded but is the panic-path budget and is not the right consumer here. The header now states what the timeout actually bounds instead of overclaiming. -> XREF: §10 (item: "In recovery, explain whether firmware, bootloader, kernel, Secure Boot db, or baseline changed").
- [/] PARKED, owner §10 (recovery UX): quiesce and resynchronize the PS/2 mouse across a console take-over, REQUIRED before any non-terminal caller uses `BOOT_CONFIRM_IRQ_TAKE_OVER` after `mouse_init()`.
  - Found by adversarial review round 2. Masking IRQ12 stops the mouse DRIVER but not the DEVICE, and the poll discards the auxiliary bytes it sees, so the driver's 3-byte packet cursor is not guaranteed to be where it was when IRQ12 is restored. Nothing observes this today because the only take-over caller is the recovery screen, which is terminal on every path, and `boot_confirm.h` now states that limit rather than promising post-`mouse_init` reuse is safe.
  - The fix is device-level (disable reporting, drain, reset the driver's `mouse_cycle`, re-enable) and belongs with whoever builds the first returning recovery console; doing it blind now would add an untested 8042 sequence to a path no caller exercises. -> XREF: §10 (item: "In recovery, explain whether firmware, bootloader, kernel, Secure Boot db, or baseline changed").
- [ ] Provision the authorized anti-rollback anchors: define them under the §27 authPolicy and make the FIRST authorized write. -> XREF: 01-boot-platform/TODO-13 §27 (item: "The A/B floor is an authenticated DATA record").
  - §27 ships TRANSITIONS between existing records and deliberately refuses to invent a first one: an advance that wrote a fresh floor over a missing record could not tell an unprovisioned anchor from a destroyed one, which is the laundering the whole section exists to stop.
  - The ordering has to INVERT for the first write, and that is why it is a design question rather than a missing call. A fresh `TPM_NT_COUNTER` initializes to the TPM's largest-ever NV counter value on its first increment, so the generation the first record must carry is unknowable until AFTER that increment; write-then-increment cannot be used to establish the sequence it depends on.
  - That also means the offline authority cannot pre-sign the first record, since its cpHash covers bytes containing a generation nobody knows yet. Enrollment authority is this section's subject, so the first-provisioning authorization belongs here rather than with the transition layer.
  - Until it lands, a machine with an authority key compiled in has the boundary but no way through it, and `tpm_baseline_verify` reports `TPM_BASELINE_UNBOUND`. Found by §27's re-adversarial round.
- [ ] BLOCKING: build a trusted producer that can SET the sticky recovery trigger, without which this gate can never admit and no baseline can ever be enrolled.
  - Found by the post-commit adversarial pass and verified across every reference: `src/boot/uefi/boot_sticky.c:143` READS `recovery_trigger` out of the NVRAM record, `:46` and `:83` zero it, and `src/kernel/main/boot_audit.c:470-471` can only PRESERVE an existing 1 or clear it. Nothing in the tree ever sets it, the bootloader is read-only by contract, and user-mode writes to `ImpossibleOS-BootSticky` are denied -- so `sticky_recovery_trigger` is always 0 and the predicate always refuses at `sticky-trigger-clear`.
  - This is the SAME class of mistake as the earlier `selection_reason` defect and it is why the section is `[/]` rather than `[x]`: the authority DECISION shipped, is correct and is tested, but the capability it gates cannot be exercised end to end. The producer needs an authenticated operator path (recovery UI or an authorized runtime write), which is a real design decision and not a one-line addition.
- [ ] Feed the gate a validated `secure_boot_enforcing` input rather than the raw SecureBoot bit, which reads 1 in SetupMode and AuditMode where signatures are not enforced.
  - `uefi_runtime.c:1251-1322` publishes `secure_boot_enabled` from the `SecureBoot` variable alone, with no `SetupMode` / `AuditMode` qualification and no enrolled-PK check, so the gate's chain check is weaker than it reads. Needs those firmware states plumbed into `boot_info` first, plus a fixture per degraded state.
- [ ] Consume the recovery trigger durably after a successful enrollment, one-shot, so a seeded trigger cannot authorize a second enrollment on a later boot.
  - `boot_audit_classify_ack` clears the trigger only when `selection_reason == RECOVERY_REQUEST` (`boot_audit.c:407-455`), which is exactly the path this gate deliberately does NOT require, and a successful `tpm_baseline_enroll()` does not consume it either. Once a producer exists (item above), the trigger would otherwise stay live indefinitely.
- [ ] Hard-skip the legacy 8042 probe on hardware-reduced ACPI platforms, matching the rule `keyboard.c:309-312` already sets for the same controller.
  - `confirm_console_present()` reads port 0x64 with no `acpi_hw_reduced()` gate, making this the only code in the tree that touches the legacy 8042 ports on a hardware-reduced platform, on the Phase 1 boot path. Expected to be a benign 0xFF read, but it violates a rule the tree states explicitly.
- [ ] Give the Phase 1 confirmation prompt its own POST16 entry/exit so a wedge inside it does not present as a TPM transport hang on a POST card.
  - The prompt can block Phase 1 for up to 15s between `POST16_TPM_TRANSPORT` and `POST16_TPM_TRANSPORT_OK`, so on a POST card any stall inside it is indistinguishable from a TPM transport hang. Gate 4 requires POST16 on boot-path work and the `0xDD00-0xDFFF` range has free codes.
- [ ] Expose the `boot_confirm.c` pure helpers for unit test: the scancode decode that turns 0x15 into the `y` authorizing a baseline write is currently untestable.
  - `confirm_scancode_to_ascii`, `confirm_key_accepted` and `confirm_budget`'s overflow-checked multiply are all file-static, so the decode gating a security decision has zero coverage. The test policy rightly forbids driving the live prompt from a test, which is the argument for exposing the pure parts rather than accepting the gap.
- [ ] Consolidate the duplicated i8042 port, status-bit and scancode definitions now spread across `boot_confirm.c`, `keyboard.c` and `mouse.c` into one shared header.
  - `boot_confirm.c` defines its own `PS2_DATA_PORT` 0x60, `PS2_STATUS_PORT` 0x64, `PS2_STATUS_OBF` 0x01, `PS2_STATUS_AUX` 0x20 and `PS2_BREAK_CODE` 0x80 alongside the drivers' copies, and its `confirm_scancode_to_ascii` is a second, smaller scancode map. The values agree today; the risk is drift, and the AUX bit in particular is now load-bearing for a security decision.
- [x] Commit: `"tpm: loader-validated + console-confirmed baseline enrollment"`

**Test checkpoint:** the gate refuses a boot carrying a perfect-looking recovery decision record (`boot_path` RECOVERY, `boot_reason` RECOVERY_TRIGGER, RECOVERY_TRIGGERED source flag, `tpm_enroll` set) whose NVRAM sticky record is absent -- the exact ESP-only spoof §6 accepted, and the one an earlier draft would have re-admitted by trusting those three derived fields. It refuses the reverse shape too: a claimed `RECOVERY_REQUEST` selection with a coherent record but no sticky backing. It ADMITS the shape today's loader actually produces -- a real NVRAM trigger the ladder could not act on, so the selection is `STORE_DEFAULT` and the record describes a normal boot -- because requiring the ladder's reason would refuse every boot on every supported path; the same tuple with the trigger cleared is still refused, which is what makes that relaxation safe. Replay is whitelisted: `replay_known == 0` and out-of-range verdicts refuse, not just TAMPER/UNVERIFIABLE. It refuses when `audit_degraded` is set without reading the sticky fields at all, refuses an untrusted boot chain, and refuses on timeout, wrong key and unavailable console as three distinct values. An unknown `confirm` value is a decline and NULL inputs fail closed rather than fault. Labels are total, with a control asserting a VALID value does not report `unknown` -- without it the totality assertions would pass if every label returned `unknown`. Scope: this section does NOT re-cover baseline CONTENT, rotation/reset UX (§10), or the NV mechanism (§6, §7, §17, §21). Platforms: kernel unit suites cover the predicate end to end; the console primitive's PS/2 and IOAPIC/PIC save-restore paths are hardware and are exercised on QEMU boot, while the live enroll cycle is QEMU-swtpm / bare-metal and operator-gated (no `swtpm` on the dev host).

> **Notes:**
> - **What shipped:** a pure total enrollment-authority predicate (`tpm_enroll_gate.c`) plus a returning console-confirmation primitive (`boot_confirm.c`), replacing a boot.conf-only gate that proved ESP write access and nothing else.
> - **How it runs:** the predicate anchors on NVRAM-backed signals (`sticky_*`, `selection_reason`), requires Secure Boot and a clean replay verdict, and admits only after a keypress; the prompt is drawn only on `needs_confirm`.
> - **Downstream effects:** §6's accepted [H] provenance finding is annotated RESOLVED; `ioapic_irq_masked()` / `pic_irq_masked()` are new read-back accessors; `boot_recovery.c` now shares the one PS/2 poll lifecycle.
> - **Canonical doc:** the trust model in `include/kernel/tpm_enroll_gate.h`; the two IRQ modes and why they differ in `include/kernel/boot_confirm.h`.
> - **Scope boundary:** this section owns WHO may enroll. Baseline content is §16, the NV mechanism is §17/§21, the headless escape hatch is §23, and reset/rotation UX stays §10.
> - **Not yet reachable:** the decision shipped and is tested, but nothing in the tree SETS the sticky recovery trigger it anchors on, so no enrollment can complete end to end; §15 stays `[/]` until the producer lands.

> **Verified:** 2026-08-17 | commit `625d3a48b` | 7/17 items (3 parked, 7 open) | build OK (`=== BUILD OK ===`) | lint rc 0 | SUITE=security 1315 kernel + 17 user-mode PASS | smoke matrix 4/4 legs (kvm/tcg x 1/2 cpu) | evidence: single admitting path at `tpm_enroll_gate.c:219` (1 of 18 result-setting calls covering all 19 returns); every early return in `boot_confirm_wait_keys` lies outside the `confirm_irq_begin`/`confirm_irq_end` window (380/390/393/399 before, 421/424 after); test wiring proven by a deliberate assertion break (`FAIL: 1 of 1291`) then revert
> **Deferred:** [Critical] no in-tree producer SETS `sticky_recovery_trigger`, so the gate always refuses at `sticky-trigger-clear` and enrollment is unreachable end to end (reason: needs an authenticated operator path, a design decision not a one-line fix) -> XREF: 01-boot-platform/TODO-13 §15 (item: "BLOCKING: build a trusted producer that can SET the sticky recovery trigger")
> **Deferred:** [H] `secure_boot_enabled` is the raw SecureBoot bit, which reads 1 under SetupMode/AuditMode where signatures are not enforced (reason: needs those firmware states plumbed into boot_info first) -> XREF: 01-boot-platform/TODO-13 §15 (item: "Feed the gate a validated `secure_boot_enforcing` input")
> **Deferred:** [H] a seeded recovery trigger is never consumed after a successful enroll, so it would remain replayable on later boots -> XREF: 01-boot-platform/TODO-13 §15 (item: "Consume the recovery trigger durably after a successful enrollment")
> **Deferred:** [M] the 8042 probe is not gated on `acpi_hw_reduced()`, against the rule `keyboard.c:309-312` sets for the same controller -> XREF: 01-boot-platform/TODO-13 §15 (item: "Hard-skip the legacy 8042 probe on hardware-reduced ACPI platforms")
> **Deferred:** [M] the Phase 1 prompt has no POST16 of its own, so a stall inside it reads as a TPM transport hang -> XREF: 01-boot-platform/TODO-13 §15 (item: "Give the Phase 1 confirmation prompt its own POST16 entry/exit")
> **Deferred:** [M] `boot_confirm.c`'s pure helpers are file-static and therefore untested, including the scancode decode that gates the write -> XREF: 01-boot-platform/TODO-13 §15 (item: "Expose the `boot_confirm.c` pure helpers for unit test")
> **Deferred:** [M] i8042 port/status/scancode constants are duplicated between `boot_confirm.c` and the keyboard/mouse drivers, so a bit definition can drift between them -> XREF: 01-boot-platform/TODO-13 §15 (item: "Consolidate the duplicated i8042 port, status-bit and scancode definitions")
> **Quality reviewed:** 2026-08-17 | Codex 10x (design, adversarial x7, consistency, perf, test-coverage) | 1C+8H+6M fixed, 1C+2H+4M deferred | scope: kernel-code-quality + kernel-quality-auditor + concurrency-evidence-mapper

---

## 16. Baseline ABI-Manifest Identity -- Populate the Reserved Digest

> **Spawned-by:** §14 (split)

The baseline records what §6 could reach: per-bank PCR digests, Secure Boot state, a firmware-version hash, and a RESERVED `abi_manifest` field. Two things about that field are load-bearing and both were stated wrongly in this section's first draft. It is not an image hash -- `boot_proto_descriptor.sha256` is a BUILD-TIME hash of `build/boot-info-abi.kernel.json`, baked into `build/boot_proto_sha.h` by `tools/boot-info-manifest/gen-proto-sha-header.sh`, so it tracks the ABI manifest and stays put when the kernel image changes underneath it. And it is not populated: `tpm_baseline_snapshot()` sets `abi_manifest_present = 0` because no kernel-side accessor is exposed (`src/kernel/tpm_baseline.c:202-206`). The struct reserves the field and nothing fills it.

**Scope was narrowed 2026-08-17 by a SPLIT-RECOMMENDED verdict, and the narrowing is what makes the section shippable.** The original §16 carried eight items spanning the UEFI loader, the `boot_info` ABI, and a crash-safe NV index resize. Three of those were not reachable at all: both "consume the kernel digest from its owner" items XREF `[ ]` items in other TODOs that have no code behind them (`kernel_measure()` is defined nowhere in `src/`), and the struct-growth item depends on §17's anti-rollback state, which has not shipped. What remains here is the one digest the struct ALREADY has room for, which needs no struct growth, no `BOOT_INFO_VERSION` bump, and no NV migration. The rest moved to §19 (versioned growth + NV migration) and §20 (the `BOOTX64.EFI` loader digest).

**This section does NOT compute the KERNEL hashes, and an earlier draft of it did -- which duplicated two open owners elsewhere in the corpus.** Kernel image measurement is `04-drivers-hardware/TODO-04 §7` (SHA-256 over `.text`/`.rodata`, PCR[10] extend, its own golden record). The bootloader-side hash of the loaded kernel ELF before ExitBootServices is `18-future-research/TODO-04 §2` (PCR 8). Neither carries a digest into §6's measured-boot baseline blob, and neither measures the LOADER.

- [x] Exposed `boot_proto_abi_digest()` in `src/kernel/main/boot_proto.c` (declared in `include/kernel/boot_proto_descriptor.h`), so `tpm_baseline.c` reads the build-time digest without including the generated header.
  - The generated `build/boot_proto_sha.h` stays a dependency of that ONE TU. `BOOT_PROTO_ABI_DIGEST_LEN` is `_Static_assert`ed against the descriptor field, and again in `tpm_baseline.c` against `TPM_BASELINE_DIGEST`, so the two lengths cannot drift.
  - Answered the open question FAIL-CLOSED: bad magic or an all-zero digest returns 0 and zeroes the output, and the caller treats 0 as a hard failure. The digest is a compile-time constant the build cannot link without, so 0 means corrupt read-only data, never "absent optional field".
  - `boot_proto_abi_digest_from()` is the pure seam over a caller-supplied descriptor. Added because `kernel_boot_proto` is const and linked, so the corruption branches -- the actual safety property -- were otherwise untestable.
- [x] `tpm_baseline_snapshot()` populates `abi_manifest` and sets `abi_manifest_present = 1`, replacing the unconditional `abi_manifest_present = 0`.
  - It returns `TPM_BASELINE_SELF_CORRUPT` when the accessor fails rather than enrolling a baseline with the identity dropped, and `tpm_baseline_verify()` publishes `BOOT_INTEGRITY_MISMATCH` for that status. Deliberately narrow: `NO_TPM` and `BADARG` still leave the status unpublished, because a machine that cannot measure is not a machine that failed to match.
  - The check runs BEFORE the PCR reads in snapshot and BEFORE the NV read in verify, which the post-ship adversarial review forced. Behind either one, an absent baseline or an unreadable PCR returned first and the corruption was never reported -- and both are the COMMON state on a machine that has never enrolled or has no TPM, so the masking covered the normal case rather than a corner.
  - `TPM_BASELINE_SELF_CORRUPT` is a SEPARATE status from `TPM_BASELINE_CORRUPT`, not a reuse. The two are operationally opposite: a corrupt stored blob invites "offer a re-enroll", which is exactly the wrong response to a corrupt kernel descriptor because it would enroll the corruption as the new golden.
  - Both publication branches in `boot_interrupts.c` handle it. Verification runs only under `!admit`, so an enrolling boot previously logged the corruption and carried on under whatever status preceded it.
- [x] Routed the attestation export through the same accessor (`src/kernel/tpm_attest_report.c`), which had been copying `kernel_boot_proto.sha256` directly and exporting a corrupt descriptor as a VALID manifest identity.
  - Found by the post-ship consistency audit: the header contract said callers must treat a 0 return as a hard failure, and this consumer never called the accessor at all. It now returns `TPM_ATTEST_SELF_CORRUPT` instead of shipping zeros a verifier could mistake for a digest.
  - Its `manifest_sha256[32]` was a third independent length literal; it now carries a `_Static_assert` against `BOOT_PROTO_ABI_DIGEST_LEN`, and its comments no longer call the ABI-manifest hash a kernel-IMAGE identity, which contradicted this section's load-bearing distinction.
- [x] Pinned the generated-header length with a `_Static_assert` over `KERNEL_ABI_SHA256`'s own initializer count, the one link a C declaration cannot pin.
  - Array initialization zero-fills a short generated header with no diagnostic at any warning level, and the resulting truncated-plus-zeroes digest still has non-zero bytes, so it would sail past the accessor's all-zero canary and be enrolled as a real identity. Proven by deliberately breaking the assert and observing the build fail with its own message.
- [x] Fixed the compare path: the `abi_manifest` presence gate is now SYMMETRIC, and the difference from the fw-hash gate is documented at both sites.
  - Decided the asymmetric cases rather than inheriting the previous behavior. BOTH orientations are a MISMATCH. `fw_hash` stays one-way on purpose because SMBIOS can be genuinely absent, whereas every correctly built kernel carries the ABI digest, so a golden without one can only be a pre-binding baseline.
  - `tpm_baseline_validate()` also refuses a CRC-valid blob claiming `abi_manifest_present = 1` with an all-zero digest, a shape no honest enroll can produce.
- [x] Commit: `"tpm: populate the baseline ABI-manifest digest"`

**Test checkpoint:** the distinction the field exists to make is proven by the compare path: a golden baseline whose manifest digest differs from the current one is a MISMATCH, while the SAME digest matches even though this section changes no kernel image. Both asymmetric present/absent orientations assert their stated verdict, with a control asserting that two populated, equal manifests still MATCH -- without it the asymmetry assertions would pass against a compare that rejected everything. A CRC-valid blob claiming a present but all-zero manifest is refused by validate, while the legacy `present == 0` blob still validates so compare (not validate) is what reports it. The accessor's fail-closed branches are driven through `boot_proto_abi_digest_from`: bad magic, an all-zero digest and NULL each return 0 AND leave a prefilled output buffer wiped, with a control proving a well-formed synthetic descriptor still succeeds and a last-byte case proving the zero scan covers the whole field. The generated-header length is a compile-time error, verified by deliberately breaking the assert and observing the build fail with its own message. Scope: this section owns ONLY the ABI-manifest digest and its compare semantics. Struct growth, the NV index migration, and the two externally-produced KERNEL digests are §19; the `BOOTX64.EFI` loader digest is §20; the PCR-11 manifest extend and attestation report schema stay §9's. Platforms: kernel unit suites cover the PURE surface end to end. **The live wrappers are NOT covered here** -- `tpm_baseline_snapshot` / `enroll` / `verify` need a fake-TIS fixture plus a descriptor-injection seam that does not exist, so the `SELF_CORRUPT` propagation and the MISMATCH publication are reasoned and reviewed but not asserted; that fixture is filed in §19 rather than claimed here.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 30198 kernel + 17 user-mode full-suite, 0 failures (+28 assertions across 4 new suites in `test_tpm_baseline.c`)

> **Notes:**
> - **What shipped:** `boot_proto_abi_digest()` + its pure `_from()` seam, a snapshot that populates the reserved `abi_manifest` field, and a symmetric presence gate in `tpm_baseline_compare()`.
> - **How it integrates:** the identity check runs before the PCR reads in snapshot and before the NV read in verify, so a corrupt descriptor cannot be masked by NO_TPM or NO_BASELINE; both publication branches in `boot_interrupts.c` report it.
> - **Downstream effects:** a baseline enrolled before this ships compares as MISMATCH rather than silently VERIFIED, and §19 owns the authorized upgrade path because re-enrollment cannot currently repair it.
> - **Canonical doc:** the fail-closed contract in `include/kernel/boot_proto_descriptor.h`; the two presence rules and why they differ in `include/kernel/tpm_baseline.h`.
> - **Scope boundary:** this section owns the ABI-manifest digest only. Struct growth and the NV migration are §19, the `BOOTX64.EFI` loader digest is §20.
> - **Not covered:** the live wrappers have no test caller (no fake-TIS fixture, no descriptor-injection seam), so `SELF_CORRUPT` propagation is reviewed but not asserted; filed in §19.

> **Verified:** 2026-08-17 | commit `739a0a004` + review fixes | 6/6 items | build OK (`=== BUILD OK ===`) | lint rc 0, 0 errors | 30198 kernel + 17 user-mode PASS | smoke matrix 4/4 legs (kvm/tcg x 1/2 cpu, 250s) | evidence: the generated-length `_Static_assert` proven by deliberately breaking it and observing `generated KERNEL_ABI_SHA256 must carry exactly BOOT_PROTO_ABI_DIGEST_LEN bytes`, then reverting; test wiring proven by the security-suite deltas 1315 -> 1331 -> 1343 matching +16 and +12 exactly
> **Accepted:** [H] an owner-writable NV baseline is still forgeable into VERIFIED, since CRC is integrity and not authenticity (reason: authenticating the record is §27's authorization boundary; binding another field cannot fix an unauthenticated index) -> XREF: 01-boot-platform/TODO-13 §27 (item: "Consumer: baseline rotation gets the SAME authorization boundary as the floor record" at line 845)
> **Deferred:** [H] a pre-binding baseline is now a permanent MISMATCH with no reachable re-enrollment path (reason: needs the §15 authority plus an atomic migration, neither of which exists) -> XREF: 01-boot-platform/TODO-13 §19 (item: "Provide a reachable, authorized upgrade for a PRE-BINDING baseline")
> **Deferred:** [M] the live baseline wrappers have no test caller, so `SELF_CORRUPT` propagation and the MISMATCH publication are unasserted (reason: needs a fake-TIS fixture and a descriptor-injection seam) -> XREF: 01-boot-platform/TODO-13 §19 (item: "Build the fake-TIS fixture and descriptor-injection seam the live baseline wrappers need")
> **Deferred:** [M] every compare branch collapses to one undifferentiated MISMATCH, so a known migration is indistinguishable from tamper (reason: report-schema work owned by the attribution section) -> XREF: 01-boot-platform/TODO-13 §25 (item: "Attribute a mismatch to the FIELD that differed, not only to a PCR") (RETARGETED 2026-08-17: §18 split the scalar-field attribution out to §25)
> **Deferred:** [M] the `"verified"` status string does not yet mean the kernel IMAGE was verified (reason: the image digests are unimplemented in their owning TODOs) -> XREF: 01-boot-platform/TODO-13 §25 (item: "Scope what the `\"verified\"` status string actually claims") (RETARGETED 2026-08-17: §18 split the status-string scoping out to §25)
> **Quality reviewed:** 2026-08-17 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) | 3H+4M+2L fixed, 5 open | scope: kernel-code-quality (kernel-quality-auditor + parity-research-analyst)

---

## 17. Write-Locked and Monotonic NV Index Primitives

> **Spawned-by:** §14 (split)

**Scope was narrowed 2026-08-17 by a SPLIT-RECOMMENDED verdict (11 work items + ABI impact), and the narrowing is what makes the section shippable.** The original §17 carried one mechanism, two indexes, two consumers, a TPM policy-authorization construction and a bootloader-side read path: five distinct failure modes spanning the kernel, the UEFI loader and the NV wire format. What stays here is the WIRE and TRANSPORT layer -- the attributes and NV type a define can request, the two missing commands, their classification, and the boot-path command budget. One failure mode (wrong bits on the wire, or a locked index wedging the transport), fully testable through the fake-TIS seam with no consumer in play. The trust-anchor index lifecycle moved to §21 and its record authorization to §27, the bootloader floor read to §22, and the headless enrollment escape hatch to §23.

§7 had modelled only the attributes it needed: OWNER/AUTH/POLICY read and write, `NO_DA`, and the read-only `WRITTEN` status, with no `WRITEDEFINE`, no `WRITE_STCLEAR`, no NV-type counter, and no command code for either `TPM2_NV_WriteLock` or `TPM2_NV_Increment`. Nothing above this layer was buildable until a define could ask for a counter or a write-lockable index at all, which is why this was the first of the four sections rather than the interesting one.

- [x] Modelled the WHOLE TPMA_NV table + the TPM_NT type field in `tpm_nv.h`, and CORRECTED the read bits: OWNERREAD/AUTHREAD/POLICYREAD sat at 18/19/20 against the spec's 17/18/19.
  - The old numbering made `tpm_nv_define_data` request AUTHREAD while `tpm_nv_read` authorized with owner auth, and made `tpm_nv_define_baseline` set Reserved bit 20 in place of POLICYREAD, so the baseline index asked for NO read permission. Both are refusals on conforming firmware, invisible here because the dev host has no `swtpm`.
  - Nothing caught it because every assertion compared the macros against themselves. The fixtures now carry spec-literal attribute words as an INDEPENDENT oracle, which is what makes the correction stick.
  - `tpm_nv_attrs_valid()` (pure) rejects reserved bits, TPM-maintained status bits, an index nobody may read or write, undefined TPM_NT values, wrong per-type sizes, CLEAR_STCLEAR on a counter, and the platform-hierarchy attributes an owner-auth define cannot request. PIN types are refused outright rather than half-validated.
- [x] Added `TPM2_NV_Increment` + `TPM2_NV_WriteLock` builders over one shared two-handle marshaller, with `tpm_nv_increment` / `tpm_nv_write_lock` / `tpm_nv_read_counter` wrappers and `TPM2_RC_F1_ATTRIBUTES` classified to a new `TPM_NV_ATTRS`.
  - Counter reads take exactly 8 bytes at offset 0 and decode only on an exact-length read; `TPM_NV_UNINIT` is propagated, never synthesized as zero, because a recreated index's first increment can land above a previous value and conflating the two is how a rollback launders itself.
- [x] Bounded the boot-path cost with ONE cumulative budget across every command in an operation: `tpm2_seq_run(work, cleanup, fn, ctx)` in the transport, plus `nv_exec_bounded()` so even single-command wrappers are covered.
  - Ownership is an opaque generation TOKEN, not a CPU: a raw begin/end pair lets an end-after-failed-begin release somebody else's sequence, and a CPU check both admits same-CPU reentrancy and rejects the owner after a thread migration.
  - Budget expiry is RECOVERABLE (`TPM_T_ERR_BUDGET` + a bounded interface quiesce) rather than sticky-failing the transport. A slow-but-responsive TPM is a real machine, and poisoning it disabled every later TPM operation over one slow command.
  - A mandatory teardown runs on its OWN allowance, saved and restored around the work budget. Three earlier shapes were shipped and reverted in review: replacing the budget eagerly discarded a live one, replacing it only after expiry let a teardown die mid-FlushContext, and extending it leaked reserved time into the irreversible NV_DefineSpace that followed a mid-sequence flush.
  - No fixed number can be a true PTP worst case here (the TIS restarts its burst deadline on every chunk), so the budget is documented as a POLICY statement about how long this boot will wait.
- [x] Session teardown requires PROOF: `nv_flush()` takes only an exact header-only `ST_NO_SESSIONS` envelope carrying SUCCESS or "no such handle", retries anything else, and logs a possible leak rather than disabling the TPM.
  - Both proof paths are gated, which is the point: a desynchronized `ST_SESSIONS` reply carrying `TPM_RC_HANDLE` classifies as NOTFOUND and would otherwise end a teardown with the session still allocated.
  - Disabling the transport was tried across three review rounds and reverted: a held slot terminates on its own in a definite `StartAuthSession` failure every caller treats as a refusal, while poisoning also breaks PCR reads and attestation that open no session. Verified fail-closed at `tpm_baseline.c:372`, where VERIFIED needs a successful read AND compare.
- [x] Unit-tested through the fake-TIS seam: spec-literal wire oracles, the attribute matrix over every reserved position, both builders, counter semantics, the define mismatch, the sequence token/budget/reserve, and the teardown envelope.
- [x] Commit: `"tpm: write-locked and monotonic NV index primitives"`

**Test checkpoint:** VERIFIED. A counter define emits the correct TPMA_NV bits and NV type and a write-lockable define emits `WRITEDEFINE`, both asserted against spec-literal attribute words rather than the macros under test; an increment past a write-lock returns LOCKED through `tpm_nv_classify_rc` and a later command still runs. A stalling TPM exhausts the cumulative budget and reports BUDGET with the transport left usable, while a failed abort still sticky-fails -- and a control asserts the same stalling TPM completes inside the real budget, without which the bound would pass by firing on everything. A teardown accepts only an exact envelope, retries an illegal tag, a wrong shape and a wrong-envelope HANDLE, and accepts well-formed SUCCESS and HANDLE replies on the FIRST attempt. Scope: this section owns the NV wire primitives and the command budget ONLY. The index lifecycle and the two-index contract are §21, record authorization is §27; the bootloader-side read path is §22; the headless escape hatch is §23; baseline content stays §6. Platforms: fake-TIS unit suites are the whole automatable surface; the live swtpm round trip and real-fTPM write-lock semantics are operator-gated (no `swtpm` on the dev host).

> **Test runner:** `make test-security` (or `scripts/debug/kernel/run-security-tests.bat`) | 1629 security-suite assertions pass; 22 `tpm: NV *` suites.

> **Notes:**
> - **What shipped:** the corrected + complete TPMA_NV/TPM_NT model with a pure `tpm_nv_attrs_valid()`, `TPM2_NV_Increment`/`TPM2_NV_WriteLock` builders and wrappers, `tpm_nv_read_counter`, and a bounded-sequence transport seam (`tpm2_seq_run` / `tpm2_submit_seq` / `tpm2_submit_seq_teardown`).
> - **How it integrates:** `tpm_policy_session_run` and every exported NV wrapper now run inside one bounded sequence; `tpm_policy_op_fn` gained the sequence token and `tpm_seal.c`'s callback moved with it.
> - **Downstream effects:** the read-bit correction changes what a define REQUESTS, so an index enrolled under the old attributes is refused as `TPM_NV_MISMATCH` rather than silently reused; authorized re-definition of such an index is §21's.
> - **Canonical doc:** the TPMA_NV table comment in `include/kernel/tpm_nv.h` cites the spec table and records the off-by-one.
> - **Scope boundary:** wire primitives + command budget only. Lifecycle is §21, record authorization is §27, the loader read is §22, the headless hatch is §23.

> **Verified:** 2026-08-17 | commit `68ca66d7d` | 6/6 items | build OK | 30484 kernel + 17 user tests; lint 0 errors; smoke matrix 4/4 legs
> **Accepted:** [H] seal and attestation still run outside a bounded sequence, and attestation's teardown discards the FlushContext result (reason: another component's surface; this section converted the NV path only) -> XREF: 01-boot-platform/TODO-13 §24 (item: "Route `tpm_seal_secret` and `tpm_unseal_secret` through one bounded sequence each" at line 770)
> **Accepted:** [M] the sequence deadline compares a raw TSC the owner can migrate away from (reason: pre-dates this section, and the corrected per-CPU reader does not exist in the tree) -> XREF: 01-boot-platform/TODO-13 §24 (item: "Decide whether the sequence deadline should survive a thread migration" at line 774)
> **Quality reviewed:** 2026-08-17 | Codex 37x (design, adversarial, re-adversarial, consistency, perf, test-coverage) | 2H+13M+5L fixed, 2 open | scope: kernel-code-quality

---

## 18. Atomic Boot-Integrity Report Publication

> **Spawned-by:** §14 (split)

STATE BEFORE THIS SECTION (all of it now fixed; kept because it is why the design is what it is). `tpm_integrity_init()` set `pcrs[0..7].status = BOOT_INTEGRITY_NO_CRYPTO` in Phase 0 and never revisited them. The three post-init setters -- `tpm_integrity_set_rng_available`, `tpm_integrity_set_replay_verdict` and `tpm_integrity_set_overall_status` -- touched scalars only. So after a Phase-1 VERIFIED verdict was published the per-PCR detail still read NO_CRYPTO and the report contradicted itself in the one place a reader would look to see WHICH measurement was trusted. The array was `pcrs[8]`, PCR 0-7, while the measured baseline set is {0-7,11}: PCR 11 had no slot to be reported in at all.

Publication was field-by-field into one static struct with no lock and no snapshot swap. That was safe only because every writer ran single-threaded on the BSP before the APs came up, which is an accident of init ordering rather than a stated contract, while the readers are already UI and VPD consumers.

- [x] Sized `boot_integrity_report.pcrs[]` by `BOOT_INTEGRITY_MAX_PCRS`, `_Static_assert`-tied to `TPM_BASELINE_MAX_PCRS`, indices filled from `tpm_pcr_baseline_pcrs()` -- PCR 11 now has a slot and cannot drift from the measured set.
- [x] `tpm_integrity_publish_baseline()` replaces `tpm_integrity_set_overall_status()`, carrying the verdict AND per-PCR statuses in ONE publication, from the new pure `tpm_baseline_compare_pcrs()`.
  - This needs a per-PCR comparison OUTPUT, which `tpm_baseline_compare` does not produce: it returns MISMATCH on a bank, Secure Boot, firmware-hash or ABI-manifest difference BEFORE the PCR loop (`src/kernel/tpm_baseline.c:104-133`), and `tpm_baseline_verify` never reaches it at all on NO_BASELINE, corrupt-baseline or self-corrupt (`tpm_baseline.c:333-379`). Found by this section's design review, so the per-PCR result surface belongs HERE; §25 owns only which SCALAR field differed.
  - Every verdict-producing path defines its per-PCR output rather than leaving the previous value standing: a comparison that ran publishes its real per-PCR result even when a scalar field is what forced MISMATCH, and a path that never compared publishes a not-evaluated status. Publishing uninitialized detail, keeping stale `NO_CRYPTO`, or attributing a scalar failure to a PCR are all wrong answers.
- [x] Report built OFF-lock and published by release-store swap into one of two immutable slots; the naked-pointer `tpm_integrity_report()` is GONE, replaced by `tpm_integrity_report_copy()`, and all 5 callers migrated.
  - The reader protocol chosen is the lock-spanning-acquisition option: `tpm_integrity_report_copy()` holds `s_publish_lock` across BOTH the pointer load and the copy, so no reader can be paused between them and no reader is inside a slot once it drops the lock. That is also why two slots suffice; a lock-free reader would have needed hazard pointers or an RCU grace period.
  - A bare refcount is explicitly NOT sufficient and an earlier draft allowed it: acquire-loading the pointer and then incrementing its refcount leaves a load-to-pin window in which the writer can swap and reclaim, so the reader pins a buffer that is already gone.
  - `tpm_integrity_report()` handed out an unowned pointer, so the choice was to migrate every naked-pointer caller or make the accessor a synchronized copy-out. It became a copy-out and the pointer accessor was deleted.
- [x] Writers serialize on the same `s_publish_lock`: base-load, mutate and publish inside one critical section, with the TAMPER-pins-MISMATCH guard evaluated INSIDE it so no baseline writer can lose it.
  - The setters mutate independent fields, so if each rebuilds off-lock from the snapshot it read, a replay-tamper writer publishing MISMATCH can be overwritten by a slower baseline writer publishing VERIFIED that never saw it. Every published snapshot stays internally coherent, so an old-or-new reader test passes while the report says VERIFIED on a tampered boot.
  - This is a live invariant, not a hypothetical: the pre-section `tpm_integrity_set_overall_status()` already pinned MISMATCH once a TAMPER verdict landed. An off-lock rebuild would have silently discarded that guard, so preserving it was part of the acceptance, not a nicety. It now lives inside the publication critical section.
  - Two alternatives were considered and both rejected in writing so nobody re-derives them. An unsynchronized bounded copy is not publication at all: the old `tpm_integrity_report()` returned a pointer to a multi-field static struct, so a field-by-field copy straddled a publication and saw a new verdict beside old per-PCR detail -- the exact contradiction this section exists to remove.
  - A seqlock was the second, and it cannot meet this section's own checkpoint without more machinery than the swap costs: `seqlock_read_begin` spins while the sequence is odd, so a stuck-odd writer stalls a UI/VPD reader forever, and bounding the OUTER retries does not bound that spin. Making it work would need a copy-out accessor whose validation spans the whole copy, a defined result on attempt exhaustion, and migration of every existing caller off the pointer accessor.
  - Whichever form ships, no blocking or TPM work may sit inside the writer's publication region.
- [x] Rewrote the stale Phase-0 "read golden values" contract comment on `tpm_integrity_init()` and the stale roadmap block in `tpm.c` to describe the two-phase assembly that actually ships.
- [x] Annotated §6's accepted [H] per-PCR publication stamp RESOLVED (kept, not deleted). -> XREF: §6 (the "Accepted: [H] baseline verify leaves stale per-PCR `NO_CRYPTO`" stamp).
- [x] Commit: `"tpm: atomic boot-integrity report publication"`

**Test checkpoint:** after a published VERIFIED verdict no slot still reads `NO_CRYPTO`, and PCR 11 is present in the reported set; after a PCR-CAUSED MISMATCH the per-PCR detail names which PCR differed rather than leaving the reader to guess. The qualifier is load-bearing and was added by the design review: a mismatch forced by a SCALAR field (bank, Secure Boot, firmware hash, ABI manifest) must show its PCRs as they actually compared rather than manufacturing a PCR culprit, and a path that never compared at all (NO_BASELINE, corrupt baseline, self-corrupt) must show a not-evaluated status rather than stale detail. A reader paused between the pointer LOAD and its pin, while the writer publishes and tries to reclaim, still ends with a coherent report -- that acquisition window is the one a held-reference test starts too late to cover. A replay-tamper update racing a baseline-verified update leaves MISMATCH authoritative, never VERIFIED: the existing TAMPER-pins-MISMATCH guard must survive the new publication path. A reader held across several publications likewise sees a coherent report and no reuse underneath it; under repeated concurrent publication readers observe old-or-new, never a mix, and always complete -- a test that only asserts old-or-new passes against a writer that stalls readers forever. Scope: this section owns the report STRUCT and its publication only; the verdicts themselves stay with §4 (replay), §5 (Secure Boot) and §6 (baseline), and WHICH scalar field differed plus what the `"verified"` string claims are §25. Platforms: kernel unit suites; no TPM required.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 1932 kernel + 17 user-mode, 0 failures
> **Notes:**
> - Shipped: `pcrs[]` sized `BOOT_INTEGRITY_MAX_PCRS`, two-slot immutable-snapshot publication under `s_publish_lock`, copy-out-only reads, and two new pure cores: `tpm_baseline_compare_pcrs()` and `tpm_integrity_build_report()`.
> - Integration: `tpm_integrity_report()` DELETED and all five callers migrated to `tpm_integrity_report_copy()`; `tpm_integrity_set_overall_status()` replaced by `tpm_integrity_publish_baseline()` (verdict + detail in one call).
> - Downstream: `tpm_baseline_verify()` gained per-PCR out-params and refuses an undersized detail buffer with BADARG, AFTER the self-identity check so a corrupt kernel descriptor still publishes MISMATCH.
> - Both size contracts fail CLOSED: a measured set not exactly `BOOT_INTEGRITY_MAX_PCRS` publishes `pcr_count=0` + UNKNOWN with a `LOG_ERROR`, matching how `tpm_baseline_snapshot()` guards the same invariant.
> - `tpm_integrity_test_republish()` is a `KERNEL_TESTS`-gated save/restore seam, verified absent from a `KERNEL_TESTS=off` binary; wrapper-level verify coverage needing an NV-blob fake is §26.
> - Scope boundary: this section owns the report struct, its publication and the per-PCR detail; scalar-field attribution and status-string scoping are §25.
> **Verified:** 2026-08-17 | commit `37e2dbb9c` + review fixes | 7/7 items | build OK (`=== BUILD OK ===`) | lint rc 0, 0 errors | 30787 kernel + 17 user-mode PASS (SUITE=security 1932) | smoke matrix 4/4 legs (kvm/tcg x 1/2 cpu, 261s) | evidence: live boot logs "Boot integrity: skipped (no TPM)" then "Boot integrity status: no-TPM" through the new publication and copy-out path; `tpm_integrity_report(` and `tpm_integrity_set_overall_status` have zero references tree-wide
> **Accepted:** [H] wrapper-level `tpm_baseline_verify` per-PCR plumbing is untested on every path that actually compares (reason: needs an NV fake that can return a crafted baseline blob; the current fake serves only a 4-byte payload or a counter) -> XREF: 01-boot-platform/TODO-13 §26 (item: "Cover `tpm_baseline_verify` end-to-end for: full match, one PCR differing, scalar-only mismatch (Secure Boot / ABI manifest), NO_BASELINE, corrupt blob, and snapshot failure." at line 812)
> **Accepted:** [M] no deterministic proof that an unlocked or in-place publication fails a test (reason: mutation-checked and the sequential suite does not catch it; a forced-interleaving hook needs a bounded race-free `thread_join`, which `src/kernel/sched/task.c:5964-5971` assigns elsewhere) -> XREF: 01-boot-platform/TODO-13 §26 (item: "Deterministic concurrent-publication coverage: a `KERNEL_TESTS`-gated hook widening the publication window so an unlocked publication FAILS. BLOCKED on the scheduler join/wait fix (see sub-bullets)." at line 816)
> **Quality reviewed:** 2026-08-17 | Codex 13x (design, adversarial, test-coverage, consistency, perf, re-adversarial x5) + kernel-quality-auditor + concurrency-evidence-mapper + parity-research-analyst | 4H+12M+6L fixed, 2 accepted | scope: kernel-code-quality

---

## 19. Versioned Baseline Growth and NV Index Migration

> **Spawned-by:** §16 (split)

Split out of §16 on 2026-08-17. §16 could populate the digest the struct already reserves; every ADDITIONAL digest requires the struct to grow, and growth is not a header edit. The blob is 412 bytes today, `tpm_nv_define_data` sizes `TPM_NV_INDEX_BASELINE` at exactly `sizeof(struct tpm_baseline)`, and `tpm_baseline_parse` rejects any blob whose `size` differs (`src/kernel/tpm_baseline.c:45`). Growing the struct therefore lands on an index too small to hold it, and define-if-present returns `DEFINED` rather than enlarging it -- so "older blob still validates behind the version field" is false without a migration. That migration is the section: an undefine/redefine window over the trust anchor, which must not lose the generation counter or the anti-rollback state across a crash.

**This is why it is not part of §16.** The failure mode is opposite: §16 fails by attributing a change to the wrong image, this section fails by leaving the machine with NO baseline at all after an interrupted resize. It is also blocked on §21 and §27, which between them own the authorized lifecycle and the anti-rollback record state the migration has to carry across the window; sequencing it earlier would mean designing the preservation of state that does not exist yet.

- [/] BLOCKED on §21 and §27: the migration must preserve anti-rollback state across the undefine/redefine window, and those two own it. -> XREF: 01-boot-platform/TODO-13 §28 (item: "Specify the baseline counter's crash-consistent ordering").
- [ ] Specify a version-sized read path that probes the stored header before demanding the new `sizeof` -- `NV_ReadPublic` for the index size, or a size-prefix read -- so a v1 blob is recognized as v1 rather than read as corrupt.
- [ ] Design an authorized, crash-safe resize that preserves the generation counter and the §27 anti-rollback state across the undefine/redefine window, and state which step is the commit point.
  - The window is the whole risk: between undefine and the rewrite there is no baseline on the machine, so the recovery path has to be reachable from a boot that finds the index absent but the anti-rollback counter advanced.
- [ ] Decide the chunked-NV question explicitly rather than discovering it mid-implementation.
  - Three 32-byte digests leave the blob near `TPM_NV_MAX_DATA` 512, so either the chunked path §14 deferred becomes necessary here or it is ruled out with the arithmetic written down. -> XREF: 01-boot-platform/TODO-13 §14 (item: "Commit: `\"tpm: post-ship follow-up backfill\"`").
- [/] Consume the kernel-image digest from its owner, never recomputing it -- parked, owner has no code behind it. -> XREF: [`04-drivers-hardware/TODO-04 §7`](../04-drivers-hardware/TODO-04-security-hardware.md) (item: "`kernel_measure()`").
  - Verified 2026-08-17: `kernel_measure()` is defined nowhere in `src/`; the owning item is still `[ ]`.
- [/] Consume the loader-side kernel digest from its owner -- parked on the same grounds. -> XREF: [`18-future-research/TODO-04 §2`](../18-future-research/TODO-04-secureboot-tpm.md) (item: "PCR 8 -- kernel hash extension").
- [ ] Fixture the migration from a REAL 412-byte v1 index, not a synthesized one: the failure under test is the on-NV size mismatch, which a hand-built blob of the new size cannot reproduce.
- [ ] Provide a reachable, authorized upgrade for a PRE-BINDING baseline, which §16 turned into a permanent MISMATCH. -> XREF: 01-boot-platform/TODO-13 §16 (item: "Fixed the compare path").
  - Found by §16's post-ship adversarial review. A baseline enrolled before §16 has `abi_manifest_present = 0`; validate still accepts it but the symmetric gate rejects it against every current snapshot, and re-enrollment cannot repair it because §15's gate admits nothing.
  - §16's own commit claimed no such baseline could exist. That was wrong: it conflated "cannot enroll now" with "never enrolled", and earlier committed code did permit config-gated enrollment.
  - Migrate under the §15 authority with a generation bump and an atomic persist. NEVER auto-migrate merely because the legacy CRC validates, which would let a hand-written legacy blob upgrade itself into a bound one.
- [ ] Migrate an enrolled v1 baseline index to the policy-bearing v2 contract, side by side rather than in place. -> XREF: 01-boot-platform/TODO-13 §27 (item: "Consumer: baseline rotation gets the SAME authorization boundary").
  - An authPolicy is fixed at `NV_DefineSpace` and hashed into the index Name, so v1 cannot acquire the §27 boundary in place. §27 ships the v2 contract constants, the legacy-v1 detection and the refusal to fall back silently; the destructive half is this section.
  - Own the state machine: create v2, write and verify an authority-approved freshly measured record, commit an independently protected migration marker, and only then disable the legacy path and optionally remove v1.
  - NEVER auto-wrap the v1 blob into v2. v1 is owner-writable, so copying its content across would authenticate attacker-controlled data under the new policy.
  - A boot that finds the migration marker committed but v2 absent enters authorized recovery, never first enrollment: enrolling there would launder a rollback into a fresh golden baseline.
- [ ] Build the fake-TIS fixture and descriptor-injection seam the live baseline wrappers need. -> XREF: 01-boot-platform/TODO-13 §16 (item: "`tpm_baseline_snapshot()` populates `abi_manifest`").
  - §16 shipped `TPM_BASELINE_SELF_CORRUPT` propagation and the `BOOT_INTEGRITY_MISMATCH` publication with NO test caller, because snapshot reads the const descriptor directly and the round trip needs a transport.
  - The CC-dispatch fake-TIS pattern already exists in `test_tpm_nv.c`, `test_tpm_seal.c` and `test_tpm_attest.c`, so this is buildable rather than blocked.
  - Cover descriptor success and failure through snapshot, enroll, verify AND the publication in `boot_interrupts.c`, including corruption combined with NO_BASELINE and with NO_TPM, which is exactly where the masking bug §16 fixed used to hide.
- [ ] Commit: `"tpm: versioned baseline growth and NV index migration"`

**Test checkpoint:** a migration starting from a REAL 412-byte v1 NV index ends with the enlarged index readable, the generation counter preserved and strictly greater than the pre-migration value, and the anti-rollback state intact. An interrupted migration is recoverable rather than leaving the machine with no baseline: a boot that finds the index absent while the anti-rollback counter has advanced reaches the recovery path rather than enrolling a fresh baseline over the gap, which is the shape that would silently launder a rollback. A v1 blob read through the version-sized path is recognized as v1 and not reported CORRUPT, with a control asserting that a genuinely corrupt blob of the same length still IS reported corrupt -- without it the version-sized read would pass by accepting everything. The chunked-NV decision is asserted by the arithmetic in a test, not only in prose. Scope: this section owns the SCHEMA GROWTH and the NV migration. The ABI-manifest digest is §16, the loader digest is §20, the NV wire primitives are §17, the authenticated lifecycle is §21, the record authorization is §27 and the crash-consistent pairing is §28, and the two kernel digests stay with their producers. Platforms: kernel unit suites via the fake-TIS seam are the whole automatable surface; a live resize against real NV is operator-gated (no `swtpm` on the dev host).

---

## 20. BOOTX64.EFI On-Disk Self-Measurement

> **Spawned-by:** §16 (split)

Split out of §16 on 2026-08-17. Both digests §16 originally consumed measure the KERNEL, so nothing in the corpus identifies the bootloader binary that ran. This section produces that digest. Restored during the deduplication pass that dropped §16's two duplicate kernel-hash tasks and took this one with them, which would have let the baseline complete while still unable to say which loader ran. Searched before restoring -- the `BOOTX64.EFI` digests elsewhere in `todo/` are build-time signing, USB-write verification and release manifests, none a runtime self-measurement.

**The measured bytes are the ON-DISK FILE, and that is the whole correctness argument.** `EFI_LOADED_IMAGE_PROTOCOL` hands back a RELOCATED in-memory `ImageBase`/`ImageSize` whose relocations are already applied and whose data sections mutate as the loader runs, so hashing that range yields a different digest for the same binary across boots and destroys the attribution. The canonical bytes are the ESP file's contents, reopened through the loaded-image `DeviceHandle` + `FilePath`.

- [x] Reopen `BOOTX64.EFI` through the loaded-image `DeviceHandle` + `FilePath` and hash the file bytes, never the resident image range.
  - `self_measure_run()` in `src/boot/uefi/bootx64.c` runs right after Step 0 captures the loaded image, while Boot Services are still live: resolve the path, `HandleProtocol` SimpleFS on the boot-device handle, `OpenVolume`, `Open` read-only, two-call `GetInfo` for the size, then one forward pass over 64 KiB chunks.
  - The path comes from `dpfp_extract()` in [`include/boot/devpath_filepath.h`](../../include/boot/devpath_filepath.h) over a device path MEASURED by `EFI_DEVICE_PATH_UTILITIES_PROTOCOL->GetDevicePathSize`. The loader never self-walks a firmware-owned path to find its end, which is the rule `net_dp_validated_size` already records.
  - The parser REFUSES rather than guesses: an `END_INSTANCE` node, a non-FILEPATH node, an embedded NUL with characters behind it, an odd payload, a node length under 4 or past the measure, and a missing `END_ENTIRE` each return a distinct status and an empty path.
  - The loader has no runtime SHA-256, so [`include/boot/sha256_boot.h`](../../include/boot/sha256_boot.h) adds a header-only one (a new `.c` would need the loader Makefile, which is operator-only machinery). It is gated by `sha256b_selftest()` at measurement time: a wrong hash publishes nothing.
- [x] Report the digest ABSENT on paths where the file cannot be reopened (NULL `DeviceHandle`, pure HTTP boot) rather than substituting a resident-range hash, because a silently different measurement is worse than a missing one.
  - Twelve distinct statuses (`no-device-handle`, `no-file-path`, `no-filesystem`, `open-failed`, `getinfo-failed`, `empty-file`, `over-size-cap`, `alloc-failed`, `read-failed`, `short-read`, `sha256-selftest-failed`, `ok`), each printed by name so an absence says WHY.
  - A `Read` returning 0 before `FileSize` bytes is `short-read` and publishes nothing. Hashing the prefix would produce a digest indistinguishable from the real one, which is the exact failure the ABSENT rule exists to prevent.
  - Never fatal. A loader that refused to boot because it could not measure itself would turn an observability feature into a brick.
- [x] Bound the cost: a size ceiling on what is hashed, exactly one pass over the bytes, and a pre-ExitBootServices timing milestone recorded on serial.
  - The loader accepts images up to 32 MiB and the smoke matrix does not fail on a latency regression, so an unbounded "hash the file" instruction would silently lengthen every boot as the binary grows.
  - Ceiling 8 MiB against a shipped 351,952-byte binary; over it the status is `over-size-cap` and no digest is published. One `boot_rdtsc()` delta covers the whole measurement and rides the same serial line, and POST16 `0xB0A4`/`0xB0A5` bracket it (both in the smoke required set, so a boot that stops measuring fails the gate).
  - Cost measured END TO END, not just the hash: the compression is ~2.8-4.3 ms on KVM at the logged 3.93 GHz, and the 138-byte serial line that carries the digest is another ~12 ms at 115200 baud or ~36 ms at 38400. The line stays: it is the ONLY handoff until the `boot_info` carriage lands, it is what the smoke oracle checks, and it is 3% of the 4,623 bytes the loader already writes to serial, so removing it would not change the class of cost.
- [/] Store the loader digest in the baseline blob -- parked until the struct can grow. -> XREF: 01-boot-platform/TODO-13 §19 (item: "Design an authorized, crash-safe resize that preserves the generation counter").
- [x] Refuse a `GetInfo` that succeeds into a buffer too small to hold `FileSize`, rather than reading the pool bytes behind it (post-commit adversarial).
  - Both sizes are firmware-written: the probe result chooses the allocation and the second call may lower it, so a provider reporting success into a 4-byte buffer would have had its neighbouring pool bytes read as a file size. A plausible value under the 8 MiB cap would then have driven a fully successful measurement of the wrong length. Status `getinfo-malformed`.
- [x] Refuse a file that still reads past its declared `FileSize`, instead of publishing a prefix digest as complete (post-commit adversarial).
  - The loop stopped at the declared length and never probed for EOF, so a stale or hostile under-report produced a prefix digest that is indistinguishable from the whole-file one. One bounded one-byte read past the declared end settles it; status `trailing-bytes`.
  - Not fixture-asserted: both refusals need a fake `EFI_FILE_PROTOCOL`, and the loader has no such seam. Reviewed at file:line and exercised on the live boot path only in their success direction.
- [x] Commit: `"boot: BOOTX64.EFI on-disk self-measurement"`

**Test checkpoint:** the digest the loader reports on serial equals a SHA-256 computed independently on the host over the exact staged loader binary, and its byte count equals that file's size -- which is what `scripts/test-smoke.sh` asserts on every smoke run, and what a prefix read, a wrong file or a constant cannot satisfy. Cross-leg agreement is a consequence rather than the test: each leg checks the same oracle, so four legs agreeing proves four correct measurements rather than four consistent ones. Hashing the resident image range instead of the file fails that oracle immediately, because relocations make the resident bytes differ from the file. A boot path where the file cannot be reopened reports the digest ABSENT with a NAMED reason on serial (twelve statuses, and a record whose zero value is `not-run`, never `ok`), and the reporting runs even when `EFI_LOADED_IMAGE_PROTOCOL` itself was unavailable. Hashing stays within the 8 MiB ceiling, the pre-ExitBootServices TSC delta appears on the same line, and POST16 `0xB0A4`/`0xB0A5` are in the smoke required set so a boot that stops measuring fails the gate. The parser is tested where firmware cannot be asked to misbehave: kernel fixtures in `test_uefi_boot.c` cover malformed node lengths, an odd payload, an embedded NUL, `END_INSTANCE`, a non-FILEPATH node, a missing `END_ENTIRE`, all four separator combinations at a node boundary, and the output bound with an exactly-fits control. Scope: this section owns the LOADER digest and its serial reporting. The `boot_info` carriage is PARKED on operator-only ABI machinery and is therefore NOT claimed here, storing the digest in the baseline blob waits on §19, and binding the digest to the EXECUTED image is owned by §25 (which depends on §1's event-log parser). Platforms: the QEMU smoke legs prove the oracle; bare-metal firmware whose `FilePath` is not a plain FILEPATH node can only be exercised on real hardware and stays with §6's platform item.

> **Test runner:** `bash scripts/test.sh SUITE=boot` (12 new suites in `src/kernel/test/test_uefi_boot.c`) + `bash scripts/test-smoke.sh` (live oracle against the staged binary) | 30819 kernel + 17 user-mode tests pass, smoke PASSED

> **Notes:**
> - **What shipped:** a loader self-measurement that hashes the ESP file it was launched from, two pure headers behind it (`sha256_boot.h`, `devpath_filepath.h`) shared with the kernel test suite, twelve named statuses, POST16 `0xB0A4`/`0xB0A5`, and a smoke gate that checks the reported digest against an independent hash of the staged binary.
> - **How it integrates:** `self_measure_run()` is called from Step 0 in `efi_main` while `DeviceHandle` + `FilePath` are in hand and Boot Services are live; it allocates one 64 KiB pool chunk, frees it, and leaves nothing across ExitBootServices.
> - **Why the primitives are headers:** the loader translation-unit set is fixed by machinery this run may not edit, and pure headers are also what let the parser and the hash be TESTED at all -- firmware will not hand the loader a malformed device path on request, but `test_uefi_boot.c` can build one.
> - **Evidence the measurement is real, not merely consistent:** the smoke gate compares against `sha256sum` of the exact staged loader plus its byte count, so a prefix read, a wrong file or a constant fails. Cross-leg equality alone would only have proven determinism, which was a review finding on the original plan.
> - **Scope boundary:** this section owns the loader digest and its reporting. The `boot_info` carriage is parked on operator-only ABI machinery, the baseline-blob storage is §19, and binding the digest to the EXECUTED image is owned by §25, which depends on §1's event-log parser rather than being §1 itself.
> - **Not covered:** the digest is an on-disk-path observation, not proof of executed code; a bare-metal firmware whose `FilePath` is not a plain FILEPATH node reports `no-file-path` rather than a digest, and only real hardware can exercise that.

> **Verified:** 2026-08-18 | commit `fe07053fc` + review fixes | 6/7 items (1 parked on §19 struct growth; three review-found gaps re-homed to their owners rather than parked here) | build OK (`=== BUILD OK ===`) | 30819 kernel + 17 user-mode tests, 0 failures | lint rc 0, 0 errors | `bash scripts/test-smoke.sh` PASSED with `self-measure matches build/tools/BOOTX64.signed.efi (351952 bytes)`: the live loader digest and byte count equal an independently computed hash of the staged binary, against a serial line validated by anchored schema (`status=ok bytes=351952 tsc=0000000000A98D87 sha256=6C3642AF...`) | evidence the fixtures can fail: the single-node and join fixtures caught a real separator bug (a `\` inserted between every character) before the parser ever ran in firmware, and the four-combination fixture added in review caught a doubled separator at a node boundary
> **Deferred:** [H] the loader digest reaches the kernel on serial only, because a new `boot_info` field cannot be added unattended at all (reason: `check-doc-coverage.py:462` refuses a field without an `F()` row in `dump-fields.inc`, and `tools/boot-info-manifest/*` is operator-only ABI machinery) -> XREF: `todo/overnight-runner-improvements/overnight-runner-improvements-v16.md` (item: "A `boot_info` field cannot be added unattended at all, so the loader self-measurement shipped without its kernel carriage" at line 50)
> **Deferred:** [M] the two new headers are not in the loader build prerequisites, so a header-only edit leaves `bootx64.o` and the signed image stale (reason: `Makefile*` is operator-only machinery in an unattended run; the kernel side is safe via `-MMD -MP`) -> XREF: `todo/overnight-runner-improvements/overnight-runner-improvements-v16.md` (item: "The loader sub-make lists bootx64.o prerequisites by hand, so a new shared header is invisible to incremental builds" at line 54)
> **Deferred:** [M] the digest attributes an on-disk FILE rather than the executed image, so no consumer may read it as proof of executed code (reason: the binding is the PCR 4 `EV_EFI_BOOT_SERVICES_APPLICATION` event, whose Authenticode PE hash is different work from a flat file hash) -> XREF: 01-boot-platform/TODO-13 §25 (item: "Scope what the loader digest claims, and bind it to the firmware-measured executed image where the event log allows" at line 822)
> **Quality reviewed:** 2026-08-18 | Codex 10x (design, adversarial x2, consistency x2, perf x2, re-adversarial x3) | 5H+8M+2L fixed, 1H+3M open (every one owned) | scope: boot-code-quality (boot-quality-auditor: all 14 gates pass, 1L fixed; parity-research-analyst: Win11/Linux measured-boot comparison) | re-adversarial closed on an explicit approve at round 3 ("no spec-conforming firmware path was found that publishes a wrong digest instead of explicit absence"); rounds 1 and 2 each found a real fail-open in the EOF probe, so the loop earned its rounds rather than spinning on fixture hardness

---

## 21. Authenticated NV Index Lifecycle and Anti-Rollback Trust Anchor

> **Spawned-by:** §17 (split)

§6's baseline rotation rejects a lower generation in SOFTWARE (`tpm_baseline_rotation_ok`, `src/kernel/tpm_baseline.c:221`), so an attacker who can write the NV index simply overwrites the generation along with the blob. And TODO-21 §8's per-slot A/B floor has no authenticated store at all: its design note states that CRC metadata is a cached hint and never the floor authority, and that selection enforcement ships WITH the store. That is why its `ROLLBACK_BLOCKED` enforcement is a Critical deferral pointed here rather than shipped code.

**Split again on 2026-08-17: the record AUTHORIZATION half is now §27, and this section is the index IDENTITY and LIFECYCLE half.** The earlier draft argued "one mechanism, two consumers, which is why they are one section", and that argument is still right about the CONSUMERS: sharing the primitives is correct and sharing one counter is not. It said nothing about the two greenfield constructions underneath, which have opposite failure modes. This section fails by letting an attacker DESTROY AND RECREATE the index, or answer with a different index at the same handle, so nothing it protects survives to be checked. §27 fails by accepting a FORGED record at an index that is perfectly intact.

**Both halves are greenfield, which is what made one section the wrong shape.** Verified against the tree on 2026-08-17: `TPM2_CC_NV_UndefineSpaceSpecial` has no constant, builder or parser; `PolicyCommandCode`, `PolicySigned` and `PolicyAuthorize` are absent entirely; no TPM2 Name is computed or verified anywhere in `src/kernel/tpm*.c`; and `TPMA_NV_POLICY_DELETE` is modelled at `tpm_nv.h:86` but REFUSED by `tpm_nv_attrs_valid` (`tpm_nv.c:92`). What does exist to build on is `tpm2_build_start_auth_session` + `PolicyPCR` + `PolicyGetDigest` (`tpm_nv.c:438-559`) and the `PolicySecret` precedent in `tpm_attest.c:106-129`.

**This section is the AUTHORIZATION BOUNDARY the index itself carries.** §17's counter and write-lock commands stop an ordinary write from lowering a live index and nothing more. Every LIFECYCLE attack passes every cooperative check §17 can make, and the tree states why: `tpm_nv.h:15-16` records that define/undefine use owner auth with an empty password session PRECISELY because owner auth bypasses PCR policy, so the OS path that legitimately provisions the index is also the path that can destroy it. Getting this wrong reproduces exactly the false security TODO-21 §8 refused to ship, so it takes its own Codex design review before implementation.

- [/] Reset authorization is EXPRESSIBLE and recreation is DETECTED, but deletion is not yet unreachable from the ordinary OS path.
  - The delete policy SCOPES to one command; it does not authenticate anyone. -> XREF: 01-boot-platform/TODO-13 §27 (item: "Record-write must be unavailable to ordinary or rolled-back OS code").
  - `tpm_nv_attrs_valid_platform` (`src/kernel/tpm_nv.c:72`) admits `TPMA_NV_POLICY_DELETE` only under a PLATFORM-authorized define and only WITH an authPolicy, and requires `TPMA_NV_PLATFORMCREATE`; the owner validator still refuses both outright, so the ordinary OS path cannot reach them.
  - Kept as a SEPARATE builder (`tpm2_build_nv_define_platform`) rather than a hierarchy flag on the owner define: a shared entry point with a mode argument is one wrong call away from admitting the attributes under owner auth.
  - `tpm2_build_nv_undefine_special` marshals the two authorized handles in the normative order (nvIndex under its own policy, then `TPM_RH_PLATFORM`) and `tpm2_build_policy_command_code` + `tpm_nv_delete_policy_digest` compute a delete policy bound to that one command.
  - Recovery for a legitimate TPM clear or replacement is DELIBERATELY not inferred: a replaced TPM and an attacker-deleted index present identical observations, so the classifier returns `RECOVERY_REQUIRED` and stops rather than handing the attacker the recovery path.
  - The attack is tested directly through the fake-TIS seam: an anchor enrolled written that reads back UNWRITTEN is `REFUSE_RECREATED`, because a freshly defined index has `TPMA_NV_WRITTEN` clear and a byte-identical redefinition produces a byte-identical Name.
  - STILL OPEN, found by the final re-adversarial round: `tpm_nv_delete_policy_digest` asserts ONLY `PolicyCommandCode`, which carries no secret and is reproducible by any caller, and the builder sends an empty `platformAuth`. So a delete-recreate-WRITE against an ordinary data index defeats the WRITTEN detector, and the section's promise of authorization unreachable from the OS path is not met by this alone. The counter detector still holds (a recreated `TPM_NT_COUNTER` cannot restart below the TPM's highest-ever value), which is why the anchors are counter-backed.
  - The code says so at `src/kernel/tpm_nv.c` above `tpm_nv_delete_policy_digest`, and a test pins that the digest is REPRODUCIBLE, so the policy can never be mistaken for a trust boundary.
- [ ] Bind deletion to an authenticated assertion alongside `PolicyCommandCode`, and accept a real platform authorization.
  - `PolicySigned`, or `PolicyAuthorize` over a signed policy; the builder's empty `platformAuth` is the other half. -> XREF: 01-boot-platform/TODO-13 §27 (item: "Record-write must be unavailable to ordinary or rolled-back OS code").
  - Neither `PolicySigned` nor any signed-policy machinery exists in the tree; §27 owns that construction for record writes and the same primitive serves the delete policy.
  - Acceptance: an unauthenticated policy session cannot delete, redefine, write and then pass verification -- tested end to end, not asserted in prose.
- [x] Index NAME and public attributes are verified before contents are read, so the handle number is never what is trusted.
  - `tpm2_nv_name_compute` (`src/kernel/tpm_nv.c:186`) marshals `TPMS_NV_PUBLIC` and emits `nameAlg || SHA-256`, and `tpm2_parse_nv_name` extracts the trailing `TPM2B_NAME` the parser previously bounded and discarded; the fake now serves a REAL name so the field is consumed rather than ignored.
  - The enrolled contract is NORMALIZED (`struct tpm_nv_identity`, `TPMA_NV_STATUS_MASK` cleared) plus an explicit `expect_written`, because `TPMA_NV_WRITTEN` is inside the hashed public area and the Name therefore CHANGES on first write. A single enrolled Name would have rejected the legitimate initialized anchor.
  - `tpm_nv_verify_identity` reads the public area, requires the reported Name to be self-consistent with it, compares the contract, and enforces the SAME lifecycle check as the atomic path (a divergence here would be a trap for the next caller). Its answer EXPIRES with its sequence, so `tpm_nv_verify_then` holds ONE bounded transport sequence across ReadPublic, the Name and identity checks and the caller's op -- found by the adversarial round, which showed that verify-then-read in two sequences is a check-then-use race another CPU can win by recreating the index in between.
  - The lifecycle classifier separates `counter_required` from `counter_known` and returns `REFUSE_INCOMPLETE`: gating the rollback comparison on "did we read it" alone let a FAILED counter read fall through to ACCEPT, which is a fail-open anti-rollback decision an attacker gets just by making the read fail.
  - `tpm_nv_verify_and_read` OWNS the handle (taken from the contract, never a caller argument) because the callback form cannot stop an op submitting against a different index, and its transfer is EXACT: a short or over-length response is `TPM_NV_TRANSPORT`, not a quiet partial success.
  - A malformed PERSISTED contract reports `TPM_NV_CONTRACT`, not `TPM_NV_BADARG`: the struct is exported for persistence, so corrupt stored state is an authorized-recovery question while BADARG is documented as caller misuse with no transaction issued.
  - The atomic path enforces the LIFECYCLE state, not just the definition, returning `TPM_NV_RECREATED` before the op runs. The re-adversarial round found that identity alone lets a byte-identical recreation through, whose `NV_Read` then reports `UNINIT`, which `tpm_baseline.c:305` maps to `NO_BASELINE` -- laundering a destroyed anchor into a first enrollment.
- [x] Defined the anchors as SEPARATE indexes with separate contracts, and the floor as a record rather than a counter.
  - `TPM_NV_INDEX_BASELINE_GEN` and `TPM_NV_INDEX_AB_SEQ` are distinct `TPM_NT_COUNTER` indexes (`include/kernel/tpm_nv.h:181`), pairwise-distinct by `_Static_assert` and asserted distinct on the marshalled command bytes.
  - The A/B floor is `TPM_NV_INDEX_AB_FLOOR`, an ORDINARY data index, not a counter. The design review caught the original draft here: a TPM counter advances by exactly one per `NV_Increment`, so it cannot represent a security version that jumps, and this file already pinned the record-plus-sequence shape. -> XREF: 01-boot-platform/TODO-13 §27 (item: "Represent the A/B floor as an authenticated DATA record holding the security version").
- [x] Unit-tested the lifecycle refusals, the Name mismatch, the two-index separation and the multi-session envelope, each beside a passing control.
  - 18 suites / 207 assertions in `src/kernel/test/test_tpm_nv.c`, all `TEST_CAT_SECURITY`. Security suite 1932 -> 2139 kernel assertions; a mutation probe on the ACCEPT control was confirmed to fail the suite, so the suites demonstrably execute.
  - The two-session delete is exercised END TO END through `tpm_session_cmd_exec_seq_n`, with the same response proven to be REFUSED by the one-session path. Testing builder and validator separately had left the executor untested, where a wrong count stays green locally and rejects every real firmware success.
  - The Name oracle is INDEPENDENT of the module: the test hand-marshals `TPMS_NV_PUBLIC` and hashes it with `sha256()` directly, so a field-order or width error shows as a mismatch instead of being echoed back.
  - Controls run beside every refusal: a healthy anchor is ACCEPTED, an advanced counter is accepted, a plain platform define is legal, the unchanged public area matches, and the single-session validator still passes.
- [/] Adopt the identity gate in the record consumers, which store the enrolled contract this module deliberately does not. -> XREF: 01-boot-platform/TODO-13 §27 (item: "Call `tpm_nv_verify_identity` before every record read").
  - `tpm_nv_verify_identity` needs an ENROLLED contract to judge against, and persisting one is record state. This module owns the gate and the ordering; the consumers own when to call it and where the contract lives.
- [x] Commit: `"tpm: authenticated NV index lifecycle and index identity"`

**Test checkpoint:** an undefine/redefine cycle followed by a rollback attempt is REFUSED rather than read as a fresh install, and a handle whose Name no longer matches the enrolled public area is refused BEFORE its contents are read, which is the ordering that matters: a Name check that runs after the read has already trusted the bytes. A delete authorized under the index's own policy succeeds, and the same delete attempted through the ordinary owner-auth path is refused, so the two are proven to be different authorities rather than the same one renamed. A control asserting that a correctly authorized define and a correctly authorized delete both SUCCEED runs beside every refusal; without it each refusal assertion would pass against a helper that refuses everything. The two indexes are defined with distinct handles and distinct attributes, asserted on the marshalled command bytes, so a later change collapsing them back to one value fails here. A legitimate TPM clear reaches the stated recovery rather than the rollback refusal. Scope: this section owns the index LIFECYCLE, IDENTITY and the two-index definition contract. Record authorization and the crash-consistent pairing are §27, the wire primitives are §17, the loader-side read is §22, the headless authorization is §23, A/B selection enforcement stays TODO-21 §8, and baseline content stays §6. Platforms: fake-TIS unit suites are the whole automatable surface; live swtpm and real-fTPM policy-delete semantics are operator-gated (no `swtpm` on the dev host).

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 18 new suites / 207 assertions in `src/kernel/test/test_tpm_nv.c` | security suite 1932 -> 2139 kernel assertions, 0 failures

> **Notes:**
> - Ships index IDENTITY (TPM2 Name compute/parse, a normalized enrolled contract, and a `tpm_nv_verify_identity` gate that reads no contents) and LIFECYCLE classification, in `include/kernel/tpm_nv.h` + `src/kernel/tpm_nv.c`.
> - Admits `TPMA_NV_POLICY_DELETE` only through a separate platform-authorized define + validator, and adds the `NV_UndefineSpaceSpecial`, `PolicyCommandCode` and delete-policy-digest primitives the reset authorization needs.
> - Generalizes the response-envelope check to N authorization sessions, which every existing one-session path delegates to unchanged; without it a real two-authorization delete would classify as `TPM_NV_TRANSPORT`.
> - `tpm_nv_verify_and_read` is the API a content reader must use: it holds one transport sequence across verification and the read AND owns the handle, so a verification cannot approve one index while the read targets another.
> - Defines `TPM_NV_INDEX_BASELINE_GEN`, `TPM_NV_INDEX_AB_SEQ` and `TPM_NV_INDEX_AB_FLOOR` as pairwise-distinct handles, with the floor an ordinary record rather than a counter.
> - Canonical doc: `include/kernel/tpm_nv.h` header block "Index IDENTITY and LIFECYCLE".
> - Scope boundary: record content, its authorization and the persisted contract are §27; provisioning a POLICY_DELETE index against real firmware is operator-gated (platform auth, no `swtpm` on the dev host).
> - NOT closed by this section: the delete policy scopes to one command but authenticates nobody, so deletion is not yet unreachable from the OS path; the section stays `[/]` with that work filed rather than claiming a boundary it does not enforce.

> **Verified:** 2026-08-18 | commit `e70a055b4` + review fixes | 4/7 items (2 parked with named owners, 1 open by design: the section does not claim a boundary it does not enforce) | build OK (`=== BUILD OK ===`) | 31035 kernel + 17 user-mode tests, 0 failures | lint rc 0, 0 errors | todo-graph 10/10 | evidence the tests can fail: a mutation probe on the ACCEPT control flipped the suite to `FAIL: 1 of 2063`, and the independent hand-marshalled Name oracle (`test_tpm_nv.c`) checks the module's marshaller against a separately written TPMS_NV_PUBLIC layout rather than echoing it | stack measured with `clang-19 -fstack-usage`: the new verified-read chain is 1312 bytes against the pre-existing seal chain's 1640, so this section is not the module watermark
> **Accepted:** [H] production baseline reads still bypass the identity gate, so the UNINIT-to-NO_BASELINE laundering stays reachable until a consumer adopts it (reason: this section owns the gate, the record layer owns calling it and persisting the contract) -> XREF: 01-boot-platform/TODO-13 §27 (item: "Read every record through `tpm_nv_verify_and_read`, and persist the enrolled `struct tpm_nv_identity` this layer owns" at line 918)
> **Accepted:** [M] every verified read costs a second TPM transaction, and N records can each claim the 3000 ms budget (reason: both commands share ONE bounded sequence by design; an aggregate boot deadline is an adoption-time decision) -> XREF: 01-boot-platform/TODO-13 §27 (item: "Benchmark the aggregate Phase-1 verified-read cost before adoption, and impose ONE boot deadline rather than a fresh budget per record" at line 918)
> **Accepted:** [M] transient TPM contention reaches the baseline layer as `TPM_BASELINE_NO_TPM`, which does not say it is retryable (reason: `TPM_BASELINE_*` is the baseline layer's public enum, not this module's; the NV side only reports the condition) -> XREF: 01-boot-platform/TODO-13 §24 (item: "Give the baseline layer a RETRYABLE status so transient TPM contention is not reported as an unavailable TPM" at line 842)
> **Accepted:** [M] the module's deepest stack chain is the pre-existing seal path at 1640 bytes against 8 KiB task stacks (reason: measured NOT to be this section's chain, which is 1312; filing it where the deep path lives beats parking it here) -> XREF: 01-boot-platform/TODO-13 §24 (item: "Bound the per-call STACK cost of the seal path, which owns the module's deepest chain" at line 838)
> **Deferred:** [H] deletion is not yet unreachable from the ordinary OS path -- the delete policy is `PolicyCommandCode` only, which scopes but authenticates nobody, and the builder sends an empty platformAuth (reason: needs PolicySigned/PolicyAuthorize, which does not exist in the tree) -> XREF: 01-boot-platform/TODO-13 §27 (item: "Record-write must be unavailable to ordinary or rolled-back OS code" at line 905)
> **Quality reviewed:** 2026-08-18 | Codex 11x (design, adversarial x4, test-coverage, re-adversarial x3, consistency, perf) | 10H+11M+6L fixed, 1H+2M open (every one owned above) | scope: kernel-code-quality (kernel-quality-auditor: all 11 gates walked, no critical or high, 2M+7L fixed; concurrency-evidence-mapper: sequence/session/ownership inventory; parity via TCG Part 1-3 spec research) | the loop earned its rounds rather than spinning: every round through 3 found a real defect, and the two reviewers DISAGREED on stack pressure -- adjudicated by measuring rather than by picking a side

---

## 22. Bootloader-Side NV Floor Read (EFI_TCG2 Adapter)

> **Spawned-by:** §17 (split)

**This is the constraint that decides whether the trust anchor can satisfy TODO-21 §8 at all**, and an earlier draft of §17 missed it by publishing a kernel API alone. `select_active_slot()` runs at `src/boot/uefi/bootx64.c:16140`, before the kernel is loaded, so a below-floor slot is already executing by the time any kernel-side floor read happens. The existing `tpm_nv_*` wrappers ride the kernel's Phase-1 `tpm2_submit()` and have no bootloader counterpart at all.

Its failure mode is the opposite of §27's. §27 fails by accepting a forged record; this section fails by reading a perfectly good record too late, or by drifting from the kernel's marshaling so the two sides disagree about the same bytes. That is a producer/consumer duplication hazard of exactly the kind the `boot_info` mirror rule exists for, so the marshaling is SHARED rather than reimplemented loader-side.

- [ ] Add a typed `EFI_TCG2` `SubmitCommand` adapter in the loader, with the NV command marshaling SHARED with the kernel path -- one definition, no producer/consumer drift.
- [ ] Read the floor BEFORE `select_active_slot()` decides, and state what selection does when the read fails, the protocol is absent, or the TPM is disabled -- the no-TPM machine must still boot.
- [ ] Keep the ADVANCE kernel-side after a verified mark-good; the loader reads and never writes, so a compromised pre-kernel path cannot raise or lower the floor.
- [ ] Bound the loader-side cost against §17's budget and record a pre-ExitBootServices milestone on serial, so a slow TPM shows up as a boot-time number rather than an unexplained stall.
- [ ] Unit-test the shared marshaling from both sides against the same fixtures, and prove the absent-protocol and failed-read paths reach the stated selection outcome.
- [ ] Commit: `"boot: bootloader-side NV floor read"`

**Test checkpoint:** a below-floor slot is refused BEFORE the kernel is loaded, not after -- asserted at the selection call site rather than by a kernel-side check that runs too late to matter. The same NV read command marshalled by the loader path and by the kernel path is byte-identical over a shared fixture, which is the assertion that makes "one definition" checkable rather than a comment. A machine with no `EFI_TCG2` protocol, and one whose floor read fails, both reach the stated selection outcome and still boot; the smoke matrix is the backstop for that on all four legs. Scope: this section owns the LOADER read path only. The record format and its authorization are §27, the kernel-side read shape it mirrors is §28, the index lifecycle is §21, the wire primitives are §17, and A/B selection enforcement stays TODO-21 §8. Platforms: unit suites cover the marshaling parity; the QEMU legs cover the absent-protocol path, and a real-fTPM loader read is operator-gated (no `swtpm` on the dev host).

---

## 23. Headless Enrollment Authorization Escape Hatch

> **Spawned-by:** §17 (split)

§15 requires a physical keypress and refuses `CONFIRM_UNAVAILABLE` when no PS/2 console answers. That is the right default and it leaves a headless server unable to ever enroll a baseline, so today the only machines that can hold a measured-boot baseline are the ones with a keyboard attached.

**It is here rather than in §15 because it is the same authorization-construction problem as §27's floor record**, and it carries the same failure mode: a captured or replayed blob that authorizes an operation the operator never approved. The predecessor design this replaces was file-only authority on the ESP, which is precisely what §15 was built to remove -- so re-admitting it under a new name is the one outcome that would make things worse than the current fail-close.

- [ ] Specify a one-shot signed authorization bound to device EK/AK identity, the exact current PCR set, and a TPM-backed nonce or counter, so a captured blob authorizes nothing on a second boot or a different machine.
- [ ] Keep the signing secret and the replay state OFF the ESP; a blob an attacker can write beside the loader is file-only authority wearing a signature.
- [ ] Bind the authorization to the exact requested operation, so a blob captured for one enrollment cannot authorize a rotation, a reset, or a different baseline.
- [ ] Report the headless authority as its own distinct value beside the console one, never as a console confirmation. -> XREF: §15 (item: "Reported the enrollment AUTHORITY beside every verdict").
- [ ] Keep the §15 fail-close as the default: no valid authorization present means `CONFIRM_UNAVAILABLE`, exactly as today.
- [ ] Unit-test replay, wrong-machine, wrong-PCR-set, wrong-operation and expired-nonce as five distinct refusals, plus the accept path.
- [ ] Commit: `"tpm: headless enrollment authorization escape hatch"`

**Test checkpoint:** a valid one-shot authorization enrolls exactly once, and the same bytes replayed on the next boot are REFUSED -- the single assertion that separates this from the file-only authority §15 removed. The same blob presented on a different device identity, against a different PCR set, or for a different operation than the one it was signed for, is refused as three distinct values rather than one generic decline, so an operator reading the log can tell a stale blob from a wrong machine. With no authorization present at all the verdict is still `CONFIRM_UNAVAILABLE`, unchanged from §15, and a control asserts the console path is untouched by any of this. Scope: this section owns the headless enrollment AUTHORITY only. Who may enroll on a console machine stays §15, baseline content stays §6, and the NV record authorization it borrows is §27. Platforms: kernel unit suites cover every refusal and the accept path; an end-to-end headless enrollment against real firmware is operator-gated (no `swtpm` on the dev host, and the bare-metal leg stays with §6's platform item).

---

## 24. Extend the Bounded Sequence and Teardown Proof to Seal and Attestation

> **Spawned-by:** §17 (review)
> **User impact:** on a machine whose TPM is slow or briefly desynchronized, FDE unlock and EK-certificate reads can each spend their own unbounded per-command timeouts and can leak a session or transient-object handle per attempt. The TPM has only a few slots, so after a handful of retries `StartAuthSession` starts refusing outright and sealing stops working for the rest of the boot -- the user sees FDE unlock fail late, with no message connecting it to the slow TPM that caused it.

§17 gave the NV path a bounded sequence and a teardown that requires PROOF the session was released. The seal and attestation paths were not converted, so the two largest remaining multi-command flows in the subsystem still have the behavior §17 exists to remove. Found by §17's own consistency, perf and kernel-auditor passes, all three independently.

- [ ] Route `tpm_seal_secret` and `tpm_unseal_secret` through one bounded sequence each.
  - Today each runs 2-4 transport transactions with the gate released between them (`src/kernel/tpm_seal.c` CreatePrimary, Create/Load, and two `seal_flush` calls), so the per-burst PTP timeouts accumulate exactly as they did before §17.
- [ ] Give attestation a verified teardown, factoring §17's proof-requiring helper rather than writing a second one.
  - `at_flush` (`src/kernel/tpm_attest.c`) discards the FlushContext result, so a transient warning or a wrong-envelope reply leaves the session allocated while the code proceeds as if it were released -- the defect §17 fixed in `nv_flush`, still live here.
- [ ] Decide whether the sequence deadline should survive a thread migration. -> XREF: 01-boot-platform/TODO-10 (bare-metal hardening owns per-CPU TSC behavior).
  - `tpm_t_budget_spent` compares a raw RDTSC against a deadline recorded earlier, while `tpm2_seq_run` explicitly allows its owner to migrate; `per_cpu.tsc_offset` exists for this but has no corrected reader anywhere in the tree, so the choice is to build one, to pin the sequence, or to state the exposure and accept it. Pre-dates §17 (every TPM deadline already used raw TSC) but §17 widened its reach.
- [ ] Give the baseline layer a RETRYABLE status so transient TPM contention is not reported as an unavailable TPM. -> XREF: 01-boot-platform/TODO-13 §21 (item: "Index NAME and public attributes are verified before contents are read").
  - `nv_to_baseline` maps `TPM_NV_BUSY` and `TPM_NV_BUDGET` to `TPM_BASELINE_NO_TPM`, which is honest about the outcome (the verdict stays unpublished) but not about the CAUSE: both are retryable, and seal and attestation already preserve BUSY as such.
  - Adding `TPM_BASELINE_BUSY` changes a public enum the baseline layer owns, so it belongs here rather than in the NV module that merely reports the condition.
  - Acceptance: a bounded retry or a retry-aware caller, with a test proving contention does not abandon the integrity verdict under a false no-TPM diagnosis.
- [ ] Bound the per-call STACK cost of the seal path, which owns the module's deepest chain. -> XREF: 01-boot-platform/TODO-13 §21 (item: "Index NAME and public attributes are verified before contents are read").
  - Measured 2026-08-18 with `clang-19 -fstack-usage` on `src/kernel/tpm_nv.c`: `tpm_policy_session_run` 56 + `nv_policy_session_run_seq` 312 + `nv_policy_op_cb` 1272 = 1640 bytes live, against kernel task stacks of 8 KiB.
  - §21's verified-read chain measured 1312 bytes on the same run and is therefore NOT the watermark, which is why this is filed here rather than there. A §21 perf review read the 640-byte response buffer in isolation and called it new pressure; the measurement showed a deeper path already shipped.
  - Acceptance: a documented ceiling for TPM paths plus a check that fails the build when a function exceeds it, not a one-off measurement.
- [ ] Unit-test each converted flow for the same properties §17 proved: one budget across the whole operation, a teardown that requires proof, and a leak that is reported rather than silently accepted.
- [ ] Commit: `"tpm: bounded sequences and verified teardown for seal and attestation"`

**Test checkpoint:** a slow-but-responsive TPM makes a seal and an unseal report BUDGET rather than spending an unbounded multiple of the per-command timeouts, with a control proving the same fake completes inside a generous budget. An attestation teardown that receives a transient warning is retried and, if never proven, reported -- asserted by counting FlushContext commands, not by the operation's return value. Scope: this section converts the seal and attestation flows only; the NV primitives and their budget are §17, and the authenticated lifecycle is §21. Platforms: fake-TIS unit suites are the whole automatable surface; the live swtpm round trip is operator-gated (no `swtpm` on the dev host).

---

## 25. Field-Level Baseline Mismatch Attribution and Status Scoping

> **Spawned-by:** §18 (split)

Split out of §18 because it is a different surface with a different owner: §18 owns the report STRUCT, its publication path (`include/kernel/tpm.h`, `src/kernel/tpm.c`) and the PER-PCR comparison result it publishes, while this section adds WHICH SCALAR FIELD differed (`src/kernel/tpm_baseline.{h,c}`) and scopes what the reported status string claims. The boundary is per-PCR (§18) against scalar-field (§25); §18's design review is what drew it there, having found that the per-PCR detail §18 must publish cannot come from anywhere else.

- [ ] Attribute a mismatch to the FIELD that differed, not only to a PCR: every branch of `tpm_baseline_compare` collapses to one undifferentiated verdict today. -> XREF: 01-boot-platform/TODO-13 §16 (item: "Fixed the compare path").
  - Filed from §16's parity review. Bank, Secure Boot state, Secure Boot validity, firmware hash, ABI-manifest presence, ABI-manifest content and each PCR all return the same `TPM_BASELINE_MISMATCH`, which reaches the operator as the single string `"baseline-mismatch"` (`src/kernel/tpm.c`).
  - §18's per-PCR items do not cover the SCALAR fields, so this is a distinct gap rather than a restatement. Windows ships WBCL/TCG-log decoding and systemd-pcrlock is per-component precisely so a verifier can say WHICH component moved.
  - It becomes operator-visible the moment §16 ships: a pre-binding baseline flips to MISMATCH, and nothing distinguishes that known migration case from a genuine tamper event.
- [ ] Scope what the `"verified"` status string actually claims, since it does not yet mean the kernel IMAGE was verified. -> XREF: 01-boot-platform/TODO-13 §19 (item: "Consume the kernel-image digest from its owner").
  - `tpm_integrity_status_string()` reports a bare `"verified"` for a boot proven only against PCR replay, Secure Boot state, a firmware-version hash and the ABI-manifest shape. A reader carrying over Windows/Linux expectations will assume kernel content was measured, and it was not.
- [ ] Scope what the loader digest claims, and bind it to the firmware-measured executed image where the event log allows. -> XREF: 01-boot-platform/TODO-13 §20 (item: "Reopen `BOOTX64.EFI` through the loaded-image `DeviceHandle` + `FilePath`")
  - §20 ships a hash of the ESP FILE reached through `DeviceHandle` + `FilePath`. That is not the bytes firmware executed: the file can be replaced between `LoadImage` and the reopen, and a remount can resolve the same path elsewhere. The code names every field FILE for that reason, and this section owns stopping a consumer from upgrading the claim.
  - The binding that would make it executed-code evidence is the firmware event itself: PCR 4, `EV_EFI_BOOT_SERVICES_APPLICATION`, whose digest is an Authenticode PE hash over the loaded image rather than a flat file hash, so it needs the §1 event-log parser plus a PE-hash implementation and cannot be a variation on the flat hash.
  - Decide explicitly what happens when the two DISAGREE, because on a machine with no TPM or no event log there is nothing to compare and the honest answer is a weaker claim rather than a mismatch.
  - Parity research 2026-08-18 sharpens WHY this matters: firmware already measured this exact file via `LoadImage` as an `EV_EFI_BOOT_SERVICES_APPLICATION` event into PCR 4 (TCG PC Client Platform Firmware Profile v1.06r52), pre-relocation, Authenticode-hashed. Every real precedent measures the NEXT stage rather than itself: shim measures the image it chain-loads (PCR 4), GRUB the kernel/initrd/cmdline it reads (PCR 8/9), systemd-stub the UKI sections firmware cannot see (PCR 11-13), and shim deliberately skips re-measuring a UKI firmware already measured.
  - So an UNCORRELATED second hash of an already-measured file is the weak form, and the correlation is what turns it into evidence. Without it a TOCTOU swap between `LoadImage` and the reopen yields two internally consistent digests and nothing notices, which is exactly the gap §20 documents but cannot close.
  - Three things a verifier needs that the serial line cannot provide, all belonging with the binding rather than with §20: the digest reaching `TPM2_PCR_Extend` and the event log at all (today it exists only as UART text, invisible to remote attestation), the ESP path that was hashed (A/B and alternate boot entries make "which file" a real question), and a nonce or monotonic counter so one boot's line cannot be replayed as another's.
- [ ] Commit: `"tpm: field-level baseline mismatch attribution"`

**Test checkpoint:** each distinguishable mismatch cause (bank, Secure Boot state, Secure Boot validity, firmware hash, ABI-manifest presence, ABI-manifest content, a PCR digest) produces its OWN reported reason rather than the single `"baseline-mismatch"` string, with a control asserting a matching baseline still reports VERIFIED. The `"verified"` string states what it covers, so a reader cannot read kernel-image measurement into it. Scope: this section owns the compare path's return detail and the status strings only; the report struct and its publication are §18. Platforms: kernel unit suites; no TPM required.

---

## 26. Baseline-Blob NV Fake and Wrapper-Level Verify Coverage

> **Spawned-by:** §18 (review)
> **User impact:** on a boot where the measured state genuinely changed, the operator is told WHICH PCR moved. That attribution is plumbed through `tpm_baseline_verify` and nothing tests the plumbing, so a refactor can silently pass the wrong snapshots, the wrong capacity, or drop the call entirely, and the report goes back to a bare "baseline-mismatch" with no per-PCR detail. The pure comparison tests stay green throughout, which is exactly what makes it survivable.

Filed by §18's test-coverage review. §18 shipped the per-PCR detail and covered `tpm_baseline_compare_pcrs` thoroughly as a pure function, plus the one wrapper path reachable without a fake: no transport, which proves the not-evaluated sentinel is set. Every path that actually COMPARES is untested at the wrapper level, because reaching it needs an NV fake that returns a crafted `struct tpm_baseline` blob and PCR reads whose digests match it.

The existing fake in `test_tpm_nv.c` serves only a 4-byte `"DATA"` payload or an 8-byte counter (`nvf_read_payload_len`, `test_tpm_nv.c:380-392`), so it cannot carry a baseline blob. This section builds that capability once, for the whole TPM suite.

- [ ] Extend the NV fake so an `NV_Read` can return caller-supplied bytes of arbitrary length, rather than the two fixed payload shapes it has today.
- [ ] Add a PCR-read fake path so `tpm_pcr_get` resolves to controlled digests without `tpm_pcr_cache_init` (a subsystem `_init`, which tests may not call).
- [ ] Cover `tpm_baseline_verify` end-to-end for: full match, one PCR differing, scalar-only mismatch (Secure Boot / ABI manifest), NO_BASELINE, corrupt blob, and snapshot failure.
  - Assert the overall verdict, the returned count, and EVERY per-PCR status together -- the point is that they describe one comparison. Assert the untouched sentinels on the non-comparing paths too.
  - The scalar-only case is the one that matters most: overall MISMATCH with every PCR VERIFIED is the shape that stops a Secure Boot change reading as PCR tampering. -> XREF: 01-boot-platform/TODO-13 §18 (item: "`tpm_integrity_publish_baseline()` replaces `tpm_integrity_set_overall_status()`").
- [ ] Cover the enroll wrapper against the same fake while it is being built, since it reads the existing blob for the generation guard.
- [/] Deterministic concurrent-publication coverage: a `KERNEL_TESTS`-gated hook widening the publication window so an unlocked publication FAILS. BLOCKED on the scheduler join/wait fix (see sub-bullets).
  - §18 WROTE a `race_barrier` + kthread concurrent suite and then REMOVED it, which is the evidence here rather than a gap to re-discover. Mutation-checked: deleting the lock pair from `tpm_integrity_report_copy()` left it green at 1917/1917, because the window is a sub-microsecond memcpy inside an IRQs-disabled region and the interleaving never lands in a bounded run. It also could not force the tamper race -- both writers rendezvous only before their loops, so writer A can finish before B raises TAMPER and removing the pin guard would still pass.
  - It was removed rather than kept because it can HANG THE WHOLE SUITE for no detection benefit: `thread_join` has a documented publish-before-block window that can strand the joiner forever (`src/kernel/sched/task.c:5964-5971`), and a stalled run also leaves the live integrity report unrestored.
  - So this is BLOCKED on scheduler work, not TPM work. -> XREF: 03-memory-concurrency/TODO-06 (item: "Commit: `\"sched: wait/wake transaction locking -- close the lost-wakeup window in all wait primitives\"`"). Once a bounded race-free join exists, the widener is a busy-wait (never a yield -- the reader holds a spinlock, and `include/kernel/test/race_barrier.h` records that yielding under a caller-held spinlock deadlocks), and acceptance is the mutation itself: with the hook armed, removing the lock must fail.
  - The sequential suites still cover the pin deterministically (both writer orderings through the real `tpm_integrity_set_replay_verdict`), so what is missing is the CONCURRENT proof, not the pin. -> XREF: 01-boot-platform/TODO-13 §18 (item: "Report built OFF-lock and published by release-store swap into one of two immutable slots").
- [ ] Commit: `"test: baseline-blob NV fake and wrapper-level verify coverage"`

**Test checkpoint:** deleting the `tpm_baseline_compare_pcrs` call from `tpm_baseline_verify`, passing it the wrong capacity, or swapping golden and current each makes at least one assertion FAIL -- that is the regression the pure-core tests cannot see, so a control proving the suite is green before the mutation is part of the checkpoint. Scope: this section owns the fake and the wrapper coverage; the comparison logic itself is §18 and the scalar attribution is §25. Platforms: fake-TIS kernel unit suites; no live TPM (the dev host has no `swtpm`).

---

## 27. Authorized NV Record Transitions

> **Spawned-by:** §21 (split)

Split out of §21 on 2026-08-17, which was carrying seven greenfield work items across two constructions with opposite failure modes. §21 owns the index the record lives in: who may create it, who may destroy it, and proving the handle answers with the index that was enrolled. This section owns the RECORD, and it fails the other way round: the index is perfectly intact, its Name verifies, the counter is monotonic, and the value inside it is still a lie an attacker chose.

**Split again on 2026-08-18** on a SPLIT-RECOMMENDED complexity verdict (7 work items, 6 files, ABI impact). This section keeps the AUTHORIZATION construction and the two consumers that share it. The crash-consistent pairing, the published floor APIs and the aggregate read budget are §28. The seam is the failure mode rather than the file: this section fails when a FORGED record is accepted, §28 fails when an HONEST one is misread.

**The pairing alone is NOT anti-rollback, and an earlier wording ("the counter supplies monotonicity") got that wrong.** A holder of record-write authority can increment the counter once and store a LOWER version: the counter advanced, the record matches it, there is no skew, the index Name and attributes are still valid, so every cooperative check passes and the bootloader accepts a vulnerable slot. So the AUTHORIZATION carries the monotonicity and the arithmetic never does.

**Both consumers have the identical hole, which is why they are one section rather than two.** The floor record can be written with a lower version under a fresh counter; the baseline record can be relabelled with the current generation and a recomputed CRC. The same construction closes both: an authorization that names the exact (counter transition, whole-record digest) pair. Splitting the consumers apart would build that construction twice.

- [x] The A/B floor is an authenticated DATA record holding the security version, bound to a +1 counter transaction, never the counter value itself.
  - Two earlier drafts were wrong in opposite directions. "Increment-only over an arbitrary jump" contradicted the only primitive available. "One increment per release ordinal" then made the counter BE the version, which a TPM counter cannot represent: it starts at zero and moves by one, so a fresh install at ordinal 500 needs 500 transactions, and rejecting that delta leaves the stored counter numerically unrelated to the `rollback_index` the A/B ABI compares.
  - The counter supplies UPDATE SEQUENCE and the record supplies the VALUE. Store the counter generation INSIDE the record and verify the whole transition on read.
  - Record-write must be unavailable to ordinary or rolled-back OS code, authorizing the exact (counter transition, new version) pair. The 2026-08-18 design review rejected direct `PolicySigned` for this: its `aHash` binds `nonceTPM`, which the TPM mints per session (`tpm2_parse_start_auth_session`, `include/kernel/tpm_nv.h:452`), so only an ONLINE signer could produce it and this authority is offline.
  - So the index carries a `PolicyAuthorize` authPolicy over an authority-signed APPROVED policy, and the approved policy carries `PolicyCommandCode` + `PolicyCpHash` + `PolicyNV`. The kernel turns the authority's detached signature into a ticket with `VerifySignature` and never verifies a signature itself. None of `PolicyAuthorize`, `PolicyCpHash`, `PolicyNV`, `VerifySignature`, `LoadExternal` or any cpHash computation exists in the tree; `tpm2_build_policy_secret` emits an always-empty `cpHashA` (`src/kernel/tpm_attest.c:125`).
  - The cpHash is computed over the LIVE NV Name, because `TPMA_NV_WRITTEN` is inside the hashed public area and the Name therefore changes after the first write; a cpHash built from the enrolled Name would authorize nothing once the index is initialized.
  - The commit point is the POLICY-PROTECTED `NV_Increment`, run after the authorized record is written and read back. Increment-then-write advances irreversible state before the authorized bytes exist, and an increment left reachable under ordinary owner auth would let an attacker manufacture the counter-ahead residue deliberately, which makes it an authorization defect here rather than a crash-consistency one in §28.
  - One transition lock covers the whole re-read, write, readback and increment, or two concurrent rotations both observe the same generation and both believe they advanced it.
  - The whole-record digest canonicalizes its own coverage: the digest field and every padding byte are zeroed before hashing, because hashing a record that already contains its digest is circular.
  - Test the bypass directly: a lower version paired with a freshly incremented counter must be REFUSED, not accepted as the newest record. A read finding a HIGHER stored value than the caller expects likewise stays a REFUSAL.
- [x] Consumer: baseline rotation gets the SAME authorization boundary as the floor record, not merely a counter to consult. -> XREF: 01-boot-platform/TODO-13 §6 (item: "Baseline rotation under the recovery gate + monotonic generation counter").
  - The baseline index is owner-writable, so an attacker can take an OLD vulnerable baseline, stamp it with the CURRENT counter generation, recompute the CRC and write it back without touching the counter at all. Nothing is skewed, the CRC validates, and verify accepts a vulnerable measured state as golden.
  - So baseline-record writes authorize the exact counter transition together with a DIGEST OF THE WHOLE new record, under the §15 enrollment/rotation authority. A CRC is an integrity check against corruption and was never an authenticity check; `gpt_crc32` over the blob (`tpm_baseline.c:34`) is doing exactly the job it is fit for and no more.
  - An authPolicy is fixed at `NV_DefineSpace` and is hashed into the index Name, so an enrolled baseline index cannot ACQUIRE this boundary in place. The boundary therefore ships as a SIDE-BY-SIDE v2 index: this section owns the v2 contract constants, the detection of a legacy v1 index and the refusal to silently fall back to it. -> XREF: 01-boot-platform/TODO-13 §19 (item: "Migrate an enrolled v1 baseline index to the policy-bearing v2 contract").
  - Never auto-wrap the legacy blob into the protected index: v1 content is owner-writable, so copying it across would authenticate attacker-controlled data under the new policy and leave the boundary worse than no boundary.
  - The BOUNDARY is enforced, not merely offered: `tpm_baseline_enroll_bound` writes the blob and binds the bytes it reads back, and the bare `tpm_baseline_enroll` REFUSES with `TPM_BASELINE_UNBOUND` while an authority is provisioned. An adversarial round caught the first version of this item marked done on the API existing while production still did a bare owner write.
  - The production CALLER is not this section's to supply: `boot_interrupts.c` still calls the plain enroll, because nothing yet produces transition grants. That is the same first-provisioning gap filed with the enrollment owner, and it is stated here rather than left to be re-discovered. -> XREF: 01-boot-platform/TODO-13 §15 (item: "Provision the authorized anti-rollback anchors").
  - A second round caught the bound path binding a FRESH snapshot rather than the stored blob; a snapshot's generation is zero where the stored blob carries the one the enroll assigned, so every rotation would have verified as a mismatch. It now binds the bytes read back from the index.
  - Test the relabelling directly: old baseline content, current counter generation, valid CRC, must be REFUSED.
- [x] The enrolled `struct tpm_nv_identity` is DERIVED from a compiled manifest (`s_manifest`, `src/kernel/tpm_authz.c`), and every record read verifies identity, lifecycle and contents inside ONE bounded sequence.
  - -> XREF: 01-boot-platform/TODO-13 §21 (item: "Index NAME and public attributes are verified before contents are read").
  - §21 ships the gate and the ordering but deliberately stores nothing: judging an index against an enrolled contract needs that contract, which is record state and belongs here.
  - The 2026-08-18 design review rejected PERSISTING it as the authority. The contract carries `nv_index` and `tpm_nv_verify_and_read` follows that handle (`include/kernel/tpm_nv.h:986`), so a swapped contract redirects the verified read to an index an attacker defined to match it, which is fail-OPEN. `tpm_authz_contract_cache_ok` therefore compares a stored copy field by field against the manifest and never prefers it.
  - SHIPPED SHAPE, which is NOT `tpm_nv_verify_and_read` as the item originally said: that wrapper opens its own sequence, and a transition has to read the COUNTER in the same one, so `authz_read_seq` performs the identity read, the contract comparison, the lifecycle check and the content read inside one `tpm2_seq_run`. It keeps both properties the wrapper exists for -- the handle comes from the contract, never a caller argument, and the verdict cannot expire between the check and the read.
  - The lifecycle check is the half that is easy to omit and was: `tpm_nv_identity_match` normalizes `TPMA_NV_WRITTEN` away, so a byte-identical destroy-and-recreate passes it, then reads UNINIT, which downstream means "no record yet". Found while reconciling this item against the code; the read now reports `TPM_NV_RECREATED` and `tpm_ab_floor_advance` refuses rather than writing a fresh floor over the gap.
  - Use `tpm_nv_verify_and_read`, NOT `tpm_nv_verify_identity` followed by a read: the standalone check runs in its own transport sequence and its answer expires when that sequence closes, so the pair is a check-then-use race. Treat `TPM_NV_CONTRACT` as corrupt persisted state needing authorized recovery, distinct from `TPM_NV_BADARG`.
- [x] Unit-tested the relabel refusal, the lower-version-with-fresh-counter refusal and the increment-only invariant through the fake-TIS seam, each beside a passing control.
- [ ] Drive `tpm_baseline_enroll_bound` through the fake-TIS seam: it has no executable coverage, so the authority guard, the stored-byte readback and the bind ordering can all regress green.
  - Needs a deterministic core that takes a prepared snapshot, or a `KERNEL_TESTS` snapshot-provider seam, because the real path calls `tpm_baseline_snapshot` and a test may not stand up PCR state.
  - Assert blob-before-bind ordering, that the bind covers the EXACT stored bytes and generation, and that a readback or bind failure propagates rather than reporting success.
- [ ] Cover the write path's recreated-anchor gate directly: the current test returns `TPM_NV_RECREATED` from the READ path before `tpm_authz_write_record` is ever reached, so the separate write-side check has no coverage.
  - Call `tpm_authz_write_record` with the fixture reporting WRITTEN clear, and assert the refusal lands BEFORE LoadExternal, NV_Write or NV_Increment, beside a written-index success control.
- [ ] Give the fake a corrupted-readback mode: the transaction promises not to commit until the record reads back byte for byte, but the fixture always returns exactly what was written, so deleting that comparison would leave every suite green.
  - Assert `TPM_NV_MISMATCH`, one write, zero increments and no surviving handles for both an altered and a short readback.
- [ ] Cover the missing and uninitialized anchor refusals at the public writers, so an absent or never-written index cannot be papered over by a fresh write.
- [x] Commit: `"tpm: authorized NV record transitions"`

**Test checkpoint:** old baseline content stamped with the current counter generation and a valid CRC is REFUSED, and so is a lower version paired with a freshly incremented counter. Those are the two shapes that pass every cooperative check §17 and §21 can make, which is why they are the acceptance rather than the counter arithmetic. Each floor helper refuses an advance that would lower its own stored value and refuses a read whose expectation is below what is stored. A first install at a high security version, and an upgrade skipping several releases, both work with ONE counter transaction rather than one per version step. A control asserting that a correctly authorized rotation and a correctly authorized floor advance both SUCCEED runs beside every refusal; without it each refusal assertion would pass against a helper that refuses everything. Scope: this section owns record AUTHORIZATION for TRANSITIONS between existing records, and the enrolled identity it derives. It deliberately does NOT provision a first record: an advance that wrote one over a missing record could not distinguish an unprovisioned anchor from a destroyed one, and the first write's generation is unknowable before the counter's first increment, so the authorization for it is an enrollment question owned by §15. The crash-consistent pairing, the published floor APIs and the read budget are §28, the index lifecycle and identity are §21, the wire primitives are §17, the loader-side read is §22, the headless authorization is §23, A/B selection enforcement stays TODO-21 §8, and baseline content stays §6. Platforms: fake-TIS unit suites are the whole automatable surface; live swtpm and real-fTPM signed-policy semantics are operator-gated (no `swtpm` on the dev host).

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 25 new suites in `src/kernel/test/test_tpm_authz.c` | security suite 2139 -> 2417 kernel assertions, 0 failures

> **Notes:**
> - Shipped the authorization boundary for NV records: `tpm_record.{h,c}` (the authenticated record and its transition rules) and `tpm_authz.{h,c}` (the compiled enrollment manifest, the offline authority, and the write-then-increment transition), plus five TPM2 policy builders and their parsers in `tpm_nv.{h,c}`.
> - Integrates by binding, not by replacement: the baseline blob stays in its owner-writable index and gains a side-by-side bind record, so `tpm_baseline_verify` now refuses an unbound blob as `TPM_BASELINE_UNBOUND` whenever an authority is provisioned.
> - Downstream, the two anti-rollback counters became POLICYWRITE-only, so `tpm_nv_increment` refuses them outright and the irreversible commit point is unreachable without the authority.
> - Fails CLOSED by configuration: no authority key is compiled in by default, every authorized write returns `TPM_NV_UNAVAIL`, and the legacy baseline path is left untouched rather than reporting a false mismatch on every enrolled machine.
> - Known boundary, filed not hidden: nothing yet provisions a FIRST record, so a machine with an authority compiled in has the boundary and no way through it (`- [ ]` in §15, with the inverted-ordering reason recorded there).
> - Canonical doc: the header banners in `include/kernel/tpm_authz.h` and `include/kernel/tpm_record.h` carry the threat model and the write-then-increment argument.
> - Scope boundary: this section owns record AUTHORIZATION and the enrolled identity it derives; the crash-consistent pairing, the published floor APIs and the aggregate read budget are the next section, and the v1 baseline migration belongs to the versioned-growth work.

---

## 28. Crash-Consistent Record Pairing, Floor APIs and Verified-Read Boot Budget

> **Spawned-by:** §27 (split)

Split out of §27 on 2026-08-18 on a SPLIT-RECOMMENDED complexity verdict. §27 owns the authorization that makes a record trustworthy to WRITE. This section owns everything that decides whether an honest record is read CORRECTLY: the ordering between the counter transaction and the record write, what a torn pairing means on the next boot, the API shape the consumers and the loader both call, and what the whole verified-read path costs before Phase 1 will carry it.

**Its failure mode is the opposite of its parent's.** §27 fails when a forged record is ACCEPTED. This section fails when an honest one is MISREAD: a counter that advanced with no record behind it trusted as current, a recovery path that papers a rollback over with a fresh enrollment, or a verified-read path that misses the boot deadline and gets disabled for it. A crash is not an attacker and does not need an authorization answer; it needs an ordering and a detector.

**The budget belongs with the read, not with the authorization.** Each verified read is two TPM transactions where the unverified read was one, so the cost is a property of the READ path this section publishes, and it is the one part §27 cannot bound because §27 does not decide how many records Phase 1 reads.

- [ ] Specify the baseline counter's crash-consistent ordering against the blob write, and how a counter-ahead-of-blob pairing is detected and resolved rather than silently trusted.
  - `tpm_baseline_enroll` today defines the index (`src/kernel/tpm_baseline.c:394`) and writes the blob under owner auth (`:397`) with nothing between them, so there is no ordering to reason about yet and no skew record to consult.
  - Name the commit point explicitly. A counter that advanced with no blob behind it is a corrupt pairing, and the next verify has to DETECT it rather than treat the advance as evidence the blob is current.
  - The recovery path is reached rather than a fresh enrollment papering over the gap, which is the shape that would silently launder a rollback.
- [ ] Publish the kernel-side floor and generation APIs the consumers call, stating the READ shape §22 must reach from the loader. -> XREF: 01-boot-platform/TODO-13 §22 (item: "Add a typed `EFI_TCG2` `SubmitCommand` adapter").
  - The read shape is the part §22 cannot design for itself: the loader has no `tpm2_submit()` and must reach the same bytes through `EFI_TCG2` `SubmitCommand`, so the marshalling this section publishes is the one definition both sides share.
  - The API surfaces the skew verdict as its own outcome rather than folding it into a generic read error, because the caller's response to "torn pairing" is recovery and its response to a transport error is retry.
- [ ] Benchmark the aggregate Phase-1 verified-read cost before adoption, and impose ONE boot deadline rather than a fresh budget per record.
  - Each verified read is two TPM transactions (ReadPublic then NV_Read) sharing one bounded sequence, where the unverified read was one; the per-call budget is 3000 ms, so N records can each claim it.
  - Acceptance: measured on the slowest supported TPM, with budget-expiry and contention tests for the two-command path. The measurement leg is operator-gated (no `swtpm` and no discrete TPM on the dev host); the deadline accounting itself is unit-testable through the fake-TIS seam.
- [ ] Unit-test the counter-ahead-of-blob recovery path and the single-deadline accounting through the fake-TIS seam, each beside a passing control.
- [ ] Commit: `"tpm: crash-consistent record pairing and verified-read boot budget"`

**Test checkpoint:** a baseline counter incremented with no blob write behind it is DETECTED on the next verify rather than trusted, and the recovery path is reached rather than a fresh enrollment papering over the gap. A control asserting that an intact pairing verifies CLEANLY runs beside it, because a detector that fires on everything would pass the refusal assertion alone. The floor and generation read helpers marshal the same bytes the loader path must produce, asserted over a shared fixture rather than by comment. Aggregate verified reads across Phase 1 are charged against ONE deadline: a test proves that N records cannot each claim the full per-call budget, and that expiry is reported rather than silently truncating the read set. Scope: this section owns the crash-consistent pairing, the published read APIs and the boot budget. Record authorization is §27, the index lifecycle is §21, the wire primitives are §17, the loader-side adapter is §22, and baseline content stays §6. Platforms: fake-TIS unit suites are the whole automatable surface; the slowest-TPM measurement is operator-gated (no `swtpm` on the dev host).

---

## OS Comparison

| ⭐  | Feature                                | Windows                     | Linux                                  | Impossible OS                                                                    |
| --- | -------------------------------------- | --------------------------- | -------------------------------------- | -------------------------------------------------------------------------------- |
| 💎  | TPM2 command transport (TIS/CRB)       | tpm.sys TIS/CRB             | tpm_tis/tpm_crb drivers                | ✅ §2 burst-chunked TIS + CRB                                                    |
| 💎  | Secure Boot PCR integration            | Measured Boot               | IMA/TPM tools                          | ⚠️ §5 structural SB var reconcile                                                |
| 💎  | PCR replay                             | internal/Defender           | tpm2-tools                             | ✅ §4 SHA-1/256/384/512 replay + tamper verify                                   |
| 💎  | TPM NV index storage (PCR-sealed)      | TBS NV / BitLocker          | tpm2_nvdefine + kernel RM              | ✅ §7 NV CRUD + PolicyPCR-sealed baseline index                                  |
| 💎  | NV counter / write-lock primitives     | TBS NV counters             | tpm2_nvincrement / nvwritelock         | ✅ §17 TPMA_NV + TPM_NT model, Increment/WriteLock, bounded command budget       |
| 💎  | Authorized anti-rollback record writes | Signed policy over TBS NV   | PolicyAuthorize + tpm2_policyauthorize | ✅ §27 PolicyAuthorize + cpHash + PolicyNV, write-then-increment commit          |
| 💎  | Measured-boot baseline (enroll/verify) | Measured Boot baseline      | IMA + systemd-pcrlock                  | ⚠️ §6 recovery-gated enroll + Phase-1 verify + generation rotation               |
| 💎  | Kernel-ABI identity bound in baseline  | Boot config / WBCL binding  | IMA template hash binding              | ✅ §16 build-time `.bootproto` sha256, fail-closed, symmetric presence gate      |
| 💎  | Sealed secrets                         | BitLocker                   | systemd-cryptenroll                    | ✅ §8 PCR-7 KEYEDHASH seal (PolicyPCR) + FDE/CI hooks + recovery handoff         |
| ⭐  | Boot attestation report (JSON)         | Device Health Attestation   | Keylime AK quote JSON                  | ⚠️ §9 signed report to `X:\Diag\attestation.json`; query API + PCR-11 pending    |
| 💎  | Remote attestation (TPM2 Quote)        | Device Health Attestation   | Keylime AK quote                       | ✅ §13 EK->AK provision + TPM2_Quote + nonce anti-replay                         |
| 💎  | PCR allocation policy                  | PCR7+11 BitLocker seal      | systemd-pcrlock CEL                    | ✅ §12 event-centric table + derived masks                                       |
| ⭐  | Baseline enrollment authority          | TPM PPI physical presence   | root + interactive prompt              | ✅ §15 NVRAM-anchored gate + console confirm + reported authority value          |
| 💎  | Boot-integrity report publication      | Measured Boot / WBCL state  | sysfs PCRs + IMA runtime               | ✅ §18 immutable snapshot swap, copy-out readers, per-PCR detail with verdict    |
| 💎  | Loader binary identity                 | Firmware PCR-4 Authenticode | shim/GRUB self-measure to PCR 4/9      | ⚠️ §20 on-disk file digest on serial; PCR extend + event-log correlation are §25 |
| 💎  | NV index identity + delete authority   | TBS index handle trust      | tpm2_nvreadpublic name; policy-delete  | ✅ §21 Name verified before any content read; POLICY_DELETE platform-only        |

## Unit Tests

- [x] Shipped with §2 (2 suites, TEST_CAT_SECURITY, `test_tpm_transport.c`): transport marshaling + fake-TIS submit (chunking, protocol violations, sticky fail, reentrancy).
- [ ] `test_tpm_event_log_tpm12_fixture`
- [ ] `test_tpm_event_log_tpm20_fixture`
- [ ] `test_tpm_pcr_replay_sha256`
- [x] `test_tpm_integrity_mismatch_report` -- §18 ships it as "tpm: boot-integrity report publication" + "tpm: baseline per-PCR compare detail" (`test_tpm_baseline.c`): MISMATCH publication, per-PCR attribution, TAMPER pin, writer orderings.
- [ ] `test_tpm_pcr_allocation_manifest_layer` (§12: manifest-only change attributed to kernel-ABI layer, not blanket mismatch)
- [ ] `test_tpm_quote_verify_nonce` (§13: swtpm quote verifies under AK pubkey + carries nonce; stale-nonce replay rejected)

## Verification

- [ ] QEMU OVMF + swtpm
- [ ] Secure Boot enabled OVMF
- [ ] Bare metal with TPM 2.0
- [ ] Bare metal without TPM degrades cleanly
