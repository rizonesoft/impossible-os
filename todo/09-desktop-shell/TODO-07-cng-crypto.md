---
schema_version: 1
id: cng-crypto
domain: 09-desktop-shell
status: active
title: "TODO-07 -- CNG Crypto & Certificate Store"
---

# TODO-07 -- CNG Crypto & Certificate Store

> **Goal:** Build the full Windows CNG (Cryptography Next Generation) API surface on top of monocypher (TODO-06): AES-256-GCM, SHA-256, HMAC-SHA256, RSA-2048, ECDH X25519 primitives, a minimal X.509 DER certificate store with chain verification, a per-user encrypted key store, Windows-compatible BCrypt/NCrypt stub tables, transparent NTFS EFS file encryption, PE32+ code signing, and a TLS PRF helper consumed by the Mbed TLS network stack.

> [!IMPORTANT]
> **Already exists**: `crypto_blake2b()` + `crypto_argon2()` + `crypto_x25519()` + `crypto_ed25519_*()` in the vendored Monocypher 4.0.2 (`src/libs/monocypher/`). `csprng_fill()` / `csprng_u64()` in `include/kernel/csprng.h` (kernel CSPRNG). SHA-256, SHA-1, SHA-384, SHA-3 and `crypto_hash()` in `include/kernel/crypto/`. Ed25519 verify in `include/kernel/ci/ci_crypto.h`. `kmalloc/kfree`, `pmm_alloc_contiguous`. `uefi_runtime.h` has `CRYPTO_IND_RSA_*` constants for Secure Boot -- these are for UEFI, not runtime CNG; do not reuse. No `HKLM\SECURITY` hive exists yet (HKLM has SYSTEM, SOFTWARE, HARDWARE); §3 and §7 create their keys. `vfs_open/read/write` for file encryption. NTFS already parses the Windows `$EFS` stream and routes encrypted reads and writes through `src/kernel/fs/ntfs/ntfs_efs.c`, whose key unwrap and AES calls are stubs -- §8 fills those stubs rather than defining a new format. `08-graphics-ui/TODO-11` establishes the `kernel32` stub table pattern; BCrypt/NCrypt follow the same stub table approach. **Missing**: AES-256-GCM (monocypher is ChaCha20-Poly1305 only), HMAC-SHA256, RSA-2048, X.509, all CNG structs and APIs. Mbed TLS 3.6.2 (vendored at `src/libs/mbedtls/`, not built; port owned by 02-kernel-core/TODO-03 §7) carries AES, GCM, RSA and X.509, so §1 and §3 should wrap it rather than add a second implementation. **monocypher provides**: `crypto_x25519()` for §1 ECDH Curve25519; `crypto_ed25519_sign/check()` for §7 code signing alternative; `crypto_blake2b()` for §1 BLAKE2b-256. Complete sections in order: primitives → TLS helper → cert store → key store → BCrypt → NCrypt → code signing → EFS.

## Inputs

- `include/libs/monocypher.h` (TODO-06 §2) -- `crypto_x25519`, `crypto_blake2b`, `crypto_ed25519_*` -- reused in §1 ECDH and §7 code signing
- `include/kernel/csprng.h` (TODO-06 §11) -- `csprng_read()` -- used by §1 keygen and §3 key store nonces
- `include/kernel/mm/heap.h` -- `kmalloc/kfree` -- used throughout for struct allocation
- `include/registry.h` -- `HKLM\SECURITY\Certificates\*`, `HKCU\SECURITY\Keys\*` -- §2 cert store, §3 key store
- `include/kernel/fs/vfs.h` -- `vfs_open/read/write` -- §6 EFS file data path
- `include/kernel/fs/ntfs_internal.h` -- NTFS inode attributes -- §6 EFS `$EFS` attribute attachment
- `include/kernel/auth.h` (TODO-06 §2) -- `auth_get_current_uid()`, `auth_hash_password()` -- §3 key store KEK derivation
- `include/kernel/klog.h` -- `klog()` -- throughout
- → XREF: `07-networking/TODO-03-http-tls.md` -- Mbed TLS integration calls `cng_tls_prf()` (§8) and `cng_rsa_pkcs1_sign/verify()` for TLS handshake; §8 is consumed there
- → XREF: `07-networking/TODO-03-http-tls.md §6` -- Mozilla CA bundle loaded at boot used to seed §2 cert store root CAs
- → XREF: `05-storage-filesystems/TODO-02-ntfs-readwrite.md` -- §6 EFS integration hooks into the NTFS data path; EFS `$EFS` attribute parsing lives in NTFS driver
- → XREF: `08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md` -- establishes `kernel32` stub table; `bcrypt.dll` (§4) and `ncrypt.dll` (§5) follow the same stub table pattern
- → XREF: `09-desktop-shell/TODO-06-security-accounts.md §2` -- monocypher and `csprng_read()` must be complete before §1 starts

## Outcome

- AES-256-GCM encrypt/decrypt; SHA-256; HMAC-SHA256; ECDH X25519; RSA-2048 keygen/encrypt/decrypt; BLAKE2b-256.
- Minimal DER X.509 parser; kernel cert store in Registry; Mozilla CA root bundle loaded at boot; chain verification.
- Per-user AES-256-GCM encrypted key store in `HKCU\SECURITY\Keys\*`.
- `BCryptOpenAlgorithmProvider/Encrypt/Hash/GenRandom` wired to CNG primitives; registered in `bcrypt.dll` stub table.
- `NCryptOpenStorageProvider/OpenKey/Encrypt/ImportKey` wired to key store; `ncrypt.dll` stub table.
- `efs_encrypt/decrypt_file()` wraps AES-256-GCM + RSA key wrap; transparent in NTFS data path.
- `codesign_sign/verify()` on PE32+ binaries; optional enforcement via Registry flag.
- `cng_tls_prf()` + `cng_rsa_pkcs1_sign/verify()` consumed by Mbed TLS (TODO-03).

