---
schema_version: 1
id: kernel-libraries
domain: 02-kernel-core
status: active
title: "TODO-03 -- Kernel Embedded Libraries"
---

# TODO-03 -- Kernel Embedded Libraries

> **Validated:** 2026-06-21 | validate-todo-file clean (structure / IO table / XREF / test wiring)
> **Gap-audited:** 2026-06-21 | confirmatory parity pass (mature TODO) + codex-gap-audit; 2 new baselines filed -- §9 Zstandard (Win11 24H2 ReFS + Linux lib/zstd; closes IXFS §3 dangling dep), §10 SHA-3/SHAKE (Win11 24H2 CNG + Linux sha3_generic; Codex-caught)

> **Goal:** Build the complete freestanding library layer the rest of the kernel depends on: `snprintf`/`vsnprintf` in `libc` (§1; `panic.c` / klog migration still deferred), full floating-point math beyond `kmath.h` (§2), LZ4 (§3; assumed by TODO-26/TODO-27), miniz deflate/ZIP (§4), Monocypher + kernel CSPRNG (§5), cJSON (§6), and Mbed TLS record layer (§7) for HTTPS/FTPS consumers. All ports compile with `-ffreestanding -nostdlib` and route heap through `kmalloc`/`kfree` with `pmm_alloc_contiguous` for buffers > 4 KiB.

> [!IMPORTANT]
> **Current state (code-truth 2026-06-12):** `src/libc/string.c` + `include/libc/string.h` ship `snprintf`/`vsnprintf` and string/memory ops (§1 core done; `panic.c` still hand-rolls crash text). `include/kernel/kmath.h` already inlines `kmath_cos`, `kmath_acos`, `kmath_pow`, `kmath_sqrt` for stb; §2 `src/libc/math.c` trig/log suite not started. `src/libs/cjson/` + `src/kernel/json.c` implement §6 parser + thin wrappers; `cJSON_SetMaxDepth` / production call sites still open. `src/libs/monocypher/` + `src/kernel/csprng.c` ship §5 (crypto + CSPRNG + `NtGetRandom`); `test_register_klibs` is wired in `test_runner.c` with 15 crypto/CSPRNG suites. `src/libs/lz4/` (LZ4 v1.10.0) block codec is ported + wired (§3: `src/kernel/lz4.c` + `include/libs/lz4.h`, on-stack state via `LZ4_MEMORY_USAGE=11`, scoped dead-strip); `src/libs/miniz/` (3.0.2) and `src/libs/mbedtls/` (3.6.2 LTS) source vendored 2026-06-20 but NOT yet wired (excluded from the C-source auto-glob); §4 (miniz) freestanding port is ready to implement, §7 (Mbed TLS) deferred on port effort.
> **Already done -- do NOT re-implement:**
> - `stb_truetype` -> `src/kernel/gfx/stb_truetype_impl.c` [done]
> - `stb_image` -> `include/stb_image.h` + `src/kernel/image.c` [done]
> - `kmath.h` -> exists with `fabs`, `floor`, `ceil`, `fmod`, `sqrt` [done]
>   (incomplete; §2 extends it)
> **Not in scope for this TODO:**
> - `dr_wav`, `dr_mp3`, `stb_vorbis`, `pl_mpeg` audio/video codecs -> belong in the multimedia domain (`11-apps` or `09-desktop-shell`).
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
- -> XREF: `02-kernel-core/TODO-03-kernel-libraries.md` -- COMPLEMENT: ZIP writer + stream deflate after D02 T03 §6 reader/port; do not re-vendor miniz here
- -> XREF: `05-storage-filesystems/TODO-07-ixfs-advanced-enterprise.md §3` -- CONSUMER: IXFS transparent compression selects LZ4 (§3) or Zstd (§9) per file; this TODO owns both codecs (IXFS §3 was the dangling consumer that justified §9)
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
- Zstandard (zstd) provides a deflate-class ratio at LZ4-class decode speed for IXFS transparent compression (the ratio-favoring codec alongside LZ4) -- the kernel-libraries owner IXFS §3 was waiting on.
- SHA-3 / SHAKE (FIPS 202 Keccak) gives the kernel the both-platform hash baseline (Win11 24H2 CNG + Linux sha3_generic) that the SHA-2 family and Monocypher Blake2b did not cover, plus the SHAKE XOF future post-quantum signatures need.
- Monocypher provides production-grade ChaCha20, Poly1305, Blake2b, Argon2id, X25519, and Ed25519 with a RDRAND-seeded CSPRNG (`csprng_fill()`).
- cJSON parses and serialises JSON for theme files, settings, update manifests, and NTP server lists.
- Mbed TLS 3.x compiles freestanding in a minimal configuration (TLS 1.3 record layer + AES-GCM + RSA/ECDSA) ready for use by the networking layer.
- A kernel checksum dispatch (`kcrc32` IEEE + `kcrc32c` Castagnoli with an SSE4.2 fast path) and base64/hex codecs give every subsystem one bounds-checked implementation instead of scattered local CRC/codec copies.

---

## Implementation Order

