# Colour Icons: License Notice

The colour icons in this directory are original Impossible OS artwork, authored on 2026-09-27 and licensed under **GPL-3.0-only** like the rest of the repository (see [LICENSE](../../../LICENSE)).

- **Sources:** hand-written SVGs in [`resources/icons/src/`](../src/), one file per icon on a 48x48 grid.
- **Rasters:** the PNGs in `16/` through `256/` are rendered from those sources by `bash scripts/convert-icons.sh` and committed, so a build never needs an SVG renderer. `irespack` packs them into `icons.ires` at build time.
- **Spec:** the grid, palette, light direction and small-size rules are in [docs/design/icons.md](../../../docs/design/icons.md).

The Icons8 colour icons this directory used to reference (paid license, not redistributable as standalone files) are no longer used and are no longer present on `main`.
