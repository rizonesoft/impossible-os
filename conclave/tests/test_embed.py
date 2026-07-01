from conclave import embed as embed_mod
from conclave.embed import Embedder


def test_default_uses_default_model(monkeypatch, cfg):
    calls = {}

    def fake_embed(secret, model, inputs, timeout):
        calls["model"] = model
        return [[0.1] * 4 for _ in inputs], 0.0

    monkeypatch.setattr(embed_mod.http, "embed", fake_embed)
    e = Embedder(cfg(embed_default="qwen/qwen3-embedding-8b"), "k")
    v = e.embed_query("hello")
    assert calls["model"] == "qwen/qwen3-embedding-8b" and len(v) == 4


def test_embed_docs_batches(monkeypatch, cfg):
    monkeypatch.setattr(embed_mod.http, "embed", lambda s, m, i, t: ([[1.0] for _ in i], 0.0))
    e = Embedder(cfg(), "k")
    assert e.embed_docs(["a", "b", "c"]) == [[1.0], [1.0], [1.0]]


def test_ensemble_calls_all_three(monkeypatch, cfg):
    seen = []
    monkeypatch.setattr(embed_mod.http, "embed",
                        lambda s, m, i, t: (seen.append(m) or [[0.0] * 2 for _ in i], 0.0))
    e = Embedder(cfg(embedders=["a", "b", "c"], ensemble_rrf=True), "k")
    out = e.embed_query_ensemble("q")
    assert [m for m, _ in out] == ["a", "b", "c"] and seen == ["a", "b", "c"]
