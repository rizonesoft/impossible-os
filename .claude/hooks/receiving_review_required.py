#!/usr/bin/env python3
"""Block Edit / Write / MultiEdit until the latest Codex review has
been processed through `Skill(superpowers:receiving-code-review)`.

PreToolUse hook on `Edit` / `Write` / `MultiEdit`. Reads
`.claude/state/last-codex-review.json` (written by
`codex_review_completed.py`). Block decision:

  * No state file -> allow (no recent Codex trigger).
  * `received: true` -> allow (agent already triaged).
  * `received: false` AND `now - timestamp_ns > 3600s` -> allow
    (stale trigger from > 1h ago; agent has presumably moved on).
  * `received: false` AND TTL still valid -> BLOCK (exit 2) with
    the documented envelope naming the trigger and the opt-out.

Stale-pass policy:

  * TIME-based (1 hour TTL): YES. A stale `received: false` from
    yesterday should not block today.
  * NEW REVIEW: YES (implicit). Each new trigger overwrites the
    state with `received: false` + new timestamp.
  * HEAD / tree mismatch: NO. Codex design review High caught
    "stale-tree allow" semantics as a bypass: the agent could
    edit a non-code file (changing the tree) between trigger and
    receive, then the gate would see HEAD/tree mismatch and let
    the next code edit through without the receive ever firing.
    The block stays until the receive happens, period.

Opt-out:

  `RECEIVING_REVIEW_OVERRIDE=1` env var on the same call. Agent
  must set it explicitly per call AND state in their next message
  what code-evidence quote justifies skipping (e.g. "review found
  zero findings, output was 'no issues'"). Documented self-audit
  surface for the user.

Fail-open on malformed state / missing git / hook bug -- a hook
crash should never block tool calls.

Owner: TODO-08-automation-hardening section 3.
"""

import json
import os
import re
import sys
import time
from pathlib import Path

TTL_SECONDS = 3600  # 1 hour stale-pass window

# Bash write-detection (v14 close-out, 2026-08-16). The gate's guarantee ("no
# edit before reception") held only for the Edit/Write tools; applying review
# fixes through `python3 - <<'PY' ... open(p,"w")` heredocs bypassed it, and
# that happened FOUR observed times (TODO-06 sections 39 and 48) -- not
# maliciously, but because awkward edits (regex escapes, BOM/CRLF literals,
# whole-function rewrites) push an honest session toward Bash.
#
# It is PROGRAM-AWARE, not a raw-line regex, because a regex over the whole
# command cannot tell executed code from a quoted data argument: it both
# MISSED cp/mv/dd (Codex adversarial [high]) and FALSE-POSITIVED on
# `grep -F "open(p,'w')" src` (Codex adversarial [medium]) -- and blocking a
# read-only grep during review reception is a regression, because reception IS
# evidence-gathering. So the executed PROGRAM of each pipeline segment decides:
# an interpreter's inline/heredoc body IS executed and is scanned for writes;
# a read-only tool's quoted argument is data and is never scanned. This is a
# best-effort EARLY catch; the load-bearing guarantee is the commit/section
# gate re-asserting reception at commit time (the receipt is checked there),
# so a residual miss delays a catch, it does not ship unreviewed work. A full
# allow-only-read-only inversion is a separate design pass, CARRIED.
import shlex

