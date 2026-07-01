"""Knowledge-compilation pipeline: compile labeled run transcripts into the curated
corpus + embedded vectors as <= shard_max_mib gz shards, with a provenance ledger.

record_run() persists a transcript; compile() shards + ledgers + advances LATEST;
should_compile() is the cadence gate; eval_compiled() scores resolved-rate.
"""
from __future__ import annotations

import gzip
import json
import time
from pathlib import Path


def _runs_path(root, project) -> Path:
    return Path(root) / "data" / "projects" / project / "runs.jsonl"


def record_run(root, project, *, run_id, brief, synthesis, verdict="unknown") -> None:
    p = _runs_path(root, project)
    p.parent.mkdir(parents=True, exist_ok=True)
    with p.open("a", encoding="utf-8") as f:
        f.write(json.dumps({"run_id": run_id, "brief": brief, "synthesis": synthesis,
                            "verdict": verdict, "ts": time.strftime("%Y-%m-%dT%H:%M:%S")}) + "\n")


def set_run_verdict(root, project, run_id, verdict) -> None:
    """Update an existing transcript's verdict so compile includes resolved runs."""
    p = _runs_path(root, project)
    try:
        lines = p.read_text(encoding="utf-8").splitlines()
    except Exception:
        return
    out, changed = [], False
    for ln in lines:
        try:
            r = json.loads(ln)
        except Exception:
            out.append(ln)
            continue
        if r.get("run_id") == run_id:
            r["verdict"] = verdict
            changed = True
        out.append(json.dumps(r))
    if changed:
        p.write_text("\n".join(out) + "\n", encoding="utf-8")


def _load_runs(root, project) -> list:
    try:
        return [json.loads(l) for l in _runs_path(root, project).read_text(encoding="utf-8").splitlines() if l]
    except Exception:
        return []


def _ledger_path(root) -> Path:
    return Path(root) / "ledger.jsonl"


def _last_compile(root):
    try:
        recs = [json.loads(l) for l in _ledger_path(root).read_text(encoding="utf-8").splitlines() if l]
        return recs[-1] if recs else None
    except Exception:
        return None


def should_compile(root, cfg, project: str) -> bool:
    runs = _load_runs(root, project)
    last = _last_compile(root)
    seen = last.get("num_total", 0) if last else 0
    if len(runs) - seen >= cfg.compile_new_examples:
        return True
    if last is None:
        return False
    try:
        last_t = time.strptime(last["date"], "%Y-%m-%dT%H:%M:%S")
        days = (time.time() - time.mktime(last_t)) / 86400.0
        return days >= cfg.compile_every_days and len(runs) > seen
    except Exception:
        return False


def _shard_blobs(store, prefix: str, blobs: list, max_mib: int) -> list:
    parts, cur, cur_size, idx = [], [], 0, 1
    limit = max_mib * 1024 * 1024

    def flush():
        nonlocal cur, cur_size, idx
        if not cur:
            return
        data = gzip.compress(b"\n".join(cur))
        parts.append(store.put(f"{prefix}.part-%03d.gz" % idx, data))
        idx += 1
        cur, cur_size = [], 0

    for b in blobs:
        cur.append(b)
        cur_size += len(b)
        if cur_size >= limit:
            flush()
    flush()
    return parts


def compile(root, cfg, project, *, embedder, store) -> dict:
    runs = _load_runs(root, project)
    labeled = [r for r in runs if r.get("verdict") == "resolved"]
    last = _last_compile(root)
    vn = (int(last["version"].rsplit("v", 1)[-1]) + 1) if last else 1
    version = f"conclave-v{vn}"

    docs = [f"PROBLEM:\n{r['brief']}\n\nSOLUTION:\n{r['synthesis']}" for r in labeled]
    corpus_shards = _shard_blobs(store, f"corpus/{version}",
                                 [d.encode("utf-8") for d in docs], cfg.shard_max_mib)
    vecs = embedder.embed_docs(docs) if docs else []
    vector_shards = _shard_blobs(store, f"vectors/{version}",
                                 [json.dumps(v).encode("utf-8") for v in vecs], cfg.shard_max_mib)

    rec = {"version": version, "date": time.strftime("%Y-%m-%dT%H:%M:%S"), "trigger": "manual",
           "num_examples": len(labeled), "num_total": len(runs),
           "eval_scores": {"resolved_rate": eval_compiled(root, project)},
           "output_shards": corpus_shards + vector_shards}
    with _ledger_path(root).open("a", encoding="utf-8") as f:
        f.write(json.dumps(rec) + "\n")
    (Path(root) / "LATEST").write_text(version + "\n", encoding="utf-8")
    return {"version": version, "shards": corpus_shards + vector_shards, "num_examples": len(labeled)}


def eval_compiled(root, project: str) -> float:
    """Resolved-rate from totals -- the ground-truth quality signal."""
    try:
        t = json.loads((Path(root) / "data" / "projects" / project / "totals.json").read_text())
    except Exception:
        return 0.0
    r, u = int(t.get("resolved", 0)), int(t.get("unresolved", 0))
    return round(r / (r + u), 4) if (r + u) else 0.0
