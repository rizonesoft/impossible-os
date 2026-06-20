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
> **Current state (code-truth 2026-06-12):** `src/libc/string.c` + `include/libc/string.h` ship `snprintf`/`vsnprintf` and string/memory ops (§1 core done; `panic.c` still hand-rolls crash text). `include/kernel/kmath.h` already inlines `kmath_cos`, `kmath_acos`, `kmath_pow`, `kmath_sqrt` for stb; §2 `src/libc/math.c` trig/log suite not started. `src/libs/cjson/` + `src/kernel/json.c` implement §6 parser + thin wrappers; `cJSON_SetMaxDepth` / production call sites still open. `src/libs/monocypher/` + `src/kernel/csprng.c` ship §5 (crypto + CSPRNG + `NtGetRandom`); `test_register_klibs` is wired in `test_runner.c` with 15 crypto/CSPRNG suites. No `src/libs/lz4`, `miniz`, or `mbedtls` trees yet.
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
- -> XREF: `TODO-10-kernel-security-hardening.md §12` -- `__stack_chk_guard` canary seeded with RDRAND; the full CSPRNG (§5) is a superset of that; share the seed path
- -> XREF: `TODO-15-security-reference-monitor.md §14` -- AppContainer / package SID hashing uses Monocypher (T03 §5; T15 §14)
- -> XREF: `TODO-14-registry-completion.md §5` -- registry hive WAJ uses CRC32C; provided by the §8 checksum/codec dispatch (not Monocypher)
- -> XREF: `TODO-13-atom-nls-locale-subsystem.md` -- CANONICAL owner of Unicode case-folding + UTF-16<->UTF-8/code-page conversion (the `RtlUnicodeToUTF8N` equivalent); this TODO does NOT add a UTF section, consumers (env vars D02 T22, clipboard D09 T01, exFAT/NTFS) use T13's converter
- -> XREF: `TODO-04-system-logging.md §10` -- HMAC-Blake2b chain for tamper-evident `events.jsonl`; requires Monocypher Blake2b + kernel CSPRNG (§5)
- -> XREF: `07-networking/TODO-03-http-tls.md` §3 -- Mbed TLS Kernel Port in networking depends on this §7 freestanding port being complete first
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
- A kernel checksum dispatch (`kcrc32` IEEE + `kcrc32c` Castagnoli with an SSE4.2 fast path) and base64/hex codecs give every subsystem one bounds-checked implementation instead of scattered local CRC/codec copies.

---

## Implementation Order

| ⭐  | Order | Deliverable                                          | Depends On          | Status |
| --- | :---: | ---------------------------------------------------- | ------------------- | :----: |
| 💎  |   1   | Freestanding string library (`snprintf`/`vsnprintf`) | --                   |  [x]   |
| 💎  |   2   | Complete floating-point math library                 | --                   |  [x]   |
| 💎  |   3   | LZ4 block compressor                                 | --                   |  [/]   |
| 💎  |   4   | miniz deflate/inflate + ZIP                          | §1                   |  [/]   |
| 💎  |   5   | Monocypher crypto primitives + kernel CSPRNG         | §1                   |  [x]   |
| 💎  |   6   | cJSON DOM parser                                     | §1                   |  [/]   |
| 💎  |   7   | Mbed TLS freestanding port (record layer)            | §1, §2, §5            |  [ ]   |
| 💎  |   8   | Checksum + base64/hex codec dispatch (CRC32/CRC32C)  | §1                   |  [ ]   |

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

