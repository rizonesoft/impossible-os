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

| ⭐  | Order | Deliverable                                                  | Depends On                                                | Status |
| --- | :---: | ------------------------------------------------------------ | --------------------------------------------------------- | :----: |
| 💎  |   1   | Harden TCG event-log parser                                  | --                                                        |  [/]   |
| 💎  |   2   | TPM2 command transport                                       | §1 (ordering-only; transport does not consume the parser) |  [x]   |
| 💎  |   3   | PCR read API                                                 | §2                                                        |  [x]   |
| 💎  |   4   | PCR replay engine                                            | §1, ../02-kernel-core/TODO-03 §3                          |  [x]   |
| 💎  |   5   | Secure Boot variable measurement reconciliation (structural) | §1, TODO-02 §3                                            |  [x]   |
| 💎  |   6   | Baseline enrollment and storage                              | §3, §4, §7                                                |  [/]   |
| 💎  |   7   | TPM NV index support                                         | §2 (transport); §12 (baseline mask)                       |  [/]   |
| ⭐  |   8   | Sealed-secret boot policy hooks                              | §7, §12                                                   |  [x]   |
| 💎  |   9   | Attestation report export                                    | §3-§6, §12, §13                                           |  [/]   |
| ⭐  |  10   | Recovery and mismatch UX                                     | §6, TODO-22                                               |  [/]   |
| 💎  |  11   | TPM tests and event-log fixtures                             | §1-§10, §12, §13                                          |  [x]   |
| 💎  |  12   | PCR allocation table and policy masks                        | (foundational; consumed by §6/§8/§13)                     |  [x]   |
| 💎  |  13   | Attestation key provisioning and TPM2 quote                  | §3, §7, §12                                               |  [x]   |
| 💎  |  14   | Post-ship follow-up backfill (2026-07-31 cohort)             | --                                                        |  [x]   |
| 💎  |  15   | Trusted enrollment provenance                                | §6, §14                                                   |  [ ]   |
| 💎  |  16   | Baseline image identity (real bootloader + kernel hashes)    | §6, §14, TODO-01 (boot_info ABI)                          |  [ ]   |
| 💎  |  17   | Write-locked and monotonic NV indexes (anti-rollback anchor) | §6, §7, §14                                               |  [ ]   |
| 💎  |  18   | Atomic boot-integrity report publication                     | §6, §12, §14                                              |  [ ]   |

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
> **Accepted:** [M] ACPI start methods 2/8 degrade-with-WARN until an AML interpreter exists -> XREF: 04-drivers-hardware/TODO-03 §1 (item: "TPM2 ACPI start method (2/8)" at line 80)
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
> - Scope: §6 owns baseline CONTENT/enroll/verify/rotate; console confirmation is §15, image-digest carriage is §16, NV write-lock anti-rollback is §17 (moved there by the §14 split; "infra-blocked" was wrong for the console half, which is buildable).
> **Verified:** 2026-06-14 | commit `c7032e45` (impl) + review fixes | 4/9 items | build OK | tests 668 security PASS, smoke PASS (KVM 2.50s)
> **Accepted:** [H] enrollment gate is boot.conf config (`boot_mode==recovery && tpm_enroll`), not loader-validated recovery provenance -- config-spoofable -> XREF: 01-boot-platform/TODO-13 §15 (item: "Gate enrollment on the LOADER-decided recovery signal") (RETARGETED 2026-08-17: the follow-up item moved out of this section when §14 split the backfill cohort)
> **Accepted:** [H] baseline verify leaves stale per-PCR `NO_CRYPTO` + omits PCR11 in `boot_integrity_report` (self-contradicts VERIFIED) -> XREF: 01-boot-platform/TODO-13 §18 (item: "Refresh every per-PCR status when the baseline verdict is published") (RETARGETED 2026-08-17: the follow-up item moved out of this section when §14 split the backfill cohort)
> **Accepted:** [M] measured PCR set `{0-7,11}` duplicated across `tpm.c`/`tpm_replay.c`/`tpm_baseline.c`/test -> XREF: 01-boot-platform/TODO-13 §6 (item: "Follow-up: consolidate the measured PCR set" at line 179) (RESOLVED 2026-06-14 by §6 commit 2b0dd4ed: `tpm_pcr_baseline_pcrs()` derives the set from the allocation-table mask; the 3 consumers migrated)
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
> - Scope: §7 owns the NV storage MECHANISM only; §6 owns baseline CONTENT/enrollment. The UEFI-var -> TPM-NV migration item was closed not-applicable by §14 (§6 never used a UEFI variable); write-lock/monotonic indexes are §17.
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
> **Deferred:** [H] cryptographic response authenticity + parameter encryption vs a physical bus interposer (forged well-formed success / key sniffing); structural validation only today -> XREF: 01-boot-platform/TODO-13 §8 (item: "salted/bound HMAC sessions + parameter encryption" at line 227)
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
> **Deferred:** [M] native attestation query API for user-mode (new syscall/SSDT ABI exposing attestation evidence + an access-control decision; stop-and-ask) -> XREF: 01-boot-platform/TODO-13 §9 (item: "Add native query API for user-mode system settings" at line 251)
> **Deferred:** [M] bootloader PCR-11 manifest extend + `BOOT_CAP_MANIFEST_PCR_BOUND` cap-bit (firmware TCG2 HashLogExtendEvent; real-HW validation; cap-bit ABI owned by TODO-01 §11) -> XREF: 01-boot-platform/TODO-13 §9 (item: "Extend a TPM PCR (target PCR 11..." at line 269)
> **Deferred:** [L] remote-attestation placeholder needs a remote-attest protocol design decision -> XREF: 01-boot-platform/TODO-13 §9 (item: "Add remote-attestation placeholder for platform services" at line 252)
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
> **Deferred:** [H] `TPMS_ATTEST.qualifiedSigner` not bound to the AK Qualified Name -> XREF: 01-boot-platform/TODO-13 §13 (item: "Bind `TPMS_ATTEST.qualifiedSigner` to the AK Qualified Name" at line 355) (reason: needs EK-pub capture + Name-algebra; verifier binds the signer via the exported AK pub)
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
- TPM NV write-lock / monotonic-counter anti-rollback for baseline rotation, from the stamped section 6 -> §17.
- Monotonic / write-locked NV index for the A/B per-slot anti-rollback floor, from the stamped section 7 -> §17, the same mechanism with a second consumer.
- Atomic `boot_integrity_report` per-PCR publication, from the stamped section 6 -> §18.

