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
| ACPICA | 20260408 (`ACPI_CA_VERSION`) | https://github.com/acpica/acpica | Intel ACPI CA OR BSD-3-Clause OR GPL-2.0 (BSD arm taken) | `077d74a4` (2026-08-17), vendored at `src/kernel/acpica/` |
| Monocypher | 4.0.2 (source header banner; no version macro) | https://monocypher.org | BSD-2-Clause OR CC0-1.0 | `99910014` (2026-06-12) |
| markdown-it-py | 3.0.0 (`markdown_it.__version__`) | https://github.com/executablebooks/markdown-it-py (PyPI wheel) | MIT | 2026-09-27, vendored at `tools/vendor/markdown_it/` (host tool, not in the image) |
| mdurl | 0.1.2 (`mdurl.__version__`) | https://github.com/executablebooks/mdurl (PyPI wheel) | MIT | 2026-09-27, vendored at `tools/vendor/mdurl/` (host tool, not in the image) |

## License notes

- **LZ4** ships dual-licensed upstream: the `lib/` sources are BSD-2-Clause,
  the `programs/` sources are GPL-2.0-or-later. Only `lib/` files are vendored
  here (`lz4.c`, `lz4hc.c`, `lz4frame.c`, `xxhash.c` and their headers), so the
  effective license for our copy is **BSD-2-Clause**. Each library ships its
  upstream `LICENSE` file alongside the source; cJSON's MIT text lives in
  `src/libs/cjson/LICENSE`.
- **Mbed TLS** and cJSON provide their own `LICENSE` files in-tree.
- **ACPICA** is TRIPLE-licensed: the Intel ACPI Component Architecture license, a BSD-3-Clause-style license, OR GPL-2.0. We take the **BSD-3-Clause arm**, which is compatible with GPL-3.0-only; the GPL-2.0 arm is NOT (see CREDITS.md). `04-drivers-hardware/TODO-03` recorded it as Apache-2.0, which was wrong and is corrected there.
- **ACPICA** lives under `src/kernel/acpica/` rather than `src/libs/`, because the TODO that owns it specified that path and the OS Services Layer sits beside it in `src/kernel/`. It is the only vendored library outside `src/libs/`; a license sweep must cover both roots.
- **Monocypher** is dual-licensed BSD-2-Clause OR CC0-1.0 and ships its license text as `LICENCE.md` (British spelling). A license sweep globbing `LICENSE*` will not match it; check for both spellings.

User-facing attribution for every entry in this table lives in [CREDITS.md](../../CREDITS.md) at the repository root, which is the notice set that must accompany a redistributed binary image. Update both files in the same commit.

## Local additions (NOT upstream)

- `src/libs/lz4/lz4_merge.ld` -- project linker glue for merging LZ4 sections
  into the kernel image. Authored here, not part of upstream LZ4.
- `src/kernel/acpica/include/platform/acimpossible.h` -- the platform header
  ACPICA requires per host OS. Authored here, modelled on upstream `aczephyr.h`.
- `src/kernel/acpica/include/platform/acenv.h` -- ONE upstream line changed: an
  `#elif defined(__IMPOSSIBLE_OS__)` branch added to the platform dispatch
  chain, which otherwise ends in `#error Unknown target environment`. This is
  the entire delta against upstream ACPICA; everything else is verbatim.

Excluded from the ACPICA build (see the Makefile for the measured reasons):
`components/debugger`, `components/disassembler`, `source/tools`,
`source/os_specific` (we supply our own OSL), plus `rsdump.c` (debug-only, its
descriptor tables compile out) and `utprint.c` (defines snprintf/vsnprintf
unconditionally, colliding with the kernel formatter).

## Updating a vendored library

Re-drop the upstream `lib/` (or equivalent) tree, keep the upstream `LICENSE`
file, preserve any local additions listed above, and update the version + commit
row here in the same commit.
