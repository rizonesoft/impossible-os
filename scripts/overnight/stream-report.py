#!/usr/bin/env python3
"""Format `claude -p --output-format stream-json --verbose` output for run logs.

Reads stream-json lines on stdin and writes one timestamped line per event:
assistant text blocks (when the message completes), tool-use events as
"tool: <name>  <key argument>" (the command/file/skill/etc. that makes the line
self-explanatory), failed tool results as "tool error: <message>", and the
final result marked "=== final ===". Lines that are not parseable JSON pass
through unchanged. Stdlib only.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from datetime import datetime

# A Bash command that is really a code SEARCH (grep/rg/find/...). Split out so
# the report can prove whether an optimization actually cut main-loop search
# volume rather than just moving it into a subagent.
_SEARCH_RE = re.compile(
    r"(?:^|[|;&`]|\$\()\s*(?:rg|grep|egrep|fgrep|find|ag|ack|locate)\b")


def _git_head() -> str | None:
    """Current HEAD SHA, best-effort (None on any failure). Called only at a
    section flush (a handful of times per run), never per-turn."""
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"],
                           capture_output=True, text=True, timeout=5)
        out = r.stdout.strip()
        return out if r.returncode == 0 and out else None
    except Exception:
        return None


def _atomic_write(path: str, data: str) -> None:
    """Overwrite `path` atomically (temp + os.replace) so a crash mid-write can
    never leave a torn live snapshot. Best-effort; never raises into the pipe."""
    tmp = f"{path}.tmp.{os.getpid()}"
    try:
        with open(tmp, "w", encoding="ascii") as fh:
            fh.write(data)
        os.replace(tmp, path)
    except Exception:
        try:
            os.unlink(tmp)
        except Exception:
            pass


def _run_id_from_path(path: str | None) -> str | None:
    if not path:
        return None
    stem = os.path.basename(path)
    for suf in (".jsonl", ".json"):
        if stem.endswith(suf):
            stem = stem[: -len(suf)]
    return stem or None


def stamp() -> str:
    return datetime.now().strftime("%H:%M:%S")


# Terminal escape sequences captured from tool output (a tool error quoting the
# lint banner embedded its \\033[0;36m codes verbatim -- run-20260730-151719
# line 376). A viewer tailing across such a line inherits the colour until the
# next reset, i.e. "everything after this point is cyan". The report is a plain-
# text artifact; strip CSI/OSC sequences at the writer so no reader has to.
_ANSI_RE = re.compile(r"\x1b(?:\[[0-9;?]*[ -/]*[@-~]|\][^\x07\x1b]*(?:\x07|\x1b\\\\)?)")


def emit(text: str) -> None:
    for line in text.splitlines() or [""]:
        sys.stdout.write(f"{stamp()} {_ANSI_RE.sub('', line)}\n")
    sys.stdout.flush()


# Advisory (non-blocking) hook output worth putting in the run log. Keyed on
# the bracketed tag every such hook already prints, so a new advisory hook is
# picked up without touching this list as long as it follows the convention.
_ADVISORY_RE = re.compile(
    r"\[(?:sequencer|todo-wrap|build-offload|receipt-surface|cd-prefix|"
    r"inline-churn|slice-read|codex-[a-z-]+|[a-z-]*reminder)[^\]]*\]",
    re.I)


def _context_tokens(usage) -> int:
    """Tokens of context one API request carried: fresh input plus cache read
    plus cache creation (output excluded)."""
    if not isinstance(usage, dict):
        return 0
    return sum(v for k in ("input_tokens", "cache_read_input_tokens", "cache_creation_input_tokens")
               if isinstance(v := usage.get(k), int))


def _clip(value, limit: int = 200) -> str:
    """Collapse whitespace/newlines to one line and cap the length (ASCII only)."""
    s = " ".join(str(value).split())
    return s if len(s) <= limit else s[: limit - 3] + "..."


def _bash(inp: dict) -> str:
    # Flatten the FULL command before clipping (runner-kit 2026-07-03): the
    # old first-line-only render dropped heredoc commit messages and Codex
    # prompt bodies from the run log entirely.
    desc = (inp.get("description") or "").strip()
    cmd = " ".join((inp.get("command") or "").split())
    # 200 chars truncated mid-word on any compound command, so a verb in the
    # TAIL vanished from the log entirely -- `run_phase_guard.py rollover`
    # grepped as 0 hits on a segment that demonstrably rolled over, making
    # every log-based diagnostic quietly unreliable. The runner routinely
    # chains 3-5 statements; 600 covers them without turning the log into a
    # transcript.
    parts = []
    if desc:
        parts.append(desc)
    if cmd:
        parts.append(_clip(f"$ {cmd}", 500))
    return "  ".join(parts)


def _grep(inp: dict) -> str:
    pattern = inp.get("pattern") or ""
    path = inp.get("path")
    return f"{pattern}  in {path}" if path else str(pattern)


def _skill(inp: dict) -> str:
    name = inp.get("skill") or inp.get("command") or ""
    args = inp.get("args") or ""
    return f"{name} {args}".strip()


def _task(inp: dict) -> str:
    subtype = inp.get("subagent_type") or ""
    desc = inp.get("description") or inp.get("prompt") or ""
    label = f"{subtype}: {desc}" if subtype else str(desc)
    return label.strip(": ").strip()


def _seq_cursor():
    """Live sequencer cursor (TODO file + section index) for metrics
    attribution. The headless launcher does not export OVERNIGHT_TODO /
    OVERNIGHT_SECTION, which left every metrics record's todo/section null
    (Codex audit 2026-07-13, verified). Read the authoritative cursor from
    `.claude/state/sequencer-run.json` fresh at each flush (flushes are a
    handful per run, so re-reading is cheap). Fail-open to (None, None)."""
    base = os.environ.get("CLAUDE_PROJECT_DIR") or "."
    for root in (base, "."):
        try:
            with open(os.path.join(root, ".claude/state/sequencer-run.json"),
                      encoding="utf-8") as f:
                d = json.load(f)
            todo = d.get("file") or None
            sec = d.get("section_idx")
            src = d.get("section_source")
            return (todo, sec if isinstance(sec, int) else None,
                    src if isinstance(src, str) else None)
        except Exception:
            continue
    return (None, None, None)


def _resolve_session_transcript(session_id, cwd):
    """Locate the CLI session transcript for output reconciliation.

    The LIVE stream undercounts output_tokens ~140x (Codex audit 2026-07-13,
    root-caused this session against run-20260713-044429: recorded 2,019 vs a
    real 285,961). Reason: input/cache-read are known when the request is
    dispatched (so the streamed events carry them correctly), but the COMPLETED
    output_tokens never arrives in the streamed assistant events -- it lands
    only in the session transcript the CLI writes AFTER each message finishes.
    That transcript (one per session id, always persisted) is authoritative.

    Honors `OVERNIGHT_SESSION_TRANSCRIPT` (tests / non-standard homes). Returns
    an existing path or None (fail-open: no transcript -> keep stream values).
    """
    override = os.environ.get("OVERNIGHT_SESSION_TRANSCRIPT")
    if override:
        return override if os.path.exists(override) else None
    if not session_id:
        return None
    base = cwd or os.getcwd()
    slug = re.sub(r"[/.]", "-", base)
    home = os.environ.get("HOME") or os.path.expanduser("~")
    p = os.path.join(home, ".claude", "projects", slug, str(session_id) + ".jsonl")
    return p if os.path.exists(p) else None


def _transcript_output_maps(path):
    """message-id -> COMPLETE output_tokens from a session transcript, split
    into (main, sidechain). Each assistant message id is unique and its usage
    is final here, so max-per-id is exact. Returns (main, side) or None on a
    read error."""
    main: dict = {}
    side: dict = {}
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    d = json.loads(line)
                except Exception:
                    continue
                if d.get("type") != "assistant":
                    continue
                m = d.get("message") or {}
                mid = m.get("id")
                if not mid:
                    continue
                out = (m.get("usage") or {}).get("output_tokens")
                out = out if isinstance(out, int) else 0
                store = side if d.get("isSidechain") else main
                store[mid] = max(store.get(mid, 0), out)
    except OSError:
        return None
    return (main, side)


def _todos(inp: dict) -> str:
    todos = inp.get("todos")
    if not isinstance(todos, list):
        return ""
    done = sum(1 for t in todos if isinstance(t, dict) and t.get("status") == "completed")
    active = next(
        (t.get("content") for t in todos
         if isinstance(t, dict) and t.get("status") == "in_progress"),
        None,
    )
    head = f"{done}/{len(todos)} done"
    return f"{head}; now: {active}" if active else head


# name -> function extracting the most informative argument for the log line.
TOOL_ARG = {
    "Bash": _bash,
    "Edit": lambda i: i.get("file_path"),
    "MultiEdit": lambda i: i.get("file_path"),
    "Write": lambda i: i.get("file_path"),
    "Read": lambda i: i.get("file_path"),
    "NotebookEdit": lambda i: i.get("notebook_path"),
    "Glob": lambda i: i.get("pattern"),
    "Grep": _grep,
    "Skill": _skill,
    "Task": _task,
    "Agent": _task,
    "WebFetch": lambda i: i.get("url"),
    "WebSearch": lambda i: i.get("query"),
    "ToolSearch": lambda i: i.get("query"),
    "TodoWrite": _todos,
}

# generic fallback: first informative scalar for tools without a dedicated rule.
_GENERIC_KEYS = ("file_path", "path", "url", "query", "pattern", "command", "name")


def _generic(inp: dict) -> str:
    for key in _GENERIC_KEYS:
        value = inp.get(key)
        if isinstance(value, str) and value.strip():
            return value
    return ""


def summarize_tool(name: str, inp) -> str:
    if not isinstance(inp, dict):
        return ""
    fn = TOOL_ARG.get(name)
    summary = ""
    if fn is not None:
        try:
            summary = fn(inp) or ""
        except Exception:
            summary = ""
    if not summary:
        summary = _generic(inp)
    return _clip(summary) if summary else ""


def _result_text(content) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = [
            b.get("text", "")
            for b in content
            if isinstance(b, dict) and b.get("type") == "text"
        ]
        return " ".join(p for p in parts if p)
    return ""


def _content_text(content) -> str:
    """Extract assistant text from Claude or Codex-ish content shapes."""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = []
        for block in content:
            if not isinstance(block, dict):
                continue
            text = (
                block.get("text")
                or block.get("output_text")
                or block.get("content")
            )
            if isinstance(text, str) and text:
                parts.append(text)
        return "\n".join(parts)
    return ""


def _codex_command(event: dict) -> str:
    for key in ("cmd", "command", "shell_command"):
        value = event.get(key)
        if isinstance(value, str) and value.strip():
            return value
    item = event.get("item")
    if isinstance(item, dict):
        for key in ("cmd", "command", "shell_command"):
            value = item.get(key)
            if isinstance(value, str) and value.strip():
                return value
    return ""


def _codex_message(event: dict) -> str:
    text = _content_text(event.get("content"))
    if text:
        return text
    message = event.get("message")
    if isinstance(message, str):
        return message
    if isinstance(message, dict):
        text = _content_text(message.get("content"))
        if text:
            return text
    item = event.get("item")
    if isinstance(item, dict):
        if item.get("type") in ("message", "assistant_message"):
            text = _content_text(item.get("content"))
            if text:
                return text
        if isinstance(item.get("message"), str):
            return item["message"]
    return ""


class SectionMetrics:
    """Accumulate per-section token + tool counts; flush JSONL at boundaries."""

    TOKEN_FIELDS = (
        "input_tokens", "output_tokens",
        "cache_read_input_tokens", "cache_creation_input_tokens",
    )

    def __init__(self, path: str | None) -> None:
        self.path = path
        # Live snapshot: the running (unflushed) section state, rewritten
        # atomically each turn so a crash mid-section does not lose the tail --
        # a final section with no progress/result marker used to vanish entirely.
        self.snapshot_path = os.environ.get("OVERNIGHT_METRICS_SNAPSHOT") or (
            (path + ".live") if path else None)
        # Attribution stamps carried on every record.
        self.run_id = os.environ.get("OVERNIGHT_RUN_ID") or _run_id_from_path(path)
        self.index = 0
        self.model_seen: str | None = None  # run-level, NOT reset per section
        # Context size of the session's FIRST main-loop turn: the fixed floor
        # (system prompt, CLAUDE.md, memory, tool schemas) every later turn
        # re-reads as cached input. Run-level, NOT reset per section.
        self.context_floor: int | None = None
        # Session identity (from the stream) so finalization can reconcile the
        # undercounted stream output against the authoritative session transcript.
        self.session_id: str | None = os.environ.get("OVERNIGHT_SESSION_ID") or None
        self.cwd: str | None = None
        # Run-level record of which message ids landed in which section, so the
        # transcript reconciliation attributes each id's true output back to its
        # section WITHOUT any wall-clock/timezone bucketing. List of
        # (section_index, main_id_set, side_id_set).
        self._section_ids: list = []
        # Section boundary SHAs: a section starts at the prior flush's HEAD and
        # ends at the HEAD read when it flushes.
        self._start_sha: str | None = _git_head()
        self._last_head: str | None = self._start_sha
        self._reset()

    def _reset(self) -> None:
        # tokens/turns hold ONLY id-less (synthetic/test) usage, summed as-is.
        self.turns = 0
        self.tokens = {k: 0 for k in self.TOKEN_FIELDS}
        self.sidechain_turns = 0
        self.sidechain_tokens = {k: 0 for k in self.TOKEN_FIELDS}
        # Per-message-id best usage (main + sidechain). The CLI emits one stream
        # event PER CONTENT BLOCK, each repeating the message envelope; input +
        # cache-read are stable across them but output_tokens GROWS as the message
        # streams. First-seen dedupe therefore captured a PARTIAL output count
        # (the ~47x undercount: 12,553 reported vs ~588,840 real for one section).
        # Keep the MAX per field per id; turns = number of distinct ids.
        self._main_msg: dict = {}
        self._side_msg: dict = {}
        self.agent_dispatches = 0
        self.grep_calls = 0
        self.lsp_calls = 0
        # Per-origin tool attribution (main loop vs subagent sidechain), so a
        # report can prove WHICH loop an optimization moved work out of. Keys:
        # read, grep, glob, bash, bash_search, lsp, agent, edit, other.
        self.main_tools: dict = {}
        self.side_tools: dict = {}

    def note_session(self, session_id, cwd) -> None:
        """Latch the session id + cwd from the stream (system/result/assistant
        events all carry session_id) so finalization can find the transcript."""
        if session_id and not self.session_id:
            self.session_id = str(session_id)
        if cwd and not self.cwd:
            self.cwd = str(cwd)

    def note_model(self, model) -> None:
        """Log the ACTUAL model id from each MAIN-LOOP assistant turn's API
        response -- not just the --model flag the launcher was invoked with.
        Closes a real gap: overnight-launch.sh echoes its own CLI args at
        startup, but nothing confirmed which model actually produced the work,
        or caught a mid-run fallback-to-sonnet (or a wrong-default regression)
        silently. Caller must NOT feed sidechain (subagent) events here: the
        analyst fleet legitimately runs Sonnet, and its messages interleave
        with the main loop in the same stream.
        """
        if not isinstance(model, str) or not model:
            return
        if self.model_seen is None:
            self.model_seen = model
            expected = (os.environ.get("OVERNIGHT_MODEL") or "opus").lower()
            if expected != "inherit" and expected not in model.lower():
                emit(f"model confirmed: {model}  ** WARNING: expected '{expected}' "
                     f"per OVERNIGHT_MODEL/arm default -- check overnight-arm.sh model drop-in **")
            else:
                emit(f"model confirmed: {model}")
        elif model != self.model_seen:
            emit(f"model changed: {self.model_seen} -> {model} "
                 f"(fallback engaged mid-run, or an override took effect)")
            self.model_seen = model

    def add_usage(self, usage, msg_id=None, sidechain: bool = False) -> None:
        """Fold one stream event's usage into the section totals. Events sharing
        a message id are ONE API request: keep the MAX of each field for that id
        (output_tokens grows across the per-content-block events, so max = the
        final complete count; input/cache-read are stable). Id-less events
        (synthetic/test) sum individually. Turns = number of distinct ids +
        id-less events. Every call refreshes the atomic live snapshot."""
        if not isinstance(usage, dict):
            return
        if msg_id and not sidechain and self.context_floor is None:
            ctx = _context_tokens(usage)
            if ctx:
                self.context_floor = ctx
        if msg_id:
            store = self._side_msg if sidechain else self._main_msg
            best = store.get(msg_id)
            if best is None:
                best = {}
                store[msg_id] = best
            for k in self.TOKEN_FIELDS:
                v = usage.get(k)
                if isinstance(v, int):
                    best[k] = max(best.get(k, 0), v)
        else:
            bucket = self.sidechain_tokens if sidechain else self.tokens
            if sidechain:
                self.sidechain_turns += 1
            else:
                self.turns += 1
            for k in self.TOKEN_FIELDS:
                v = usage.get(k)
                if isinstance(v, int):
                    bucket[k] += v
        self.snapshot()

    def add_tool(self, name: str, inp=None, sidechain: bool = False) -> None:
        # Legacy aggregate counters (main + sidechain combined) kept for
        # back-compat with metrics-report / existing dashboards.
        if name in ("Task", "Agent"):
            self.agent_dispatches += 1
        elif name == "Grep":
            self.grep_calls += 1
        elif name.startswith("mcp__lsp-bridge__"):
            self.lsp_calls += 1
        # Per-origin split.
        if name in ("Task", "Agent"):
            key = "agent"
        elif name == "Read":
            key = "read"
        elif name == "Grep":
            key = "grep"
        elif name == "Glob":
            key = "glob"
        elif name in ("Edit", "Write", "MultiEdit"):
            key = "edit"
        elif name.startswith(("mcp__lsp-bridge__", "mcp__lsp")):
            key = "lsp"
        elif name == "Bash":
            cmd = (inp or {}).get("command", "") if isinstance(inp, dict) else ""
            key = "bash_search" if _SEARCH_RE.search(cmd or "") else "bash"
        else:
            key = "other"
        bucket = self.side_tools if sidechain else self.main_tools
        bucket[key] = bucket.get(key, 0) + 1

    def _totals(self):
        """(main_tokens, side_tokens, main_turns, side_turns), folding the
        per-message-id maxima into the id-less sums."""
        main = dict(self.tokens)
        side = dict(self.sidechain_tokens)
        for u in self._main_msg.values():
            for k in self.TOKEN_FIELDS:
                main[k] += u.get(k, 0)
        for u in self._side_msg.values():
            for k in self.TOKEN_FIELDS:
                side[k] += u.get(k, 0)
        return (main, side,
                self.turns + len(self._main_msg),
                self.sidechain_turns + len(self._side_msg))

    def _context_stats(self) -> dict:
        """Per-turn CONTEXT size (input + cache read + cache creation) of the
        section's main-loop turns. Cache reads dominate run cost (2026-09-28:
        1,981M cached-read vs 4.5M output tokens over 30 sections), and how far
        the context grows before a segment rotates is the lever; these fields
        make that tunable from data. `resident_share` is the fraction of the
        section's context re-reads that were the fixed session floor."""
        ctxs = sorted(c for c in (_context_tokens(u) for u in self._main_msg.values()) if c)
        if not ctxs:
            return {"context_floor_tokens": self.context_floor}

        def pct(q):
            return ctxs[min(len(ctxs) - 1, int(q * len(ctxs)))]
        total = sum(ctxs)
        floor = self.context_floor
        return {
            "context_floor_tokens": floor,
            "context_min_tokens": ctxs[0],
            "context_p50_tokens": pct(0.5),
            "context_p90_tokens": pct(0.9),
            "context_max_tokens": ctxs[-1],
            "context_mean_tokens": total // len(ctxs),
            "resident_share": round(min(1.0, floor * len(ctxs) / total), 4) if floor else None,
        }

    def _pending(self) -> bool:
        return bool(self.turns or self.sidechain_turns or self._main_msg
                    or self._side_msg or self.main_tools or self.side_tools)

    def _record(self, marker: str, end_sha) -> dict:
        main, side, turns, side_turns = self._totals()
        _seq_todo, _seq_section, _seq_src = _seq_cursor()
        return {
            "run_id": self.run_id,
            "section_index": self.index,
            "marker": marker,
            "timestamp": stamp(),
            "todo": os.environ.get("OVERNIGHT_TODO") or _seq_todo or None,
            "section": (os.environ.get("OVERNIGHT_SECTION")
                        if os.environ.get("OVERNIGHT_SECTION")
                        else _seq_section),
            # Provenance of `section`, so the split-predictor calibration can
            # drop rows it cannot trust: explicit | derived | stale | unset,
            # or `env` when OVERNIGHT_SECTION pinned it. `stale` means the
            # triage oracle was unavailable at the cursor move and the number
            # was carried over from the previous section -- plausible, but not
            # measured, and averaging it in silently would bias the fit.
            "section_source": ("env" if os.environ.get("OVERNIGHT_SECTION")
                               else _seq_src),
            "start_sha": self._start_sha,
            "end_sha": end_sha,
            "turns": turns,
            **main,
            "sidechain_turns": side_turns,
            **{f"sidechain_{k}": v for k, v in side.items()},
            **self._context_stats(),
            "agent_dispatches": self.agent_dispatches,
            "grep_calls": self.grep_calls,
            "lsp_calls": self.lsp_calls,
            "main_tools": self.main_tools,
            "sidechain_tools": self.side_tools,
            "model": self.model_seen,
            "effort": os.environ.get("OVERNIGHT_EFFORT") or None,
        }

    def snapshot(self) -> None:
        """Rewrite the live (unflushed) section snapshot atomically. Cheap:
        reuses the last-known HEAD, no git call per turn."""
        if not self.snapshot_path:
            return
        _atomic_write(self.snapshot_path,
                      json.dumps(self._record("live", self._last_head)) + "\n")

    def flush(self, marker: str) -> None:
        if not self.path:
            return
        # An EOF/error flush must be a no-op when the prior boundary already
        # drained the section (clean 'final' then reset); progress/final always
        # write (explicit boundaries). This is what recovers a trailing section
        # that ended with no progress/result marker instead of losing it.
        if marker == "eof" and not self._pending():
            return
        end_sha = _git_head()
        self._last_head = end_sha
        rec = self._record(marker, end_sha)
        with open(self.path, "a", encoding="ascii") as fh:
            fh.write(json.dumps(rec) + "\n")
        # Remember this section's message ids (keyed to the index just written)
        # for transcript reconciliation at finalization -- captured BEFORE the
        # reset clears the per-id stores.
        self._section_ids.append(
            (self.index, set(self._main_msg), set(self._side_msg)))
        self.index += 1
        self._start_sha = end_sha
        self._reset()

    def reconcile_from_transcript(self) -> None:
        """Rewrite each flushed record's output_tokens (main + sidechain) from
        the authoritative session transcript, attributing every message id to
        the section that streamed it. input/cache-read stay as the stream
        reported them (they are correct mid-flight); only output was undercounted.
        Fail-open: no transcript, no matching ids, or any error -> leave stream
        values untouched. Stamps `output_source` so a consumer can tell which
        records are transcript-exact vs stream-approx."""
        if not self.path:
            return
        tpath = _resolve_session_transcript(self.session_id, self.cwd)
        if not tpath:
            return
        maps = _transcript_output_maps(tpath)
        if not maps:
            return
        main_map, side_map = maps
        if not main_map and not side_map:
            return
        ids_by_index = {i: (mi, si) for (i, mi, si) in self._section_ids}
        try:
            with open(self.path, encoding="utf-8") as fh:
                recs = [json.loads(l) for l in fh if l.strip()]
        except OSError:
            return
        changed = False
        for r in recs:
            mi, si = ids_by_index.get(r.get("section_index"), (set(), set()))
            main_hit = bool(mi) and any(x in main_map for x in mi)
            if main_hit:
                r["output_tokens"] = sum(main_map.get(x, 0) for x in mi)
                changed = True
            if si and any(x in side_map for x in si):
                r["sidechain_output_tokens"] = sum(side_map.get(x, 0) for x in si)
                changed = True
            r["output_source"] = "transcript" if main_hit else "stream-approx"
        if changed:
            _atomic_write(self.path,
                          "".join(json.dumps(r) + "\n" for r in recs))

    def cleanup_snapshot(self) -> None:
        """Remove the `.live` snapshot once the run has finalized cleanly, so a
        finished run does not leave a stale orphan (7 such duplicates were found
        2026-07-13). A crash path deliberately SKIPS this so the aggregator can
        still ingest the orphan .live."""
        if self.snapshot_path:
            try:
                os.remove(self.snapshot_path)
            except OSError:
                pass


