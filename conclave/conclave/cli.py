"""The `conclave` CLI. Cheap agent verbs (recall/note) never touch the panel;
ask/dispatch run the harness; teach/compile/outcome/stats/doctor are lifecycle.
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from conclave import config

_CMDS = ("ask", "dispatch", "poll", "list", "recall", "note", "teach", "compile",
         "outcome", "stats", "doctor")


def _deps(root, cfg):
    from conclave.embed import Embedder
    from conclave.index import open_index
    idx = open_index(cfg, Path(root) / "data" / "index")
    emb = Embedder(cfg, cfg.secret)
    return idx, emb


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] not in _CMDS:
        print(f"usage: conclave {{{'|'.join(_CMDS)}}} ...", file=sys.stderr)
        return 1
    cmd, rest = argv[0], argv[1:]
    root = Path.cwd()

    p = argparse.ArgumentParser(prog=f"conclave {cmd}")
    p.add_argument("text", nargs="*")
    p.add_argument("--project", default="default")
    p.add_argument("--source")
    p.add_argument("--signature")
    p.add_argument("--lesson")
    p.add_argument("--verdict")
    p.add_argument("--id")
    p.add_argument("--mode", default="stuck")
    p.add_argument("--target", default="")
    p.add_argument("--distill", action="store_true")
    p.add_argument("--online", action="store_true")
    p.add_argument("--verified", action="store_true")
    p.add_argument("--corpus", action="store_true",
                   help="target the shared, project-agnostic corpus instead of a project")
    a = p.parse_args(rest)
    text = " ".join(a.text)
    # The shared corpus is a reserved namespace stored at data/corpus/, not under a project.
    proj = config.CORPUS if a.corpus else a.project
    cfg = config.load(root)

    if cmd == "doctor":
        from conclave import heal
        rpt = heal.doctor(root, cfg, cfg.secret, online=a.online)
        for c in rpt["checks"]:
            print(f"[{'ok' if c['ok'] else 'XX'}] {c['name']}: {c['detail']}")
        return 0 if rpt["ok"] else 1

    if cmd == "stats":
        from conclave import jobs
        print(jobs.stats(root, a.project))
        return 0

    if cmd == "poll":
        from conclave import jobs
        print(jobs.poll(root, a.project, a.id or text))
        return 0

    if cmd == "list":
        from conclave import jobs
        print(jobs.list_jobs(root, a.project))
        return 0

    if cmd == "recall":
        try:
            idx, emb = _deps(root, cfg)
            from conclave.memory import Memory
            hits = Memory(root, proj, idx, emb).recall(text)
            for h in hits:
                print(f"- {h['lesson']}")
            if not hits:
                print("(no results)")
        except Exception as e:
            print(f"(recall unavailable: {str(e)[:120]})")
        return 0

    if cmd == "note":
        if not a.verified:
            print("refusing: pass --verified (only verified lessons are stored)", file=sys.stderr)
            return 1
        idx, emb = _deps(root, cfg)
        from conclave.memory import Memory
        ok = Memory(root, proj, idx, emb).note(a.signature or text, a.lesson or text, verified=True)
        print("noted" if ok else "rejected")
        return 0 if ok else 1

    if cmd == "teach":
        idx, emb = _deps(root, cfg)
        from conclave import teach as t
        r = t.teach(root, proj, Path(a.source), distill=a.distill, embedder=emb, index=idx)
        print(f"added={r['added']} updated={r['updated']} pruned={r['pruned']}")
        return 0

    if cmd == "compile":
        idx, emb = _deps(root, cfg)
        from conclave import compile as cc
        from conclave.store import open_store
        out = cc.compile(root, cfg, a.project, embedder=emb, store=open_store(cfg, root))
        print(f"{out['version']}: {out['num_examples']} examples, {len(out['shards'])} shards")
        return 0

    if cmd == "outcome":
        from conclave import jobs
        jobs.outcome(root, a.project, a.id, a.verdict)
        print(f"{a.id} -> {a.verdict}")
        return 0

    if cmd == "dispatch":
        from conclave import jobs
        brief = Path(a.source).read_text() if a.source else text
        jid = jobs.dispatch(root, a.project, a.mode, a.target or a.mode, brief, [])
        print(jid)
        return 0

    if cmd == "ask":
        os.environ.setdefault("CONCLAVE_ENABLED", "1")
        cfg = config.load(root)
        idx, emb = _deps(root, cfg)
        from conclave import harness
        from conclave.memory import Memory
        from conclave.metrics import MetricsStore
        brief = Path(a.source).read_text() if a.source else text
        r = harness.escalate(cfg=cfg, secret=cfg.secret, mode=a.mode, brief=brief,
                             project=a.project, root=root, memory=Memory(root, a.project, idx, emb),
                             metrics=MetricsStore(root, a.project), target=a.target or None)
        print(f"[{r['status']}]")
        print(r.get("output", ""))
        return 0 if r["status"] in ("ok", "panel_only") else 2

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
