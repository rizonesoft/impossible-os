# TODO-20 — Kernel Embedded Libraries

> **Goal:** Build the complete freestanding library layer that every other kernel subsystem depends on but cannot yet use: a safe `snprintf`/`vsnprintf` (currently absent — `panic.c` explicitly works around this), a full floating-point math library extending the stb_truetype-only `kmath.h`, an LZ4 block compressor (already assumed by TODO-15 hibernation and TODO-16 crash dump but not yet present), a miniz deflate/ZIP library for IXFS compression and HTTP gzip, the Monocypher cryptographic primitives with a kernel CSPRNG, a cJSON DOM parser for config and theme files, and Mbed TLS as a freestanding TLS record layer for HTTPS/FTPS. All libraries compile with `-ffreestanding -nostdlib` and redirect heap allocation through `kmalloc`/`kfree` with correct `pmm_alloc_contiguous` escalation for buffers > 4 KiB.

> [!IMPORTANT]
> **Already done — do NOT re-implement:**
> - `stb_truetype` → `src/kernel/gfx/stb_truetype_impl.c` ✅
> - `stb_image` → `include/stb_image.h` + `src/kernel/image.c` ✅
> - `kmath.h` → exists with `fabs`, `floor`, `ceil`, `fmod`, `sqrt` ✅
>   (incomplete; §2 extends it)
>
> **Not in scope for this TODO:**
> - `dr_wav`, `dr_mp3`, `stb_vorbis`, `pl_mpeg` audio/video codecs → belong
>   in the multimedia domain (`10-apps` or `08-desktop-shell`).
> - TLS session management, certificate validation, HTTPS client logic →
>   networking domain (`06-networking`); §7 of this TODO only ports Mbed TLS
>   as a freestanding library.
> - IXFS compression, HTTP gzip, package format — they *use* these libraries;
>   they're out of scope here.

> [!CAUTION]
> **Memory rule:** Decompression/decryption output buffers can easily exceed 4 KiB. Always use `pmm_alloc_contiguous()` for output buffers; `kmalloc` is only safe for in-library state structs (≤ 4 KiB). Violating this silently corrupts the 2 MiB heap.

---

## Inputs

- `include/kernel/kmath.h` — existing math helpers (fabs/floor/ceil/fmod/sqrt); extend in §2
- `src/kernel/panic.c` — explicitly comments "no snprintf in freestanding"; fixed by §1
- `src/kernel/gfx/stb_truetype_impl.c` — example of a correctly integrated freestanding stb library; use as the pattern for all new ports
- `src/kernel/image.c` — example of tiered kmalloc/pmm allocator delegation for stb_image; replicate the pattern for miniz/monocypher/Mbed TLS
- → XREF: `TODO-15-power-management.md §4` — S4 hibernation image write uses LZ4 block compression; requires §3 of this TODO to be complete first
- → XREF: `TODO-16-crash-dump-generation.md §5–§6` — crash dump minidump/kernel dump use LZ4 compression; requires §3
- → XREF: `TODO-17-kernel-security-hardening.md §9` — `__stack_chk_guard` canary seeded with RDRAND; the full CSPRNG (§5) is a superset of that; share the seed path
- → XREF: `TODO-11-security-reference-monitor.md §4` — token/SID hashing uses Blake2b from Monocypher (§5)
- → XREF: `TODO-13-registry-completion.md §4` — registry hive WAJ uses CRC32C; separate from Monocypher but benefits from a consistent hash dispatch layer (§5)
- → XREF: `06-networking/TODO-03-http-tls.md §5` — Mbed TLS Kernel Port in networking depends on this §7 freestanding port being complete first

---

## Outcome

- `snprintf`/`vsnprintf` are available kernel-wide; `panic.c` and all other subsystems can use safe formatted string output without hand-rolling it.
- A complete floating-point math library (`kmath.h` + `src/libc/math.c`) provides sin/cos/tan/atan2/exp/log/pow with SSE2 precision; no libm needed.
- LZ4 block compression is available for crash dumps, hibernation images, and any other kernel subsystem that needs fast in-kernel compression.
- miniz provides zlib-compatible deflate/inflate and ZIP archive reading, unblocking IXFS transparent compression and HTTP gzip.
- Monocypher provides production-grade ChaCha20, Poly1305, Blake2b, Argon2i, X25519, and Ed25519 with a RDRAND-seeded CSPRNG (`csprng_fill()`).
- cJSON parses and serialises JSON for theme files, settings, update manifests, and NTP server lists.
- Mbed TLS 3.x compiles freestanding in a minimal configuration (TLS 1.3 record layer + AES-GCM + RSA/ECDSA) ready for use by the networking layer.