_INTERPRETERS = frozenset({
    "python", "python3", "python2", "node", "nodejs", "perl", "ruby",
    "php", "deno", "bun",
})
# Programs that mutate the filesystem by their very invocation.
_MUTATOR_PROGS = frozenset({
    "tee", "cp", "mv", "dd", "truncate", "install", "ln", "rm", "rmdir",
    "mkdir", "touch", "patch", "shred", "chmod", "chown",
})
# git subcommands that write the working tree or index.
_GIT_WRITE_SUBS = frozenset({
    "apply", "checkout", "restore", "reset", "stash", "clean", "mv", "rm",
    "add", "commit", "am", "cherry-pick", "revert", "merge", "rebase",
})
# Write-op signatures inside an interpreter body (executed code, so scanned).
_CODE_WRITE_RES = (
    re.compile(r"\bopen\s*\([^)]*['\"][waxr]?[+]?[waxb+]*['\"]"
               r"|\bopen\s*\([^)]*['\"][wax]"),
    re.compile(r"\.write_text\s*\("),
    re.compile(r"\.write_bytes\s*\("),
    re.compile(r"\bos\.(?:rename|replace|remove|unlink|mkdir|makedirs|"
               r"rmdir|symlink|link|truncate|chmod)\s*\("),
    re.compile(r"\bshutil\.(?:copy\w*|move|rmtree)\s*\("),
    re.compile(r"\bfs\.(?:write|append|rename|unlink|copy|rm|mkdir|"
               r"truncate)\w*\s*\("),
    re.compile(r"\bPath\([^)]*\)\.(?:write_text|write_bytes|rename|"
               r"replace|unlink|mkdir)\b"),
)
# open() in read mode must NOT count -- the exact F3 false-positive class,
# except here it appears in genuinely executed code (`print(open('f').read())`).
_OPEN_READ_RE = re.compile(r"\bopen\s*\([^)]*['\"]r[b]?['\"]\s*[),]")
_OPEN_WRITE_RE = re.compile(r"\bopen\s*\([^)]*,\s*['\"][waxr]?[+]?[wax][b+]*['\"]"
                            r"|\bopen\s*\([^)]*,\s*['\"][wax]")
_REDIR_RE = re.compile(r"(?<![\d&\-=<])>{1,2}\s*(?!/tmp/|/dev/|&)[\w./$]")


def _has_interpreter(text: str) -> bool:
    return any(re.search(r"\b" + re.escape(p) + r"\b", text)
              for p in _INTERPRETERS)


def _segment_mutates(seg: str) -> bool:
    seg = seg.strip()
    if not seg:
        return False
    # A redirect to a non-tmp path mutates regardless of the program.
    if _REDIR_RE.search(seg):
        return True
    try:
        toks = shlex.split(seg, comments=False, posix=True)
    except ValueError:
        # Unbalanced quotes (often a heredoc/partial) -- fall back to a body
        # scan rather than allowing silently. Conservative in the safe
        # direction: a missed read-only case just costs a receive.
        return _body_writes(seg)
    # Skip leading VAR=val environment assignments.
    i = 0
    while i < len(toks) and re.match(r"^[A-Za-z_]\w*=", toks[i]):
        i += 1
    if i >= len(toks):
        return False
    prog = toks[i].rsplit("/", 1)[-1]
    rest = toks[i + 1:]
    if prog in _INTERPRETERS:
        # -c "code" or a script arg: scan the executed body only.
        return _body_writes(" ".join(rest))
    if prog == "sed":
        return any(a == "-i" or (a.startswith("-") and "i" in a and
                                 not a.startswith("--")) for a in rest)
    if prog == "git":
        sub = next((a for a in rest if not a.startswith("-")), "")
        return sub in _GIT_WRITE_SUBS
    if prog == "tee":
        # tee to /tmp or /dev is logging, not a source mutation (same
        # exemption redirects get). Gate only when a real path is written.
        targets = [a for a in rest if not a.startswith("-")]
        return any(not (t.startswith("/tmp/") or t.startswith("/dev/"))
                   for t in targets) if targets else False
    if prog in _MUTATOR_PROGS:
        return True
    # Any other program (grep, rg, cat, ls, wc, awk-read, ...): its arguments
    # are DATA, not executed code -- do not scan them (F3).
    return False


def _body_writes(body: str) -> bool:
    if _OPEN_WRITE_RE.search(body):
        return True
    for r in _CODE_WRITE_RES[1:]:  # skip the loose open() alt; handled above
        if r.search(body):
            return True
    return False