| ⭐  | Order | Deliverable                                          | Depends On | Status |
| --- | :---: | ---------------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Freestanding string library (`snprintf`/`vsnprintf`) | --         |  [x]   |
| 💎  |   2   | Complete floating-point math library                 | --         |  [x]   |
| 💎  |   3   | LZ4 block compressor                                 | --         |  [/]   |
| 💎  |   4   | miniz deflate/inflate + ZIP                          | §1         |  [/]   |
| 💎  |   5   | Monocypher crypto primitives + kernel CSPRNG         | §1         |  [x]   |
| 💎  |   6   | cJSON DOM parser                                     | §1         |  [x]   |
| 💎  |   7   | Mbed TLS freestanding port (record layer)            | §1, §2, §5 |  [/]   |
| 💎  |   8   | Checksum + base64/hex codec dispatch (CRC32/CRC32C)  | §1         |  [x]   |
| 💎  |   9   | Zstandard (zstd) freestanding port                   | §1         |  [/]   |
| 💎  |  10   | SHA-3 / SHAKE (Keccak) hash family                   | §1         |  [x]   |

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
> **Deferred:** [M] hardened float cosf/acosf/sqrtf/powf diverge from font-grade kmath.h double cos/acos/pow/sqrt on special values (reason: kmath.h ODR ownership; promoting the doubles needs a stb golden-raster baseline) -> XREF: 02-kernel-core/TODO-03-kernel-libraries.md §2 (item: "Unify float/double special values" at line 161)
> **Quality reviewed:** 2026-06-20 | Codex 20x (design, adversarial-impl, adversarial, consistency, perf, re-adversarial, test-coverage) | 9H+11M fixed, 1M deferred | scope: kernel-code-quality

---

## 3. LZ4 Block Compressor

- [x] Vendor `lz4.c` + `lz4.h` from the official LZ4 repository (BSD-2 license) into `src/libs/lz4/` -- LZ4 v1.10.0 (lz4/lz4hc/lz4frame/xxhash + LICENSE) vendored 2026-06-20
- [x] Compile freestanding via `LZ4_FREESTANDING=1` (block-only; `LZ4_mem*` as clang builtins, `-isystem include/freestanding`); the block API is zero-allocation on caller buffers
- [x] Expose the size-safe block API in `include/libs/lz4.h` + `src/kernel/lz4.c` (`size_t` lengths bounds-rejected before narrowing to the vendored int core):
  ```c
  size_t lz4_compress_bound(size_t input_size);                                         /* 0 if oversized */
  int lz4_compress(const void *src, size_t src_size, void *dst, size_t dst_capacity);   /* >0 size, or LZ4_ERR_* */
  int lz4_decompress(const void *src, size_t src_size, void *dst, size_t dst_capacity); /* >=0 size, or LZ4_ERR_* (safe decoder) */
  ```
- [x] Buffer allocation responsibility (callers own buffers; `pmm_alloc_contiguous()` for output > 4 KiB) documented in the `include/libs/lz4.h` banner
- [/] Wire into TODO-27 crash dump writer -- BLOCKED: crash-dump compression is itself deferred; codec ready. -> XREF `TODO-27-crash-dump-generation.md §6`
- [/] Wire into TODO-26 hibernation image writer -- BLOCKED: kernel hibernation writer unimplemented; codec ready. -> XREF `TODO-26-power-management.md §4`
- [/] Wire into TODO-20 EIF compressed segments -- BLOCKED: EIF compressed-segment support unimplemented; codec ready. -> XREF `TODO-20-eif-full-implementation.md` §6

- [x] For streaming use cases (large IXFS extents), vendor `lz4frame.c` from the same repository; provides the standard `.lz4` framing layer with content checksum (XXH32) and block independence flag -- vendored (lz4frame.c/.h + xxhash.c/.h)
- [/] `lz4f_compress_begin / update / end` / `lz4f_decompress` API -- DEFERRED: heap-backed frame layer (LZ4F context > 4 KiB kmalloc ceiling) with no in-tree consumer. -> XREF `05-storage-filesystems/TODO-07-ixfs-advanced-enterprise.md §3`

- [x] Commit: `"libs: LZ4 block compressor freestanding port -- size-safe wrapper, scoped dead-strip"`

**Test checkpoint:** compressible-buffer round-trip through `lz4_compress` / `lz4_decompress` matches byte-for-byte; `lz4_compress_bound` >= actual compressed size; malformed frame and oversized/undersized lengths rejected with `LZ4_ERR_*`, never overrun. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | 1 suite (klibs: lz4 block roundtrip), 0 failures
> **Notes:**
> - Shipped `include/libs/lz4.h` + `src/kernel/lz4.c`: size-safe block wrapper (`lz4_compress`/`lz4_decompress`/`lz4_compress_bound`) over vendored LZ4 v1.10.0 built `LZ4_FREESTANDING=1` (block-only, zero allocation).
> - Build: scoped dead-strip (`ld -r --gc-sections` + `lz4_merge.ld`) drops ~50 KiB of unused LZ4 variants so the kernel image stays under the 0x800000 user base without a global `--gc-sections`.
> - `size_t` API rejects NULL / zero / `> LZ4_BLOCK_INPUT_MAX` / `> INT_MAX` before narrowing to the int core; `lz4_decompress` uses the bounds-checked safe decoder for untrusted/off-box frames.
> - Canonical doc: `include/libs/lz4.h` banner.
> - Scope boundary: §3 owns the block codec; the `.lz4` frame/streaming layer and the TODO-26/27/20 + IXFS consumer wiring are deferred (no in-tree consumer yet), Zstd ratio codec is §9.
> **Verified:** 2026-06-21 | commit `de381ef1` | 5/9 items | build OK | tests 6623 kernel + 16 user PASS (TCG)
> **Deferred:** [M] `.lz4` frame/streaming layer (`lz4f_*`) -- heap-backed (LZ4F context > 4 KiB kmalloc ceiling), no in-tree consumer (reason: block API is the must-ship core; frame layer pulls forward when IXFS streaming needs it) -> XREF: 05-storage-filesystems/TODO-07-ixfs-advanced-enterprise.md §3 (item: "ixfs_compress_block(algo, in_buf, in_len, out_buf, out_size)")
> **Deferred:** [L] Consumer wiring (crash dump / hibernation / EIF) -- codec ready, consumers unimplemented (reason: TODO-27 §6, TODO-26 §4, TODO-20 §6 all unimplemented/deferred) -> XREF: 02-kernel-core/TODO-03-kernel-libraries.md §3 (items "Wire into TODO-27/26/20")
> **Quality reviewed:** 2026-06-21 | Codex 9x (design, adversarial, re-adversarial, consistency, perf, test-coverage) | 3H+1M+2L fixed, 0 open | scope: kernel-code-quality