**Test checkpoint:** no code ships from this section. The not-applicable verdict is checkable by grep -- `src/kernel/tpm_baseline.c` has no UEFI variable call and its enroll/verify paths go through `tpm_nv_*`. The migrated items are covered by their new sections' checkpoints.

> **Notes:**
> - **What shipped:** a disposition, not code -- every cohort item traced to current source before routing, so the split rests on what the tree does rather than on the items' own descriptions.
> - **How it runs:** one item closed as not-applicable to §6's blob (narrowly, not corpus-wide); five migrated to §15-§18, which own the work and state what they do not re-cover.
> - **Downstream effects:** TODO-21 §8's Critical deferral now resolves against §17; reciprocal items filed in `04-drivers-hardware/TODO-04 §7` and `18-future-research/TODO-04 §2`, which produce the kernel digests §16 consumes.
> - **Canonical doc:** the per-section bodies of §15-§18; the review trail behind their current shape is in the stamp commit message.
> - **Scope boundary:** §14 owns the cohort record only. §17 carries the two items that are one mechanism; §16's NV-index migration is where §14's deferred chunked-NV question comes due.

> **Verified:** 2026-08-17 | split disposition, no code | 2/2 items | build OK (`=== BUILD OK ===`, unchanged tree) | lint rc 0 | todo-graph 10/10 | section-order clean | evidence: `tpm_baseline.c:222-309` enrolls/verifies through `tpm_nv_*` against `TPM_NV_INDEX_BASELINE` with no `SetVariable` in the file; the other five items are open `[ ]` at §15-§18
> **Quality reviewed:** 2026-08-17 | Codex 8x (adversarial, consistency, perf, re-adversarial x5) | 16H+8M fixed, 0 open | round 8 closed on an explicit approve | scope: N/A (roadmap text, no code surface; parity research skipped for the same reason)

---

## 15. Trusted Enrollment Provenance

> **Spawned-by:** §14 (split)

§6 gates baseline enrollment on `config.boot_mode == 2 && config.tpm_enroll` (`src/kernel/main/boot_interrupts.c:548-550`). Both fields are parsed verbatim from `boot.conf` on the ESP, so the gate proves ESP-write authority and nothing more: anyone who can add a line to a text file can make their own tampered boot the golden baseline that every later boot is measured against. §6's review recorded this as an accepted [H] finding, not a design choice.

