#!/usr/bin/env python3
# block-via: exit 2 (headless overnight run only -- OVERNIGHT_SEQUENCER_RUN=1;
# researcher SUBAGENTS and interactive sessions are never gated)
"""PreToolUse (WebSearch/WebFetch): R4 -- main-session web research reroute.

The three researcher agents (parity-research-analyst, spec-research-analyst,
web-research-analyst) exist so multi-page web reads land in a throwaway
context. The 2026-07-19 digest caught the headless MAIN session firing 10
WebSearch calls in a 36-second burst (run-20260719-100809 log:75-84) IN
PARALLEL with a parity-research-analyst dispatch doing the same research --
paying for 10 raw search-result payloads on top of the agent's bounded report,
with every subsequent turn re-reading them in the cached prefix.

R4 blocks main-session WebSearch/WebFetch in the headless run and reroutes to
the owning researcher. Subagent calls pass untouched (the researchers DO the
searching -- keyed on the agent transcript path, same discriminator as
runner_bash_guard), so the reroute can never deadlock. Interactive sessions
are exempt (env discriminator absent). Fail-open on any error.

Caller identity is a POSITIVE test, so a negative is ambiguous rather than
proof of a main-session caller -- see `_is_subagent_caller`. Because of that
ambiguity the block is bounded by an anti-wedge valve (see MAX_BLOCKS): the
same call is released after two blocks, so a misclassified researcher loses two
calls instead of its whole capability. `--selftest` covers both.
"""
from __future__ import annotations

import hashlib
import json
import os
import sys
from pathlib import Path

# ANTI-WEDGE VALVE (Option C, 2026-07-31). The SAME call is released after this
# many blocks. WHY a valve rather than a demotion or a researcher allowlist:
# `_is_subagent_caller` returns a POSITIVE identification, so a False means "no
# marker found" -- exactly the state that produced the 3-section
# misclassification -- and is NOT evidence of a main-session caller. A headless
# run cannot unset OVERNIGHT_SEQUENCER_RUN, so before this valve a wrongly
# blocked researcher was blocked FOREVER: the 2026-07-31 dispatch "retried and
# could not get the gate to release", and the run paid a full researcher
# dispatch for explicitly-uncited recollection, three sections running.
#
# The asymmetry that sets the direction: a false BLOCK costs EVIDENCE (4 OS
# Comparison rows across sections 24/27/36/37 carry unverifiable cells); a
# false PASS costs TOKENS (10 main-session searches in 36s,
# run-20260719-100809). The valve keeps the nudge for the main session -- which
# retries only if it genuinely needs the search -- while bounding the worst case
# for a researcher at two blocks instead of forever. Precedent: the identical
# valve in search_offload_gate, added for the identical reason.
MAX_BLOCKS = 2
STATE_REL = ".claude/state/websearch-offload-valve.json"

_MSG = (
    "[websearch-offload BLOCK -- R4] Headless main-session {tool} is a "
    "researcher-agent task: dispatch parity-research-analyst (Win11/Linux "
    "feature parity), spec-research-analyst (normative hardware/format specs), "
    "or web-research-analyst (toolchain/emulator/host/CI -- everything else) "
    "and consume its bounded report instead of raw search payloads (measured: "
    "10 main-session WebSearch calls ran in parallel with a researcher "
    "dispatch on the same question, run-20260719-100809). Subagent calls pass "
    "untouched. If you are a researcher agent and this is a misclassification, "
    "RETRY the identical call: the anti-wedge valve releases it after "
    "{max_blocks} blocks rather than blocking it forever."
)


def _is_subagent_caller(d: dict) -> bool:
    """Positive subagent identity from the payload (2026-07-31 fix).

    Was keyed on `transcript_path` alone, which a subagent payload fills with
    the PARENT session's transcript -- so every researcher dispatch read as
    main-session and was blocked by the rule that exists to route work TO it
    (measured 3 consecutive sections; run-20260731-095007 log:218-240).
    """
    path = str(d.get("transcript_path") or "")
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import runner_bash_guard
        return runner_bash_guard.is_subagent_payload(d)
    except Exception:
        return _is_subagent_transcript(path)


