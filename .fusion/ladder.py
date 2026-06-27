#!/usr/bin/env python3
"""Fusion async job queue + tier mechanism (Plan 2). Dispatch a Fusion escalation as
a DETACHED job, defer the section, collect the durable result on a later pass.

CLI:
  dispatch --mode {stuck,review} --target T [--brief-file F] [--prior SRC=FILE]...
  poll <id> | list | outcome <id> <verdict> | _worker <id>
Off-by-default; fail-open; the detached worker survives the main loop. Stdlib only.
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
    res = _jobs(root) / f"{jid}.result.txt"
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
        run_worker(root, argv[1])
        return 0
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
        print(dispatch(root, mode, target, brief, prior))
        return 0
    if cmd == "poll":
        print(poll(root, argv[1]))
        return 0
    if cmd == "list":
        print(list_jobs(root))
        return 0
    if cmd == "outcome":
        outcome(root, argv[1], argv[2])
        return 0
    print(f"unknown command: {cmd}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