---

## Implementation Order

| ⭐  | Order | Deliverable                                          | Depends On          | Status |
| --- | :---: | ---------------------------------------------------- | ------------------- | :----: |
| 💎  |   1   | Freestanding string library (`snprintf`/`vsnprintf`) | —                   |  [ ]   |
| 💎  |   2   | Complete floating-point math library                 | —                   |  [ ]   |
| 💎  |   3   | LZ4 block compressor                                 | —                   |  [ ]   |
| 💎  |   4   | miniz deflate/inflate + ZIP                          | 1                   |  [ ]   |
| 💎  |   5   | Monocypher crypto primitives + kernel CSPRNG         | 1                   |  [ ]   |
| 💎  |   6   | cJSON DOM parser                                     | 1                   |  [ ]   |
| 💎  |   7   | Mbed TLS freestanding port (record layer)            | 1, 2, 5             |  [ ]   |

> 💎 = parity work — matches what Windows 11 and Linux already do.
> ⭐ = exclusive work — Impossible OS is superior or first.

---

## 1. Freestanding String Library `[Sonnet]`

### 1.1 Audit existing string functions

- [x] Grep the entire kernel source for `memcpy`, `memset`, `strlen`, `strcmp`, `snprintf` etc.; document which are provided by compiler builtins (`__builtin_memcpy`) vs. missing vs. hand-rolled in callers
- [x] Map against the functions `panic.c`, `printk.h`, VFS, and drivers call — the gap list drives the implementation priority

> **Gap analysis results (2026-03-27):**
>
> **Provided (weak symbols in `image.c` + `stb_truetype_impl.c` + `freestanding/string.h`):**
> - `memcpy`, `memset`, `memmove`, `memcmp`, `strlen` — 2 duplicate implementations (image.c + stb_truetype_impl.c); should consolidate into `libc/string.c`
>
> **Compiler builtins used:**
> - `__builtin_memcpy`, `__builtin_memset` — only in `image_save.c` (4 uses)
>
> **Missing entirely (no implementation, no builtin):**
> - `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strcat`, `strncat` — 0 actual calls, but hand-rolled `str_eq()` in 5 files (klog.c, klog_disk.c, icon_store.c, ipc/shmem.c, blkdev.c)
> - `snprintf` / `vsnprintf` — not available; `panic.c` has a comment noting "no snprintf in freestanding"; `klog.c` has `vformat_buf()` (private, supports %d %u %x %p %s %c only)
> - `atoi`, `strtol`, `strtoul` — not available
> - `memchr`, `strstr`, `strchr`, `strrchr` — not available
>
> **Priority for §1.2:**
> 1. Consolidate `memcpy`/`memset`/`memmove`/`memcmp`/`strlen` into `libc/string.c` (eliminate 2 duplicate weak symbol sets)
> 2. Add `strcmp`/`strncmp` (replace 5 hand-rolled `str_eq()` functions)
> 3. Add `snprintf`/`vsnprintf` (promote `klog.c:vformat_buf()` to public API)
> 4. Add `strcpy`/`strncpy`/`strcat` (needed by future Win32 API + registry)
> 5. Add `atoi`/`strtoul` (needed by future config parsing + shell)

### 1.2 Core memory and string functions

- [x] Create `src/libc/string.c` and `include/libc/string.h`; compile with `-ffreestanding -nostdlib -O2`
- [x] Memory operations: `memcpy`, `memmove`, `memset`, `memcmp`, `memchr` — all implemented
- [x] String operations: `strlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strstr`, `strtol`, `strtoul`, `atoi` — all implemented
- [x] BSD safe: `strlcpy`, `strlcat` — always NUL-terminate
- [x] Removed duplicate weak symbols from `image.c` and `stb_truetype_impl.c`; updated `freestanding/string.h` to redirect to `libc/string.h`

