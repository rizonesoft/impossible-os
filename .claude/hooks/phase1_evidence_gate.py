#!/usr/bin/env python3
# block-via: exit-2 (PreToolUse gate -- BLOCKs the adversarial Codex
# dispatch when Phase 1 of review-todo-section was skipped).
"""PreToolUse Phase-1 evidence gate (TODO-08 §17).

Fires on Bash invocations of `codex-companion.mjs adversarial-review`
whose prompt's first non-blank line carries `[review-kind: adversarial]`
(NOT design / consistency / perf / re-adversarial). When an active
`review-todo-section` entry exists in `.claude/state/skill-progress.json`,
walk the transcript JSONL forward from that entry's `started_ts` and
count `Read` / `Grep` tool_use events whose location-input matches
`src/` or `include/`. BLOCK exit 2 if count < 2.

Closes the recurring failure mode named in `feedback_skill_invocation_drift`
and re-confirmed in the 2026-04-28 Boot UX Polish review session: the
agent jumped straight from invoking review-todo-section to dispatching
the adversarial Codex without doing Phase 1 (evidence map + scope-gap
audit + test-checkpoint verify). Phase 1 then surfaced findings the
Codex dispatch missed.

Per Codex design review 2026-04-28 H1: tool-history.jsonl carries only
tool_name + ts_ns and CANNOT prove path-scoped Read/Grep. Walking the
transcript JSONL is the only viable evidence source. The regex matches
ONLY the location-bearing input fields (`file_path` / `path` / `glob`),
NOT `pattern` -- a Grep call searching repo docs for the literal string
"src/kernel/foo" must NOT count as evidence of source exploration.

Opt-out:
    SKIP_PHASE1_BLOCK=1 SKIP_PHASE1_BLOCK_REASON="<text >= 12 chars>"

Both required; reason logged to .claude/state/skip-log.jsonl.
"""

import json
import os
# TODO-08 section-23: shared SKIP-env scanner.
from pathlib import Path as _SkipPath
_SKIP_HOOK_DIR = _SkipPath(__file__).resolve().parent
import sys as _SkipSys
if str(_SKIP_HOOK_DIR) not in _SkipSys.path:
    _SkipSys.path.insert(0, str(_SKIP_HOOK_DIR))
import _skip_env as _se  # noqa: E402
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

_HOOK_DIR = Path(__file__).resolve().parent
if str(_HOOK_DIR) not in sys.path:
    sys.path.insert(0, str(_HOOK_DIR))

# pylint: disable=wrong-import-position
from skill_step_map import MULTI_STEP_SKILLS  # noqa: E402
import _heuristic_misses as hm  # noqa: E402

_STATE_REL = ".claude/state/skill-progress.json"
_SKIP_LOG_REL = ".claude/state/skip-log.jsonl"
_SKIP_REASON_MIN_LEN = 12

# Threshold for required Read/Grep evidence on src/include/.claude/scripts/docs
# before allowing a step-5 adversarial Codex dispatch. Originally chosen at
# section-17 ship time as "enough to prove Phase 1 evidence ran" without
# empirical tuning. Section-27 of 00-infrastructure/TODO-08 wires this gate
# into the section-21 heuristic miss-log surface so the threshold can be
# tuned by data: every BLOCK fires emit_warn(step=4), the periodic
# scripts/heuristic-misses-report.sh surfaces the FP ratio, and the
# threshold here adjusts up or down based on observed false-positive rate
# over a documented N-section window. Tuning is a one-line edit.
PHASE1_MIN_READS = 2
_PHASE1_MIN_EVIDENCE = PHASE1_MIN_READS  # legacy alias retained for callers

# review-kind detection. Anchored to first non-blank line per
# codex_review_completed.py _detect_review_kind: scanning the entire
# body lets earlier markers in repository text or example diffs spoof
# the attribution.
_REVIEW_KIND_RE = re.compile(
    r"\[\s*review[-_ ]kind\s*:\s*([a-zA-Z][a-zA-Z-]*)\s*\]",
    re.IGNORECASE,
)