Half the substrate exists, which the cohort item's own text ("owner: recovery-kind + console-input infra") did not know and which is why this is a section rather than a park. The bootloader publishes a recovery signal it DECIDED rather than read back: `BOOT_PATH_RECOVERY` (`include/kernel/boot_info.h:673`), `BOOT_REASON_RECOVERY_TRIGGER` (`boot_info.h:713`), `BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED` (`boot_info.h:804`) and `sticky_recovery_trigger` (`boot_info.h:1855`). Nothing gates enrollment on any of them today.

**The console half does NOT exist, and an earlier draft of this section wrongly claimed it did.** `boot_recovery.c` polls PS/2 scancodes (`kbd_poll_char`, lines 81-94), but `kbd_poll_begin` (lines 49-68) executes `cli` and masks the keyboard GSI with NO restore counterpart anywhere in the tree, and every caller runs `boot_recovery_act()` straight into reboot / power-off / halt. That lifecycle is TERMINAL by construction: it never has to give the machine back. A confirmation prompt must return and continue booting on all three outcomes, so the reusable primitive is a prerequisite this section builds, not substrate it inherits.

**Ownership split with §10, which owns an overlapping open item.** §10's `- [ ]` "Allow trusted baseline reset only from recovery mode with local confirmation" covers RESET / rotation UX. This section covers the ENROLLMENT authority question -- which signal is trusted to authorize a write at all. They share the console primitive below and nothing else. -> XREF: §10 (item: "Allow trusted baseline reset only from recovery mode with local confirmation").

- [ ] Build a RETURNING console-confirmation primitive: restores interrupt state and the prior keyboard-GSI mask on every exit path (confirm, refuse, timeout), so the caller keeps booting.
  - `boot_recovery.c` is refactored onto it rather than copied, so there is one lifecycle and the terminal callers keep their current behavior by choosing to halt, not by the primitive forcing it.
- [ ] Gate enrollment on the LOADER-decided recovery signal (`boot_path` / `boot_reason` / `boot_source_flags`), not `config.boot_mode`; `config.tpm_enroll` stays a second factor and is never sufficient alone.
- [ ] Require confirmation through that primitive before an enroll writes NV; a timeout or a wrong key REFUSES, fail-closed, with a distinct status rather than silence.
- [ ] Report the enrollment AUTHORITY alongside the verdict so an operator can distinguish a console-confirmed enroll from a config-only one; a config-only path must never be reported as trusted.
- [ ] Unit-test the gate as a pure predicate over (boot_path, boot_reason, source_flags, tpm_enroll, confirmed) so every refusal reason is asserted without a live TPM.
- [ ] Annotate §6's accepted [H] provenance stamp RESOLVED (do not delete it) once this ships. -> XREF: §6 (the "Accepted: [H] enrollment gate is boot.conf config" stamp).
- [ ] Commit: `"tpm: loader-validated + console-confirmed baseline enrollment"`

**Test checkpoint:** the gate predicate refuses a boot whose `config.boot_mode` says recovery but whose loader-decided `boot_path`/`boot_reason` do not, which is the exact spoof §6 accepted; it refuses on timeout and on a wrong key; and it admits only a loader-signalled recovery boot with `tpm_enroll` set AND an affirmative keypress. Refusal reasons are distinct values, not one boolean. The console primitive restores the interrupt state and the prior GSI mask on ALL THREE exit paths, asserted by reading the mask back after each -- a prompt that returned with IRQs still off would hang the boot it was supposed to continue. Scope: this section does NOT re-cover baseline CONTENT, rotation/reset UX (§10), or the NV mechanism (§6, §7, §17). Platforms: kernel unit suites cover the predicate and the save/restore; the live enroll cycle is QEMU-swtpm / bare-metal and operator-gated (no `swtpm` on the dev host).

---

## 16. Baseline Image Identity -- Carry Real Image Digests

> **Spawned-by:** §14 (split)

The baseline records what §6 could reach: per-bank PCR digests, Secure Boot state, a firmware-version hash, and a RESERVED `abi_manifest` field. Two things about that field are load-bearing and both were stated wrongly in this section's first draft. It is not an image hash -- `boot_proto_descriptor.sha256` is a BUILD-TIME hash of `build/boot-info-abi.kernel.json`, baked into `build/boot_proto_sha.h` by `tools/boot-info-manifest/gen-proto-sha-header.sh`, so it tracks the ABI manifest and stays put when the kernel image changes underneath it. And it is not populated: `tpm_baseline_snapshot()` sets `abi_manifest_present = 0` because no kernel-side accessor is exposed (`src/kernel/tpm_baseline.c:202-206`). The struct reserves the field and nothing fills it.