---

## 4. miniz Deflate / Inflate + ZIP

- [x] Vendor `miniz.h` / `miniz.c` (MIT) into `src/libs/miniz/` -- miniz 3.0.2 amalgamation (miniz.c/.h + LICENSE) vendored 2026-06-20
- [/] Compile with `-ffreestanding -nostdlib -msse2` -- DEFERRED (workspace blocker, see Notes)
- [/] Allocator (CORRECTED): `tinfl` (~11 KiB + 32 KiB dict) and `tdefl` (~150 KiB) both EXCEED the 4 KiB `kmalloc` ceiling; per-call `pmm` is racy and the stack is too small, so the state must be a caller-provided workspace, not a hidden allocation
- [/] Decompression output > 4 KiB: consumer-owned via the workspace allocator hook (`mz_alloc_func`/`m_pAlloc`), not a hidden per-call allocation
- [/] Core API must be REWRITTEN around an explicit workspace object (caller owns allocation + serialization); the workspace-free `mz_compress`/`mz_uncompress`/`zip_*` draft is not SMP-safe here
- [/] Use gzip decompression for HTTP `Content-Encoding: gzip` -- the first real consumer; it owns the inflate workspace -> XREF `07-networking/TODO-03-http-tls.md`
- [/] Compress a known 8 KiB buffer; decompress; verify byte-for-byte integrity -- DEFERRED with the port
- [/] Open a ZIP archive from memory; list entries; extract one; verify against a known SHA-256 (Monocypher Blake2b from §5) -- DEFERRED with the port

- [ ] Commit: `"libs: miniz deflate/inflate, ZIP archive reading"`

**Test checkpoint:** 8 KiB gzip round-trip identity; ZIP in-memory extract matches golden hash; the consumer-owned workspace covers the >4 KiB state with no hidden per-call allocation. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Notes:**
> - Source vendored: miniz 3.0.2 (MIT) amalgamation in `src/libs/miniz/` (excluded from the C-source auto-glob; not yet ported).
> - DEFERRED whole-section (Codex design): miniz states (~11 KiB / ~150 KiB) exceed the 4 KiB `kmalloc` ceiling, per-call `pmm` is racy, and are too large for the stack -- no SMP-safe hidden-allocation path, no must-ship core today.
> - Reframed: the port must take a caller-provided workspace owned by the first real consumer (07-networking HTTP gzip), which allocates the pmm-backed state at init and owns serialization; the workspace-free draft API is not safely representable.
> - Scope boundary: §3 owns LZ4 (block compression), §9 owns Zstd (IXFS ratio codec); miniz/deflate has no in-tree consumer until networking gzip lands.
> **Deferred:** [H] miniz freestanding port (deflate/inflate + ZIP read) -- no SMP-safe internal allocation path (states exceed kmalloc ceiling; pmm unsynchronized; too large for stack), no in-tree consumer (reason: infra -- needs consumer-owned workspace lifecycle) -> XREF: 07-networking/TODO-03-http-tls.md §3 (Mbed TLS Kernel Port / HTTP transport -- the gzip-decode consumer that owns the inflate workspace)

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
- [/] Wire `csprng_u64()` into EIF `load_base=0` randomized base -- BLOCKED on `TODO-20-eif-full-implementation.md §8` (unimplemented; its spec already names the kernel CSPRNG)
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
> **Accepted:** [H] `NtGetRandom` (and every UserMode NtXxx handler) trusts `ProbeFor*IfUser` range-only check; a ring-3 low kernel VA below `MM_USER_PROBE_ADDRESS` passes (systemic, per-process frames shared until PE loader) -> XREF: 02-kernel-core/TODO-10-kernel-security-hardening.md §2 (item: "Harden the user-copy path to reject supervisor destinations" at line 142)
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
- [x] Malformed-input hardening: `-DCJSON_NESTING_LIMIT=32` (was 1000, no vendored edit); `json_parse_len` bounds untrusted reads (`cJSON_ParseWithLengthOpts`) + rejects trailing garbage; `JSON_MAX_INPUT` 1 MiB input cap (= node bound).
- [x] Kernel wrapper `src/kernel/json.c` + `include/kernel/json.h`: `json_parse`/`json_parse_len`/`get`/`str`/`int`/`u32`/`u64`/`double`/`bool`/`is_array`/`array_size`/`array_get`/`array_first`/`array_next`/`print`/`free`.
- [x] In use by `src/kernel/firmware_advisor.c` + `src/kernel/main/boot_trend.c`; theme.json/settings/manifests/NTP are future consumers owned by their domains.
- [x] Test: `test_json_lib` parses the sample doc + extracts fields, and checks the hardening (depth>32 rejected, length-bounded parse, trailing-garbage rejected, print round-trip).
- [ ] Make kernel cJSON parse reentrant: cJSON's `global_error` is written per parse (SMP race); add per-CPU error state or strip global-error publication. Benign today (no wrapper consumer reads `cJSON_GetErrorPtr`). Consumer: `json_parse_len`.
- [ ] Bound parse heap for untrusted JSON: a near-cap flat doc of many small nodes can transiently pressure the 2 MiB heap; add a per-parse PMM allocation budget. Today bounded by kmalloc's 4 KiB per-alloc ceiling + graceful failure.