> **Notes:**
> - Freestanding string lib shipped: `mem*`/`str*`/`strl*` + `snprintf`/`vsnprintf` (full flag/width/precision/length-modifier support); weak-symbol dupes removed from `image.c` + `stb_truetype_impl.c`.
> - Fixed a serious `vsnprintf` infinite loop: `PUTC` skipped evaluating side-effecting args (`tmp[--n]`, `*fmt++`) once the buffer filled, spinning forever on size=0 or mid-number truncation. Now binds the arg to a local first.
> - Hardened formatter/conversions: integer precision, `%p` width/flags, `FMT_FIELD_MAX` cap, `%s`/`strlcat` overread bounds, `INT64_MIN`-safe, `strtoul`/`strtol` sign + guarded `0x` + overflow-before-negate (shared sign-less core).
> - `memcpy`/`memmove`/`memset` word-at-a-time (`may_alias`) scalar fast path; `strstr` first-char skip; `config_dump` tunable pagination uses a stable raw-slot cursor; `boot_timing` clamps `snprintf` return before `vfs_write`.
> - 2 items deferred: `panic.c` + `klog`/`printk` migration onto `snprintf` (boot-path regression risk). Reclaimed 36 KB test BSS (shared bls fixture) to fit the test build under the 0x800000 ceiling.
> **Verified:** 2026-06-20 | commit `02087f12` | 10/12 items | build OK | tests 6458 kernel + 16 user PASS (TCG)
> **Deferred:** [L] panic.c + klog/printk still use hand-rolled formatters, not snprintf/vsnprintf (reason: cosmetic dedup, no functional gap; migration during active dev risks boot regression) -> XREF: TODO-03 §1 (items "Update panic.c to use snprintf" + "Update klog / printk to use vsnprintf internally" at lines 110-111)
> **Quality reviewed:** 2026-06-20 | Codex 11x (adversarial, consistency, perf, re-adversarial, design, test-coverage) | 4H+11M fixed, 0 open, 1 perf rejected (SIMD memcpy: -mno-sse2) | scope: kernel-code-quality

---

## 2. Complete Floating-Point Math Library

- [x] Reconciled with `include/kernel/kmath.h`: new lib declares ONLY non-overlapping names (no ODR clash); the 8 inlines `kmath_fabs/floor/ceil/fmod/sqrt/pow/cos/acos` (used by stb_truetype + cJSON) stay put -- TrueType raster precision unchanged.
- [x] Header-only, all `KM_AI` (`static inline __attribute__((always_inline))`): `-mno-sse2` gives no legal `xmm` return for an extern `double`, so no `src/libc/math.c` -- everything inlines into callers.
- [x] Documented `kmath.h`-provided set + callers (stb STBTT_* macros; cJSON floor/fabs/pow) in the header banner.

- [x] `include/libc/math.h` -- few-ULP (not 1-ULP) software math; Cody-Waite range reduction with split pi/2 + fdlibm minimax kernels; IEEE special values + signed zero at every entry point:
  - `kmath_sin`/`kmath_tan` -- octant reduction + fdlibm kernels; NaN for NaN/+-inf and (narrowed domain) finite `|x| >= KMATH_TRIG_REDUCE_MAX`; signed zero preserved
  - `kmath_asin` -- `atan2(x, km_sqrt_(1-x^2))`; NaN for `|x|>1`/inf, `+/-pi/2` only at exactly +/-1
  - `kmath_atan` -- 1/x + pi/6 breakpoint (minimax arg `<= 2-sqrt(3)`); `atan(+/-0)=+/-0`
  - `kmath_atan2` -- four-quadrant; full C99 infinity + signed-zero handling
  - (`cos`/`acos` double remain the `kmath.h` inlines)
- [x] `float` variants `kmath_sinf`..`kmath_truncf` (double then narrow): `cosf` uses the guarded reduction (`kmath_cos` hangs on inf), `acosf` = `pi/2-asin` + domain check, `sqrtf` uses seeded `km_sqrt_`, `powf` uses hardened `km_pow_`.

- [x] `kmath_exp` -- split-ln2 reduction + 14-term Taylor on `|r|<=ln2/2`, reconstruct `2^k` via the IEEE exponent field (musl `scalbn`); overflow -> +inf, underflow -> 0, NaN-safe
- [x] `kmath_log` -- exponent/mantissa via bit-cast, centered around 1, `2*atanh((m-1)/(m+1))` series; `log(0)=-inf`, `log(<0)=NaN`, `log(+inf)=+inf`, subnormal scale-up
- [x] `kmath_log2` -- `log(x)*log2(e)`
- [x] `kmath_log10` -- `log(x)*log10(e)`
- [x] `pow` -- double stays the `kmath.h` inline (ODR); hardened `km_pow_` (full C99 special values, parity via bounded low-bit test, no `fmod`/`floor` UB) backs `kmath_powf`.

- [x] `kmath_round` -- round half away from zero; `>= 2^52` and `+/-0` pass through
- [x] `kmath_trunc` -- toward zero; `>= 2^52` and `+/-0` pass through
- [x] `kmath_cbrt` -- `exp(log/3)` seed + 2 Newton steps; preserves sign, zero, +/-inf

- [ ] Unify float/double special values: hardened float cosf/acosf/sqrtf/powf diverge from font-grade kmath.h double cos/acos/pow/sqrt; promote the doubles (or add hardened non-overlapping names), gated on a stb golden-raster baseline.

