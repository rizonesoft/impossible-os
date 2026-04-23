---
schema_version: 1
id: tpm-measured-boot-attestation
domain: 01-boot-platform
status: active
title: "TODO-13 -- TPM Measured Boot, PCR Replay & Attestation"
---

# TODO-13 -- TPM Measured Boot, PCR Replay & Attestation

> **Goal:** Complete the measured-boot trust chain from firmware through bootloader and kernel. The bootloader retrieves the TCG event log and the kernel parses it, but true integrity requires PCR replay, TPM2 PCR reads, baseline enrollment, sealed storage, and diagnostics. This TODO turns the current TPM presence/event-count code into a full boot integrity feature.
> **Current state:** `retrieve_tpm_event_log()` copies the event log into `boot_info`, `tpm_init()` parses enough to count events and identify hash algorithms, and `tpm_integrity_init()` is a stub that reports `BOOT_INTEGRITY_NO_CRYPTO`. There is no TPM2 command transport, no PCR read, no PCR replay, no baseline enrollment, no TPM NV storage, and no attestation report.

## Inputs

- [`include/kernel/tpm.h`](../../include/kernel/tpm.h)
- [`src/kernel/tpm.c`](../../src/kernel/tpm.c)
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- -> XREF: `TODO-02-uefi-hardening-secureboot.md §3-§6` -- Secure Boot state and signed bootloader
- -> XREF: `TODO-01-boot-protocol-abi-handoff.md §4` -- TPM event log as typed boot payload
- -> XREF: `../02-kernel-core/TODO-03-kernel-libraries.md §3` -- cryptographic primitives/CSPRNG dependency
- -> XREF: `../02-kernel-core/TODO-19-code-integrity-trust-policy.md` -- image trust policy consumes measured boot

## Outcome

- TPM 1.2/2.0 event logs are parsed robustly and preserved for diagnostics.
- TPM2 PCR values can be read and compared against replayed event-log values.
- First-boot baseline enrollment stores golden measurements securely.
- Boot integrity status is available in Registry, BlackBox, system settings, and policy gates.
- Disk unlock, code integrity, and recovery can use measured-boot state.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| 💎 | 1 | Harden TCG event-log parser | TODO-01 §4 | [ ] |
| 💎 | 2 | TPM2 command transport | §1 | [ ] |
| 💎 | 3 | PCR read API | §2 | [ ] |
| 💎 | 4 | PCR replay engine | §1, ../02-kernel-core/TODO-03 §3 | [ ] |
| 💎 | 5 | Secure Boot variable measurement reconciliation | §4, TODO-02 §3 | [ ] |
| 💎 | 6 | Baseline enrollment and storage | §3, §4 | [ ] |
| 💎 | 7 | TPM NV index support | §2, §6 | [ ] |
| ⭐ | 8 | Sealed-secret boot policy hooks | §7 | [ ] |
| 💎 | 9 | Attestation report export | §3-§6 | [ ] |
| ⭐ | 10 | Recovery and mismatch UX | §6, TODO-22 | [ ] |
| 💎 | 11 | TPM tests and event-log fixtures | §1-§10 | [ ] |

## 1. Harden TCG Event-Log Parser

- [ ] Parse TPM 1.2 and TPM 2.0 crypto-agile logs without unaligned reads.
- [ ] Preserve per-event PCR index, type, digest list, and event payload metadata.
- [ ] Reject truncation with explicit offsets and status codes.
- [ ] Export event summaries to `X:\Diag\tpm-events.json`.
- [ ] When the bootloader copies the TCG event log, publish a typed payload descriptor of type `BOOT_PAYLOAD_TPM_EVENT_LOG` (enum in [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)) pointing at the copy so TODO-01 §4's overlap validator retains the region alongside boot_info / rt_mmap / USB DMA / framebuffer. -> XREF: [`01-boot-platform/TODO-01 §4`](TODO-01-boot-protocol-abi-handoff.md#4-optional-payload-descriptor-array)
- [ ] Commit: `"tpm: harden measured boot event log parser"`

## 2. TPM2 Command Transport

- [ ] Locate TPM2 ACPI table or TIS/CRB interface.
- [ ] Implement locality request/release and command/response buffer handling.
- [ ] Add timeout and degraded-boot behavior for absent or wedged TPMs.
- [ ] Support TPM2_Startup detection without perturbing firmware-owned state.
- [ ] Commit: `"tpm: add TPM2 command transport"`

## 3. PCR Read API

