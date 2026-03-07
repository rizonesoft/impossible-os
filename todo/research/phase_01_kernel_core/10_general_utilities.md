# Fonts, Compression, Crypto & Utility Libraries — Research

## Overview

Essential supporting libraries for a complete OS: font rendering, compression (ZIP/GZIP), cryptography, math, and general utilities.

---

## Font Rendering

### Libraries

| Library | License | Redistributable | Size | Notes |
|---------|---------|-----------------|------|-------|
| **stb_truetype** | MIT / Public Domain | ✅ Yes | Single header, ~5000 lines | TrueType/OpenType rasterizer |
| **FreeType** | FreeType License (BSD-like) or GPL 2.0 | ✅ Yes | ~150K lines | Industry standard, full hinting |
| **libschrift** | ISC | ✅ Yes | ~1500 lines | Minimal TrueType renderer |

### Open-Source Fonts (redistributable)

| Font | License | Style | Notes |
|------|---------|-------|-------|
| **Noto Sans/Serif** | OFL 1.1 | Sans/Serif | Google, covers all Unicode |
| **Inter** | OFL 1.1 | Sans | Modern UI font |
| **JetBrains Mono** | OFL 1.1 | Monospace | Excellent for terminals |
| **Fira Code** | OFL 1.1 | Monospace | Ligatures for code |
| **Source Code Pro** | OFL 1.1 | Monospace | Adobe, clean |
| **Liberation Sans/Mono** | OFL 1.1 | Sans/Mono | Metric-compatible with Arial/Courier |
| **Roboto** | Apache 2.0 | Sans | Android's default |

> [!TIP]
> **stb_truetype** is ideal for Impossible OS. Single header, public domain, renders TrueType fonts to bitmaps. Combined with a redistributable font like **Inter** or **JetBrains Mono**, we get beautiful text rendering with no licensing issues.

> **Current state**: Impossible OS uses a custom 8×16 bitmap font (`font.c`). stb_truetype would be a major visual upgrade.

---

## Compression

### Libraries

| Library | License | Redistributable | Size | Formats | Notes |
|---------|---------|-----------------|------|---------|-------|
| **miniz** | MIT | ✅ Yes | Single file, ~4000 lines | deflate, zlib, ZIP | Drop-in replacement for zlib |
| **zlib** | Zlib | ✅ Yes | ~15K lines | deflate, gzip | The standard |
| **lz4** | BSD-2 | ✅ Yes | ~3000 lines | LZ4 | Extremely fast |
| **zstd** | BSD-3 | ✅ Yes | ~30K lines | Zstandard | Best ratio + speed balance |
| **brotli** | MIT | ✅ Yes | ~20K lines | Brotli | Used in HTTP |
| **minizip** | Zlib | ✅ Yes | Uses miniz/zlib | ZIP archives | Read/write ZIP files |

### Why we need compression

| Use case | Format | Library |
|----------|--------|---------|
| **JAR files** (Java) | ZIP | miniz |
| **PNG decoding** | deflate | Already in stb_image |
| **HTTP content** | gzip/brotli | miniz / brotli |
| **Disk image compression** | LZ4/zstd | lz4 / zstd |
| **Archive support** (`.zip`) | ZIP | miniz |

> [!TIP]
> **miniz is the single best choice.** One C file, MIT license, handles deflate + zlib + ZIP. Perfect as a foundation for PNG, Java JARs, and general archive support.

---

## Cryptography

### Libraries

| Library | License | Redistributable | Size | Notes |
|---------|---------|-----------------|------|-------|
| **BearSSL** | MIT | ✅ Yes | ~30K lines | Minimal SSL/TLS, constant-time |
| **wolfSSL** | GPL 2.0 (free) / Commercial | ⚠️ GPL version | ~50K lines | Embedded-focused |
| **Mbed TLS** | Apache 2.0 | ✅ Yes | ~60K lines | ARM's TLS library |
| **libsodium** | ISC | ✅ Yes | ~25K lines | Modern crypto primitives |
| **monocypher** | BSD-2 | ✅ Yes | ~3000 lines | Minimal, audited |
| **tweetnacl** | Public Domain | ✅ Yes | ~100 lines | Tiny, educational |
| **tiny-AES-c** | Public Domain | ✅ Yes | ~500 lines | AES-128/192/256 |

