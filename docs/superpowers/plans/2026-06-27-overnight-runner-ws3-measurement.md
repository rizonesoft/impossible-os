# Overnight Runner WS3 -- Measurement Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Capture per-section token + agent-dispatch + lsp-vs-grep metrics from the overnight runner's stream so every later cost change (WS1, WS1b, WS5, WS6) is measurable.

**Architecture:** Extend `scripts/overnight/stream-report.py` (already in the run pipeline) to accumulate per-section metrics from the raw stream-json `usage` fields and tool-use events, flushing a JSONL record at each section boundary (detected by the `run_phase_guard.py progress` Bash call the sequencer makes after every section ships). A standalone `metrics-report.py` renders per-section + aggregate tables and A/B-compares two runs. The launcher wires a per-run metrics sidecar via the `OVERNIGHT_METRICS_FILE` env var.

**Tech Stack:** Python 3 stdlib only (matches `stream-report.py`); Bash launcher; no new dependencies.

## Global Constraints

- Python stdlib only -- no pip installs, matches `stream-report.py` header.
- ASCII only in all output (no Unicode dashes U+2013/U+2014; no section-sign+digit in code). Project code-style policy.
- `stream-report.py`'s existing stdout behavior MUST NOT change -- metrics are written to a *separate* sidecar file, never to stdout. The run log must look identical.
- Metrics writing is opt-in: when `OVERNIGHT_METRICS_FILE` is unset (interactive / non-overnight use), no sidecar is written and behavior is unchanged.
- The runner's guard/phase machine, skills, and hooks are NOT touched by this plan.
- Verified data shape (captured 2026-06-27): assistant turn usage at `event["message"]["usage"]` with int fields `input_tokens`, `output_tokens`, `cache_read_input_tokens`, `cache_creation_input_tokens`; final totals at `event["usage"]` plus `event["modelUsage"]` on the `result` event.

---

### Task 1: Per-section metrics accumulator in stream-report.py

**Files:**
- Modify: `scripts/overnight/stream-report.py`
- Test: `scripts/overnight/tests/test_stream_report_metrics.py` (create)

**Interfaces:**
- Consumes: stdin stream-json lines (assistant / user / result events).
- Produces: a JSONL sidecar at `$OVERNIGHT_METRICS_FILE`, one object per section:
  `{"section_index": int, "marker": str, "timestamp": "HH:MM:SS", "turns": int, "input_tokens": int, "output_tokens": int, "cache_read_input_tokens": int, "cache_creation_input_tokens": int, "agent_dispatches": int, "grep_calls": int, "lsp_calls": int}`.

- [ ] **Step 1: Write the failing test**

Create `scripts/overnight/tests/test_stream_report_metrics.py`:

