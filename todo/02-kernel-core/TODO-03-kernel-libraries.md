---
schema_version: 1
id: kernel-libraries
domain: 02-kernel-core
status: active
title: "TODO-03 -- Kernel Embedded Libraries"
---

# TODO-03 -- Kernel Embedded Libraries

> **Goal:** Build the complete freestanding library layer the rest of the kernel depends on: `snprintf`/`vsnprintf` in `libc` (§1; `panic.c` / klog migration still deferred), full floating-point math beyond `kmath.h` (§2), LZ4 (§3; assumed by TODO-26/TODO-27), miniz deflate/ZIP (§4), Monocypher + kernel CSPRNG (§5), cJSON (§6), and Mbed TLS record layer (§7) for HTTPS/FTPS consumers. All ports compile with `-ffreestanding -nostdlib` and route heap through `kmalloc`/`kfree` with `pmm_alloc_contiguous` for buffers > 4 KiB.

> [!IMPORTANT]
> **Current state (code-truth 2026-04-13):** `src/libc/string.c` + `include/libc/string.h` ship `snprintf`/`vsnprintf` and string/memory ops (§1 core done; `panic.c` still hand-rolls crash text). `include/kernel/kmath.h` already inlines `kmath_cos`, `kmath_acos`, `kmath_pow`, `kmath_sqrt` for stb; §2 `src/libc/math.c` trig/log suite not started. `src/libs/cjson/` + `src/kernel/json.c` implement §6 parser + thin wrappers; `cJSON_SetMaxDepth` / production call sites still open. No `src/libs/lz4`, `miniz`, `monocypher`, or `mbedtls` trees. `test_register_klibs` is not declared or called from `src/kernel/test/test_runner.c`.
> **Already done -- do NOT re-implement:**
> - `stb_truetype` -> `src/kernel/gfx/stb_truetype_impl.c` [done]
> - `stb_image` -> `include/stb_image.h` + `src/kernel/image.c` [done]
> - `kmath.h` -> exists with `fabs`, `floor`, `ceil`, `fmod`, `sqrt` [done]
>   (incomplete; §2 extends it)
> **Not in scope for this TODO:**
> - `dr_wav`, `dr_mp3`, `stb_vorbis`, `pl_mpeg` audio/video codecs -> belong in the multimedia domain (`10-apps` or `08-desktop-shell`).
> - TLS session management, certificate validation, HTTPS client logic -> networking domain (`07-networking`); §7 of this TODO only ports Mbed TLS as a freestanding library.
> - IXFS compression, HTTP gzip, package format -- they *use* these libraries; they're out of scope here.

> [!CAUTION]
> **Memory rule:** Decompression/decryption output buffers can easily exceed 4 KiB. Always use `pmm_alloc_contiguous()` for output buffers; `kmalloc` is only safe for in-library state structs (<= 4 KiB). Violating this silently corrupts the 2 MiB heap.

---

## Inputs

- `include/kernel/kmath.h` -- existing math helpers (fabs/floor/ceil/fmod/sqrt); extend in §2
- `src/kernel/panic.c` -- legacy comment path still hand-rolls crash text; safe to migrate to `snprintf` when §1 stabilizes in panic path
- `src/kernel/gfx/stb_truetype_impl.c` -- example of a correctly integrated freestanding stb library; use as the pattern for all new ports
- `src/kernel/image.c` -- example of tiered kmalloc/pmm allocator delegation for stb_image; replicate the pattern for miniz/monocypher/Mbed TLS
- -> XREF: `12-user-platform-sdk/TODO-03-kernel-libraries.md` -- COMPLEMENT: ZIP writer + stream deflate after D02 T03 §6 reader/port; do not re-vendor miniz here
- -> XREF: `TODO-26-power-management.md §4` -- S4 hibernation image write uses LZ4 block compression; requires §3 of this TODO to be complete first
- -> XREF: `TODO-27-crash-dump-generation.md §5, §6` -- crash dump minidump/kernel dump use LZ4 compression; requires §3
- -> XREF: `TODO-10-kernel-security-hardening.md §11` -- `__stack_chk_guard` canary seeded with RDRAND; the full CSPRNG (§5) is a superset of that; share the seed path
- -> XREF: `TODO-15-security-reference-monitor.md §14` -- AppContainer / package SID hashing uses Monocypher (T03 §8; T15 §14)
- -> XREF: `TODO-14-registry-completion.md §5` -- registry hive WAJ uses CRC32C; separate from Monocypher but benefits from a consistent hash dispatch layer (§6)
- -> XREF: `TODO-04-system-logging.md §10` -- HMAC-Blake2b chain for tamper-evident `events.jsonl`; requires Monocypher Blake2b + kernel CSPRNG (§5)
- -> XREF: `07-networking/TODO-03-http-tls.md` §3 -- Mbed TLS Kernel Port in networking depends on this §8 freestanding port being complete first
- -> XREF: `TODO-28-bsod-ux-enhancements.md` §4 -- BSOD smart QR may zlib-compress or LZ4-compress crash payloads using §4 miniz or §3 LZ4 once those subsystems are merged (panic path obeys T03 allocator rules)

