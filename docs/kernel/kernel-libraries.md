<!-- docs: covers=todo/02-kernel-core/TODO-03-kernel-libraries.md sources=include/libc/string.h,src/libc/string.c,include/libc/math.h,include/kernel/kmath.h,include/libs/lz4.h,src/kernel/lz4.c,src/libs/lz4/lz4.c,include/kernel/csprng.h,src/kernel/csprng.c,src/libs/monocypher/monocypher.h,include/kernel/json.h,src/kernel/json.c,src/libs/cjson/cJSON.h,include/kernel/kchecksum.h,src/kernel/kchecksum.c,include/kernel/kcodec.h,src/kernel/kcodec.c,include/kernel/crypto/sha3.h,src/kernel/crypto/sha3.c,include/kernel/crypto/hash.h,src/kernel/crypto/hash.c,src/kernel/test/test_klibs.c,src/libs/PROVENANCE.md reviewed=2026-09-28 order=3 -->
# Kernel Embedded Libraries

## What is it?

Impossible OS is a freestanding kernel: no libc, no libm, no standard headers. Every general-purpose facility the kernel needs (formatted output, floating-point math, compression, cryptography, JSON, checksums and hashing) has to be written or ported to run under `-ffreestanding -nostdlib` with the kernel's own allocators. This page covers that library layer.

Seven libraries are built into the kernel today: the string and `snprintf` library, the floating-point math headers, the LZ4 block compressor, Monocypher with the kernel CSPRNG, the cJSON parser, CRC32/CRC32C with base64 and hex codecs, and SHA-3/SHAKE. LZ4 ships its block codec, which klog uses to compress rotated log files, but not its frame layer. Three more are not in the kernel image: miniz and Mbed TLS are vendored as source but not built, and Zstandard is not vendored at all.

The rule that ties it together is the allocator boundary: `kmalloc()` for library state up to 4 KiB, `pmm_alloc_contiguous()` for anything larger. Every port either fits under that cap or makes its caller own the workspace instead of allocating one internally.

## How does it work?

**String and formatting** ([`string.c`](../../src/libc/string.c)) is hand-written: the `mem*` and `str*` families, `strlcpy`/`strlcat`, `strtol`/`strtoul`/`atoi`, and a full `vsnprintf`/`snprintf` with width, precision, length modifiers and truncation semantics.

**Floating-point math** lives entirely in headers ([`math.h`](../../include/libc/math.h), extending [`kmath.h`](../../include/kernel/kmath.h)). Because the kernel builds with `-mno-sse` and `-mno-sse2`, an out-of-line function returning a `double` has no legal return register, so every routine is `static inline` and always inlined into its caller. `kmath.h` keeps the font-grade helpers used by `stb_truetype` and cJSON; `libc/math.h` adds only the names `kmath.h` lacks, so a file can include both.

**LZ4** is vendored under `src/libs/lz4/` and wrapped by [`lz4.c`](../../src/kernel/lz4.c). The kernel builds only the block codec (`LZ4_FREESTANDING=1`). The wrapper takes `size_t` lengths and rejects anything above `LZ4_BLOCK_INPUT_MAX` before narrowing to LZ4's internal `int` API, so a 64-bit length can never truncate silently. Compression state lives on the caller's stack; nothing is allocated.

**Monocypher and the CSPRNG**: Monocypher supplies ChaCha20, Poly1305, AEAD, Blake2b, Argon2id, X25519 and Ed25519 as zero-allocation C. [`csprng.c`](../../src/kernel/csprng.c) builds a fast-key-erasure generator on top: `csprng_init()` seeds a ChaCha20 key from a Blake2b-conditioned transcript of the boot entropy sources, and every `csprng_fill()` ratchets the key under one short irqsave spinlock hold and streams the caller's bytes from a single-use key outside the lock, so earlier output cannot be recovered and bulk generation never holds the lock.

**cJSON** is vendored under `src/libs/cjson/` with its allocator hooks pointed at the kernel heap and hardened for untrusted input: a nesting limit of 32 (`CJSON_NESTING_LIMIT`) and a 1 MiB input cap (`JSON_MAX_INPUT`). [`json.c`](../../src/kernel/json.c) wraps it so callers never touch cJSON internals.

**Checksums and codecs** ([`kchecksum.c`](../../src/kernel/kchecksum.c), [`kcodec.c`](../../src/kernel/kcodec.c)) give the kernel one CRC-32 (IEEE) and one CRC-32C (Castagnoli) instead of per-subsystem copies. Both tables are `const` data, so there is no racy first-use initialization. CRC-32C also has an SSE4.2 path using the `crc32` instruction (a general-purpose-register instruction, safe under `-mno-sse2`), chosen only when every online CPU has the feature. The base64 and hex codecs write into a caller buffer, bounds-checked against its capacity.

