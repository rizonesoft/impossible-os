"""Pluggable bulk-artifact tier. LocalStore (git+Releases small scale) now;
S3/R2 backend slots in behind the same interface at scale. sha256-verified."""
from __future__ import annotations

import abc
import hashlib
import json
from pathlib import Path


class ArtifactStore(abc.ABC):
    @abc.abstractmethod
    def put(self, key: str, data: bytes) -> dict: ...

    @abc.abstractmethod
    def get(self, key: str) -> bytes: ...

    @abc.abstractmethod
    def list(self) -> list: ...

    @abc.abstractmethod
    def manifest(self) -> list: ...


class LocalStore(ArtifactStore):
    def __init__(self, base: Path):
        self.base = Path(base)
        self.base.mkdir(parents=True, exist_ok=True)
        self.mpath = self.base / "manifest.jsonl"

    def _man(self) -> list:
        try:
            return [json.loads(ln) for ln in self.mpath.read_text(encoding="utf-8").splitlines() if ln]
        except Exception:
            return []

    def put(self, key: str, data: bytes) -> dict:
        p = self.base / key
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        meta = {"key": key, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        man = [m for m in self._man() if m["key"] != key] + [meta]
        self.mpath.write_text("\n".join(json.dumps(m) for m in man) + "\n", encoding="utf-8")
        return meta

    def get(self, key: str) -> bytes:
        data = (self.base / key).read_bytes()
        want = next((m["sha256"] for m in self._man() if m["key"] == key), None)
        if want and hashlib.sha256(data).hexdigest() != want:
            raise ValueError(f"sha256 mismatch for {key}")
        return data

    def list(self) -> list:
        return [m["key"] for m in self._man()]

    def manifest(self) -> list:
        return self._man()


class S3Store(ArtifactStore):
    def __init__(self, *a, **k):
        raise NotImplementedError(
            "S3/R2 backend not built yet -- use git-releases until the corpus approaches ~1 GB")

    def put(self, key, data): ...  # pragma: no cover
    def get(self, key): ...  # pragma: no cover
    def list(self): ...  # pragma: no cover
    def manifest(self): ...  # pragma: no cover


def open_store(cfg, root) -> ArtifactStore:
    if cfg.store_backend in ("git-releases", "local"):
        return LocalStore(Path(root) / "data")
    return S3Store()