## Implementation Order

| ⭐  | Order | Deliverable                                                                                                          | Depends On                                                                                                                                         | Status |
| --- | :---: | -------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- | :----: |
| ⭐  |   1   | §1 Crypto primitives -- `cng_*` over new AES-256-GCM, HMAC-SHA256, RSA-2048 and the shipped SHA-256, X25519, BLAKE2b | monocypher (vendored, built); `csprng_fill()` (kernel CSPRNG); `crypto/sha256.c` (exists); Mbed TLS port (02-kernel-core/TODO-03 §7)               |  [ ]   |
| 💎  |   2   | §2 TLS handshake helper -- `cng_tls_prf()`, `cng_rsa_pkcs1_sign/verify()`                                            | §1 primitives (SHA-256, RSA); used by 07-networking/TODO-03 Mbed TLS                                                                               |  [ ]   |
| ⭐  |   3   | §3 X.509 cert store -- DER parser, Registry store, CA bundle, chain verification                                     | §1 SHA-256 (thumbprint), §1 RSA (chain verify); Registry (exists)                                                                                  |  [ ]   |
| ⭐  |   4   | §4 Key store -- per-user AES-256-GCM encrypted key storage in `HKCU\SECURITY\Keys\*`                                 | §1 AES-256-GCM; §3 cert store; `auth_get_current_uid()` (TODO-06)                                                                                  |  [ ]   |
| 💎  |   5   | §5 BCrypt API -- `BCryptOpenAlgorithmProvider/Encrypt/Hash/GenRandom`; `bcrypt.dll` stub table                       | §1 all primitives; `csprng_fill()` (exists)                                                                                                        |  [ ]   |
| 💎  |   6   | §6 NCrypt API -- `NCryptOpenStorageProvider/OpenKey/Encrypt/Import`; `ncrypt.dll` stub table                         | §4 key store; §5 BCrypt                                                                                                                            |  [ ]   |
| ⭐  |   7   | §7 Code signing -- `codesign_sign/verify()` on PE32+; optional enforcement via Registry                              | §1 RSA/Ed25519; §3 cert store; PE32+ format (02-kernel-core/TODO-17 binary system)                                                                 |  [ ]   |
| ⭐  |   8   | §8 EFS integration -- `efs_encrypt/decrypt_file()`, `$EFS` NTFS attribute, transparent data path                     | §1 AES-256 sector mode (CBC or XTS) + RSA-OAEP unwrap, the contract `ntfs_efs.c` stubs specify; §4 key store; NTFS driver (TODO-02-ntfs-readwrite) |  [ ]   |

---

## 1. Crypto Primitives `[Opus]`

`cng_*` wrappers: AES-256-GCM and RSA-2048 over the vendored Mbed TLS 3.6.2 (`src/libs/mbedtls/`, freestanding port owned by 02-kernel-core/TODO-03 §7); SHA-256 and SHA-1 over the shipped `src/kernel/crypto/`; HMAC-SHA256 over the shipped SHA-256; BLAKE2b-256 and ECDH Curve25519 via monocypher.

**Files:** `src/kernel/cng/cng_primitives.c` (new), `include/kernel/cng/cng_primitives.h` (new), `src/libs/mbedtls/` (vendored; build port per 02-kernel-core/TODO-03 §7)