---

## Outcome

- `snprintf`/`vsnprintf` are available kernel-wide; `panic.c` and all other subsystems can use safe formatted string output without hand-rolling it.
- A complete floating-point math library (`kmath.h` + `src/libc/math.c`) provides sin/cos/tan/atan2/exp/log/pow with SSE2 precision; no libm needed.
- LZ4 block compression is available for crash dumps, hibernation images, and any other kernel subsystem that needs fast in-kernel compression.
- miniz provides zlib-compatible deflate/inflate and ZIP archive reading, unblocking IXFS transparent compression and HTTP gzip.
- Monocypher provides production-grade ChaCha20, Poly1305, Blake2b, Argon2id, X25519, and Ed25519 with a RDRAND-seeded CSPRNG (`csprng_fill()`).
- cJSON parses and serialises JSON for theme files, settings, update manifests, and NTP server lists.
- Mbed TLS 3.x compiles freestanding in a minimal configuration (TLS 1.3 record layer + AES-GCM + RSA/ECDSA) ready for use by the networking layer.

---

## Implementation Order

| ⭐  | Order | Deliverable                                          | Depends On          | Status |
| --- | :---: | ---------------------------------------------------- | ------------------- | :----: |
| 💎  |   1   | Freestanding string library (`snprintf`/`vsnprintf`) | --                   |  [x]   |
| 💎  |   2   | Complete floating-point math library                 | --                   |  [ ]   |
| 💎  |   3   | LZ4 block compressor                                 | --                   |  [ ]   |
| 💎  |   4   | miniz deflate/inflate + ZIP                          | §1                   |  [ ]   |
| 💎  |   5   | Monocypher crypto primitives + kernel CSPRNG         | §1                   |  [ ]   |
| 💎  |   6   | cJSON DOM parser                                     | §1                   |  [/]   |
| 💎  |   7   | Mbed TLS freestanding port (record layer)            | §1, §2, §5            |  [ ]   |

> 💎 = parity work: matches what Windows 11 and Linux already do.
> ⭐ = exclusive work: Impossible OS is superior or first.

---

## 1. Freestanding String Library

- [x] Grep the entire kernel source for `memcpy`, `memset`, `strlen`, `strcmp`, `snprintf` etc.; document which are provided by compiler builtins (`__builtin_memcpy`) vs. missing vs. hand-rolled in callers
- [x] Map against the functions `panic.c`, `printk.h`, VFS, and drivers call -- the gap list drives the implementation priority

> **Gap analysis results (2026-03-27) -- historical; see Current state above for 2026-04-13 code-truth.**
> **Provided (weak symbols in `image.c` + `stb_truetype_impl.c` + `freestanding/string.h`):**
> - `memcpy`, `memset`, `memmove`, `memcmp`, `strlen` -- 2 duplicate implementations (image.c + stb_truetype_impl.c); should consolidate into `libc/string.c`
> **Compiler builtins used:**
> - `__builtin_memcpy`, `__builtin_memset` -- only in `image_save.c` (4 uses)
> **Missing entirely (no implementation, no builtin):**
> - `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strcat`, `strncat` -- 0 actual calls, but hand-rolled `str_eq()` in 5 files (klog.c, klog_disk.c, icon_store.c, ipc/shmem.c, blkdev.c)
> - `snprintf` / `vsnprintf` -- not available; `panic.c` has a comment noting "no snprintf in freestanding"; `klog.c` has `vformat_buf()` (private, supports %d %u %x %p %s %c only)
> - `atoi`, `strtol`, `strtoul` -- not available
> - `memchr`, `strstr`, `strchr`, `strrchr` -- not available
> **Priority for §1 (remaining):**
> 1. Consolidate `memcpy`/`memset`/`memmove`/`memcmp`/`strlen` into `libc/string.c` (eliminate 2 duplicate weak symbol sets)
> 2. Add `strcmp`/`strncmp` (replace 5 hand-rolled `str_eq()` functions)
> 3. Add `snprintf`/`vsnprintf` (promote `klog.c:vformat_buf()` to public API)
> 4. Add `strcpy`/`strncpy`/`strcat` (needed by future Win32 API + registry)
> 5. Add `atoi`/`strtoul` (needed by future config parsing + shell)

