#!/usr/bin/env bash
# qemu-screenshot.sh -- capture a PNG of the live QEMU framebuffer via monitor protocol
#
# TODO-05-desktop-ui-test-framework.md §2 QEMU Framebuffer Dump.
#
# Connects to a QEMU HMP monitor (telnet, default 127.0.0.1:4444), issues a
# `screendump <ppm>` command, waits for QEMU to flush the PPM file, converts
# PPM -> PNG via ImageMagick, and validates the output. Host-side script; the
# target QEMU must have been launched with `-monitor telnet:127.0.0.1:4444,server,nowait`
# (added by run-qemu.ps1 -Monitor).
#
# Usage:
#   bash scripts/qemu-screenshot.sh [output.png]
#   bash scripts/qemu-screenshot.sh --socket /tmp/qemu-mon.sock [output.png]
#
# Environment:
#   QEMU_MONITOR_HOST     default 127.0.0.1 (TCP mode)
#   QEMU_MONITOR_PORT     default 4444      (TCP mode)
#   QEMU_MONITOR_SOCKET   UNIX-socket path; when set, overrides TCP mode
#                         and `nc -U <path>` is used instead of TCP. The
#                         `--socket <path>` flag sets the same variable.
#                         Linux shared-host CI should prefer this over
#                         TCP to avoid cross-user monitor access.
#   QEMU_MONITOR_TIMEOUT  seconds, default 5
#   QEMU_SCREENSHOT_MIN   minimum PNG size in bytes, default 100*1024 (100 KiB)
#
# Exit codes:
#   0  success; prints `OK: <path> (<w>x<h>, <bytes> bytes)`
#   1  precondition failure: tooling missing (ImageMagick `convert`,
#      `identify`, `nc`), invalid output path (HMP-unsafe chars), or
#      cannot create the output directory
#   2  monitor unreachable within QEMU_MONITOR_TIMEOUT seconds (no TCP
#      accept); note that `screendump` HMP command errors surface as
#      code 3 because nc output is not parsed -- the kernel-written PPM
#      either lands on disk or it does not
#   3  PPM not produced or empty after the size-stability poll
#   4  PNG conversion failed (ImageMagick `convert` error, zero-byte output)
#   5  PNG validation failed (too small vs liveness floor, bad magic,
#      dimensions unreadable)

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# --socket <path> parses BEFORE the positional PNG arg so both orderings work:
#   qemu-screenshot.sh --socket /tmp/x.sock out.png
#   qemu-screenshot.sh out.png --socket /tmp/x.sock
SOCKET_PATH="${QEMU_MONITOR_SOCKET:-}"
POSITIONAL=()
while [ $# -gt 0 ]; do
    case "$1" in
        --socket) SOCKET_PATH="$2"; shift 2 ;;
        --socket=*) SOCKET_PATH="${1#--socket=}"; shift ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) POSITIONAL+=("$1"); shift ;;
    esac
done

OUTPUT_PNG="${POSITIONAL[0]:-$PROJECT_ROOT/build/screenshot.png}"
PPM_PATH="${OUTPUT_PNG%.png}.ppm"

HOST="${QEMU_MONITOR_HOST:-127.0.0.1}"
PORT="${QEMU_MONITOR_PORT:-4444}"
MONITOR_TIMEOUT="${QEMU_MONITOR_TIMEOUT:-5}"
MIN_BYTES="${QEMU_SCREENSHOT_MIN:-$((100 * 1024))}"

RED=$'\033[0;31m'
GREEN=$'\033[0;32m'
YELLOW=$'\033[1;33m'
CYAN=$'\033[0;36m'
RESET=$'\033[0m'

die() {
    local code=$1; shift
    printf '%s[qemu-screenshot]%s %s\n' "$RED" "$RESET" "$*" >&2
    exit "$code"
}

info() {
    printf '%s[qemu-screenshot]%s %s\n' "$CYAN" "$RESET" "$*"
}

ok() {
    printf '%s[qemu-screenshot]%s %s\n' "$GREEN" "$RESET" "$*"
}