- [x] Commit: `"libc: floating-point math -- sin/tan/asin/atan/atan2/exp/log/cbrt + float variants"`

**Test checkpoint:** `kmath_sin(0)==0`, `kmath_cos(0)==1`, `kmath_atan2(1,1)` within 1e-9 of pi/4; `kmath_exp(1)` within 1e-9 of e; IEEE edges hold (sin/exp/log of NaN -> NaN, exp overflow -> +inf, log(0) -> -inf, asin domain -> NaN, signed zero preserved, `cosf(inf)` does not hang); spot-check stb_truetype glyph raster unchanged vs baseline. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | 1 suite (klibs: math lib), 0 failures
> **Notes:**
> - Shipped `include/libc/math.h` (header-only, all `KM_AI` always-inline): trig + `exp/log/log2/log10` + `round/trunc/cbrt` + 16 float variants; Cody-Waite (33-bit pio2_1) + fdlibm kernels.
> - All static-inline by ABI necessity (`-mno-sse2`: no `xmm` for extern `double`); declares only names absent from `kmath.h` so a TU may include both; sin/cosf use single-kernel reducers, tan uses the shared `km_sincos_`.
> - IEEE-complete: NaN/inf/over-underflow/domain, signed zero at every entry point, atan2 C99 quadrants, `cosf` no-hang; few-ULP within `|x| < 2^20*pi/2` (NaN above); per-finding hardening in the ship + review commits.
> - Canonical doc: `include/libc/math.h` banner.
> - Scope boundary: double `cos/acos/pow/sqrt` stay font-grade in `kmath.h` (unification deferred, this section); LZ4/miniz/crypto/checksum in §3-§8.
> **Verified:** 2026-06-20 | commit `77eec81d` | 13/14 items | build OK | tests 377 kernel + 16 user PASS (TCG)
> **Deferred:** [M] hardened float cosf/acosf/sqrtf/powf diverge from font-grade kmath.h double cos/acos/pow/sqrt on special values (reason: kmath.h ODR ownership; promoting the doubles needs a stb golden-raster baseline) -> XREF: 02-kernel-core/TODO-03-kernel-libraries.md §2 (item: "Unify float/double special values" at line 153)
> **Quality reviewed:** 2026-06-20 | Codex 20x (design, adversarial-impl, adversarial, consistency, perf, re-adversarial, test-coverage) | 9H+11M fixed, 1M deferred | scope: kernel-code-quality

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

> **Deferred:** [H] LZ4 needs official `lz4.c`/`lz4.h`/`lz4frame.c` (BSD-2) operator-vendored into `src/libs/lz4/` like monocypher/cJSON -- the agent environment cannot fetch verbatim upstream, and reference-format interop for OFF-BOX crash-dump decode (WinDbg/host) cannot be proven without the reference codec, so a hand-rolled substitute is unsafe for that consumer. Block API + frame layer + TODO-26/27/20 wiring follow once vendored. -> XREF: 02-kernel-core/TODO-03-kernel-libraries.md §3 (item: "Vendor `lz4.c` + `lz4.h` from the official LZ4 repository" at line 174)

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

> **Deferred:** [H] miniz needs official `miniz.h`/`miniz.c` (MIT) operator-vendored into `src/libs/miniz/` like Monocypher (§5) and cJSON -- the agent environment cannot fetch verbatim upstream, and deflate/gzip/ZIP wire-format interop (HTTP gzip responses, off-box ZIP archives) cannot be proven without the reference codec, so a hand-rolled DEFLATE substitute is unsafe. malloc->kmalloc shims, ZIP API, and HTTP gzip wiring follow once vendored. -> XREF: 02-kernel-core/TODO-03-kernel-libraries.md §4 (item: "Vendor `miniz.h` / `miniz.c` (MIT, ~6000 lines) into `src/libs/miniz/`" at line 202)

---

## 5. Monocypher Crypto Primitives + Kernel CSPRNG

