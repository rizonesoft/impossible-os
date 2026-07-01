# Fusion Ladder Plan 2 -- controller + async job queue + stuck-detect + wiring

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the (validated DIY) Fusion apex tier fire automatically -- a stuck-detect trigger, a ladder controller that dispatches Fusion as a DETACHED async job (defer the section, collect the durable result on a later pass), and the skill wiring that drives the Claude -> Codex -> Fusion (3 -> 2 -> 1) escalation.

**Architecture:** `.fusion/ladder.py` is the Fusion-tier async job manager (`dispatch`/`poll`/`list`/`_worker`/`outcome`): `dispatch` spawns a detached worker that runs `fusion_escalate.escalate(prior=...)` and writes a result file; the loop defers the section and collects on a later pass. `.claude/hooks/fusion_stuck_detect.py` counts consecutive same-target build/test/smoke failures and nudges the loop into the ladder at threshold. Skills wire the orchestration. All off-by-default, fail-open, overnight-scoped.

**Tech Stack:** Python 3 stdlib (`subprocess` detached, `json`); `.claude/settings.json` hook wiring; Markdown skill edits. No new deps.

## Global Constraints

- Python stdlib only; ASCII; no section-sign+digit in code.
- Off-by-default: `dispatch` does nothing unless `FUSION_ENABLED=1` + a secret (it just prints `disabled`/`no_key`).
- The detached worker SURVIVES the main loop (compaction/watchdog) -- `start_new_session=True`; result file persists.
- Fail-open: any error -> the job is marked `failed` and the runner continues; nothing blocks.
- Overnight-scoped: the stuck-detect hook gates on `OVERNIGHT_SEQUENCER_RUN` (silent interactively), like `agent_dispatch_required.py`.
- `.fusion/jobs/` is gitignored (already added).
- The caller is the DIY `fusion_escalate.escalate(..., prior=[{source,content}])` -- the ladder feeds Opus(Tier 0) + Codex(Tier 1) analyses via `prior`.

---

### Task 1: ladder.py -- async job queue + tier mechanism

**Files:**
- Create: `.fusion/ladder.py`
- Test: `scripts/overnight/tests/test_fusion_ladder.py` (create)

**Interfaces:**
- CLI: `dispatch --mode {stuck,review} --target T [--brief-file F] [--prior SRC=FILE]...` -> prints a job id (or `disabled`/`no_key`); `poll <id>` -> `PENDING` | `DONE\n<result>` | `FAILED <reason>`; `list` -> `<id> <status> <target>` lines; `_worker <id>` (internal); `outcome <id> <resolved|unresolved|unknown>`.
- Job files in `.fusion/jobs/`: `<id>.request.json`, `<id>.meta.json` `{id,target,mode,status,started_at,finished_at,cost,error}`, `<id>.result.txt`.

- [ ] **Step 1: Write the failing test**

Create `scripts/overnight/tests/test_fusion_ladder.py`:

```python
#!/usr/bin/env python3
import json, sys, tempfile, pathlib, importlib.util

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
spec = importlib.util.spec_from_file_location("ladder", REPO / ".fusion" / "ladder.py")
ladder = importlib.util.module_from_spec(spec); spec.loader.exec_module(ladder)


def _jobs(root):
    return pathlib.Path(root) / ".fusion" / "jobs"


def test_worker_runs_escalate_and_writes_result(monkeypatch=None):
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        jobs = _jobs(root); jobs.mkdir(parents=True)
        jid = "20260627-000000-abc123"
        (jobs / f"{jid}.request.json").write_text(json.dumps(
            {"mode": "stuck", "target": "build", "brief": "why crash", "prior": []}))
        (jobs / f"{jid}.meta.json").write_text(json.dumps(
            {"id": jid, "target": "build", "mode": "stuck", "status": "pending"}))
        # stub the caller so the worker does not hit the network
        ladder._load_caller = lambda root: type("C", (), {
            "escalate": staticmethod(lambda **kw: {"status": "ok", "output": "SYNTH", "cost": 0.5}),
            "_load_cfg": staticmethod(lambda r: {}), "_read_secret": staticmethod(lambda r: "x")})
        ladder.run_worker(root, jid)
        assert (jobs / f"{jid}.result.txt").read_text() == "SYNTH"
        meta = json.loads((jobs / f"{jid}.meta.json").read_text())
        assert meta["status"] == "done" and meta["cost"] == 0.5


def test_poll_reports_states():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d); jobs = _jobs(root); jobs.mkdir(parents=True)
        (jobs / "j1.meta.json").write_text(json.dumps({"id": "j1", "status": "pending", "target": "t"}))
        (jobs / "j2.meta.json").write_text(json.dumps({"id": "j2", "status": "done", "target": "t"}))
        (jobs / "j2.result.txt").write_text("ANSWER")
        assert ladder.poll(root, "j1") == "PENDING"
        assert ladder.poll(root, "j2").startswith("DONE") and "ANSWER" in ladder.poll(root, "j2")


def test_list_enumerates():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d); jobs = _jobs(root); jobs.mkdir(parents=True)
        (jobs / "j1.meta.json").write_text(json.dumps({"id": "j1", "status": "pending", "target": "build"}))
        out = ladder.list_jobs(root)
        assert "j1" in out and "pending" in out


if __name__ == "__main__":
    test_worker_runs_escalate_and_writes_result()
    test_poll_reports_states()
    test_list_enumerates()
    print("PASS: fusion_ladder")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 scripts/overnight/tests/test_fusion_ladder.py`
Expected: FAIL (`.fusion/ladder.py` does not exist).

- [ ] **Step 3: Write `.fusion/ladder.py`**

