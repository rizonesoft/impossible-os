# P0109 — Kernel Embedded Libraries

> **Goal:** Port freestanding third-party libraries into `src/libs/` — compression, crypto,
> math, and JSON. All redirect `malloc`/`free` to `kmalloc`/`kfree`. No OS dependencies.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. Compression (miniz)

**Prompt:** Port miniz (single-file, MIT license) into `src/libs/miniz/` — it provides zlib-compatible deflate/inflate and ZIP archive reading. Compile with `-ffreestanding -nostdlib` and redirect its `malloc`/`free` calls to `kmalloc`/`kfree` using `#define` overrides. miniz is needed by: IXFS transparent compression (Phase 06 §5.5), HTTP gzip content encoding (Phase 07), and the IPKG package format (Phase 12 §2.1). Test by compressing a known buffer, decompressing it, and comparing with the original. The ZIP API (`zip_open`, `zip_read_file`, `zip_close`) wraps miniz's `mz_zip_reader_*` functions. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"libs: miniz compression (deflate/ZIP)"`. Add notes, gotchas, and design decisions directly in this TODO section covering miniz integration, deflate/inflate API, and ZIP archive reading.

> **Memory note:** Decompression output buffers can easily exceed 4 KB. Use `pmm_alloc_contiguous()` for output buffers; `kmalloc` is fine only for the miniz internal state struct.

- [ ] Port **miniz** (single file, MIT) into `src/libs/miniz/`
- [ ] Redirect `malloc`/`free` → `kmalloc`/`kfree` via `#define` (state struct only — small)
- [ ] Use `pmm_alloc_contiguous()` for decompression output buffers > 4 KB
- [ ] Implement `deflate`/`inflate` for gzip support
- [ ] Implement ZIP archive read: `zip_open()`, `zip_read_file()`, `zip_close()`
- [ ] Test: compress a buffer ≥ 4 KB, decompress, verify byte-for-byte integrity
- [ ] Test: list and extract files from a ZIP archive
- [ ] Commit: `"libs: miniz compression (deflate/ZIP)"`

---

## 2. Cryptography (monocypher)

**Prompt:** Port monocypher (BSD-2, ~3000 lines) into `src/libs/monocypher/` — it provides ChaCha20 (stream cipher), Poly1305 (MAC), Blake2b (hash), Argon2i (password hash), X25519 (key exchange), and Ed25519 (signatures). Compile with `-ffreestanding`. This library is the cryptographic foundation for: password hashing in Phase 09 §1.2, TLS in Phase 07 §5, executable signing in Phase 09 §8, and data encryption in Phase 09 §4. The kernel CSPRNG should seed from RDRAND (via inline assembly) and use ChaCha20 to generate random bytes. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"libs: monocypher crypto primitives"`. Add notes, gotchas, and design decisions directly in this TODO section covering monocypher integration, CSPRNG seeding, and exposed crypto primitives.

> **No malloc needed:** monocypher uses only caller-provided buffers — pure `-ffreestanding` with zero heap use.

- [ ] Port **monocypher** (~3000 lines, BSD-2) into `src/libs/monocypher/`
- [ ] Expose: ChaCha20 (stream cipher), Poly1305 (MAC), Blake2b (hash)
- [ ] Expose: Argon2i (password hash), X25519 (key exchange), Ed25519 (sign/verify)
- [ ] Implement kernel CSPRNG: `csprng_fill(buf, len)` — seed from `RDRAND`, stream via ChaCha20
- [ ] Expose `/dev/random` equivalent for user-mode via `SYS_GETRANDOM` syscall
- [ ] Test: hash a known string with Blake2b, compare to reference value
- [ ] Commit: `"libs: monocypher crypto primitives"`

---

## 3. Math Library

**Prompt:** `kmath.h` already exists (`include/kernel/kmath.h`) with `fabs`, `floor`, and a subset of math functions required by stb_truetype. Verify what's already there and extend with any missing functions needed by audio synthesis (Phase 08) and the TTS engine (Phase 13 §7.1). Port remaining functions from OpenLibm or musl libc (both MIT-compatible) into `src/libs/math/`. Do NOT duplicate what's in `kmath.h` — extend it. Key missing functions to check: `sin`, `cos`, `tan`, `exp`, `log`, `pow`, `sqrt`, `ceil`, `fmod`, `atan2`. Compile with `-msse2`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"libc: floating-point math functions"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> **Note:** `kmath.h` exists from the stb_truetype integration — audit it before porting to avoid duplication.

- [ ] Audit `include/kernel/kmath.h` — document which functions exist vs. missing
- [ ] Port missing trig: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`
- [ ] Port missing: `exp`, `log`, `log2`, `log10`, `pow`
- [ ] Port missing: `sqrt`, `cbrt`, `ceil`, `round`, `trunc`, `fmod`, `remainder`
- [ ] Verify: compile all with `-msse2`, zero dependency on libm
- [ ] Commit: `"libc: floating-point math functions"`

