#!/usr/bin/env python3
"""Agent model replay: may this agent move to a cheaper model without losing quality?

Decided 2026-09-28: an agent's model pin changes (Opus -> Sonnet, Sonnet -> Haiku 5.5+)
only after a REPLAY on archived inputs with known answers shows no loss against the
current model. This is that replay.

How it works
------------
Tasks are JSONL, one per line: {id, commit, prompt, expect: {kind, values}, severity?}.
Each task runs as `claude -p --agent <name>` with the agent defined INLINE from its
`.claude/agents/<name>.md` file (same system prompt and tools; only the model under
test differs), in an exported snapshot of the task's commit (`git archive`), so it sees
the tree as it was and none of the repo's live hooks or state. The answer is scored
against the deterministic `expect`:

  locs     hit when the report names the file with a line within +/-TOL of the answer
  commit   hit when the report contains the answer's short sha (7+ hex chars)

Task sets (build them with `build`; each draws on a DETERMINISTIC oracle):

  kernel-quality-auditor   105+ confirmed defects from .claude/state/finding-triage.jsonl
                           (decision "fix", src/kernel|include/kernel|src/boot), replayed
                           at the last commit before the finding was triaged
  git-historian            "which commit added <file>?" -- `git log --diff-filter=A`

Verdict (`compare`): the candidate PASSES when its recall is at least the baseline's AND
it misses no high/critical task the baseline caught. Cost and wall time are reported so
the saving is visible next to the quality.

Usage
-----
  agent-replay.py build   --agent kernel-quality-auditor [--limit N] [--out FILE]
  agent-replay.py run     --agent NAME --model MODEL --tasks FILE [--limit N] [--out FILE]
  agent-replay.py rescore --tasks FILE RESULTS.jsonl     (re-score stored reports, no model calls)
  agent-replay.py compare BASELINE.jsonl CANDIDATE.jsonl
Runs cost real tokens (the baseline is often Opus). Start with --limit 5.
"""
from __future__ import annotations

import argparse
import json
import random
import re
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOL = 8
AUDIT_KINDS = {"adversarial", "re-adversarial", "kernel-auditor", "kernel-quality", "consistency", "quality"}


def git(*a, cwd=REPO, check=True) -> str:
    return subprocess.run(["git", *a], cwd=cwd, capture_output=True, text=True, check=check).stdout


# ---------------------------------------------------------------- agent definitions
def agent_def(name: str, model: str) -> dict:
    """The agent from .claude/agents/<name>.md, with `model` swapped in."""
    text = (REPO / ".claude/agents" / f"{name}.md").read_text()
    m = re.match(r"^---\n(.*?)\n---\n(.*)$", text, re.S)
    if not m:
        raise SystemExit(f"agent-replay: {name}.md has no frontmatter")
    front = dict(re.findall(r"^(\w+):\s*(.*)$", m.group(1), re.M))
    tools = [t.strip() for t in front.get("tools", "").split(",") if t.strip()]
    return {name: {"description": front.get("description", name), "prompt": m.group(2).strip(),
                   "tools": tools, "model": model}}


# ---------------------------------------------------------------- task builders
def build_kernel_auditor(limit: int | None, seed: int) -> list[dict]:
    rows = [json.loads(l) for l in (REPO / ".claude/state/finding-triage.jsonl").read_text().splitlines() if l.strip()]
    tasks, seen = [], set()
    for r in rows:
        loc = r.get("loc") or ""
        if r.get("decision") != "fix" or r.get("kind") not in AUDIT_KINDS:
            continue
        if not loc.startswith(("src/kernel", "include/kernel", "src/boot")):
            continue
        path, _, ln = loc.partition(":")
        try:
            line = int(ln.split("-")[0])
        except ValueError:
            continue
        commit = git("rev-list", "-1", f"--before={r['epoch']}", "HEAD").strip()
        if not commit:
            continue
        show = subprocess.run(["git", "show", f"{commit}:{path}"], cwd=REPO, capture_output=True, text=True)
        if show.returncode or len(show.stdout.splitlines()) < line:
            continue
        key = (commit, path, line)
        if key in seen:
            continue
        seen.add(key)
        tasks.append({
            "id": r.get("id") or f"{path}:{line}",
            "commit": commit,
            "severity": r.get("severity"),
            "title": r.get("title"),
            "prompt": (f"Audit `{path}` in this tree for SMP, lock-order, memory-safety and bare-metal defects, "
                       f"as you would a section diff. Report every finding as `file:line` with a one-line reason."),
            "expect": {"kind": "locs", "values": [f"{path}:{line}"]},
        })
    random.Random(seed).shuffle(tasks)
    return tasks[:limit] if limit else tasks