# --- Step 1: preflight ---------------------------------------------------

for tool in nc convert identify; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        die 1 "required tool '$tool' not found in PATH (install: ImageMagick for convert/identify, netcat for nc)"
    fi
done

# HMP protocol safety: OUTPUT_PNG and the derived PPM path flow verbatim
# into the monitor command stream via `printf 'screendump %s\n...' "$PPM_PATH"`.
# Shell metacharacters are quoted correctly, but HMP is line-oriented: a
# newline in the path would terminate the `screendump` command early and
# inject whatever follows as a new HMP command (e.g., `stop`, `cont`,
# `system_powerdown`). Semicolons, shell-special bytes, and ASCII control
# characters also have undefined behavior inside `screendump`'s path arg.
# Reject them up-front. Codex [H] adversarial review of the desktop-UI
# test-framework screendump section.
case "$OUTPUT_PNG" in
    *[$'\n\r\t\v\f']*)
        die 1 "output path contains a control character (newline/CR/tab); HMP would interpret it as a command boundary"
        ;;
    *';'*|*'|'*|*'&'*|*'\`'*)
        die 1 "output path contains shell/HMP metacharacter (; | & backtick); refuse to embed in monitor command"
        ;;
esac

mkdir -p "$(dirname "$OUTPUT_PNG")" || die 1 "cannot create output directory: $(dirname "$OUTPUT_PNG")"

# Remove stale artifacts so "does output exist?" is an honest check post-run.
rm -f "$PPM_PATH" "$OUTPUT_PNG"

# --- Step 2: screendump via HMP monitor ----------------------------------

# Socket transport selection:
#   - QEMU_MONITOR_SOCKET / --socket set -> UNIX domain socket via `nc -U`.
#     Required mode on shared-host Linux / WSL CI, where a TCP listener
#     on 127.0.0.1:<port> is reachable by any other local user and can
#     silently issue `quit` / `stop` / `system_powerdown` to our VM.
#   - Unset -> fall back to TCP (local dev default; Windows hosts keep
#     TCP because WSL's AF_UNIX interop across the WSL/Windows boundary
#     is flaky).
if [ -n "$SOCKET_PATH" ]; then
    if [ ! -S "$SOCKET_PATH" ]; then
        die 2 "monitor socket not found or not a socket: $SOCKET_PATH (is QEMU running with -monitor unix:$SOCKET_PATH,server,nowait ?)"
    fi
    info "dumping framebuffer via unix:$SOCKET_PATH -> $PPM_PATH"
    MONITOR_DESC="unix:$SOCKET_PATH"
    NC_CMD=(nc -q 0 -U "$SOCKET_PATH")
else
    info "dumping framebuffer via $HOST:$PORT -> $PPM_PATH"
    MONITOR_DESC="$HOST:$PORT"
    NC_CMD=(nc -q 0 "$HOST" "$PORT")
fi

# HMP echoes "(qemu) " at the prompt and processes commands line-by-line.
# Do NOT send `quit` here: the HMP `quit` command exits the entire VM
# (not "disconnect this monitor"), which breaks any caller that wants to
# take multiple screenshots in one QEMU session. Closing the stream from
# our side is enough -- `server,nowait` keeps the monitor listening for
# the next client. `nc -q 0` exits immediately after EOF so we are not
# paying the default 1-second drain. Post-disconnect flush latency for
# the PPM file is caught by the deterministic size-stability poll below,
# not by blocking here. Codex findings from earlier review; the
# multi-capture retry loop in test-desktop.sh revealed that `quit` was
# killing QEMU.
if ! printf 'screendump %s\n' "$PPM_PATH" \
    | timeout "$MONITOR_TIMEOUT" "${NC_CMD[@]}" >/dev/null 2>&1; then
    die 2 "monitor at $MONITOR_DESC did not respond within ${MONITOR_TIMEOUT}s"
fi

