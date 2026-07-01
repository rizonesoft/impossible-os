"""Corpus teaching: ingest existing project files so the harness knows the project
before any escalation. Incremental by content hash -- re-teach updates/prunes.

teach(root, project, source, *, distill, embedder, index) -> {added, updated, pruned}.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

from conclave import config, http

_CHUNK_WORDS = 400
_OVERLAP = 50
_DISTILL_MODEL = "z-ai/glm-5.2"


def _chunks(text: str) -> list:
    words = text.split()
    if not words:
        return []
    out, i = [], 0
    while i < len(words):
        out.append(" ".join(words[i:i + _CHUNK_WORDS]))
        if i + _CHUNK_WORDS >= len(words):
            break
        i += _CHUNK_WORDS - _OVERLAP
    return out


def _ledger_path(root: Path, project: str) -> Path:
    return config.namespace_dir(root, project) / "teach-index.json"


def _load_ledger(p: Path) -> dict:
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return {}


def _distill_file(secret: str, text: str) -> str:
    """Best-effort LLM summary into key facts + signatures; fall back to raw text."""
    try:
        msgs = [{"role": "system", "content":
                 "Summarize this project document into terse key facts and the signatures "
                 "(symbols, constants, invariants) a debugger would search for. No prose."},
                {"role": "user", "content": text[:20000]}]
        content, _, _, _ = http.chat(secret, _DISTILL_MODEL, msgs, 120)
        return content or text
    except Exception:
        return text


def teach(root, project, source, *, distill, embedder, index) -> dict:
    root, source = Path(root), Path(source)
    files = [source] if source.is_file() else sorted(p for p in source.rglob("*") if p.is_file())
    anchor = source if source.is_dir() else source.parent

    new_state: dict = {}
    chunk_texts: dict = {}
    for f in files:
        try:
            text = f.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        rel = str(f.relative_to(anchor))
        if distill:
            text = _distill_file(getattr(embedder, "secret", ""), text)
        for ci, ctext in enumerate(_chunks(text)):
            cid = f"corpus:{project}:{rel}#{ci}"
            new_state[cid] = hashlib.sha1(ctext.encode("utf-8")).hexdigest()
            chunk_texts[cid] = (ctext, rel)

    lp = _ledger_path(root, project)
    old = _load_ledger(lp)
    added = [c for c in new_state if c not in old]
    updated = [c for c in new_state if c in old and old[c] != new_state[c]]
    pruned = [c for c in old if c not in new_state]

    to_embed = added + updated
    if to_embed:
        vecs = embedder.embed_docs([chunk_texts[c][0] for c in to_embed])
        index.upsert([{"id": c, "text": chunk_texts[c][0], "vector": v,
                       "meta": {"type": "corpus", "project": project, "path": chunk_texts[c][1],
                                "text": chunk_texts[c][0]}}
                      for c, v in zip(to_embed, vecs)])

    lp.parent.mkdir(parents=True, exist_ok=True)
    lp.write_text(json.dumps(new_state, indent=2), encoding="utf-8")
    return {"added": len(added), "updated": len(updated), "pruned": len(pruned)}