- [x] Vendored Monocypher 4.0.2 into `src/libs/monocypher/` (core + optional standard-Ed25519/SHA-512 unit + LICENCE; sha256 in commit; only mod: `kernel/types.h` include shim for `-nostdinc`)
- [x] Compiles with kernel flags (`-ffreestanding -nostdlib -nostdinc -mno-sse`); integer-only C99, zero heap allocation
- [x] Primitives exposed, names unchanged: ChaCha20, Poly1305, AEAD, Blake2b, Argon2id (-> XREF `09-desktop-shell/TODO-06-security-accounts.md §2`), X25519, EdDSA-Blake2b + standard Ed25519 (EIF signing -> XREF `TODO-17-binary-system.md §17`)
- [x] `src/kernel/csprng.c` + `include/kernel/csprng.h`: `csprng_init/fill/u64/is_seeded` plus pure `csprng_core_*` helpers (explicit state, unit-testable without live init; bounded irqsave lock holds)
- [x] `csprng_init()` seeds per the `entropy.h` conditioner contract: RDRAND + TSC/link-address + ACPI PM timer + staged jitter/TPM transcript, Blake2b-256 conditioned; Phase 1 after `entropy_collect_tpm()`
- [x] Continuous operation: fast-key-erasure on EVERY fill (ratchet under lock, stream outside) -- stronger than the drafted 4 MiB re-key, adopted from Codex design review
- [/] Canary seed sharing: `csprng_u64()` ready; BLOCKED on `TODO-10-kernel-security-hardening.md §12` (`canary_init()` not implemented; reciprocal item filed there)
- [x] `task_exec()` AT_RANDOM now `csprng_fill(rand_buf, 16)`; RDRAND/TSC fallback block deleted (satisfies `TODO-11-peb-teb-user-abi.md §13` follow-up)
- [ ] Wire `csprng_u64()` into EIF `load_base=0` randomized base -- BLOCKED on `TODO-20-eif-full-implementation.md §8` (unimplemented; its spec already names the kernel CSPRNG)
- [x] `NtGetRandom(buf, len, flags)` at SSDT 0x03D8 (main count 470 -> 471): probe + 256 B chunked bounce copy, 1 MiB cap, `ntdll` export, master-table row (-> XREF `TODO-A-SSDT-Master-Table.md`)
- [/] `csprng_add_entropy()` reseed API shipped (HIGH-only seed credit); remaining: reseed thresholds + interrupt-timing accounting; hwrng hook via `04-drivers-hardware/TODO-09` §12; first seed via `01-boot-platform/TODO-12` §8
- [x] Single CSPRNG ownership: duplicate entropy plans routed to `csprng_fill()`/`csprng_add_entropy()` in `09-desktop-shell/TODO-06` §1, `07-networking/TODO-03` §5, `12-user-platform-sdk/TODO-01` §4

- [x] Commit: `"libs: Monocypher crypto, kernel CSPRNG (RDRAND + Blake2b), SYS_GETRANDOM"`

**Test checkpoint:** AEAD round-trip + tamper rejection; Blake2b RFC 7693 + X25519 RFC 7748 + independent Ed25519 vectors; CSPRNG core golden vectors (two independent implementations agreed); `csprng_fill` outputs differ; `NtGetRandom` via real `ssdt_dispatch` + chunk boundaries 1/255/256/257/513 with canaries. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | 14 klibs suites in `test_klibs.c`, 0 failures

> **Notes:**
> - Shipped: `src/libs/monocypher/` (Monocypher 4.0.2) + `src/kernel/csprng.c` (ChaCha20 fast-key-erasure CSPRNG, Blake2b conditioner) + `NtGetRandom` SSDT 0x03D8 + `acpi_pmtimer_read_value()`.
> - Runs: `csprng_init()` in Phase 1 after the TPM collector (drains staged jitter/TPM transcript); `csprng_register_ssdt()` in the Phase 3 SSDT block; `task_exec()` AT_RANDOM consumes `csprng_fill`.
> - Downstream: unblocks `01-boot-platform/TODO-12` §6/§8, `TODO-04` §10 klog HMAC, EIF signing (T17 §17); Codex adoptions + two latent-bug fixes (heap truncation, user PT window) detailed in the ship commit.
> - Canonical doc: `include/kernel/csprng.h` + the `include/kernel/entropy.h` conditioner contract.
> - Scope boundary: boot seed payload mixing/ordering/policy stay with `01-boot-platform/TODO-12` §7-§9; virtio-rng hook with `04-drivers-hardware/TODO-09` §12; `canary_init()` with `TODO-10` §11.
> **Verified:** 2026-06-12 | commit `99910014` | 9/12 items | build OK | tests 4523/4523 PASS, smoke PASS
> **Accepted:** [H] `NtGetRandom` (and every UserMode NtXxx handler) trusts `ProbeFor*IfUser` range-only check; a ring-3 low kernel VA below `MM_USER_PROBE_ADDRESS` passes (systemic, per-process frames shared until PE loader) -> XREF: 02-kernel-core/TODO-10-kernel-security-hardening.md §2 (item: "Harden the user-copy path to reject supervisor destinations" at line 117)
> **Accepted:** [H] global `s_previous_mode` can race on SMP, skipping a handler's probe (reason: gated on SMP user scheduling) -> XREF: 02-kernel-core/TODO-12-native-api-ssdt.md §12 (item: "Make `s_previous_mode` ... per-CPU" at line 431)
> **Quality reviewed:** 2026-06-12 | Codex 11x (design + test-coverage + adversarial + consistency + perf + re-adversarial) | 3H+5M+1L fixed, 2H accepted-XREF | scope: kernel-code-quality