def handle(event: dict, metrics: "SectionMetrics") -> None:
    kind = event.get("type")
    # Every stream-json event (system/init, assistant, user, result) carries
    # session_id + cwd; latch them for transcript reconciliation at finalization.
    if event.get("session_id"):
        metrics.note_session(event.get("session_id"), event.get("cwd"))
    if kind == "assistant":
        message = event.get("message") or {}
        # parent_tool_use_id marks a subagent sidechain event; those carry the
        # SUBAGENT's model (Sonnet analyst fleet), so only main-loop events
        # (parent_tool_use_id absent/null) witness a genuine fallback flip.
        sidechain = bool(event.get("parent_tool_use_id"))
        if not sidechain:
            metrics.note_model(message.get("model"))
        metrics.add_usage(message.get("usage"), msg_id=message.get("id"),
                          sidechain=sidechain)
        for block in message.get("content") or []:
            if block.get("type") == "text" and block.get("text"):
                emit(block["text"])
            elif block.get("type") == "tool_use":
                name = block.get("name", "unknown")
                metrics.add_tool(name, block.get("input"), sidechain=sidechain)
                summary = summarize_tool(name, block.get("input"))
                emit(f"tool: {name}  {summary}".rstrip())
                if name == "Bash":
                    cmd = (block.get("input") or {}).get("command") or ""
                    if "run_phase_guard.py progress" in cmd:
                        metrics.flush("progress")
    elif kind == "user":
        # surface failed tool results so overnight logs flag errors inline.
        for block in (event.get("message") or {}).get("content") or []:
            if not isinstance(block, dict):
                continue
            if block.get("type") == "tool_result" and block.get("is_error"):
                msg = _result_text(block.get("content"))
                emit(f"tool error: {_clip(msg)}" if msg else "tool error")
                continue
            # ADVISORY HOOK OUTPUT (2026-07-31). Only is_error results were
            # surfaced, so a hook that emits a systemMessage instead of blocking
            # was INVISIBLE in the run log. That cost a full forensic pass: the
            # context-rotation hint fired at event 201/201 of segment
            # run-20260731-000502 and `grep -c "context-rotation hint"` returned
            # 0, which read as "the mechanism is dead" when it had actually
            # worked. An advisory gate you cannot see is one you cannot tune.
            txt = _result_text(block.get("content")) if block.get(
                "type") == "tool_result" else ""
            if txt and _ADVISORY_RE.search(txt):
                emit(f"hook: {_clip(txt, 400)}")
    elif kind == "attachment":
        # HOOK ATTACHMENTS (2026-07-31). A PostToolUse hook's systemMessage does
        # NOT arrive as a tool_result -- it arrives as its own top-level event,
        # `{"type": "attachment", "attachment": {"type": "hook_success",
        # "hookEvent": "PostToolUse", "stdout": "{\"systemMessage\": ...}"}}`.
        # stream-report handled no such kind, so it was dropped entirely.
        #
        # This is the SECOND half of the v05 observability defect, and v05
        # believed it had fixed the whole thing: the branch above surfaces
        # advisory output carried on a tool_result (the PreToolUse shape) and
        # nothing else. MEASURED CONSEQUENCE: the context-rotation hint fired on
        # schedule at event 90 and was delivered to the run 4 times, while
        # `grep -c "context-rotation hint"` over every segment of 2026-07-31
        # returned 0 -- which read as "the mechanism is dead" for a second
        # consecutive canary. An advisory gate you cannot see is one you cannot
        # tune, and the cost of not seeing this one is the rotation's entire
        # unrealised 31-41% cache-read saving.
        att = event.get("attachment")
        if isinstance(att, dict):
            atype = att.get("type")
            name = str(att.get("hookName") or att.get("hookEvent") or "hook")
            if atype == "hook_system_message":
                # THE canonical advisory channel. Measured on one segment of
                # run-20260731-155656: 113 `hook_system_message` events against
                # 119 `hook_success`, carrying the rendered text for EVERY
                # advisory hook (cd-prefix, inline-churn, verify-cadence,
                # kernel-code-quality, artifact-pipe, todo-graph, and the
                # context-rotation hint). Keying on hook_success.stdout instead
                # would surface only hooks that happen to print JSON.
                txt = att.get("content")
                if isinstance(txt, str) and txt.strip():
                    emit(f"hook: [{name}] {_clip(txt, 400)}")
            elif atype == "hook_error":
                err = att.get("stderr") or att.get("content")
                if isinstance(err, str) and err.strip():
                    emit(f"hook error: [{name}] {_clip(err, 400)}")
            # hook_success is deliberately NOT surfaced: its stdout is the raw
            # `{"systemMessage": ...}` JSON of the SAME advisory the
            # hook_system_message twin renders, so emitting both double-logs
            # every hint. Verified on the rotation hint: 2 firings produced 2
            # of each. Do not "fix" this into an extra branch.
    elif kind == "result":
        metrics.flush("final")
        emit("=== final ===")
        result = event.get("result")
        if isinstance(result, str) and result:
            emit(result)
    elif isinstance(kind, str) and kind in (
        "message", "agent_message", "assistant_message", "item.completed",
    ):
        text = _codex_message(event)
        if text:
            emit(text)
    elif isinstance(kind, str) and (
        kind.endswith(".started")
        or kind.endswith(".completed")
        or "exec" in kind
        or "command" in kind
    ):
        cmd = _codex_command(event)
        if cmd:
            label = "Bash"
            if "completed" in kind:
                code = event.get("exit_code")
                suffix = f"  exit={code}" if code is not None else ""
                emit(f"tool: {label}  {_clip(cmd)}{suffix}")
            else:
                emit(f"tool: {label}  {_clip(cmd)}")
    elif isinstance(kind, str) and "error" in kind:
        emit(f"tool error: {_clip(_codex_message(event) or event)}")
    elif os.environ.get("OVERNIGHT_STREAM_KEEP_UNKNOWN") == "1":
        emit("event: " + _clip(json.dumps(event, sort_keys=True), 500))


def main() -> int:
    metrics = SectionMetrics(os.environ.get("OVERNIGHT_METRICS_FILE") or None)
    ok = False
    try:
        for raw in sys.stdin:
            line = raw.rstrip("\n")
            if not line.strip():
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                sys.stdout.write(raw if raw.endswith("\n") else raw + "\n")
                sys.stdout.flush()
                continue
            if isinstance(event, dict):
                handle(event, metrics)
        ok = True
    finally:
        # Capture a trailing section that ended with no progress/result marker
        # (stdin EOF, a kill, or an exception) instead of dropping its tail.
        metrics.flush("eof")
        # On a CLEAN finish only: reconcile the undercounted stream output
        # against the authoritative session transcript, then drop the now-stale
        # `.live` orphan. A crash (ok=False) skips both so the orphan survives
        # for the aggregator to ingest.
        if ok:
            try:
                metrics.reconcile_from_transcript()
            except Exception:
                pass
            metrics.cleanup_snapshot()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