- [x] Commit: `"libs: cJSON DOM parser, json_parse/get/print wrapper"`

**Test checkpoint:** Sample JSON parses; `json_int`/`json_str` match literals; a document nested deeper than `CJSON_NESTING_LIMIT` is rejected without heap/stack exhaustion; `json_print` round-trip stable. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | 1 suite (klibs: cjson wrapper), 0 failures
> **Notes:**
> - cJSON v1.7.18 was already vendored (`src/libs/cjson/`) with kmalloc allocator macros + SSE2 rule; this section adds the malformed-input hardening + a unit test.
> - Hardening: `-DCJSON_NESTING_LIMIT=32` on the `cJSON.o` rule (no vendored edit); `json_parse_len` for untrusted/non-NUL buffers (`cJSON_ParseWithLengthOpts` + trailing-garbage reject); `JSON_MAX_INPUT` 1 MiB cap = node bound.
> - In use by `firmware_advisor.c` + `boot_trend.c`; theme.json/settings/manifests/NTP are future consumers owned by their domains.
> - Canonical doc: `include/kernel/json.h`.
> - Scope boundary: cJSON internals stay vendored (`src/libs/cjson/`); crypto/TLS in §5/§7, compression in §3/§4.
> **Verified:** 2026-06-20 | commit `bae54b79` | 7/9 items | build OK | tests 394 kernel + 16 user PASS (TCG)
> **Deferred:** [M] cJSON writes shared `global_error` per parse (SMP race, not reentrant) (reason: benign today -- no wrapper consumer reads the cJSON error pointer; locking across a kmalloc-heavy parse is worse) -> XREF: 02-kernel-core/TODO-03-kernel-libraries.md §6 (item: "Make kernel cJSON parse reentrant" at line 287)
> **Deferred:** [M] near-cap JSON of many small nodes can transiently pressure the 2 MiB heap (reason: per-alloc bounded by kmalloc 4 KiB ceiling + graceful failure; only file consumer is self-written boot-time cache) -> XREF: 02-kernel-core/TODO-03-kernel-libraries.md §6 (item: "Bound parse heap for untrusted JSON" at line 288)
> **Quality reviewed:** 2026-06-20 | Codex 8x (design, adversarial-impl, adversarial, consistency, perf, re-adversarial) | 2H+3M fixed, 2M deferred | scope: kernel-code-quality

---

## 7. Mbed TLS Freestanding Port

> [!IMPORTANT]
> **Scope overlap -- `07-networking/TODO-03-http-tls.md` §3** also claims "Mbed TLS Kernel Port". This §7 is the canonical freestanding port; `07-networking/TODO-03 §3` must depend on this section being complete and should not re-implement the library itself. When `07-networking/TODO-03` is next validated, update its §3 to reference this TODO-03 §7 as its prerequisite.

- [/] Clone Mbed TLS source into `src/libs/mbedtls/` (Apache-2.0) -- Mbed TLS 3.6.2 LTS vendored 2026-06-20 (`library/` + `include/` + LICENSE); freestanding `mbedtls_config.h` enabling only the required subset still remains:
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

> **Deferred:** [H] Source now vendored 2026-06-20 (Mbed TLS 3.6.2 LTS in `src/libs/mbedtls/`), so the prior fetch-blocker is cleared -- but the freestanding TLS port is large (config trim to the required subset, entropy hook to `csprng_fill`, CSPRNG/AES/SHA wiring, record-layer interop tests) and has no in-tree consumer until networking. Deferred on scope/effort, not on a blocker; pull forward when `07-networking/TODO-03 §3` (HTTPS) needs it. The tree is excluded from the C-source auto-glob until ported. -> XREF: 02-kernel-core/TODO-03-kernel-libraries.md §7 (item: "Clone Mbed TLS source into `src/libs/mbedtls/`" at line 313)

---

## 8. Checksum and Codec Dispatch (CRC32 / CRC32C / base64 / hex)

One kernel-owned checksum + codec layer so subsystems stop hand-rolling incompatible variants. Win11 exposes `RtlCrc32`/`RtlCrc64`; Linux centralizes `lib/crc32` + `lib/base64`. Today GPT, IXFS, entropy, and the bootloader each carry a private CRC32/CRC32C, and TLS/SSH/SMTP/registry would each invent base64/hex.

- [x] `kcrc32`/`kcrc32_cont` IEEE 802.3 (reflected `0xEDB88320`, finalized-in/out) via `const k_crc32_ieee[256]` rodata table (no lazy init, SMP-safe) in `src/kernel/kchecksum.c`; GPT keeps its own copy.
- [x] `kcrc32c`/`kcrc32c_cont` Castagnoli (reflected `0x82F63B78`); SSE4.2 `crc32q/b` path (`target("sse4.2")`) gated on `cpu_has(CPU_FEATURE_SSE4_2)` (now in `CPU_FEATURES_AP_PROBE_MASK`), `k_crc32c_tab[256]` fallback; hw==sw unit-tested.
- [x] `include/kernel/kchecksum.h` dispatch header; migrated `ixfs_crc32c`, `entropy_crc32c`, registry `hive_crc32` to byte-identical wrappers (on-disk checksums unchanged); GPT + bootloader mirrors keep local copies.
- [x] `base64_encode`/`base64_decode` (strict + MIME modes, padding-validated, bounds-checked, `-1` on overflow/invalid, `KCODEC_INT_MAX` guards) in `include/kernel/kcodec.h` + `src/kernel/kcodec.c`.
- [x] `hex_encode`/`hex_decode` (decode accepts lower+upper, rejects odd-length/invalid) for registry `.reg`, PEM/TLS, PDF ASCIIHex.
- [/] Canonical codec API shipped; consumer migration deferred until consumers exist (PEM gated on §7; SSH/SMTP/MIME owned by 07-networking). -> XREF: 02-kernel-core/TODO-03 §7 (item: "Clone Mbed TLS 3.x source into `src/libs/mbedtls/`" at line 307)
- [x] Commit: `"libs: kernel checksum (CRC32/CRC32C) + base64/hex codec dispatch"`

