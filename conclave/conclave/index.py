"""Hybrid retrieval: vector ANN + lexical full-text search, fused by Reciprocal
Rank Fusion. Both arms live in LanceDB -- vector, text and meta are persisted
columns and both indexes are on-disk, so a fresh process (every CLI invocation)
queries them directly without rebuilding anything. The query path touches only
the indexes (sub-linear) and reads back only the k matched rows, so recall
latency is independent of total corpus size (the perf contract). Indexing cost is
paid by the writer (teach/note), never the reader.
"""
from __future__ import annotations

import abc
import json
from pathlib import Path

import lancedb

_ANN_THRESHOLD = 256          # build a vector ANN index past this many rows; exact below
_RRF_K = 60


class IndexBackend(abc.ABC):
    @abc.abstractmethod
    def upsert(self, items: list) -> None: ...

    @abc.abstractmethod
    def search_vec(self, vector, k: int) -> list: ...

    @abc.abstractmethod
    def search_text(self, query_text: str, k: int) -> list: ...

    @abc.abstractmethod
    def size(self) -> int: ...


class LanceDBIndex(IndexBackend):
    """Vector + full-text over one memory-mapped LanceDB table. Rows are
    {id, vector, text, meta(JSON)}; upsert is by id (re-teach updates in place).
    Both search methods return (id, score, meta) so meta needs no separate fetch."""

    def __init__(self, path: Path):
        self.db = lancedb.connect(str(path))
        self._vec_indexed = False
        try:
            self.tbl = self.db.open_table("items")
        except Exception:
            self.tbl = None

    def upsert(self, items: list) -> None:
        rows = [{"id": str(it["id"]),
                 "vector": [float(x) for x in it["vector"]],
                 "text": it.get("text", "") or "",
                 "meta": json.dumps(it.get("meta", {}))}
                for it in items]
        if not rows:
            return
        if self.tbl is None:
            self.tbl = self.db.create_table("items", data=rows, mode="overwrite")
        else:
            (self.tbl.merge_insert("id")
             .when_matched_update_all()
             .when_not_matched_insert_all()
             .execute(rows))
        self._reindex()

    def _reindex(self) -> None:
        # Lexical FTS: persisted, queried sub-linearly. Rebuilt at write time so the
        # whole corpus stays indexed (new rows are also findable via the unindexed tail).
        try:
            self.tbl.create_fts_index("text", use_tantivy=False, replace=True)
        except Exception:
            pass  # tiny/empty table -- flat FTS scan still works
        # Vector ANN past the threshold; exact KNN below (correctness unchanged).
        if not self._vec_indexed and self.tbl.count_rows() >= _ANN_THRESHOLD:
            try:
                self.tbl.create_index(metric="cosine")
                self._vec_indexed = True
            except Exception:
                pass

    def _rows(self, query, k: int) -> list:
        # No .select(): LanceDB warns about score autoprojection when output columns are
        # restricted. We read back only id+meta anyway, so just take the full rows.
        out = []
        for r in query.limit(k).to_list():
            try:
                meta = json.loads(r.get("meta") or "{}")
            except Exception:
                meta = {}
            out.append((r["id"], meta))
        return out

    def search_vec(self, vector, k: int) -> list:
        if self.tbl is None:
            return []
        return self._rows(self.tbl.search([float(x) for x in vector]), k)

    def search_text(self, query_text: str, k: int) -> list:
        if self.tbl is None or not (query_text or "").strip():
            return []
        try:
            return self._rows(self.tbl.search(query_text, query_type="fts"), k)
        except Exception:
            return []

    def size(self) -> int:
        return 0 if self.tbl is None else self.tbl.count_rows()


class HybridIndex:
    """Fuses the vector ANN + FTS arms by RRF. Stateless across processes: all
    persisted state lives in the LanceDB table behind `vec`."""

    def __init__(self, vec: LanceDBIndex, hybrid_bm25: bool = True):
        self.vec = vec
        self.hybrid_bm25 = hybrid_bm25

    def upsert(self, items: list) -> None:
        self.vec.upsert(items)

    def recall(self, query_vec, query_text, k: int) -> list:
        fused: dict = {}
        meta: dict = {}
        for rank, (rid, m) in enumerate(self.vec.search_vec(query_vec, k * 4)):
            fused[rid] = fused.get(rid, 0.0) + 1.0 / (_RRF_K + rank)
            meta[rid] = m
        if self.hybrid_bm25:
            for rank, (rid, m) in enumerate(self.vec.search_text(query_text, k * 4)):
                fused[rid] = fused.get(rid, 0.0) + 1.0 / (_RRF_K + rank)
                meta.setdefault(rid, m)
        ranked = sorted(fused.items(), key=lambda kv: kv[1], reverse=True)[:k]
        return [(rid, score, meta.get(rid, {})) for rid, score in ranked]

    def size(self) -> int:
        return self.vec.size()


def open_index(cfg, path) -> HybridIndex:
    Path(path).mkdir(parents=True, exist_ok=True)
    vec = LanceDBIndex(Path(path) / "lance")
    return HybridIndex(vec, hybrid_bm25=getattr(cfg, "hybrid_bm25", True))