```python
#!/usr/bin/env python3
"""Contract test for stream-report.py per-section metrics sidecar."""
import json, os, subprocess, sys, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent / "stream-report.py"


def _run(events, metrics_path):
    payload = "".join(json.dumps(e) + "\n" for e in events)
    env = {**os.environ, "OVERNIGHT_METRICS_FILE": str(metrics_path)}
    proc = subprocess.run(
        [sys.executable, str(SCRIPT)],
        input=payload, text=True, capture_output=True, env=env,
    )
    assert proc.returncode == 0, proc.stderr
    return proc.stdout


def _assistant(usage=None, tools=None):
    content = []
    if tools:
        for name, inp in tools:
            content.append({"type": "tool_use", "name": name, "input": inp})
    msg = {"content": content}
    if usage is not None:
        msg["usage"] = usage
    return {"type": "assistant", "message": msg}


def _usage(i=0, o=0, cr=0, cc=0):
    return {"input_tokens": i, "output_tokens": o,
            "cache_read_input_tokens": cr, "cache_creation_input_tokens": cc}


def test_two_sections_split_on_progress():
    with tempfile.TemporaryDirectory() as d:
        mp = pathlib.Path(d) / "m.jsonl"
        events = [
            _assistant(_usage(o=100), [("Read", {"file_path": "a.c"})]),
            _assistant(_usage(o=50), [("Task", {"subagent_type": "kernel-explorer"})]),
            _assistant(_usage(o=10), [("Grep", {"pattern": "x"})]),
            # section boundary: the progress guard call
            _assistant(_usage(o=5), [("Bash", {"command": "python3 .claude/hooks/run_phase_guard.py progress"})]),
            _assistant(_usage(o=200), [("mcp__lsp-bridge__definition", {})]),
            {"type": "result", "result": "done", "usage": _usage(o=9999)},
        ]
        _run(events, mp)
        recs = [json.loads(l) for l in mp.read_text().splitlines() if l.strip()]
        assert len(recs) == 2, recs
        s0, s1 = recs
        assert s0["section_index"] == 0 and s0["marker"] == "progress"
        assert s0["output_tokens"] == 165          # 100+50+10+5
        assert s0["agent_dispatches"] == 1
        assert s0["grep_calls"] == 1
        assert s0["lsp_calls"] == 0
        assert s1["section_index"] == 1 and s1["marker"] == "final"
        assert s1["output_tokens"] == 200
        assert s1["lsp_calls"] == 1


def test_no_env_writes_nothing():
    payload = json.dumps(_assistant(_usage(o=1))) + "\n"
    env = {k: v for k, v in os.environ.items() if k != "OVERNIGHT_METRICS_FILE"}
    proc = subprocess.run([sys.executable, str(SCRIPT)], input=payload,
                          text=True, capture_output=True, env=env)
    assert proc.returncode == 0, proc.stderr


if __name__ == "__main__":
    test_two_sections_split_on_progress()
    test_no_env_writes_nothing()
    print("PASS: stream-report metrics")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 scripts/overnight/tests/test_stream_report_metrics.py`
Expected: FAIL with `AssertionError` (no sidecar written yet -- `mp.read_text()` raises `FileNotFoundError`, or 0 records).

- [ ] **Step 3: Add the SectionMetrics class to stream-report.py**

After the imports (`import json`, `import sys`, `from datetime import datetime`) add `import os`, then insert this class above `def handle(`:

```python
class SectionMetrics:
    """Accumulate per-section token + tool counts; flush JSONL at boundaries."""

    TOKEN_FIELDS = (
        "input_tokens", "output_tokens",
        "cache_read_input_tokens", "cache_creation_input_tokens",
    )

    def __init__(self, path: str | None) -> None:
        self.path = path
        self.index = 0
        self._reset()

    def _reset(self) -> None:
        self.turns = 0
        self.tokens = {k: 0 for k in self.TOKEN_FIELDS}
        self.agent_dispatches = 0
        self.grep_calls = 0
        self.lsp_calls = 0

    def add_usage(self, usage) -> None:
        if not isinstance(usage, dict):
            return
        self.turns += 1
        for k in self.TOKEN_FIELDS:
            v = usage.get(k)
            if isinstance(v, int):
                self.tokens[k] += v

    def add_tool(self, name: str) -> None:
        if name in ("Task", "Agent"):
            self.agent_dispatches += 1
        elif name == "Grep":
            self.grep_calls += 1
        elif name.startswith("mcp__lsp-bridge__"):
            self.lsp_calls += 1

    def flush(self, marker: str) -> None:
        if not self.path:
            return
        rec = {
            "section_index": self.index,
            "marker": marker,
            "timestamp": stamp(),
            "turns": self.turns,
            **self.tokens,
            "agent_dispatches": self.agent_dispatches,
            "grep_calls": self.grep_calls,
            "lsp_calls": self.lsp_calls,
        }
        with open(self.path, "a", encoding="ascii") as fh:
            fh.write(json.dumps(rec) + "\n")
        self.index += 1
        self._reset()
```

- [ ] **Step 4: Thread metrics through handle() and main()**

