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
| 💎 | 3 | PCR read API | §2 | [x] |
| 💎 | 4 | PCR replay engine | §1, ../02-kernel-core/TODO-03 §3 | [ ] |
| 💎 | 5 | Secure Boot variable measurement reconciliation (structural) | §1, TODO-02 §3 | [x] |
| 💎 | 6 | Baseline enrollment and storage | §3, §4 | [ ] |
| 💎 | 7 | TPM NV index support | §2, §6 | [ ] |
| ⭐ | 8 | Sealed-secret boot policy hooks | §7 | [ ] |
| 💎 | 9 | Attestation report export | §3-§6, §12, §13 | [ ] |
| ⭐ | 10 | Recovery and mismatch UX | §6, TODO-22 | [ ] |
| 💎 | 11 | TPM tests and event-log fixtures | §1-§10, §12, §13 | [ ] |
| 💎 | 12 | PCR allocation table and policy masks | (foundational; consumed by §6/§8/§13) | [x] |
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
> **Verified:** 2026-06-14 | commit `8a8685e8` | 4/4 items | build OK | tests 412 kernel + 16 user-mode PASS
> **Quality reviewed:** 2026-06-14 | Codex 6x (design + adversarial + consistency + perf + re-adversarial x2) | 1H+4M+1L fixed, 0 open | scope: kernel-code-quality

## 4. PCR Replay Engine

> **Blocked (2026-06-14):** replaying PCR extends for *every active bank* requires SHA-1, SHA-256, and SHA-384 transforms. The kernel currently has only SHA-512 (monocypher `crypto_sha512`); SHA-256/SHA-1/SHA-384 do not exist. The raw transforms are a shared crypto-primitive concern (the CNG wrapper `cng_sha256` is owned by `09-desktop-shell/TODO-07 §1`, which lists SHA-256 as "missing" and has not shipped). §4 cannot credibly replay measured boot without the SHA-1/256/384 banks. Prerequisite items below own the gap; §3 PCR reads + §1 event-log parse are already in place to consume the replay output. Escalation: the raw SHA primitives should land in a kernel-core crypto location and be consumed by BOTH this replay engine and TODO-07 §1's `cng_sha256` wrapper (avoid duplicating the transform).

- [ ] **Prerequisite:** kernel SHA-256 transform (`sha256_init/update/final`, FIPS 180-4) in a shared kernel crypto module + KAT vectors; later reused by `cng_sha256`. -> XREF: 09-desktop-shell/TODO-07 §1 (item: `cng_sha256` primitive).
- [ ] **Prerequisite:** kernel SHA-1 transform (legacy PCR bank) + SHA-384 (SHA-512 IV/truncation variant; SHA-512 already exists via monocypher); KAT vectors.
- [ ] **Prerequisite:** `pcr_extend(bank_alg, pcr, measurement_digest)` helper computing `H(old || measurement)` per TPM PCR-extend semantics, dispatching on bank alg.
- [ ] Replay PCR extend operations from parsed event log for every active bank (blocked on the SHA prerequisites above).
- [ ] Compare replayed values against hardware PCR reads (uses §3 `tpm_pcr_get`).
- [ ] Mark event-log tampering separately from baseline mismatch.
- [ ] Include exact first mismatch in diagnostics.
- [ ] Commit: `"tpm: replay measured boot PCRs"`

**Test checkpoint:** On a clean boot, replayed PCR values (extended from the parsed event log, every active bank) equal the hardware PCR reads from §3; a known-vector unit test replays a fixed log to expected digests; injecting one tampered event-log entry flags `event-log tamper` distinctly from `baseline mismatch` and diagnostics name the exact first-mismatch PCR index + offset. Platforms: kernel known-vector unit tests + QEMU swtpm KVM; bare metal.

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