```python
#!/usr/bin/env python3
"""Fusion async job queue + tier mechanism (Plan 2). Dispatch a Fusion escalation as
a DETACHED job, defer the section, collect the durable result on a later pass.

CLI:
  dispatch --mode {stuck,review} --target T [--brief-file F] [--prior SRC=FILE]...
  poll <id> | list | outcome <id> <verdict> | _worker <id>
Off-by-default; fail-open. Stdlib only.
"""
from __future__ import annotations

import json
import os
import secrets
import subprocess
import sys
import time
from pathlib import Path


def repo_root() -> Path:
    here = Path(__file__).resolve()
    for p in here.parents:
        if (p / ".fusion").is_dir():
            return p
    return Path.cwd()


def _jobs(root: Path) -> Path:
    d = root / ".fusion" / "jobs"
    d.mkdir(parents=True, exist_ok=True)
    return d


def _read_json(p: Path):
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return None


def _write_json(p: Path, data: dict) -> None:
    p.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def _load_caller(root: Path):
    sys.path.insert(0, str(root / ".fusion"))
    import fusion_escalate as fe
    return fe


def run_worker(root: Path, jid: str) -> None:
    jobs = _jobs(root)
    meta_p = jobs / f"{jid}.meta.json"
    meta = _read_json(meta_p) or {"id": jid}
    req = _read_json(jobs / f"{jid}.request.json") or {}
    try:
        fe = _load_caller(root)
        res = fe.escalate(enabled=True, secret=fe._read_secret(root),
                          cfg=fe._load_cfg(root), mode=req.get("mode", "stuck"),
                          brief=req.get("brief", ""), root=root, prior=req.get("prior"))
        if res.get("status") in ("ok", "panel_only"):
            (jobs / f"{jid}.result.txt").write_text(res.get("output") or "", encoding="utf-8")
            meta.update(status="done", cost=res.get("cost"), per_model=res.get("panel"),
                        finished_at=time.strftime("%Y-%m-%dT%H:%M:%S"))
        else:
            meta.update(status="failed", error=res.get("status"),
                        finished_at=time.strftime("%Y-%m-%dT%H:%M:%S"))
    except Exception as e:
        meta.update(status="failed", error=str(e)[:200],
                    finished_at=time.strftime("%Y-%m-%dT%H:%M:%S"))
    _write_json(meta_p, meta)


def dispatch(root: Path, mode: str, target: str, brief: str, prior: list) -> str:
    fe = _load_caller(root)
    if os.environ.get("FUSION_ENABLED") != "1":
        return "disabled"
    if not fe._read_secret(root):
        return "no_key"
    jobs = _jobs(root)
    jid = time.strftime("%Y%m%d-%H%M%S") + "-" + secrets.token_hex(3)
    _write_json(jobs / f"{jid}.request.json",
                {"mode": mode, "target": target, "brief": brief, "prior": prior or []})
    _write_json(jobs / f"{jid}.meta.json",
                {"id": jid, "target": target, "mode": mode, "status": "pending",
                 "started_at": time.strftime("%Y-%m-%dT%H:%M:%S")})
    log = open(jobs / f"{jid}.worker.log", "ab")
    subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "_worker", jid],
                     cwd=str(root), stdout=log, stderr=log, start_new_session=True)
    return jid


def poll(root: Path, jid: str) -> str:
    meta = _read_json(_jobs(root) / f"{jid}.meta.json")
    if not meta:
        return f"FAILED no such job {jid}"
    st = meta.get("status")
    if st == "pending":
        return "PENDING"
    if st == "failed":
        return "FAILED " + str(meta.get("error", ""))
    res = (_jobs(root) / f"{jid}.result.txt")
    return "DONE\n" + (res.read_text(encoding="utf-8") if res.exists() else "")


def list_jobs(root: Path) -> str:
    out = []
    for m in sorted(_jobs(root).glob("*.meta.json")):
        meta = _read_json(m) or {}
        out.append(f"{meta.get('id', m.stem)} {meta.get('status', '?')} {meta.get('target', '')}")
    return "\n".join(out)


def outcome(root: Path, jid: str, verdict: str) -> None:
    jobs = _jobs(root)
    meta_p = jobs / f"{jid}.meta.json"
    meta = _read_json(meta_p) or {"id": jid}
    meta["outcome"] = verdict
    _write_json(meta_p, meta)
    try:
        with (root / ".fusion" / "dataset.jsonl").open("a", encoding="utf-8") as f:
            f.write(json.dumps({"tier": "fusion-outcome", "job": jid, "outcome": verdict,
                                "ts": time.strftime("%Y-%m-%dT%H:%M:%S")}) + "\n")
    except Exception:
        pass


def main(argv) -> int:
    root = repo_root()
    cmd = argv[0] if argv else "list"
    if cmd == "_worker":
        run_worker(root, argv[1]); return 0
    if cmd == "dispatch":
        mode = argv[argv.index("--mode") + 1] if "--mode" in argv else "stuck"
        target = argv[argv.index("--target") + 1] if "--target" in argv else "unknown"
        if "--brief-file" in argv:
            brief = Path(argv[argv.index("--brief-file") + 1]).read_text(encoding="utf-8")
        else:
            brief = sys.stdin.read()
        prior = []
        for i, a in enumerate(argv):
            if a == "--prior" and i + 1 < len(argv) and "=" in argv[i + 1]:
                src, fp = argv[i + 1].split("=", 1)
                try:
                    prior.append({"source": src, "content": Path(fp).read_text(encoding="utf-8")})
                except Exception:
                    pass
        print(dispatch(root, mode, target, brief, prior)); return 0
    if cmd == "poll":
        print(poll(root, argv[1])); return 0
    if cmd == "list":
        print(list_jobs(root)); return 0
    if cmd == "outcome":
        outcome(root, argv[1], argv[2]); return 0
    print(f"unknown command: {cmd}", file=sys.stderr); return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 scripts/overnight/tests/test_fusion_ladder.py`
Expected: `PASS: fusion_ladder`

- [ ] **Step 5: Smoke the off-by-default dispatch (no spawn)**