**This section does NOT compute the KERNEL hashes, and an earlier draft of it did -- which duplicated two open owners elsewhere in the corpus.** Kernel image measurement is `04-drivers-hardware/TODO-04 §7` (SHA-256 over `.text`/`.rodata`, PCR[10] extend, its own golden record). The bootloader-side hash of the loaded kernel ELF before ExitBootServices is `18-future-research/TODO-04 §2` (PCR 8). Neither carries a digest into §6's measured-boot baseline blob, and neither measures the LOADER.

So the ownership line is: **§16 owns the baseline schema, the mismatch attribution, AND the `BOOTX64.EFI` loader digest; only the two KERNEL digests are externally produced.** Stated this precisely because the previous wording ("takes its digests from those producers") contradicted the restored loader task and would have let an implementer or reviewer drop it as out of scope.

- [ ] Populate the reserved `abi_manifest` field first -- expose the kernel-side accessor, set `abi_manifest_present`, enroll and verify it. It is the digest the baseline already has room for and still does not carry.
- [ ] Consume the kernel-image digest from its owner, never recomputing it. -> XREF: [`04-drivers-hardware/TODO-04 §7`](../04-drivers-hardware/TODO-04-security-hardware.md) (item: "`kernel_measure()`").
- [ ] Consume the loader-side kernel digest from its owner. -> XREF: [`18-future-research/TODO-04 §2`](../18-future-research/TODO-04-secureboot-tpm.md) (item: "PCR 8 -- kernel hash extension").
- [ ] PRODUCE the `BOOTX64.EFI` digest here over the ON-DISK FILE BYTES, not the resident image. Both consumed producers measure the KERNEL, so nothing else identifies the bootloader binary.
  - The canonical measured bytes are the ESP file's contents, reopened through the loaded-image `DeviceHandle` + `FilePath`. `EFI_LOADED_IMAGE_PROTOCOL` hands back a RELOCATED in-memory `ImageBase`/`ImageSize` whose relocations are applied and whose data sections mutate as the loader runs, so hashing that range yields a different digest for the same binary across boots and destroys the attribution the checkpoint claims.
  - State the degraded behavior explicitly for the paths where the file cannot be reopened (NULL `DeviceHandle`, pure HTTP boot): report the digest ABSENT rather than substituting a resident-range hash, because a silently different measurement is worse than a missing one.
  - Restored after the deduplication pass dropped it: removing §16's two duplicate kernel-hash tasks took this one with them, which would have let the section complete with the baseline still unable to say which loader ran. Searched before restoring -- the `BOOTX64.EFI` digests elsewhere in `todo/` are build-time signing, USB-write verification and release manifests, none a runtime self-measurement.
- [ ] Carry whichever digests the producers publish in `boot_info`: reserved-region fields where they fit, otherwise a `BOOT_INFO_VERSION` bump applied to kernel header AND mirror together. -> XREF: [`TODO-01`](TODO-01-boot-protocol-abi-handoff.md).
- [ ] Extend `struct tpm_baseline` behind its version field AND design the NV index migration, which the version field alone does not buy. Enroll stores, verify compares, and a mismatch names WHICH image differs.
  - The blob is 412 bytes today and `tpm_nv_define_data` sizes `TPM_NV_INDEX_BASELINE` at exactly `sizeof(struct tpm_baseline)`, while `tpm_baseline_parse` rejects any blob whose `size` differs (`src/kernel/tpm_baseline.c:45`). Growing the struct therefore hits an index too small to hold it -- and define-if-present returns `DEFINED` rather than enlarging it -- so "older blob still validates" is false without a migration.
  - Specify a version-sized read path (probe the header via `NV_ReadPublic` / a size-prefix read rather than demanding the new `sizeof`), plus an authorized, crash-safe resize that preserves the generation counter and the §17 anti-rollback state across the undefine/redefine window.
  - Three 32-byte digests also leave the blob near `TPM_NV_MAX_DATA` 512, so the migration design is where the chunked NV path §14 deferred either becomes necessary or is explicitly ruled out.
  - Fixture the migration from a REAL 412-byte v1 index, not a synthesized one: the failure being tested is the on-NV size mismatch, which a hand-built blob of the new size cannot reproduce.
- [ ] Bound the cost the producers are asked to pay: a size ceiling on what is hashed, exactly one pass over already-resident bytes, and a pre-ExitBootServices timing milestone.
  - The kernel image is already ~14 MiB and the loader accepts up to 32 MiB, and the smoke matrix does not fail on a latency regression, so an unbounded "hash the image" instruction would silently lengthen every boot as the image grows.
