#!/usr/bin/env bash
# heavy.sh -- run one heavy hook job (a full build, a full suite, the tooling
# pack, the static analyzer) through the host's bosun queue.
#
#   bash scripts/hooks/heavy.sh <command> [args...]
#
# bosun gives each heavy job a slot shared by every repo on this machine, on
# Windows and in WSL, so two pushes do not build at once and exhaust memory,
# and it stops a job whose tree writes past its budget. When bosun is not
# installed (CI, a fresh clone) the command runs directly: the gate's verdict
# never depends on the queue. HEAVY_NO_BOSUN=1 runs it directly as well.
#
# The exit status is the command's, or bosun's own when it refuses to start
# the job (a queue wait past its limit), which the caller treats as a failed
# gate, never as a pass.
set -u

[ "$#" -gt 0 ] || { echo "usage: heavy.sh <command> [args...]" >&2; exit 64; }

if [ "${HEAVY_NO_BOSUN:-}" != "1" ] && command -v bosun >/dev/null 2>&1; then
  exec bosun run -- "$@"
fi
exec "$@"