---

## 6. cJSON DOM Parser

- [x] Vendor `cJSON.c` + `cJSON.h` (MIT v1.7.18, 3443 lines) into `src/libs/cjson/`
- [x] Compile with `-ffreestanding -nostdlib` + SSE2 for float; freestanding shim replaces all stdlib headers
- [x] Allocator redirected via compile-time macros (`malloc` -> `kmalloc`, `free` -> `kfree`, `realloc` -> `krealloc`):
  ```c
  cJSON_Hooks hooks = { .malloc_fn = kmalloc, .free_fn = kfree };
  cJSON_InitHooks(&hooks);
  ```
- [ ] Max-depth guard: cJSON has NO runtime SetMaxDepth -- lower compile-time `CJSON_NESTING_LIMIT` (default 1000) to 32 via -D/header patch + a wrapper node-count budget before parse to avoid heap/stack exhaustion on malformed input.

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

**Test checkpoint:** Sample JSON parses; `json_int`/`json_str` match literals; a document nested deeper than `CJSON_NESTING_LIMIT` is rejected without heap/stack exhaustion; `json_print` round-trip stable. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 7. Mbed TLS Freestanding Port

> [!IMPORTANT]
> **Scope overlap -- `07-networking/TODO-03-http-tls.md` §3** also claims "Mbed TLS Kernel Port". This §7 is the canonical freestanding port; `07-networking/TODO-03 §3` must depend on this section being complete and should not re-implement the library itself. When `07-networking/TODO-03` is next validated, update its §3 to reference this TODO-03 §7 as its prerequisite.

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

## 8. Checksum and Codec Dispatch (CRC32 / CRC32C / base64 / hex)

One kernel-owned checksum + codec layer so subsystems stop hand-rolling incompatible variants. Win11 exposes `RtlCrc32`/`RtlCrc64`; Linux centralizes `lib/crc32` + `lib/base64`. Today GPT, IXFS, entropy, and the bootloader each carry a private CRC32/CRC32C, and TLS/SSH/SMTP/registry would each invent base64/hex.

- [ ] `uint32_t kcrc32(const void *data, size_t len)` IEEE 802.3 (reflected poly `0xEDB88320`) + continuable `kcrc32_cont(crc, data, len)`; reuse the existing `gpt_crc32` table as the shared implementation.
- [ ] `uint32_t kcrc32c(const void *data, size_t len)` Castagnoli (poly `0x1EDC6F41`); SSE4.2 `crc32` fast path gated on `cpu_has(CPU_FEATURE_SSE42)`, table fallback otherwise (TCG/pre-SSE4.2 must match byte-for-byte).
- [ ] `include/kernel/kchecksum.h` dispatch header; migrate IXFS WAJ, entropy, and registry-hive CRC32C call sites to it (GPT + bootloader pre-EBS mirrors may keep their local copies, noted in the header).
- [ ] `base64_encode`/`base64_decode` (strict + MIME-whitespace-tolerant modes, padding-validated, bounds-checked, `-1` on overflow/invalid) in `include/kernel/kcodec.h`.
- [ ] `hex_encode`/`hex_decode` (decode accepts lower+upper, rejects odd-length/invalid) for registry `.reg` binary values, PEM/TLS, PDF ASCIIHex.
- [ ] Expose as the canonical codec for TLS PEM (§7) + SSH/SMTP/MIME/BSOD-QR consumers; replace the planned net-local `base64_decode` with this.
- [ ] Commit: `"libs: kernel checksum (CRC32/CRC32C) + base64/hex codec dispatch"`