Run: `echo "x" | python3 .fusion/ladder.py dispatch --mode stuck --target build`
Expected: `disabled` (FUSION_ENABLED unset); no job spawned.

- [ ] **Step 6: Commit**

```bash
git add .fusion/ladder.py scripts/overnight/tests/test_fusion_ladder.py
git commit -m "fusion: Plan2 -- ladder.py async job queue (dispatch/poll/list/worker/outcome)"
```

---

### Task 2: fusion_stuck_detect.py hook + settings wiring

**Files:**
- Create: `.claude/hooks/fusion_stuck_detect.py`
- Modify: `.claude/settings.json` (PostToolUse on Bash)
- Test: extend `scripts/test-tooling.sh` with a sub-test group

**Interfaces:** PostToolUse on Bash; tracks `.claude/state/fusion-stuck.json` `{target, consecutive_failures}`; emits a `systemMessage` at threshold (3). Overnight-scoped; never blocks.

- [ ] **Step 1: Write the hook**

```python
#!/usr/bin/env python3
# block-via: warning-only (stuck-detect nudge; never exits 2)
"""PostToolUse on Bash: count consecutive same-target build/test/smoke FAILURES and
nudge the loop into the Claude->Codex->Fusion ladder at threshold. Overnight-only;
fail-open; never blocks."""
from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path

THRESHOLD = 3
_TARGETS = (("scripts/build.sh", "build"), ("scripts/test.sh", "test"),
            ("scripts/test-smoke.sh", "smoke"))
_OK = ("=== BUILD OK ===", "SMOKE TEST PASSED", "0 failed", "All tests passed")
_FAIL = ("BUILD FAILED", "SMOKE TEST FAILED", "FAIL", "error:", "Traceback")


def _repo_root() -> Path | None:
    here = Path(__file__).resolve()
    for p in here.parents:
        if (p / ".claude").is_dir():
            return p
    return None


def _classify(command: str, response_text: str):
    target = None
    for needle, name in _TARGETS:
        if needle in command:
            target = name
            break
    if target is None:
        return None, None
    txt = response_text or ""
    if any(s in txt for s in _OK):
        return target, "ok"
    if any(s in txt for s in _FAIL):
        return target, "fail"
    return target, "unknown"


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not os.environ.get("OVERNIGHT_SEQUENCER_RUN"):
        return 0
    if d.get("tool_name") != "Bash":
        return 0
    root = _repo_root()
    if root is None:
        return 0
    command = (d.get("tool_input") or {}).get("command", "") or ""
    resp = d.get("tool_response")
    text = resp if isinstance(resp, str) else json.dumps(resp)
    target, verdict = _classify(command, text)
    if target is None or verdict == "unknown":
        return 0
    state_p = root / ".claude" / "state" / "fusion-stuck.json"
    try:
        st = json.loads(state_p.read_text())
    except Exception:
        st = {}
    if verdict == "ok" or st.get("target") != target:
        st = {"target": target, "consecutive_failures": 0}
    if verdict == "fail":
        st["target"] = target
        st["consecutive_failures"] = int(st.get("consecutive_failures", 0)) + 1
    try:
        state_p.parent.mkdir(parents=True, exist_ok=True)
        state_p.write_text(json.dumps(st))
    except Exception:
        pass
    if st.get("consecutive_failures", 0) >= THRESHOLD:
        sys.stderr.write(
            f"[fusion-ladder] STUCK: {st['consecutive_failures']} consecutive '{target}' "
            f"failures. Escalate: Tier 1 Skill(codex:rescue) (2 rounds); if still stuck, "
            f"Tier 2 'python3 .fusion/ladder.py dispatch --mode stuck --target {target} "
            f"--brief-file <brief>' then DEFER the section and collect the result on a "
            f"later pass (python3 .fusion/ladder.py poll <id>).\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Wire into settings.json (PostToolUse on Bash)**

Add to the `PostToolUse` array:
```json
      {
        "matcher": "Bash",
        "hooks": [
          {
            "type": "command",
            "command": "python3 \"${CLAUDE_PROJECT_DIR:-.}/.claude/hooks/fusion_stuck_detect.py\"",
            "timeout": 5
          }
        ]
      },