- [x] Create `src/libc/string.c` and `include/libc/string.h`; compile with `-ffreestanding -nostdlib -O2`
- [x] Memory operations: `memcpy`, `memmove`, `memset`, `memcmp`, `memchr` -- all implemented
- [x] String operations: `strlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strstr`, `strtol`, `strtoul`, `atoi` -- all implemented
- [x] BSD safe: `strlcpy`, `strlcat` -- always NUL-terminate
- [x] Removed duplicate weak symbols from `image.c` and `stb_truetype_impl.c`; updated `freestanding/string.h` to redirect to `libc/string.h`

- [x] `int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)` -- full implementation with: `%d`/`%i`, `%u`, `%x`/`%X`, `%o`, `%s`, `%c`, `%p`, `%%`, length modifiers (`l`/`ll`/`h`/`hh`/`z`), width, precision (`.*`), zero-padding, left-align (`-`), sign flags (`+`/` `). Returns total chars that *would* have been written.
- [x] `int snprintf(char *buf, size_t size, const char *fmt, ...)` -- thin varargs wrapper
- [ ] Update `panic.c` to use `snprintf` -- deferred (current hand-rolled formatter works; changing it during active development risks boot regression)
- [ ] Update `klog` / `printk` to use `vsnprintf` internally -- deferred (same reason; `vformat_buf` is battle-tested)

- [x] Commit: `"libc: freestanding string library -- memcpy/memset/str*, snprintf/vsnprintf"`

**Test checkpoint:** `snprintf(buf, 64, "%lld %p", (long long)-1, (void *)0x1000)` returns expected length; `strcmp`/`memcmp` unit vectors pass; link includes `libc/string.c` without duplicate weak symbols from `image.c`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 2. Complete Floating-Point Math Library

- [ ] Reconcile with existing `include/kernel/kmath.h` inlines today: `kmath_cos`, `kmath_acos`, `kmath_pow`, `kmath_sqrt` (used by stb_truetype); avoid duplicate symbols when promoting or moving APIs into `src/libc/math.c` (confirmed `include/kernel/kmath.h:36-135`).

- [ ] Document which functions `kmath.h` already provides: `kmath_fabs`, `kmath_floor`, `kmath_ceil`, `kmath_fmod`, `kmath_sqrt`
- [ ] Identify callers across the codebase to understand which new functions are urgently needed (stb_truetype, compositor, future audio synthesis)

- [ ] Add to `src/libc/math.c` and expose in `include/libc/math.h`; use polynomial approximations accurate to <= 1 ULP for the double range `[-pi, pi]`:
  - `double kmath_sin(double x)` -- Taylor series with range reduction
  - `double kmath_cos(double x)` -- `sin(pi/2 - x)` or paired Clenshaw
  - `double kmath_tan(double x)` -- `sin/cos` with infinity guard
  - `double kmath_asin(double x)` -- valid for `|x| <= 1`
  - `double kmath_acos(double x)` -- `pi/2 - asin(x)`
  - `double kmath_atan(double x)` -- Padé approximation
  - `double kmath_atan2(double y, double x)` -- four-quadrant atan
- [ ] `float` variants (`kmath_sinf`, etc.) -- cast to double, compute, cast back; sufficient for all current kernel uses

- [ ] `double kmath_exp(double x)` -- `e^x` via Horner polynomial + range reduction by `ln2`
- [ ] `double kmath_log(double x)` -- natural log via `atanh` identity
- [ ] `double kmath_log2(double x)` -- `log(x) / log(2)`
- [ ] `double kmath_log10(double x)` -- `log(x) / log(10)`
- [ ] `double kmath_pow(double base, double exp)` -- `exp(exp * log(base))` with special-case handling for integer exponents

