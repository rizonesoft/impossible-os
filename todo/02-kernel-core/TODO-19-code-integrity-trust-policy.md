# TODO-19 -- Code Integrity & Trust Policy

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

| ⭐ | Order | Deliverable | Depends On | Status |
| -- | :---: | ----------- | ---------- | :----: |
| 💎 | 1 | Code Integrity policy object | T02 | [ ] |
| 💎 | 2 | Image validation API and callback | T17 | [ ] |
| 💎 | 3 | Hashing and signature provider bridge | T03 | [ ] |
| 💎 | 4 | Embedded signature validation | §2, §3 | [ ] |
| 💎 | 5 | Catalog database | Registry/VFS | [ ] |
| 💎 | 6 | Revocation and deny lists | §5 | [ ] |
| ⭐ | 7 | Measured-boot and Secure Boot binding | TPM | [ ] |
| 💎 | 8 | Driver/module enforcement | T18, D04 | [ ] |
| 💎 | 9 | User-mode image enforcement | T17, T20 | [ ] |
| ⭐ | 10 | CI audit, telemetry, and syscalls | T12, T16 | [ ] |

## 1. Code Integrity Policy Object

- [ ] Define `ci_policy_t`: mode, secure boot state, test-signing flag, allowed roots, denied roots, revoked hashes, audit flags.
- [ ] Load defaults from compiled policy; merge boot/registry policy via TODO-02.
- [ ] Lock policy after Phase 2 on Secure Boot systems.
- [ ] Deny attempts to downgrade enforcement after lock.

## 2. Image Validation API and Callback

- [ ] Add `ci_validate_image(ci_image_info_t *image, ci_decision_t *decision)`.
- [ ] Image info includes path, format, type, signer, hash, requested trust level, loader, process token, and measured-boot context.
- [ ] Register Executive callback `\Callback\CiImageLoad` through TODO-17.
- [ ] All loaders must call CI before mapping executable pages.

## 3. Hashing and Signature Provider Bridge

- [ ] Implement SHA-256/BLAKE2b digest bridge using kernel crypto provider.
- [ ] Support Ed25519 as native Impossible OS signing algorithm.
- [ ] Keep RSA/X.509 catalog support optional until CNG provider matures.
- [ ] Ensure validation is allocation-bounded and safe before user-mode starts.

## 4. Embedded Signature Validation

- [ ] EIF: validate signed metadata/trailer before segment mapping.
- [ ] PE: parse WIN_CERTIFICATE and Authenticode digest exclusions.
- [ ] ELF: support Impossible OS note-section signature for native ELF tools.
- [ ] Reject malformed or ambiguous signed ranges.

## 5. Catalog Database

- [ ] Load catalog files from `C:\Impossible\System\Catalogs\`.
- [ ] Catalog maps image hash to signer, policy OID/equivalent, timestamp, and allowed image type.
- [ ] Catalog cache is indexed by hash and protected by ERESOURCE.
- [ ] Add recovery path if catalogs are missing in Safe Mode.

## 6. Revocation and Deny Lists

- [ ] Registry-backed revoked hash list and signer deny list.
- [ ] Optional catalog-delivered revocation bundles.
- [ ] Revocation always overrides allow, including test-signing unless boot explicitly enters lab mode.
- [ ] Publish notification on revocation database update.

## 7. Measured-Boot and Secure Boot Binding

- [ ] Bind CI policy to Secure Boot state reported by boot platform.
- [ ] Record policy digest in TPM PCR event log when TPM is present.
- [ ] Refuse unsigned kernel drivers when Secure Boot is enabled.
- [ ] Audit mismatch between bootloader trust state and kernel policy state.

## 8. Driver/Module Enforcement

- [ ] Validate built-in drivers at boot by image hash table.
- [ ] Validate `.kmod` and driver images before relocation/load.
- [ ] Enforce signer class: kernel, boot-start driver, normal driver, test driver.
- [ ] Block unsigned executable memory allocation for driver code.

## 9. User-Mode Image Enforcement

- [ ] Apply configurable policy by path, signer, zone, and token integrity level.
- [ ] Allow audit-only mode for unsigned user applications while blocking unsigned elevated/system processes.
- [ ] Add `STATUS_INVALID_IMAGE_HASH` and `STATUS_IMAGE_CERT_REVOKED` returns.
- [ ] Include CI decision in loaded-image registry.

## 10. CI Audit, Telemetry, and Syscalls

- [ ] Publish CI allow/deny/audit events through TODO-27 and klog/ETW.
- [ ] Add `NtQuerySystemInformation(SystemCodeIntegrityInformation)`.
- [ ] Add `ci_dump_policy()` for KD/crash dump.
- [ ] Tests: valid signed image, unsigned audit, unsigned enforce deny, revoked hash deny, Secure Boot policy lock.