def build_git_historian(limit: int | None, seed: int) -> list[dict]:
    head = git("rev-parse", "HEAD").strip()
    files = [f for f in git("ls-files", "src/kernel").splitlines() if f.endswith(".c")]
    random.Random(seed).shuffle(files)
    tasks = []
    for f in files:
        added = git("log", "--diff-filter=A", "--format=%h", "--", f).split()
        if len(added) != 1:
            continue                      # renamed or re-added: no single right answer
        tasks.append({"id": f, "commit": head, "severity": None,
                      "prompt": f"Which commit first ADDED the file `{f}`? Answer with its short sha and subject.",
                      "expect": {"kind": "commit", "values": [added[0]]}})
        if limit and len(tasks) >= limit:
            break
    return tasks


BUILDERS = {"kernel-quality-auditor": build_kernel_auditor, "git-historian": build_git_historian}



MAX_SPAN = 30   # a range wider than this names a region, not a defect


def _finding_locations(text: str) -> list:
    """(file, lo, hi) for each location the report CLAIMS AS A FINDING.

    A findings envelope (review-result-v1) is authoritative when present: only its
    findings count, never locations mentioned while explaining what was checked
    and found correct (2026-09-28: `tpm.c:555-985`, cited as correctly locked,
    scored a false hit). Without an envelope, every `file:line` in the text counts,
    but a range wider than MAX_SPAN lines is a region, not a finding."""
    env = None
    for block in re.findall(r"```(?:json)?\s*(\{.*?\})\s*```", text, re.S) + [text.strip()]:
        try:
            obj = json.loads(block)
        except ValueError:
            continue
        if isinstance(obj, dict) and isinstance(obj.get("findings"), list):
            env = obj
            break
    locs = []
    if env is not None:
        for f in env["findings"]:
            if not isinstance(f, dict):
                continue
            if isinstance(f.get("file"), str) and isinstance(f.get("line"), int):
                locs.append((f["file"], f["line"], f["line"]))
            locs += _text_locations(str(f.get("summary", "")) + " " + str(f.get("evidence", "")))
        return locs
    return _text_locations(text)


def _text_locations(text: str) -> list:
    out = []
    # The name must start at a boundary: `myserial.c` is not `serial.c`.
    for m in re.finditer(r"(?<![\w.-])((?:[\w.-]+/)*[\w.-]+\.(?:c|h|S|asm|py|sh)):(\d+)(?:-(\d+))?", text):
        lo = int(m.group(2))
        hi = int(m.group(3) or lo)
        if hi - lo <= MAX_SPAN:
            out.append((m.group(1), lo, hi))
    return out

# ---------------------------------------------------------------- scoring
def score(expect: dict, text: str) -> bool:
    if expect["kind"] == "commit":
        return any(re.search(r"\b" + re.escape(v[:7]) + r"[0-9a-f]*\b", text) for v in expect["values"])
    if expect["kind"] == "locs":
        cands = _finding_locations(text)
        for v in expect["values"]:
            path, _, ln = v.partition(":")
            want = int(ln)
            base = path.split("/")[-1]
            for f, lo, hi in cands:
                if f.split("/")[-1] == base and lo - TOL <= want <= hi + TOL:
                    return True
        return False
    raise ValueError(expect["kind"])


# ---------------------------------------------------------------- run
def snapshot(commit: str, dest: Path) -> None:
    with tempfile.TemporaryFile() as fh:
        subprocess.run(["git", "archive", "--format=tar", commit], cwd=REPO, stdout=fh, check=True)
        fh.seek(0)
        with tarfile.open(fileobj=fh) as tar:
            tar.extractall(dest, filter="data")


def run_task(agent: str, model: str, task: dict, timeout: int) -> dict:
    uses_git = "Bash" in agent_def(agent, model)[agent]["tools"]
    with tempfile.TemporaryDirectory(prefix="replay-") as d:
        cwd = REPO if uses_git else Path(d)
        if not uses_git:
            snapshot(task["commit"], Path(d))
        cmd = ["claude", "-p", "--agent", agent, "--agents", json.dumps(agent_def(agent, model)),
               "--model", model, "--output-format", "json", "--permission-mode", "bypassPermissions",
               task["prompt"]]
        t0 = time.time()
        try:
            p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
            out = json.loads(p.stdout or "{}")
        except (subprocess.TimeoutExpired, ValueError) as e:
            out = {"result": "", "error": str(e)}
    text = str(out.get("result") or "")
    return {"id": task["id"], "agent": agent, "model": model, "severity": task.get("severity"),
            "hit": score(task["expect"], text), "cost_usd": out.get("total_cost_usd"),
            "seconds": round(time.time() - t0, 1), "error": out.get("error") or (None if text else "empty result"),
            "report": text[:40000]}


