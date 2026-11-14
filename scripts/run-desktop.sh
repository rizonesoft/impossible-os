#!/usr/bin/env bash
# run-desktop.sh -- what the Windows desktop shortcuts run, in a Windows Terminal tab.
#
#   bash scripts/run-desktop.sh watch     attach to the run; offer to start it if it is down
#   bash scripts/run-desktop.sh start     start (or resume) the run, then attach
#   bash scripts/run-desktop.sh stop      show the run's state and stop it after a yes
#   bash scripts/run-desktop.sh install   create the three shortcuts on the Windows desktop
#
# The shortcuts attach with the keyboard on, so you can talk to the run from the tab.
# Detach from the run with Ctrl-b then d; the tab closes and the run keeps going.
# The run itself is scripts/run.sh; this file only adds the prompts a shortcut needs.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$ROOT"
# shellcheck source=scripts/overnight/run-common.sh
. "$ROOT/scripts/overnight/run-common.sh"
RUN="bash $ROOT/scripts/run.sh"

running() { tmux has-session -t "$(run_target "$RUN_SESSION")" 2>/dev/null; }
pause() { read -r -p "Press Enter to close." _ || true; }
yes_to() { local a; read -r -p "$1 [y/N] " a || return 1; [ "$a" = y ] || [ "$a" = Y ]; }

# Render the logo (resources/brand/logo.svg) to an .ico on the Windows side and print its Windows path.
# The icon lives on a Windows drive, not in WSL, so the shortcuts keep it while WSL is stopped.
# Prints nothing and returns 1 when it cannot be made; the caller then keeps the WSL icon.
install_icon() {
  local svg="$ROOT/resources/brand/logo.svg" appdata dir tmp size pngs=()
  command -v rsvg-convert >/dev/null 2>&1 && command -v magick >/dev/null 2>&1 || return 1
  [ -f "$svg" ] || return 1
  appdata="$(powershell.exe -NoProfile -Command '[Environment]::GetFolderPath("LocalApplicationData")' | tr -d '\r')"
  [ -n "$appdata" ] || return 1
  dir="$(wslpath -u "$appdata\\ImpossibleOS")" || return 1
  tmp="$(mktemp -d)" || return 1
  for size in 16 24 32 48 64 256; do
    rsvg-convert -w "$size" -h "$size" "$svg" -o "$tmp/$size.png" || { rm -rf "$tmp"; return 1; }
    pngs+=("$tmp/$size.png")
  done
  if ! { mkdir -p "$dir" && magick "${pngs[@]}" "$dir/impossible-os.ico"; }; then rm -rf "$tmp"; return 1; fi
  rm -rf "$tmp"
  printf '%s\n' "$appdata\\ImpossibleOS\\impossible-os.ico"
}

install_shortcuts() {
  command -v powershell.exe >/dev/null 2>&1 || { echo "powershell.exe not found: run this from WSL on Windows" >&2; return 1; }
  local wt desktop icon distro="${WSL_DISTRO_NAME:-Ubuntu}"
  # The per-user app execution alias, not the versioned package folder a command lookup resolves to:
  # the package folder is replaced by every Windows Terminal update, and the shortcut dies with it.
  wt="$(powershell.exe -NoProfile -Command '$a = Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps\wt.exe"; if (Test-Path $a) { $a }' | tr -d '\r')"
  desktop="$(powershell.exe -NoProfile -Command '[Environment]::GetFolderPath("Desktop")' | tr -d '\r')"
  [ -n "$wt" ] || { echo "Windows Terminal (wt.exe) not found" >&2; return 1; }
  [ -n "$desktop" ] || { echo "could not find the Windows desktop folder" >&2; return 1; }
  if ! icon="$(install_icon)"; then
    icon='%SystemRoot%\System32\wsl.exe,0'
    echo "the Impossible OS icon could not be made (needs rsvg-convert and magick): using the WSL icon" >&2
  fi
  local verb name tip
  for verb in watch start stop; do
    case "$verb" in
      watch) name="Impossible OS - Watch run"; tip="Attach to the Impossible OS run (detach: Ctrl-b d)" ;;
      start) name="Impossible OS - Start run"; tip="Start or resume the Impossible OS run, then attach" ;;
      stop)  name="Impossible OS - Stop run";  tip="Stop the Impossible OS run (asks first)" ;;
    esac
    # Values reach PowerShell as environment variables, never spliced into its source.
    SC_PATH="$desktop\\$name.lnk" SC_TARGET="$wt" SC_TIP="$tip" SC_ICON="$icon" \
    SC_ARGS="--title \"$name\" wsl.exe -d $distro --cd $ROOT -- bash -l scripts/run-desktop.sh $verb" \
    WSLENV="SC_PATH/w:SC_TARGET/w:SC_ARGS/w:SC_TIP/w:SC_ICON/w" \
      powershell.exe -NoProfile -Command '
        $s = (New-Object -ComObject WScript.Shell).CreateShortcut($env:SC_PATH)
        $s.TargetPath = $env:SC_TARGET
        $s.Arguments = $env:SC_ARGS
        $s.Description = $env:SC_TIP
        $s.IconLocation = $env:SC_ICON
        $s.Save()' || { echo "could not create: $name" >&2; return 1; }
    echo "created: $desktop\\$name.lnk"
  done
}

case "${1:-}" in
  watch)
    if running; then exec $RUN attach; fi
    $RUN status
    if yes_to "The run is not running. Start it?"; then $RUN start && exec $RUN attach; fi
    pause
    ;;
  start)
    $RUN start || { pause; exit 1; }
    exec $RUN attach
    ;;
  stop)
    $RUN status
    if ! running; then echo "The run is not running."; pause; exit 0; fi
    if yes_to "Stop the run? It resumes from disk with the Start shortcut."; then
      $RUN stop
    else
      echo "Left running."
    fi
    pause
    ;;
  install)
    install_shortcuts
    ;;
  *)
    echo "usage: run-desktop.sh watch|start|stop|install" >&2
    exit 2
    ;;
esac
