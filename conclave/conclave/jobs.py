"""Conclave's async job queue (the tier-1 mechanism, NOT the escalation ladder --
the Claude->Codex->Conclave ladder is project-side, in each project's .conclave/).

Dispatch an escalation as a DETACHED job, defer, collect the durable result later;
the worker survives the calling process. CLI: dispatch|poll|list|outcome|stats|_worker.
Off-by-default; fail-open.
"""
from __future__ import annotations

import json
import os
import secrets
import subprocess
import sys
import time
from pathlib import Path

from conclave import config


def _jobs(root: Path, project: str) -> Path:
    d = Path(root) / "data" / "projects" / project / "jobs"
    d.mkdir(parents=True, exist_ok=True)
    return d


def _read_json(p: Path):
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return None


def _write_json(p: Path, data: dict) -> None:
    p.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def _do_escalate(root, project, jid, req) -> dict:
    """Real wiring seam (monkeypatched in tests)."""
    os.environ.setdefault("CONCLAVE_ENABLED", "1")
    from conclave import harness
    from conclave.embed import Embedder
    from conclave.index import open_index
    from conclave.memory import Memory
    from conclave.metrics import MetricsStore
    cfg = config.load(root)
    idx = open_index(cfg, Path(root) / "data" / "index")
    emb = Embedder(cfg, cfg.secret)
    mem = Memory(root, project, idx, emb)
    met = MetricsStore(root, project)
    return harness.escalate(cfg=cfg, secret=cfg.secret, mode=req.get("mode", "stuck"),
                            brief=req.get("brief", ""), project=project, root=root,
                            memory=mem, metrics=met, prior=req.get("prior"),
                            target=req.get("target"), run_id=jid)


def run_worker(root: Path, project: str, jid: str) -> None:
    jobs = _jobs(root, project)
    meta_p = jobs / f"{jid}.meta.json"
    meta = _read_json(meta_p) or {"id": jid}
    req = _read_json(jobs / f"{jid}.request.json") or {}
    try:
        res = _do_escalate(root, project, jid, req)
        if res.get("status") in ("ok", "panel_only"):
            (jobs / f"{jid}.result.txt").write_text(res.get("output") or "", encoding="utf-8")
            meta.update(status="done", cost=res.get("cost"), per_model=res.get("panel"),
                        cumulative_cost=res.get("cumulative_cost"), tokens=res.get("tokens"),
                        wall_s=res.get("wall_s"), finished_at=time.strftime("%Y-%m-%dT%H:%M:%S"))
            from conclave import compile as _cc
            _cc.record_run(root, project, run_id=jid, brief=req.get("brief", ""),
                           synthesis=res.get("output", ""), verdict="unknown")
        else:
            meta.update(status="failed", error=res.get("status"),
                        finished_at=time.strftime("%Y-%m-%dT%H:%M:%S"))
    except Exception as e:
        meta.update(status="failed", error=str(e)[:200],
                    finished_at=time.strftime("%Y-%m-%dT%H:%M:%S"))
    _write_json(meta_p, meta)


def dispatch(root: Path, project: str, mode: str, target: str, brief: str, prior: list) -> str:
    if os.environ.get("CONCLAVE_ENABLED") != "1":
        return "disabled"
    if not config._read_secret(Path(root)):
        return "no_key"
    jobs = _jobs(root, project)
    jid = time.strftime("%Y%m%d-%H%M%S") + "-" + secrets.token_hex(3)
    _write_json(jobs / f"{jid}.request.json",
                {"mode": mode, "target": target, "brief": brief, "prior": prior or []})
    _write_json(jobs / f"{jid}.meta.json",
                {"id": jid, "target": target, "mode": mode, "status": "pending",
                 "started_at": time.strftime("%Y-%m-%dT%H:%M:%S")})
    log = open(jobs / f"{jid}.worker.log", "ab")
    # Run as a module (-m), NOT as a file path: running conclave/jobs.py directly would
    # put conclave/ on sys.path[0] and shadow stdlib `http` with conclave/http.py.
    subprocess.Popen([sys.executable, "-m", "conclave.jobs", "_worker", project, jid],
                     cwd=str(root), env={**os.environ, "PYTHONPATH": str(root)},
                     stdout=log, stderr=log, start_new_session=True)
    return jid


def poll(root: Path, project: str, jid: str) -> str:
    meta = _read_json(_jobs(root, project) / f"{jid}.meta.json")
    if not meta:
        return f"FAILED no such job {jid}"
    st = meta.get("status")
    if st == "pending":
        return "PENDING"
    if st == "failed":
        return "FAILED " + str(meta.get("error", ""))
    res = _jobs(root, project) / f"{jid}.result.txt"
    return "DONE\n" + (res.read_text(encoding="utf-8") if res.exists() else "")


def list_jobs(root: Path, project: str) -> str:
    out = []
    for m in sorted(_jobs(root, project).glob("*.meta.json")):
        meta = _read_json(m) or {}
        out.append(f"{meta.get('id', m.stem)} {meta.get('status', '?')} {meta.get('target', '')}")
    return "\n".join(out)


def outcome(root: Path, project: str, jid: str, verdict: str) -> None:
    """Record a verdict and AUTOMATICALLY learn from it: bump totals, update panel/
    routing policies, mark the transcript, and (best-effort) add the synthesis as a
    retrievable lesson. This is the loop that makes Conclave self-improve."""
    from conclave import compile as cc
    from conclave import learn
    from conclave.metrics import MetricsStore
    jobs = _jobs(root, project)
    meta_p = jobs / f"{jid}.meta.json"
    meta = _read_json(meta_p) or {"id": jid}
    meta["outcome"] = verdict
    _write_json(meta_p, meta)
    MetricsStore(root, project).record_outcome(jid, verdict)
    cc.set_run_verdict(root, project, jid, verdict)
    signature = meta.get("target") or jid
    learn.update_policies(root, project,
                          {"signature": signature, "per_model": meta.get("per_model", [])}, verdict)
    if verdict == "resolved":
        try:
            _note_lesson(root, project, signature, jid)
        except Exception:
            pass


def _note_lesson(root: Path, project: str, signature: str, jid: str) -> None:
    res = _jobs(root, project) / f"{jid}.result.txt"
    synthesis = res.read_text(encoding="utf-8") if res.exists() else ""
    if not synthesis:
        return
    cfg = config.load(root)
    if not cfg.secret:
        return
    from conclave.embed import Embedder
    from conclave.index import open_index
    from conclave.memory import Memory
    idx = open_index(cfg, Path(root) / "data" / "index")
    Memory(root, project, idx, Embedder(cfg, cfg.secret)).note(signature, synthesis[:2000], verified=True)


def stats(root: Path, project: str) -> str:
    try:
        t = json.loads((Path(root) / "data" / "projects" / project / "totals.json").read_text())
    except Exception:
        return "no runs recorded yet"
    by_mode = " ".join(f"{m}:{d.get('runs', 0)}/${d.get('cost', 0):.2f}"
                       for m, d in (t.get("by_mode") or {}).items())
    return (f"runs={t.get('runs', 0)} total_cost=${t.get('total_cost', 0):.4f} "
            f"tokens={t.get('total_tokens', 0)} resolved={t.get('resolved', 0)} "
            f"unresolved={t.get('unresolved', 0)} | {by_mode}".rstrip())


def main(argv) -> int:
    root = Path.cwd()
    cmd = argv[0] if argv else "list"
    if cmd == "_worker":
        run_worker(root, argv[1], argv[2])
        return 0
    print(f"unknown command: {cmd}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
