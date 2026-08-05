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

| ⭐  | Order | Deliverable                                      | Depends On   | Status |
| --- | :---: | ------------------------------------------------ | ------------ | :----: |
| 💎  |   1   | Code Integrity policy object                     | T02          |  [/]   |
| 💎  |   2   | Image validation API and callback                | T17          |  [/]   |
| 💎  |   3   | Hashing and signature provider bridge            | T03          |  [x]   |
| 💎  |   4   | Embedded signature validation                    | §2, §3       |  [/]   |
| 💎  |   5   | Catalog database                                 | Registry/VFS |  [/]   |
| 💎  |   6   | Revocation and deny lists                        | §5           |  [/]   |
| ⭐  |   7   | Measured-boot and Secure Boot binding            | TPM          |  [/]   |
| 💎  |   8   | Driver/module enforcement                        | T18, D04 T04 |  [/]   |
| 💎  |   9   | User-mode image enforcement                      | T17, T20     |  [/]   |
| ⭐  |  10   | CI audit, telemetry, and syscalls                | T12, T16     |  [/]   |
| ⭐  |  11   | Post-ship follow-up backfill (2026-07-31 cohort) | --           |  [ ]   |

## 1. Code Integrity Policy Object

- [x] Define `ci_policy_t` (`include/kernel/ci/ci.h`): ORDERED `ci_enforcement_t` + SEPARATE `test_signing`/`measurement` flags + `ci_sb_state_t` tri-state + role-separated trust-anchor tiers + flat revoked-hash list + audit flags.
- [x] MEASUREMENT flag present (`ci_is_measurement`); records hash, feeds the TPM PCR aggregate, no verdict (distinct from audit). PCR feed -> XREF: D01 T13.
- [x] Trust anchors are role-separated tiers (`ci_trust_tier_t`: compiled-in / boot-added / firmware-platform / revoked), not a flat list.
- [x] Load compiled defaults + config merge (`kernel_config_get()`), gated FAIL-CLOSED: relax honored only when `kernel_safe_mode_allows(SAFE_COMP_CI_RELAX)` AND SB known-off; active/unreadable SB force ENFORCE + flags off.
- [x] Atomic publish: `ci_init` builds the record then release-stores a ready flag; accessors acquire-load it and fail closed (ENFORCE / revoked / not-sealed) before publish -- no torn read, no fail-open window.
- [x] Structural immutability shipped: no public mutator (only `ci_init` writes) + fail-closed pre-publish accessors. Wiring the seal into the `policy_lock.c` ratchet (registry downgrade -> `KeBugCheckEx`) is pending with the boot wiring.
- [/] Physical RO-after-lock page is BLOCKED: Phase-3 `vmm_set_ro` is local-TLB-only; needs SMP TLB shootdown + flattened page-aligned storage with the lock OUTSIDE the sealed range. -> XREF: D03 T07 §2 (SMP TLB shootdown).
- [/] Scope: the general kernel-lockdown surface (raw MSR / phys-mem / ACPI-override blocking once CI locks) is owned by T10, not here; §1 owns only image-admission policy.
- [x] Commit: `"kernel: ci -- code integrity policy object (foundational)"`