### 1.3 snprintf / vsnprintf

- [ ] `int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)` — the most critical missing function; implement format specifiers:
  - `%d` / `%i` — signed decimal
  - `%u` — unsigned decimal
  - `%x` / `%X` — hex (lower/upper)
  - `%o` — octal
  - `%s` — string
  - `%c` — character
  - `%p` — pointer (zero-padded hex with `0x` prefix)
  - `%lu`, `%llu`, `%ld`, `%lld` — long / long long variants
  - `%zu` — `size_t`
  - Width, precision, `0`-padding: `%08x`, `%.*s`, `%-20s`
  - `%%` — literal `%`
  - Always NUL-terminate even on truncation; return number of bytes that
    *would* have been written (snprintf semantics)
- [ ] `int snprintf(char *buf, size_t size, const char *fmt, ...)` — thin varargs wrapper over `vsnprintf`
- [ ] Update `panic.c` to use `snprintf` in its crash text builder; remove the current hand-rolled hex formatter
- [ ] Update `klog` / `printk` to use `vsnprintf` internally

### 1.4 Commit

- [ ] Commit: `"libc: freestanding string library — memcpy/memset/str*, snprintf/vsnprintf"`

---

## 2. Complete Floating-Point Math Library `[Sonnet]`

### 2.1 Audit kmath.h

- [ ] Document which functions `kmath.h` already provides: `kmath_fabs`, `kmath_floor`, `kmath_ceil`, `kmath_fmod`, `kmath_sqrt`
- [ ] Identify callers across the codebase to understand which new functions are urgently needed (stb_truetype, compositor, future audio synthesis)

### 2.2 Trig functions

- [ ] Add to `src/libc/math.c` and expose in `include/libc/math.h`; use polynomial approximations accurate to ≤ 1 ULP for the double range `[-π, π]`:
  - `double kmath_sin(double x)` — Taylor series with range reduction
  - `double kmath_cos(double x)` — `sin(π/2 - x)` or paired Clenshaw
  - `double kmath_tan(double x)` — `sin/cos` with infinity guard
  - `double kmath_asin(double x)` — valid for `|x| ≤ 1`
  - `double kmath_acos(double x)` — `π/2 - asin(x)`
  - `double kmath_atan(double x)` — Padé approximation
  - `double kmath_atan2(double y, double x)` — four-quadrant atan
- [ ] `float` variants (`kmath_sinf`, etc.) — cast to double, compute, cast back; sufficient for all current kernel uses

### 2.3 Exponential and logarithm

- [ ] `double kmath_exp(double x)` — `e^x` via Horner polynomial + range reduction by `ln2`
- [ ] `double kmath_log(double x)` — natural log via `atanh` identity
- [ ] `double kmath_log2(double x)` — `log(x) / log(2)`
- [ ] `double kmath_log10(double x)` — `log(x) / log(10)`
- [ ] `double kmath_pow(double base, double exp)` — `exp(exp * log(base))` with special-case handling for integer exponents

### 2.4 Rounding and miscellaneous

- [ ] `double kmath_round(double x)` — round half-away-from-zero
- [ ] `double kmath_trunc(double x)` — truncate toward zero
- [ ] `double kmath_cbrt(double x)` — cube root via Newton-Raphson

### 2.5 Commit

- [ ] Commit: `"libc: floating-point math — sin/cos/tan/atan2/exp/log/pow, float variants"`

---

## 3. LZ4 Block Compressor `[Sonnet]`

### 3.1 LZ4 port

- [ ] Vendor `lz4.c` + `lz4.h` from the official LZ4 repository (BSD-2 license, ~2000 lines) into `src/libs/lz4/`
- [ ] Compile with `-ffreestanding -nostdlib`; LZ4 has no `malloc` calls in its core API — it operates entirely on caller-provided buffers
- [ ] Expose the block API:
  ```c
  int lz4_compress(const void *src, int src_size,
                   void *dst, int dst_capacity);      /* returns compressed size */
  int lz4_decompress(const void *src, int src_size,
                     void *dst, int dst_capacity);    /* returns original size */
  int lz4_compress_bound(int input_size);             /* worst-case output size */
  ```