- [ ] Implement `tpm2_pcr_read(bank, pcr_mask, out)`.
- [ ] Support SHA-1, SHA-256, SHA-384, and SHA-512 banks where active.
- [ ] Add caching for PCR values read during Phase 0.
- [ ] Expose `tpm_pcr_get(index, alg)` to policy consumers.
- [ ] Commit: `"tpm: read PCR values"`

## 4. PCR Replay Engine

- [ ] Replay PCR extend operations from parsed event log for every active bank.
- [ ] Compare replayed values against hardware PCR reads.
- [ ] Mark event-log tampering separately from baseline mismatch.
- [ ] Include exact first mismatch in diagnostics.
- [ ] Commit: `"tpm: replay measured boot PCRs"`

## 5. Secure Boot Variable Measurement Reconciliation

- [ ] Decode EV_EFI_VARIABLE_* events for PK, KEK, db, dbx, SecureBoot, SetupMode.
- [ ] Compare measured Secure Boot state with UEFI variables read by TODO-02.
- [ ] Flag impossible combinations such as Secure Boot active but PCR7 missing policy events.
- [ ] Feed results to `boot_integrity_report`.
- [ ] Commit: `"tpm: reconcile Secure Boot measurements"`

## 6. Baseline Enrollment and Storage

- [ ] Add first-boot enrollment mode gated by physical-console confirmation.
- [ ] Store golden PCR values in UEFI authenticated variable or TPM NV index.
- [ ] Include bootloader hash, kernel hash, Secure Boot state, and firmware version metadata.
- [ ] Support baseline rotation after trusted updates.
- [ ] Commit: `"tpm: enroll measured boot baseline"`

## 7. TPM NV Index Support

- [ ] Implement NV read/write/define/undefine for one OS-owned index.
- [ ] Protect baseline with PCR policy where TPM supports it.
- [ ] Handle no-space and locked-NV degraded states.
- [ ] Add migration path from UEFI variable storage to TPM NV.
- [ ] Commit: `"tpm: measured boot NV index storage"`

## 8. Sealed-Secret Boot Policy Hooks

- [ ] Add API for sealing and unsealing small secrets to PCR policy.
- [ ] Provide FDE key-unlock hook for future storage encryption.
- [ ] Provide code-integrity policy seal hook.
- [ ] Ensure recovery can prompt when unseal fails.
- [ ] Commit: `"tpm: sealed boot policy hooks"`

## 9. Attestation Report Export

- [ ] Build signed `boot_attestation_report_t` with PCRs, event digest, Secure Boot state, and nonce.
- [ ] Export to `X:\Diag\attestation.json`.
- [ ] Add native query API for user-mode system settings.
- [ ] Add remote-attestation placeholder for platform services.
- [ ] Commit: `"tpm: export boot attestation report"`

## 10. Recovery and Mismatch UX

- [ ] Add VPD/boot diagnostics status for verified, no TPM, no baseline, mismatch, and event-log tamper.
- [ ] In recovery, explain whether firmware, bootloader, kernel, Secure Boot db, or baseline changed.
- [ ] Allow trusted baseline reset only from recovery mode with local confirmation.
- [ ] Integrate with A/B rollback if kernel measurement changed unexpectedly.
- [ ] Commit: `"recovery: measured boot mismatch UX"`

## 11. TPM Tests and Event-Log Fixtures

- [ ] Add event-log parser fixtures for TPM 1.2 and TPM 2.0.
- [ ] Add PCR replay known-vector tests.
- [ ] Add QEMU swtpm test path.
- [ ] Add degraded tests for no TPM, truncated log, and inactive PCR banks.
- [ ] Commit: `"test: TPM measured boot coverage"`

## OS Comparison

| ⭐ | Feature | Windows | Linux | Impossible OS |
| --- | --- | --- | --- | --- |
| 💎 | Secure Boot PCR integration | Measured Boot | IMA/TPM tools | TODO-13 |
| 💎 | PCR replay | internal/Defender | tpm2-tools | TODO-13 §4 |
| 💎 | Sealed secrets | BitLocker | systemd-cryptenroll | TODO-13 §8 |
| ⭐ | BlackBox attestation JSON | internal logs | external tools | TODO-13 §9 |

## Unit Tests

- [ ] `test_tpm_event_log_tpm12_fixture`
- [ ] `test_tpm_event_log_tpm20_fixture`
- [ ] `test_tpm_pcr_replay_sha256`
- [ ] `test_tpm_integrity_mismatch_report`

## Verification

- [ ] QEMU OVMF + swtpm
- [ ] Secure Boot enabled OVMF
- [ ] Bare metal with TPM 2.0
- [ ] Bare metal without TPM degrades cleanly