- [ ] `double kmath_round(double x)` -- round half-away-from-zero
- [ ] `double kmath_trunc(double x)` -- truncate toward zero
- [ ] `double kmath_cbrt(double x)` -- cube root via Newton-Raphson

- [ ] Commit: `"libc: floating-point math -- sin/cos/tan/atan2/exp/log/pow, float variants"`

**Test checkpoint:** `kmath_sin(0)==0`, `kmath_cos(0)==1`, `kmath_atan2(1,1)` within 1e-9 of pi/4; `kmath_exp(1)` within 1e-9 of e; spot-check stb_truetype glyph raster unchanged vs baseline. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 3. LZ4 Block Compressor

- [ ] Vendor `lz4.c` + `lz4.h` from the official LZ4 repository (BSD-2 license, ~2000 lines) into `src/libs/lz4/`
- [ ] Compile with `-ffreestanding -nostdlib`; LZ4 has no `malloc` calls in its core API -- it operates entirely on caller-provided buffers
- [ ] Expose the block API:
  ```c
  int lz4_compress(const void *src, int src_size,
                   void *dst, int dst_capacity);      /* returns compressed size */
  int lz4_decompress(const void *src, int src_size,
                     void *dst, int dst_capacity);    /* returns original size */
  int lz4_compress_bound(int input_size);             /* worst-case output size */
  ```
- [ ] Buffer allocation responsibility: callers provide pre-allocated buffers; for output buffers > 4 KiB use `pmm_alloc_contiguous()`; document this in `include/libs/lz4.h`
- [ ] Wire into TODO-27 crash dump writer: replace the placeholder LZ4 call with the actual implementation (-> XREF `TODO-27-crash-dump-generation.md §6`)
- [ ] Wire into TODO-26 hibernation image writer (-> XREF `TODO-26-power-management.md §4`)
- [ ] Wire into `TODO-20-eif-full-implementation.md` §6 EIF compressed segments (-> XREF `TODO-20-eif-full-implementation.md` §6)

- [ ] For streaming use cases (large IXFS extents), vendor `lz4frame.c` from the same repository; provides the standard `.lz4` framing layer with content checksum (XXH32) and block independence flag
- [ ] `lz4f_compress_begin / update / end` / `lz4f_decompress` API

- [ ] Commit: `"libs: LZ4 block compressor, freestanding port, wired to crash dump + hibernate"`

**Test checkpoint:** 64 KiB random buffer round-trip through `lz4_compress` / `lz4_decompress` matches; `lz4_compress_bound` >= actual compressed size; TODO-27 writer calls real LZ4 API without placeholder. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 4. miniz Deflate / Inflate + ZIP

- [ ] Vendor `miniz.h` / `miniz.c` (MIT, ~6000 lines) into `src/libs/miniz/`
- [ ] Compile with `-ffreestanding -nostdlib -msse2`
- [ ] Redirect `malloc`/`free` -> `kmalloc`/`kfree` via `#define` overrides for the internal `tinfl` decompressor state struct (<= 4 KiB -- kmalloc safe)
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

- [ ] Compress a known 8 KiB buffer; decompress; verify byte-for-byte integrity
- [ ] Open a ZIP archive from memory; list entries; extract one file; verify content against a known SHA-256 (using Monocypher Blake2b from §5)

- [ ] Commit: `"libs: miniz deflate/inflate, ZIP archive reading"`

**Test checkpoint:** 8 KiB gzip round-trip identity; ZIP in-memory extract matches golden hash; no `kmalloc` for output >4 KiB without `pmm_alloc_contiguous`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 5. Monocypher Crypto Primitives + Kernel CSPRNG

- [ ] Vendor `monocypher.c` + `monocypher.h` (BSD-2, ~3000 lines) into `src/libs/monocypher/`
- [ ] Compile with `-ffreestanding -nostdlib`; Monocypher uses zero heap allocation -- all state is on the caller-provided stack or caller-provided buffers; no `malloc` redirects needed
- [ ] Expose the following primitives (keep names unchanged):
  - **ChaCha20** -- `crypto_chacha20_djb()` / `crypto_chacha20_x()` stream cipher; 256-bit key, 64-bit nonce
  - **Poly1305** -- `crypto_poly1305()` MAC; pairs with ChaCha20 for AEAD
  - **ChaCha20-Poly1305** -- `crypto_aead_lock()` / `crypto_aead_unlock()` authenticated encryption
  - **Blake2b** -- `crypto_blake2b()` cryptographic hash; 256 or 512-bit output
  - **Argon2id** -- `crypto_argon2()` memory-hard password hash; used by desktop login/password hashing (-> XREF `09-desktop-shell/TODO-06-security-accounts.md §2`)
  - **X25519** -- `crypto_x25519()` Diffie-Hellman key exchange
  - **Ed25519** -- `crypto_eddsa_sign()` / `crypto_eddsa_check()` signatures; used for EIF code signing (-> XREF `TODO-17-binary-system.md §17`)

