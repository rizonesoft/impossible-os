---
schema_version: 1
id: code-integrity-trust-policy
domain: 02-kernel-core
status: active
title: "TODO-19 -- Code Integrity & Trust Policy"
---

# TODO-19 -- Code Integrity & Trust Policy

> **Validated:** 2026-07-10 | validate-todo-file clean (structure / IO table / XREF / test wiring)

> **Gap-audited:** 2026-07-10 | gap-audit + codex-gap-audit; 10 findings filed -- 💎 measurement-only mode, per-page-hash on demand-paging, role-separated trust anchors, signed+versioned policy provenance; ⭐ dynamic/JIT code admission, revocation-bundle freshness, RO-after-lock policy page (guard-page rejected: unmaps -> reframed RO-after-init), tamper-evident CI audit-log; reciprocal XREFs to T04 §10 (HMAC-chain), T10 §17 (RO-after-init), D01 T13 (PCR), T17 (paging), T18 §9 (CI-decision sink). Scope-out: ELAM / DRTM / IPE / fs-verity-non-exec / PPL / general-lockdown (T10)

> **Goal:** Implement the kernel Code Integrity plane that decides whether executable code may run in ring 0 or ring 3. This covers kernel image verification, driver/module admission, EIF/PE/ELF executable trust, catalog files, revocation, measured-boot binding, audit mode, test-signing mode, and native policy query APIs. Code signing primitives exist elsewhere; this TODO owns the kernel decision point and enforcement policy.

> [!IMPORTANT]
> **Current state:** Secure Boot and TPM state are detected in boot work; EIF code signing is planned in TODO-17/TODO-23; CNG/code-signing primitives are planned outside kernel core; release artifact signing is planned in release TODOs. There is no kernel Code Integrity engine, no image admission callback, no catalog database, no revocation list, no audit/enforce mode split, and no unified policy for drivers vs user images.

## Inputs

- [`src/kernel/exec.c`](../../src/kernel/exec.c)
- [`src/kernel/eif.c`](../../src/kernel/eif.c)
- [`src/kernel/pe.c`](../../src/kernel/pe.c)
- → XREF: [`TODO-17-binary-system.md`](./TODO-17-binary-system.md) -- image loaders
- → XREF: [`TODO-03-kernel-libraries.md`](./TODO-03-kernel-libraries.md) -- Monocypher/CSPRNG provider
- → XREF: [`TODO-02-kernel-configuration-policy.md`](./TODO-02-kernel-configuration-policy.md) -- CI mode, test-signing, audit mode
- → XREF: [`04-drivers-hardware/TODO-04-security-hardware.md`](../04-drivers-hardware/TODO-04-security-hardware.md) -- TPM/secure hardware inputs

## Outcome

- Every executable image admission path calls `ci_validate_image()`.
- Policy modes are explicit: disabled, audit, enforce, secure-boot-enforced, test-signing.
- Kernel drivers/modules require stronger policy than user-mode applications.
- Catalog signatures and embedded signatures are both supported.
- Revoked hashes/signers are rejected even if signatures are otherwise valid.
- Decisions are logged, auditable, queryable, and visible in crash/debug metadata.

## Implementation Order

| ⭐   | Order | Deliverable                           | Depends On   | Status |
| --- | :---: | ------------------------------------- | ------------ | :----: |
| 💎   |   1   | Code Integrity policy object          | T02          |  [ ]   |
| 💎   |   2   | Image validation API and callback     | T17          |  [ ]   |
| 💎   |   3   | Hashing and signature provider bridge | T03          |  [ ]   |
| 💎   |   4   | Embedded signature validation         | §2, §3       |  [ ]   |
| 💎   |   5   | Catalog database                      | Registry/VFS |  [ ]   |
| 💎   |   6   | Revocation and deny lists             | §5           |  [ ]   |
| ⭐   |   7   | Measured-boot and Secure Boot binding | TPM          |  [ ]   |
| 💎   |   8   | Driver/module enforcement             | T18, D04 T04 |  [ ]   |
| 💎   |   9   | User-mode image enforcement           | T17, T20     |  [ ]   |
| ⭐   |  10   | CI audit, telemetry, and syscalls     | T12, T16     |  [ ]   |