**Test checkpoint:** `kcrc32("123456789",9) == 0xCBF43926` (IEEE vector); `kcrc32c("123456789",9) == 0xE3069283` (Castagnoli vector) on BOTH the SSE4.2 path and the table fallback; base64 round-trips RFC 4648 vectors and rejects bad padding; hex round-trips and rejects odd-length/invalid input. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | 414 kernel + 16 user-mode, 0 failures
> **Notes:**
> - `kcrc32`/`kcrc32c` (+ `_cont`) and base64/hex codec in `src/kernel/kchecksum.c` + `src/kernel/kcodec.c`; headers `include/kernel/kchecksum.h`, `include/kernel/kcodec.h`.
> - CRC tables are `const` rodata (no lazy init) so first-use across CPUs cannot race; CRC32C SSE4.2 path gated on the all-online-CPU `cpu_has(CPU_FEATURE_SSE4_2)` intersection.
> - `ixfs_crc32c`/`entropy_crc32c`/registry `hive_crc32` migrated to thin wrappers; outputs byte-identical so on-disk checksums are unchanged.
> - Test `klibs: checksum + codec` proves IEEE/Castagnoli vectors, hw==sw parity, split-vector continuation, base64/hex round-trip + malformed-input rejection.
> - Consumer migration (TLS PEM, SSH/SMTP/MIME) deferred to §7 + 07-networking; codec API is canonical and ready.
> **Verified:** 2026-06-20 | commit `f91239e3` | 5/6 items | build OK | tests 419 exec + 152 x86 PASS (KVM)
> **Deferred:** [L] Canonical codec consumer migration (TLS PEM, SSH/SMTP/MIME) deferred until consumers exist; codec API is ready (reason: scope -- no consumer code today) -> XREF: 02-kernel-core/TODO-03-kernel-libraries.md §7 (item: "Clone Mbed TLS 3.x source into `src/libs/mbedtls/`" at line 307)
> **Quality reviewed:** 2026-06-20 | Codex 6x (design, adversarial-impl, adversarial, consistency, perf, re-adversarial) | 2H+2M fixed | scope: kernel-code-quality

---

## 9. Zstandard (zstd) Compression

Zstandard is now a first-class compression baseline on both platforms: Linux ships `lib/zstd` in-kernel (btrfs/squashfs/zram/kernel image since 4.14) and Windows 11 24H2 ReFS volume compression offers selectable LZ4 **and** ZSTD levels. zstd delivers a deflate-class (or better) ratio at LZ4-class decompression speed, the right codec where ratio matters more than the raw write throughput LZ4 targets. This section owns the freestanding zstd codec; consumers select per-use between LZ4 (§3, speed) and zstd (§9, ratio). Source is NOT yet vendored (the headless runner has no network); the first item follows the same operator-fetch path that unblocked §3/§4.

- [/] Vendor zstd source (BSD-3 / dual GPLv2) into `src/libs/zstd/` -- BLOCKED: source not vendored and the headless runner has no network; needs the same operator interactive-fetch that vendored LZ4/miniz/mbedtls
- [/] Compile `-ffreestanding -nostdlib`; route `ZSTD_customMem` alloc/free -- same allocator blocker as §4 (zstd CCtx + decode window far exceed the 4 KiB `kmalloc` ceiling; per-call `pmm` is racy), so state must be a caller-provided workspace
- [/] Expose the single-shot block API in `include/libs/zstd.h` -- DEFERRED with the port
- [/] Untrusted-frame hardening (reject declared content size > `dst_cap`; cap decode window; never panic) -- DEFERRED with the port
- [/] Makefile build rule for `src/libs/zstd/` -- DEFERRED with the port
- [/] Wire into IXFS transparent compression as the `algo=2` (Zstd) path -> XREF: `05-storage-filesystems/TODO-07-ixfs-advanced-enterprise.md §3` (item: "ixfs_compress_block(algo, in_buf, in_len, out_buf, out_size)")
- [ ] Commit: `"libs: Zstandard freestanding port -- zstd_compress/decompress/bound, kmalloc+pmm allocator hooks"`

**Test checkpoint:** 64 KiB structured buffer round-trips through `zstd_compress` (level 3) / `zstd_decompress` byte-for-byte; `zstd_compress_bound(65536)` >= actual compressed size; a frame declaring content size larger than `dst_cap` is rejected without overrun; decode of a malformed frame returns -1 without panic; `klog(LOG_INFO, "zstd", ...)` selftest line on boot. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Notes:**
> - DEFERRED whole-section: zstd source is NOT vendored and the headless runner has no network to fetch it -- needs an operator interactive-fetch (the path that vendored LZ4/miniz/mbedtls).
> - Second blocker: zstd's CCtx + multi-MB decode window far exceed the 4 KiB `kmalloc` ceiling and per-call `pmm` is unsynchronized (the §3/§4 allocator blocker), so the port needs a caller-provided workspace owned by the IXFS consumer.
> - Scope boundary: §3 owns LZ4 (speed codec); §9 owns the zstd ratio codec; IXFS §3 selects `algo=2` once both source + workspace land.
> **Deferred:** [H] zstd freestanding port -- source not vendored (headless no network) + no SMP-safe allocation path (CCtx/window exceed kmalloc ceiling, pmm unsynchronized) + no consumer-owned workspace yet (reason: infra -- operator fetch + consumer workspace) -> XREF: 05-storage-filesystems/TODO-07-ixfs-advanced-enterprise.md §3 (item: "ixfs_compress_block(algo, in_buf, in_len, out_buf, out_size)")

