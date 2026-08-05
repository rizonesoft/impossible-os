---
schema_version: 1
id: kernel-libraries-sdk
domain: 12-user-platform-sdk
status: active
title: "TODO-01 -- Kernel Embedded Libraries"
---

# TODO-01 -- Kernel Embedded Libraries

> **Goal:** Port and consolidate all embedded third-party libraries -- string/math libc,
> miniz, monocypher, cJSON, Mbed TLS, and STB -- into `src/libs/` with freestanding memory
> redirects and host-side integration tests. This is a foundational prerequisite consumed by
> 06-networking, 08-desktop-shell, 10-platform-services, and 10-apps.

> [!IMPORTANT]
> **Source migration:** All sections from
> `todo-old/010-Kernel-Foundations/TODO-027-Kernel-Libraries.md` are migrated here.
>
> **Code-truth (2026-04-13):** D02 T20 section 1 snprintf/vsnprintf and section 6 cJSON plus json.c wrappers are in-tree; T20 sections 3 through 5 and 7 remain open. This file stays the ZIP writer and stream extension on top of T20 section 4.
>
> **Overlap -- miniz:** `02-kernel-core/TODO-03-kernel-libraries.md §6` already specifies
> the miniz port to `src/libs/miniz/` and the ZIP reader API (`zip_open/entry_count/find/
> read/close`). This TODO-01 §3 extends that work with the **ZIP writer** (`mz_zip_writer_*`)
> and **stream API** (`mz_deflate`/`mz_inflate`) that §4 does not cover. Do not
> re-port the reader; treat §4 as the prerequisite.
>
> **Overlap -- math:** `include/kernel/kmath.h` already has `fabs`, `floor`, `ceil`, `fmod`,
> `sqrt`, `pow`, `cos`, `acos` as inline functions for stb_truetype. §2 extends this with the
> missing functions; do not remove the existing implementations -- only add to them.
>
> **Overlap -- STB:** `stb_truetype.h` and `stb_image_write.h` are already present in
> `include/`; `src/kernel/gfx/stb_truetype_impl.c` is the implementation shim. §7 audits
> and consolidates -- do not re-implement.
>
> **monocypher CSPRNG** (`SYS_GETRANDOM`) is security-critical: RDRAND entropy + ChaCha20
> stream. Treat §4 as `[Opus]`. **Mbed TLS** TLS 1.2 client + certificate validation is
> equally security-critical -- treat §6 as `[Opus]`.

---

## Inputs