**Test checkpoint:** the pure builder yields SECUREBOOT+flags-off under active SB, ENFORCE+flags-off under unreadable SB, and honors relax only when safe-mode-allowed AND SB known-off; the enforcement scalar is ordered and a permissive flag flip does not move it; pre-publish accessors fail closed (ENFORCE, every hash revoked, not sealed); a published fixture confirms a NULL digest still denies and the revocation scan matches. 8 `TEST_CAT_SECURITY` suites. Artifact Ed25519 auth + version-floor + POST_REGISTRY seal-log land with the follow-up items. Test on: QEMU WHPX + TCG; bare metal.
> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 8 CI suites, 0 failures
>
> **Notes:**
> - Shipped `include/kernel/ci/ci.h` + `src/kernel/ci/ci_policy.c` (`ci_policy_t`, pure builder, `ci_init`, fail-closed accessors, test-only publish/reset seam) + `src/kernel/test/test_ci.c` (8 `TEST_CAT_SECURITY` suites).
> - Fail-closed: relax only under safe-mode-allow AND known-off SB; active/unknown SB force ENFORCE + flags off; sealed record keeps the SB tri-state (UNKNOWN != OFF); pre-publish accessors + a NULL/uncomputed digest deny (ENFORCE / revoked / not-sealed).
> - `ci_init` not yet wired into boot (accessors fail closed until called); POST_REGISTRY wiring + `SUBSYS_CI` + policy_lock ratchet + Ed25519 artifact auth + version floor remain `[ ]`; physical RO-after-lock `[/]` blocked on D03 T07 §2.
> - Canonical doc: the `include/kernel/ci/ci.h` header contract.
> - Scope: §1 owns the policy object only. §2 owns `ci_validate_image`; §3 the crypto bridge; §6 revocation-list population; T10 general lockdown.
>
> **Verified:** 2026-07-10 | commit b5270770 | 7 [x] / 13 items (5 [ ] follow-up, 1 [/] blocked D03 T07 §2) | build OK | 1150 tests / 130 Security suites, 0 failed
> **Quality reviewed:** 2026-07-10 | Codex 8x (design, adversarial x2, re-adversarial, consistency x2, perf x2) + kernel-quality-auditor | fail-closed H + SB-tri-state/perf/header-contract/weak-test M fixed; NULL-deny red-green verified | scope: kernel-code-quality

---

## 2. Image Validation API and Callback

- [x] Define `ci_image_info_t`/`ci_decision_t`/`ci_verdict_t`/`ci_deny_reason_t`/`ci_image_type_t`/`ci_loader_id_t` in `include/kernel/ci/ci_image.h`; the validator recomputes the digest and never trusts a caller-supplied hash.
- [x] `ci_validate_image()` decision engine (`src/kernel/ci/ci_image.c`): revoked -> DENY; unverified -> DENY under ENFORCE/SECUREBOOT, AUDIT_ALLOW under AUDIT; DISABLED -> ALLOW; bad-request/unsealed fail closed.
- [x] `\Callback\CiImageLoad` notification object (`ci_image_init` via `ExCreateCallback`); `ci_validate_image` fires `ExNotifyCallback` with the decision when the object exists.
- [x] `ci_validate_dynamic_code()` engine for JIT / W->X: W+X refused outright; a non-W+X runtime request is UNVERIFIED and mapped to a verdict by enforcement mode.
- [/] Image-info `signer` + measured-boot context reserved (zeroed): signer needs embedded-signature parsing; the PCR record needs the TPM measured-boot binding. -> XREF: T19 §4; D01 T13.
- [/] Wire `ci_image_init` + `ci_init` policy seal + `SUBSYS_CI` into boot: BLOCKED on unifying the enforcement authority (policy_lock `policy.ci.mode` vs `ci_policy`). -> XREF: T19 §1 ratchet-domain merge.
- [/] Loaders call CI before mapping: needs transactional NX-until-admission remap (`task_exec` remaps before `exec_load_fmt`) + a real ALLOW path from signatures; per-domain wiring in §8/§9. -> XREF: T19 §4, §8, §9.
- [x] Commit: `"kernel: ci -- image validation API and callback"`