def _bash_mutates_files(cmd: str) -> bool:
    # A heredoc feeding an interpreter carries executed code that shlex and
    # segment-splitting would mangle (a `;` or `|` inside the body is Python,
    # not a shell operator), so it is handled WHOLE and first. `grep "<<..."`
    # with no interpreter name never reaches this branch.
    if "<<" in cmd and _has_interpreter(cmd):
        return _body_writes(cmd)
    # Otherwise split into pipeline segments on the shell control operators;
    # any segment that mutates taints the whole command.
    for seg in re.split(r"\|\||&&|[;|&]", cmd):
        if _segment_mutates(seg):
            return True
    return False


def _opt_out(payload: dict) -> bool:
    # TODO-08 section-23: shared SKIP-env scanner -- inline + environ.
    # `payload` is the harness PostToolUse JSON (tool_name + tool_input);
    # required-arg form keeps the helper out of module-globals scope.
    import _skip_env as _se
    _cmd = ""
    if payload.get("tool_name") == "Bash":
        _cmd = (payload.get("tool_input") or {}).get("command", "") or ""
    if _se.read_skip_envs(
        _cmd, keys=("RECEIVING_REVIEW_OVERRIDE",)
    ).get("RECEIVING_REVIEW_OVERRIDE") == "1":
        return True

    # File-based ONE-SHOT override, for the tools that have no other channel.
    #
    # The env scanner reads an INLINE prefix, which only exists for Bash. An
    # Edit/Write has no command string, so its only route was the ambient
    # process environment -- which an interactive session cannot set per call.
    # The documented opt-out was therefore unusable for exactly the two tools
    # this gate blocks.
    #
    # One-shot on purpose: the sentinel is consumed on read, so it cannot be
    # created once and left to disable the gate indefinitely. That keeps it an
    # override rather than an off-switch, which is the property that makes it
    # safe to offer at all.
    try:
        sentinel = (Path(__file__).resolve().parent.parent
                    / "state" / "receiving-review-override")
        if sentinel.exists():
            sentinel.unlink()          # consume BEFORE allowing
            return True
    except OSError:
        pass                            # unreadable sentinel is not an override
    return False


def _load_state(path: Path) -> dict | None:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def _ns_to_iso(ns: int) -> str:
    """Format a nanosecond timestamp as a UTC ISO-8601 string for
    the BLOCK envelope. Defensive against bad input."""
    try:
        from datetime import datetime, timezone
        return datetime.fromtimestamp(ns / 1e9, tz=timezone.utc).isoformat(timespec="seconds")
    except Exception:
        return f"ts_ns={ns}"