```

- [ ] **Step 3: Validate settings.json parses**

Run: `python3 -c "import json; json.load(open('.claude/settings.json')); print('ok')"`
Expected: `ok`.

- [ ] **Step 4: Add a test-tooling.sh sub-test group**

After the existing groups, add probes (build the synthetic payload + assert behavior):
```bash
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[fusion_stuck_detect]${NC}"
_FSD="$REPO_ROOT/.claude/hooks/fusion_stuck_detect.py"
_fsd_runs() {  # <desc> <env...> ; PAYLOAD on stdin ; PASS iff exit 0
  local desc="$1"; shift
  local rc; printf '%s' "$PAYLOAD" | env "$@" python3 "$_FSD" >/dev/null 2>&1; rc=$?
  if [ "$rc" = "0" ]; then t_pass "fusion_stuck_detect: $desc"; else t_fail "fusion_stuck_detect: $desc (rc=$rc)"; fi
}
PAYLOAD='{"tool_name":"Bash","tool_input":{"command":"bash scripts/build.sh"},"tool_response":"BUILD FAILED"}'
_fsd_runs "silent when OVERNIGHT_SEQUENCER_RUN unset" "PATH=$PATH"
_fsd_runs "runs on build-fail under overnight" "PATH=$PATH" "OVERNIGHT_SEQUENCER_RUN=1"
PAYLOAD='{"tool_name":"Bash","tool_input":{"command":"ls"},"tool_response":"x"}'
_fsd_runs "ignores non-target commands" "PATH=$PATH" "OVERNIGHT_SEQUENCER_RUN=1"
```

- [ ] **Step 5: Run the sub-tests**

Run: `bash scripts/test-tooling.sh 2>&1 | grep -iE "fusion_stuck_detect|FAIL" | head`
Expected: all three pass; no FAIL.

- [ ] **Step 6: Commit**

```bash
git add .claude/hooks/fusion_stuck_detect.py .claude/settings.json scripts/test-tooling.sh
git commit -m "fusion: Plan2 -- fusion_stuck_detect hook (overnight-only nudge into the ladder)"
```

---

### Task 3: Skill wiring (orchestration)

**Files:**
- Modify: `.claude/skills/debug-session/SKILL.md` (stuck path -> ladder)
- Modify: `.claude/skills/review-todo-section/SKILL.md` (high-risk Fusion review supplement)
- Modify: `.claude/skills/overnight-sequencer/SKILL.md` (collect pending jobs each pass)
- Modify: `.claude/hooks/runner_status.py` (show N pending Fusion jobs)

- [ ] **Step 1: debug-session -- the stuck ladder**

In `.claude/skills/debug-session/SKILL.md`, in the section on a resistant root cause, append:
```markdown
- **Stuck ladder (3 -> 2 -> 1):** after ~3 failed hypothesis cycles on the same
  symptom, escalate: Tier 1 `Skill(codex:rescue)` (up to 2 rounds); if still stuck,
  Tier 2 `python3 .fusion/ladder.py dispatch --mode stuck --target <sym> --brief-file <f> --prior opus=<your-analysis> --prior codex=<rescue-output>` -- it runs the DIY Fusion ensemble ASYNC. Do other work; collect with `python3 .fusion/ladder.py poll <id>` and validate the synthesis before applying (it is a lead, not a verdict).
```

- [ ] **Step 2: review-todo-section -- high-risk Fusion review supplement**

In step 9 (industry/parity) or step 8 area, append:
```markdown
- **High-risk Fusion review (apex, optional):** for a section on the highest-stakes
  paths (SMP/lock-order, boot ABI, security) where the Codex review left real doubt,
  dispatch a Fusion review job: `python3 .fusion/ladder.py dispatch --mode review --target <section> --brief-file <diff> --prior codex=<codex-findings>`. Async; collect + validate before acting. Off unless FUSION_ENABLED + a key.