- [ ] Buffer allocation responsibility: callers provide pre-allocated buffers; for output buffers > 4 KiB use `pmm_alloc_contiguous()`; document this in `include/libs/lz4.h`
- [ ] Wire into TODO-16 crash dump writer: replace the placeholder LZ4 call with the actual implementation (→ XREF `TODO-16-crash-dump-generation.md §6`)
- [ ] Wire into TODO-15 hibernation image writer (→ XREF `TODO-15-power-management.md §4`)

### 3.2 LZ4 frame format (optional)

- [ ] For streaming use cases (large IXFS extents), vendor `lz4frame.c` from the same repository; provides the standard `.lz4` framing layer with content checksum (XXH32) and block independence flag
- [ ] `lz4f_compress_begin / update / end` / `lz4f_decompress` API

### 3.3 Commit

- [ ] Commit: `"libs: LZ4 block compressor, freestanding port, wired to crash dump + hibernate"`

---

## 4. miniz Deflate / Inflate + ZIP `[Sonnet]`

### 4.1 miniz port

- [ ] Vendor `miniz.h` / `miniz.c` (MIT, ~6000 lines) into `src/libs/miniz/`
- [ ] Compile with `-ffreestanding -nostdlib -msse2`
- [ ] Redirect `malloc`/`free` → `kmalloc`/`kfree` via `#define` overrides for the internal `tinfl` decompressor state struct (≤ 4 KiB — kmalloc safe)
- [ ] For decompression output buffers > 4 KiB: override the output buffer allocation callback to use `pmm_alloc_contiguous()`
- [ ] Core API exposed:
  ```c
  /* Low-level deflate/inflate */
  int mz_compress(uint8_t *dst, uint32_t *dst_len,
                  const uint8_t *src, uint32_t src_len);
  int mz_uncompress(uint8_t *dst, uint32_t *dst_len,
                    const uint8_t *src, uint32_t src_len);
  /* ZIP archive reading (read-only) */
  bool zip_open(zip_t *z, const void *data, size_t size);
  int  zip_entry_count(const zip_t *z);
  bool zip_find(const zip_t *z, const char *name, zip_entry_t *out);
  int  zip_read(const zip_t *z, const zip_entry_t *entry,
                void *buf, size_t buf_size);
  void zip_close(zip_t *z);
  ```
- [ ] Use gzip decompression (`MZ_DEFAULT_STRATEGY`) for HTTP `Content-Encoding: gzip` responses in the networking layer

### 4.2 Smoke tests

- [ ] Compress a known 8 KiB buffer; decompress; verify byte-for-byte integrity
- [ ] Open a ZIP archive from memory; list entries; extract one file; verify content against a known SHA-256 (using Monocypher Blake2b from §5)

### 4.3 Commit

- [ ] Commit: `"libs: miniz deflate/inflate, ZIP archive reading"`

---

## 5. Monocypher Crypto Primitives + Kernel CSPRNG `[Opus]`

### 5.1 Monocypher port

- [ ] Vendor `monocypher.c` + `monocypher.h` (BSD-2, ~3000 lines) into `src/libs/monocypher/`
- [ ] Compile with `-ffreestanding -nostdlib`; Monocypher uses zero heap allocation — all state is on the caller-provided stack or caller-provided buffers; no `malloc` redirects needed
- [ ] Expose the following primitives (keep names unchanged):
  - **ChaCha20** — `crypto_chacha20_djb()` / `crypto_chacha20_x()` stream cipher; 256-bit key, 64-bit nonce
  - **Poly1305** — `crypto_poly1305()` MAC; pairs with ChaCha20 for AEAD
  - **ChaCha20-Poly1305** — `crypto_aead_lock()` / `crypto_aead_unlock()` authenticated encryption
  - **Blake2b** — `crypto_blake2b()` cryptographic hash; 256 or 512-bit output
  - **Argon2id** — `crypto_argon2()` memory-hard password hash; used by login auth (→ XREF `TODO-11-security-reference-monitor.md §4`)
  - **X25519** — `crypto_x25519()` Diffie-Hellman key exchange
  - **Ed25519** — `crypto_eddsa_sign()` / `crypto_eddsa_check()` signatures; used for EIF code signing (→ XREF `TODO-08-binary-system.md §12`)

