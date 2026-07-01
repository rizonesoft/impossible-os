"""Load conclave.toml + secret into a Config. Fail-open on a missing secret."""
from __future__ import annotations

import json
import os
import tomllib
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Config:
    panel: list
    judge: str
    embedders: list
    embed_default: str
    ensemble_rrf: bool
    hybrid_bm25: bool
    max_calls: int
    min_credits: float
    workers: int
    model_timeout_s: int
    judge_timeout_s: int
    compile_every_days: int
    compile_new_examples: int
    shard_max_mib: int
    index_backend: str
    store_backend: str
    enabled: bool
    secret: str | None


CORPUS = "corpus"


def namespace_dir(root, name: str) -> Path:
    """Base dir for a knowledge namespace. The shared, project-agnostic corpus lives at
    data/corpus/; a specific project lives at data/projects/<name>/."""
    root = Path(root)
    if name == CORPUS:
        return root / "data" / "corpus"
    return root / "data" / "projects" / name


def _read_secret(root: Path) -> str | None:
    # Prefer a structured secrets.json ({"openrouter_api_key": "..."}); fall back to
    # the legacy plain `secret` file (whole file = the key). Both stay gitignored.
    try:
        obj = json.loads((root / "secrets.json").read_text(encoding="utf-8"))
        if isinstance(obj, dict):
            for k in ("openrouter_api_key", "api_key", "openrouter", "secret"):
                v = obj.get(k)
                if isinstance(v, str) and v.strip():
                    return v.strip()
    except Exception:
        pass
    try:
        s = (root / "secret").read_text(encoding="utf-8").strip()
        return s or None
    except Exception:
        return None


def load(root: Path) -> Config:
    with (Path(root) / "conclave.toml").open("rb") as f:
        t = tomllib.load(f)
    rt = t.get("runtime", {})
    pa = t.get("panel", {})
    ju = t.get("judge", {})
    em = t.get("embedders", {})
    co = t.get("compile", {})
    ix = t.get("index", {})
    st = t.get("store", {})
    enabled_env = rt.get("enabled_env", "CONCLAVE_ENABLED")
    return Config(
        panel=pa.get("models", []),
        judge=ju.get("model", ""),
        embedders=em.get("models", []),
        embed_default=em.get("default", ""),
        ensemble_rrf=em.get("ensemble_rrf", False),
        hybrid_bm25=em.get("hybrid_bm25", True),
        max_calls=rt.get("max_calls", 3),
        min_credits=rt.get("min_credits", 2.0),
        workers=rt.get("workers", 6),
        model_timeout_s=rt.get("model_timeout_s", 480),
        judge_timeout_s=rt.get("judge_timeout_s", 480),
        compile_every_days=co.get("every_days", 7),
        compile_new_examples=co.get("or_new_examples", 200),
        shard_max_mib=co.get("shard_max_mib", 90),
        index_backend=ix.get("backend", "lancedb"),
        store_backend=st.get("backend", "git-releases"),
        enabled=os.environ.get(enabled_env) == "1",
        secret=_read_secret(Path(root)),
    )