**SHA-3/SHAKE** ([`sha3.c`](../../src/kernel/crypto/sha3.c)) implements the FIPS 202 Keccak-f[1600] sponge over 25 `uint64_t` lanes with a caller-owned context, so it has no shared state. [`hash.c`](../../src/kernel/crypto/hash.c) adds `crypto_hash(alg, ...)`, which selects SHA-2 or SHA-3 by enum.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `snprintf`, `vsnprintf` | Bounds-checked formatted output that always terminates the string ([`string.h`](../../include/libc/string.h)) |
| `kmath_sin`, `kmath_atan2`, `kmath_exp`, `kmath_log` and float variants | Software math, header-only ([`math.h`](../../include/libc/math.h)) |
| `lz4_compress`, `lz4_decompress`, `lz4_compress_bound` | Size-safe LZ4 block codec, no allocation ([`lz4.h`](../../include/libs/lz4.h)) |
| `csprng_init`, `csprng_fill`, `csprng_u64`, `csprng_add_entropy` | Kernel CSPRNG ([`csprng.h`](../../include/kernel/csprng.h)) |
| `NtGetRandom` | User-mode access to `csprng_fill` ([`csprng.c`](../../src/kernel/csprng.c)) |
| `json_parse`, `json_parse_len`, `json_get`, `json_str`, `json_int`, `json_print` | Typed cJSON wrapper ([`json.h`](../../include/kernel/json.h)) |
| `kcrc32`, `kcrc32_cont`, `kcrc32c`, `kcrc32c_cont` | CRC-32 and CRC-32C ([`kchecksum.h`](../../include/kernel/kchecksum.h)) |
| `base64_encode`, `base64_decode`, `hex_encode`, `hex_decode` | Codecs into caller buffers ([`kcodec.h`](../../include/kernel/kcodec.h)) |
| `sha3_256`, `sha3_384`, `sha3_512`, `shake128`, `shake256` | FIPS 202 hashes, one-shot and streaming ([`sha3.h`](../../include/kernel/crypto/sha3.h)) |
| `crypto_hash` | Digest selection across SHA-2 and SHA-3 ([`hash.h`](../../include/kernel/crypto/hash.h)) |
| Monocypher `crypto_*` functions | Vendored primitives under their upstream names ([`monocypher.h`](../../src/libs/monocypher/monocypher.h)) |

## How do I use it?

Include the header and call the function; none of these libraries needs initialization except the CSPRNG, which boot seeds for you.

```c
char msg[64];
snprintf(msg, sizeof(msg), "value=%d", 42);

uint8_t key[32];
csprng_fill(key, sizeof(key));

uint32_t crc = kcrc32(data, len);
```

The library tests live in [`test_klibs.c`](../../src/kernel/test/test_klibs.c), registered by `test_register_klibs()` in the exec category: string edge cases, LZ4 round trips, SHA-3 NIST known-answer and streaming tests, the `crypto_hash` dispatch, checksums and codecs, cJSON, AEAD and the CSPRNG.

```bash
bash scripts/test.sh SUITE=exec
```

## What is not implemented yet?

- **miniz** (deflate, inflate and ZIP) is vendored under `src/libs/miniz/` but not built: its decoder and encoder state are far larger than the 4 KiB `kmalloc` ceiling and too large for a stack frame, so the port needs a caller-owned workspace, and there is no in-tree consumer yet ([miniz Deflate / Inflate + ZIP](../../todo/02-kernel-core/TODO-03-kernel-libraries.md#4-miniz-deflate--inflate--zip)).
- **Mbed TLS** is vendored under `src/libs/mbedtls/` but not yet trimmed to a freestanding configuration or linked; networking's HTTPS work waits on it ([Mbed TLS Freestanding Port](../../todo/02-kernel-core/TODO-03-kernel-libraries.md#7-mbed-tls-freestanding-port)).
- **Zstandard** is not vendored; once it is, it hits the same oversized-state problem as miniz. IXFS transparent compression is the planned consumer ([Zstandard (zstd) Compression](../../todo/02-kernel-core/TODO-03-kernel-libraries.md#9-zstandard-zstd-compression)).
- **More LZ4 consumers and framing**: log rotation already uses LZ4, but the crash dump, hibernation and EIF consumers are not built, and the `.lz4` frame layer is deferred because its context exceeds the `kmalloc` ceiling ([LZ4 Block Compressor](../../todo/02-kernel-core/TODO-03-kernel-libraries.md#3-lz4-block-compressor)).

## How does it compare with Windows 11 and Linux?

Most rows are parity work. `snprintf`, string functions and a math library match the Windows CRT and glibc or musl. LZ4 matches Linux's in-kernel `lib/lz4`. ChaCha20-Poly1305, Blake2b, Argon2id, X25519 and Ed25519 match Windows CNG and the Linux crypto API. SHA-3 matches the SHA-3 algorithms Windows 11 24H2 added to CNG and Linux's `sha3_generic`. CRC and base64 match `RtlCrc32` and Linux `lib/crc32` and `lib/base64`.

JSON in the kernel goes beyond both: Windows parses JSON only in user mode and a stock Linux kernel does not parse it at all. The CSPRNG is also a single ring-0 entry point, where Windows spreads the role across many CNG calls and Linux across `get_random_bytes` and driver RNGs.

Deflate (Windows has LZNT1 and Xpress, Linux has zlib), a TLS record layer (SChannel, kTLS) and Zstandard (Linux `lib/zstd`) are still open here.

## See also

- [Kernel Embedded Libraries roadmap](../../todo/02-kernel-core/TODO-03-kernel-libraries.md)
- [Vendored library provenance](../../src/libs/PROVENANCE.md)
- [Early Entropy and Random Seed](../boot/early-entropy-random-seed.md)