- [ ] Commit: `"tpm: carry real image digests in the measured-boot baseline"`

**Test checkpoint:** a kernel rebuilt with no ABI-manifest change produces a DIFFERENT baseline kernel digest and the SAME `abi_manifest` digest, which is the distinction this section exists to make and which today's baseline cannot express at all because the manifest field is unpopulated. Rebuilding ONLY the bootloader changes the loader digest and leaves the kernel digest untouched, and the reverse holds -- that independence is what makes attribution real rather than a single "something changed" bit. A migration starting from a REAL 412-byte v1 NV index ends with the enlarged index readable, the generation counter and anti-rollback state preserved, and an interrupted migration recoverable rather than leaving no baseline at all. Hashing stays within its stated size ceiling and the pre-EBS milestone is recorded. A rebuilt-but-identical `BOOTX64.EFI` measured on two consecutive boots yields the SAME digest, and a boot path where the file cannot be reopened reports it ABSENT rather than falling back to a resident-range hash. Scope: this section owns the BASELINE SCHEMA, the attribution, and the LOADER digest; only the two KERNEL digests come from the producers XREF'd above, and the PCR-11 manifest extend and attestation report schema stay §9's. Platforms: kernel unit suites for the marshal/compare/version paths; the producers' own sections own their platform validation.

---

## 17. Write-Locked and Monotonic NV Indexes (Anti-Rollback Trust Anchor)

> **Spawned-by:** §14 (split)

Two cohort items, one mechanism, which is why they are one section. §6's baseline rotation rejects a lower generation in SOFTWARE (`tpm_baseline_rotation_ok`), so an attacker who can write the NV index simply overwrites the generation along with the blob. And TODO-21 §8's per-slot A/B floor has no authenticated store at all: its design note states that CRC metadata is a cached hint and never the floor authority, and that selection enforcement ships WITH the store. That is why its `ROLLBACK_BLOCKED` enforcement is a Critical deferral pointed here rather than shipped code.

§7 modelled only the attributes it needed. `include/kernel/tpm_nv.h:56-64` carries OWNER/AUTH/POLICY read and write, `NO_DA`, and the read-only `WRITTEN` status; there is no `WRITE_LOCKED`, no `WRITEDEFINE`, no `WRITE_STCLEAR`, and no NV-type counter. Neither `TPM2_NV_WriteLock` nor `TPM2_NV_Increment` has a command code (`src/kernel/tpm_nv.c:647-760`).

**One mechanism, TWO indexes -- an earlier draft of this section said "the monotonic floor index", singular, and that was a design error.** The two consumers hold values with different meanings and different advance rules. The baseline generation advances by exactly one per authorized rotation and must stay crash-consistently bound to the blob it describes, so a counter that advanced without the blob landing is a corrupt pairing that has to be detectable. The A/B floor is a security VERSION that advances only after a verified mark-good and may jump several versions at once. Sharing the primitives is right; sharing one counter would make each consumer's invariant unenforceable.

- [ ] Model the missing TPMA_NV attributes and the NV index TYPE field, so a define can request a counter or a write-lockable index rather than only an ordinary data index.
- [ ] Close the index LIFECYCLE hole, without which none of this is a trust anchor: an attacker who can undefine and recreate the index resets it to zero and restores an old baseline or floor.
  - Counters and write locks only stop an ordinary write from lowering a LIVE index. `tpm_nv.c:15-19` states define/undefine use owner auth with an empty password session precisely because owner auth bypasses PCR policy -- so the OS path that legitimately provisions the index is also the path that can destroy it.
  - Prescribe: reset authorization unreachable from the ordinary OS path (protected owner/platform auth or a policy-delete lifecycle), verification of the index Name and public attributes on EVERY use rather than trusting the handle, and a stated recovery for a legitimate TPM clear or replacement.
  - Test the attack directly: an undefine/redefine cycle followed by an attempted rollback must be refused, not silently accepted as a fresh install.