- [ ] Create `src/kernel/csprng.c` and `include/kernel/csprng.h`:
  ```c
  void     csprng_init(void);              /* called from Phase 1 init */
  void     csprng_fill(void *buf, size_t len); /* fills with random bytes */
  uint64_t csprng_u64(void);               /* convenience -- random uint64_t */
  ```
- [ ] `csprng_init()` seeds from multiple entropy sources:
  1. Three `RDRAND` retries (Intel/AMD hardware RNG); if any succeed use the 64-bit value
  2. RDTSC XOR'd with the physical address of `csprng_init` (ASLR entropy)
  3. ACPI PM timer reading (adds ~20 bits of timing entropy)
  4. XOR all sources into a 256-bit seed; expand with Blake2b to produce the initial ChaCha20 key
- [ ] Continuous operation: maintain a global ChaCha20 state; `csprng_fill` calls `crypto_chacha20_x()` to generate output; re-key every 4 MiB of output (forward-secrecy)
- [ ] Share the RDRAND seed path with `TODO-10-kernel-security-hardening.md §11` (`__stack_chk_guard` canary init); `canary_init()` calls `csprng_u64()` after `csprng_init()` runs
- [ ] Replace `task_exec()` AT_RANDOM TSC fallback with `csprng_fill(rand_buf, 16)` (-> XREF `02-kernel-core/TODO-11-peb-teb-user-abi.md §13` follow-up). Until this lands, AT_RANDOM uses RDRAND on hardware and a TSC-mixed fallback on TCG; user binaries are `-fno-stack-protector` so this is not currently exploitable.
- [ ] Wire `csprng_u64()` into `TODO-20-eif-full-implementation.md` §8 so `load_base=0` EIF binaries choose a randomized base without a TSC fallback
- [ ] `SYS_GETRANDOM` syscall: `getrandom(buf, len, flags)` fills user buffer via `csprng_fill` + `copy_to_user`; add to SSDT (-> XREF `TODO-12-native-api-ssdt.md §5`)

- [ ] Commit: `"libs: Monocypher crypto, kernel CSPRNG (RDRAND + Blake2b), SYS_GETRANDOM"`

**Test checkpoint:** `crypto_aead_lock`/`unlock` round-trip on test vector; `csprng_fill` two 32 B buffers differ; Blake2b self-test vector passes; `RDRAND` path exercised on hardware when present. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 6. cJSON DOM Parser

- [x] Vendor `cJSON.c` + `cJSON.h` (MIT v1.7.18, 3443 lines) into `src/libs/cjson/`
- [x] Compile with `-ffreestanding -nostdlib` + SSE2 for float; freestanding shim replaces all stdlib headers
- [x] Allocator redirected via compile-time macros (`malloc` -> `kmalloc`, `free` -> `kfree`, `realloc` -> `krealloc`):
  ```c
  cJSON_Hooks hooks = { .malloc_fn = kmalloc, .free_fn = kfree };
  cJSON_InitHooks(&hooks);
  ```
- [ ] Memory note: parsed JSON trees can grow large; add a max-depth guard (`cJSON_SetMaxDepth(32)`) and validate total node count before full parse to avoid heap exhaustion from malformed documents

- [ ] Expose a thin kernel wrapper `src/kernel/json.c` / `include/kernel/json.h`:
  ```c
  cJSON *json_parse(const char *text);         /* wrapper over cJSON_Parse */
  cJSON *json_get(cJSON *obj, const char *key); /* cJSON_GetObjectItem */
  const char *json_str(cJSON *item);            /* cJSON_GetStringValue */
  int    json_int(cJSON *item);                 /* cJSON_GetNumberValue as int */
  char  *json_print(cJSON *obj);               /* cJSON_PrintUnformatted */
  void   json_free(cJSON *obj);                /* cJSON_Delete */
  ```