---

## 10. SHA-3 / SHAKE (Keccak) Hash Family

SHA-3 (FIPS 202, Keccak) is a both-platform baseline the §5 Monocypher set (Blake2b only) and the existing `src/kernel/crypto/` SHA-2 family (SHA-1/256/384) do not cover: Windows 11 24H2 added `BCRYPT_SHA3_256/384/512` to CNG, and Linux ships `crypto/sha3_generic.c` in the kernel Crypto API. This section owns the freestanding SHA-3 fixed-output hashes plus the SHAKE128/256 extendable-output functions (XOFs) future post-quantum signatures (ML-DSA/SLH-DSA) and KMAC consume. Implemented in `src/kernel/crypto/sha3.c` alongside the existing `sha256.c`/`sha384.c`. (The 2026-04-13 gap-analysis noted "SHA3 24H2" but never filed an owner; this section closes that.)

- [x] Keccak-f[1600] core written from the FIPS 202 spec (public-domain, no fetch) in `src/kernel/crypto/sha3.c` + `sha3.h`; integer-only, zero heap (200-byte sponge in the caller context), endianness-neutral arithmetic lane access (ARM-port safe)
- [x] Fixed-output API: `sha3_256/384/512(in,len,out)` one-shot + per-variant `sha3_256_init/384_init/512_init` + `sha3_update`/`sha3_final` streaming (typed init wrappers over an internal common init; no exposed invalid-rate path)
- [x] SHAKE XOF API: `shake128`/`shake256(in,in_len,out,out_len)` one-shot squeeze; returns `int`, rejects `out_len > SHAKE_MAX_OUTPUT` (4096) / NULL / 0; streaming SHAKE deferred to its first consumer
- [x] NIST FIPS 202 KAT in `test_klibs.c` (TEST_CAT_EXEC): SHA3-256/384/512 + SHAKE128/256 vectors, independent hashlib boundary KATs (135/136/137), 200-byte multi-block SHAKE, streaming==one-shot, cap/NULL rejection
- [x] Kernel hash dispatch `crypto_hash`/`crypto_hash_digest_len` in `src/kernel/crypto/hash.c` + `hash.h`: selects SHA-256/384 or SHA3-256/384/512 by `crypto_hash_alg_t`, bounds-checked (unknown alg, undersized out, NULL+nonzero-len rejected)
- [x] Commit: `"crypto: SHA-3 (FIPS 202) SHA3-256/384/512 + SHAKE128/256 XOF, NIST KAT"`

**Test checkpoint:** `sha3_256("")` == NIST `a7ffc6f8...`; `sha3_512("abc")` == NIST vector; SHAKE128/256 squeeze matches the known-answer prefix (incl. a 200-byte multi-block squeeze); streaming `update` equals the one-shot digest and independent boundary vectors; `crypto_hash` dispatch returns the matching digest for all 5 algorithms. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | 3 suites (klibs: sha3 NIST KAT / sha3 streaming boundary / crypto_hash dispatch), 0 failures
> **Notes:**
> - Shipped `src/kernel/crypto/sha3.c` + `sha3.h`: FIPS 202 Keccak-f[1600], SHA3-256/384/512 (one-shot + streaming) + SHAKE128/256 XOF; pure, caller-owned 200-byte context -> SMP-safe by construction, zero heap.
> - Endianness-neutral: sponge lanes are read/written by arithmetic shift (never byte-aliased), so absorb/squeeze are correct on big-endian too (ARM64 port safety).
> - Shipped `src/kernel/crypto/hash.c` + `hash.h`: `crypto_hash` dispatch selecting SHA-2 or SHA-3 by `crypto_hash_alg_t`, bounds-checked; the kernel-wide hash selector CNG/TLS consumers bind to.
> - Validated against NIST + independent (Python hashlib) KATs across the pad10*1 rate boundary and a multi-block SHAKE squeeze; SHAKE capped at one page (`SHAKE_MAX_OUTPUT`).
> - Canonical doc: `include/kernel/crypto/sha3.h` banner.
> - Scope boundary: §10 owns SHA-3/SHAKE + the hash dispatch; SHA-2 stays in `sha256.c`/`sha384.c`, Blake2b in §5 Monocypher; streaming SHAKE XOF deferred to its first consumer (KMAC / ML-DSA).
> **Verified:** 2026-06-21 | commit `7f064ed8` | 5/5 items | build OK | tests 6798 kernel + 16 user PASS (TCG)
> **Quality reviewed:** 2026-06-21 | Codex 13x (design, adversarial, re-adversarial, consistency, perf, test-coverage) | 7H+4M+2L fixed, 0 open | scope: kernel-code-quality

---

## OS Comparison