> [!NOTE]
> **Superseded 2026-09-29:** the tiny-AES-c, hand-written SHA-256 and mini-RSA plan in this note is replaced by wrappers over Mbed TLS and the shipped kernel hashes (vendor-first, see the preamble); the `cng_*` API shapes below stand. `[Opus]` due to: security-critical cryptography (every misuse leaks secrets), RSA modular exponentiation design (novel, no prior Impossible OS precedent), and AES-GCM integration combining two separate algorithm components. **AES-256-GCM**: vendor `tiny-AES-c` (MIT, ~200 lines, `src/libs/tiny_aes/aes.c`) for AES-256-ECB block cipher; implement GCM mode manually (`cng_gcm_encrypt/decrypt`): GHASH via 128-bit carry-less multiply (pure C via `uint64_t` split); Counter Mode (CTR) using AES-ECB block; authenticate ciphertext with GHASH; return tag in `tag_out[16]`; verify tag on decrypt with `cng_consttime_compare(tag, expected, 16)` -- returning -1 on mismatch without revealing which bytes differ. **SHA-256**: vendor a single-file public-domain SHA-256 (e.g., `src/libs/sha256/sha256.c`, 250 lines, no libc); same freestanding compile flags + `libc_shim.h`. **HMAC-SHA256**: implement over SHA-256 directly (inner+outer padding pattern, 64-byte block). **ECDH X25519**: `cng_ecdh_curve25519(priv[32], pub[32], shared_out[32])` = `crypto_x25519(shared_out, priv, pub)` -- thin wrapper. **RSA-2048**: vendor `LibTomMath` or a ~1000-line mini-RSA (big-integer modular exponentiation for PKCS#1 v1.5 2048-bit). RSA work-area via `pmm_alloc_contiguous()`. `cng_rsa2048_keygen(pub_out, priv_out)`: generate two 1024-bit primes using Miller-Rabin primality test seeded from `csprng_read()`.

- [ ] Depend on the Mbed TLS freestanding port (02-kernel-core/TODO-03 §7) for AES, GCM and RSA; no second AES or RSA implementation
- [ ] EFS primitives for §8: AES-256 sector mode (CBC or XTS, per the Windows fixture) and RSA-OAEP decrypt/encrypt, both over Mbed TLS
- [ ] `int cng_aes256gcm_encrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *plain, size_t plen, uint8_t *cipher_out, uint8_t tag_out[16])` -- AES-CTR + GHASH
- [ ] `int cng_aes256gcm_decrypt(...)` -- verify GHASH tag first (constant-time); decrypt; return -1 on tag mismatch
- [x] SHA-256 source: already shipped as `sha256()` in `include/kernel/crypto/sha256.h` (NIST vectors in `test_sha256.c`); `cng_sha256()` wraps it
- [ ] `void cng_sha256(const uint8_t *data, size_t len, uint8_t hash_out[32])`
- [ ] `void cng_hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *data, size_t dlen, uint8_t mac_out[32])`
- [ ] `void cng_blake2b256(const uint8_t *data, size_t len, uint8_t hash_out[32])` -- thin wrapper: `crypto_blake2b(out, 32, data, len)`
- [ ] `void cng_ecdh_curve25519(const uint8_t priv[32], const uint8_t pub[32], uint8_t shared_out[32])` -- `crypto_x25519()` wrapper
- [ ] `int cng_rsa2048_keygen(uint8_t pub_out[256], uint8_t priv_out[512])` -- over Mbed TLS RSA; seeded from `csprng_fill()`, refused unless `csprng_crypto_ok()`
- [ ] `int cng_rsa2048_encrypt(const uint8_t pub[256], const uint8_t *plain, size_t plen, uint8_t cipher_out[256])` -- PKCS#1 v1.5 padding
- [ ] `int cng_rsa2048_decrypt(const uint8_t priv[512], const uint8_t cipher[256], uint8_t *plain_out, size_t *plen_out)` -- PKCS#1 v1.5 unpad; constant-time
- [ ] `static int cng_consttime_compare(const uint8_t *a, const uint8_t *b, size_t n)` -- no early return; XOR-fold
- [ ] `void cng_sha1(const uint8_t *data, size_t len, uint8_t hash_out[20])` -- wraps the shipped `sha1()`
  - Needed by WIM file format (-> XREF: 15-installer-release/TODO-02 §5), Windows-compatible service SID derivation (-> XREF: 02-kernel-core/TODO-15 §16), and WPA2 PBKDF2 (-> XREF: 04-drivers-hardware/TODO-15 §5)
- [ ] Bootloader verify subset callable from `bootx64.c` pre-EBS: SHA-256 already exists header-only in `include/boot/sha256_boot.h`; add `boot_ed25519_verify()`; bootloader cannot link kernel `cng_*`. Consumer: 01-boot-platform/TODO-06 §7 items 1-3.
- [ ] Boot log: `klog(LOG_OK, "cng", "primitives ready: AES-256-GCM SHA-256 SHA-1 HMAC RSA-2048 X25519 BLAKE2b")`
- [ ] Commit: `"cng: crypto primitives -- AES-256-GCM, SHA-256, SHA-1, HMAC, RSA-2048, X25519, BLAKE2b"`

## 2. TLS Handshake Helper `[Sonnet]`

`cng_tls_prf(secret, label, seed, out, len)` HMAC-SHA256-based PRF. `cng_rsa_pkcs1_sign(priv, data, dlen, sig_out)`, `cng_rsa_pkcs1_verify(pub, data, dlen, sig)`. Consumed by Mbed TLS integration in TODO-03.

**Files:** `src/kernel/cng/cng_tls.c` (new), `include/kernel/cng/cng_tls.h` (new)

> [!NOTE]
> TLS 1.2 PRF (RFC 5246 §5): `P_hash(secret, label || seed)` via iterative HMAC-SHA256. `cng_tls_prf(secret, slen, label, lseed, seed, out, len)`: `A(0) = label||seed`; `A(i) = HMAC_SHA256(secret, A(i-1))`; output chunk = `HMAC_SHA256(secret, A(i) || label||seed)`; concatenate until `len` bytes produced. `cng_rsa_pkcs1_sign(priv[512], data[32], dlen, sig_out[256])`: DigestInfo DER prefix for SHA-256 (`30 31 30 0d 06 09 60...`) + hash → PKCS#1 v1.5 pad → RSA private-key operation. `cng_rsa_pkcs1_verify(pub[256], data, dlen, sig[256])`: RSA public-key operation → strip PKCS#1 pad → compare DigestInfo + hash. These are the minimum surface Mbed TLS needs to call into the CNG layer.

- [ ] `void cng_tls_prf(const uint8_t *secret, size_t slen, const char *label, const uint8_t *seed, size_t seedlen, uint8_t *out, size_t len)` -- HMAC-SHA256 P_hash per RFC 5246
- [ ] `int cng_rsa_pkcs1_sign(const uint8_t priv[512], const uint8_t *data, size_t dlen, uint8_t sig_out[256])` -- SHA-256 DigestInfo + PKCS#1 v1.5 + RSA private op
- [ ] `int cng_rsa_pkcs1_verify(const uint8_t pub[256], const uint8_t *data, size_t dlen, const uint8_t sig[256])` -- RSA public op + PKCS#1 unpad + hash compare
- [ ] `cng_sha256_digest_info[19]` static DER prefix constant for SHA-256 AlgorithmIdentifier
- [ ] Commit: `"cng: TLS helper -- cng_tls_prf (RFC 5246 P_hash), RSA PKCS#1 v1.5 sign/verify"`

## 3. X.509 Certificate Store `[Opus]`

`struct x509_cert` (subject, issuer, public_key, validity, DER buffer). `cng_cert_parse_der()` over Mbed TLS `x509_crt` (vendored), shared with the 07-networking/TODO-03 §6 CA store rather than a second parser. `cng_cert_store_add/find/verify_chain()`. Mozilla CA bundle loaded at boot.

**Files:** `src/kernel/cng/cng_cert.c` (new), `include/kernel/cng/cng_cert.h` (new), `resources/ca-bundle.der` (embedded)

> [!NOTE]
> `[Opus]` due to: ASN.1/DER parsing (novel format, no prior parser), X.509 chain verification (security-critical: incorrect implementation breaks TLS), and embedded CA bundle integration. **DER parser scope**: parse TBSCertificate for `subject` (CN), `issuer` (CN), `subjectPublicKeyInfo` (RSA public key bytes), `validity` (notBefore/notAfter as `int64_t` UTC seconds), `serialNumber`. Skip unrecognized extensions without error (only fail on critical extensions with `OID` we cannot parse). `cng_cert_parse_der(buf, len, cert_out)` fills `x509_cert_t`; stores DER buffer pointer (not copied -- caller owns memory). **Thumbprint**: `cng_sha256(DER_buf, DER_len, thumbprint)`. Registry store: `HKLM\SECURITY\Certificates\{thumbprint_hex}\DER` (binary value), `\Subject`, `\Issuer`, `\Expiry`. **Chain verification**: `cng_cert_verify_chain(leaf, store)` -- find issuer in store by `leaf->issuer == candidate->subject`; verify `cng_rsa_pkcs1_verify(issuer->pub_key, leaf->tbscert_der, tbscert_len, leaf->signature)`; recurse to root; root must be self-signed AND present in store. **CA bundle**: embed Mozilla's CA bundle as a binary DER sequence at `resources/ca-bundle.der`; `cng_cert_store_load_bundle()` called from `kernel_main()` after CNG init; parses sequence and calls `cng_cert_store_add()` for each.

- [ ] `typedef struct x509_cert { char subject_cn[64]; char issuer_cn[64]; uint8_t pub_key[256]; int64_t not_before; int64_t not_after; uint8_t thumbprint[32]; const uint8_t *der_buf; size_t der_len; uint8_t signature[256]; } x509_cert_t;`
- [ ] `int cng_cert_parse_der(const uint8_t *buf, size_t len, x509_cert_t *out)` -- wraps `mbedtls_x509_crt_parse_der()`; fills subject/issuer/pubkey/validity
- [ ] `int cng_cert_store_add(const x509_cert_t *cert)` -- persist to `HKLM\SECURITY\Certificates\{thumbprint_hex}\*`
- [ ] `int cng_cert_store_find(const char *subject_cn, x509_cert_t *out)` -- scan Registry; parse DER; fill `out`
- [ ] `int cng_cert_verify_chain(const x509_cert_t *leaf, int max_depth)` -- recursive issuer lookup + RSA signature verify
- [ ] `void cng_cert_store_load_bundle(void)` -- load `resources/ca-bundle.der`; parse; bulk `cng_cert_store_add()`
- [ ] Validity check: `cng_cert_is_expired(cert)` -- compare `cert->not_after` to `time_now()` (08-graphics-ui/TODO-12 §1)
- [ ] Boot log: `klog(LOG_OK, "cng", "cert store: %u CA roots loaded", root_count)`
- [ ] Commit: `"cng: X.509 cert store -- DER parser, Registry store, CA bundle, chain verification"`

## 4. Key Store `[Sonnet]`

Per-user private key storage encrypted at rest with AES-256-GCM. `cng_key_store_import/get/delete`. Keys in `HKCU\SECURITY\Keys\{name}` encrypted with user password-derived KEK.

**Files:** `src/kernel/cng/cng_keystore.c` (new), `include/kernel/cng/cng_keystore.h` (new)

> [!NOTE]
> KEK (Key Encryption Key): derived from user password via `cng_hmac_sha256(user_password_hash[64], uid_bytes, kek[32])` -- not `csprng_read()`, so the same user password always derives the same KEK (deterministic). Key slot format in Registry (all under `HKCU\SECURITY\Keys\{name}\`): `Type` (RSA_PRIV/RSA_PUB/SYMMETRIC), `Nonce` (12 bytes hex), `EncryptedData` (AES-256-GCM ciphertext hex), `Tag` (16 bytes hex). `cng_key_store_import(name, key_bytes, klen, type)`: fresh `csprng_read(nonce, 12)`; `cng_aes256gcm_encrypt(kek, nonce, key_bytes, ...)` → write Registry. `cng_key_store_get(name, key_buf_out, klen_out)`: read Registry; `cng_aes256gcm_decrypt(kek, nonce, ...)` → fill output. `cng_key_store_delete(name)`: delete `HKCU\SECURITY\Keys\{name}` subtree.

- [ ] `#define CNG_KEY_TYPE_RSA_PRIV 0`, `CNG_KEY_TYPE_RSA_PUB 1`, `CNG_KEY_TYPE_SYMMETRIC 2`
- [ ] `void cng_keystore_get_kek(uint8_t kek_out[32])` -- `cng_hmac_sha256(password_hash, uid_bytes, kek_out)`; uses `auth_get_current_user()`
- [ ] `int cng_key_store_import(const char *name, const uint8_t *key, size_t klen, uint8_t type)` -- encrypt + Registry write
- [ ] `int cng_key_store_get(const char *name, uint8_t *key_out, size_t *klen_out)` -- Registry read + AES-GCM decrypt
- [ ] `int cng_key_store_delete(const char *name)` -- delete Registry subtree
- [ ] Commit: `"cng: key store -- per-user AES-256-GCM encrypted keys in HKCU\\SECURITY\\Keys, KEK derivation"`

## 5. BCrypt API `[Sonnet]`

`BCryptOpenAlgorithmProvider`, `BCryptEncrypt/Decrypt`, `BCryptHash/FinishHash`, `BCryptGenRandom`, `BCryptGenerateKeyPair`, `BCryptImportKeyPair`, `BCryptExportKey`. Registered in `bcrypt.dll` stub table.

**Files:** `src/kernel/cng/bcrypt_stubs.c` (new), `include/kernel/cng/bcrypt.h` (new)

> [!NOTE]
> Follow the same stub-table pattern as `kernel32.dll` in TODO-11 and Win32-GDI. `bcrypt.dll` stub table entries: each Win32 function name → function pointer. `BCRYPT_ALG_HANDLE`, `BCRYPT_KEY_HANDLE`, `BCRYPT_HASH_HANDLE` are opaque `uint32_t` handles (handle = index into a 64-slot `bcrypt_object_t` table). `BCryptOpenAlgorithmProvider(phAlgo, pszAlgId, ...)`: `pszAlgId` = `L"AES"` → AES-256-GCM slot; `L"SHA256"` → SHA-256 slot; `L"RNG"` → CSPRNG slot; `L"RSA"` → RSA-2048 slot. `BCryptEncrypt(hKey, pbInput, cbInput, pPaddingInfo, pbIV, cbIV, pbOutput, cbOutput, pcbResult, dwFlags)` → dispatch to `cng_aes256gcm_encrypt()`. `BCryptHash(hAlgo, ..., pbInput, cbInput, pbOutput, cbOutput)` → `cng_sha256()` or `cng_blake2b256()`. `BCryptGenRandom(hAlgo, pbBuffer, cbBuffer, dwFlags)` → `csprng_read()`. `BCryptGenerateKeyPair(hAlgo, phKey, dwLength, ...)` → `cng_rsa2048_keygen()` if hAlgo = RSA handle. Note: wide-char `pszAlgId` -- use a simple ASCII comparison after skipping leading null byte (PE32+ user-mode passes UTF-16LE, kernel stub accepts ASCII-cast).

- [ ] `typedef uint32_t BCRYPT_ALG_HANDLE; typedef uint32_t BCRYPT_KEY_HANDLE; typedef uint32_t BCRYPT_HASH_HANDLE;`
- [ ] 64-slot `bcrypt_object_t g_bcrypt_objects[64]` table with type tag
- [ ] `NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE *phAlgo, const char *pszAlgId, ...)` -- open algorithm slot
- [ ] `NTSTATUS BCryptEncrypt(BCRYPT_KEY_HANDLE hKey, ...)` → `cng_aes256gcm_encrypt()`
- [ ] `NTSTATUS BCryptDecrypt(BCRYPT_KEY_HANDLE hKey, ...)` → `cng_aes256gcm_decrypt()`
- [ ] `NTSTATUS BCryptHash(BCRYPT_ALG_HANDLE hAlgo, ...)` → `cng_sha256()` or `cng_blake2b256()`
- [ ] `NTSTATUS BCryptGenRandom(BCRYPT_ALG_HANDLE hAlgo, uint8_t *buf, uint32_t len, ...)` → `csprng_read(buf, len)`
- [ ] `NTSTATUS BCryptGenerateKeyPair(BCRYPT_ALG_HANDLE hAlgo, BCRYPT_KEY_HANDLE *phKey, uint32_t dwLength, ...)` → `cng_rsa2048_keygen()`
- [ ] `NTSTATUS BCryptImportKeyPair(...)` → `cng_key_store_get()` then load handle
- [ ] `NTSTATUS BCryptExportKey(...)` → serialize key bytes from handle
- [ ] `NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE hAlgo, ...)` → free slot
- [ ] Register all in `bcrypt.dll` stub table using same pattern as `kernel32` stubs
- [ ] `#define STATUS_SUCCESS 0`, `STATUS_INVALID_PARAMETER 0xC000000D`, `STATUS_NOT_SUPPORTED 0xC00000BB` (NT status codes)
- [ ] Commit: `"cng: BCrypt API stubs -- OpenAlgorithmProvider/Encrypt/Hash/GenRandom/KeyPair, bcrypt.dll table"`

## 6. NCrypt API `[Sonnet]`

`NCryptOpenStorageProvider`, `NCryptOpenKey`, `NCryptEncrypt/Decrypt`, `NCryptImportKey`, `NCryptFreeObject`. Registered in `ncrypt.dll` stub table.

**Files:** `src/kernel/cng/ncrypt_stubs.c` (new), `include/kernel/cng/ncrypt.h` (new)

> [!NOTE]
> NCrypt is the CNG key storage API (builds on top of key store §3). `NCRYPT_PROV_HANDLE`, `NCRYPT_KEY_HANDLE` = `uint32_t` opaque handles; 32-slot table. `NCryptOpenStorageProvider(phProvider, pszProviderName, ...)`: `pszProviderName` = `NULL` or `L"Microsoft Software Key Storage Provider"` → open software key store handle. `NCryptOpenKey(hProvider, phKey, pszKeyName, ...)` → `cng_key_store_get(name, ...)` + populate handle. `NCryptEncrypt(hKey, pbInput, ..., pbOutput, ...)` → `cng_aes256gcm_encrypt()` using key from handle. `NCryptImportKey(hProvider, ..., pszBlobType, pbData, cbData, ...)` → `cng_key_store_import()`. `NCryptFreeObject(hObject)` → release handle slot. Export: `NCryptExportKey(hKey, ..., pbOutput, ...)` → `cng_key_store_get()`.

- [ ] `typedef uint32_t NCRYPT_PROV_HANDLE; typedef uint32_t NCRYPT_KEY_HANDLE;`
- [ ] 32-slot `ncrypt_object_t g_ncrypt_objects[32]` table
- [ ] `SECURITY_STATUS NCryptOpenStorageProvider(NCRYPT_PROV_HANDLE *phProvider, const char *pszProviderName, ...)` -- open provider
- [ ] `SECURITY_STATUS NCryptOpenKey(NCRYPT_PROV_HANDLE hProv, NCRYPT_KEY_HANDLE *phKey, const char *pszKeyName, ...)` → `cng_key_store_get()`
- [ ] `SECURITY_STATUS NCryptEncrypt(NCRYPT_KEY_HANDLE hKey, ...)` → `cng_aes256gcm_encrypt()`
- [ ] `SECURITY_STATUS NCryptDecrypt(NCRYPT_KEY_HANDLE hKey, ...)` → `cng_aes256gcm_decrypt()`
- [ ] `SECURITY_STATUS NCryptImportKey(NCRYPT_PROV_HANDLE hProv, ..., const char *pszBlobType, ...)` → `cng_key_store_import()`
- [ ] `SECURITY_STATUS NCryptExportKey(NCRYPT_KEY_HANDLE hKey, ...)` → `cng_key_store_get()`
- [ ] `SECURITY_STATUS NCryptFreeObject(ULONG_PTR hObject)` → free slot
- [ ] Register in `ncrypt.dll` stub table
- [ ] Commit: `"cng: NCrypt API stubs -- OpenStorageProvider/OpenKey/Encrypt/Import/Free, ncrypt.dll table"`

## 7. Code Signing `[Opus]`

`codesign_sign(exe_path, priv_key)` appends Ed25519 signature to PE32+. `codesign_verify(exe_path, cert_store)` validates. Optional enforcement: `HKLM\SECURITY\CodeSigning\Required` (default 0).

**Files:** `src/kernel/cng/codesign.c` (new), `include/kernel/cng/codesign.h` (new)

> [!NOTE]
> `[Opus]` due to: security-critical (incorrect verification is equivalent to no code signing), PE32+ binary format manipulation (security-critical), and policy enforcement in `task_create_user()` (kernel security boundary). **Use Ed25519 via monocypher** (`crypto_ed25519_sign/check()`) rather than RSA for code signing -- Ed25519 is faster, shorter signatures (64 bytes vs 256), and monocypher already implements it. Signature appended to PE32+ after the last section: 64-byte Ed25519 signature + 32-byte signer public key + 4-byte magic (`0x434F5349` = "COSI"). **Signing**: `codesign_sign(exe_path, priv_key[64])` -- `vfs_read()` entire PE; `crypto_ed25519_sign(sig, priv_key, pub_key, pe_data, pe_len)` over full binary bytes; append 100-byte trailer. **Verification**: `codesign_verify(exe_path, cert_store)` -- detect trailer (check last-4-bytes magic); extract pub_key; `crypto_ed25519_check(sig, pub_key, pe_data, pe_len - 100)` -- returns 0 on valid. If `Required=1` and verification fails: `task_create_user()` returns `TASK_ERR_UNSIGNED`. Self-signed: signer pub_key can match a cert in `cert_store`; if no cert match and `Required=1`: deny.

- [ ] `int codesign_sign(const char *exe_path, const uint8_t priv_key[64])` -- read PE; Ed25519 sign; append 100-byte trailer
- [ ] `int codesign_verify(const char *exe_path, int strict)` -- detect trailer; `crypto_ed25519_check()`; return 0=valid, -1=invalid, -2=unsigned
- [ ] `#define CODESIGN_MAGIC 0x434F5349` (`"COSI"`) -- trailer magic
- [ ] `int codesign_is_required(void)` -- `RegGetValue(HKLM\\SECURITY\\CodeSigning\\Required)` → bool
- [ ] Hook `task_create_user()`: if `codesign_is_required()` and `codesign_verify()` returns -1: `klog(LOG_WARN, "codesign", "blocked unsigned EXE: %s", path)`; return `TASK_ERR_UNSIGNED`
- [ ] `codesign_sign` tool: add `tools/codesign.c` (host-side) that reads priv key from file and signs an EXE
- [ ] `#define TASK_ERR_UNSIGNED -42` in `task.h` (or return existing permission error)
- [ ] Commit: `"cng: code signing -- Ed25519 PE32+ trailer, verify in task_create_user, optional enforcement"`

## 8. EFS Integration `[Opus]`

`efs_encrypt_file(path)` generates a 256-bit file key (FEK), encrypts the file with AES-256 in the sector-based mode Windows EFS uses (CBC or XTS with an IV derived from the file offset, as the `ntfs_efs.c` stubs document), and RSA-OAEP-wraps the FEK into the Windows-format `$EFS` stream. AES-256-GCM is not used here: it is not the Windows EFS format. `efs_decrypt_file(path)` reverses. The NTFS driver already parses `$EFS` (a `$LOGGED_UTILITY_STREAM`, type 0x100, named `$EFS`, with DDF entries) and routes encrypted reads and writes through `src/kernel/fs/ntfs/ntfs_efs.c`; this section fills that file's unwrap and cipher stubs and adds the write-side stream, so files stay compatible with Windows.

**Files:** `src/kernel/cng/efs.c` (new), `include/kernel/cng/efs.h` (new), `src/kernel/fs/ntfs/ntfs_efs.c` (fill the stubs), `include/kernel/fs/ntfs.h`

> [!NOTE]
> **Superseded 2026-09-29:** the custom `$EFS` layout below (magic `0x45465300`, `NTFS_ATTR_EFS = 0x40`) is replaced by the Windows format the shipped parser reads; do not add a second on-disk format. `[Opus]` due to: security-critical (file encryption in kernel VFS hot path), NTFS attribute extension (`$EFS` is an NTFS alternate data stream concept), and transparent encryption/decryption in the NTFS data path (must be invisible to VFS callers). **`$EFS` attribute format** (simplified, stored as named NTFS attribute `$EFS`): 4-byte magic `0x45465300` + 32-byte nonce + 16-byte GCM tag + 256-byte RSA-wrapped content key. `efs_encrypt_file(path)`: (a) `csprng_read(ck, 32)` -- random content key; (b) `csprng_read(nonce, 12)` -- random nonce; (c) `vfs_read(path, ...)` → plaintext buffer; (d) `cng_aes256gcm_encrypt(ck, nonce, plain, plen, cipher, tag)` -- allocate cipher buffer via `pmm_alloc_contiguous`; (e) `cng_rsa2048_encrypt(user_pub_key, ck, 32, wrapped_key)` -- wrap content key with user RSA pub key; (f) write `$EFS` NTFS attribute; (g) overwrite file data with ciphertext. `efs_decrypt_file(path)`: read `$EFS` → `cng_rsa2048_decrypt(user_priv_key, wrapped_key, ck)` → `cng_aes256gcm_decrypt(ck, nonce, cipher, clen, plain, tag)`. **Transparent hook**: NTFS read path -- after reading file data, if `$EFS` attribute present: decrypt inline before returning to VFS caller. NTFS write path -- if `$EFS` set: encrypt before writing to disk. Only for authenticated users with matching RSA key.

- [ ] `int efs_encrypt_file(const char *path)` -- FEK gen; AES-256 sector-mode encrypt; RSA-OAEP wrap into a DDF entry; write the `$EFS` stream
- [ ] `int efs_decrypt_file(const char *path)` -- read `$EFS`; RSA-OAEP unwrap the FEK; AES-256 sector-mode decrypt; return plaintext
- [ ] Interoperability fixture: a file encrypted by Windows 11 EFS with a known test certificate decrypts to its known plaintext; the cipher mode (CBC vs XTS) is taken from that fixture, not assumed
- [ ] `$EFS` stream: reuse the shipped `ntfs_efs_parse()` for reads; add the write side of the same `$LOGGED_UTILITY_STREAM` layout (no new attribute type)
- [ ] Transparent read: fill `ntfs_efs_unwrap_fek()` and `ntfs_efs_decrypt_data()` so the existing `ntfs_read_encrypted_data()` path returns plaintext
- [ ] Transparent write: fill `ntfs_efs_encrypt_data()` so the existing `ntfs_write_encrypted_data()` path encrypts before the disk write
- [ ] Raw ciphertext write before filling that stub: `ntfs_write_encrypted_data()` writes through `ntfs_write_data()`, whose encrypted branch calls it again
  - The loop is `src/kernel/fs/ntfs/ntfs_efs.c` (write via `ntfs_write_data()`) to `src/kernel/fs/ntfs/ntfs_data_write.c` (`NTFS_ATTR_FLAG_ENCRYPTED` branch). It is dormant only because the cipher stub fails first; add a raw write that bypasses EFS dispatch while keeping journaling and allocation, and test that a write then read-back encrypts exactly once.
- [ ] Offset-aware cipher API: pass the file offset to `ntfs_efs_encrypt_data()` / `ntfs_efs_decrypt_data()` so the sector IV can be derived, read whole sectors, and read-modify-write partial sectors
  - Today both take only key, buffer and length, and the read and write paths drop the offset. The interoperability fixture covers nonzero-offset, unaligned and cross-sector reads and writes.
- [ ] `int efs_is_encrypted(const char *path)` → check `$EFS` attribute presence
- [ ] Right-click File Manager context menu stub: "Encrypt" / "Decrypt" → `efs_encrypt/decrypt_file()` (forward ref to File Manager TODO)
- [ ] Commit: `"cng: EFS -- efs_encrypt/decrypt_file, Windows $EFS stream, AES-256 sector mode + RSA-OAEP key wrap, transparent data path"`

---

## OS Comparison


| ⭐  | Feature                                      | 🪟 Win11                                                   | 🐧 Linux                                                             | 🚀 Impossible OS                                          |
| --- | -------------------------------------------- | ---------------------------------------------------------- | -------------------------------------------------------------------- | --------------------------------------------------------- |
| ⭐  | Crypto primitives                            | ✅ BCryptPrimitives.dll; AES-NI acceleration; CNG provider | ✅ kernel `crypto/` subsystem; AES-NI; GCM                           | ⬜ §1 -- `⭐` no AES-NI yet (pure-C                       |
| 💎  | TLS handshake helpers                        | ✅ SChannel; BCrypt TLS extension; full                    | ✅ OpenSSL / BoringSSL / mbedTLS;                                    | ⬜ §8 -- TLS 1.2 PRF + PKCS#1                             |
| 💎  | X.509 cert store                             | ✅ Windows Certificate Store; `certmgr.msc`; CAPI2;        | ✅ `ca-certificates`; OpenSSL cert store; `update-ca-certificates`;  | ⬜ §2 -- minimal DER parser; Registry backend             |
| ⭐  | Key store                                    | ✅ DPAPI; CNG key storage providers;                       | ✅ `gnome-keyring`/`kwallet`; `libsecret`; kernel keyring (`keyctl`) | ⬜ §3 -- `⭐` no TPM dependency; HKCU                     |
| 💎  | BCrypt API                                   | ✅ Full BCrypt.dll; all algorithms; hardware               | ✅ No BCrypt equivalent; use libgcrypt                               | ⬜ §4 -- Win32-compatible `bcrypt.dll` stub table; routes |
| 💎  | NCrypt API -- `NCryptOpenKey/Encrypt/Import` | ✅ Full NCrypt.dll; TPM-backed keys; smart                 | ✅ PKCS#11 (`p11-kit`); no NCrypt equivalent                         | ⬜ §5 -- `ncrypt.dll` stub; backed by HKCU                |
| ⭐  | Code signing                                 | ✅ Authenticode (RSA + X.509); Kernel                      | ✅ IMA/EVM (kernel integrity measurement); `kexec`                   | ⬜ §7 -- `⭐` Ed25519 (smaller+faster than RSA            |
| 💎  | EFS                                          | ✅ NTFS EFS; AES-256; DPAPI-backed keys;                   | ✅ `fscrypt` (ext4/f2fs); eCryptfs; `dm-crypt` (block-level);        | ⬜ §6 -- `⭐` RSA-2048 key wrap +                         |

> **After §1–§8:** Impossible OS has a complete CNG-compatible cryptographic stack. The `⭐` differentiators: Ed25519 code signing is a smaller, faster, and more modern algorithm than Windows Authenticode RSA; the key store requires no TPM (useful on VMs and embedded hardware); and EFS uses a direct AES-256-GCM + RSA key wrap in the NTFS data path with no DPAPI complexity.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Boot serial: `[cng] primitives ready: AES-256-GCM SHA-256 HMAC RSA-2048 X25519 BLAKE2b`
- [ ] `cng_aes256gcm_encrypt(key, nonce, "hello", 5, cipher, tag)` → `cng_aes256gcm_decrypt(key, nonce, cipher, 5, plain, tag)` → plain == "hello"; corrupt `tag[0]++` → decrypt returns -1
- [ ] `cng_sha256("abc", 3, hash)` → matches known SHA-256("abc") = `ba7816bf...`
- [ ] `cng_hmac_sha256(key, 4, "data", 4, mac)` → matches RFC 4231 HMAC-SHA256 test vector
- [ ] `cng_rsa2048_keygen(pub, priv)` → `cng_rsa2048_encrypt(pub, "test", 4, cipher)` → `cng_rsa2048_decrypt(priv, cipher, plain, &plen)` → plain == "test"
- [ ] `cng_cert_store_load_bundle()` serial log: `[cng] cert store: N CA roots loaded` (N > 100)
- [ ] `cng_cert_store_find("DigiCert Global Root CA", &cert)` → returns valid cert struct
- [ ] `BCryptHash(sha256_algo, ..., "abc", 3, out, 32)` → same result as `cng_sha256("abc", 3, out)`
- [ ] `BCryptGenRandom(NULL, buf, 16, ...)` → `buf` contains non-zero bytes (CSPRNG output)
- [ ] `NCryptOpenKey(prov, &key, "test_key", ...)` → key handle valid after `cng_key_store_import("test_key", ...)`
- [ ] `efs_encrypt_file("C:\\Users\\Admin\\Documents\\secret.txt")` → file contents unreadable raw; `efs_decrypt_file(...)` → original content restored
- [ ] `efs_is_encrypted("C:\\Users\\Admin\\Documents\\secret.txt")` → 1 after encrypt
- [ ] `codesign_sign("C:\\test.exe", priv_key)` → last 100 bytes = COSI trailer; `codesign_verify("C:\\test.exe", 0)` → 0 (valid)
- [ ] With `Required=1`: unsigned EXE launch → `TASK_ERR_UNSIGNED` logged; process not started
- [ ] `cng_tls_prf(secret, 16, "key expansion", seed, 32, out, 48)` → matches known TLS 1.2 P_hash test vector
- [ ] Commit: `"cng: crypto and cert store -- all CNG sections complete"`
