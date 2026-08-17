# Vendored third-party libraries -- provenance

Upstream source, version, and license for every third-party library vendored
under `src/libs/`. Versions are the values embedded in each library's own
headers (authoritative and re-checkable in-tree). All licenses are compatible
with the project's GPL-3.0-only license.

Reviewed and approved as load-bearing on 2026-07-16 (CLAUDE.md dependency gate).

| Library | Version (embedded macro) | Upstream | License | Vendored in |
| --- | --- | --- | --- | --- |
| LZ4 | 1.10.0 (`LZ4_VERSION_{MAJOR,MINOR,RELEASE}`) | https://github.com/lz4/lz4 | BSD-2-Clause | `38525b91` (2026-06-20) |
| miniz | 11.0.2 (`MZ_VERSION`) | https://github.com/richgel999/miniz | MIT | `38525b91` (2026-06-20) |
| Mbed TLS | 3.6.2 (`MBEDTLS_VERSION_NUMBER 0x03060200`) | https://github.com/Mbed-TLS/mbedtls | Apache-2.0 OR GPL-2.0-or-later | `38525b91` (2026-06-20) |
| cJSON | 1.7.18 (`CJSON_VERSION_{MAJOR,MINOR,PATCH}`) | https://github.com/DaveGamble/cJSON | MIT | `389812e6` (2026-03-27) |
| Monocypher | 4.0.2 (source header banner; no version macro) | https://monocypher.org | BSD-2-Clause OR CC0-1.0 | `99910014` (2026-06-12) |

## License notes

- **LZ4** ships dual-licensed upstream: the `lib/` sources are BSD-2-Clause,
  the `programs/` sources are GPL-2.0-or-later. Only `lib/` files are vendored
  here (`lz4.c`, `lz4hc.c`, `lz4frame.c`, `xxhash.c` and their headers), so the
  effective license for our copy is **BSD-2-Clause**. Each library ships its
  upstream `LICENSE` file alongside the source; cJSON's MIT text lives in
  `src/libs/cjson/LICENSE`.
- **Mbed TLS** and cJSON provide their own `LICENSE` files in-tree.
- **Monocypher** is dual-licensed BSD-2-Clause OR CC0-1.0 and ships its license text as `LICENCE.md` (British spelling). A license sweep globbing `LICENSE*` will not match it; check for both spellings.

User-facing attribution for every entry in this table lives in [CREDITS.md](../../CREDITS.md) at the repository root, which is the notice set that must accompany a redistributed binary image. Update both files in the same commit.

## Local additions (NOT upstream)

- `src/libs/lz4/lz4_merge.ld` -- project linker glue for merging LZ4 sections
  into the kernel image. Authored here, not part of upstream LZ4.

## Updating a vendored library

Re-drop the upstream `lib/` (or equivalent) tree, keep the upstream `LICENSE`
file, preserve any local additions listed above, and update the version + commit
row here in the same commit.