# Path scope: relative or absolute. Per Codex re-adversarial 2026-04-28
# H1, Phase 1 evidence is not limited to kernel paths -- infrastructure
# TODOs (TODO-08 itself, the host-side automation TODOs) own .claude/
# hooks, scripts, and docs. The regex covers the codebase regions where
# `[x]` checklist items legitimately point: kernel/boot source, host-
# side automation hooks/skills, build/test scripts, and infrastructure
# docs. todo/ is intentionally NOT included -- the TODO file itself is
# the meta layer the review walks over, not the implementation evidence.
_PHASE1_PATH_RE = re.compile(
    r"(?:^|/)(?:src|include|\.claude|scripts|docs)(?:/|$)"
)


# Bash-command path scan: paths inside shell command strings appear
# after whitespace / quote / slash / start, and may end at whitespace
# / quote / pipe / EOL. Permissive boundaries; the `has_explore_cmd`
# gate above is what scopes this to actual exploration commands.
_PHASE1_BASH_PATH_RE = re.compile(
    r"(?:^|[\s'\"/])(?:src|include|\.claude|scripts|docs)/"
)


def _repo_root() -> Optional[str]:
    """Root for this gate's state.

    `git rev-parse` in the CURRENT directory makes the answer depend on how the
    caller set itself up -- a `mktemp` fixture resolves to the fixture, the real
    repo, or nothing depending on the environment, which is why four of this
    gate's assertions flapped across suite runs on 2026-07-31 with no
    intervening edits. An explicit override lets a test be authoritative.
    """
    try:
        import sys as _sys
        _sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import _state_root
        forced = _state_root.override()
        if forced:
            return forced
    except Exception:
        pass
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip() or None
    except Exception:
        return None


def _ts_ns() -> int:
    return time.time_ns() if hasattr(time, "time_ns") else int(time.time() * 1e9)


def _is_adversarial_dispatch(cmd: str) -> bool:
    """Match Bash invocations of codex-companion.mjs adversarial-review
    whose prompt's first non-blank line carries [review-kind: adversarial].
    Returns False for design / consistency / perf / re-adversarial.

    Delegated to _review_kind.detect_review_kind_from_cmd (shared helper)
    per TODO-08 §17 deferred-XREF M2 fix: skill_step_map.py + this gate
    + codex_review_completed.py now share one classifier.
    """
    from _review_kind import is_dispatch_of_kind
    return is_dispatch_of_kind(cmd, "adversarial")


def _load_active_review_skill(root: str) -> Optional[dict]:
    """Return the active review-todo-section entry from
    skill-progress.json, or None. Skips compaction-orphaned entries
    per TODO-08 §16 doctrine (mirror skill_step_block._select_active_skill).
    """
    state_path = os.path.join(root, _STATE_REL)
    try:
        with open(state_path, "r", encoding="utf-8") as f:
            state = json.load(f)
    except Exception:
        return None
    if not isinstance(state, dict):
        return None
    if "review-todo-section" not in MULTI_STEP_SKILLS:
        return None
    entry = state.get("review-todo-section")
    if not isinstance(entry, dict):
        return None
    if entry.get("compaction_orphaned") is True:
        return None
    return entry


def _extract_event_ts_ns(ev: dict) -> int:
    """Best-effort timestamp extraction from a transcript event. Returns
    0 if no usable timestamp.
    """
    ts_ns = ev.get("ts_ns")
    if isinstance(ts_ns, int) and ts_ns > 0:
        return ts_ns
    ts = ev.get("timestamp")
    if isinstance(ts, str) and ts:
        try:
            from datetime import datetime
            try:
                dt = datetime.fromisoformat(ts.replace("Z", "+00:00"))
            except ValueError:
                return 0
            return int(dt.timestamp() * 1e9)
        except Exception:
            return 0
    return 0


