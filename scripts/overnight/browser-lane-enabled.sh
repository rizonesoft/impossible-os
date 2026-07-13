#!/usr/bin/env bash
# browser-lane-enabled.sh -- decide whether overnight-launch.sh should acquire a
# ChromeMCP browser lane. FAIL-SAFE OFF (I5): the lane is acquired ONLY on an
# EXPLICIT positive browser signal, so a kernel run that fails to propagate the
# old OVERNIGHT_NO_CHROMEMCP env drop-in through the systemd/watchdog chain still
# correctly skips the lane. That propagation gap was the I5 bug -- all 8 kernel
# runs claimed a lane nobody used, producing CDP noise in the report.
#
#   exit 0 -> acquire the lane (browser-owned work, e.g. gh-pages)
#   exit 1 -> skip the lane (default; every kernel run)
#
# Positive signals (either enables): OVERNIGHT_WITH_BROWSER=1 in the env, or the
# sentinel file .claude/state/overnight-with-browser (written by
# arm-sequencer.sh --with-browser). OVERNIGHT_NO_CHROMEMCP=1 is a hard override
# that always skips, kept for backward compatibility.
set -u

SENTINEL="${OVERNIGHT_WITH_BROWSER_SENTINEL:-.claude/state/overnight-with-browser}"

# Hard override wins: never acquire when explicitly disabled.
[ "${OVERNIGHT_NO_CHROMEMCP:-}" = "1" ] && exit 1

# Explicit positive signals.
[ "${OVERNIGHT_WITH_BROWSER:-}" = "1" ] && exit 0
[ -f "$SENTINEL" ] && exit 0

# Default: no browser lane.
exit 1