- `include/kernel/kmath.h` -- existing: `fabs`, `floor`, `ceil`, `fmod`, `sqrt`, `pow`, `cos`, `acos`; §2 adds remainder
- `src/kernel/gfx/stb_truetype_impl.c` -- existing STB shim; §7 consolidates
- `include/stb_truetype.h`, `include/stb_image_write.h` -- already present; §7 audits
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous()` -- §3 §4 large decompress/key buffers
- `include/kernel/mm/heap.h` or equivalent -- `kmalloc`, `kfree` -- malloc redirect in §3 §5 §6
- `include/kernel/cpuid.h` -- `CPU_FEATURE_RDRAND` flag check -- §4 CSPRNG seed
- `include/kernel/syscall.h` -- syscall number table; add `SYS_GETRANDOM` -- §4
- `02-kernel-core/TODO-03-kernel-libraries.md §6` (→ XREF) -- miniz port + ZIP reader prerequisite -- §5
- `06-networking/TODO-03-http-tls.md` (→ XREF) -- Mbed TLS TLS 1.2 consumer -- §4
- `10-platform-services/TODO-07` (→ XREF) -- monocypher consumer (WiFi WPA2, SSH crypto) -- §4
- `include/kernel/klog.h` -- `klog()` for library init diagnostics -- §8

---

## Outcome

`src/libs/` contains six vendored, freestanding-safe library trees: `libc/` (string + math),
`miniz/` (deflate + ZIP read/write), `monocypher/` (all primitives + CSPRNG), `cjson/`
(JSON parse/print), `mbedtls/` (TLS 1.2 client subset), `stb/` (image + truetype shims).
`scripts/test-libs.sh` runs all integration tests and exits non-zero on failure.
`src/libs/README.md` documents license, version, and source URL for every library.

---

## Implementation Order

| Step | Section                               | 💎/⭐ | Dependency                               |
| ---- | ------------------------------------- | --- | ---------------------------------------- |
| 1    | String Library Consolidation          | 💎   | `kmalloc`, `kfree`; freestanding build flags |
| 2    | Math Library Extension                | 💎   | existing `kmath.h`; `-msse2` flag confirmed |
| 3    | miniz (Writer + Stream Extension)     | 💎   | §4 (reader/port) must be done first      |
| 4    | monocypher + CSPRNG + SYS_GETRANDOM   | ⭐   | §1 string utils; `RDRAND` CPU feature check |
| 5    | cJSON                                 | 💎   | §1 string lib; `kmalloc`/`kfree`         |
| 6    | Mbed TLS Subset                       | 💎   | §2 math; §4 monocypher entropy; §1 string |
| 7    | STB Consolidation + `stb_image_write` | 💎   | existing `stb_truetype_impl.c`; `image_save_png` consumers |
| 8    | Library Integration Tests + README    | ⭐   | §1–§7 all complete                       |

---

## 1. String Library Consolidation `[Sonnet]`

**Output files:** `src/libs/libc/string.c`, `include/libc/string.h`

- [ ] **Audit** existing string functions: scan `src/kernel/` for `memset`, `memcpy`, `strlen`, `snprintf` definitions; list duplicates across `panic.c`, `log.c`, and any other files
- [ ] **Memory functions** (implement or consolidate):
  - `memset(dst, c, n)`, `memcpy(dst, src, n)`, `memmove(dst, src, n)`, `memcmp(a, b, n)`, `memchr(s, c, n)`
- [ ] **String functions**:
  - `strlen`, `strcpy`, `strncpy`, `strcat`, `strncat`
  - `strcmp`, `strncmp`, `strstr`, `strchr`, `strrchr`, `strcasestr`
  - `strtol`, `strtoul`, `strtoll`, `atoi`
  - `itoa(int val, char *buf, int base)` -- custom; base 10/16 only required
- [ ] **Printf functions** (`src/libs/libc/printf.c`):
  - `int snprintf(char *buf, size_t size, const char *fmt, ...)` -- safe, caps at `size-1`, null-terminates
  - `int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)` -- core implementation
  - Format specifiers: `%d`, `%i`, `%u`, `%x`, `%X`, `%o`, `%s`, `%c`, `%p`, `%%`, `%zu`, `%ld`, `%llu`; width + precision + left-justify flag (`%-10s`, `%08x`)
  - **No heap allocation** -- purely stack-based formatting
- [ ] All files compiled with `-ffreestanding -nostdlib -nostdinc`; no `<string.h>` or `<stdio.h>` includes
- [ ] Update existing callers to include `"libc/string.h"` instead of scattered private prototypes
- [ ] **Deduplication**: remove any `static inline` copies from old kernel files once `libc/string.h` is included

---

## 2. Math Library Extension `[Sonnet]`

> Existing `kmath.h` has: `fabs`, `floor`, `ceil`, `fmod`, `sqrt`, `pow`, `cos`, `acos`.
> Add the following without removing existing implementations.

**Output files:** `src/libs/libc/math.c`, additions appended to `include/kernel/kmath.h`

- [ ] **Missing functions** to add (port from musl libc or OpenLibm, MIT-compatible):
  - `kmath_sin(x)` -- Taylor series (already near-complete from `kmath_cos` symmetry)
  - `kmath_tan(x)` -- `sin(x)/cos(x)` with zero-check
  - `kmath_asin(x)` -- `pi/2 - acos(x)` using existing `kmath_acos`
  - `kmath_atan(x)` -- polynomial approximation; range reduce via `atan(x) = pi/2 - atan(1/x)` for |x|>1
  - `kmath_atan2(y, x)` -- four-quadrant atan using `kmath_atan`
  - `kmath_exp(x)` -- Taylor series `sum(x^n/n!)`, range-reduce via `e^(k + r)` where k=integer part
  - `kmath_log(x)` -- natural log; use existing `ln_base` polynomial from `kmath_pow` (extract and expose)
  - `kmath_log2(x)` -- `kmath_log(x) / kmath_log(2.0)`
  - `kmath_log10(x)` -- `kmath_log(x) / kmath_log(10.0)`
  - `kmath_cbrt(x)` -- Newton-Raphson cube root (`x = x/3 * (x + x/3 * r^2)^(-1)`)
  - `kmath_round(x)` -- `floor(x + 0.5)` for positive; `ceil(x - 0.5)` for negative
  - `kmath_trunc(x)` -- cast to `long` and back
  - `kmath_remainder(x, y)` -- IEEE remainder: `x - round(x/y) * y`
- [ ] Add constants: `KMATH_PI`, `KMATH_E`, `KMATH_SQRT2`, `KMATH_LN2`
- [ ] Compile with `-msse2` (enabled for kernel); no libm dependency; `static inline` in header for trivial wrappers, `src/libs/libc/math.c` for non-trivial (sin/exp/log)
- [ ] **Precision target**: within `1e-6` relative error for all functions over their standard domain; sufficient for Calculator, PDF text placement, and general app math

---

## 3. miniz (ZIP Writer + Stream Extension) `[Sonnet]`

> → XREF: `02-kernel-core/TODO-03-kernel-libraries.md §6` -- miniz port to `src/libs/miniz/`
> and ZIP reader (`zip_open/entry_count/find/read/close`) must be complete first.
> This section adds only the ZIP **writer** and **stream** APIs.

**Output files:** `src/libs/miniz/zip_write.c`, `include/libs/miniz_write.h`

- [ ] **ZIP writer API** (`zip_archive_t` -- backing type wraps `mz_zip_archive`):
  - [ ] `zip_archive_t *zip_create(const char *path)` -- `mz_zip_writer_init_heap(z, 0, 0)`; store VFS path for `zip_close_write`
  - [ ] `int zip_add_file(zip_archive_t *a, const char *filepath, const char *entry_name)` -- `vfs_open` + `vfs_read` into `pmm_alloc_contiguous` buffer; `mz_zip_writer_add_mem(a, entry_name, data, size, MZ_DEFAULT_LEVEL)`; free buffer
  - [ ] `int zip_add_mem(zip_archive_t *a, const char *entry_name, const void *data, size_t size)` -- direct in-memory add
  - [ ] `int zip_delete_entry(zip_archive_t *a, const char *entry_name)` -- rebuild: iterate, skip matching entry, re-add all others into new writer, swap
  - [ ] `int zip_close_write(zip_archive_t *a)` -- `mz_zip_writer_finalize_heap_archive` → buffer; `vfs_create(path)` + `vfs_write(buf, size)`; free buffer + `mz_zip_writer_end`
- [ ] **Stream API** (exposed from miniz directly, re-declared in `include/libs/miniz.h`):
  - [ ] `mz_stream_deflate_init/push/end` wrappers around `mz_deflate_*`
  - [ ] `mz_stream_inflate_init/push/end` wrappers around `mz_inflate_*`
  - [ ] Used by HTTP gzip (`Content-Encoding: gzip` in 06-networking) and IXFS block compression
- [ ] **Memory redirect** (if not already done in §4): `#define MZ_MALLOC(sz) kmalloc(sz)`, `MZ_FREE(p) kfree(p)`, custom `MZ_REALLOC` shim using `kmalloc` + `memcpy` + `kfree`; buffers > 4 KB → `pmm_alloc_contiguous`
- [ ] **Verification**: compress a 64 KB zero-filled buffer → decompress → compare byte-for-byte; open existing 3-file ZIP in-memory → list entries → extract one; create new ZIP → add 2 files → close → re-open as reader → verify entry count