_BASH_PHASE1_CMDS = (
    "grep", "rg", "cat", "head", "tail", "sed", "awk",
    "find", "wc", "nl", "ls", "less", "more",
)


def _bash_is_phase1_evidence(cmd: str) -> bool:
    """True if a Bash command qualifies as Phase-1 exploration evidence:
    invokes one of the read-only exploration tools AND names a path
    under the Phase-1 region (src/include/.claude/scripts/docs).

    Heuristic by design -- the goal is to count legitimate shell-side
    exploration, not perfectly classify every shell command. False
    positives lower the BLOCK rate (acceptable; the gate is advisory
    enough that over-counting is preferable to under-counting and
    forcing SKIP-env workarounds). False negatives just leave the
    gate firing as before -- the agent can still use Read/Grep tools.
    """
    if not isinstance(cmd, str) or not cmd:
        return False
    # Walk command-line tokens. The first non-env-prefix token is the
    # entry program; subsequent tokens are arguments. We accept the
    # entry program if it ends in any of _BASH_PHASE1_CMDS, OR if any
    # later token after a pipe / && / ; matches an exploration command.
    # Codex re-adversarial M1 fix: do NOT lowercase the command for
    # exact-match. POSIX command names are case-sensitive; `RG` and
    # `Grep` are not real commands and matching them would let
    # mixed-case typos satisfy the gate. The explore set is fixed
    # lowercase; the command head must match exactly.
    has_explore_cmd = False
    for explore in _BASH_PHASE1_CMDS:
        if (
            cmd.startswith(f"{explore} ")
            or cmd.startswith(f"{explore}\t")
            or f" | {explore} " in cmd
            or f"|{explore} " in cmd
            or f"&& {explore} " in cmd
            or f"; {explore} " in cmd
            or f'"{explore} ' in cmd
            or f"'{explore} " in cmd
        ):
            has_explore_cmd = True
            break
    if not has_explore_cmd:
        return False
    # Bash commands embed paths after whitespace, quotes, or slashes;
    # _PHASE1_PATH_RE alone only matches start-of-string or after a
    # slash, missing the common ` <path>` argument shape. Extend the
    # boundary chars here for the Bash-command scanning case only;
    # the strict regex still guards Read/Grep/Glob inputs above.
    return bool(_PHASE1_BASH_PATH_RE.search(cmd))


def _count_phase1_evidence(transcript_path: str, started_ts: int) -> int:
    """Walk the transcript JSONL forward from started_ts, counting
    Read/Grep/Glob tool_use events whose input names a path under any
    Phase-1-relevant codebase region (src / include / .claude / scripts
    / docs; see _PHASE1_PATH_RE).

    Per Codex design Q1: match ONLY location fields (file_path / path /
    glob); pattern is the search needle, not the location -- excluded.
    """
    if not transcript_path or not os.path.exists(transcript_path):
        return 0
    count = 0
    try:
        with open(transcript_path, "r", encoding="utf-8") as f:
            for line in f:
                try:
                    ev = json.loads(line)
                except Exception:
                    continue
                ev_ts = _extract_event_ts_ns(ev)
                # Codex adversarial M2 fix (2026-04-28): fail-closed on
                # missing timestamps. The earlier `if ev_ts and ev_ts <
                # started_ts: continue` counted timestamp-less events as
                # post-skill evidence, defeating the started_ts boundary
                # for malformed or older transcripts. Skip ANY event we
                # cannot prove came after the skill started.
                if ev_ts == 0 or ev_ts < started_ts:
                    continue
                msg = ev.get("message")
                if not isinstance(msg, dict):
                    continue
                content = msg.get("content")
                if not isinstance(content, list):
                    continue
                for c in content:
                    if not isinstance(c, dict):
                        continue
                    if c.get("type") != "tool_use":
                        continue
                    name = c.get("name") or ""
                    raw_input = c.get("input")
                    # Codex test-coverage M1 fix: a truthy non-dict
                    # `input` (string, list) used to crash inp.get()
                    # mid-walk, aborting later valid evidence. Coerce
                    # to a real dict; non-dicts contribute zero
                    # location evidence by construction.
                    inp = raw_input if isinstance(raw_input, dict) else {}
                    if name == "Read":
                        loc = inp.get("file_path") or ""
                        if _PHASE1_PATH_RE.search(loc):
                            count += 1
                    elif name == "Grep":
                        loc = inp.get("path") or ""
                        glob = inp.get("glob") or ""
                        if _PHASE1_PATH_RE.search(loc) or _PHASE1_PATH_RE.search(glob):
                            count += 1
                    elif name == "Glob":
                        loc = inp.get("path") or inp.get("pattern") or ""
                        if _PHASE1_PATH_RE.search(loc):
                            count += 1
                    elif name == "Bash":
                        # User feedback 2026-04-28: Phase 1 evidence
                        # work frequently happens via shell `grep` /
                        # `cat` / `rg` / `head` / `find` invocations
                        # through the Bash tool, NOT just the dedicated
                        # Read/Grep tools. Without counting Bash, the
                        # gate fires on legitimate Phase 1 work that
                        # used shell exploration -- forcing the agent
                        # to either redo the search via Read/Grep or
                        # use SKIP env (training around the gate, the
                        # exact failure mode `feedback_skill_invocation
                        # _drift` warns against). Count Bash invocations
                        # that look like exploration commands AND name
                        # a path under the Phase-1 region.
                        cmd = inp.get("command") or ""
                        if _bash_is_phase1_evidence(cmd):
                            count += 1
    except Exception:
        pass
    return count


