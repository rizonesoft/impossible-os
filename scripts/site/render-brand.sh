#!/usr/bin/env bash
# Re-render the README brand images from their sources:
#   resources/brand/wordmark-{dark,light}-bg.png  <- resources/brand/src/wordmark-*.html
#   resources/brand/desktop-preview.png            <- gh-pages/design/ (the live desktop mockup)
# Needs a Chromium (CHROME=path, or the newest Playwright download) and ImageMagick.
# Run after changing the logo, the wordmark, the design tokens or the mockup.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHROME="${CHROME:-$(ls -d "$HOME"/.cache/ms-playwright/chromium-*/chrome-linux*/chrome 2>/dev/null | sort -V | tail -1)}"
[ -x "$CHROME" ] || { echo "render-brand: no Chromium found; set CHROME=/path/to/chrome" >&2; exit 2; }
command -v convert >/dev/null || { echo "render-brand: ImageMagick 'convert' is required" >&2; exit 2; }

python3 "$ROOT/scripts/site/build.py" --quiet
PORT="${PORT:-8791}"
TMP="$(mktemp -d)"
python3 -m http.server "$PORT" --directory "$ROOT" >/dev/null 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null; rm -rf "$TMP"' EXIT
for _ in $(seq 50); do curl -s "localhost:$PORT/" >/dev/null && break; sleep 0.1; done

shot() { "$CHROME" --headless=new --no-sandbox --hide-scrollbars --virtual-time-budget=6000 "$@" 2>/dev/null; }

for v in dark light; do
  shot --default-background-color=00000000 --force-device-scale-factor=2 --window-size=900,150 \
       --screenshot="$TMP/wm-$v.png" "http://localhost:$PORT/resources/brand/src/wordmark-$v-bg.html"
  convert "$TMP/wm-$v.png" -trim +repage "$ROOT/resources/brand/wordmark-$v-bg.png"
done
shot --window-size=1600,960 --screenshot="$TMP/desktop.png" "http://localhost:$PORT/build/site/design/?shot#start"
convert "$TMP/desktop.png" -strip "$ROOT/resources/brand/desktop-preview.png"
echo "render-brand: wrote resources/brand/wordmark-{dark,light}-bg.png and desktop-preview.png"