---

## 4. monocypher + CSPRNG + SYS_GETRANDOM `[Opus]`

> Security-critical: RDRAND-seeded entropy, ChaCha20 stream CSPRNG, and Ed25519 sign/verify
> have no prior Impossible OS precedent and are used by SSH (§4), WiFi WPA2, and the
> credential store. `SYS_GETRANDOM` is a new kernel–userspace boundary.

**Output files:** `src/libs/monocypher/monocypher.c`, `include/libs/monocypher.h`,
`src/libs/monocypher/csprng.c`, `include/libs/csprng.h`

- [x] OWNERSHIP RESOLVED (2026-06-12): `02-kernel-core/TODO-03` §5 shipped first and owns Monocypher + CSPRNG + `NtGetRandom`; this section is a consumer (SDK headers + user-mode `getrandom` wrapper only; reconcile sub-items at next validate pass)
- [ ] **Port monocypher** (BSD-2-Clause, ~3000 lines, v4.x):
  - [ ] Vendor `monocypher.c` + `monocypher.h` into `src/libs/monocypher/`
  - [ ] Zero malloc needed -- all state in caller-provided buffers; verify no `malloc`/`free` calls in source
  - [ ] Compile with `-ffreestanding -nostdlib`; no standard headers; replace any `assert` with `kpanic`