---

## 4. JSON Parser (cJSON)

**Prompt:** Port cJSON (MIT, ~2000 lines) into `src/libs/cjson/` — it provides a simple DOM-style JSON parser and serializer. Redirect `malloc`/`free` to `kmalloc`/`kfree` via `cJSON_InitHooks()`. cJSON is used for: the update manifest (Phase 12 §1.1), theme definitions (Phase 04 desktop theming), NTP server lists, and any future REST API interaction. Test by parsing a JSON string like `{"version": "1.0", "name": "Impossible OS"}`, extracting values with `cJSON_GetObjectItem`, and verifying correctness. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"libs: cJSON parser"`. Add notes, gotchas, and design decisions directly in this TODO section covering cJSON integration, memory hook setup, and usage examples.

> **Memory note:** Parsed JSON trees can grow large. Audit cJSON item allocations — if a single document could exceed 4 KB total, consider limiting max document depth or switching to a streaming parser.

- [ ] Port **cJSON** (MIT, ~2000 lines) into `src/libs/cjson/`
- [ ] Redirect `malloc`/`free` → `kmalloc`/`kfree` via `cJSON_InitHooks()` at kernel init
- [ ] Test: parse `{"version": "1.0", "name": "Impossible OS"}`, verify field extraction
- [ ] Test: serialize a cJSON object back to string, verify round-trip
- [ ] Use for: settings files, theme definitions, asset manifests, update manifests
- [ ] Commit: `"libs: cJSON parser"`

---

## 5. String Library (libc shims)

**Prompt:** Freestanding kernel code needs `memset`, `memcpy`, `memmove`, `strlen`, `strcpy`, `strncpy`, `strcmp`, `strncmp`, `strstr`, `strtol`, `snprintf`, `vsnprintf` — but cannot use the system libc. Audit `src/kernel/string.c` and `src/kernel/kprintf.c` to see what already exists, then fill missing gaps. `snprintf`/`vsnprintf` are the most critical — they must be correct and safe (no buffer overflow). Compile with `-ffreestanding -nostdlib`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"libc: string and printf shims"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> **Note:** Audit first — many of these may already exist scattered across the kernel. Consolidate into `src/libs/libc/string.c`.
- [ ] Audit existing string functions across kernel source files
- [ ] Consolidate into `src/libs/libc/string.c` + `include/libc/string.h`
- [ ] Implement: `memset`, `memcpy`, `memmove`, `memcmp`, `memchr`
- [ ] Implement: `strlen`, `strcpy`, `strncpy`, `strcat`, `strncat`
- [ ] Implement: `strcmp`, `strncmp`, `strstr`, `strchr`, `strrchr`
- [ ] Implement: `strtol`, `strtoul`, `atoi`
- [ ] Implement: `snprintf`, `vsnprintf` (safe, no overflow)
- [ ] Verify all compile with `-ffreestanding -nostdlib`
- [ ] Commit: `"libc: string and printf shims"`

---

## Priority Order

| Priority | Section                   | Reason                                                  |
|----------|---------------------------|---------------------------------------------------------|
| 🔴 P0    | 5. String library         | Audit/consolidate — likely partially done; fill gaps    |
| 🟠 P1    | 3. Math Library           | Audit first — likely mostly done via `kmath.h`          |
| 🟠 P1    | 4. cJSON                  | Needed for theme/config loading in desktop phase        |
| 🟠 P1    | 1. miniz                  | Needed for IXFS compression (Phase 06) and HTTP gzip    |
| 🟡 P2    | 2. monocypher             | Needed for Phase 07 TLS and Phase 09 security           |

---

## OS Comparison

| Library / Primitive     | Windows Kernel (WDK)        | Linux Kernel                | Impossible OS                       |
|-------------------------|-----------------------------|-----------------------------|-------------------------------------|
| String functions        | ✅ `RtlCopy/MoveMemory` etc | ✅ `lib/string.c`            | ⬜ §5 P0 — audit + consolidate    |
| Math functions          | ✅ `rtlmath`                | ✅ `lib/math/`               | ⬜ §3 P1 — extend `kmath.h`       |
| Compression (zlib)      | ✅ `RtlDecompressBuffer`    | ✅ `lib/zlib_deflate/`       | ⬜ §1 P1 — miniz                  |
| Cryptography            | ✅ BCrypt kernel APIs       | ✅ `crypto/` subsystem       | ⬜ §2 P2 — monocypher             |
| JSON parser             | ❌ (COM-based, user-mode)   | ❌                           | ⬜ §4 P1 — **in-kernel JSON**     |
| **In-kernel JSON**      | ❌                          | ❌                           | ⬜ **§4 — Impossible OS only**    |