- [ ] Add `TPM2_NV_Increment` and `TPM2_NV_WriteLock` builders and parsers beside the existing NV ops, classified through the same format-first `tpm_nv_classify_rc` so a locked index never wedges.
- [ ] Define TWO separate indexes with separate contracts: a baseline generation counter and an A/B security-version floor. Never one shared value.
- [ ] Specify the baseline counter's crash-consistent ordering against the blob write, and how a counter-ahead-of-blob pairing is detected and resolved rather than silently trusted.
- [ ] Represent the A/B floor as an authenticated DATA record holding the security version, bound to a +1 counter transaction -- never the counter value itself -- with crash recovery for counter/record skew.
  - Two earlier drafts of this item were wrong in opposite directions. "Increment-only over an arbitrary jump" contradicted the only primitive available. "One increment per release ordinal" then made the counter BE the version, which a TPM counter cannot represent: it starts at zero and moves by one, so a fresh install at ordinal 500 needs 500 transactions, and rejecting that delta leaves the stored counter numerically unrelated to the `rollback_index` the A/B ABI actually compares.
  - The counter supplies UPDATE SEQUENCE and the record supplies the VALUE -- and the pairing alone is NOT yet anti-rollback, which the previous wording ("counter supplies monotonicity") got wrong. A holder of record-write authority can increment once and store a LOWER version: the counter advanced, the record matches it, there is no skew, and the index Name and attributes are still valid, so every cooperative check passes and the bootloader accepts a vulnerable slot.
  - So the AUTHORIZATION carries the monotonicity, not the arithmetic: record-write must be unavailable to ordinary or rolled-back OS code, authorizing the exact (counter transition, new version) pair -- a signed policy or cpHash-bound session. Store the counter generation INSIDE the record and verify the whole transition on read.
  - Test the bypass directly: a lower version paired with a freshly incremented counter must be REFUSED, not accepted as the newest record. A read finding a HIGHER stored value than the caller expects likewise stays a REFUSAL.
  - This authorization design is the one part of §17 that warrants its own Codex design review before implementation; it is a TPM policy-construction problem, and getting it wrong reproduces exactly the false security TODO-21 §8 refused to ship.
- [ ] Bound the boot-path cost: ONE cached floor read per boot, no unbounded retries, and one cumulative wall-clock budget across every NV command in an operation, with the fail/degrade policy stated for budget expiry.
  - A slow-but-responsive TPM can spend multi-second transport timeouts per command without ever timing out, against a whole-boot target measured in seconds. Bounding this after implementation means discovering it on the slowest real machine.
- [ ] Consumer 1 -- baseline rotation gets the SAME authorization boundary as the floor record, not merely a counter to consult. -> XREF: §6 (rotation item).
  - Pairing the blob with a counter is not enough, and an earlier draft of this item stopped there while fixing the identical hole for Consumer 2. The baseline index is owner-writable, so an attacker can take an OLD vulnerable baseline, stamp it with the CURRENT counter generation, recompute the CRC and write it back without touching the counter at all. Nothing is skewed, the CRC validates, and verify accepts a vulnerable measured state as golden.
  - So baseline-record writes must also be unreachable from ordinary or rolled-back OS code, authorizing the exact counter transition together with a DIGEST OF THE WHOLE new record under the §15 enrollment/rotation authority. A CRC is an integrity check against corruption and was never an authenticity check.
  - Test the relabeling directly: old baseline content, current counter generation, valid CRC, must be REFUSED.
- [ ] Consumer 2 -- the floor READ must be reachable from the BOOTLOADER, not only the kernel. -> XREF: [`TODO-21 §8`](TODO-21-ab-boot-rollback.md) (item: "Store `rollback_floor` in an authenticated monotonic / write-locked TPM-NV index").
  - This is the constraint that decides whether §17 can satisfy TODO-21 §8 at all, and an earlier draft missed it by publishing a kernel API alone. `select_active_slot()` runs at `src/boot/uefi/bootx64.c:16140`, before the kernel is loaded, so a below-floor slot is already executing by the time any kernel-side floor read happens. The existing `tpm_nv_*` wrappers ride the kernel's Phase-1 `tpm2_submit()` and have no bootloader counterpart.
  - So: a typed `EFI_TCG2` `SubmitCommand` adapter with marshaling SHARED with the kernel path (one definition, no producer/consumer drift), read before selection; the ADVANCE stays kernel-side after verified mark-good, where it belongs.
- [ ] Unit-test the builders, the classifier against locked and counter response codes, the increment-only invariant, and the counter-ahead-of-blob recovery path through the fake-TIS seam.
- [ ] Commit: `"tpm: write-locked and monotonic NV indexes"`