- [ ] **Exposed API** (re-declared in `include/libs/monocypher.h`):
  - `crypto_chacha20_*`: `init`, `x_init` (XChaCha20), `update`, `final`
  - `crypto_poly1305_*`: `init`, `update`, `final` → 16-byte MAC
  - `crypto_aead_lock` / `crypto_aead_unlock`: ChaCha20-Poly1305 AEAD (used by SSH)
  - `crypto_blake2b`, `crypto_blake2b_init/update/final`: 64-byte hash (HKDF base)
  - `crypto_argon2i(work_area, nb_blocks, nb_iterations, nb_lanes, ...)`: memory-hard KDF (password hashing)
  - `crypto_x25519(shared, your_secret, their_public)`: Diffie-Hellman (used by SSH ECDH)
  - `crypto_ed25519_public_key`, `crypto_ed25519_sign`, `crypto_ed25519_check`: keypair + sign/verify
- [ ] **Kernel CSPRNG** (`src/libs/monocypher/csprng.c`):
  - [ ] `csprng_fill(uint8_t *buf, size_t len)`:
    - [ ] Seed entropy pool: 32-byte seed from `RDRAND` -- loop 4× `__asm__ volatile("rdrand %0" : "=r"(val))` (8 bytes each); check carry flag; if RDRAND unavailable (check `cpuid_get()` RDRAND flag), mix `system_get_ticks()` + stack pointer + PML4 address as fallback
    - [ ] Derive keystream: `crypto_chacha20_x_init(&ctx, seed, nonce)` → `crypto_chacha20_update(&ctx, buf, len)` → `crypto_chacha20_final(&ctx)`
    - [ ] Re-key after every 64 KB generated (forward secrecy): generate 32 new seed bytes from current stream, reinitialize context
  - [ ] `SYS_GETRANDOM` syscall: `sys_getrandom(void *buf, size_t len, uint32_t flags)` → validates buf pointer is user-mode address range; calls `csprng_fill`; returns bytes written; add to `include/kernel/syscall.h` as `SYS_GETRANDOM = {next free number}`
- [ ] **Verification**: Blake2b of `"test"` string → compare against published test vector `0x928b20366943e2...`; `crypto_x25519` key exchange: generate two keypairs, derive shared secret from both sides → must match; `csprng_fill` 1 KB → no zero runs > 8 bytes (statistical sanity)

---

## 5. cJSON `[Sonnet]`

**Output files:** `src/libs/cjson/cjson.c`, `include/libs/cjson.h`

- [ ] **Port cJSON** (MIT, ~2000 lines, v1.7.x):
  - [ ] Vendor `cJSON.c` + `cJSON.h` into `src/libs/cjson/`; rename to lowercase `cjson.c/h` for consistency
  - [ ] Memory redirect: call `cJSON_InitHooks(&hooks)` at kernel init with `{ .malloc_fn = kmalloc, .free_fn = kfree }` -- no `realloc` in cJSON's default path
  - [ ] Set `CJSON_NESTING_LIMIT 32` at compile time to cap stack depth and memory usage
  - [ ] Compile with `-ffreestanding`; replace any `strtod` with `(double)strtol` (integer JSON only) or keep `strtod` if libc provides it; `snprintf` provided by §1
- [ ] **Exposed API** (re-declared in `include/libs/cjson.h`):
  - `cJSON *cJSON_Parse(const char *value)` -- returns root or NULL
  - `cJSON *cJSON_GetObjectItem(const cJSON *object, const char *key)` -- case-insensitive lookup
  - `cJSON *cJSON_GetArrayItem(const cJSON *array, int index)`
  - `int cJSON_GetArraySize(const cJSON *array)`
  - `char *cJSON_Print(const cJSON *item)` -- kmalloc'd string; caller must `kfree`
  - `char *cJSON_PrintUnformatted(const cJSON *item)`
  - `void cJSON_Delete(cJSON *item)` -- recursive free
  - `cJSON *cJSON_CreateObject()`, `cJSON *cJSON_CreateArray()`, `cJSON *cJSON_CreateString(const char *)`, `cJSON *cJSON_CreateNumber(double)`, `cJSON *cJSON_CreateBool(int)`
  - `cJSON_AddItemToObject`, `cJSON_AddItemToArray` -- for building JSON