def _is_subagent_transcript(path: str) -> bool:
    # Single source of truth: runner_bash_guard's battle-tested detector
    # (review 2026-07-19 reuse finding -- a copy here would silently diverge
    # the next time the transcript-naming heuristic is hardened). Fallback
    # only if the import itself breaks: treat as MAIN session, which fails
    # toward the reroute message, never toward silently skipping the gate.
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import runner_bash_guard
        return runner_bash_guard._is_subagent_transcript(path)
    except Exception:
        if not path:
            return False
        base = os.path.basename(path)
        return base.startswith("agent-") or "/subagents/" in path


def _log(event: str, tool: str) -> None:
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import _offload_log
        _offload_log.log_event(Path.cwd(), event, "websearch_offload_gate", tool)
    except Exception:
        pass


def _state_path() -> Path:
    """Valve state file. Overridable so TESTS never touch live runner state.

    The repo filed a finding on 2026-07-31 that control-plane tests flap because
    hooks resolve their root from the CURRENT directory and end up reading the
    live `.claude/state/` the run is mutating. This gate refuses to add another
    instance of that: `WEBSEARCH_VALVE_STATE` lets a test point the valve at a
    fixture path explicitly instead of relying on cwd resolution.
    """
    env = os.environ.get("WEBSEARCH_VALVE_STATE")
    if env:
        return Path(env)
    # parents[2] of .claude/hooks/x.py is the repo root. Getting this wrong is
    # not a local bug -- a `.claude/.claude/` directory once silently disabled
    # five live gates (test-tooling caught it 2026-07-28).
    return Path(__file__).resolve().parents[2] / STATE_REL


def _call_key(tool: str, d: dict) -> str:
    """Identity of THIS call: tool + query/url.

    Keyed on the exact call so insisting on ONE search does not disable the gate
    for every other search in the session.
    """
    ti = d.get("tool_input") if isinstance(d.get("tool_input"), dict) else {}
    subject = str(ti.get("query") or ti.get("url") or "")
    return hashlib.sha256(f"{tool}\x00{subject}".encode()).hexdigest()[:16]


def _valve(key: str, session_id: str) -> bool:
    """True = still gated; False = release (the caller insisted, let it through).

    Session-scoped: a new session starts a fresh count, so a block budget cannot
    leak across runs. Fail-open on any state error -- a valve that cannot track
    must not be the thing that wedges a run.
    """
    try:
        p = _state_path()
        try:
            st = json.loads(p.read_text(encoding="utf-8"))
            if not isinstance(st, dict) or st.get("session_id") != session_id:
                raise ValueError
        except Exception:
            st = {"session_id": session_id, "counts": {}}
        n = int(st["counts"].get(key) or 0) + 1
        st["counts"][key] = n
        if len(st["counts"]) > 200:
            st["counts"] = dict(list(st["counts"].items())[-200:])
        try:
            p.parent.mkdir(parents=True, exist_ok=True)
            tmp = p.with_suffix(p.suffix + ".tmp")
            tmp.write_text(json.dumps(st))
            tmp.replace(p)
        except Exception:
            # CANNOT PERSIST -> CANNOT BOUND. Swallowing this (as the
            # search_offload_gate precedent does) leaves every call reading
            # n=1 forever, so the budget never advances and the valve can never
            # release -- reintroducing the exact permanent block it exists to
            # prevent. Caught by the selftest before this shipped. An
            # untrackable valve must fail OPEN, like every other error path here.
            return False
        return n <= MAX_BLOCKS
    except Exception:
        return False        # cannot track -> do not gate


def main() -> int:
    if os.environ.get("OVERNIGHT_SEQUENCER_RUN") != "1":
        return 0
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0
    tool = d.get("tool_name")
    if tool not in ("WebSearch", "WebFetch"):
        return 0
    if _is_subagent_caller(d):
        return 0
    key = _call_key(str(tool), d)
    gated = _valve(key, str(d.get("session_id") or ""))
    if not gated:
        # Valve released this exact call. Logged so a release is MEASURABLE --
        # the whole R4 defect stayed invisible for three sections because
        # nothing recorded what the gate was doing.
        _log("release", str(tool))
        return 0
    sys.stderr.write(_MSG.format(tool=tool, max_blocks=MAX_BLOCKS) + "\n")
    _log("fire", str(tool))
    return 2


