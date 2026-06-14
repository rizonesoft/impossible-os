---
schema_version: 1
id: tpm-measured-boot-attestation
domain: 01-boot-platform
status: active
title: "TODO-13 -- TPM Measured Boot, PCR Replay & Attestation"
---

# TODO-13 -- TPM Measured Boot, PCR Replay & Attestation

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

| ⭐ | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| 💎 | 1 | Harden TCG event-log parser | -- | [/] |
| 💎 | 2 | TPM2 command transport | §1 (ordering-only; transport does not consume the parser) | [x] |
| 💎 | 3 | PCR read API | §2 | [ ] |
| 💎 | 4 | PCR replay engine | §1, ../02-kernel-core/TODO-03 §3 | [ ] |
| 💎 | 5 | Secure Boot variable measurement reconciliation | §4, TODO-02 §3 | [ ] |
| 💎 | 6 | Baseline enrollment and storage | §3, §4 | [ ] |
| 💎 | 7 | TPM NV index support | §2, §6 | [ ] |
| ⭐ | 8 | Sealed-secret boot policy hooks | §7 | [ ] |
| 💎 | 9 | Attestation report export | §3-§6, §12, §13 | [ ] |
| ⭐ | 10 | Recovery and mismatch UX | §6, TODO-22 | [ ] |
| 💎 | 11 | TPM tests and event-log fixtures | §1-§10, §12, §13 | [ ] |
| 💎 | 12 | PCR allocation table and policy masks | §4, §6, §8 | [ ] |
| 💎 | 13 | Attestation key provisioning and TPM2 quote | §3, §7, §12 | [ ] |

## 1. Harden TCG Event-Log Parser

- [x] Parse TPM 1.2 + TPM 2.0 crypto-agile logs with unaligned-safe byte loads (`tpm_le16`/`tpm_le32`); pure `tpm_evlog_parse()` in `tpm.c`, no pointer-cast reads.
- [x] Preserve per-event metadata (PCR index, type, digest count, primary digest off/len/alg, payload off/size) in `struct tpm_event` (offset-based into the retained log, no copy).
- [x] Reject truncation/corruption with `tpm_evlog_status_t` + exact `fail_offset`; partial-tail, cap-overflow, and unsupported-alg are distinct; event count stays 0 on any non-OK status.
- [x] Export `X:\Diag\tpm-events.json` as a TCG CEL-JSON subset via a streaming writer (`tpm_evlog_export_cel`, one bounded `vfs_write` per event), hooked post-mount in `boot_desktop.c` (parse runs Phase 0, before the filesystem).
- [x] Bootloader copies the EXACT log (`tpm_compute_log_size`, not the `+256` estimate) or degrades `BOOT_CAP_TPM_EVENT_LOG`; retention via the legacy `tpm_event_log` boot_reserved path (no descriptor: would double-reserve + fatal).
- [ ] Bound the bootloader TPM read/copy by the UEFI memory-map extent of `log_location` (vs firmware reporting `!truncated` past its buffer); today bounded only by `TPM_EVENT_LOG_MAX`. (Codex adversarial [H].)
- [x] Commit: `"tpm: harden measured boot event log parser"`

**Test checkpoint:** Kernel unit fixtures parse a TPM 1.2 SHA-1 log and a TPM 2.0 crypto-agile (multi-bank) log with zero unaligned reads; a truncated log is rejected with an explicit byte offset + status code (not a silent stop); `X:\Diag\tpm-events.json` lists per-event PCR index / type / digest count; the page-aligned bootloader copy is retained by the legacy `tpm_event_log` boot_reserved reservation (no double-reserve fatal at boot). Platforms: kernel unit tests (fixtures) + QEMU swtpm KVM smoke; bare metal (test laptop fTPM).

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 9 tpm-evlog suites in `test_tpm_event_log.c`, 0 failures

> **Notes:**
> - **What shipped:** pure `tpm_evlog_parse()` (unaligned-safe byte loads, per-event `struct tpm_event` metadata, structured status + fail offset) + streaming CEL-JSON exporter; bootloader `tpm_compute_log_size()` exact-size copy or cap-degrade.
> - **How it integrates:** parse runs Phase 0 from `tpm_init` (which wraps the pure walker); CEL export runs post-mount from `boot_desktop.c`; the log is retained by the legacy `tpm_event_log` boot_reserved reservation.
> - **Downstream:** §4 PCR replay + §9 attestation/CEL consume the preserved metadata + offsets. Codex 4x (design + adversarial + re-adversarial) adoptions in the commit message.
> - **Canonical doc:** `include/kernel/tpm.h` (parse contract); TCG CEL-JSON for the export format.
> - **Scope boundary:** the UEFI mem-map-extent read bound is a deferred `[ ]` item in this section; retention is legacy-fields-only (no typed RESERVED descriptor).