```

- [ ] **Step 3: overnight-sequencer -- collect pending jobs each pass**

In the Situational-awareness section, append:
```markdown
- **Collect Fusion jobs each pass.** If `runner-status` shows Fusion jobs pending,
  `python3 .fusion/ladder.py poll <id>` for each; on DONE, validate + apply the
  synthesis (main thread is the review layer), then record the outcome:
  `python3 .fusion/ladder.py outcome <id> <resolved|unresolved>`. A fusion-deferred
  section stays `[/]` until its job is collected.
```

- [ ] **Step 4: runner_status.py -- show pending Fusion jobs**

In `runner_status.py`, add a `fusion_jobs(root)` that counts `.fusion/jobs/*.meta.json` with `status=="pending"` (and `done` not yet collected), and include `fusion:<n>` in `anchor_line` + a FUSION line in `full_brief`. Fail-open.

- [ ] **Step 5: Verify wiring + lint**

Run:
```bash
grep -c "ladder.py dispatch" .claude/skills/debug-session/SKILL.md .claude/skills/review-todo-section/SKILL.md
grep -c "ladder.py poll" .claude/skills/overnight-sequencer/SKILL.md
python3 scripts/overnight/tests/test_runner_status.py
bash scripts/lint.sh 2>&1 | tail -3
```
Expected: greps >= 1; runner_status test green; lint exits 0.

- [ ] **Step 6: Commit**

```bash
git add .claude/skills/debug-session/SKILL.md .claude/skills/review-todo-section/SKILL.md .claude/skills/overnight-sequencer/SKILL.md .claude/hooks/runner_status.py
git commit -m "fusion: Plan2 -- skill wiring (debug-session/review/sequencer ladder + runner-status jobs)"
```

---

### Task 4: Live end-to-end test (real async dispatch)

- [ ] **Step 1: Dispatch a real Fusion job async**

Run (FUSION_ENABLED + secret present; the brutal heisenbug brief still at /tmp/heisenbug-brief.txt):
```bash
FUSION_ENABLED=1 python3 .fusion/ladder.py dispatch --mode stuck --target heisenbug --brief-file /tmp/heisenbug-brief.txt
```
Expected: prints a job id immediately (non-blocking); a detached worker is now running.

- [ ] **Step 2: Confirm it is pending + detached**

Run: `python3 .fusion/ladder.py list; python3 .fusion/ladder.py poll <id>`
Expected: `list` shows the job `pending`; `poll` prints `PENDING`. (The worker survives independent of this shell.)

- [ ] **Step 3: Collect when done**

After ~6 min: `python3 .fusion/ladder.py poll <id>`
Expected: `DONE` + the judge synthesis. Confirm `.fusion/jobs/<id>.meta.json` has `status=done` + `cost`.

- [ ] **Step 4: Record the outcome + reset budget**

```bash
python3 .fusion/ladder.py outcome <id> resolved
rm -f .claude/state/fusion-budget.json
```

---

## Self-Review

**Spec coverage:** ladder controller + async job queue (Task 1, mirrors the spec's job-queue model), stuck-detect trigger (Task 2), skill wiring + runner-status tie-in (Task 3), live validation (Task 4). The `prior` feed-forward (Opus+Codex analyses to the judge) is threaded through `dispatch --prior`. Off-by-default + fail-open + overnight-scoped throughout.

**Placeholder scan:** every code step is complete; commands carry expected output. Task 4 uses `<id>` as a runtime value (not a placeholder -- it is the dispatch output).

**Type consistency:** `ladder.run_worker/poll/list_jobs/dispatch/outcome(root, ...)` match the test; job-file names (`<id>.request.json/.meta.json/.result.txt`) and meta fields (`status`, `cost`, `target`) are consistent across dispatch/worker/poll/list.