**Test checkpoint:** a define requesting a counter index emits the correct TPMA_NV bits and NV type; an increment past a write-lock returns the locked status rather than wedging the transport; each floor helper refuses an advance that would lower its own stored value and refuses a read whose expectation is below what is stored. A baseline counter incremented with no blob write behind it is DETECTED on the next verify rather than trusted. A simulated slow TPM exhausts the cumulative budget and takes the stated degrade path instead of extending the boot. A first install at a high security version, and an upgrade skipping several releases, both work with ONE counter transaction rather than one per version step. An undefine/redefine cycle followed by a rollback attempt is REFUSED rather than read as a fresh install. A below-floor slot is refused BEFORE the kernel is loaded, not after. Scope: this section owns the NV MECHANISM and publishes both APIs. It does NOT re-cover A/B selection enforcement, which stays TODO-21 §8, nor baseline content, which stays §6. Platforms: fake-TIS unit suites are the whole automatable surface; the live swtpm round trip and real-fTPM write-lock semantics are operator-gated (no `swtpm` on the dev host).

---

## 18. Atomic Boot-Integrity Report Publication

> **Spawned-by:** §14 (split)

`tpm_integrity_init()` sets `pcrs[0..7].status = BOOT_INTEGRITY_NO_CRYPTO` in Phase 0 and never revisits them (`src/kernel/tpm.c:684`, 730-736). The three post-init setters -- `tpm_integrity_set_rng_available`, `tpm_integrity_set_replay_verdict` and `tpm_integrity_set_overall_status` (`tpm.c:795`, 800, 810) -- touch scalars only. So after a Phase-1 VERIFIED verdict is published the per-PCR detail still reads NO_CRYPTO and the report contradicts itself in the one place a reader would look to see WHICH measurement was trusted. The array is also `pcrs[8]`, PCR 0-7 (`include/kernel/tpm.h:170`), while the measured baseline set is {0-7,11}: PCR 11 has no slot to be reported in at all.

Publication is field-by-field into one static struct with no lock and no snapshot swap. That is safe TODAY only because every writer runs single-threaded on the BSP before the APs are up, which is an accident of init ordering rather than a stated contract, while the readers are already UI and VPD consumers.

- [ ] Add a PCR 11 slot and size the array from the baseline mask rather than the literal 8, so the reported set cannot drift from `tpm_pcr_baseline_pcrs()`.
- [ ] Refresh every per-PCR status when the baseline verdict is published, so the overall verdict and the per-PCR detail can never disagree.
- [ ] Build the report OFF-lock and publish by release-store swap to an IMMUTABLE snapshot, with a reader protocol that makes ACQUISITION itself safe: a lock spanning load-plus-pin, hazard publication then revalidation, or an RCU grace period.
  - A bare refcount is explicitly NOT sufficient and an earlier draft allowed it: acquire-loading the pointer and then incrementing its refcount leaves a load-to-pin window in which the writer can swap and reclaim, so the reader pins a buffer that is already gone.
  - `tpm_integrity_report()` hands out an unowned pointer today, so every naked-pointer caller migrates, or the accessor becomes a synchronized copy-out.
- [ ] Serialize the WRITERS too: base-load, mutate and publish under one short writer lock, or a CAS/rebase loop that preserves fields an intervening writer set. Safe readers do not prevent lost updates.
  - The setters mutate independent fields, so if each rebuilds off-lock from the snapshot it read, a replay-tamper writer publishing MISMATCH can be overwritten by a slower baseline writer publishing VERIFIED that never saw it. Every published snapshot stays internally coherent, so an old-or-new reader test passes while the report says VERIFIED on a tampered boot.
  - This is a live invariant, not a hypothetical: `tpm_integrity_set_overall_status()` already pins MISMATCH once a TAMPER verdict landed (`src/kernel/tpm.c:817`). An off-lock rebuild would silently discard that guard, so preserving it is part of the acceptance, not a nicety.
  - Two alternatives were considered and both rejected in writing so nobody re-derives them. An unsynchronized bounded copy is not publication at all: `tpm_integrity_report()` returns a pointer to a multi-field static struct, so a field-by-field copy straddles a publication and sees a new verdict beside old per-PCR detail -- the exact contradiction this section exists to remove.
  - A seqlock was the second, and it cannot meet this section's own checkpoint without more machinery than the swap costs: `seqlock_read_begin` spins while the sequence is odd, so a stuck-odd writer stalls a UI/VPD reader forever, and bounding the OUTER retries does not bound that spin. Making it work would need a copy-out accessor whose validation spans the whole copy, a defined result on attempt exhaustion, and migration of every existing caller off the pointer accessor.
  - Whichever form ships, no blocking or TPM work may sit inside the writer's publication region.