| ⭐  | Feature                                                         | 🪟 Win11                    | 🐧 Linux                        | 🚀 Impossible OS                                           |
| --- | --------------------------------------------------------------- | --------------------------- | ------------------------------- | ---------------------------------------------------------- |
| 💎  | snprintf safe                                                   | ✅ Rtl safe                 | ✅ vsprintf                     | ✅ §1 libc                                                 |
| 💎  | Core string ops                                                 | ✅ Rtl mem                  | ✅ string.c                     | ✅ §1 libc                                                 |
| 💎  | Float math lib                                                  | ✅ CRT                      | ✅ libm                         | ✅ §2 libc (trig/exp/log/cbrt, few-ULP, IEEE)              |
| 💎  | LZ4 in kernel                                                   | ⚠️ Xpress                   | ✅ lib/lz4                      | ✅ §3 block codec (size-safe, frame deferred)              |
| 💎  | Deflate zlib                                                    | ✅ Rtl LZNT1                | ✅ zlib                         | ⬜ §4 miniz                                                |
| 💎  | Zstandard zstd                                                  | ⚠️ ReFS 24H2                | ✅ lib/zstd                     | ⬜ §9                                                      |
| 💎  | ZIP read                                                        | ✅ Shell                    | ✅ unzip                        | ⬜ §4                                                      |
| 💎  | ChaCha Poly AEAD                                                | ✅ BCrypt                   | ✅ chacha                       | ✅ §5 Monocypher                                           |
| 💎  | Blake2b                                                         | ✅ no native                | ✅ blake2b                      | ✅ §5 Monocypher                                           |
| 💎  | SHA-3 / SHAKE                                                   | ✅ CNG 24H2                 | ✅ sha3_generic                 | ✅ §10 Keccak (SHA3-256/384/512 + SHAKE128/256 + dispatch) |
| 💎  | Argon2id                                                        | ✅ KDF API                  | ✅ argon2                       | ✅ §5 Monocypher                                           |
| 💎  | X25519 ECDH                                                     | ✅ BCrypt                   | ✅ ecdh                         | ✅ §5 Monocypher                                           |
| 💎  | Ed25519                                                         | ✅ BCrypt                   | ✅ eddsa                        | ✅ §5 std + Blake2b EdDSA                                  |
| 💎  | Kernel CSPRNG                                                   | ✅ BCrypt                   | ✅ random.c                     | ✅ §5 fast-key-erasure ChaCha20                            |
| 💎  | getrandom syscall                                               | ✅ BCryptGenRandom          | ✅ getrandom(2)                 | ✅ §5 NtGetRandom 0x03D8                                   |
| 💎  | JSON in kernel                                                  | ❌ user COM                 | ❌ none std                     | ✅ §6 cJSON                                                |
| 💎  | TLS record crypto                                               | ✅ SChannel                 | ✅ ktls                         | ⬜ §7 mbed                                                 |
| 💎  | CRC32 / CRC32C dispatch                                         | ✅ RtlCrc32                 | ✅ lib/crc32                    | ✅ §8 kcrc32/kcrc32c (SSE4.2+table)                        |
| 💎  | base64 / hex codec                                              | ✅ Crypt32                  | ✅ lib/base64                   | ✅ §8 kcodec (strict+MIME)                                 |
| 💎  | UTF-16<->UTF-8 conversion                                       | ✅ RtlUnicode               | ✅ nls/utf8                     | ⬜ owned by T13 NLS                                        |
| ⭐  | One ring-0 CSPRNG façade (TLS seed + hiber + dumps + klog HMAC) | ⚠️ many BCrypt entry points | ⚠️ `get_random_bytes` + drivers | ✅ §5 `csprng_fill`/`csprng_add_entropy`                   |