**Test checkpoint:** `kcrc32("123456789",9) == 0xCBF43926` (IEEE vector); `kcrc32c("123456789",9) == 0xE3069283` (Castagnoli vector) on BOTH the SSE4.2 path and the table fallback; base64 round-trips RFC 4648 vectors and rejects bad padding; hex round-trips and rejects odd-length/invalid input. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11      | 🐧 Linux      | 🚀 Impossible OS |
| --- | -------------------------- | ------------- | ------------- | ---------------- |
| 💎 | snprintf safe              | ✅ Rtl safe   | ✅ vsprintf   | ✅ §1 libc       |
| 💎 | Core string ops            | ✅ Rtl mem    | ✅ string.c   | ✅ §1 libc       |
| 💎 | Float math lib             | ✅ CRT        | ✅ libm       | ✅ §2 libc (trig/exp/log/cbrt, few-ULP, IEEE) |
| 💎 | LZ4 in kernel              | ⚠️ Xpress     | ✅ lib/lz4    | ⬜ §3            |
| 💎 | Deflate zlib               | ✅ Rtl LZNT1 | ✅ zlib       | ⬜ §4 miniz      |
| 💎 | ZIP read                   | ✅ Shell      | ✅ unzip      | ⬜ §4            |
| 💎 | ChaCha Poly AEAD           | ✅ BCrypt     | ✅ chacha     | ✅ §5 Monocypher |
| 💎 | Blake2b                    | ✅ no native  | ✅ blake2b    | ✅ §5 Monocypher |
| 💎 | Argon2id                   | ✅ KDF API    | ✅ argon2     | ✅ §5 Monocypher |
| 💎 | X25519 ECDH                | ✅ BCrypt     | ✅ ecdh       | ✅ §5 Monocypher |
| 💎 | Ed25519                    | ✅ BCrypt     | ✅ eddsa      | ✅ §5 std + Blake2b EdDSA |
| 💎 | Kernel CSPRNG              | ✅ BCrypt     | ✅ random.c   | ✅ §5 fast-key-erasure ChaCha20 |
| 💎 | getrandom syscall          | ✅ BCryptGenRandom | ✅ getrandom(2) | ✅ §5 NtGetRandom 0x03D8 |
| 💎 | JSON in kernel             | ❌ user COM   | ❌ none std   | ✅ §6 cJSON      |
| 💎 | TLS record crypto          | ✅ SChannel   | ✅ ktls       | ⬜ §7 mbed       |
| 💎 | CRC32 / CRC32C dispatch    | ✅ RtlCrc32   | ✅ lib/crc32  | ⬜ §8 kcrc32/kcrc32c |
| 💎 | base64 / hex codec         | ✅ Crypt32    | ✅ lib/base64 | ⬜ §8 kcodec     |
| 💎 | UTF-16<->UTF-8 conversion  | ✅ RtlUnicode | ✅ nls/utf8   | ⬜ owned by T13 NLS |
| ⭐ | One ring-0 CSPRNG façade (TLS seed + hiber + dumps + klog HMAC) | ⚠️ many BCrypt entry points | ⚠️ `get_random_bytes` + drivers | ✅ §5 `csprng_fill`/`csprng_add_entropy` |

After §1 through §8, freestanding libc and these libs unblock LZ4, miniz, crypto, JSON, the TLS record layer, and a shared checksum/codec dispatch for hibernation, dumps, IXFS, registry, and `07-networking` HTTPS.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_klibs()` (see `src/kernel/test/test_runner.c` and `include/kernel/test/test.h`). Use `test_suite_register_cat(..., TEST_CAT_EXEC)` for each case.
> Boot tests run with `debug=1` or `test=1` in `boot.conf`.

- [/] Create `src/kernel/test/test_klibs.c` -- EXISTS with 15 crypto/CSPRNG suites (§5 coverage: Monocypher vectors, CSPRNG core/golden/absorb, NtGetRandom dispatch + chunks); the §1-§4 cases below still to add:
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
  - `kcrc32("123456789", 9)` -> `0xCBF43926` (IEEE vector); `kcrc32c("123456789", 9)` -> `0xE3069283` (Castagnoli) on BOTH the SSE4.2 path and the table fallback
  - base64 encode/decode round-trips RFC 4648 vectors; `base64_decode` rejects bad padding; `hex_decode` rejects odd-length/invalid input
- [x] Add `extern void test_register_klibs(void);` in `test_runner.c`, call `test_register_klibs()` from `test_runner_init()`
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
- [ ] **Checksum/codec**: `kcrc32`/`kcrc32c` match the IEEE/Castagnoli vectors on both SSE4.2 and table paths; base64 + hex round-trip RFC 4648 vectors and reject malformed input.
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