- [x] Kernel wrapper API created: `json_parse()`, `json_get()`, `json_str()`, `json_int()`, `json_double()`, `json_bool()`, `json_print()`, `json_free()` in `src/kernel/json.c` + `include/kernel/json.h`
- [ ] Use for: desktop theme files (`theme.json`), settings persistence, update manifests, NTP server list
- [ ] Test: parse `{"os": "Impossible", "build": 1024, "debug": true}`; verify all fields extract correctly

- [x] Commit: `"libs: cJSON DOM parser, json_parse/get/print wrapper"`

**Test checkpoint:** Sample JSON parses; `json_int`/`json_str` match literals; `cJSON_SetMaxDepth` enforced on fuzz input without heap exhaustion; `json_print` round-trip stable. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 7. Mbed TLS Freestanding Port

> [!IMPORTANT]
> **Scope overlap -- `07-networking/TODO-03-http-tls.md` §3** also claims "Mbed TLS Kernel Port". This §8 is the canonical freestanding port; `07-networking/TODO-03 §3` must depend on this section being complete and should not re-implement the library itself. When `07-networking/TODO-03` is next validated, update its §3 to reference this TODO-03 §8 as its prerequisite.

- [ ] Clone Mbed TLS 3.x source into `src/libs/mbedtls/` (Apache-2.0); create a `mbedtls_config.h` that enables only the required subset:
  - `MBEDTLS_AES_C` + `MBEDTLS_GCM_C` -- AES-128-GCM / AES-256-GCM
  - `MBEDTLS_SHA256_C` + `MBEDTLS_SHA512_C` -- hash primitives
  - `MBEDTLS_RSA_C` + `MBEDTLS_PKCS1_V21` -- RSA-OAEP for certificate public key operations
  - `MBEDTLS_ECDSA_C` + `MBEDTLS_ECP_DP_SECP256R1_ENABLED` -- ECDSA P-256
  - `MBEDTLS_SSL_TLS_C` + `MBEDTLS_SSL_PROTO_TLS1_3` -- TLS 1.3 record layer
  - `MBEDTLS_CTR_DRBG_C` backed by the kernel CSPRNG (§5)
  - **Disable:** `MBEDTLS_NET_C`, `MBEDTLS_TIMING_C`, `MBEDTLS_FS_IO` -- all OS-dependent components
- [ ] Platform abstraction layer (`include/libs/mbedtls/mbedtls_platform.h`):
  - `mbedtls_calloc` -> `kmalloc` (zero-initialised, <= 4 KiB)
  - `mbedtls_free` -> `kfree`
  - Large crypto scratch buffers (key agreement, certificate parsing) use `pmm_alloc_contiguous()`; wrap via custom `mbedtls_platform_set_calloc_free`
  - `mbedtls_printf` -> `klog(LOG_DEBUG, "tls", ...)`
  - Entropy source -> `csprng_fill()` (§5) fed into `mbedtls_entropy_add_source`

- [ ] Build the minimal Mbed TLS subset with the kernel flags: `-ffreestanding -nostdlib -fno-stack-protector -mno-red-zone -O2 -g` -- confirm zero link errors
- [ ] Implement `tls_selftest()`: run `mbedtls_aes_self_test(1)` and `mbedtls_sha256_self_test(1)` -- pass means AES and SHA-256 are functionally correct in freestanding mode
- [ ] The full TLS session layer (handshake, certificates, SNI) is implemented in `07-networking`; this section only validates the port

- [ ] Commit: `"libs: Mbed TLS 3.x freestanding port, AES/SHA256/TLS1.3 config, CSPRNG entropy hook"`