> **Verified:** 2026-06-14 | commit `53e24f76` (impl) + review (perf coalescing + stamps) | 5/6 items | build OK | 394 kernel tests PASS (9 new tpm-evlog) + smoke PASS (KVM 2.650s)
> **Deferred:** [H] bootloader TPM read/copy not bounded by the firmware UEFI mem-map extent (reads from `log_last_entry` capped only by `TPM_EVENT_LOG_MAX`) -> XREF: 01-boot-platform/TODO-13 §1 (item: "Bound the bootloader TPM read/copy by the UEFI memory-map extent of `log_location`")
> **Quality reviewed:** 2026-06-14 | Codex 5x (design, adversarial, re-adversarial, consistency, perf) | 1C+1H+3M fixed, 1H deferred | scope: kernel-code-quality + boot-code-quality

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
> **Deferred:** [L] CRB submit path lacks a fake-buffer unit seam (validated live via swtpm checkpoint) -> XREF: 01-boot-platform/TODO-13 §11 (item: "Fake-CRB buffer seam + unit suite for crb_submit()" at line 102)
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

## 4. PCR Replay Engine

- [ ] Replay PCR extend operations from parsed event log for every active bank.
- [ ] Compare replayed values against hardware PCR reads.
- [ ] Mark event-log tampering separately from baseline mismatch.
- [ ] Include exact first mismatch in diagnostics.
- [ ] Commit: `"tpm: replay measured boot PCRs"`

**Test checkpoint:** On a clean boot, replayed PCR values (extended from the parsed event log, every active bank) equal the hardware PCR reads from §3; a known-vector unit test replays a fixed log to expected digests; injecting one tampered event-log entry flags `event-log tamper` distinctly from `baseline mismatch` and diagnostics name the exact first-mismatch PCR index + offset. Platforms: kernel known-vector unit tests + QEMU swtpm KVM; bare metal.

## 5. Secure Boot Variable Measurement Reconciliation

- [ ] Decode EV_EFI_VARIABLE_* events for PK, KEK, db, dbx, SecureBoot, SetupMode.
- [ ] Compare measured Secure Boot state with UEFI variables read by TODO-02.
- [ ] Flag impossible combinations such as Secure Boot active but PCR7 missing policy events.
- [ ] Feed results to `boot_integrity_report`.
- [ ] Commit: `"tpm: reconcile Secure Boot measurements"`

**Test checkpoint:** `EV_EFI_VARIABLE_*` events for PK / KEK / db / dbx / SecureBoot / SetupMode decode to the same values the UEFI variables report (read via TODO-02); an injected "SecureBoot active but PCR7 has no policy events" combination is flagged as impossible; the reconciliation result lands in `boot_integrity_report`. Platforms: QEMU swtpm + OVMF Secure Boot KVM; bare metal.

## 6. Baseline Enrollment and Storage

- [ ] Add first-boot enrollment mode gated by physical-console confirmation.
- [ ] Store golden PCR values in UEFI authenticated variable or TPM NV index.
- [ ] Include bootloader hash, kernel hash, Secure Boot state, and firmware version metadata.
- [ ] Support baseline rotation after trusted updates.
- [ ] Commit: `"tpm: enroll measured boot baseline"`

**Test checkpoint:** First-boot enrollment (gated by physical-console confirmation) stores golden PCRs + bootloader hash + kernel hash + Secure Boot state + firmware version; a second boot reads the stored baseline and reports `verified`; a simulated trusted update triggers baseline rotation (old baseline retired, new one stored), not a mismatch halt. Platforms: QEMU swtpm KVM (enrollment + re-boot); bare metal.

## 7. TPM NV Index Support

- [ ] Implement NV read/write/define/undefine for one OS-owned index.
- [ ] Protect baseline with PCR policy where TPM supports it.
- [ ] Handle no-space and locked-NV degraded states.
- [ ] Add migration path from UEFI variable storage to TPM NV.
- [ ] Commit: `"tpm: measured boot NV index storage"`