- [ ] **Depth limit**: any parse that exceeds 32 nesting levels returns NULL + serial log `"[cJSON] max depth exceeded"`
- [ ] **Verification**: parse `{"version":"1.0","name":"Impossible OS","flags":[1,2,3]}`; verify `cJSON_GetObjectItem(..., "name")->valuestring == "Impossible OS"`; verify array size = 3; `cJSON_Print` → round-trip parse → re-verify; `cJSON_Delete` → no memory leak check via PMM frame delta

---

## 6. Mbed TLS Subset `[Opus]`

> Security-critical TLS 1.2 client: certificate chain validation, RSA key exchange, AES-GCM
> record encryption. No prior Impossible OS TLS implementation exists.
> → XREF: `06-networking/TODO-03-http-tls.md` -- primary consumer.

**Output files:** `src/libs/mbedtls/` (selected source files), `include/libs/mbedtls/`

- [ ] **Port Mbed TLS 3.x** (Apache-2.0):
  - [ ] Download and vendor only required modules into `src/libs/mbedtls/`:
    `aes.c`, `aes_wrap.c`, `sha256.c`, `sha512.c`, `md.c`, `rsa.c`, `bignum.c`,
    `entropy.c`, `ctr_drbg.c`, `ssl_client.c`, `ssl_msg.c`, `ssl_tls.c`,
    `x509_crt.c`, `x509.c`, `pem.c`, `base64.c`, `pk.c`, `pk_wrap.c`,
    `ecp.c`, `ecp_curves.c`, `ecdh.c`, `ecdsa.c` (for ECDHE-RSA)
  - [ ] `mbedtls_platform_set_calloc_free(kmalloc_shim, kfree)` -- redirect at `mbedtls_init()`; provide `calloc` shim: `kmalloc(n*size)` + `memset(0)`
  - [ ] Entropy source: wire `mbedtls_entropy_add_source` → `csprng_fill` from §4
  - [ ] Disable unneeded features via `mbedtls_config.h`: no filesystem, no threading, no DTLS, no SSLv3/TLSv1.0/TLSv1.1; enable TLS 1.2 only
  - [ ] `MBEDTLS_PLATFORM_SNPRINTF_MACRO` → `snprintf` from §1
- [ ] **Exposed API** (thin wrapper in `include/libs/tls_client.h`):
  - `tls_ctx_t *tls_connect(int tcp_fd, const char *hostname)` -- creates Mbed TLS context, performs handshake
  - `int tls_write(tls_ctx_t *ctx, const uint8_t *data, size_t len)` -- TLS record send
  - `int tls_read(tls_ctx_t *ctx, uint8_t *buf, size_t max)` → bytes read
  - `void tls_close(tls_ctx_t *ctx)` -- alert + free
  - `int tls_set_ca_cert(tls_ctx_t *ctx, const uint8_t *pem, size_t len)` -- load trust anchor
- [ ] **Certificate bundle**: vendor Mozilla CA bundle as `src/libs/mbedtls/cacert.pem`; embed as `const uint8_t g_ca_bundle[]` in `cacert_bundle.c`; loaded at `mbedtls_init()`
- [ ] **Cipher suites** (restrict to safe set): TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, TLS_RSA_WITH_AES_128_GCM_SHA256
- [ ] **Verification**: TLS connect to `example.com:443` (if network available in QEMU); failing that: run Mbed TLS unit tests for AES-GCM and SHA-256 on host-side test binary; verify known AES-256-GCM test vector from NIST SP 800-38D

---

## 7. STB Consolidation + `stb_image_write` `[Sonnet]`

> `stb_truetype.h` and `stb_image_write.h` are already present in `include/`. This section
> audits, consolidates, and verifies memory redirects.

**Output directory:** `src/libs/stb/`

- [ ] **Audit existing shim** (`src/kernel/gfx/stb_truetype_impl.c`):
  - [ ] Verify `STBTT_malloc(x,u)` → `kmalloc(x)` and `STBTT_free(x,u)` → `kfree(x)` are defined before `#define STB_TRUETYPE_IMPLEMENTATION`
  - [ ] Verify no `assert.h` include (replace `STBTT_assert` with `kpanic(...)` if needed)
