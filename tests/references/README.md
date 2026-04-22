# tests/references/

Known-good reference PNG screenshots for visual regression testing.

These images are captured once from a golden-path boot via
`scripts/qemu-screenshot.sh` and committed as the baseline that
`scripts/compare-screenshot.sh` checks future runs against.

## Naming convention

`<scenario>-<resolution>.png`, lower-kebab-case.

| Name | Source | Consumer |
|---|---|---|
| `desktop-idle-1280x720.png` | Fresh boot, DESKTOP_READY, no user input | Smoke test baseline |
| `cmd-prompt-1280x720.png` | Fresh boot with `Command Prompt` window focused and empty | §5 terminal verification |

Captures are always PNG (24-bit RGB), taken from the QEMU HMP monitor
`screendump` path, converted via ImageMagick `convert` (see
`scripts/qemu-screenshot.sh`). The native framebuffer is BGRA; the
`screendump` PPM emission + PNG conversion strips the alpha channel.

## Updating a reference

A reference image changes only when a UI redesign is intentional. To
update:

1. Boot QEMU with a monitor: `bash scripts/machines/run-qemu.sh --monitor`
   (or `run-qemu.ps1 -Monitor` on Windows).
2. Wait for `DESKTOP_READY` on serial.
3. Capture: `bash scripts/qemu-screenshot.sh tests/references/<name>.png`
4. Sanity-check the new file visually.
5. Commit with a message naming the UI change that motivated the update.

## Tolerance

`scripts/compare-screenshot.sh` allows up to 5 percent pixel difference
by default (`--threshold 95`) with a 2 percent per-channel fuzz
(`--fuzz 2`). These absorb anti-aliasing wobble, sub-pixel timing
differences between captures, and cursor-blink phase without
ignoring a wallpaper or layout change. Tune per-scenario via CLI
flags if a specific reference needs a tighter or looser budget.

## Perceptual diff + structured needles

`compare-screenshot.sh` also supports SSIM / SSIMULACRA2 perceptual
modes and openQA-style JSON needles. Invocation examples:

```
bash scripts/compare-screenshot.sh --mode ssim reference.png current.png
bash scripts/compare-screenshot.sh --mode pixel --needle desktop-idle.needle.json reference.png current.png
```

When a sibling `<reference>.needle.json` exists next to the reference
PNG (for example `tests/references/desktop-idle-1280x720.png` paired
with `tests/references/desktop-idle-1280x720.needle.json`), it is
auto-discovered and applied even when `--needle` is not passed.
Explicit `--needle` always wins.

SSIMULACRA2 mode spawns an external subprocess per scored region, so
it only supports a single scored region per run; multi-region needles
must use `--mode ssim` or `--mode pixel`. An attempted multi-region
SSIMULACRA2 invocation exits 1 with a clear message pointing at the
alternative modes.

Non-pixel modes delegate to `scripts/needle-compare.py`, which uses
Python + scikit-image (for SSIM), Pillow (for cropping + diff image),
and optionally tesseract (for region OCR) and the external
`ssimulacra2` binary (for SSIMULACRA2 mode). The CI job installs
`python3-skimage`, `python3-pil`, `python3-numpy`, and `tesseract-ocr`.

### Needle sidecar schema (version 1)

A needle is a JSON file named `<reference>.needle.json` alongside the
PNG. Example:

```json
{
  "version": 1,
  "regions": [
    {
      "name": "taskbar",
      "x": 0, "y": 690, "w": 1280, "h": 30,
      "tolerance": "strict"
    },
    {
      "name": "client",
      "x": 0, "y": 30, "w": 1280, "h": 660,
      "tolerance": "relaxed"
    },
    {
      "name": "clock",
      "x": 1100, "y": 695, "w": 150, "h": 20,
      "tolerance": "ignored"
    },
    {
      "name": "cmd_prompt",
      "x": 64, "y": 120, "w": 80, "h": 24,
      "tolerance": "strict",
      "ocr": "C:\\>"
    }
  ]
}
```

Tolerance labels:

- `strict`   -- require pixel-identical (pixel mode) or SSIM >= 0.99.
- `relaxed`  -- use the caller's `--threshold` / `--fuzz` values.
- `ignored`  -- skip; used for clocks, cursor-blink zones, per-boot
  RNG-driven art. The region is drawn as a gray overlay in the diff
  image for visual confirmation.

`ocr` (optional per region): after the pixel/SSIM check, crop the
region and run tesseract; the OCR'd text must contain the quoted
string or the region fails. Absent tesseract: soft-skip via
`--allow-missing-ocr`, or hard-fail (exit 4) when omitted.

### Regenerating needle skeletons

`make update-ui-refs` re-captures reference PNGs. It does NOT
auto-generate needle JSON; the first time a reference lands, author a
skeleton by copying `desktop-idle.needle.json.example` (once it
ships with the first baseline) and tuning region rects for the
captured resolution. Needle JSON should be reviewed and committed
intentionally, like any other test fixture.