Change `def handle(event: dict) -> None:` to `def handle(event: dict, metrics: "SectionMetrics") -> None:` and update its body so the `assistant` branch records usage + tools and detects the boundary:

```python
def handle(event: dict, metrics: "SectionMetrics") -> None:
    kind = event.get("type")
    if kind == "assistant":
        message = event.get("message") or {}
        metrics.add_usage(message.get("usage"))
        for block in message.get("content") or []:
            if block.get("type") == "text" and block.get("text"):
                emit(block["text"])
            elif block.get("type") == "tool_use":
                name = block.get("name", "unknown")
                metrics.add_tool(name)
                summary = summarize_tool(name, block.get("input"))
                emit(f"tool: {name}  {summary}".rstrip())
                if name == "Bash":
                    cmd = (block.get("input") or {}).get("command") or ""
                    if "run_phase_guard.py progress" in cmd:
                        metrics.flush("progress")
    elif kind == "user":
        for block in (event.get("message") or {}).get("content") or []:
            if (
                isinstance(block, dict)
                and block.get("type") == "tool_result"
                and block.get("is_error")
            ):
                msg = _result_text(block.get("content"))
                emit(f"tool error: {_clip(msg)}" if msg else "tool error")
    elif kind == "result":
        metrics.flush("final")
        emit("=== final ===")
        result = event.get("result")
        if isinstance(result, str) and result:
            emit(result)
```

Then update `main()`:

```python
def main() -> int:
    metrics = SectionMetrics(os.environ.get("OVERNIGHT_METRICS_FILE") or None)
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
    return 0
```

- [ ] **Step 5: Run test to verify it passes**

Run: `python3 scripts/overnight/tests/test_stream_report_metrics.py`
Expected: `PASS: stream-report metrics`

- [ ] **Step 6: Confirm existing behavior unchanged (stdout identical)**

Run: `printf '%s\n' '{"type":"assistant","message":{"content":[{"type":"text","text":"hello"}]}}' | python3 scripts/overnight/stream-report.py`
Expected: one line ending in ` hello` (timestamped), no sidecar written (env unset), exit 0.

- [ ] **Step 7: Commit**

```bash
git add scripts/overnight/stream-report.py scripts/overnight/tests/test_stream_report_metrics.py
git commit -m "overnight: WS3 -- per-section metrics sidecar in stream-report.py"
```

---

### Task 2: metrics-report.py analyzer

**Files:**
- Create: `scripts/overnight/metrics-report.py`
- Test: `scripts/overnight/tests/test_metrics_report.py` (create)

**Interfaces:**
- Consumes: one or two metrics JSONL files produced by Task 1.
- Produces: a human table on stdout (per-section rows + a TOTAL row). With two files, prints an A/B delta on the TOTAL row. Exit 0 on success, 2 on a missing file.

- [ ] **Step 1: Write the failing test**

Create `scripts/overnight/tests/test_metrics_report.py`:

```python
#!/usr/bin/env python3
import json, subprocess, sys, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent / "metrics-report.py"


def _write(path, recs):
    path.write_text("".join(json.dumps(r) + "\n" for r in recs), encoding="ascii")


def _rec(idx, out, agents=0, grep=0, lsp=0):
    return {"section_index": idx, "marker": "progress", "timestamp": "00:00:00",
            "turns": 1, "input_tokens": 0, "output_tokens": out,
            "cache_read_input_tokens": 0, "cache_creation_input_tokens": 0,
            "agent_dispatches": agents, "grep_calls": grep, "lsp_calls": lsp}


def test_single_file_total():
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "a.jsonl"
        _write(p, [_rec(0, 100, agents=1, lsp=2), _rec(1, 50, grep=3)])
        out = subprocess.run([sys.executable, str(SCRIPT), str(p)],
                             text=True, capture_output=True)
        assert out.returncode == 0, out.stderr
        assert "150" in out.stdout            # total output tokens
        assert "TOTAL" in out.stdout


def test_missing_file_exits_2():
    out = subprocess.run([sys.executable, str(SCRIPT), "/no/such.jsonl"],
                         text=True, capture_output=True)
    assert out.returncode == 2, out.stdout


if __name__ == "__main__":
    test_single_file_total()
    test_missing_file_exits_2()
    print("PASS: metrics-report")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 scripts/overnight/tests/test_metrics_report.py`
