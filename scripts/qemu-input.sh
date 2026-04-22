#!/usr/bin/env bash
# qemu-input.sh -- inject keyboard / mouse events into a running QEMU via HMP monitor
#
# Host-side wrapper for the desktop UI test framework input-injection
# section. Forwards HMP commands (`sendkey`, `mouse_move`, `mouse_button`)
# to the QEMU monitor exposed by run-qemu.ps1 -Monitor or a manual
# `-monitor telnet:127.0.0.1:<port>,server,nowait` flag.
#
# Usage:
#   bash scripts/qemu-input.sh sendkey <keyname> [hold-ms]
#   bash scripts/qemu-input.sh mouse_move <dx> <dy>
#   bash scripts/qemu-input.sh mouse_button <mask>
#   bash scripts/qemu-input.sh sendstring <literal>    # chains sendkey per char
#
# QEMU keyname reference: lower-case letters (a-z), digits (0-9),
# `ret` (Enter), `esc`, `spc`, `tab`, `backspace`, `up`/`down`/`left`/`right`,
# `ctrl-a`/`alt-f4`/etc. See QEMU docs for the full keymap table.
#
# Environment:
#   QEMU_MONITOR_HOST     default 127.0.0.1
#   QEMU_MONITOR_PORT     default 4444
#   QEMU_MONITOR_TIMEOUT  seconds, default 5
#
# Exit codes:
#   0  success
#   1  precondition failure (missing nc, invalid args)
#   2  monitor unreachable within timeout

set -u

HOST="${QEMU_MONITOR_HOST:-127.0.0.1}"
PORT="${QEMU_MONITOR_PORT:-4444}"
MONITOR_TIMEOUT="${QEMU_MONITOR_TIMEOUT:-5}"

RED=$'\033[0;31m'
CYAN=$'\033[0;36m'
RESET=$'\033[0m'

die() {
    local code=$1; shift
    printf '%s[qemu-input]%s %s\n' "$RED" "$RESET" "$*" >&2
    exit "$code"
}

info() {
    printf '%s[qemu-input]%s %s\n' "$CYAN" "$RESET" "$*"
}

usage() {
    sed -n '2,24p' "$0" >&2
    exit 1
}

# --- Preflight -----------------------------------------------------------

command -v nc >/dev/null 2>&1 || die 1 "required tool 'nc' not found (apt install netcat-openbsd)"

[ $# -ge 1 ] || usage
cmd="$1"; shift

# --- HMP safety ----------------------------------------------------------
# The HMP protocol is line-oriented: a newline embedded in a keyname or
# literal terminates the current command and the remainder is parsed as a
# new command. A malicious or typo'd input string could inject `stop`,
# `quit`, `system_powerdown`, etc. Reject control characters and shell
# metacharacters in every argument before they reach the monitor socket.
# Same rule applied by scripts/qemu-screenshot.sh for the PPM path.
check_hmp_safe() {
    local arg="$1"
    case "$arg" in
        *[$'\n\r\t\v\f']*)
            die 1 "argument contains control character (newline/CR/tab): refuse to embed in HMP"
            ;;
        *';'*|*'|'*|*'&'*|*'\`'*)
            die 1 "argument contains shell/HMP metacharacter (; | & backtick)"
            ;;
    esac
}

for a in "$@"; do check_hmp_safe "$a"; done

# --- Build the HMP command ----------------------------------------------

build_hmp() {
    case "$cmd" in
        sendkey)
            [ $# -ge 1 ] || die 1 "sendkey requires a keyname (e.g. 'ret', 'a', 'ctrl-c')"
            local key="$1"
            local hold="${2:-}"
            if [ -n "$hold" ]; then
                # hold must be a non-negative integer -- silently
                # downgrading 'abc' to a zero-hold send hides typos in
                # automation. Reject anything non-numeric so the caller
                # learns fast. Codex [M] review.
                if ! [[ "$hold" =~ ^[0-9]+$ ]]; then
                    die 1 "sendkey hold-ms must be a non-negative integer (got: '$hold')"
                fi
                if [ "$hold" -gt 0 ]; then
                    printf 'sendkey %s %s\n' "$key" "$hold"
                else
                    printf 'sendkey %s\n' "$key"
                fi
            else
                printf 'sendkey %s\n' "$key"
            fi
            ;;
        mouse_move)
            [ $# -eq 2 ] || die 1 "mouse_move requires <dx> <dy> (relative, integer)"
            printf 'mouse_move %s %s\n' "$1" "$2"
            ;;
        mouse_button)
            [ $# -eq 1 ] || die 1 "mouse_button requires <mask> (1=L, 2=R, 4=M, 0=release)"
            printf 'mouse_button %s\n' "$1"
            ;;
        sendstring)
            [ $# -eq 1 ] || die 1 "sendstring requires a literal ASCII string"
            local s="$1"
            # Chain one sendkey per character. QEMU's HMP does not take raw
            # strings; this macro expands "dir" into sendkey d / i / r.
            local i
            for ((i = 0; i < ${#s}; i++)); do
                local ch="${s:$i:1}"
                case "$ch" in
                    ' ') printf 'sendkey spc\n' ;;
                    [a-z0-9]) printf 'sendkey %s\n' "$ch" ;;
                    [A-Z]) printf 'sendkey shift-%s\n' "$(printf '%s' "$ch" | tr '[:upper:]' '[:lower:]')" ;;
                    *) die 1 "sendstring: character '$ch' not supported; use explicit sendkey calls" ;;
                esac
            done
            ;;
        *)
            die 1 "unknown command: $cmd (expected sendkey, mouse_move, mouse_button, sendstring)"
            ;;
    esac
}

# build_hmp runs in a $() subshell; a `die` call inside that subshell only
# exits the subshell -- the parent would continue with an empty CMD_STREAM
# and fall through to nc. Propagate the subshell's exit code explicitly so
# validation errors terminate the script, not just the subprocess.
CMD_STREAM=$(build_hmp "$@") || exit $?
[ -n "$CMD_STREAM" ] || die 1 "internal: build_hmp produced empty command stream"

info "sending to $HOST:$PORT:"
printf '  %s\n' "$CMD_STREAM" | sed -e 's/^/    /'

# --- Send to monitor ----------------------------------------------------

# Same nc invocation pattern as qemu-screenshot.sh: -q 0 exits on EOF
# immediately; do NOT send `quit` because that exits QEMU entirely rather
# than closing the monitor connection. `server,nowait` keeps the monitor
# listening for subsequent clients.
if ! printf '%s' "$CMD_STREAM" \
    | timeout "$MONITOR_TIMEOUT" nc -q 0 "$HOST" "$PORT" >/dev/null 2>&1; then
    die 2 "monitor at $HOST:$PORT did not respond within ${MONITOR_TIMEOUT}s (is QEMU running with -monitor telnet:$HOST:$PORT,server,nowait ?)"
fi

info "ok"
exit 0