def _log_skip(root: str, reason: str, evidence_count: int) -> None:
    p = os.path.join(root, _SKIP_LOG_REL)
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        rec = {
            "ts_ns": _ts_ns(),
            "kind": "SKIP_PHASE1_BLOCK",
            "reason": reason[:240],
            "evidence_count": evidence_count,
        }
        with open(p, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")
    except Exception:
        pass


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0

    if d.get("tool_name") != "Bash":
        return 0
    cmd = (d.get("tool_input") or {}).get("command") or ""
    if not _is_adversarial_dispatch(cmd):
        return 0

    root = _repo_root()
    if not root:
        return 0

    # TODO-08 §22 #2 bootstrap-mode: when this hook's own file is in
    # the staged diff, the commit ships the hook itself -- downgrade
    # BLOCK to WARN to avoid the chicken-and-egg loop.
    try:
        from _bootstrap_mode import is_bootstrap_commit
        if is_bootstrap_commit(__file__):
            sys.stderr.write(
                "[phase1-evidence-gate] WARN (bootstrap-mode) -- staged "
                "diff includes this hook's own file; gate downgraded "
                "from BLOCK to WARN for the commit that ships it.\n"
            )
            return 0
    except Exception:
        pass

    # TODO-08 section-23: shared SKIP-env scanner -- inline cmd env
    # AND os.environ. Cmd is always present for Bash gate.
    _skip_envs = _se.read_skip_envs(
        cmd,
        keys=("SKIP_PHASE1_BLOCK", "SKIP_PHASE1_BLOCK_REASON"),
    )
    skip_req = _skip_envs.get("SKIP_PHASE1_BLOCK", "") == "1"
    skip_reason = _skip_envs.get("SKIP_PHASE1_BLOCK_REASON", "")
    if skip_req and len(skip_reason) < _SKIP_REASON_MIN_LEN:
        sys.stderr.write(
            "[phase1-evidence-gate] BLOCK -- opt-out malformed: "
            f"SKIP_PHASE1_BLOCK=1 set but SKIP_PHASE1_BLOCK_REASON "
            f"missing or under {_SKIP_REASON_MIN_LEN} chars. The "
            f"opt-out requires a plain-language reason.\n"
        )
        return 2

    entry = _load_active_review_skill(root)
    if not entry:
        return 0

    started_ts = entry.get("started_ts", 0)
    if not isinstance(started_ts, int) or started_ts <= 0:
        return 0

    # Credit prior-skill evidence: when implement-todo-section ran in
    # the same session immediately before review-todo-section, the
    # agent has already done plenty of Read/Grep on the same surface.
    # Walk forward from the EARLIEST multi-step skill start in this
    # session so that handoff doesn't force redundant exploration.
    try:
        state_path = os.path.join(root, _STATE_REL)
        with open(state_path, "r", encoding="utf-8") as f:
            state = json.load(f)
        if isinstance(state, dict):
            for k, v in state.items():
                if k not in MULTI_STEP_SKILLS or not isinstance(v, dict):
                    continue
                if v.get("compaction_orphaned") is True:
                    continue
                ts = v.get("started_ts", 0)
                if (isinstance(ts, int) and ts > 0
                        and ts < started_ts
                        and v.get("session_id") == entry.get("session_id")):
                    started_ts = ts
    except Exception:
        pass

    transcript_path = d.get("transcript_path", "")
    evidence_count = _count_phase1_evidence(transcript_path, started_ts)
    if evidence_count >= _PHASE1_MIN_EVIDENCE:
        return 0

    if skip_req:
        _log_skip(root, skip_reason, evidence_count)
        sys.stderr.write(
            f"[phase1-evidence-gate] SKIP allowed -- reason: {skip_reason}\n"
            f"[phase1-evidence-gate]   logged to {_SKIP_LOG_REL}; "
            f"evidence count was {evidence_count}/{_PHASE1_MIN_EVIDENCE}.\n"
        )
        return 0

    sys.stderr.write(
        "[phase1-evidence-gate] BLOCK -- review-todo-section is at "
        "step 5 (adversarial Codex dispatch) but Phase 1 evidence is "
        f"missing. Found {evidence_count} Read/Grep on src / include "
        f"/ .claude / scripts / docs since the skill started; require "
        f"{PHASE1_MIN_READS}.\n"
        "[phase1-evidence-gate]   Phase 1 of review-todo-section: "
        "(1) evidence map per [x] item -> file:line, (2) scope-gap "
        "audit grep for TODO/FIXME/HACK/STATUS_NOT_IMPLEMENTED, "
        "(3) test-checkpoint verification grepping serial/klog "
        "messages. Run >=2 Read/Grep calls scoped to the codebase "
        "region this TODO owns (src/include for kernel; .claude/"
        "scripts/docs for infrastructure) before dispatching the "
        "adversarial Codex.\n"
        "[phase1-evidence-gate]   Opt-out (verified-already-clean "
        "section, etc.): SKIP_PHASE1_BLOCK=1 "
        f"SKIP_PHASE1_BLOCK_REASON=\"<text >= {_SKIP_REASON_MIN_LEN} chars>\".\n"
        "[phase1-evidence-gate]   Doctrine: feedback_skill_invocation_drift "
        "memory + TODO-08 sections 17 and 27.\n"
    )
    # Section-27: emit a miss-log entry alongside the BLOCK so the section-21
    # heuristic-miss telemetry path can compute the FP ratio over time. Step 4
    # is the canonical step id for Phase 1 evidence (review-todo-section
    # steps 1-3 = evidence map / scope-gap audit / test-checkpoint, step 4 =
    # build that gates step 5 adversarial). The gate stays a hard BLOCK; the
    # WARN is additive (logged-for-ratio, not converted-to-warning).
    todo_path, section = hm.parse_active_todo_section(entry)
    hm.emit_warn(
        root, 4,
        "phase1-evidence-missing",
        f"step-5 adversarial dispatch attempted with {evidence_count}/"
        f"{PHASE1_MIN_READS} Phase 1 Read/Grep evidence calls. Logged for "
        "FP-ratio tuning of the threshold per section-27 doctrine.",
        todo_path=todo_path, section=section,
    )
    return 2


if __name__ == "__main__":
    sys.exit(main())
