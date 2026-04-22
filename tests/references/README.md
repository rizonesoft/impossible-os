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