def _selftest() -> int:
    """Exercise every branch against a FIXTURE state file (never live state)."""
    import io
    import tempfile

    failures = []

    def check(label, want, got):
        if want != got:
            failures.append(f"FAIL: {label}: want {want}, got {got}")

    def call(payload, headless=True, state=None):
        old_stdin, old_stderr = sys.stdin, sys.stderr
        old_env = os.environ.get("OVERNIGHT_SEQUENCER_RUN")
        old_state = os.environ.get("WEBSEARCH_VALVE_STATE")
        if headless:
            os.environ["OVERNIGHT_SEQUENCER_RUN"] = "1"
        else:
            os.environ.pop("OVERNIGHT_SEQUENCER_RUN", None)
        if state:
            os.environ["WEBSEARCH_VALVE_STATE"] = state
        sys.stdin = io.StringIO(payload if isinstance(payload, str)
                                else json.dumps(payload))
        sys.stderr = io.StringIO()
        try:
            return main()
        finally:
            sys.stdin, sys.stderr = old_stdin, old_stderr
            if old_env is None:
                os.environ.pop("OVERNIGHT_SEQUENCER_RUN", None)
            else:
                os.environ["OVERNIGHT_SEQUENCER_RUN"] = old_env
            if old_state is None:
                os.environ.pop("WEBSEARCH_VALVE_STATE", None)
            else:
                os.environ["WEBSEARCH_VALVE_STATE"] = old_state

    with tempfile.TemporaryDirectory() as td:
        st = str(Path(td) / "valve.json")
        MAIN = {"tool_name": "WebSearch", "session_id": "s1",
                "tool_input": {"query": "win11 quota semantics"},
                "transcript_path": "/x/1234.jsonl"}

        # inert outside the headless run, and on non-web tools
        check("interactive exempt", 0, call(MAIN, headless=False, state=st))
        check("non-web tool", 0,
              call({**MAIN, "tool_name": "Bash"}, state=st))

        # main session: blocked twice, then the valve releases the SAME call
        check("main block 1", 2, call(MAIN, state=st))
        check("main block 2", 2, call(MAIN, state=st))
        check("valve releases on 3rd", 0, call(MAIN, state=st))
        check("stays released", 0, call(MAIN, state=st))

        # the valve is keyed per call: a different query is independent
        other = {**MAIN, "tool_input": {"query": "linux kselftest tap"}}
        check("different query still blocks", 2, call(other, state=st))
        # ... and so is the same subject under the other tool
        check("WebFetch is a distinct key", 2,
              call({**MAIN, "tool_name": "WebFetch"}, state=st))

        # subagents pass on ANY positive marker, and burn no valve budget
        st2 = str(Path(td) / "valve2.json")
        for marker in ({"transcript_path": "/x/subagents/agent-ab.jsonl"},
                       {"agent_transcript_path": "/x/subagents/agent-ab.jsonl"},
                       {"agent_id": "agent-ab"},
                       {"agent_type": "parity-research-analyst"},
                       {"subagent_type": "spec-research-analyst"}):
            check(f"subagent passes via {sorted(marker)[0]}", 0,
                  call({**MAIN, **marker}, state=st2))
        check("subagent consumed no budget", 2, call(MAIN, state=st2))

        # a new session starts a fresh budget (no leak across runs)
        st3 = str(Path(td) / "valve3.json")
        check("s1 block", 2, call(MAIN, state=st3))
        check("s1 block again", 2, call(MAIN, state=st3))
        check("s2 is fresh", 2, call({**MAIN, "session_id": "s2"}, state=st3))

        # empty tool_input is still a valid key, not a crash
        check("missing tool_input blocks", 2,
              call({"tool_name": "WebSearch", "session_id": "s9",
                    "transcript_path": "/x/1.jsonl"},
                   state=str(Path(td) / "v4.json")))

        # fail-open: garbage payload, and an unwritable state path
        check("garbage payload fails open", 0, call("not json", state=st))
        check("unreadable state fails open (releases)", 0,
              call({**MAIN, "session_id": "s7"},
                   state="/proc/nonexistent-dir/valve.json"))

        # the live state file must NEVER be touched when the override is set
        live = Path(__file__).resolve().parents[2] / STATE_REL
        before = live.read_text() if live.exists() else None
        call({**MAIN, "session_id": "s8"}, state=str(Path(td) / "v5.json"))
        after = live.read_text() if live.exists() else None
        check("live state untouched under override", before, after)

    for f in failures:
        print(f)
    print(f"{'FAIL' if failures else 'PASS'}: websearch_offload_gate selftest "
          f"({len(failures)} failure(s))")
    return 1 if failures else 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(_selftest())
    try:
        sys.exit(main())
    except Exception:
        sys.exit(0)  # fail-open: a broken guard must never wedge the run