**Test checkpoint:** the decision engine denies an unverified image under ENFORCE (reason UNVERIFIED) and audit-allows it under AUDIT; a revoked digest is denied (reason REVOKED, overriding unverified) and the validator recomputes the digest, ignoring a caller-supplied hash; NULL/empty/unsealed inputs fail closed; DISABLED allows; dynamic W+X is refused and non-W+X runtime code is UNVERIFIED-by-mode; caller-prefilled signer/measured OUT fields are cleared. 4 new `TEST_CAT_SECURITY` suites. Live loader firing + boot wiring are deferred (see `[/]` items). Test on: QEMU WHPX + TCG; bare metal.
> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 12 CI suites, 0 failures
>
> **Notes:**
> - Shipped `include/kernel/ci/ci_image.h` + `src/kernel/ci/ci_image.c`: the CI image-admission decision engine (`ci_validate_image`, `ci_validate_dynamic_code`, `ci_image_init`) + 4 `TEST_CAT_SECURITY` suites.
> - Fail-closed engine: always recomputes the digest; revocation overrides all; UNVERIFIED -> DENY under ENFORCE, AUDIT_ALLOW under AUDIT; NULL/empty/unsealed deny; W+X refused. Design-review adoptions in the commit message.
> - Deferred (concrete owners): live loader enforcement + `ci_init`/`SUBSYS_CI` boot wiring BLOCKED on the §1 ratchet-merge + §4 signatures + transactional remap; dynamic-code gating filed as a `[ ]` item naming the `nt_memory.c` sites.
> - Canonical doc: the `include/kernel/ci/ci_image.h` header contract.
> - Scope: §2 owns the decision engine + callback object only. §3 the crypto bridge; §4 embedded signatures (the real ALLOW path); §6 revocation population; T10 general lockdown.
>
> **Verified:** 2026-07-10 | commit `884f8c6c` | 4/8 items | build OK | tests 1176/1176 PASS (12 CI suites)
> **Accepted:** [L] `ci_validate_dynamic_code` fires no `\Callback\CiImageLoad` (dynamic code has no `ci_image_info_t`); synthetic-info callback lands with the call-site wiring -> XREF: 02-kernel-core/TODO-19 §2 (item: "Gate dynamic-code sites: `NtAllocateVirtualMemory`..." at line 94)
> **Quality reviewed:** 2026-07-10 | Codex 6x (design, adversarial x2, re-adversarial, consistency, perf) + kernel-quality-auditor | 2H+3M+3L fixed, 1L accepted-XREF | test gap: ci_image_init/callback publication (needs live OB, no-live-boot policy) | scope: kernel-code-quality

---

## 3. Hashing and Signature Provider Bridge

- [x] SHA-256/SHA-384/BLAKE2b-256 digest bridge (`ci_crypto_digest`, `include/kernel/ci/ci_crypto.h` + `src/kernel/ci/ci_crypto.c`) over `crypto_hash` + Monocypher BLAKE2b; fail-closed on unknown alg / small `out_cap` / NULL-data-with-length.
- [x] Ed25519 native signature verification (`ci_crypto_verify`, `CI_SIG_ED25519`) via Monocypher PureEd25519: true only on a valid signature, false on tamper / wrong-key / wrong-length / NULL. Signers use PureEd25519 over the raw bytes.
- [x] RSA/X.509 reserved: `CI_SIG_RSA_PKCS1` denies (fail closed) until the CNG provider matures.
- [x] Allocation-free bridge (`crypto_hash` + Monocypher are stack / caller-context) -- safe before user-mode starts.
- [x] Commit: `"kernel: ci -- hashing and signature provider bridge"`