def _emit_block(state: dict, target_path: str) -> None:
    trigger = state.get("trigger") or "(unknown)"
    ts_ns = int(state.get("timestamp_ns") or 0)
    ts_iso = _ns_to_iso(ts_ns) if ts_ns else "(unknown time)"
    age_s = max(0, int((time.time_ns() - ts_ns) / 1e9)) if ts_ns else 0
    sys.stderr.write(
        f"[receiving-review-required] BLOCK -- Codex review at "
        f"{ts_iso} (trigger: {trigger}) has not been processed "
        f"through `Skill(superpowers:receiving-code-review)`.\n"
        f"[receiving-review-required] age: {age_s}s; TTL: "
        f"{TTL_SECONDS}s; target file: {target_path or '(unknown)'}\n"
        f"[receiving-review-required] policy: invoke "
        f"`Skill(name=\"superpowers:receiving-code-review\")` to "
        f"verify each finding at file:line and classify "
        f"Fix / Reject / Accept BEFORE editing. The receive must "
        f"happen even when the review reports zero findings -- the "
        f"act of receiving is the verification.\n"
        f"[receiving-review-required] opt-out: set "
        f"`RECEIVING_REVIEW_OVERRIDE=1` on the same call AND "
        f"state in your next message what code-evidence quote "
        f"justifies skipping (e.g. \"review found zero findings, "
        f"output was 'no issues'\").\n"
        f"[receiving-review-required] doctrine: CLAUDE.md "
        f"\"Mandatory Skill Triggers\" + memory "
        f"feedback_skill_invocation_drift / "
        f"feedback_never_skip_review.\n"
    )


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except Exception:
        return 0  # malformed -- fail open
    if _opt_out(payload):
        return 0
    tool = payload.get("tool_name")
    if tool not in ("Edit", "Write", "MultiEdit", "Bash"):
        return 0
    if tool == "Bash":
        # Bash route (v14 close-out): only file-MUTATING commands are gated --
        # the observed heredoc/`sed -i`/redirect evasion shapes. Everything
        # else (grep, git status, builds, dispatches) passes untouched.
        cmd = (payload.get("tool_input") or {}).get("command", "") or ""
        if not _bash_mutates_files(cmd):
            return 0
    # Hot-path optimization (review-pipeline perf H5): compute the
    # state path from this file's location instead of forking
    # `git rev-parse --show-toplevel`. The hook lives at
    # `<repo>/.claude/hooks/receiving_review_required.py`; the state
    # file is always at `<repo>/.claude/state/last-codex-review.json`.
    # Pre-fix Codex measurement: ~34ms median per Edit on the
    # no-state fast path (git subprocess dominated). Post-fix: skip
    # the fork entirely when the state file is absent.
    state_path = Path(__file__).resolve().parent.parent / "state" / "last-codex-review.json"
    if not state_path.exists():
        return 0  # no prior Codex trigger -- fast-path early return
    state = _load_state(state_path)
    if state is None:
        return 0  # malformed state file -- fail open
    if state.get("received") is True:
        return 0  # already triaged

    # SESSION BINDING. This state file is per-repo shared state, so an unbound
    # gate blocks every session in the repo -- not just the one that dispatched.
    # Measured 2026-07-24 21:40: the headless overnight run dispatched a review
    # and 6 seconds later the interactive operator's unrelated Edit to a backlog
    # file was BLOCKed by it, followed by a Write to a scratchpad .py OUTSIDE the
    # repo. Same class as the unspaced-operator bug: it fails closed, so nothing
    # unreviewed ships, but it blocks CORRECT work and trains reflexive
    # RECEIVING_REVIEW_OVERRIDE use -- the habit these gates exist to prevent.
    #
    # Receiving another session's review would also be WRONG, not merely
    # unnecessary: it consumes a receipt bound to that pipeline, the same
    # receipt-state corruption runner_bash_guard.py exists to stop subagents
    # causing. So a foreign record is skipped, never auto-received.
    #
    # Bound on session_id, NOT the driver_run_id the backlog item suggested:
    # that field is parsed from an optional prompt marker and is empty on every
    # real dispatch, so it would bind to nothing. Records written before this
    # change carry no session_id; those stay enforcing for every session
    # (fail-closed on absence), which keeps a mid-flight review from being
    # silently dropped by the upgrade.
    rec_session = state.get("session_id") or ""
    cur_session = payload.get("session_id") or ""
    if rec_session and cur_session and rec_session != cur_session:
        return 0  # another session's review -- not this session's to receive

    # Cross-lane guard for the case where session ids are unavailable on one
    # side: a review dispatched BY the headless run is never the interactive
    # operator's to receive, and vice versa. OVERNIGHT_SEQUENCER_RUN=1 is the
    # discriminator CLAUDE.md already documents for run_phase_guard.py -- this
    # gate family simply never got the same treatment.
    rec_overnight = state.get("overnight_run")
    cur_overnight = os.environ.get("OVERNIGHT_SEQUENCER_RUN") == "1"
    if isinstance(rec_overnight, bool) and rec_overnight != cur_overnight:
        return 0  # dispatched by the other lane
    ts_ns = state.get("timestamp_ns")
    if not isinstance(ts_ns, int):
        return 0  # malformed state -- fail open
    age_s = (time.time_ns() - ts_ns) / 1e9
    if age_s > TTL_SECONDS:
        return 0  # stale trigger; allow
    ti = payload.get("tool_input") or {}
    target = (ti.get("file_path", "")
              or ("Bash: " + (ti.get("command", "") or "")[:80]))
    _emit_block(state, target)
    return 2


if __name__ == "__main__":
    sys.exit(main())