### 5.2 Kernel CSPRNG

- [ ] Create `src/kernel/csprng.c` and `include/kernel/csprng.h`:
  ```c
  void     csprng_init(void);              /* called from Phase 1 init */
  void     csprng_fill(void *buf, size_t len); /* fills with random bytes */
  uint64_t csprng_u64(void);               /* convenience — random uint64_t */
  ```
- [ ] `csprng_init()` seeds from multiple entropy sources:
  1. Three `RDRAND` retries (Intel/AMD hardware RNG); if any succeed use the 64-bit value
  2. RDTSC XOR'd with the physical address of `csprng_init` (ASLR entropy)
  3. ACPI PM timer reading (adds ~20 bits of timing entropy)
  4. XOR all sources into a 256-bit seed; expand with Blake2b to produce the initial ChaCha20 key
- [ ] Continuous operation: maintain a global ChaCha20 state; `csprng_fill` calls `crypto_chacha20_x()` to generate output; re-key every 4 MiB of output (forward-secrecy)
- [ ] Share the RDRAND seed path with `TODO-17-kernel-security-hardening.md §9` (`__stack_chk_guard` canary init); `canary_init()` calls `csprng_u64()` after `csprng_init()` runs
- [ ] `SYS_GETRANDOM` syscall: `getrandom(buf, len, flags)` fills user buffer via `csprng_fill` + `copy_to_user`; add to SSDT (→ XREF `TODO-05-native-api-layer.md §4`)

### 5.3 Commit

- [ ] Commit: `"libs: Monocypher crypto, kernel CSPRNG (RDRAND + Blake2b), SYS_GETRANDOM"`

---

## 6. cJSON DOM Parser `[Sonnet]`

### 6.1 cJSON port

- [ ] Vendor `cJSON.c` + `cJSON.h` (MIT, ~2000 lines) into `src/libs/cjson/`
- [ ] Compile with `-ffreestanding -nostdlib`
- [ ] Redirect allocator via `cJSON_InitHooks()` at kernel init (Phase 2, after heap is ready):
  ```c
  cJSON_Hooks hooks = { .malloc_fn = kmalloc, .free_fn = kfree };
  cJSON_InitHooks(&hooks);
  ```
- [ ] Memory note: parsed JSON trees can grow large; add a max-depth guard (`cJSON_SetMaxDepth(32)`) and validate total node count before full parse to avoid heap exhaustion from malformed documents

### 6.2 Usage API

- [ ] Expose a thin kernel wrapper `src/kernel/json.c` / `include/kernel/json.h`:
  ```c
  cJSON *json_parse(const char *text);         /* wrapper over cJSON_Parse */
  cJSON *json_get(cJSON *obj, const char *key); /* cJSON_GetObjectItem */
  const char *json_str(cJSON *item);            /* cJSON_GetStringValue */
  int    json_int(cJSON *item);                 /* cJSON_GetNumberValue as int */
  char  *json_print(cJSON *obj);               /* cJSON_PrintUnformatted */
  void   json_free(cJSON *obj);                /* cJSON_Delete */
  ```
- [ ] Use for: desktop theme files (`theme.json`), settings persistence, update manifests, NTP server list, and any kernel-side config read at boot
- [ ] Test: parse `{"os": "Impossible", "build": 1024, "debug": true}`; verify all fields extract correctly

### 6.3 Commit

- [ ] Commit: `"libs: cJSON DOM parser, json_parse/get/print wrapper, cJSON_InitHooks"`

---

## 7. Mbed TLS Freestanding Port `[Opus]`

> [!IMPORTANT]
> **Scope overlap — `06-networking/TODO-03-http-tls.md §5`** also claims "Mbed TLS Kernel Port". This §7 is the canonical freestanding port; `TODO-03 §5` must depend on this section being complete and should not re-implement the library itself. When TODO-03 is next validated, update its §5 to reference this TODO-20 §7 as its prerequisite.

### 7.1 Minimal build configuration

