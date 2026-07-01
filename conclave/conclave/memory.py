"""Memory: verified-lesson note() + recall() over the hybrid index + lesson store.

recall is the cheap agent verb (embed + index lookup, no panel). note refuses
unverified lessons so the corpus is never polluted by unconfirmed guesses.
"""
from __future__ import annotations

import hashlib
from pathlib import Path

from conclave import config


class Memory:
    def __init__(self, root, project: str, index, embedder):
        self.root = Path(root)
        self.project = project
        self.index = index
        self.embedder = embedder
        self.lessons_dir = config.namespace_dir(root, project) / "lessons"

    def recall(self, query: str, k: int = 5) -> list:
        qv = self.embedder.embed_query(query)
        hits = self.index.recall(query_vec=qv, query_text=query, k=k)
        out = []
        for rid, score, meta in hits:
            lesson = meta.get("lesson") or meta.get("synthesis") or meta.get("text", "")
            out.append({"lesson": lesson, "meta": meta, "score": score})
        return out

    def note(self, signature: str, lesson: str, *, verified: bool) -> bool:
        if not verified:
            return False
        sig_hash = hashlib.sha1(signature.encode("utf-8")).hexdigest()[:12]
        rid = f"lesson:{self.project}:{sig_hash}"
        vec = self.embedder.embed_query(lesson)
        self.index.upsert([{
            "id": rid, "text": f"{signature}\n{lesson}", "vector": vec,
            "meta": {"type": "lesson", "project": self.project,
                     "signature": signature, "lesson": lesson}}])
        self.lessons_dir.mkdir(parents=True, exist_ok=True)
        (self.lessons_dir / f"{sig_hash}.md").write_text(
            f"# {signature}\n\n{lesson}\n", encoding="utf-8")
        return True