# Deterministic flush detection: poll file size until it stabilizes across
# three consecutive 50 ms samples, or until the 5 s timeout expires. The
# HMP ack returns before QEMU finishes writing the file on busy hosts;
# a fixed sleep was previously used but that is timing-dependent
# (Codex [M] adversarial review of the desktop-UI screendump section).
{
    last=-1
    stable=0
    deadline=$((SECONDS + 5))
    while [ $SECONDS -lt $deadline ]; do
        if [ -f "$PPM_PATH" ]; then
            cur=$(stat -c%s "$PPM_PATH" 2>/dev/null || echo 0)
            if [ "$cur" -eq "$last" ] && [ "$cur" -gt 0 ]; then
                stable=$((stable + 1))
                [ "$stable" -ge 3 ] && break
            else
                stable=0
            fi
            last=$cur
        fi
        sleep 0.05
    done
}

# --- Step 3: validate PPM ------------------------------------------------

if [ ! -s "$PPM_PATH" ]; then
    die 3 "PPM file not produced or empty: $PPM_PATH (QEMU accepted the command but did not write)"
fi

PPM_BYTES=$(stat -c%s "$PPM_PATH" 2>/dev/null || wc -c <"$PPM_PATH")
info "PPM captured: $PPM_BYTES bytes"

# --- Step 4: PPM -> PNG --------------------------------------------------

if ! convert "$PPM_PATH" "$OUTPUT_PNG" 2>/dev/null; then
    die 4 "ImageMagick convert failed: $PPM_PATH -> $OUTPUT_PNG"
fi

if [ ! -s "$OUTPUT_PNG" ]; then
    die 4 "PNG not produced: $OUTPUT_PNG"
fi

# --- Step 5: validate PNG ------------------------------------------------

PNG_BYTES=$(stat -c%s "$OUTPUT_PNG" 2>/dev/null || wc -c <"$OUTPUT_PNG")
# The size floor is a CONTENT-liveness heuristic, not a format-validity gate.
# PNG compresses strongly for low-entropy frames (all-black, solid splash,
# pre-paint desktop), so a small file often means the framebuffer had no
# meaningful content yet. Default 100 KiB matches the TODO §2 test checkpoint
# wording ("file size > 100 KiB") and empirically separates a blank capture
# (~1.2 KiB) from a real desktop (>500 KiB). Callers who want a pure
# format-validity check (e.g., early boot splash comparisons) can override
# with `QEMU_SCREENSHOT_MIN=0`. Codex [M] adversarial review of the
# desktop-UI screendump size-floor heuristic.
if [ "$PNG_BYTES" -lt "$MIN_BYTES" ]; then
    die 5 "PNG size $PNG_BYTES < liveness floor $MIN_BYTES bytes (framebuffer likely uniform/all-black; set QEMU_SCREENSHOT_MIN=0 if validating an intentionally low-entropy frame, or wait for DESKTOP_READY before capturing)"
fi

# First 8 bytes MUST be the PNG magic number (0x89 P N G \r \n 0x1A \n). Any
# deviation means convert emitted something other than PNG (quota exhaustion,
# truncated write, library mismatch).
PNG_MAGIC=$(head -c 8 "$OUTPUT_PNG" | od -An -tx1 | tr -d ' \n')
if [ "$PNG_MAGIC" != "89504e470d0a1a0a" ]; then
    die 5 "PNG magic mismatch: got $PNG_MAGIC, expected 89504e470d0a1a0a"
fi

# identify parses the IHDR chunk; gives width and height in one shot.
DIMS=$(identify -format '%w %h' "$OUTPUT_PNG" 2>/dev/null || echo "0 0")
W="${DIMS% *}"
H="${DIMS#* }"
if [ "$W" -le 0 ] || [ "$H" -le 0 ]; then
    die 5 "PNG dimensions not decodable (identify returned '$DIMS')"
fi

# Clean up the intermediate PPM now that the PNG is validated. Caller gets
# only the PNG; the raw PPM was a processing artifact.
rm -f "$PPM_PATH"

ok "$OUTPUT_PNG (${W}x${H}, $PNG_BYTES bytes)"
exit 0
