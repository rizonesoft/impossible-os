import time

import numpy as np

from conclave.index import open_index


def _vec(seed, d=8):
    rng = np.random.default_rng(seed)
    return [float(x) for x in rng.random(d)]


def test_hybrid_recall_ranks_relevant_first(tmp_path, cfg):
    idx = open_index(cfg(), tmp_path / "idx")
    idx.upsert([{"id": str(i), "text": f"doc about topic {i}", "vector": _vec(i), "meta": {"i": i}}
                for i in range(50)])
    hits = idx.recall(query_vec=_vec(7), query_text="topic 7", k=5)
    ids = [h[0] for h in hits]
    assert "7" in ids
    assert hits[0][2]["i"] == int(hits[0][0])      # meta travels with the hit


def test_bm25_only_path_still_recalls(tmp_path, cfg):
    idx = open_index(cfg(hybrid_bm25=True), tmp_path / "idx")
    idx.upsert([{"id": "a", "text": "spinlock fixes the smp race", "vector": _vec(1), "meta": {}},
                {"id": "b", "text": "unrelated boot timing note", "vector": _vec(2), "meta": {}}])
    hits = idx.recall(query_vec=_vec(99), query_text="smp race spinlock", k=2)
    assert hits[0][0] == "a"


def test_recall_survives_reopen(tmp_path, cfg):
    # A separate CLI invocation re-opens the index from disk. Previously only id+vector
    # persisted (text/meta + BM25 were in-memory), so taught lessons came back blank.
    path = tmp_path / "idx"
    open_index(cfg(), path).upsert([
        {"id": "a", "text": "spinlock fixes the smp race", "vector": _vec(1),
         "meta": {"lesson": "take the runqueue spinlock"}},
        {"id": "b", "text": "unrelated boot timing note", "vector": _vec(2),
         "meta": {"lesson": "irrelevant"}}])

    reopened = open_index(cfg(), path)             # fresh handle == fresh process
    # mismatched query vector: only persisted lexical (FTS) can rank "a" first
    hits = reopened.recall(query_vec=_vec(99), query_text="smp race spinlock", k=2)
    assert hits[0][0] == "a"
    assert hits[0][2]["lesson"] == "take the runqueue spinlock"   # meta survived reopen


def test_latency_flat_small_to_large(tmp_path, cfg):
    # Contract: query latency must not scale linearly with N.
    def q_time(n):
        idx = open_index(cfg(), tmp_path / f"idx{n}")
        idx.upsert([{"id": str(i), "text": f"d{i}", "vector": _vec(i), "meta": {}} for i in range(n)])
        t = time.perf_counter()
        idx.recall(query_vec=_vec(1), query_text="d1", k=5)
        return time.perf_counter() - t

    small, large = q_time(1000), q_time(50000)
    assert large < small * 5 + 0.05          # near-flat, not ~50x linear
