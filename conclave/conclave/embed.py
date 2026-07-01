"""Embedders via OpenRouter (same key). Default single model for indexing/queries;
opt-in ensemble returns all three (model, vector) pairs for per-model RRF."""
from __future__ import annotations

from conclave import http


class Embedder:
    def __init__(self, cfg, secret):
        self.secret = secret
        self.default = cfg.embed_default
        self.models = cfg.embedders
        self.ensemble = cfg.ensemble_rrf
        self.timeout = getattr(cfg, "model_timeout_s", 480)

    def embed_docs(self, texts: list) -> list:
        if not texts:
            return []
        vecs, _ = http.embed(self.secret, self.default, texts, self.timeout)
        return vecs

    def embed_query(self, text: str) -> list:
        vecs, _ = http.embed(self.secret, self.default, [text], self.timeout)
        return vecs[0]

    def embed_query_ensemble(self, text: str) -> list:
        out = []
        for m in self.models:
            vecs, _ = http.embed(self.secret, m, [text], self.timeout)
            out.append((m, vecs[0]))
        return out
