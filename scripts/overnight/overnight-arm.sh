#!/usr/bin/env bash
# Arm/disarm the impossible-os overnight sequencer's systemd timers.
#
# Vendored 2026-06-16 from the rizonetech overnight-runner plugin
# (scripts/overnight-arm.sh, v0.2.4) and de-coupled: the repo owns this so the
# armed systemd unit points at the repo's own launcher
# (scripts/overnight/overnight-launch.sh) by absolute path -- no
# version-resolving shim into the user plugin cache, no plugin-CLI dependency.
# It is normally invoked through .claude/skills/overnight-sequencer/
# arm-sequencer.sh (which adds the guard marker + per-unit env drop-ins); it can
# also be run directly.
#
# Usage (run from the project root):
#   overnight-arm.sh <todo.md> [--mode bypassPermissions|acceptEdits]
#                    [--at "<systemd calendar>"] [--watchdog "<cadence>"]
#                    [--no-watchdog] [--disarm]
#
# Defaults: --mode bypassPermissions, --at = 2 minutes from now,
# --watchdog = "*:0/30". The launcher's flock + usage-limit snooze make watchdog
# ticks free no-ops while a run is alive or a limit is active.
set -euo pipefail

TODO_FILE="${1:?usage: overnight-arm.sh <todo.md> [--mode M] [--at T] [--watchdog C] [--no-watchdog] [--disarm]}"
shift

MODE="bypassPermissions"
AT=""
WATCHDOG_CAL="*:0/30"
ARM_WATCHDOG=1
DISARM=0
while [ $# -gt 0 ]; do
  case "$1" in
    --mode)        MODE="${2:?--mode needs a value}"; shift 2 ;;
    --at)          AT="${2:?--at needs a systemd calendar value}"; shift 2 ;;
    --watchdog)    WATCHDOG_CAL="${2:?--watchdog needs a cadence}"; shift 2 ;;
    --no-watchdog) ARM_WATCHDOG=0; shift ;;
    --disarm)      DISARM=1; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER="$SCRIPT_DIR/overnight-launch.sh"

if ! systemctl --user is-system-running >/dev/null 2>&1 &&
   ! systemctl --user is-system-running 2>/dev/null | grep -qE "running|degraded"; then
  echo "FATAL: user systemd is not available (systemctl --user errors). Timers cannot be armed." >&2
  exit 1
fi

PROJECT_DIR="$PWD"
PROJECT_NAME="$(basename "$PROJECT_DIR")"
UNIT="overnight-${PROJECT_NAME}"

if [ "$DISARM" = "1" ]; then
  systemctl --user stop "${UNIT}.timer" "${UNIT}.service" \
    "${UNIT}-watchdog.timer" "${UNIT}-watchdog.service" 2>/dev/null || true
  systemctl --user reset-failed 2>/dev/null || true
  echo "disarmed ${UNIT} timers (main + watchdog)"
  exit 0
fi

[ -f "$PROJECT_DIR/$TODO_FILE" ] || { echo "FATAL: $TODO_FILE not found under $PROJECT_DIR" >&2; exit 1; }
[ -x "$LAUNCHER" ] || { echo "FATAL: launcher not found/executable: $LAUNCHER" >&2; exit 1; }

if [ "$(loginctl show-user "$USER" --property=Linger --value 2>/dev/null)" != "yes" ]; then
  if [ -t 0 ]; then
    read -r -p "Linger is off (timers die on logout). Enable it now? [y/N] " reply
    case "$reply" in y|Y) loginctl enable-linger "$USER" && echo "linger enabled for $USER" ;; esac
  else
    echo "WARN: linger is off -- armed timers stop when you log out. Fix: loginctl enable-linger $USER" >&2
  fi
fi

# Replace any prior units for this project (idempotent re-arm).
systemctl --user stop "${UNIT}.timer" "${UNIT}.service" \
  "${UNIT}-watchdog.timer" "${UNIT}-watchdog.service" 2>/dev/null || true
systemctl --user reset-failed 2>/dev/null || true

[ -n "$AT" ] || AT="$(date -d '+2 minutes' '+%Y-%m-%d %H:%M:00')"

# ExecStart points straight at the repo launcher (absolute path). Re-arm
# regenerates the transient unit, so a moved repo just needs a re-arm.
systemd-run --user --on-calendar="$AT" --unit="$UNIT" \
  "$LAUNCHER" "$PROJECT_DIR" "$TODO_FILE" "$MODE"
echo "armed ${UNIT}: starts at ${AT} (mode: ${MODE})"

if [ "$ARM_WATCHDOG" = "1" ]; then
  systemd-run --user --on-calendar="$WATCHDOG_CAL" --unit="${UNIT}-watchdog" \
    "$LAUNCHER" "$PROJECT_DIR" "$TODO_FILE" "$MODE"
  echo "armed ${UNIT}-watchdog: cadence ${WATCHDOG_CAL} (same mode -- a watchdog in a weaker mode stalls on relaunch)"
fi

REPORT_BASE="$PROJECT_DIR/.claude/overnight/reports"
echo "report logs: $REPORT_BASE/  |  disarm: overnight-arm.sh $TODO_FILE --disarm"
systemctl --user list-timers 2>/dev/null | grep -F "$UNIT" || true