**Test checkpoint:** Define / read / write / undefine round-trip on one OS-owned NV index against QEMU swtpm; where the TPM supports it the index is PCR-policy-protected (a read under the wrong PCR state is denied); no-space and locked-NV return explicit degraded states (not a wedge); the UEFI-variable -> TPM-NV migration path moves an existing baseline without loss. Platforms: QEMU swtpm KVM; bare metal (test laptop fTPM).

## 8. Sealed-Secret Boot Policy Hooks

- [ ] Add API for sealing and unsealing small secrets to PCR policy.
- [ ] Provide FDE key-unlock hook for future storage encryption.
- [ ] Provide code-integrity policy seal hook.
- [ ] Ensure recovery can prompt when unseal fails.
- [ ] Commit: `"tpm: sealed boot policy hooks"`

**Test checkpoint:** Seal a small secret to a PCR policy, then unseal succeeds while PCRs match and is denied (with a recovery prompt) after a PCR changes; the FDE key-unlock hook and the code-integrity seal hook are present + callable (stub consumers OK until storage-encryption / CI policy land). Platforms: QEMU swtpm KVM (seal + PCR-change deny); bare metal.

## 9. Attestation Report Export

- [ ] Build the TPM-rooted `boot_attestation_report_t`: PCRs, event digest, Secure Boot state, verifier nonce, and the §13 `TPM2_Quote` blob + AK public + EK-cert chain (TPM-signed quote, not a software signature).
- [ ] Export to `X:\Diag\attestation.json`.
- [ ] Add native query API for user-mode system settings.
- [ ] Add remote-attestation placeholder for platform services.
- [ ] Extend a TPM PCR (target PCR 11, vendor-policy-extensible) with the kernel `.bootproto` manifest sha256 from the bootloader pre-jump path, BEFORE the bootloader transfers control to the kernel. Concretely: in `src/boot/uefi/bootx64.c` after `bootproto_verify_or_reset()` succeeds, walk the EFI_TCG2_PROTOCOL via `LocateProtocol(EFI_TCG2_PROTOCOL_GUID, ...)` and call `Tcg2Protocol->HashLogExtendEvent` with the 32-byte sha256 from the descriptor + a stable event-log entry name (e.g. `IMPOSSIBLE_OS_KERNEL_ABI_MANIFEST`). On success, the bootloader sets `boot_info.caps_present |= BOOT_CAP_MANIFEST_PCR_BOUND` (bit reserved by TODO-01 §11). Failure to extend (no TPM, locality denied, command failure) MUST leave the bit clear and continue boot -- this is a measurement, not a gate. Replay path: §4 PCR replay must include the manifest extend event so a baseline mismatch flags a different kernel-image-vs-manifest pairing, not just a different kernel image. -> XREF: [`01-boot-platform/TODO-01 §11`](TODO-01-boot-protocol-abi-handoff.md#11-capability-negotiation-and-degraded-feature-flags) (item: "Forward-reserve `BOOT_CAP_MANIFEST_PCR_BOUND` (1u<<10) bit").
- [ ] Include the bootloader-to-kernel handoff triple in `boot_attestation_report_t`: ABI manifest sha256 (from `boot_proto_descriptor.sha256`), capability words (`caps_required` / `caps_present` / `caps_degraded`), boot-path provenance (`boot_path` / `boot_reason` / `boot_source_flags` / `boot_fallback_depth`), and bootloader build identity (when TODO-01 §20 ships). Today downstream attestation consumers re-read `g_boot_info` directly which couples them to the live struct layout; the report API decouples by snapshotting the triple at attestation-build time. -> XREF: [`01-boot-platform/TODO-01 §11`](TODO-01-boot-protocol-abi-handoff.md#11-capability-negotiation-and-degraded-feature-flags) caps fields, [`§12`](TODO-01-boot-protocol-abi-handoff.md#12-common-boot-path-provenance-and-decision-record) decision record, [`§17`](TODO-01-boot-protocol-abi-handoff.md#17-bootloader-pre-jump-abi-mismatch-screen) `.bootproto` sha, [`§20`](TODO-01-boot-protocol-abi-handoff.md#20-bootloader-build-identity) bootloader build identity.
- [ ] Reserve forward-compat slots for DRTM (Dynamic Root of Trust for Measurement) fields: `drtm_entry_pcr` (typically PCR 17), `drtm_acm_status`, `drtm_measurement_type` (Intel TXT SENTER vs AMD SKINIT vs none). The native `BOOTX64.EFI` does not issue SENTER/SKINIT today, so these fields are zero/absent; the attestation report format must accept and round-trip them so a future TrenchBoot-style Secure Launch adapter (via TODO-08 alternate protocols, or firmware-initiated DRTM on supported platforms) can populate them without a schema break. -> XREF: [`01-boot-platform/TODO-01 §8`](TODO-01-boot-protocol-abi-handoff.md#8-boot-protocol-documentation-and-schema-changelog) owns the `boot_info` handoff-ABI side of any DRTM fields.
- [ ] Commit: `"tpm: export boot attestation report"`

**Test checkpoint:** A signed `boot_attestation_report_t` carries PCRs + event digest + Secure Boot state + nonce and exports to `X:\Diag\attestation.json`; the native query API returns the same snapshot to user-mode; the bootloader manifest PCR-extend (PCR 11) ran pre-jump and set `BOOT_CAP_MANIFEST_PCR_BOUND` (clear + boot-continue when no TPM); the report snapshots the handoff triple (manifest sha256, caps words, boot-path provenance) and round-trips the zeroed DRTM forward-compat slots. Platforms: QEMU swtpm KVM; bare metal.

## 10. Recovery and Mismatch UX

- [ ] Add VPD/boot diagnostics status for verified, no TPM, no baseline, mismatch, and event-log tamper.
- [ ] In recovery, explain whether firmware, bootloader, kernel, Secure Boot db, or baseline changed.
- [ ] Allow trusted baseline reset only from recovery mode with local confirmation.
- [ ] Integrate with A/B rollback if kernel measurement changed unexpectedly.
- [ ] Commit: `"recovery: measured boot mismatch UX"`

**Test checkpoint:** VPD / boot diagnostics show distinct status for verified / no-TPM / no-baseline / mismatch / event-log-tamper; recovery mode names which layer changed (firmware vs bootloader vs kernel vs Secure Boot db vs baseline); a trusted baseline reset is accepted ONLY from recovery mode with local confirmation; an unexpected kernel-measurement change triggers A/B rollback integration (TODO-22). Platforms: manual recovery-mode walkthrough (QEMU swtpm + injected mismatch); bare metal.

## 11. TPM Tests and Event-Log Fixtures

- [ ] Add event-log parser fixtures for TPM 1.2 and TPM 2.0.
- [ ] Add PCR replay known-vector tests.
- [ ] Add QEMU swtpm test path.
- [ ] Fake-CRB buffer seam + unit suite for `crb_submit()` in `src/kernel/tpm_transport.c` (cmdReady/START/goIdle, shared cmd/rsp buffer, oversized response) -- the §2 io seam covers TIS only; CRB is validated live via swtpm.
- [ ] Add degraded tests for no TPM, truncated log, and inactive PCR banks.
- [ ] Commit: `"test: TPM measured boot coverage"`

**Test checkpoint:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) runs the new TPM suites with 0 failures: TPM 1.2 + 2.0 event-log parser fixtures, PCR-replay known-vector tests, the fake-CRB buffer seam suite for `crb_submit()`, and degraded cases (no TPM, truncated log, inactive banks); the QEMU swtpm path boots green on KVM. Platforms: kernel unit tests + QEMU swtpm KVM; bare metal (test laptop fTPM).

---

## 12. PCR Allocation and Policy Mask Table

A single source of truth for which boot events extend which PCR, and which PCR masks the sealed-secret / baseline / quote consumers use. Without it the §9 manifest-extend into PCR 11 collides with established conventions (BitLocker seals to PCR7+PCR11; systemd-stub/pcrlock use PCR 11 for UKI kernel-boot; Keylime may exclude PCR 11 from runtime masks), so a benign kernel-ABI-manifest or event-order change could trigger FDE unseal failure, a recovery prompt, or a false attestation mismatch instead of being reported as the intended layer.

- [ ] Author `docs/boot/pcr-allocation.md`: the Impossible OS PCR ownership table (firmware/Secure Boot PCR 0-7, kernel-ABI-manifest + UKI kernel-boot on PCR 11, OS-owned indices) with the exact event name extended into each.
- [ ] Make the §9 `.bootproto` manifest extend a distinct *named* TCG event inside PCR 11 (shared with the UKI kernel-boot convention, NOT a separate PCR), with a documented extend ordering so replay (§4) is deterministic.
- [ ] Define the sealed-secret default PCR mask (§8) and document whether it includes PCR 11 (BitLocker's PCR7+PCR11 default re-seals on any PCR-11 change -- state the trade-off vs a PCR7-only mask).
- [ ] Define the quote PCR mask (§13) and the baseline PCR mask (§6) so a manifest-only change is attributed to the kernel-ABI layer, not a blanket mismatch.
- [ ] Expose `tpm_pcr_owner(index)` / a static allocation table consulted by §6, §8, and §13 so masks are derived from one place, not duplicated per consumer.
- [ ] Commit: `"tpm: PCR allocation table and policy masks"`

**Test checkpoint:** A manifest-only change (rebuild kernel with a new `.bootproto` sha) is reported as the kernel-ABI layer in diagnostics and does NOT spuriously fail FDE unseal (§8) or remote-attestation verification (§13); the allocation table in `docs/boot/pcr-allocation.md` matches the runtime extends (a unit test asserts each owned PCR's event name). Platforms: kernel unit tests + QEMU swtpm KVM; bare metal.

---

## 13. Attestation Key Provisioning and TPM2 Quote

The TPM-rooted signing mechanism §9's report needs. A software-signed JSON cannot prove the PCRs came from this machine's TPM; both Win11 Device Health Attestation and Linux Keylime rely on a TPM-resident Attestation Key (AK) bound to the Endorsement Key, and `TPM2_Quote` (the TPM signs the PCR digest + a verifier nonce). This section provisions the AK and produces verifiable quotes; §9 consumes them. -> XREF: supersedes the research-spike `18-future-research/TODO-04 §2` (item: "TPM Quote (remote attestation)") for the active implementation.

- [ ] Retrieve the EK certificate from the standard NV indices (RSA `0x01c00002`, ECC `0x01c0000a`) and expose the EK-cert chain to verifiers; degrade cleanly when absent (vTPM/fTPM without a cert).
- [ ] Provision the AK: `TPM2_CreatePrimary` EK under the endorsement hierarchy, `TPM2_Create` + `TPM2_Load` a restricted signing AK, persist it via `TPM2_EvictControl` to a stable handle (or recreate deterministically each boot).
- [ ] Support `TPM2_MakeCredential` / `TPM2_ActivateCredential` so a remote verifier can confirm the AK is bound to a TPM whose EK cert it trusts (credential-activation challenge).
- [ ] Implement `tpm2_quote(pcr_mask, nonce, sig_out, sig_len)` via `TPM2_CC_Quote`: sign the selected-PCR digest + verifier nonce with the AK (ECDSA or RSASSA); return the `TPMS_ATTEST` + signature.
- [ ] Anti-replay: the quote MUST embed the verifier-supplied nonce; reject quote requests with no fresh nonce; surface the TPM `clockInfo` (resetCount/restartCount) in the attest so the verifier can detect stale/replayed quotes.
- [ ] Wire the quote blob + AK public + EK-cert chain into §9's `boot_attestation_report_t`, replacing the software-"signed" placeholder so the exported report is TPM-rooted and remote-verifiable.
- [ ] Degraded: no TPM / no EK cert / AK provisioning failure -> report `attestation_unavailable` and continue boot; NEVER gate boot on a quote.
- [ ] Commit: `"tpm: attestation key provisioning and TPM2 quote"`

**Test checkpoint:** Against QEMU swtpm, `tpm2_quote(mask, nonce, ...)` returns a `TPMS_ATTEST` + signature that verifies under the AK public key and carries the supplied nonce; a credential-activation round-trip (`MakeCredential`/`ActivateCredential`) succeeds; a replayed quote (stale nonce) is rejected; with no TPM the path reports `attestation_unavailable` and boot continues. Platforms: QEMU swtpm KVM (quote + verify); bare metal (test laptop fTPM, real EK cert).

---

## OS Comparison

| ⭐ | Feature | Windows | Linux | Impossible OS |
| --- | --- | --- | --- | --- |
| 💎 | TPM2 command transport (TIS/CRB) | tpm.sys TIS/CRB | tpm_tis/tpm_crb drivers | ✅ §2 burst-chunked TIS + CRB |
| 💎 | Secure Boot PCR integration | Measured Boot | IMA/TPM tools | TODO-13 |
| 💎 | PCR replay | internal/Defender | tpm2-tools | TODO-13 §4 |
| 💎 | Sealed secrets | BitLocker | systemd-cryptenroll | TODO-13 §8 |
| ⭐ | BlackBox attestation JSON | internal logs | external tools | TODO-13 §9 |
| 💎 | Remote attestation (TPM2 Quote) | Device Health Attestation | Keylime AK quote | TODO-13 §13 (EK->AK + TPM2_Quote) |
| 💎 | PCR allocation policy | PCR7+11 BitLocker seal | systemd-pcrlock CEL | TODO-13 §12 (allocation table) |

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