After §1 through §8, freestanding libc and these libs unblock LZ4, miniz, crypto, JSON, the TLS record layer, and a shared checksum/codec dispatch for hibernation, dumps, IXFS, registry, and `07-networking` HTTPS.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_klibs()` (see `src/kernel/test/test_runner.c` and `include/kernel/test/test.h`). Use `test_suite_register_cat(..., TEST_CAT_EXEC)` for each case.
> Boot tests run with `debug=1` or `test=1` in `boot.conf`.

- [x] `src/kernel/test/test_klibs.c` covers every shipped section (§1/§2/§3/§5/§6/§8/§10; 6800 kernel + 16 user PASS, TCG 2026-06-21); only deferred §4/§7/§9 lack tests. Draft sub-bullets below are historical:
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
  - zstd compress/decompress round-trip (§9): 64 KiB input -> compressed -> decompressed matches; malformed frame rejected (-1, no panic) -- `TEST_SKIP`/`TEST_PENDING` until §9 source is vendored
  - SHA-3 (§10): `sha3_256("")` + `sha3_512("abc")` NIST FIPS 202 KAT; SHAKE128/256 squeeze prefix; streaming == one-shot -- `TEST_SKIP`/`TEST_PENDING` until §10 Keccak core lands
  - CRC32: known input -> matches precomputed CRC
  - SHA-256: `"abc"` -> known hash `ba7816bf...`
  - AES-128-ECB: encrypt + decrypt round-trip -> plaintext matches
  - [x] `kcrc32("123456789", 9)` -> `0xCBF43926` (IEEE); `kcrc32c("123456789", 9)` -> `0xE3069283` (Castagnoli) on SSE4.2 (`kcrc32c_hw_test`) + table (`kcrc32c_sw_test`) paths, plus split-vector continuation -- `test_checksum_codec` in `test_klibs.c`
  - [x] base64 encode/decode round-trips RFC 4648 vectors + MIME-whitespace mode; rejects bad padding/data-after-pad/len%4; `hex_encode`/`hex_decode` round-trip, reject odd-length/non-hex -- `test_checksum_codec`
- [x] Add `extern void test_register_klibs(void);` in `test_runner.c`, call `test_register_klibs()` from `test_runner_init()`
- [x] Skip-gating resolved: every shipped section wired real tests as it landed (no TEST_SKIP needed); only deferred §4/§7/§9 remain unwired.
- [x] Commit: kernel libraries test suite committed incrementally per section (latest §10 in `1d1098b2`).

---

## Verification

- [x] **snprintf**: width/precision/flags + truncation covered by `test_string_lib_edges` (`test_klibs.c`); 6800 kernel PASS on TCG 2026-06-21.
- [x] **Math**: `kmath_sin/cos/atan2/exp/sqrt` + IEEE edges covered by `test_math_lib` (`test_klibs.c`); PASS.
- [x] **LZ4**: compress/decompress round-trip + bound + malformed-frame + size-boundary covered by `test_lz4_block` (`test_klibs.c`); PASS.
- [ ] **miniz deflate**: compress 8 KiB string; decompress; verify identity. (§4 DEFERRED -- port blocked on consumer-owned workspace; no test yet.)
- [ ] **miniz ZIP**: open a 3-file ZIP from memory; enumerate entries; extract one file; verify content. (§4 DEFERRED.)
- [x] **Monocypher AEAD**: round-trip + tamper-reject covered by `test_aead_roundtrip` (`test_klibs.c`); PASS.
- [x] **CSPRNG**: distinct-output + core/golden-vector coverage in the `klibs: csprng *` suites (`test_klibs.c`); PASS.
- [x] **cJSON**: parse + field-extract + hardening covered by `test_json_lib` (`test_klibs.c`); PASS.
- [ ] **Mbed TLS selftest**: `mbedtls_aes_self_test(1)` and `mbedtls_sha256_self_test(1)` both return 0. (§7 DEFERRED -- port not started.)
- [x] **Checksum/codec**: `kcrc32`/`kcrc32c` match the IEEE/Castagnoli vectors on both SSE4.2 and table paths; base64 + hex round-trip RFC 4648 vectors and reject malformed input. (`test_checksum_codec`)
- [x] **SHA-3/SHAKE (§10)**: NIST FIPS 202 KATs + independent boundary vectors + multi-block SHAKE + dispatch covered by `test_sha3_kat`/`test_sha3_streaming`/`test_crypto_hash_dispatch` (`test_klibs.c`); PASS.
- [x] **Build check**: `bash scripts/build.sh clean` -> `=== BUILD OK ===` 2026-06-21 (all library files under `-ffreestanding -nostdlib`; BSS end 0x7ff000 < 0x800000).
- [ ] **Platforms:** run verification matrix on QEMU WHPX, QEMU TCG, VirtualBox, bare metal. (manual -- TCG covered 2026-06-21 6800/6800 + 16/16; WHPX/VirtualBox/bare metal pending user rig.)
- [x] Commit: `"libs: snprintf/vsnprintf, kmath, LZ4, Monocypher/CSPRNG, cJSON, CRC/codec, SHA-3 freestanding ports"`

**Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec)

---

## History

| Date | Action | Summary |
| --- | --- | --- |
| 2026-04-10 | validate | Removed ### N.M and model tags, ASCII `->`/`<=` sweep, Depends On uses §, fixed HTTP-TLS path to `07-networking/TODO-03-http-tls.md` §3, compact OS table after Implementation Order, §1/§4 status [x], per-section **Test checkpoint** + four platforms, Unit Tests -> `TEST_CAT_EXEC` + `test_runner.c`, added `scripts/debug/kernel/run-exec-tests.bat`, back-XREF on `TODO-32` Inputs, Goal aligned with libc snprintf present. **Parity:** Win11+Linux-strong rows covered by §2 through §7 except JSON row (Impossible leads). **Flag:** CRC32/AES unit bullets need real APIs from §4/§5 before tests compile. |
| 2026-04-13 | validate | Moved `## OS Comparison` after §7 and before `## Unit Tests`; each §1 through §7 now ends with `- [ ] Commit:` / `- [x] Commit:` before `**Test checkpoint:**`; Inputs XREF fixes (T10 stack canary §12, T15 AppContainer §14, Argon2 -> D09 `TODO-06-security-accounts.md` §2); OS table re-padded; IMPORTANT callout blank line removed; §1 gap-analysis blockquote empty `>` lines removed; prior History row date un-bolded. **Parity:** unchanged intent (⬜ rows tracked in §2 through §7). **Blocked deps:** Implementation Order §4 through §7 still depend on open sections on source TODOs. |
| 2026-04-13 | gap-analysis | 6 web + 2 Learn fetches (CNG alg IDs incl. ChaCha20-Poly1305 + SHA3 24H2; RtlCompressBuffer LZNT1/Xpress); code-truth: §1/§6 partial, §2 partial in `kmath.h` only, no lz4/miniz/monocypher/mbedtls dirs, no `test_register_klibs`; IO row §6 -> `[/]`; Outcome Argon2i to Argon2id; OS ⭐ CSPRNG façade row; Inputs D12 T01 COMPLEMENT; Unit Tests skip-gate; INDEX snprintf stale line; §1 gap block marked historical; INDEX line 47 substring; D12 T01 code-truth note; §2 kmath reconcile bullet. |
| 2026-04-14 | validate | Idempotent pass: OS Comparison after §7 before Unit Tests; Commit before Test checkpoint §1-§7; fixed gap-analysis History pipe; parity footnote lines 63-64 colon style; Inputs paths and XREF targets checked; run-exec-tests.bat present. **Parity:** ⬜ rows still §2-§7. **Flag:** external IO deps open; §6 [/] per partial cJSON. |
