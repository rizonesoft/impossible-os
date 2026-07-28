#!/usr/bin/env python3
# block-via: RETIRED 2026-07-28 -- unwired from settings.json, never fires.
r"""RETIRED (2026-07-28). Kept as the record of a measurement that did not hold.

WHY IT WAS RETIRED, from the live 2026-07-28 canary run rather than reasoning.
Two independent faults, either one fatal:

  1. THE REMEDIATION IS IMPOSSIBLE WHERE THE GATE FIRES. This hook was scoped
     to the headless run alone (OVERNIGHT_SEQUENCER_RUN=1), and the headless
     session has no Grep tool: `Grep is not available in this session -- search
     file contents with `grep` via the Bash tool instead`. So it blocked a
     command and directed the model to a tool that does not exist there. The
     scoping decision and the remediation contradicted each other from day one.

  2. THE SAVING WAS NEVER REAL. The premise was that a Bash search's output
     "lands in this context and is re-charged as cache-read on every later
     turn", unlike the Grep tool. But EVERY tool result lands in context,
     including the Grep tool's. Measured on the live run: the blocked
     `grep -n X file | head -20` and the piped form it was pushed into,
     `cat file | grep -n X | head -20`, return IDENTICAL output, hence
     identical context. Zero saving, and the gate cost 3 tool calls where 1
     would do (block, retry, reformulate) before the run settled on
     `cat file | grep` as its standing idiom.

The item is not repairable by rewording: bounding output and agent offload are
already owned by other gates, and the Grep-tool premise cannot be fixed in an
environment with no Grep tool. Retained (not deleted) so the next person to
propose "route Bash searches to the Grep tool" finds the measurement first.

Original design notes follow.

PreToolUse (Bash): reroute a LEADING file-search to the Grep/Glob tool.

CLAUDE.md sets the floor -- "the Grep-tool-over-Bash-grep floor is
MCP-independent and always applies" -- and it is measurably not being held.

MEASURED 2026-07-27 (token-saver T2-3). The main session's tool mix was
`bash 1,810 / bash_search 885 / edit 969 / read 282 / agent 48`: the search
bucket was the second-largest consumer and every one of its results landed in
the main context, where it is re-read in the cached prefix on every later turn.

THE ITEM'S 885 IS NOT ALL ADDRESSABLE, and the difference is the whole design.
Re-counting `.claude/state/tool-history.jsonl` with shlex tokenization instead
of a regex:

    669   LEAD file-search           <- gated here; the Grep tool does this
    563   search in a pipe/subshell  <- MUST be allowed: it filters a COMMAND's
                                        stdout, which the Grep tool cannot do
  1,619   unparseable by shlex       <- fail open; a third of Bash traffic is
                                        multi-line/heredoc and cannot be judged
  1,860   not a search

So this gate reroutes ~669 calls, not 885, and is deliberately blind to a third
of the traffic. A regex would have "seen" more and been wrong: a grep pattern
containing `\|` (alternation) inside quotes reads as a pipe to a regex, so a
naive matcher mistakes a leading file-search for an output filter and vice
versa. Tokenizing is the only way to tell them apart, and an unparseable
command is one we decline to judge.

Scoped to the HEADLESS run, following `websearch_offload_gate`'s precedent: the
measurement comes from unattended runs, and an interactive operator can make
this call themselves. Subagents are never gated -- their searches ARE the
delegated work.

Code: [SEARCH-OFFLOAD] -- docs/infrastructure/hook-codes.md
Selftest: python3 search_offload_gate.py --selftest
"""
from __future__ import annotations

import hashlib
import json
import os
import shlex
import sys
from pathlib import Path

_SEARCH = {"grep", "egrep", "fgrep", "rg", "ag", "ack"}
_FIND = {"find", "locate"}
_WRAPPERS = {"cd", "env", "sudo", "doas", "nice", "nohup", "timeout",
             "ionice", "stdbuf", "command", "exec"}
_BREAK = {"&&", "||", ";", "|", "&"}

STATE_REL = ".claude/state/search-offload-blocks.json"
MAX_BLOCKS = 2      # then the SAME command is released -- see _valve()