> **Blocked (2026-06-14):** a credible measured-boot baseline requires the bootloader/kernel image hashes (SHA-256) and a verify loop (PCR replay), both blocked on the absent kernel SHA primitives (see §4). Golden PCR values (§3) + Secure Boot state (§5) + firmware version are available, but a baseline missing the image hashes and with no replay-based verify path is not a credible enrollment -- shipping it would be false completeness. Storage also wants the §7 TPM NV index (or a UEFI authenticated variable). Deferred until the SHA prerequisite (§4) lands; storage depends on §7. -> XREF: §4 (item: "Prerequisite: kernel SHA-256 transform") + §7 (TPM NV index).

- [ ] Add first-boot enrollment mode gated by physical-console confirmation.
- [ ] Store golden PCR values in UEFI authenticated variable or TPM NV index (blocked on §7 storage).
- [ ] Include bootloader hash, kernel hash, Secure Boot state, and firmware version metadata (image hashes blocked on the §4 SHA prerequisite).
- [ ] Support baseline rotation after trusted updates.
- [ ] Commit: `"tpm: enroll measured boot baseline"`

**Test checkpoint:** First-boot enrollment (gated by physical-console confirmation) stores golden PCRs + bootloader hash + kernel hash + Secure Boot state + firmware version; a second boot reads the stored baseline and reports `verified`; a simulated trusted update triggers baseline rotation (old baseline retired, new one stored), not a mismatch halt. Platforms: QEMU swtpm KVM (enrollment + re-boot); bare metal.

## 7. TPM NV Index Support

> **Design (2026-06-14, Codex 2H+1M adopted pre-code):** §7 owns the NV storage + PCR-policy MECHANISM; §6 owns the baseline CONTENT/enrollment that consumes it (resolves the prior circular §6<->§7 ownership). Constraints for the implementor: a policy-protected index MUST use POLICYREAD/POLICYWRITE, NOT OWNERREAD/OWNERWRITE (owner auth is an operational bypass of any PCR policy; owner auth is for define/undefine only). NV response-code classification MUST branch by FORMAT first -- the NV warnings 0x148-0x14C are format-0 (RC_VER1) and must be exact-compared, never run through a format-1 handle/parameter mask (which would corrupt them and hide locked/no-space states). Do NOT mark §7 complete after CRUD-only; policy protection is part of §7's contract.

- [ ] NV CRUD for one OWNER-auth OS data index: pure `tpm2_build_nv_{define,undefine,write,read}` seam + `tpm_nv_*` wrappers; password auth area (`TPM_RS_PW`). Owner-auth data index, distinct from the policy baseline.
- [ ] Degraded states: format-first NV RC classification, exact-comparing format-0 warnings (`NV_LOCKED` 0x148, `NV_SPACE` 0x14B, `NV_DEFINED` 0x14C) to `tpm_nv_status_t`; never a wedge. Fixtures for raw 0x148-0x14C.
- [ ] PCR-policy-protected baseline index: POLICYREAD/POLICYWRITE + `TPM2_StartAuthSession`/`TPM2_PolicyPCR` (trial computes authPolicy; real session satisfies read/write); owner auth only for define/undefine. -> XREF: §8.
- [ ] Migration from UEFI authenticated-variable storage to TPM NV (lossless) -- consumes the §6 baseline format. -> XREF: §6 (item: "Store golden PCR values in UEFI authenticated variable or TPM NV index").
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
| 💎 | Secure Boot PCR integration | Measured Boot | IMA/TPM tools | ⚠️ §5 structural SB var reconcile |
| 💎 | PCR replay | internal/Defender | tpm2-tools | TODO-13 §4 |
| 💎 | Sealed secrets | BitLocker | systemd-cryptenroll | TODO-13 §8 |
| ⭐ | BlackBox attestation JSON | internal logs | external tools | TODO-13 §9 |
| 💎 | Remote attestation (TPM2 Quote) | Device Health Attestation | Keylime AK quote | TODO-13 §13 (EK->AK + TPM2_Quote) |
| 💎 | PCR allocation policy | PCR7+11 BitLocker seal | systemd-pcrlock CEL | ✅ §12 event-centric table + derived masks |

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