- [ ] **Create `src/libs/stb/stb_image_impl.c`** (consolidate from wherever `stb_image` is currently instantiated):
  - [ ] `#define STBI_MALLOC(sz) kmalloc(sz)`, `STBI_FREE kfree`, `STBI_REALLOC` shim
  - [ ] `#define STBI_NO_STDIO` (no file I/O -- use `vfs_read` before passing to `stb_image`)
  - [ ] `#define STBI_ONLY_JPEG`, `STBI_ONLY_PNG`, `STBI_ONLY_BMP`, `STBI_ONLY_GIF` -- include only formats in use
  - [ ] `#define STB_IMAGE_IMPLEMENTATION`; include `"stb_image.h"` (if not in include/ yet, add it)
- [ ] **`stb_image_write.h` verification**: already in `include/`; create `src/libs/stb/stb_image_write_impl.c`:
  - [ ] `#define STBIW_MALLOC kmalloc`, `STBIW_FREE kfree`, `STBIW_REALLOC` shim
  - [ ] `#define STB_IMAGE_WRITE_IMPLEMENTATION`
  - [ ] Used by `image_save_png()` and `image_save_bmp()` in `include/kernel/image.h`
- [ ] **Move/symlink** `stb_truetype_impl.c` into `src/libs/stb/` (update Makefile to reflect new path)
- [ ] **`src/libs/stb/` Makefile target**: compile all three shim `.c` files as `stb.o` linked into kernel

---

## 8. Library Integration Tests + README `[Sonnet]`

**Output files:** `scripts/test-libs.sh`, `src/libs/README.md`

- [ ] **`scripts/test-libs.sh`** -- host-side test binary compiled with `gcc` (not clang-19):
  - [ ] Compile `src/libs/libc/string.c` + `src/libs/libc/math.c` with shim headers that map `kmalloc→malloc`, `kfree→free`
  - [ ] **String tests**: `memset/memcpy/memmove` correctness; `snprintf` format tests (int, hex, width, precision, overflow truncation); `strtol` edge cases (negative, hex prefix, overflow); `strstr` found/not-found
  - [ ] **Math tests**: `kmath_sin(0.0) == 0.0`, `kmath_cos(0.0) == 1.0`, `kmath_sin(KMATH_PI/2) ≈ 1.0` (within 1e-6); `kmath_exp(1.0) ≈ KMATH_E`; `kmath_log(KMATH_E) ≈ 1.0`; `kmath_atan2(1,1) ≈ pi/4`; `kmath_round(2.5) == 3.0`
  - [ ] **miniz tests**: compress 64 KB zeros → decompress → byte match; create ZIP with 2 entries → re-open → entry count == 2 → extract both; stream deflate then inflate identity
  - [ ] **monocypher tests**: Blake2b(`"test"`) == known vector; X25519 exchange: two keypairs → shared secrets match; `csprng_fill(buf, 32)` → non-zero (statistical)
  - [ ] **cJSON tests**: parse + extract + round-trip as described in §5; depth-limit test: 33-level nesting → returns NULL
  - [ ] **Mbed TLS tests**: AES-256-GCM encrypt + decrypt known plaintext/key/nonce → matches NIST vector; SHA-256 of `"abc"` == known hash
  - [ ] Exit 0 on all pass; exit 1 on first failure with descriptive message; integrate as `make check` target in `Makefile`
- [ ] **`src/libs/README.md`** -- table:

| Library         | Version | License       | Source URL                         | Purpose                                  |
| --------------- | ------- | ------------- | ---------------------------------- | ---------------------------------------- |
| monocypher      | 4.x     | BSD-2-Clause  | github.com/LoupVaillant/Monocypher | ChaCha20, Blake2b, X25519, Ed25519, Argon2i |
| miniz           | 2.x     | MIT           | github.com/richgel999/miniz        | deflate/inflate, ZIP read/write          |
| cJSON           | 1.7.x   | MIT           | github.com/DaveGamble/cJSON        | JSON parse/emit                          |
| Mbed TLS        | 3.x     | Apache-2.0    | github.com/Mbed-TLS/mbedtls        | TLS 1.2, AES, SHA, RSA, ECDH             |
| stb_truetype    | 1.26    | Public Domain | github.com/nothings/stb            | TrueType font rasterization              |
| stb_image       | latest  | Public Domain | github.com/nothings/stb            | JPEG/PNG/BMP/GIF decode                  |
| stb_image_write | latest  | Public Domain | github.com/nothings/stb            | PNG/BMP encode                           |