_MSG = (
    "[SEARCH-OFFLOAD BLOCK -- T2-3] `{cmd}` is a LEADING file search: its whole "
    "result lands in this context and is re-charged as cache-read on every later "
    "turn. Use the {tool} TOOL instead -- it returns the same matches without the "
    "shell round-trip, and supports `-n`, `-A`/`-B`/`-C`, `-c`, `-l`, `head_limit`, "
    "and glob/type filters, so the flags you were reaching for are covered.{extra}\n"
    "NOT gated, because the Grep tool cannot do them: a search that filters "
    "another command's OUTPUT (anything after a `|`), and any command this hook "
    "cannot tokenize. If you genuinely need the shell form, pipe it -- "
    "`<cmd> | grep ...` is allowed by construction. And if you genuinely need this "
    "exact shell form (aggregation the Grep tool cannot do, e.g. `| sort | uniq -c`), "
    "repeat it: the same command is released after {maxb} blocks."
)

_TODO_EXTRA = (
    " This one targets `todo/` -- for TODO STRUCTURE (sections, XREFs, stamps, "
    "readiness) prefer `python3 scripts/todo-graph/query.py <verb>`, which "
    "returns the structured answer a grep sweep only approximates."
)


_VALUE_FLAGS = {"-A", "-B", "-C", "-m", "-e", "-f", "-d", "--include",
                "--exclude", "--exclude-dir", "--max-count", "--context"}


def _leading_search(cmd: str):
    """Return ("grep"|"find", token, targets_todo) for a LEADING file search.

    Returns None when the command is not a leading file search, or cannot be
    tokenized -- both mean "do not judge this one".

    `|` and `&&` are NOT the same and the distinction is the whole point. After
    a PIPE the search filters another command's stdout, which the Grep tool
    cannot do, so it is allowed. After `&&`/`;` a NEW command begins -- and
    `cd <dir> && grep ...` is the single most common real shape, so treating a
    sequence operator as "not leading" would miss most of the traffic.
    """
    try:
        toks = shlex.split(cmd, posix=True, comments=False)
    except ValueError:
        return None                      # unparseable -> fail open
    segments, cur = [], []
    for t in toks:
        if t == "|":
            segments.append(cur); cur = None      # everything past a pipe filters
            break
        if t in ("&&", "||", ";", "&"):
            segments.append(cur); cur = []
            continue
        if cur is not None:
            cur.append(t)
    if cur:
        segments.append(cur)
    # ONLY the first non-wrapper segment counts. A compound command whose real
    # work comes first -- `sed -i ... && grep -c ... file`, `make && grep` --
    # must NOT be blocked: rejecting it rejects the WRITE too, and the caller
    # loses the edit, not just the search. Found by validating against real
    # command history, where exactly this shape appeared.
    for seg in segments:
        if not seg:
            continue
        i = 0
        while i < len(seg):
            tok = seg[i]
            if tok in _WRAPPERS:
                i += 1
                if tok in ("cd", "timeout", "env") and i < len(seg):
                    i += 1
                continue
            if "=" in tok and not tok.startswith("-"):
                i += 1
                continue
            break
        if i >= len(seg):
            continue             # pure-wrapper segment (bare `cd dir`): skip
        head = os.path.basename(seg[i])
        if head not in _SEARCH and head not in _FIND:
            return None          # first real command is not a search -> allow
        rest = seg[i + 1:]
        if head in _FIND:
            return ("find", head, any("todo" in a for a in rest))
        # grep-family: skip flags (and their values), take the PATTERN, then any
        # remaining non-flag operand is a PATH. No path operand -> reads stdin,
        # which is not a file search.
        j, pattern_seen, has_path = 0, False, False
        while j < len(rest):
            a = rest[j]
            if a.startswith("-"):
                j += 2 if a in _VALUE_FLAGS else 1
                continue
            if not pattern_seen:
                pattern_seen = True
            else:
                has_path = True
                break
            j += 1
        if not has_path:
            return None          # reads stdin, not a file search
        return ("grep", head, any("todo/" in a or a == "todo" for a in rest))
    return None


