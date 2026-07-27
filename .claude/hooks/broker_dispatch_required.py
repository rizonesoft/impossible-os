#!/usr/bin/env python3
# block-via: exit 2 (unattended run only; interactive sessions untouched)
"""Force review-kind Codex dispatches through the BROKER in the unattended run.

Why
---
Measured over the 2026-07-25 -> 2026-07-26 run (12.95h, 6 rollover segments):
54 Codex dispatches, 37 via the mandated broker and **17 via the raw
scripts/codex-dispatch.sh**. The split was worst exactly where it costs most --
**12 of 19 re-adversarial rounds went direct**, and re-adversarial is the round
that decides whether a finding is resolved.

The direct path is a known-lossy output shape: the 2026-07-15 live-gotcha
measured it truncating a review to a 544-byte capture starting mid-finding, and
only the broker persists a sha256 `.out` artifact under
`.claude/overnight/reviews/`. So roughly a third of that run's review evidence
was received from a shape known to truncate, with nothing to re-read.

It is a COST item, not only a quality one: a truncated verdict either forces a
re-dispatch (a full Codex round -- minutes plus tokens) or silently drops
findings that resurface a round later as new work. Both are strictly more
expensive than dispatching correctly the first time.

Scope
-----
UNATTENDED RUN ONLY (`OVERNIGHT_SEQUENCER_RUN=1`). The direct script stays fully
available for interactive ad-hoc use, where an operator watches the output and a
truncated tail is an inconvenience rather than silent evidence loss. This mirrors
`overnight_plugin_skill_block`, which redirects arming the same way.

Only REVIEW-KIND dispatches are gated. A dispatch with no `[review-kind: ...]`
marker is not review evidence, has no broker artifact to lose, and is left alone.

Recognition uses the shared `_codex_dispatch` argv machinery, not raw regex --
raw scanning was historically bypassed by `bash -lc`, `cd X;`, and env prefixes.
The broker was cross-checked as recognized by BOTH `_review_kind` and the
receipt binder BEFORE this gate was enabled, so enforcing it cannot darken the
receipt path at the same moment.
"""
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    import _codex_dispatch as _cd
    from _review_kind import detect_review_kind_from_cmd
except Exception:  # noqa: BLE001 -- a helper import failure must never gate work
    _cd = None
    detect_review_kind_from_cmd = None

# The one wrapper that is NOT the broker. Basename equality, not a suffix test:
# the broker is `review-broker-codex-dispatch.sh`, which ENDS IN the same string,
# and a suffix check would classify the broker as direct and block it.
_DIRECT_BASENAME = "codex-dispatch.sh"
_BROKER = "scripts/overnight/review-broker-codex-dispatch.sh"


def _uses_direct_script(cmd: str) -> bool:
    """True iff some segment invokes the raw codex-dispatch.sh by basename."""
    if _cd is None:
        return False
    try:
        tokens = _cd._tokenize(cmd)
        for seg in _cd._segment_by_separators(tokens):
            for tok in seg:
                # Compare the BASENAME so an absolute path, a $REPO_ROOT-relative
                # path and a bare relative path all classify identically.
                if os.path.basename(tok) == _DIRECT_BASENAME:
                    return True
    except Exception:  # noqa: BLE001
        return False
    return False


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0  # malformed -- fail open
    if not isinstance(d, dict) or d.get("tool_name") != "Bash":
        return 0
    # Unattended run only. Absent/!=1 means an interactive operator session,
    # which keeps the direct script.
    if os.environ.get("OVERNIGHT_SEQUENCER_RUN") != "1":
        return 0
    if os.environ.get("BROKER_DISPATCH_OVERRIDE") == "1":
        return 0
    if detect_review_kind_from_cmd is None:
        return 0  # helpers unavailable -- never gate on a broken import

    cmd = str((d.get("tool_input") or {}).get("command") or "")
    if not cmd.strip():
        return 0

    kind = detect_review_kind_from_cmd(cmd)
    if not kind:
        return 0  # not a review-kind dispatch -- nothing to lose, nothing to gate
    if not _uses_direct_script(cmd):
        return 0  # broker (or another wrapper) -- allowed

    sys.stderr.write(
        "[broker-dispatch-required] BLOCK -- review-kind '%s' dispatched via the "
        "raw scripts/codex-dispatch.sh in the unattended run.\n" % kind
    )
    sys.stderr.write(
        "[broker-dispatch-required] why: the direct path is a known-lossy output "
        "shape (measured truncating a review to a 544-byte capture starting "
        "mid-finding, 2026-07-15) and persists NO artifact. Only the broker "
        "writes a sha256 .out under .claude/overnight/reviews/ that you can "
        "re-read instead of re-dispatching.\n"
    )
    sys.stderr.write(
        "[broker-dispatch-required] cost: a truncated verdict forces a full "
        "re-dispatch (minutes + tokens) or silently drops findings that resurface "
        "a round later as new work. 12 of 19 re-adversarial rounds went direct on "
        "the 2026-07-25 run.\n"
    )
    sys.stderr.write(
        "[broker-dispatch-required] use instead:\n"
        "    bash %s '[review-kind: %s] <todo-path> <body>'\n" % (_BROKER, kind)
    )
    sys.stderr.write(
        "[broker-dispatch-required] scope: unattended run only "
        "(OVERNIGHT_SEQUENCER_RUN=1); interactive sessions keep the direct "
        "script. Opt-out: BROKER_DISPATCH_OVERRIDE=1 on the same call.\n"
    )
    return 2


if __name__ == "__main__":
    sys.exit(main())