**Test checkpoint:** the bridge digest matches the underlying primitive for SHA-256 and BLAKE2b-256; unknown alg / too-small `out_cap` / NULL-data-with-length all return -1 (NULL+len0 is valid); a valid Ed25519 signature verifies and tamper / wrong-key / wrong-length / NULL / unsupported-alg all deny. 2 new `TEST_CAT_SECURITY` suites. Test on: QEMU WHPX + TCG; bare metal.
> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 14 CI suites, 0 failures
>
> **Notes:**
> - Shipped `include/kernel/ci/ci_crypto.h` + `src/kernel/ci/ci_crypto.c`: the fail-closed CI crypto bridge (`ci_crypto_digest`, `ci_crypto_verify`, `ci_crypto_digest_len`) + 2 `TEST_CAT_SECURITY` suites.
> - Thin selection over existing primitives: `crypto_hash` (SHA-2/3), Monocypher BLAKE2b + PureEd25519; allocation-free, every error/unsupported path denies. Design-review NULL-BLAKE2b guard adopted (commit message).
> - Downstream: unblocks embedded-signature validation (§4) which consumes `ci_crypto_verify` + `ci_crypto_digest`; the §1 policy-artifact Ed25519 auth item can also use it.
> - Canonical doc: the `include/kernel/ci/ci_crypto.h` header contract.
> - Scope: §3 owns the digest + verify bridge only. §4 owns EIF/PE/ELF signature parsing (which ranges to hash/verify); RSA/X.509 stays reserved until the CNG provider lands.
>
> **Verified:** 2026-07-10 | commit `45b1fc46` | 4/4 items | build OK | tests 1197/1197 PASS (14 CI suites)
> **Quality reviewed:** 2026-07-10 | Codex 6x (design, adversarial x2, re-adversarial, consistency, perf) + kernel-quality-auditor | 1H+1M+3L fixed | scope: kernel-code-quality

---

## 4. Embedded Signature Validation

- [/] EIF: validate signed trailer before segment mapping. BLOCKED: the spec'd signature block (algo/sig_size/sig) carries no signer pubkey/key-id, and anchors store only key_id -- no pubkey to Ed25519-verify. -> XREF: D02 T20.
- [ ] EIF sig-block ABI decision (prereq): amend `specs/eif-format.md` to carry the signer pubkey[32] (+ key-id) + bump `EIF_VERSION`, so CI can Ed25519-verify and match the pubkey hash to a trust anchor. OWNED by D02 T20 §11. -> XREF: D02 T20 §11.
- [/] PE: parse `WIN_CERTIFICATE` + Authenticode digest exclusions. BLOCKED: real Authenticode is PKCS#7/ASN.1; §3 `ci_crypto` only does raw Ed25519. Needs an Impossible-OS simplified `wCertificateType` decision.
- [/] ELF: Impossible-OS note-section signature. BLOCKED: no `PT_NOTE` walker + no vendor note namespace; needs a note-format decision (n_name namespace, n_type, 4-vs-8-byte align per the GNU-property precedent).
- [ ] Anchor-lookup helper (prereq): add `ci_anchor_find(key_id, tier)` over `ci_policy_get()->anchors` to §1; needs the compiled Ed25519 policy-root anchor. -> XREF: T19 §1 (line 62).
- [/] Reject malformed/ambiguous signed ranges -- `eif_validate()` already bounds-checks `signature_offset` + canonical ordering in `src/kernel/eif.c`; PE/ELF range checks land with those formats.
- [ ] Per-page hash validation on demand-paging (parity 💎: Win page-hashes + Linux fs-verity): a pre-map whole-file hash is bypassed if a page faults in later. Validate each executable page as paged in. -> XREF: TODO-17 (paging/loader).
- [ ] IMA/EVM-style appraisal of image security metadata (not just file bytes): if images carry integrity-protected attributes, appraise them too, not only the content digest.
- [ ] Commit: `"kernel: ci -- embedded signature validation"`

**Test checkpoint:** a signed EIF trailer validates before segment mapping; a malformed/overlapping signed range is rejected with an error (not silently accepted); PE Authenticode digest excludes the checksum + certificate-table fields per spec. Test on: QEMU WHPX + TCG; bare metal.
>
> **Deferred:** [H] Embedded-signature validation is architecturally blocked: all three formats need a signer-key delivery decision, plus a compiled trust anchor + anchor-lookup and §2 loader wiring. No code shippable without those. -> XREF: 02-kernel-core/TODO-19 §4 (item: "EIF sig-block ABI decision (prereq)" at line 139); 02-kernel-core/TODO-19 §4 (item: "Anchor-lookup helper (prereq)" at line 142); 02-kernel-core/TODO-19 §1 (item: "Authenticate the policy artifact's own signature against a compiled-in Ed25519 policy-root key" at line 62)

