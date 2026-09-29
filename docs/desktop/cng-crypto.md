<!-- docs: covers=todo/09-desktop-shell/TODO-07-cng-crypto.md sources=include/kernel/crypto/sha256.h,src/kernel/crypto/sha256.c,include/kernel/crypto/hash.h,include/kernel/ci/ci_crypto.h,src/kernel/ci/ci_crypto.c,src/libs/monocypher/monocypher.h,src/libs/monocypher/monocypher-ed25519.h,src/kernel/fs/ntfs/ntfs_efs.c,src/libs/PROVENANCE.md,Makefile reviewed=2026-09-29 order=13 -->
# CNG Crypto and Certificate Store

## What is it?

CNG (Cryptography API: Next Generation) is the Windows programming interface for encryption, hashing, signatures and key storage. This roadmap plans a CNG-shaped crypto layer in the kernel: AES-256-GCM, SHA-256, HMAC and RSA-2048 alongside the existing curve primitives, TLS helpers, an X.509 certificate store with a CA bundle, a per-user key store, `bcrypt.dll` and `ncrypt.dll` entry points, Ed25519 code signing for programs, and NTFS encrypted files (EFS). None of its eight sections has shipped, but a good part of the primitive layer already exists under other owners.

## How does it work?

**Today.** Crypto code is spread across three places:

- **Kernel hashes.** SHA-256, SHA-1, SHA-384 and SHA-3 are implemented in `src/kernel/crypto/` ([`sha256.h`](../../include/kernel/crypto/sha256.h)), with a selector, `crypto_hash()`, over SHA-256, SHA-384 and the SHA-3 sizes ([`hash.h`](../../include/kernel/crypto/hash.h)). The TPM, feature and code-integrity code use them.
- **Monocypher.** The vendored and built Monocypher 4.0.2 provides ChaCha20-Poly1305 authenticated encryption, BLAKE2b, X25519, Argon2 and EdDSA ([`monocypher.h`](../../src/libs/monocypher/monocypher.h)), and its Ed25519 unit adds standard Ed25519, SHA-512, HMAC-SHA-512 and HKDF ([`monocypher-ed25519.h`](../../src/libs/monocypher/monocypher-ed25519.h)).
- **Code integrity.** The code-integrity provider computes SHA-256, SHA-384 or BLAKE2b-256 digests (`ci_crypto_digest()`) and, separately, verifies PureEd25519 signatures (`ci_crypto_verify()`). Verification takes the raw message bytes, not a digest: the signer signs the message itself and Ed25519 hashes it internally. Its RSA PKCS#1 slot is reserved and always refuses ([`ci_crypto.c`](../../src/kernel/ci/ci_crypto.c), [`ci_crypto.h`](../../include/kernel/ci/ci_crypto.h)). See [Code Integrity and Trust Policy](../kernel/code-integrity-trust-policy.md).

What is missing is exactly what this roadmap names: there is no AES, no HMAC-SHA-256, no RSA, no X.509 parser, no certificate or key store and no CNG entry point. Mbed TLS 3.6.2, which contains AES, GCM, RSA, big numbers and an X.509 parser, is vendored but excluded from the build until its freestanding port lands ([`PROVENANCE.md`](../../src/libs/PROVENANCE.md), [`Makefile`](../../Makefile)); see [HTTP and TLS](../networking/http-tls.md). The roadmap should build on that port rather than add a second RSA and X.509 implementation.

**EFS.** The NTFS driver already parses the Windows `$EFS` stream (the data decryption field entries with their certificate thumbprints) and routes reads and writes of encrypted files through it ([`ntfs_efs.c`](../../src/kernel/fs/ntfs/ntfs_efs.c)). Unwrapping the file key and the AES work are stubs that return "not ready", so an encrypted file cannot be read yet ([NTFS](../storage/ntfs.md)).

**Planned design.**