def cmd_run(a) -> int:
    tasks = [json.loads(l) for l in Path(a.tasks).read_text().splitlines() if l.strip()]
    if a.limit:
        tasks = tasks[:a.limit]
    out = Path(a.out or REPO / "build/replay" / f"{a.agent}-{a.model}-{time.strftime('%Y%m%d-%H%M%S')}.jsonl")
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w") as fh:
        for i, t in enumerate(tasks, 1):
            r = run_task(a.agent, a.model, t, a.timeout)
            fh.write(json.dumps(r) + "\n")
            fh.flush()
            print(f"[{i}/{len(tasks)}] {t['id']}: {'HIT' if r['hit'] else 'miss'} "
                  f"({r['seconds']}s, ${r['cost_usd']}){' ERROR ' + r['error'] if r['error'] else ''}")
    print(f"results: {out}")
    return 0


def cmd_rescore(a) -> int:
    """Re-score stored reports with the CURRENT scorer (no model calls), in place."""
    tasks = {t["id"]: t for t in (json.loads(l) for l in Path(a.tasks).read_text().splitlines() if l.strip())}
    rows = [json.loads(l) for l in Path(a.results).read_text().splitlines() if l.strip()]
    changed = 0
    for r in rows:
        t = tasks.get(r["id"])
        if t is None or "report" not in r:
            continue
        hit = score(t["expect"], r["report"])
        changed += hit != r["hit"]
        r["hit"] = hit
    Path(a.results).write_text("".join(json.dumps(r) + "\n" for r in rows))
    print(f"rescored {len(rows)} result(s), {changed} verdict(s) changed")
    return 0


def load(p):
    return {r["id"]: r for r in (json.loads(l) for l in Path(p).read_text().splitlines() if l.strip())}


def summary(rs):
    n = len(rs)
    hits = sum(r["hit"] for r in rs.values())
    cost = sum(r["cost_usd"] or 0 for r in rs.values())
    return n, hits, cost, sum(r["seconds"] for r in rs.values())


def cmd_compare(a) -> int:
    base, cand = load(a.baseline), load(a.candidate)
    common = sorted(set(base) & set(cand))
    if not common:
        print("compare: no common tasks")
        return 2
    b = {k: base[k] for k in common}
    c = {k: cand[k] for k in common}
    bn, bh, bc, bs = summary(b)
    _, ch, cc, cs = summary(c)
    lost = [k for k in common if b[k]["hit"] and not c[k]["hit"]]
    lost_serious = [k for k in lost if (b[k].get("severity") or "") in ("high", "critical")]
    bm, cm = next(iter(b.values()))["model"], next(iter(c.values()))["model"]
    print(f"tasks {bn}: baseline {bm} {bh}/{bn} hits ${bc:.2f} {bs:.0f}s | candidate {cm} {ch}/{bn} hits ${cc:.2f} {cs:.0f}s")
    if lost:
        print("candidate missed what the baseline caught: " + ", ".join(lost))
    ok = ch >= bh and not lost_serious
    print(("PASS" if ok else "FAIL") + (f" -- serious misses: {', '.join(lost_serious)}" if lost_serious else ""))
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--agent", required=True, choices=sorted(BUILDERS))
    b.add_argument("--limit", type=int)
    b.add_argument("--seed", type=int, default=20260928)
    b.add_argument("--out")
    r = sub.add_parser("run")
    r.add_argument("--agent", required=True)
    r.add_argument("--model", required=True)
    r.add_argument("--tasks", required=True)
    r.add_argument("--limit", type=int)
    r.add_argument("--timeout", type=int, default=900)
    r.add_argument("--out")
    rs = sub.add_parser("rescore")
    rs.add_argument("--tasks", required=True)
    rs.add_argument("results")
    c = sub.add_parser("compare")
    c.add_argument("baseline")
    c.add_argument("candidate")
    a = ap.parse_args()
    if a.cmd == "build":
        tasks = BUILDERS[a.agent](a.limit, a.seed)
        out = Path(a.out or REPO / "scripts/overnight/replay" / f"{a.agent}.jsonl")
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text("".join(json.dumps(t) + "\n" for t in tasks))
        print(f"{len(tasks)} task(s) -> {out}")
        return 0
    return {"run": cmd_run, "rescore": cmd_rescore, "compare": cmd_compare}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