**Test checkpoint:** `mbedtls_aes_self_test(1)` and `mbedtls_sha256_self_test(1)` return 0; kernel links with `src/libs/mbedtls/` objects only via this port; serial shows entropy hook OK. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11      | 🐧 Linux      | 🚀 Impossible OS |
| --- | -------------------------- | ------------- | ------------- | ---------------- |
| 💎 | snprintf safe              | ✅ Rtl safe   | ✅ vsprintf   | ✅ §1 libc       |
| 💎 | Core string ops            | ✅ Rtl mem    | ✅ string.c   | ✅ §1 libc       |
| 💎 | Float math lib             | ✅ CRT        | ✅ libm       | ⚠️ §2 partial (`kmath.h` cos/pow/sqrt; no `libm`) |
| 💎 | LZ4 in kernel              | ⚠️ Xpress     | ✅ lib/lz4    | ⬜ §3            |
| 💎 | Deflate zlib               | ✅ Rtl LZNT1 | ✅ zlib       | ⬜ §4 miniz      |
| 💎 | ZIP read                   | ✅ Shell      | ✅ unzip      | ⬜ §4            |
| 💎 | ChaCha Poly AEAD           | ✅ BCrypt     | ✅ chacha     | ⬜ §5            |
| 💎 | Blake2b                    | ✅ no native  | ✅ blake2b    | ⬜ §5            |
| 💎 | Argon2id                   | ✅ KDF API    | ✅ argon2     | ⬜ §5            |
| 💎 | X25519 ECDH                | ✅ BCrypt     | ✅ ecdh       | ⬜ §5            |
| 💎 | Ed25519                    | ✅ BCrypt     | ✅ eddsa      | ⬜ §5            |
| 💎 | Kernel CSPRNG              | ✅ BCrypt     | ✅ random.c   | ⬜ §5            |
| 💎 | JSON in kernel             | ❌ user COM   | ❌ none std   | ✅ §6 cJSON      |
| 💎 | TLS record crypto          | ✅ SChannel   | ✅ ktls       | ⬜ §7 mbed       |
| ⭐ | One ring-0 CSPRNG façade (TLS seed + hiber + dumps + klog HMAC) | ⚠️ many BCrypt entry points | ⚠️ `get_random_bytes` + drivers | ⬜ §5 `csprng_fill` |

After §1 through §7, freestanding libc and these libs unblock LZ4, miniz, crypto, JSON, and the TLS record layer for hibernation, dumps, IXFS, and `07-networking` HTTPS.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_klibs()` (see `src/kernel/test/test_runner.c` and `include/kernel/test/test.h`). Use `test_suite_register_cat(..., TEST_CAT_EXEC)` for each case.
> Boot tests run with `debug=1` or `test=1` in `boot.conf`.

- [ ] Create `src/kernel/test/test_klibs.c` with:
  - `snprintf(buf, 32, "%d", 42)` -> `"42"`, returns 2
  - `snprintf(buf, 32, "%s %s", "hello", "world")` -> `"hello world"`
  - `snprintf(buf, 32, "0x%x", 0xDEAD)` -> `"0xdead"`
  - `snprintf(buf, 4, "hello")` -> `"hel"` (truncated, NUL-terminated)
  - `strlen("test")` -> 4
  - `strcmp("abc", "abc")` -> 0; `strcmp("abc", "abd")` -> negative
  - `memcpy` 256-byte round-trip -> destination matches source
  - `memset(buf, 0xAA, 64)` -> all bytes are 0xAA
  - `memcmp(a, a, 32)` -> 0; `memcmp(a, b, 32)` -> non-zero when different
  - LZ4 compress/decompress round-trip: 1 KiB input -> compressed -> decompressed matches original
  - CRC32: known input -> matches precomputed CRC
  - SHA-256: `"abc"` -> known hash `ba7816bf...`
  - AES-128-ECB: encrypt + decrypt round-trip -> plaintext matches
- [ ] Add `extern void test_register_klibs(void);` in `test_runner.c`, call `test_register_klibs()` from `test_runner_init()`
- [ ] Until §3 exists: register LZ4 round-trip test as `TEST_SKIP` or behind `#if 0`; until §4/§5 ship crypto: skip CRC32 / SHA-256 / AES bullets or use `TEST_SKIP` with explicit reason (avoid linking missing symbols)
- [ ] Commit: `"test: add kernel libraries test suite"`

---

## Verification