---

## 5. Catalog Database

- [/] Load catalog files from `C:\Impossible\System\Catalogs\`. BLOCKED: a catalog file format must be decided (Windows `.cat` is PKCS#7/ASN.1 and §3 `ci_crypto` has no ASN.1 parser).
- [ ] Catalog file-format decision (prereq): define an Impossible-OS native catalog format (versioned header + hash->signer entries, Ed25519-signed) since ASN.1 `.cat` is out of scope. -> XREF: T19 §3, §4.
- [/] Catalog maps image hash to signer, policy tag, timestamp, allowed image type. BLOCKED on the format decision + §4 signer identity.
- [ ] Catalog cache indexed by hash + ERESOURCE-protected (implementable once the format lands; the cache structure is format-independent).
- [ ] Recovery path if catalogs missing in Safe Mode.
- [ ] Commit: `"kernel: ci -- catalog database"`

**Test checkpoint:** a catalog file maps a known image hash to its signer; a hash lookup hits the ERESOURCE-protected cache; Safe Mode with missing catalogs takes the recovery path without hanging. Test on: QEMU WHPX + TCG; bare metal.
>
> **Deferred:** [H] Catalog database is blocked on a catalog file-format decision (ASN.1 `.cat` is out of scope for the raw-Ed25519 `ci_crypto` bridge) and on §4 signer identity. The hash-indexed ERESOURCE cache is implementable once the format lands. -> XREF: 02-kernel-core/TODO-19 §5 (item: "Catalog file-format decision (prereq)" at line 157); 02-kernel-core/TODO-19 §4 (item: "EIF sig-block ABI decision (prereq)" at line 139)

---

## 6. Revocation and Deny Lists

- [x] Revocation always overrides allow -- `ci_hash_revoked` (§1) denies any revoked digest and `ci_validate_image` (§2) checks revocation before every allow path incl. DISABLED. No lab-mode exemption (fail-closed).
- [/] Registry-backed revoked hash list + signer deny list. BLOCKED: the §1 revoked list is sealed-immutable; registry population needs `ci_init` pre-seal (unwired) or a mutable refresh store. -> XREF: T19 §1, §10.
- [ ] Optional catalog-delivered revocation bundles. BLOCKED on the §5 catalog format. -> XREF: T19 §5.
- [ ] Publish notification on revocation database update (needs the mutable store + an update event). -> XREF: T19 §10.
- [ ] Revocation-bundle freshness: bundles carry a monotonic version; reject a bundle older than the applied floor (anti-rollback) and flag stale revocation data past a policy age. -> XREF: D01 T13 (monotonic anti-rollback anchor).
- [ ] Commit: `"kernel: ci -- revocation and deny lists"`

**Test checkpoint:** a revoked hash is denied even with an otherwise-valid signature; a revoked signer is denied; a revocation-DB update fires the update notification. Test on: QEMU WHPX + TCG; bare metal.
>
> **Deferred:** [H] Revocation enforcement is DONE (§1 `ci_hash_revoked` + §2 revocation-first), but registry-backed population + update-notification are blocked on `ci_init` boot-wiring and a mutable/refresh revocation store (the §1 list is sealed-immutable). -> XREF: 02-kernel-core/TODO-19 §1 (item: "Wire `ci_init` at `POLICY_PHASE_POST_REGISTRY`" at line 61); 02-kernel-core/TODO-19 §10 (item: "Add `NtSetSystemInformation(SystemCodeIntegrityPolicyInformation)` for authorized policy refresh" at line 232)

---

## 7. Measured-Boot and Secure Boot Binding

- [x] Bind CI policy to Secure Boot state -- §1 `ci_sb_state` resolves the tri-state and pins SECUREBOOT enforcement under active SB; UNKNOWN fails closed.
- [/] Record policy digest in TPM PCR event log when a TPM is present. BLOCKED on the TPM measured-boot binding. -> XREF: D01 T13.
- [/] Refuse unsigned kernel drivers when Secure Boot is enabled. BLOCKED on §4 signature validation + §8 driver enforcement. -> XREF: T19 §4, §8.
- [ ] Audit mismatch between bootloader trust state and kernel policy state (§1 exposes `degraded_trust_flags`; needs a boot-vs-kernel compare + audit event).
- [ ] Slot-boot-verification query (was the active slot signature-verified this boot?) so A/B mark-good + the anti-rollback floor refuse to bless an unverified slot. -> XREF: [`D01 T21 §5`](../01-boot-platform/TODO-21-ab-boot-rollback.md) + `§8`.
- [ ] Commit: `"kernel: ci -- measured-boot and secure boot binding"`

**Test checkpoint:** with Secure Boot enabled, an unsigned kernel driver is refused; the policy digest is recorded in the TPM PCR event log when a TPM is present; a bootloader-vs-kernel trust-state mismatch is audited. Test on: QEMU WHPX + TCG; bare metal.
>
> **Deferred:** [H] Secure-Boot binding is DONE (§1 `ci_sb_state` pins SECUREBOOT enforcement); the TPM PCR digest record is blocked on the measured-boot binding, refuse-unsigned-drivers on §4 + §8, and the slot-verification query on the A/B boot work. -> XREF: 04-drivers-hardware/TODO-04 (TPM measured-boot); 02-kernel-core/TODO-19 §8 (item: "Validate `.kmod` and driver images before relocation/load" at line 197); 01-boot-platform/TODO-21 §5 (slot verification)

---

## 8. Driver/Module Enforcement

- [/] Validate built-in drivers at boot by image hash table. BLOCKED on §4 signature validation + a compiled boot driver-hash table. -> XREF: T19 §4.
- [/] Validate `.kmod`/driver images before relocation/load. BLOCKED on §4 + the kernel module loader. -> XREF: T19 §4, D04 T05.
- [ ] Enforce signer class (kernel / boot-start / normal / test driver). BLOCKED on §4 signer identity. -> XREF: T19 §4.
- [/] Block unsigned executable memory allocation for driver code. BLOCKED on wiring `ci_validate_dynamic_code` into the nt_memory PAGE_EXECUTE sites (§2 [ ]). -> XREF: T19 §2.
- [ ] Commit: `"kernel: ci -- driver/module enforcement"`

**Test checkpoint:** a built-in driver validates against its boot hash table; an unsigned `.kmod` is blocked before relocation/load; signer-class gating rejects a normal-signed image that requests boot-start. Test on: QEMU WHPX + TCG; bare metal.
>
> **Deferred:** [H] Driver/module enforcement is blocked on §4 embedded-signature validation, the §2 loader/dynamic-code wiring, and the kernel module loader. No enforcement is possible until a real ALLOW path + loader call sites exist. -> XREF: 02-kernel-core/TODO-19 §4 (item: "EIF sig-block ABI decision (prereq)" at line 139); 02-kernel-core/TODO-19 §2 (item: "Gate dynamic-code sites: `NtAllocateVirtualMemory`..." at line 94); 04-drivers-hardware/TODO-05 (kernel module loader)

---

## 9. User-Mode Image Enforcement

- [/] Apply configurable policy by path, signer, zone, token integrity level. BLOCKED on §2 loader wiring + §4 signer identity. -> XREF: T19 §2, §4.
- [/] Audit-only for unsigned user apps while blocking unsigned elevated/system. BLOCKED on the §2 loader enforcement wiring. -> XREF: T19 §2.
- [ ] Add `STATUS_INVALID_IMAGE_HASH` + `STATUS_IMAGE_CERT_REVOKED` (consumed by the blocked enforcement path; land with it). -> XREF: T19 §2.
- [ ] Include CI decision in loaded-image registry. -> XREF: TODO-18 §9 (provenance CI decision field).
- [ ] Commit: `"kernel: ci -- user-mode image enforcement"`

**Test checkpoint:** an unsigned user app runs in audit mode but an unsigned elevated/system process is blocked with `STATUS_INVALID_IMAGE_HASH`; a revoked cert yields `STATUS_IMAGE_CERT_REVOKED`; the CI decision appears in the loaded-image registry. Test on: QEMU WHPX + TCG; bare metal.
>
> **Deferred:** [H] User-mode image enforcement is blocked on the §2 loader enforcement wiring (no loader calls `ci_validate_image`) + §4 embedded-signature validation for the real ALLOW path. -> XREF: 02-kernel-core/TODO-19 §2 (item: "Loaders call CI before mapping" at line 93); 02-kernel-core/TODO-19 §4 (item: "EIF sig-block ABI decision (prereq)" at line 139)

---

## 10. CI Audit, Telemetry, and Syscalls

- [x] `ci_validate_image` already emits a klog audit line per decision (allow/deny reason + mode). The tamper-evident HMAC-chained log emission is deferred. -> XREF: TODO-04 §10 ("Log integrity verification (HMAC)").
- [/] Add `NtQuerySystemInformation(SystemCodeIntegrityInformation)`. BLOCKED on the SSDT/syscall surface. -> XREF: T12, T16.
- [/] `ci_dump_policy()` for KD/crash dump -- implementable now (dumps the §1 sealed policy via klog); the crash-dump surfacing needs the KD path. -> XREF: T19 §1.
- [/] Add `NtSetSystemInformation(SystemCodeIntegrityPolicyInformation)` for authorized policy refresh (revocation update); reject downgrades after lock. BLOCKED on the mutable/refresh policy store + SSDT. -> XREF: T12.
- [ ] Commit: `"kernel: ci -- audit, telemetry, and syscalls"`

**Test checkpoint:** `NtQuerySystemInformation(SystemCodeIntegrityInformation)` returns the current mode + flags; a deny decision is published via klog/ETW; `ci_dump_policy()` output appears in a crash dump; a post-lock policy downgrade via `NtSetSystemInformation` is rejected. Test on: QEMU WHPX + TCG; bare metal.
>
> **Deferred:** [H] CI audit/syscalls are blocked on the SSDT/syscall surface (NtQuery/NtSet) + the tamper-evident HMAC log; per-decision klog audit already ships in §2. `ci_dump_policy()` is the one near-term-implementable item (dumps the §1 policy). -> XREF: 02-kernel-core/TODO-19 §1 (item: "Wire `ci_init` at `POLICY_PHASE_POST_REGISTRY`" at line 61); 04-drivers-hardware/TODO-04 §10 (HMAC log integrity)

---

## 11. Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)

Items moved here VERBATIM from their original, already-stamped sections, where they were unreachable: the triage oracle classifies a stamped section DONE without reading its body, so an item appended after the stamp is invisible to every later pass. Source section noted per group. Cohort context: `todo/overnight-runner-improvements/overnight-runner-improvements-v05.md` item 3.

From the stamped section 1:
- [ ] Wire `ci_init` at `POLICY_PHASE_POST_REGISTRY` (boot_desktop.c Phase 3) + add `SUBSYS_CI`; the module is unwired today so accessors fail closed until called.
- [ ] Authenticate the policy artifact's own signature against a compiled-in Ed25519 policy-root key + key epoch (non-circular); `crypto_ed25519_check` is available.
- [ ] Persistent CI-policy version floor (anti-rollback): reuse the `boot_rollback.c` mechanism but a DISTINCT counter (NOT `IPOSRequiredSecVersion`); fail-closed reads, steady-boot advance.
- [ ] Ratchet-domain merge: policy_lock caps ci.mode at 2 + collapses SB to a bool (vs CI UNKNOWN->ENFORCE / ACTIVE->SECUREBOOT). Wiring the ratchet needs a shared tri-state SB resolver + full ci_enforcement_t mode domain.
From the stamped section 2:
- [ ] Gate dynamic-code sites: `NtAllocateVirtualMemory` (PAGE_EXECUTE) + `NtProtectVirtualMemory` (non-exec->exec) in src/kernel/nt/nt_memory.c must call `ci_validate_dynamic_code` before changing PTEs. -> XREF: T19 §4.

**Test checkpoint:** per moved item; each carries its original acceptance text.

---

## OS Comparison

| ⭐  | Feature                    | 🪟 Win11                          | 🐧 Linux                       | 🚀 Impossible OS                       |
| --- | -------------------------- | --------------------------------- | ------------------------------ | -------------------------------------- |
| 💎  | Kernel CI engine           | ✅ ci.dll (CI/WDAC)               | ✅ IMA/EVM appraisal           | 🔄 §1-§2 ci_validate_image + policy    |
| 💎  | Enforce/audit mode split   | ✅ WDAC audit vs enforce          | ✅ IMA log vs enforce          | 🔄 §1 explicit mode enum               |
| 💎  | Embedded signatures        | ✅ Authenticode                   | ✅ PE/module appended sig      | 🔄 §4 EIF trailer + PE Authenticode    |
| 💎  | Catalog signatures         | ✅ .cat catalog store             | ⚠️ IMA sig files               | 🔄 §5 catalog DB (hash->signer)        |
| 💎  | Revocation                 | ✅ CRL / dbx / revoked hashes     | ⚠️ manual keyring revoke       | 🔄 §6 revoked-hash + signer deny       |
| 💎  | Measured/Secure Boot bind  | ✅ HVCI + PCR policy binding      | ✅ IMA + TPM PCR               | 🔄 §7 policy digest -> PCR + SB refuse |
| 💎  | Driver signing enforce     | ✅ WHQL / boot-start classes      | ✅ CONFIG_MODULE_SIG           | 🔄 §8 signer-class + boot hash table   |
| ⭐  | Native Ed25519 signing     | ❌ RSA/ECDSA Authenticode         | ⚠️ RSA module sig              | ✅ §3 ci_crypto_verify PureEd25519     |
| ⭐  | CI decision in image reg   | ⚠️ separate CI state              | ❌ none unified                | 🔄 §9 -> TODO-18 §9 provenance field   |
| ⭐  | CI query syscall           | ✅ SystemCodeIntegrityInformation | ⚠️ /sys/kernel/security/ima    | 🔄 §10 NtQuery/NtSet + ci_dump_policy  |
| 💎  | Per-page hash on demand    | ✅ Authenticode page hashes       | ✅ fs-verity Merkle            | 🔄 §4 validate each paged-in page      |
| 💎  | Layered trust anchors      | ⚠️ cert-store roots               | ✅ 4-tier keyring              | 🔄 §1 role-separated anchor tiers      |
| 💎  | Signed/versioned policy    | ✅ signed WDAC policy + rollback  | ⚠️ keyring, no policy artifact | 🔄 §1 authenticate policy provenance   |
| 💎  | Measurement-only mode      | ⚠️ audit only                     | ✅ IMA measure vs appraise     | 🔄 §1 measure mode -> PCR aggregate    |
| ⭐  | Dynamic/JIT code admission | ✅ dynamic code policy (.NET)     | ⚠️ W^X, no CI hook             | 🔄 §2 ci_validate_dynamic_code         |
| ⭐  | Policy self-protection     | ✅ HVCI VTL-isolated CI           | ⚠️ lockdown, same ring         | 🔄 §1 RO-after-lock policy page        |
| ⭐  | Tamper-evident CI audit    | ⚠️ ETW (mutable)                  | ⚠️ audit log (mutable)         | 🔄 §10 -> T04 §10 HMAC-chain           |

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