### What we'd need crypto for

| Use case | Algorithm | Library |
|----------|-----------|---------|
| **HTTPS** | TLS 1.2/1.3 | BearSSL or Mbed TLS |
| **Password hashing** | Argon2, bcrypt | libsodium |
| **Random numbers** | ChaCha20, RDRAND | monocypher or custom |
| **File encryption** | AES-256-GCM | libsodium or tiny-AES |
| **SSH** | RSA, Ed25519, AES | BearSSL |

> [!TIP]
> **monocypher** (3000 lines, BSD-2) is the best starting point for Impossible OS. It provides all essential primitives: X25519, Ed25519, ChaCha20, Blake2b, Argon2.

---

## Math Libraries

| Library | License | Redistributable | Size | Notes |
|---------|---------|-----------------|------|-------|
| **fdlibm** | Public Domain | ✅ Yes | ~10K lines | Sun's math library |
| **musl libc math** | MIT | ✅ Yes | ~15K lines | Part of musl, standalone-able |
| **OpenLibm** | MIT/BSD | ✅ Yes | ~20K lines | Portable `libm` successor |

> **Current state**: Impossible OS has a basic `user/lib/math.c`. For more functions (sin, cos, exp, log, pow, sqrt), port routines from **OpenLibm** or **musl** (both MIT).

---

## Networking

| Library | License | Redistributable | Size | Notes |
|---------|---------|-----------------|------|-------|
| **lwIP** | BSD-3 | ✅ Yes | ~30K lines | Full TCP/IP stack for embedded |
| **picoTCP** | GPL 2.0 / Commercial | ⚠️ GPL | ~25K lines | Modular TCP/IP stack |
| **uIP** | BSD-3 | ✅ Yes | ~5K lines | Minimal TCP/IP for microcontrollers |
| **smoltcp** | BSD-0 | ✅ Yes | Rust | Modern, for Rust-based systems |

> **Current state**: Impossible OS has custom Ethernet/ARP/IPv4/ICMP/UDP/DHCP. For full TCP support, consider porting **lwIP** (BSD-3, widely used in embedded OS).

---

## XML / JSON Parsing

| Library | License | Redistributable | Size | Notes |
|---------|---------|-----------------|------|-------|
| **cJSON** | MIT | ✅ Yes | ~2000 lines | JSON parser/generator |
| **yxml** | MIT | ✅ Yes | ~600 lines | Minimal streaming XML parser |
| **mxml** | Apache 2.0 | ✅ Yes | ~8000 lines | Full XML parser |

---

## Master License Compatibility Table

All libraries marked ✅ below can be bundled with Impossible OS without legal issues:

| License | Commercial | Redistribute binary | Redistribute source | Must share changes | Attribution |
|---------|-----------|--------------------|--------------------|-------------------|-------------|
| **Public Domain / CC0** | ✅ | ✅ | ✅ | ❌ | ❌ |
| **MIT** | ✅ | ✅ | ✅ | ❌ | ✅ (include license) |
| **BSD-2 / BSD-3** | ✅ | ✅ | ✅ | ❌ | ✅ (include license) |
| **Zlib** | ✅ | ✅ | ✅ | ❌ | ❌ |
| **ISC** | ✅ | ✅ | ✅ | ❌ | ✅ (include license) |
| **Apache 2.0** | ✅ | ✅ | ✅ | ❌ | ✅ + patent grant |
| **OFL 1.1** (fonts) | ✅ | ✅ | ✅ | ❌ | ✅ (include license) |
| **LGPL 2.1** | ✅ | ✅ (dynamic link) | ✅ | ⚠️ Changes to library | ✅ |
| **GPL 2.0** | ⚠️ | ⚠️ (whole program GPL) | ✅ | ✅ All linked code | ✅ |

> [!IMPORTANT]
> **Stick to Public Domain, MIT, BSD, Zlib, ISC, and Apache 2.0 libraries.** These allow full redistribution with no copyleft obligations. Avoid GPL-only libraries unless they're isolated in a separate process.
