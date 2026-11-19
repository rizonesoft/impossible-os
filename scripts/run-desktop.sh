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
cd "$ROOT" || exit 1
# shellcheck source=scripts/overnight/run-common.sh
. "$ROOT/scripts/overnight/run-common.sh"
RUN="bash $ROOT/scripts/run.sh"

running() { tmux has-session -t "$(run_target "$RUN_SESSION")" 2>/dev/null; }
pause() { read -r -p "Press Enter to close." _ || true; }
yes_to() { local a; read -r -p "$1 [y/N] " a || return 1; [ "$a" = y ] || [ "$a" = Y ]; }

# The badge each shortcut carries on the logo (scripts/campaign-icon.py): Start runs Claude, so it carries
# Claude's spark, as the other projects' campaign shortcuts do; Watch an eye; Stop a stop square.
badge_for() {
  case "$1" in
    start) echo claude ;;
    watch) echo watch ;;
    stop)  echo stop ;;
    *) return 1 ;;
  esac
}

# Render the logo (resources/brand/logo.svg) to impossible-os.ico in directory $1, then one badged
# impossible-os-<verb>.ico per shortcut. Returns 1 when any of them cannot be made.
make_icons() {
  local dir="$1" svg="$ROOT/resources/brand/logo.svg" icon="$ROOT/scripts/campaign-icon.py" tmp verb
  command -v rsvg-convert >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && [ -f "$svg" ] || return 1
  tmp="$(mktemp -d)" || return 1
  # One large render, area-averaged down to every frame size; the badge step reads these PNG frames.
  if ! { rsvg-convert -w 512 -h 512 "$svg" -o "$tmp/logo.png" && mkdir -p "$dir" \
         && python3 "$icon" frames "$tmp/logo.png" "$tmp/logo.png" "$dir/impossible-os.ico"; }; then
    rm -rf "$tmp"; return 1
  fi
  rm -rf "$tmp"
  for verb in start watch stop; do
    python3 "$icon" badge "$dir/impossible-os.ico" "$(badge_for "$verb")" \
      "$dir/impossible-os-$verb.ico" || return 1
  done
}

# Make the icons on the Windows side and print the Windows folder that holds them.
# They live on a Windows drive, not in WSL, so the shortcuts keep them while WSL is stopped.
# Prints nothing and returns 1 when they cannot be made; the caller then keeps the WSL icon.
install_icons() {
  local appdata dir
  appdata="$(powershell.exe -NoProfile -Command '[Environment]::GetFolderPath("LocalApplicationData")' | tr -d '\r')"
  [ -n "$appdata" ] || return 1
  dir="$(wslpath -u "$appdata\\ImpossibleOS")" || return 1
  make_icons "$dir" || return 1
  printf '%s\n' "$appdata\\ImpossibleOS"
}

install_shortcuts() {
  command -v powershell.exe >/dev/null 2>&1 || { echo "powershell.exe not found: run this from WSL on Windows" >&2; return 1; }
  local wt desktop icons icon distro="${WSL_DISTRO_NAME:-Ubuntu}"
  # The per-user app execution alias, not the versioned package folder a command lookup resolves to:
  # the package folder is replaced by every Windows Terminal update, and the shortcut dies with it.
  # shellcheck disable=SC2016  # PowerShell source: its $ names are PowerShell's, not the shell's
  wt="$(powershell.exe -NoProfile -Command '$a = Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps\wt.exe"; if (Test-Path $a) { $a }' | tr -d '\r')"
  desktop="$(powershell.exe -NoProfile -Command '[Environment]::GetFolderPath("Desktop")' | tr -d '\r')"
  [ -n "$wt" ] || { echo "Windows Terminal (wt.exe) not found" >&2; return 1; }
  [ -n "$desktop" ] || { echo "could not find the Windows desktop folder" >&2; return 1; }
  if ! icons="$(install_icons)"; then
    icons=""
    echo "the Impossible OS icons could not be made (needs rsvg-convert and python3): using the WSL icon" >&2
  fi
  local verb name tip
  for verb in watch start stop; do
    case "$verb" in
      watch) name="Impossible OS - Watch run"; tip="Attach to the Impossible OS run (detach: Ctrl-b d)" ;;
      start) name="Impossible OS - Start run"; tip="Start or resume the Impossible OS run, then attach" ;;
      stop)  name="Impossible OS - Stop run";  tip="Stop the Impossible OS run (asks first)" ;;
    esac
    if [ -n "$icons" ]; then icon="$icons\\impossible-os-$verb.ico,0"; else icon='%SystemRoot%\System32\wsl.exe,0'; fi
    # Values reach PowerShell as environment variables, never spliced into its source.
    # shellcheck disable=SC2016  # PowerShell source: its $env: names are PowerShell's, not the shell's
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

# Sourced (by its test), it only defines the functions above.
[ "${BASH_SOURCE[0]}" = "$0" ] || return 0

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