- [ ] Clone Mbed TLS 3.x source into `src/libs/mbedtls/` (Apache-2.0); create a `mbedtls_config.h` that enables only the required subset:
  - `MBEDTLS_AES_C` + `MBEDTLS_GCM_C` — AES-128-GCM / AES-256-GCM
  - `MBEDTLS_SHA256_C` + `MBEDTLS_SHA512_C` — hash primitives
  - `MBEDTLS_RSA_C` + `MBEDTLS_PKCS1_V21` — RSA-OAEP for certificate public key operations
  - `MBEDTLS_ECDSA_C` + `MBEDTLS_ECP_DP_SECP256R1_ENABLED` — ECDSA P-256
  - `MBEDTLS_SSL_TLS_C` + `MBEDTLS_SSL_PROTO_TLS1_3` — TLS 1.3 record layer
  - `MBEDTLS_CTR_DRBG_C` backed by the kernel CSPRNG (§5.2)
  - **Disable:** `MBEDTLS_NET_C`, `MBEDTLS_TIMING_C`, `MBEDTLS_FS_IO` — all OS-dependent components
- [ ] Platform abstraction layer (`include/libs/mbedtls/mbedtls_platform.h`):
  - `mbedtls_calloc` → `kmalloc` (zero-initialised, ≤ 4 KiB)
  - `mbedtls_free` → `kfree`
  - Large crypto scratch buffers (key agreement, certificate parsing) use `pmm_alloc_contiguous()`; wrap via custom `mbedtls_platform_set_calloc_free`
  - `mbedtls_printf` → `klog(LOG_DEBUG, "tls", ...)`
  - Entropy source → `csprng_fill()` (§5.2) fed into `mbedtls_entropy_add_source`

### 7.2 Integration test

- [ ] Build the minimal Mbed TLS subset with the kernel flags: `-ffreestanding -nostdlib -fno-stack-protector -mno-red-zone -O2 -g` — confirm zero link errors
- [ ] Implement `tls_selftest()`: run `mbedtls_aes_self_test(1)` and `mbedtls_sha256_self_test(1)` — pass means AES and SHA-256 are functionally correct in freestanding mode
- [ ] The full TLS session layer (handshake, certificates, SNI) is implemented in `06-networking`; this section only validates the port

### 7.3 Commit

- [ ] Commit: `"libs: Mbed TLS 3.x freestanding port, AES/SHA256/TLS1.3 config, CSPRNG entropy hook"`

---

## OS Comparison

| ⭐  | Feature                               | 🪟 Windows 11 (WDK)                | 🐧 Linux kernel                  | 🚀 Impossible OS                           |
| --- | ------------------------------------- | ---------------------------------- | -------------------------------- | ------------------------------------------ |
| 💎  | `snprintf` / `vsnprintf`              | ✅ `RtlStringCbPrintf*` (safe)     | ✅ `lib/vsprintf.c` (600+ lines) | ⬜ Planned — §1 (absent; panic.c worksaround) |
| 💎  | Core string ops (memcpy/strlen/etc.)  | ✅ `RtlCopyMemory`, `RtlZeroMemory` | ✅ `lib/string.c`                | ⚠️ Partial — §1 (compiler builtins only)   |
| 💎  | Floating-point math (sin/cos/log/pow) | ✅ `rtlmath` + CRT                 | ✅ `lib/math/` + libgcc          | ⚠️ Partial — §2 (kmath.h, no trig/log)    |
| 💎  | LZ4 fast compression                 | ⚠️ Xpress (proprietary format)    | ✅ `lib/lz4/` in-tree            | ⬜ Planned — §3 (assumed by TODO-15/16)    |
| 💎  | Deflate / zlib compression            | ✅ `RtlDecompressBuffer` (LZNT1)   | ✅ `lib/zlib_deflate/` in-tree   | ⬜ Planned — §4 (miniz)                   |
| 💎  | ZIP archive reading                   | ✅ Shell extension / Expand.exe    | ✅ `unzip` user-mode             | ⬜ Planned — §4 (miniz ZIP API)           |
| 💎  | ChaCha20 / Poly1305 AEAD              | ✅ BCrypt (`BCRYPT_CHACHA20_POLY1305_ALGORITHM`) | ✅ `crypto/chacha20poly1305.c` | ⬜ Planned — §5 (Monocypher) |
| 💎  | Blake2b hash                          | ✅ BCrypt SHA-2 (no Blake2b)       | ✅ `crypto/blake2b_generic.c`    | ⬜ Planned — §5                           |
| 💎  | Argon2id password hash                | ✅ BCrypt `BCRYPT_KDF_SP800_108`   | ✅ `crypto/argon2.c`             | ⬜ Planned — §5                           |
| 💎  | X25519 key exchange                   | ✅ BCrypt ECDH curve25519          | ✅ `crypto/ecdh.c`               | ⬜ Planned — §5 (Monocypher)              |
| 💎  | Ed25519 signatures                    | ✅ BCrypt EdDSA                    | ✅ `crypto/eddsa.c`              | ⬜ Planned — §5 (Monocypher)              |
| 💎  | Kernel CSPRNG (`/dev/urandom`)        | ✅ `KeQuerySystemEntropy` / BCrypt | ✅ `drivers/char/random.c`       | ⬜ Planned — §5 (RDRAND + Blake2b)        |
| 💎  | JSON parser in-kernel                 | ❌ COM-based, user-mode only       | ❌ Not in mainline kernel         | ⬜ Planned — §6 (cJSON) 🚀                |
| 💎  | TLS record layer (AES-GCM, TLS 1.3)  | ✅ SChannel (kernel + user)        | ✅ `net/tls/` in-tree            | ⬜ Planned — §7 (Mbed TLS port)           |