1. **Primitives.** `cng_*` wrappers for AES-256-GCM, SHA-256, SHA-1, HMAC-SHA-256, RSA-2048, X25519 and BLAKE2b, plus a small verify subset for the bootloader.
2. **TLS helper.** The TLS 1.2 PRF and RSA PKCS#1 v1.5 sign and verify.
3. **Certificate store.** A DER X.509 parser, certificates in the Registry, the Mozilla CA bundle and chain verification.
4. **Key store.** Per-user keys encrypted with AES-256-GCM under `HKCU\SECURITY\Keys`.
5. **BCrypt** and 6. **NCrypt.** Win32 `BCrypt*` and `NCrypt*` functions over the layers above.
7. **Code signing.** An Ed25519 signature trailer on PE32+ files, enforceable through the Registry.
8. **EFS.** Transparent file encryption with an RSA-wrapped file key.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `sha256()`, `sha1()`, `crypto_hash()` | Shipped |
| `crypto_aead_lock()`, `crypto_x25519()`, `crypto_blake2b()`, `crypto_ed25519_sign()` | Shipped (Monocypher) |
| `ci_crypto_digest()`, `ci_crypto_verify()` | Shipped: three digests; PureEd25519 verify over raw message bytes |
| `ntfs_efs_parse()` | Shipped parser; key unwrap and AES are stubs |
| `cng_*`, `BCrypt*`, `NCrypt*`, certificate store, code signing | Planned |

## How do I use it?

It cannot be used by programs yet. The kernel uses the existing hashes and Ed25519 internally for measured boot and code integrity.

## What is not implemented yet?

- [Crypto Primitives](../../todo/09-desktop-shell/TODO-07-cng-crypto.md#1-crypto-primitives-opus)
- [TLS Handshake Helper](../../todo/09-desktop-shell/TODO-07-cng-crypto.md#2-tls-handshake-helper-sonnet)
- [X.509 Certificate Store](../../todo/09-desktop-shell/TODO-07-cng-crypto.md#3-x509-certificate-store-opus), overlapping the [CA certificate store](../../todo/07-networking/TODO-03-http-tls.md#6-ca-certificate-store-sonnet) in the HTTP and TLS roadmap
- [Key Store](../../todo/09-desktop-shell/TODO-07-cng-crypto.md#4-key-store-sonnet)
- [BCrypt API](../../todo/09-desktop-shell/TODO-07-cng-crypto.md#5-bcrypt-api-sonnet) and [NCrypt API](../../todo/09-desktop-shell/TODO-07-cng-crypto.md#6-ncrypt-api-sonnet)
- [Code Signing](../../todo/09-desktop-shell/TODO-07-cng-crypto.md#7-code-signing-opus)
- [EFS Integration](../../todo/09-desktop-shell/TODO-07-cng-crypto.md#8-efs-integration-opus), which has to build on the existing `$EFS` parser rather than define a new on-disk format

## How does it compare with Windows 11 and Linux?

Windows 11 has CNG with AES-NI acceleration, SChannel for TLS, the certificate store, DPAPI and key storage providers including the TPM, Authenticode signing with RSA and X.509, and EFS. Linux has the kernel crypto subsystem, OpenSSL or similar libraries, the `ca-certificates` bundle, keyrings, IMA and EVM for signed files, and fscrypt or dm-crypt for encryption. Impossible OS has hashes, curve cryptography and Ed25519 verification today, but no Windows-compatible API. The plan chooses Ed25519 for code signing because it is smaller and faster than RSA.

## See also

- [CNG Crypto and Certificate Store roadmap](../../todo/09-desktop-shell/TODO-07-cng-crypto.md)
- [Kernel Libraries](../kernel/kernel-libraries.md)
- [HTTP and TLS](../networking/http-tls.md)
- [Code Integrity and Trust Policy](../kernel/code-integrity-trust-policy.md)
- [NTFS](../storage/ntfs.md)
- [Security and User Accounts](security-accounts.md)