---

## OS Comparison


| ⭐   | Feature                                  | 🪟 Win11                         | 🐧 Linux                        | 🚀 Impossible OS                          |
| --- | ---------------------------------------- | ------------------------------- | ------------------------------ | ---------------------------------------- |
| 💎   | Freestanding string/printf libc          | ✅ ntdll CRT subset              | ✅ kernel/lib/string.c + printk | ⬜ §1 -- `include/libc/string.h` consolidation |
| 💎   | Full software math library               | ✅ MSVC CRT `libcmt`             | ✅ kernel/lib/math.c + libm     | ⬜ §2 -- extends existing `kmath.h`; all trig/exp/log |
| 💎   | deflate/inflate + ZIP read/write         | ✅ ntoskrnl LZNT1 + Cabinet.dll  | ✅ lib/zlib in-tree + zip via   | ⬜ §3 -- (extends §4); `mz_zip_writer` +  |
| ⭐   | RDRAND-seeded ChaCha20 CSPRNG as inbox API | ✅ `BCryptGenRandom` (CNG)       | ✅ `get_random_bytes` (kernel)  | ⬜ §4 -- monocypher + RDRAND entropy +    |
| 💎   | JSON parse/emit in-kernel                | ❌ Not in ntoskrnl               | ❌ Not in kernel                | ⬜ §5 -- cJSON with `kmalloc` hooks +     |
| 💎   | TLS 1.2 client                           | ✅ Schannel (kernel TLS offload) | ✅ Rustls / OpenSSL via socket  | ⬜ §6 -- Mbed TLS 3.x subset; ECDHE-RSA-AES128-GCM-SHA256 |
| 💎   | STB image decode/encode                  | ✅ WIC (COM, Ring 3)             | ✅ GdkPixbuf / libpng           | ✅ §7 -- Done -- stb_truetype + stb_image |
| ⭐   | Host-side integration test suite for embedded libs | ✅ Partial (vendor unit tests)   | ✅ Partial (lib/crypto/testmgr) | ⬜ §8 -- `scripts/test-libs.sh` with known test vectors |

Impossible OS carries the CSPRNG as an **inbox kernel service** backed by hardware RDRAND
entropy -- the same CSPRNG that seeds SSH key generation, WiFi WPA2, and the credential store
-- exposed to user-mode via a single `SYS_GETRANDOM` syscall. No COM stacks, no dynamic
linking, no trust boundary crossings: pure in-kernel primitives directly callable from any
driver or subsystem.

---

## Verification

Run `bash scripts/build.sh run` (full build) and `bash scripts/test-libs.sh` (host tests).

- [ ] **String**: `snprintf(buf, 8, "%d", 123456789)` → `buf == "1234567"` (truncated, null-terminated); `strstr("hello world", "world")` → non-null pointer; `strtol("-0x1F", NULL, 16)` → -31
- [ ] **Math**: `kmath_sin(KMATH_PI/6)` ≈ 0.5 ± 1e-6; `kmath_exp(0.0)` == 1.0; `kmath_log(1.0)` == 0.0; `kmath_atan2(0.0, -1.0)` ≈ KMATH_PI ± 1e-6
- [ ] **miniz**: `bash scripts/test-libs.sh` → compress/decompress identity; ZIP list/extract pass
- [ ] **monocypher**: Blake2b test vector matches; X25519 exchange symmetric; `csprng_fill` produces non-trivial bytes; `SYS_GETRANDOM` syscall from user-mode returns requested bytes
- [ ] **cJSON**: round-trip parse test passes; depth-limit returns NULL for 33-level input
- [ ] **Mbed TLS**: AES-256-GCM NIST vector passes; SHA-256 `"abc"` → `ba7816bf…`; TLS handshake in QEMU network if available
- [ ] **STB**: `image_load` + `image_save_png` round-trip in QEMU; no memory corruption (PMM frame count stable after 10 load/free cycles)
- [ ] **test-libs.sh**: `bash scripts/test-libs.sh` exits 0; `make check` runs the script and exits 0
- [ ] Commit: `"libs: string/math libc, miniz writer, monocypher CSPRNG, cJSON, Mbed TLS subset, STB consolidation, test suite"`