After §1–6, Impossible OS reaches full parity with Windows and Linux on every
in-kernel library primitive. The in-kernel JSON parser (§6) is unique among
production OS kernels — Windows requires COM marshalling and user-mode JSON
parsing for all configuration; Linux has no standard in-kernel JSON. Having
cJSON available at boot allows theme files, update manifests, and NTP server
lists to be loaded directly by the kernel without a separate user-mode service.
The Monocypher CSPRNG (§5) uses RDRAND-first seeding with a Blake2b expander,
providing better seed quality than Linux's entropy pool on systems with a
hardware RNG, while remaining safe on hardware without RDRAND.

---

## Verification

- [ ] **snprintf**: `snprintf(buf, sizeof buf, "%08x %s %d", 0xDEAD, "os", 42)` → `"0000dead os 42"` byte-for-byte; truncation test: `snprintf(buf, 5, "hello world")` → `"hell\0"`, returns `11`.
- [ ] **Math**: `kmath_sin(0) == 0.0`, `kmath_cos(0) == 1.0`, `kmath_atan2(1.0, 1.0)` ≈ `0.7854` (π/4 ± 1e-9), `kmath_exp(1.0)` ≈ `2.71828` (± 1e-9), `kmath_sqrt(4.0) == 2.0`.
- [ ] **LZ4**: compress a 64 KiB zeroed buffer; verify compressed size < original; decompress; byte-for-byte equal; `lz4_compress_bound(65536)` is the upper bound on compressed size.
- [ ] **miniz deflate**: compress 8 KiB string; decompress; verify identity.
- [ ] **miniz ZIP**: open a 3-file ZIP from memory; enumerate entries; extract one file; verify content.
- [ ] **Monocypher AEAD**: encrypt `"hello kernel"` with a random key; decrypt; verify plaintext matches; flip one byte in ciphertext — `crypto_aead_unlock` must return -1 (authentication failure).
- [ ] **CSPRNG**: `csprng_fill(buf1, 32)` and `csprng_fill(buf2, 32)` must produce different outputs; `csprng_u64()` called 1000 times must show no repeated values (birthday bound test).
- [ ] **cJSON**: parse `{"build": 1024, "name": "Impossible OS", "debug": false}`; `json_int(json_get(root, "build")) == 1024`; `strcmp(json_str(json_get(root, "name")), "Impossible OS") == 0`.
- [ ] **Mbed TLS selftest**: `mbedtls_aes_self_test(1)` and `mbedtls_sha256_self_test(1)` both return 0 in the boot log.
- [ ] **Build check**: `bash scripts/build.sh clean` → `=== BUILD OK ===` with all library files compiled under `-ffreestanding -nostdlib`.
- [ ] Commit: `"libs: snprintf/vsnprintf, kmath, LZ4, miniz, Monocypher/CSPRNG, cJSON, Mbed TLS freestanding ports"`
