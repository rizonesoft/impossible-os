"""Shared test fixtures. Extended per task as modules come online."""
import pathlib
import sys

import pytest

# Make `import conclave` work without an editable install.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from conclave.config import Config  # noqa: E402

MIN_TOML = (
    '[runtime]\n[panel]\nmodels=["m"]\n[judge]\nmodel="j"\n'
    '[embedders]\nmodels=["a","b","c"]\ndefault="a"\n'
    '[compile]\n[index]\nbackend="lancedb"\n[store]\nbackend="local"\n'
)

_DEFAULTS = dict(
    panel=["m"], judge="j", embedders=["a", "b", "c"], embed_default="a",
    ensemble_rrf=False, hybrid_bm25=True, max_calls=3, min_credits=2.0, workers=2,
    model_timeout_s=5, judge_timeout_s=5, compile_every_days=7, compile_new_examples=200,
    shard_max_mib=90, index_backend="lancedb", store_backend="local",
    enabled=True, secret="k",
)


@pytest.fixture
def cfg():
    """Factory: cfg(**overrides) -> Config with safe test defaults."""
    def make(**over):
        d = dict(_DEFAULTS)
        d.update(over)
        return Config(**d)
    return make


@pytest.fixture
def make_repo():
    """Factory: write a minimal repo (conclave.toml + secret.example) at a path."""
    def make(tmp_path):
        p = pathlib.Path(tmp_path)
        (p / "conclave.toml").write_text(MIN_TOML)
        (p / "secret.example").write_text("sk-or-v1-REPLACE-WITH-YOUR-OPENROUTER-API-KEY\n")
        return p
    return make


@pytest.fixture
def fake_embedder():
    """Deterministic, offline embedder. Vectors are not used by fake_index."""
    class _E:
        def embed_query(self, text):
            h = abs(hash(text))
            return [float((h >> i) & 0xFF) for i in range(0, 32, 8)]

        def embed_docs(self, texts):
            return [self.embed_query(t) for t in texts]

        def embed_query_ensemble(self, text):
            return [("m", self.embed_query(text))]
    return _E()


@pytest.fixture
def fake_index():
    """In-memory HybridIndex stub: recall ranks by token overlap with the query text."""
    import re

    def tok(s):
        return set(re.findall(r"[a-z0-9]+", (s or "").lower()))

    class _I:
        def __init__(self):
            self.items = []

        def upsert(self, items):
            for it in items:
                self.items.append((str(it["id"]), it.get("text", ""), it.get("meta", {})))

        def recall(self, query_vec, query_text, k):
            q = tok(query_text)
            scored = [(len(q & tok(text)), rid, meta)
                      for rid, text, meta in self.items if q & tok(text)]
            scored.sort(key=lambda x: x[0], reverse=True)
            return [(rid, float(ov), meta) for ov, rid, meta in scored[:k]]

        def size(self):
            return len(self.items)
    return _I()


@pytest.fixture
def local_store(tmp_path):
    from conclave.store import LocalStore
    return LocalStore(tmp_path / "store")


@pytest.fixture
def seed_runs():
    """Write n resolved run transcripts under data/projects/<project>/runs.jsonl."""
    from conclave import compile as cc

    def make(root, project, n):
        for i in range(n):
            cc.record_run(root, project, run_id=f"R{i}",
                          brief=f"problem number {i} about an smp race in the scheduler",
                          synthesis=f"fix number {i}: take the runqueue spinlock before X",
                          verdict="resolved")
    return make