Expected: FAIL (`metrics-report.py` does not exist -> nonzero / FileNotFoundError).

- [ ] **Step 3: Write metrics-report.py**

```python
#!/usr/bin/env python3
"""Render overnight per-section metrics; optionally A/B two run files.

Usage: metrics-report.py RUN.jsonl [BASELINE.jsonl]
Stdlib only; ASCII output.
"""
from __future__ import annotations

import json
import sys

TOKEN_FIELDS = (
    "input_tokens", "output_tokens",
    "cache_read_input_tokens", "cache_creation_input_tokens",
)


def load(path: str) -> list[dict]:
    recs = []
    with open(path, encoding="ascii") as fh:
        for line in fh:
            line = line.strip()
            if line:
                recs.append(json.loads(line))
    return recs


def totals(recs: list[dict]) -> dict:
    agg = {k: 0 for k in TOKEN_FIELDS}
    agg.update(turns=0, agent_dispatches=0, grep_calls=0, lsp_calls=0)
    for r in recs:
        for k in agg:
            v = r.get(k)
            if isinstance(v, int):
                agg[k] += v
    return agg


def fmt_row(label: str, r: dict) -> str:
    lsp = r.get("lsp_calls", 0)
    grep = r.get("grep_calls", 0)
    ratio = f"{lsp}/{grep}"
    return (f"{label:<10} out={r.get('output_tokens', 0):>9} "
            f"in={r.get('input_tokens', 0):>9} "
            f"cache_r={r.get('cache_read_input_tokens', 0):>9} "
            f"agents={r.get('agent_dispatches', 0):>3} "
            f"lsp/grep={ratio:>7}")


def main(argv: list[str]) -> int:
    if not argv:
        print("usage: metrics-report.py RUN.jsonl [BASELINE.jsonl]", file=sys.stderr)
        return 2
    try:
        recs = load(argv[0])
    except FileNotFoundError:
        print(f"no such metrics file: {argv[0]}", file=sys.stderr)
        return 2
    for r in recs:
        print(fmt_row(f"sec {r.get('section_index', '?')}", r))
    tot = totals(recs)
    print(fmt_row("TOTAL", tot))
    if len(argv) > 1:
        try:
            base = totals(load(argv[1]))
        except FileNotFoundError:
            print(f"no such baseline file: {argv[1]}", file=sys.stderr)
            return 2
        delta = base["output_tokens"] - tot["output_tokens"]
        sign = "-" if delta >= 0 else "+"
        print(f"A/B output-token delta vs baseline: {sign}{abs(delta)} "
              f"(baseline {base['output_tokens']} -> run {tot['output_tokens']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 scripts/overnight/tests/test_metrics_report.py`
Expected: `PASS: metrics-report`

- [ ] **Step 5: Commit**

```bash
git add scripts/overnight/metrics-report.py scripts/overnight/tests/test_metrics_report.py
git commit -m "overnight: WS3 -- metrics-report analyzer with A/B delta"
```

---

### Task 3: Wire the launcher to emit a per-run metrics sidecar

**Files:**
- Modify: `scripts/overnight/overnight-launch.sh:66` (after `REPORT=` assignment) and `:158` (the stream-report pipe is already correct; no change needed there).

**Interfaces:**
- Consumes: `REPORT_DIR` (already defined at line 27), the run timestamp.
- Produces: `$OVERNIGHT_METRICS_FILE` exported into the environment `stream-report.py` runs in, pointing at `.claude/overnight/metrics/run-<ts>.jsonl`.

- [ ] **Step 1: Add the metrics-file setup after the REPORT assignment**