- [ ] Correct the stale contract comment at `include/kernel/tpm.h:184-192`, which still describes the superseded Phase-0 "read golden values" design that §6 moved to Phase 1.
- [ ] Annotate §6's accepted [H] per-PCR publication stamp RESOLVED (do not delete it) once this ships. -> XREF: §6 (the "Accepted: [H] baseline verify leaves stale per-PCR `NO_CRYPTO`" stamp).
- [ ] Commit: `"tpm: atomic boot-integrity report publication"`

**Test checkpoint:** after a published VERIFIED verdict no slot still reads `NO_CRYPTO`, and PCR 11 is present in the reported set; after a MISMATCH the per-PCR detail names which PCR differed rather than leaving the reader to guess. A reader paused between the pointer LOAD and its pin, while the writer publishes and tries to reclaim, still ends with a coherent report -- that acquisition window is the one a held-reference test starts too late to cover. A replay-tamper update racing a baseline-verified update leaves MISMATCH authoritative, never VERIFIED: the existing TAMPER-pins-MISMATCH guard must survive the new publication path. A reader held across several publications likewise sees a coherent report and no reuse underneath it; under repeated concurrent publication readers observe old-or-new, never a mix, and always complete -- a test that only asserts old-or-new passes against a writer that stalls readers forever. Scope: this section owns the report STRUCT and its publication only; the verdicts themselves stay with §4 (replay), §5 (Secure Boot) and §6 (baseline). Platforms: kernel unit suites; no TPM required.

---

## OS Comparison

| ⭐  | Feature                                | Windows                   | Linux                     | Impossible OS                                                                 |
| --- | -------------------------------------- | ------------------------- | ------------------------- | ----------------------------------------------------------------------------- |
| 💎  | TPM2 command transport (TIS/CRB)       | tpm.sys TIS/CRB           | tpm_tis/tpm_crb drivers   | ✅ §2 burst-chunked TIS + CRB                                                 |
| 💎  | Secure Boot PCR integration            | Measured Boot             | IMA/TPM tools             | ⚠️ §5 structural SB var reconcile                                             |
| 💎  | PCR replay                             | internal/Defender         | tpm2-tools                | ✅ §4 SHA-1/256/384/512 replay + tamper verify                                |
| 💎  | TPM NV index storage (PCR-sealed)      | TBS NV / BitLocker        | tpm2_nvdefine + kernel RM | ✅ §7 NV CRUD + PolicyPCR-sealed baseline index                               |
| 💎  | Measured-boot baseline (enroll/verify) | Measured Boot baseline    | IMA + systemd-pcrlock     | ⚠️ §6 recovery-gated enroll + Phase-1 verify + generation rotation            |
| 💎  | Sealed secrets                         | BitLocker                 | systemd-cryptenroll       | ✅ §8 PCR-7 KEYEDHASH seal (PolicyPCR) + FDE/CI hooks + recovery handoff      |
| ⭐  | Boot attestation report (JSON)         | Device Health Attestation | Keylime AK quote JSON     | ⚠️ §9 signed report to `X:\Diag\attestation.json`; query API + PCR-11 pending |
| 💎  | Remote attestation (TPM2 Quote)        | Device Health Attestation | Keylime AK quote          | ✅ §13 EK->AK provision + TPM2_Quote + nonce anti-replay                      |
| 💎  | PCR allocation policy                  | PCR7+11 BitLocker seal    | systemd-pcrlock CEL       | ✅ §12 event-centric table + derived masks                                    |

## Unit Tests

- [x] Shipped with §2 (2 suites, TEST_CAT_SECURITY, `test_tpm_transport.c`): transport marshaling + fake-TIS submit (chunking, protocol violations, sticky fail, reentrancy).
- [ ] `test_tpm_event_log_tpm12_fixture`
- [ ] `test_tpm_event_log_tpm20_fixture`
- [ ] `test_tpm_pcr_replay_sha256`
- [ ] `test_tpm_integrity_mismatch_report`
- [ ] `test_tpm_pcr_allocation_manifest_layer` (§12: manifest-only change attributed to kernel-ABI layer, not blanket mismatch)
- [ ] `test_tpm_quote_verify_nonce` (§13: swtpm quote verifies under AK pubkey + carries nonce; stale-nonce replay rejected)

## Verification

- [ ] QEMU OVMF + swtpm
- [ ] Secure Boot enabled OVMF
- [ ] Bare metal with TPM 2.0
- [ ] Bare metal without TPM degrades cleanly