- [ ] **snprintf**: `snprintf(buf, sizeof buf, "%08x %s %d", 0xDEAD, "os", 42)` -> `"0000dead os 42"` byte-for-byte; truncation test: `snprintf(buf, 5, "hello world")` -> `"hell\0"`, returns `11`.
- [ ] **Math**: `kmath_sin(0) == 0.0`, `kmath_cos(0) == 1.0`, `kmath_atan2(1.0, 1.0)` ~ `0.7854` (pi/4 +/- 1e-9), `kmath_exp(1.0)` ~ `2.71828` (+/- 1e-9), `kmath_sqrt(4.0) == 2.0`.
- [ ] **LZ4**: compress a 64 KiB zeroed buffer; verify compressed size < original; decompress; byte-for-byte equal; `lz4_compress_bound(65536)` is the upper bound on compressed size.
- [ ] **miniz deflate**: compress 8 KiB string; decompress; verify identity.
- [ ] **miniz ZIP**: open a 3-file ZIP from memory; enumerate entries; extract one file; verify content.
- [ ] **Monocypher AEAD**: encrypt `"hello kernel"` with a random key; decrypt; verify plaintext matches; flip one byte in ciphertext -- `crypto_aead_unlock` must return -1 (authentication failure).
- [ ] **CSPRNG**: `csprng_fill(buf1, 32)` and `csprng_fill(buf2, 32)` must produce different outputs; `csprng_u64()` called 1000 times must show no repeated values (birthday bound test).
- [ ] **cJSON**: parse `{"build": 1024, "name": "Impossible OS", "debug": false}`; `json_int(json_get(root, "build")) == 1024`; `strcmp(json_str(json_get(root, "name")), "Impossible OS") == 0`.
- [ ] **Mbed TLS selftest**: `mbedtls_aes_self_test(1)` and `mbedtls_sha256_self_test(1)` both return 0 in the boot log.
- [ ] **Build check**: `bash scripts/build.sh clean` -> `=== BUILD OK ===` with all library files compiled under `-ffreestanding -nostdlib`.
- [ ] **Platforms:** run verification matrix on QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
- [ ] Commit: `"libs: snprintf/vsnprintf, kmath, LZ4, miniz, Monocypher/CSPRNG, cJSON, Mbed TLS freestanding ports"`

**Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec)

---

## History

| Date | Action | Summary |
| --- | --- | --- |
| 2026-04-10 | validate | Removed ### N.M and model tags, ASCII `->`/`<=` sweep, Depends On uses §, fixed HTTP-TLS path to `07-networking/TODO-03-http-tls.md` §3, compact OS table after Implementation Order, §1/§4 status [x], per-section **Test checkpoint** + four platforms, Unit Tests -> `TEST_CAT_EXEC` + `test_runner.c`, added `scripts/debug/kernel/run-exec-tests.bat`, back-XREF on `TODO-32` Inputs, Goal aligned with libc snprintf present. **Parity:** Win11+Linux-strong rows covered by §2 through §7 except JSON row (Impossible leads). **Flag:** CRC32/AES unit bullets need real APIs from §4/§5 before tests compile. |
| 2026-04-13 | validate | Moved `## OS Comparison` after §7 and before `## Unit Tests`; each §1 through §7 now ends with `- [ ] Commit:` / `- [x] Commit:` before `**Test checkpoint:**`; Inputs XREF fixes (T10 stack canary §12, T15 AppContainer §14, Argon2 -> D09 `TODO-06-security-accounts.md` §2); OS table re-padded; IMPORTANT callout blank line removed; §1 gap-analysis blockquote empty `>` lines removed; prior History row date un-bolded. **Parity:** unchanged intent (⬜ rows tracked in §2 through §7). **Blocked deps:** Implementation Order §4 through §7 still depend on open sections on source TODOs. |
| 2026-04-13 | gap-analysis | 6 web + 2 Learn fetches (CNG alg IDs incl. ChaCha20-Poly1305 + SHA3 24H2; RtlCompressBuffer LZNT1/Xpress); code-truth: §1/§6 partial, §2 partial in `kmath.h` only, no lz4/miniz/monocypher/mbedtls dirs, no `test_register_klibs`; IO row §6 -> `[/]`; Outcome Argon2i to Argon2id; OS ⭐ CSPRNG façade row; Inputs D12 T01 COMPLEMENT; Unit Tests skip-gate; INDEX snprintf stale line; §1 gap block marked historical; INDEX line 47 substring; D12 T01 code-truth note; §2 kmath reconcile bullet. |
| 2026-04-14 | validate | Idempotent pass: OS Comparison after §7 before Unit Tests; Commit before Test checkpoint §1-§7; fixed gap-analysis History pipe; parity footnote lines 63-64 colon style; Inputs paths and XREF targets checked; run-exec-tests.bat present. **Parity:** ⬜ rows still §2-§7. **Flag:** external IO deps open; §6 [/] per partial cJSON. |