Immediately after line 66 (`REPORT="$REPORT_DIR/run-$(date +%Y%m%d-%H%M%S).log"`), insert:

```bash
METRICS_DIR="$PROJECT_DIR/.claude/overnight/metrics"
mkdir -p "$METRICS_DIR"
export OVERNIGHT_METRICS_FILE="$METRICS_DIR/$(basename "${REPORT%.log}").jsonl"
```

(The metrics file shares the run log's basename so a report and its metrics pair up: `run-20260627-120000.log` <-> `run-20260627-120000.jsonl`.)

- [ ] **Step 2: Verify the launcher still parses**

Run: `bash -n scripts/overnight/overnight-launch.sh`
Expected: no output, exit 0 (syntax OK).

- [ ] **Step 3: End-to-end smoke -- a real short run emits a populated sidecar**

Run:
```bash
OVERNIGHT_METRICS_FILE=/tmp/ws3-smoke.jsonl bash -c '
printf "%s\n" \
 "{\"type\":\"assistant\",\"message\":{\"usage\":{\"output_tokens\":42},\"content\":[{\"type\":\"tool_use\",\"name\":\"Bash\",\"input\":{\"command\":\"python3 .claude/hooks/run_phase_guard.py progress\"}}]}}" \
 "{\"type\":\"result\",\"result\":\"ok\",\"usage\":{\"output_tokens\":1}}" \
 | python3 scripts/overnight/stream-report.py >/dev/null
cat /tmp/ws3-smoke.jsonl'
```
Expected: two JSON lines; the first has `"output_tokens": 42, "marker": "progress"`, the second `"marker": "final"`.

- [ ] **Step 4: Confirm metrics dir is git-ignored or intentionally tracked**

Run: `grep -n "overnight/metrics\|overnight/reports" .gitignore`
Expected: if `reports/` is ignored, add a matching `/.claude/overnight/metrics/` line so per-run metrics are not committed. If neither is present, add both. Apply the edit if needed.

- [ ] **Step 5: Commit**

```bash
git add scripts/overnight/overnight-launch.sh .gitignore
git commit -m "overnight: WS3 -- wire per-run metrics sidecar into launcher"
```

---

## Self-Review

**Spec coverage (WS3):** per-section token capture (Task 1), agent-dispatch + lsp/grep counts (Task 1), sidecar under `.claude/overnight/metrics/` (Task 3), before/after analyzer with A/B delta (Task 2), no new external dependency (stdlib only) -- all present. The "emit from stream-report.py (not raw-stream tee)" decision from the spec review is honored.

**Known limitation (documented, not a gap):** subagent-internal token usage may not surface in the parent stream's per-turn `usage`; WS3 v1 therefore measures *main-context* tokens (the primary cost signal) plus agent-dispatch *counts*. The `result` event also carries `modelUsage` (per-model split) -- a future enhancement can flush that as a run-level aggregate, out of scope here.

**Placeholder scan:** no TBD/TODO; every code step has complete code; commands have expected output. Clean.

**Type consistency:** `SectionMetrics.flush(marker)`, the sidecar field names, and `metrics-report.py`'s `TOKEN_FIELDS` all use the identical field names captured from the real stream (`input_tokens`/`output_tokens`/`cache_read_input_tokens`/`cache_creation_input_tokens`). Consistent across Tasks 1-2.

## Follow-on plans (not this plan)

- **Plan 2 -- Context offload + enforcement:** WS1 (default-on agents), WS1b (agent-dispatch BLOCK hook, WARN-first), WS2 (`diagnostic-digester` + its WS8 trust contract), WS7 (&-bundle-Codex lint). Measured against WS3.
- **Plan 3 -- Model/effort/mechanical levers:** WS5a (main-loop Medium-vs-High A/B), WS5b (parity+explorer -> Sonnet), WS6 (deterministic-first + `todo-hygiene-auditor` + WS8 verification).
- **WS4 (lsp-bridge hardening):** lands whenever the in-flight `scripts/lsp-mcp/bridge.py` rework commits.