def _valve(cmd: str, session_id: str) -> bool:
    """True = still gated; False = release (the model insisted, let it through).

    ANTI-WEDGE. The Grep tool covers -n/-A/-B/-C/-c/-l/head_limit, but it cannot
    do shell aggregation: `grep -o ... | sort | uniq -c`, `grep ... | awk ...`.
    Those are LEADING searches, so they are gated -- and in the headless run the
    model cannot unset OVERNIGHT_SEQUENCER_RUN, so without a valve a genuinely
    needed pipeline would be blocked forever. That is a wedge, and every other
    gate added alongside this one has an escape (the read cache releases after
    two blocks, the advisory budget fails open, the stranded gate always allows
    `park`). This one shipped without one; found while checking canary
    readiness, before any unattended arm.

    Keyed on the exact command, so insisting on ONE pipeline does not disable
    the gate for anything else. Fail-open on any state error.
    """
    try:
        key = hashlib.sha256(cmd.encode()).hexdigest()[:16]
        # parents[2] of THIS FILE is the repo root (.claude/hooks/x.py -> root).
        # Getting this wrong is not a local bug: it created a `.claude/.claude/`
        # directory, and `_repo_root()` in build_offload_reminder, rotate_hint,
        # agent_dispatch_required, session_brief_inject and
        # verify_cadence_reminder walks up to the first directory CONTAINING a
        # `.claude`, so `.claude/` itself then satisfied the test and those
        # hooks silently resolved the root one level short -- disabling five
        # live gates with no error anywhere. Caught by test-tooling 2026-07-28.
        p = Path(__file__).resolve().parents[2] / STATE_REL
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
            pass
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
    if not isinstance(d, dict) or d.get("tool_name") != "Bash":
        return 0
    cmd = str((d.get("tool_input") or {}).get("command") or "")
    if not cmd:
        return 0
    # A subagent's searches ARE the delegated work.
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import runner_bash_guard
        if runner_bash_guard._is_subagent_transcript(
                str(d.get("transcript_path") or "")):
            return 0
    except Exception:
        pass
    hit = _leading_search(cmd)
    if not hit:
        return 0
    kind, tok, todo = hit
    if not _valve(cmd, str(d.get("session_id") or "")):
        return 0        # released after MAX_BLOCKS -- never wedge the run
    sys.stderr.write(_MSG.format(
        cmd=tok, tool="Grep" if kind == "grep" else "Glob", maxb=MAX_BLOCKS,
        extra=_TODO_EXTRA if todo else "") + "\n")
    try:
        import _offload_log
        from pathlib import Path
        _offload_log.log_event(Path.cwd(), "fire", "search_offload_gate", tok)
    except Exception:
        pass
    return 2


def _selftest() -> int:
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    # Gated: leading file searches.
    for c in ('grep -rn "kmalloc" src/kernel/mm/heap.c',
              'grep -n "void \\*kmalloc" -A 12 src/kernel/mm/heap.c',
              'cd /repo && grep -rn "foo" src/',
              'rg "pattern" include/',
              'find src -name "*.c"'):
        check(f"gated: {c[:34]}", _leading_search(c) is not None)

    # NOT gated: filters another command's output -- the Grep tool cannot.
    for c in ('git log --oneline | grep fix',
              'systemctl --user list-timers | grep overnight',
              'ls todo/*/ | grep -i log',
              'cat build.log | rg error'):
        check(f"allowed (piped): {c[:30]}", _leading_search(c) is None)

    # NOT gated: no path operand -> reads stdin.
    check("allowed: grep on stdin", _leading_search('grep -n "x"') is None)

    # NOT gated: unparseable -> decline to judge (33% of real traffic).
    check("allowed: unbalanced quote", _leading_search('grep -n "unclosed src/') is None)

    # THE REGEX TRAP: `\|` is a grep ALTERNATION inside a quoted pattern, not a
    # pipe. A regex matcher reads this as piped and wrongly allows it; the
    # tokenizer sees a leading grep and gates it.
    check("alternation is not a pipe",
          _leading_search(r'grep -n "usage\|failures" src/kernel/a.c') is not None)

    # The todo/ special case names the graph query.
    r = _leading_search('grep -rn "XREF" todo/02-kernel-core/')
    check("todo target flagged", r is not None and r[2] is True)
    r2 = _leading_search('grep -rn "kmalloc" src/kernel/mm/heap.c')
    check("non-todo not flagged", r2 is not None and r2[2] is False)

    if fails:
        sys.stderr.write("search_offload_gate selftest FAIL: "
                         + "; ".join(sorted(set(fails))) + "\n")
        return 1
    print("search_offload_gate selftest OK")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(_selftest() if "--selftest" in sys.argv else main())
    except Exception:
        sys.exit(0)   # fail-open: a broken search gate must never wedge a run