## 1. Code Integrity Policy Object

- [ ] Define `ci_policy_t`: mode, secure boot state, test-signing flag, allowed roots, denied roots, revoked hashes, audit flags.
- [ ] Load defaults from compiled policy; merge boot/registry policy via TODO-02.
- [ ] Lock policy after Phase 2 on Secure Boot systems.
- [ ] Deny attempts to downgrade enforcement after lock.
- [ ] Add a MEASUREMENT-only mode (record image hash + feed the TPM PCR aggregate, no allow/deny verdict) distinct from audit (log-but-allow). -> XREF: D01 T13 (PCR aggregate).
- [ ] Trust anchors are role-separated tiers, not a flat list: compiled-in root / boot-added / firmware-platform / revoked, each with distinct lock + downgrade semantics.
- [ ] The `ci_policy_t` artifact itself is signed + versioned: authenticate its provenance (authorized update signers) + anti-rollback on version before applying -- a mutable boot/registry merge + in-memory lock does NOT authenticate the policy.
- [ ] After lock, mark the `ci_policy_t` page READ-ONLY via PTE (RO-after-init, NOT a guard page which unmaps and would fault the CI engine's own reads). -> XREF: T10 §17 (`.rodata` RO-after-init).
- [ ] Scope: the general kernel-lockdown surface (raw MSR / phys-mem / ACPI-override blocking once CI locks) is owned by T10, not here; §1 owns only image-admission policy.
- [ ] Commit: `"kernel: ci -- code integrity policy object"`

**Test checkpoint:** `ci_policy_t` loads compiled defaults; after the Phase-2 lock on a Secure Boot system, an enforcement-downgrade attempt returns an error and the mode is unchanged. Serial: `"ci: policy locked (mode=enforce, secureboot=1)"`. Test on: QEMU WHPX + TCG; bare metal.

---

## 2. Image Validation API and Callback

- [ ] Add `ci_validate_image(ci_image_info_t *image, ci_decision_t *decision)`.
- [ ] Image info includes path, format, type, signer, hash, requested trust level, loader, process token, and measured-boot context.
- [ ] Register Executive callback `\Callback\CiImageLoad` through TODO-17.
- [ ] All loaders must call CI before mapping executable pages.
- [ ] Dynamic-code / JIT / W->X admission: image-load hooks miss code generated at runtime with no backing image. Gate a `ci_validate_dynamic_code()` path (or a policy that forbids RWX->RX transitions unless the producer is trusted).
- [ ] Commit: `"kernel: ci -- image validation API and callback"`

**Test checkpoint:** `ci_validate_image()` on a known-good image returns ALLOW and on a tampered image returns DENY; the `\Callback\CiImageLoad` callback fires before executable pages are mapped. Serial: `"ci: validate '<name>' -> <decision>"`. Test on: QEMU WHPX + TCG; bare metal.

---

## 3. Hashing and Signature Provider Bridge

- [ ] Implement SHA-256/BLAKE2b digest bridge using kernel crypto provider.
- [ ] Support Ed25519 as native Impossible OS signing algorithm.
- [ ] Keep RSA/X.509 catalog support optional until CNG provider matures.
- [ ] Ensure validation is allocation-bounded and safe before user-mode starts.
- [ ] Commit: `"kernel: ci -- hashing and signature provider bridge"`

**Test checkpoint:** the SHA-256 digest of a fixed buffer matches the known test vector; an Ed25519 signature verifies with the correct key and fails with a wrong key; validation performs no heap allocation after the policy lock. Test on: QEMU WHPX + TCG; bare metal.

---

## 4. Embedded Signature Validation

- [ ] EIF: validate signed metadata/trailer before segment mapping.
- [ ] PE: parse WIN_CERTIFICATE and Authenticode digest exclusions.
- [ ] ELF: support Impossible OS note-section signature for native ELF tools.
- [ ] Reject malformed or ambiguous signed ranges.
- [ ] Per-page hash validation on demand-paging (parity 💎: Win page-hashes + Linux fs-verity): a pre-map whole-file hash is bypassed if a page faults in later. Validate each executable page as paged in. -> XREF: TODO-17 (paging/loader).
- [ ] IMA/EVM-style appraisal of image security metadata (not just file bytes): if images carry integrity-protected attributes, appraise them too, not only the content digest.
- [ ] Commit: `"kernel: ci -- embedded signature validation"`

**Test checkpoint:** a signed EIF trailer validates before segment mapping; a malformed/overlapping signed range is rejected with an error (not silently accepted); PE Authenticode digest excludes the checksum + certificate-table fields per spec. Test on: QEMU WHPX + TCG; bare metal.

---

## 5. Catalog Database

- [ ] Load catalog files from `C:\Impossible\System\Catalogs\`.
- [ ] Catalog maps image hash to signer, policy OID/equivalent, timestamp, and allowed image type.
- [ ] Catalog cache is indexed by hash and protected by ERESOURCE.
- [ ] Add recovery path if catalogs are missing in Safe Mode.
- [ ] Commit: `"kernel: ci -- catalog database"`

**Test checkpoint:** a catalog file maps a known image hash to its signer; a hash lookup hits the ERESOURCE-protected cache; Safe Mode with missing catalogs takes the recovery path without hanging. Test on: QEMU WHPX + TCG; bare metal.

---

## 6. Revocation and Deny Lists

- [ ] Registry-backed revoked hash list and signer deny list.
- [ ] Optional catalog-delivered revocation bundles.
- [ ] Revocation always overrides allow, including test-signing unless boot explicitly enters lab mode.
- [ ] Publish notification on revocation database update.
- [ ] Revocation-bundle freshness: bundles carry a monotonic version; reject a bundle older than the applied floor (anti-rollback) and flag stale revocation data past a policy age. -> XREF: D01 T13 (monotonic anti-rollback anchor).
- [ ] Commit: `"kernel: ci -- revocation and deny lists"`

**Test checkpoint:** a revoked hash is denied even with an otherwise-valid signature; a revoked signer is denied; a revocation-DB update fires the update notification. Test on: QEMU WHPX + TCG; bare metal.

---

## 7. Measured-Boot and Secure Boot Binding

- [ ] Bind CI policy to Secure Boot state reported by boot platform.
- [ ] Record policy digest in TPM PCR event log when TPM is present.
- [ ] Refuse unsigned kernel drivers when Secure Boot is enabled.
- [ ] Audit mismatch between bootloader trust state and kernel policy state.
- [ ] Slot-boot-verification query (was the active slot signature-verified this boot?) so A/B mark-good + the anti-rollback floor refuse to bless an unverified slot. -> XREF: [`D01 T21 §5`](../01-boot-platform/TODO-21-ab-boot-rollback.md) + `§8`.
- [ ] Commit: `"kernel: ci -- measured-boot and secure boot binding"`

**Test checkpoint:** with Secure Boot enabled, an unsigned kernel driver is refused; the policy digest is recorded in the TPM PCR event log when a TPM is present; a bootloader-vs-kernel trust-state mismatch is audited. Test on: QEMU WHPX + TCG; bare metal.

---

## 8. Driver/Module Enforcement

- [ ] Validate built-in drivers at boot by image hash table.
- [ ] Validate `.kmod` and driver images before relocation/load.
- [ ] Enforce signer class: kernel, boot-start driver, normal driver, test driver.
- [ ] Block unsigned executable memory allocation for driver code.
- [ ] Commit: `"kernel: ci -- driver/module enforcement"`

**Test checkpoint:** a built-in driver validates against its boot hash table; an unsigned `.kmod` is blocked before relocation/load; signer-class gating rejects a normal-signed image that requests boot-start. Test on: QEMU WHPX + TCG; bare metal.

---

## 9. User-Mode Image Enforcement

- [ ] Apply configurable policy by path, signer, zone, and token integrity level.
- [ ] Allow audit-only mode for unsigned user applications while blocking unsigned elevated/system processes.
- [ ] Add `STATUS_INVALID_IMAGE_HASH` and `STATUS_IMAGE_CERT_REVOKED` returns.
- [ ] Include CI decision in loaded-image registry. -> XREF: TODO-18 §9 (provenance CI decision field).
- [ ] Commit: `"kernel: ci -- user-mode image enforcement"`

**Test checkpoint:** an unsigned user app runs in audit mode but an unsigned elevated/system process is blocked with `STATUS_INVALID_IMAGE_HASH`; a revoked cert yields `STATUS_IMAGE_CERT_REVOKED`; the CI decision appears in the loaded-image registry. Test on: QEMU WHPX + TCG; bare metal.

---

## 10. CI Audit, Telemetry, and Syscalls

- [ ] Publish CI allow/deny/audit events through klog/ETW, emitted INTO the tamper-evident HMAC-chained log so a post-compromise trail edit is detectable. -> XREF: TODO-04 §10 ("Log integrity verification (HMAC)").
- [ ] Add `NtQuerySystemInformation(SystemCodeIntegrityInformation)`.
- [ ] Add `ci_dump_policy()` for KD/crash dump.
- [ ] Add `NtSetSystemInformation(SystemCodeIntegrityPolicyInformation)` for authorized policy refresh (revocation update); reject downgrades after lock.
- [ ] Commit: `"kernel: ci -- audit, telemetry, and syscalls"`

**Test checkpoint:** `NtQuerySystemInformation(SystemCodeIntegrityInformation)` returns the current mode + flags; a deny decision is published via klog/ETW; `ci_dump_policy()` output appears in a crash dump; a post-lock policy downgrade via `NtSetSystemInformation` is rejected. Test on: QEMU WHPX + TCG; bare metal.

---

## OS Comparison

| ⭐   | Feature                    | 🪟 Win11                          | 🐧 Linux                        | 🚀 Impossible OS                       |
| --- | -------------------------- | -------------------------------- | ------------------------------ | ------------------------------------- |
| 💎   | Kernel CI engine           | ✅ ci.dll (CI/WDAC)               | ✅ IMA/EVM appraisal            | 🔄 §1-§2 ci_validate_image + policy    |
| 💎   | Enforce/audit mode split   | ✅ WDAC audit vs enforce          | ✅ IMA log vs enforce           | 🔄 §1 explicit mode enum               |
| 💎   | Embedded signatures        | ✅ Authenticode                   | ✅ PE/module appended sig       | 🔄 §4 EIF trailer + PE Authenticode    |
| 💎   | Catalog signatures         | ✅ .cat catalog store             | ⚠️ IMA sig files                | 🔄 §5 catalog DB (hash->signer)        |
| 💎   | Revocation                 | ✅ CRL / dbx / revoked hashes     | ⚠️ manual keyring revoke        | 🔄 §6 revoked-hash + signer deny       |
| 💎   | Measured/Secure Boot bind  | ✅ HVCI + PCR policy binding      | ✅ IMA + TPM PCR                 | 🔄 §7 policy digest -> PCR + SB refuse  |
| 💎   | Driver signing enforce     | ✅ WHQL / boot-start classes      | ✅ CONFIG_MODULE_SIG            | 🔄 §8 signer-class + boot hash table   |
| ⭐   | Native Ed25519 signing     | ❌ RSA/ECDSA Authenticode         | ⚠️ RSA module sig               | 🔄 §3 Ed25519 as native algorithm      |
| ⭐   | CI decision in image reg   | ⚠️ separate CI state             | ❌ none unified                 | 🔄 §9 -> TODO-18 §9 provenance field    |
| ⭐   | CI query syscall           | ✅ SystemCodeIntegrityInformation | ⚠️ /sys/kernel/security/ima     | 🔄 §10 NtQuery/NtSet + ci_dump_policy   |
| 💎   | Per-page hash on demand    | ✅ Authenticode page hashes       | ✅ fs-verity Merkle             | 🔄 §4 validate each paged-in page       |
| 💎   | Layered trust anchors      | ⚠️ cert-store roots               | ✅ 4-tier keyring               | 🔄 §1 role-separated anchor tiers       |
| 💎   | Signed/versioned policy    | ✅ signed WDAC policy + rollback  | ⚠️ keyring, no policy artifact  | 🔄 §1 authenticate policy provenance    |
| 💎   | Measurement-only mode      | ⚠️ audit only                     | ✅ IMA measure vs appraise      | 🔄 §1 measure mode -> PCR aggregate     |
| ⭐   | Dynamic/JIT code admission | ✅ dynamic code policy (.NET)     | ⚠️ W^X, no CI hook              | 🔄 §2 ci_validate_dynamic_code          |
| ⭐   | Policy self-protection     | ✅ HVCI VTL-isolated CI           | ⚠️ lockdown, same ring          | 🔄 §1 RO-after-lock policy page          |
| ⭐   | Tamper-evident CI audit    | ⚠️ ETW (mutable)                  | ⚠️ audit log (mutable)          | 🔄 §10 -> T04 §10 HMAC-chain            |

> **After §1-§6:** a kernel CI decision point every loader calls, with policy modes, embedded + catalog signatures, and revocation that overrides allow.
> **After §7-§10:** Secure-Boot/TPM binding, driver vs user enforcement split, CI decisions surfaced in the image registry + crash dumps, and native policy-query syscalls.

---

## Unit Tests

Create `src/kernel/test/test_ci.c`, register via `test_register_ci()` in `test_runner.c` under `TEST_CAT_SECURITY`. Tests use in-memory `ci_policy_t` / `ci_image_info_t` fixtures and pure helpers; they MUST NOT call live loaders or boot infrastructure (per the test-side-effect policy). Concrete assertions:

- `test_ci_policy_lock`: after `ci_policy_lock()`, a mode-downgrade call returns an error and `policy.mode` is unchanged.
- `test_ci_validate_allow_deny`: `ci_validate_image()` returns ALLOW for a fixture whose hash matches an allowed entry, DENY for a mismatch.
- `test_ci_digest_vector`: the SHA-256 helper on a fixed buffer equals the known test vector.
- `test_ci_ed25519_verify`: a valid Ed25519 signature verifies; a corrupted signature fails.
- `test_ci_revocation_overrides`: a revoked hash returns DENY even when its signature is otherwise valid.
- `test_ci_signer_class`: a normal-signed image requesting boot-start is rejected by signer-class gating.
- `test_ci_status_codes`: `STATUS_INVALID_IMAGE_HASH` and `STATUS_IMAGE_CERT_REVOKED` are distinct, non-success NTSTATUS values.

> **Note:** signature/digest tests use fixed vectors and pure crypto helpers, not the live CNG/TPM providers (which are hardware/provider-dependent -- validated on WHPX + bare metal via serial log).

---

## Verification

- [ ] `bash scripts/build.sh` shows `=== BUILD OK ===`.
- [ ] `bash scripts/test.sh SUITE=security` passes with the new `test_ci` assertions.
- [ ] Boot verifier: serial log confirms the CI policy locks on a Secure Boot system and an unsigned driver is refused in enforce mode.
- [ ] `bash scripts/test-smoke.sh` shows `SMOKE TEST PASSED` (CI is on the loader admission path).

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | N suites, 0 failures

