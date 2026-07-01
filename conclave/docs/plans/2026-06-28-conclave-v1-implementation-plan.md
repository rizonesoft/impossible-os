# Conclave v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build Conclave v1 -- a self-improving, cross-project reasoning harness that behaves like one model (problem in, answer out), internally running a swappable cloud panel + Opus judge, with memory, automatic learning, self-healing, and a knowledge-compilation pipeline, scaling from 5 MB to 200 GB at flat query latency.

**Architecture:** A Python package `conclave/` of small single-responsibility modules behind a `conclave` CLI. The reasoning harness orchestrates retrieve -> decompose -> structured-prompt -> panel+judge -> verify. Bulk data lives in a pluggable `ArtifactStore` (git+Releases small -> S3/R2 at scale); retrieval runs on a pluggable `IndexBackend` (LanceDB HNSW + BM25, quantized, RAM/mmap-resident) so query latency is independent of corpus size. Impossible OS becomes the first client via a thin connector.

**Tech Stack:** Python 3.12 (stdlib `tomllib`, `urllib`, `concurrent.futures`), `pytest`, `lancedb` (embedded ANN), `rank-bm25` (lexical), `numpy`. Panel + embedders called over OpenRouter HTTP (existing key). No GPU, no server at small scale.

## Global Constraints

- Python 3.12+; stdlib HTTP (`urllib.request`) -- no `requests`/`httpx` dependency in the core caller.
- Secrets live ONLY in `~/conclave/secret` (gitignored). No real key in any `*.example` or tracked file -- a pre-commit guard enforces this (scrubbed-incident rule).
- Off by default: every paid path is gated on env `CONCLAVE_ENABLED == "1"` AND a present `secret`. Fail-open for the caller, fail-closed on spend (balance floor `min_credits`, per-run `max_calls`).
- The panel NEVER does web-search.
- `recall` (retrieval) p99 < 50 ms independent of total data size; the query path touches only the index, never raw data.
- All file writes are atomic (temp + rename); all state reads fail-open to a sane default.
- Every task ends green: `pytest -q` passes and the task is committed.
- ASCII only in code/serial output (`--`, not unicode dashes).

## File structure

```
conclave/
  __init__.py
  config.py        load conclave.toml (tomllib) + secret; Config dataclass
  http.py          OpenRouter HTTP: chat_completion(), embed(), remaining_credits()
  panel.py         probe() one model; run_panel() parallel
  judge.py         discard-synthesis judge
  metrics.py       per-run metrics line, totals ledger, record_outcome
  store.py         ArtifactStore ABC + LocalStore + (stub) S3Store; sha256 manifest
  index.py         IndexBackend ABC + LanceDBIndex (HNSW) + BM25 + hybrid RRF; recall()
  embed.py         3 embedders via OpenRouter + single/RRF-ensemble
  memory.py        recall()/note() over index + lesson store
  harness.py       escalate(): retrieve->decompose->prompt->panel+judge->verify
  ladder.py        async job queue: dispatch/poll/list/outcome/stats
  teach.py         corpus ingestion: walk->chunk->(distill)->embed->index
  learn.py         policies from outcomes (panel/prompt/routing) + auto-label
  heal.py          doctor() + atomic state repair
  compile.py       cadence check, corpus+vector shards, ledger, eval harness
  cli.py           `conclave` entrypoint: ask/dispatch/recall/note/teach/compile/outcome/stats/doctor
tests/
  test_*.py        one per module
pyproject.toml     package + deps + pytest config
```

Phase A (core escalation engine): Tasks 1-9. Phase B (learning + lifecycle): Tasks 10-16.

---

## Task 1: Scaffold, config, secret guard

**Files:**
- Create: `pyproject.toml`, `conclave/__init__.py`, `conclave/config.py`, `tests/test_config.py`, `tests/conftest.py`, `scripts/check-no-secret.sh`
- Modify: `.gitignore` (already has `secret`)

**Shared test fixtures (`tests/conftest.py`)** -- referenced by later tasks; create here:
- `_cfg(**over)` -> a `Config` with safe defaults (panel `["m"]`, judge `"j"`, embedders `["a","b","c"]`, default `"a"`, `index_backend="lancedb"`, `hybrid_bm25=True`, `store_backend="local"`, `max_calls=3`, `min_credits=2.0`, `workers=2`), each overridable by kwarg (`default=`, `models=`, `ensemble=`, `shard_max_mib=`, `new_examples=`).
- `fake_embedder` -> object with `embed_query(text)`/`embed_docs(texts)`/`embed_query_ensemble(text)` returning deterministic vectors from `hash(text)`.
- `fake_index` -> in-memory `HybridIndex`-compatible stub: `upsert`, `recall(query_vec, query_text, k)` returning items whose `text` shares tokens with `query_text`.
- `local_store` -> `LocalStore(tmp_path/"store")` (after Task 5 exists; until then a tiny in-memory stub with `put/get/list/manifest`).
- `seed_runs(root, project, n)` -> writes `n` labeled run records under `data/projects/<project>/` (metrics + outcomes) for compile tests.
- `_make_repo(tmp_path)` -> writes a minimal `conclave.toml` + `secret.example` (+ `git init`) and returns the path; `_MIN_TOML` is that toml string.
- `fakes` -> a bundle exposing `.cfg`, `.memory` (with `.add(text)` + `recall`), `.metrics` (stub `append_run`/`add_run_to_totals`/`record_outcome`), `.panel_brief_contained(substr)` (records the brief passed to `panel.run`).

**Interfaces:**
- Produces: `config.load(root: Path) -> Config`; `Config` dataclass with `.panel: list[str]`, `.judge: str`, `.embedders: list[str]`, `.embed_default: str`, `.ensemble_rrf: bool`, `.max_calls: int`, `.min_credits: float`, `.workers: int`, `.model_timeout_s: int`, `.judge_timeout_s: int`, `.compile_every_days: int`, `.compile_new_examples: int`, `.shard_max_mib: int`, `.index_backend: str`, `.store_backend: str`, `.enabled: bool` (from env), `.secret: str|None`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_config.py
import os, pathlib, tomllib
from conclave import config

def test_load_reads_toml_and_secret(tmp_path, monkeypatch):
    (tmp_path / "conclave.toml").write_text(
        '[runtime]\nmax_calls=3\nmin_credits=2.0\nworkers=6\n'
        'model_timeout_s=480\njudge_timeout_s=480\n'
        '[panel]\nmodels=["a","b"]\nweb_search=false\n'
        '[judge]\nmodel="j"\n'
        '[embedders]\nmodels=["e1","e2"]\ndefault="e1"\nensemble_rrf=false\nhybrid_bm25=true\n'
        '[compile]\nevery_days=7\nor_new_examples=200\nshard_max_mib=90\n'
        '[index]\nbackend="lancedb"\n[store]\nbackend="git-releases"\n')
    (tmp_path / "secret").write_text("sk-or-v1-xxx\n")
    monkeypatch.setenv("CONCLAVE_ENABLED", "1")
    c = config.load(tmp_path)
    assert c.panel == ["a", "b"] and c.judge == "j"
    assert c.embed_default == "e1" and c.embedders == ["e1", "e2"]
    assert c.max_calls == 3 and c.enabled is True and c.secret == "sk-or-v1-xxx"

def test_disabled_without_env(tmp_path, monkeypatch):
    (tmp_path / "conclave.toml").write_text('[runtime]\n[panel]\nmodels=[]\n[judge]\nmodel="j"\n[embedders]\nmodels=[]\ndefault=""\n[compile]\n[index]\n[store]\n')
    monkeypatch.delenv("CONCLAVE_ENABLED", raising=False)
    assert config.load(tmp_path).enabled is False
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_config.py -q`
Expected: FAIL (`ModuleNotFoundError: conclave.config`).

- [ ] **Step 3: Implement `conclave/config.py`**

```python
from __future__ import annotations
import os, tomllib
from dataclasses import dataclass
from pathlib import Path

@dataclass
class Config:
    panel: list; judge: str; embedders: list; embed_default: str
    ensemble_rrf: bool; hybrid_bm25: bool
    max_calls: int; min_credits: float; workers: int
    model_timeout_s: int; judge_timeout_s: int
    compile_every_days: int; compile_new_examples: int; shard_max_mib: int
    index_backend: str; store_backend: str
    enabled: bool; secret: str | None

def _read_secret(root: Path) -> str | None:
    p = root / "secret"
    try:
        s = p.read_text(encoding="utf-8").strip()
        return s or None
    except Exception:
        return None

def load(root: Path) -> Config:
    with (root / "conclave.toml").open("rb") as f:
        t = tomllib.load(f)
    rt, pa, ju = t.get("runtime", {}), t.get("panel", {}), t.get("judge", {})
    em, co = t.get("embedders", {}), t.get("compile", {})
    ix, st = t.get("index", {}), t.get("store", {})
    return Config(
        panel=pa.get("models", []), judge=ju.get("model", ""),
        embedders=em.get("models", []), embed_default=em.get("default", ""),
        ensemble_rrf=em.get("ensemble_rrf", False), hybrid_bm25=em.get("hybrid_bm25", True),
        max_calls=rt.get("max_calls", 3), min_credits=rt.get("min_credits", 2.0),
        workers=rt.get("workers", 6), model_timeout_s=rt.get("model_timeout_s", 480),
        judge_timeout_s=rt.get("judge_timeout_s", 480),
        compile_every_days=co.get("every_days", 7), compile_new_examples=co.get("or_new_examples", 200),
        shard_max_mib=co.get("shard_max_mib", 90),
        index_backend=ix.get("backend", "lancedb"), store_backend=st.get("backend", "git-releases"),
        enabled=os.environ.get(rt.get("enabled_env", "CONCLAVE_ENABLED")) == "1",
        secret=_read_secret(root))
```

- [ ] **Step 4: Add the secret guard + pyproject**

```toml
# pyproject.toml
[project]
name = "conclave"
version = "0.1.0"
requires-python = ">=3.12"
dependencies = ["lancedb>=0.6", "rank-bm25>=0.2", "numpy>=1.26"]
[project.scripts]
conclave = "conclave.cli:main"
[tool.pytest.ini_options]
addopts = "-q"
testpaths = ["tests"]
```

```bash
# scripts/check-no-secret.sh -- pre-commit guard: no real OpenRouter key in tracked files
#!/usr/bin/env bash
set -euo pipefail
if git diff --cached --name-only | grep -qx secret; then
  echo "REFUSING: 'secret' is staged -- it must stay gitignored"; exit 1
fi
# block a real-looking key anywhere staged EXCEPT the placeholder in secret.example
if git diff --cached -U0 | grep -E '^\+' | grep -E 'sk-or-v1-[0-9a-f]{32,}' >/dev/null; then
  echo "REFUSING: a real-looking OpenRouter key is staged"; exit 1
fi
```

- [ ] **Step 5: Run + verify pass**

Run: `pip install -e . && pytest tests/test_config.py -q`
Expected: 2 passed. Wire the guard: `git config core.hooksPath .githooks` and add `.githooks/pre-commit` calling `bash scripts/check-no-secret.sh`.

- [ ] **Step 6: Commit**

```bash
git add pyproject.toml conclave/ tests/test_config.py scripts/check-no-secret.sh .githooks/pre-commit
git commit -m "conclave: scaffold + config loader + secret guard"
```

---

## Task 2: OpenRouter HTTP client

**Files:** Create `conclave/http.py`, `tests/test_http.py`. Port from `impossible-os/.fusion/fusion_escalate.py:75-101`.

**Interfaces:**
- Produces: `chat(secret, model, messages, timeout) -> (content:str, cost:float, sec:float, toks:dict)`; `embed(secret, model, inputs:list[str], timeout) -> (vectors:list[list[float]], cost:float)`; `remaining_credits(secret) -> float`.

- [ ] **Step 1: Write the failing test** (mock `urllib.request.urlopen`)

```python
# tests/test_http.py
import io, json, types
from conclave import http

class _Resp(io.BytesIO):
    def __enter__(self): return self
    def __exit__(self, *a): return False

def test_chat_parses_content_cost_tokens(monkeypatch):
    body = {"choices":[{"message":{"content":"hi"}}],
            "usage":{"cost":0.01,"prompt_tokens":10,"completion_tokens":5}}
    monkeypatch.setattr(http.urllib.request, "urlopen",
                        lambda req, timeout: _Resp(json.dumps(body).encode()))
    content, cost, sec, toks = http.chat("k", "m", [{"role":"user","content":"x"}], 5)
    assert content == "hi" and cost == 0.01 and toks == {"prompt":10,"completion":5}

def test_embed_parses_vectors(monkeypatch):
    body = {"data":[{"embedding":[0.1,0.2]},{"embedding":[0.3,0.4]}], "usage":{"cost":0.002}}
    monkeypatch.setattr(http.urllib.request, "urlopen",
                        lambda req, timeout: _Resp(json.dumps(body).encode()))
    vecs, cost = http.embed("k", "e", ["a","b"], 5)
    assert vecs == [[0.1,0.2],[0.3,0.4]] and cost == 0.002
```

- [ ] **Step 2: Run -> FAIL** (`ModuleNotFoundError`).

- [ ] **Step 3: Implement `conclave/http.py`**

```python
from __future__ import annotations
import json, time, urllib.request

BASE = "https://openrouter.ai/api/v1"
CHAT = f"{BASE}/chat/completions"; EMB = f"{BASE}/embeddings"; CREDITS = f"{BASE}/credits"

def _post(url, secret, payload, timeout):
    req = urllib.request.Request(url, data=json.dumps(payload).encode(),
        headers={"Authorization": f"Bearer {secret}", "Content-Type": "application/json"},
        method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())

def chat(secret, model, messages, timeout):
    t0 = time.time(); d = _post(CHAT, secret, {"model": model, "messages": messages}, timeout)
    u = d.get("usage") or {}
    toks = {"prompt": int(u.get("prompt_tokens") or 0), "completion": int(u.get("completion_tokens") or 0)}
    return d["choices"][0]["message"]["content"], float(u.get("cost") or 0), time.time()-t0, toks

def embed(secret, model, inputs, timeout):
    d = _post(EMB, secret, {"model": model, "input": inputs}, timeout)
    vecs = [row["embedding"] for row in d.get("data", [])]
    return vecs, float((d.get("usage") or {}).get("cost") or 0)

def remaining_credits(secret, timeout=10):
    req = urllib.request.Request(CREDITS, headers={"Authorization": f"Bearer {secret}"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = (json.loads(r.read().decode()).get("data") or {})
    return float(d.get("total_credits", 0)) - float(d.get("total_usage", 0))
```

- [ ] **Step 4: Run -> PASS.** `pytest tests/test_http.py -q`.

- [ ] **Step 5: Commit** `git add conclave/http.py tests/test_http.py && git commit -m "conclave: OpenRouter HTTP client (chat + embed + credits)"`

---

## Task 3: Panel + judge

**Files:** Create `conclave/panel.py`, `conclave/judge.py`, `tests/test_panel.py`. Port from `.fusion/fusion_escalate.py:103-134` + `_JUDGE_SYS`.

**Interfaces:**
- Produces: `panel.probe(secret, model, sys_prompt, brief, timeout) -> dict{model,status,content,cost,sec,chars,prompt_tokens,completion_tokens}`; `panel.run(secret, models, sys_prompt, brief, timeout, workers) -> list[dict]`; `judge.judge(secret, model, brief, panel_results, prior, timeout) -> (content,cost,sec,toks)`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_panel.py
from conclave import panel, judge

def test_run_parallel_collects_all(monkeypatch):
    monkeypatch.setattr(panel.http, "chat",
        lambda s,m,msgs,t: (f"a-{m}", 0.01, 1.0, {"prompt":10,"completion":5}))
    out = panel.run("k", ["m1","m2"], "sys", "brief", 5, 2)
    assert {r["model"] for r in out} == {"m1","m2"}
    assert all(r["status"]=="ok" and r["prompt_tokens"]==10 for r in out)

def test_probe_error_is_captured(monkeypatch):
    def boom(*a): raise RuntimeError("503")
    monkeypatch.setattr(panel.http, "chat", boom)
    r = panel.probe("k","m","sys","brief",5)
    assert r["status"]=="error" and r["cost"]==0
```

- [ ] **Step 2: Run -> FAIL.**

- [ ] **Step 3: Implement** (`conclave/panel.py`)

```python
from __future__ import annotations
import concurrent.futures
from conclave import http

def probe(secret, model, sys_prompt, brief, timeout):
    try:
        content, cost, sec, toks = http.chat(secret, model,
            [{"role":"system","content":sys_prompt},{"role":"user","content":brief}], timeout)
        return {"model":model,"status":"ok","content":content,"cost":cost,
                "sec":round(sec,1),"chars":len(content),
                "prompt_tokens":toks["prompt"],"completion_tokens":toks["completion"]}
    except Exception as e:
        return {"model":model,"status":"error","content":"","cost":0,"sec":0,"chars":0,
                "prompt_tokens":0,"completion_tokens":0,"err":str(e)[:160]}

def run(secret, models, sys_prompt, brief, timeout, workers):
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers or len(models) or 1) as ex:
        return list(ex.map(lambda m: probe(secret,m,sys_prompt,brief,timeout), models))
```

`conclave/judge.py` (port `_JUDGE_SYS` + `_judge` verbatim from `.fusion/fusion_escalate.py:64-72,123-134`; the function returns `http.chat(...)`'s 4-tuple).

- [ ] **Step 4: Run -> PASS.**

- [ ] **Step 5: Commit** `git commit -am "conclave: panel (parallel probes) + discard-synthesis judge"`

---

## Task 4: Metrics + totals + outcomes

**Files:** Create `conclave/metrics.py`, `tests/test_metrics.py`. Port `.fusion/fusion_escalate.py:151-244` (`_metrics_append`, `_read_totals`, `_write_totals`, `_totals_add_run`, `record_outcome`) into a `MetricsStore(root, project)` class so paths are project-namespaced under `data/projects/<project>/`.

**Interfaces:**
- Produces: `MetricsStore(root, project)` with `.append_run(record: dict)`, `.add_run_to_totals(*, mode, run_cost, panel_cost, judge_cost, tokens) -> float (cumulative)`, `.record_outcome(run_id, verdict)`, `.totals() -> dict`. Writes `data/projects/<project>/{metrics.jsonl,totals.json,outcomes.jsonl}` atomically.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_metrics.py
import json
from conclave.metrics import MetricsStore

def test_totals_accumulate_and_outcome_backfills(tmp_path):
    ms = MetricsStore(tmp_path, "proj")
    ms.append_run({"run_id":"R1","mode":"stuck","cost_usd":{"run_total":0.5},
                   "intelligence":{"outcome":"unknown"}})
    cum = ms.add_run_to_totals(mode="stuck", run_cost=0.5, panel_cost=0.4, judge_cost=0.1, tokens=100)
    assert abs(cum-0.5) < 1e-9
    ms.record_outcome("R1","resolved")
    t = ms.totals(); assert t["runs"]==1 and t["resolved"]==1
    line = json.loads((tmp_path/"data/projects/proj/metrics.jsonl").read_text().splitlines()[-1])
    assert line["intelligence"]["outcome"]=="resolved"
    assert json.loads((tmp_path/"data/projects/proj/outcomes.jsonl").read_text().splitlines()[-1])["verdict"]=="resolved"
```

- [ ] **Step 2: Run -> FAIL.**

- [ ] **Step 3: Implement** `MetricsStore` -- adapt the ported functions to write under `self.base = root/"data"/"projects"/project` (mkdir parents), keep atomic temp+rename, `record_outcome` also appends `{run_id,project,verdict,ts}` to `outcomes.jsonl` and rewrites the matching metrics line's `intelligence.outcome` (port the existing rewrite loop).

- [ ] **Step 4: Run -> PASS.**

- [ ] **Step 5: Commit** `git commit -am "conclave: project-namespaced metrics + totals + outcome labels"`

---

## Task 5: ArtifactStore (bulk tier)

**Files:** Create `conclave/store.py`, `tests/test_store.py`.

**Interfaces:**
- Produces: ABC `ArtifactStore` with `put(key:str, data:bytes) -> dict{key,bytes,sha256}`, `get(key:str) -> bytes`, `list() -> list[str]`, `manifest() -> list[dict]`. `LocalStore(base: Path)` implements it on the filesystem and maintains `manifest.jsonl`. `S3Store` stub raises `NotImplementedError` with a clear message (real impl deferred until scale). `open_store(cfg, root) -> ArtifactStore` factory.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_store.py
import hashlib
from conclave.store import LocalStore

def test_put_get_manifest_roundtrip(tmp_path):
    s = LocalStore(tmp_path/"store")
    meta = s.put("corpus/v1.part-001.gz", b"hello")
    assert meta["sha256"] == hashlib.sha256(b"hello").hexdigest() and meta["bytes"]==5
    assert s.get("corpus/v1.part-001.gz") == b"hello"
    assert "corpus/v1.part-001.gz" in s.list()
    assert s.manifest()[0]["key"] == "corpus/v1.part-001.gz"

def test_get_verifies_sha(tmp_path):
    s = LocalStore(tmp_path/"store"); s.put("k", b"abc")
    (tmp_path/"store"/"k").write_bytes(b"tampered")
    import pytest
    with pytest.raises(ValueError): s.get("k")
```

- [ ] **Step 2: Run -> FAIL.**

- [ ] **Step 3: Implement** `conclave/store.py`

```python
from __future__ import annotations
import abc, hashlib, json
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
        self.base = Path(base); self.base.mkdir(parents=True, exist_ok=True)
        self.mpath = self.base / "manifest.jsonl"
    def _man(self):
        try: return [json.loads(l) for l in self.mpath.read_text().splitlines() if l]
        except Exception: return []
    def put(self, key, data):
        p = self.base / key; p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        meta = {"key": key, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        man = [m for m in self._man() if m["key"] != key] + [meta]
        self.mpath.write_text("\n".join(json.dumps(m) for m in man) + "\n")
        return meta
    def get(self, key):
        data = (self.base / key).read_bytes()
        want = next((m["sha256"] for m in self._man() if m["key"] == key), None)
        if want and hashlib.sha256(data).hexdigest() != want:
            raise ValueError(f"sha256 mismatch for {key}")
        return data
    def list(self): return [m["key"] for m in self._man()]
    def manifest(self): return self._man()

class S3Store(ArtifactStore):
    def __init__(self, *a, **k): raise NotImplementedError(
        "S3/R2 backend not built yet -- use git-releases until corpus approaches ~1 GB")

def open_store(cfg, root):
    if cfg.store_backend in ("git-releases", "local"):
        return LocalStore(root / "data")
    return S3Store()
```

- [ ] **Step 4: Run -> PASS.**

- [ ] **Step 5: Commit** `git commit -am "conclave: ArtifactStore (LocalStore + sha256 manifest; S3 stub)"`

---

## Task 6: IndexBackend -- hybrid HNSW + BM25 (the perf contract)

**Files:** Create `conclave/index.py`, `tests/test_index.py`.

**Interfaces:**
- Produces: ABC `IndexBackend` with `upsert(items: list[dict{id,text,vector,meta}])`, `search_vec(vector, k) -> list[(id,score)]`, `size() -> int`. `LanceDBIndex(path)` implements HNSW search; `Bm25Index(path)` wraps `rank_bm25` over stored texts. `HybridIndex(vec, bm25)` fuses with RRF: `recall(query_vec, query_text, k) -> list[(id, rrf_score, meta)]`. `open_index(cfg, path) -> HybridIndex`.

- [ ] **Step 1: Write the failing tests** (functional + the flat-latency contract)

```python
# tests/test_index.py
import time, numpy as np
from conclave.index import HybridIndex, Bm25Index, open_index

def _vec(seed, d=8):
    rng = np.random.default_rng(seed); return list(rng.random(d))

def test_hybrid_recall_ranks_relevant_first(tmp_path):
    idx = open_index(_cfg("lancedb"), tmp_path/"idx")
    idx.upsert([{"id":str(i),"text":f"doc about topic {i}","vector":_vec(i),"meta":{"i":i}}
                for i in range(50)])
    hits = idx.recall(query_vec=_vec(7), query_text="topic 7", k=5)
    assert any(h[0]=="7" for h in hits)

def test_latency_flat_small_to_large(tmp_path):
    # contract: query time must not blow up with N -- assert sub-linear (50k query ~< 5x of 1k query, generously)
    def q_time(n):
        idx = open_index(_cfg("lancedb"), tmp_path/f"idx{n}")
        idx.upsert([{"id":str(i),"text":f"d{i}","vector":_vec(i),"meta":{}} for i in range(n)])
        t=time.perf_counter(); idx.recall(query_vec=_vec(1), query_text="d1", k=5); return time.perf_counter()-t
    small, large = q_time(1000), q_time(50000)
    assert large < small*5 + 0.05   # near-flat, not linear (which would be ~50x)
```
(`_cfg` is a tiny test helper returning a config with `index_backend="lancedb"`, `hybrid_bm25=True`.)

- [ ] **Step 2: Run -> FAIL.**

- [ ] **Step 3: Implement `conclave/index.py`** -- `LanceDBIndex` creates a LanceDB table with an HNSW (IVF-PQ) ANN index built once `size()` crosses a threshold (e.g. 256 rows) so small scale is exact and large scale is ANN; `Bm25Index` keeps an in-memory `BM25Okapi` rebuilt on upsert; `HybridIndex.recall` runs both, fuses by RRF (`score = sum 1/(60+rank)`), returns top-k with meta. Quantization (`pq`) + `metric=cosine` set from cfg. This is the module that must honor the < 50 ms / flat-latency contract -- keep the query path to ANN + BM25 only.

- [ ] **Step 4: Run -> PASS** (`pytest tests/test_index.py -q`; the latency test guards the contract).

- [ ] **Step 5: Commit** `git commit -am "conclave: hybrid HNSW+BM25 index with RRF (flat-latency contract)"`

---

## Task 7: Embedders (3 via OpenRouter + RRF ensemble)

**Files:** Create `conclave/embed.py`, `tests/test_embed.py`.

**Interfaces:**
- Produces: `Embedder(cfg, secret)` with `embed_docs(texts) -> list[vector]` (default model), `embed_query(text) -> vector`, and `embed_query_ensemble(text) -> list[(model, vector)]` (all three, for opt-in RRF across per-model indexes). Cost is summed into the run via the caller.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_embed.py
from conclave.embed import Embedder

def test_default_uses_default_model(monkeypatch):
    calls={}
    def fake_embed(secret, model, inputs, timeout):
        calls["model"]=model; return [[0.1]*4 for _ in inputs], 0.0
    monkeypatch.setattr("conclave.embed.http.embed", fake_embed)
    e = Embedder(_cfg(default="qwen/qwen3-embedding-8b"), "k")
    v = e.embed_query("hello")
    assert calls["model"]=="qwen/qwen3-embedding-8b" and len(v)==4

def test_ensemble_calls_all_three(monkeypatch):
    seen=[]
    monkeypatch.setattr("conclave.embed.http.embed",
        lambda s,m,i,t:(seen.append(m) or [[0.0]*2 for _ in i],0.0))
    e = Embedder(_cfg(models=["a","b","c"], ensemble=True), "k")
    out = e.embed_query_ensemble("q")
    assert [m for m,_ in out]==["a","b","c"] and seen==["a","b","c"]
```

- [ ] **Step 2: Run -> FAIL. Step 3: Implement** thin wrapper over `http.embed` (batch docs; single query for default; loop models for ensemble). **Step 4: PASS. Step 5: Commit** `git commit -am "conclave: 3 OpenRouter embedders + RRF ensemble option"`

---

## Task 8: Memory -- recall + note

**Files:** Create `conclave/memory.py`, `tests/test_memory.py`.

**Interfaces:**
- Produces: `Memory(root, project, index, embedder)` with `recall(query, k=5) -> list[dict{lesson|synthesis, meta, score}]`, `note(signature, lesson, *, verified: bool)`. `note` refuses unverified lessons (returns False); a verified note embeds + `index.upsert` + appends `data/projects/<project>/lessons/<sig-hash>.md`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_memory.py
from conclave.memory import Memory

def test_note_then_recall(tmp_path, fake_index, fake_embedder):
    m = Memory(tmp_path, "proj", fake_index, fake_embedder)
    assert m.note("smp-race", "use a spinlock around X", verified=True) is True
    assert m.note("bad", "guess", verified=False) is False     # unverified rejected
    hits = m.recall("race condition in X", k=3)
    assert any("spinlock" in h["lesson"] for h in hits)
```
(`fake_index`/`fake_embedder` are in-memory pytest fixtures.)

- [ ] **Step 2-5:** FAIL -> implement `Memory` (embed via embedder, upsert into index with meta `{type:"lesson", project, signature}`, write lesson file atomically) -> PASS -> `git commit -am "conclave: memory (verified note + recall)"`.

---

## Task 9: Reasoning harness -- escalate()

**Files:** Create `conclave/harness.py`, `tests/test_harness.py`. Port the orchestration spine from `.fusion/fusion_escalate.py:246-324`, inserting retrieve/decompose/verify around the panel+judge core.

**Interfaces:**
- Produces: `escalate(*, cfg, secret, mode, brief, project, root, memory, metrics, prior=None, target=None, run_id=None) -> dict{status, output, panel, cost, cumulative_cost, tokens, wall_s}`. Steps: (1) gate (enabled/secret/budget/balance), (2) `retrieved = memory.recall(brief)` -> prepend to context, (3) optional decompose (if brief > N chars, split on blank-line sections; v1 keeps it as context annotation), (4) `panel.run` with the no-web-search system prompt over brief+retrieved+prior, (5) `judge.judge`, (6) verify (if judged is short/low-confidence, one re-ask; v1: skip when judged non-empty), (7) `metrics.append_run` + `metrics.add_run_to_totals`. Status `ok`/`panel_only`/`disabled`/`no_key`/`over_budget`/`low_balance`/`unavailable`.

- [ ] **Step 1: Write the failing test** (mock panel/judge/memory/metrics)

```python
# tests/test_harness.py
from conclave import harness

def test_escalate_injects_memory_and_returns_synthesis(monkeypatch, tmp_path, fakes):
    monkeypatch.setattr(harness.panel, "run", lambda *a, **k:[
        {"model":"m","status":"ok","content":"analysis","cost":0.01,"sec":1.0,"chars":8,
         "prompt_tokens":10,"completion_tokens":5}])
    monkeypatch.setattr(harness.judge, "judge", lambda *a, **k:("SYNTH",0.2,1.0,{"prompt":3,"completion":2}))
    fakes.memory.add("prior fix: spinlock")          # recall returns this
    r = harness.escalate(cfg=fakes.cfg, secret="k", mode="stuck", brief="why crash",
                         project="proj", root=tmp_path, memory=fakes.memory, metrics=fakes.metrics)
    assert r["status"]=="ok" and r["output"]=="SYNTH"
    assert fakes.panel_brief_contained("spinlock")   # memory was injected into the panel context
```

- [ ] **Step 2-5:** FAIL -> implement (reuse the ported gate + metrics writes; inject `retrieved` into the user content) -> PASS -> `git commit -am "conclave: reasoning harness escalate() with memory injection"`.

---

## Task 10: Async ladder

**Files:** Create `conclave/ladder.py`, `tests/test_ladder.py`. Port `.fusion/ladder.py` (dispatch/poll/list/outcome/stats/run_worker) to call `harness.escalate` and `MetricsStore.record_outcome`; jobs under `data/projects/<project>/jobs/`.

**Interfaces:**
- Produces: `dispatch(root, project, mode, target, brief, prior) -> jid`, `poll(root, project, jid) -> str`, `list_jobs(root, project) -> str`, `outcome(root, project, jid, verdict)`, `stats(root, project) -> str`. Same detached-worker model (`subprocess.Popen([... "_worker", project, jid], start_new_session=True)`).

- [ ] **Step 1-5:** port tests from `.fusion` (`scripts/overnight/tests/test_fusion_ladder.py`) adapted to the project arg; FAIL -> implement -> PASS -> `git commit -am "conclave: async ladder (dispatch/poll/list/outcome/stats)"`.

---

## Task 11: Teaching -- corpus ingestion

**Files:** Create `conclave/teach.py`, `tests/test_teach.py`.

**Interfaces:**
- Produces: `teach(root, project, source: Path, *, distill: bool, embedder, index) -> dict{added, updated, pruned}`. Walks files, chunks (~512 tokens, overlap 64), content-hashes each chunk; on re-teach, unchanged chunks skip, changed update, vanished prune; optional `distill` runs one `http.chat` summarize pass per file; embeds + `index.upsert` with meta `{type:"corpus", project, path, sha}`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_teach.py
from conclave.teach import teach

def test_ingest_then_reteach_is_incremental(tmp_path, fake_index, fake_embedder):
    src = tmp_path/"specs"; src.mkdir(); (src/"a.md").write_text("uefi boot services exit\n"*20)
    r1 = teach(tmp_path, "proj", src, distill=False, embedder=fake_embedder, index=fake_index)
    assert r1["added"] > 0
    r2 = teach(tmp_path, "proj", src, distill=False, embedder=fake_embedder, index=fake_index)
    assert r2["added"] == 0 and r2["updated"] == 0          # nothing changed
    (src/"a.md").write_text("totally different content\n")
    r3 = teach(tmp_path, "proj", src, distill=False, embedder=fake_embedder, index=fake_index)
    assert r3["updated"] > 0 or (r3["added"] > 0 and r3["pruned"] > 0)
```

- [ ] **Step 2-5:** FAIL -> implement (chunker + content-hash ledger at `data/projects/<project>/teach-index.json`) -> PASS -> `git commit -am "conclave: corpus teaching (incremental ingest)"`.

---

## Task 12: Self-learning + auto-labeling

**Files:** Create `conclave/learn.py`, `tests/test_learn.py`.

**Interfaces:**
- Produces: `update_policies(root, project, run_record, verdict) -> None` (writes `policies/<project>.json`: per-problem-class `panel_credit[model]`, `prompt_variant_score[variant]`, `route[signature] -> subset`), and `select_panel(root, project, signature, full_panel) -> list[model]` (returns the learned subset or the full panel if unlearned). Auto-label helper `infer_outcome(build_ok: bool|None) -> str|None` maps a project success signal to `resolved`/`unresolved`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_learn.py
from conclave import learn

def test_resolved_run_promotes_contributing_models(tmp_path):
    rec = {"signature":"smp-race","per_model":[{"model":"m1","status":"ok"},{"model":"m2","status":"error"}]}
    learn.update_policies(tmp_path,"proj",rec,"resolved")
    pol = learn._load(tmp_path,"proj")
    assert pol["panel_credit"]["m1"] > pol["panel_credit"].get("m2",0)

def test_infer_outcome_from_build():
    assert learn.infer_outcome(True)=="resolved" and learn.infer_outcome(False)=="unresolved"
    assert learn.infer_outcome(None) is None
```

- [ ] **Step 2-5:** FAIL -> implement (simple additive credit + EMA scores, atomic write) -> PASS -> `git commit -am "conclave: self-learning policies + auto-label inference"`.

---

## Task 13: Self-healing -- doctor

**Files:** Create `conclave/heal.py`, `tests/test_heal.py`.

**Interfaces:**
- Produces: `doctor(root, cfg, secret, *, online: bool=False) -> dict{ok: bool, checks: list[dict{name,ok,detail}]}`. Checks: secret present; `secret` not tracked by git; state files parse (repair via snapshot if not); storage headroom (largest tracked file vs 90 MiB, repo size vs 1 GB); if `online`, balance >= min_credits and one panel model reachable. `repair_state(path)` restores from the newest `.snapshot` on parse failure.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_heal.py
from conclave import heal

def test_doctor_flags_missing_secret_and_repairs_state(tmp_path):
    (tmp_path/"conclave.toml").write_text(_MIN_TOML)
    bad = tmp_path/"data"/"x.json"; bad.parent.mkdir(parents=True); bad.write_text("{ broken")
    (tmp_path/"data"/"x.json.snapshot").write_text('{"ok":1}')
    rep = heal.repair_state(bad); assert rep == {"ok":1}
    rpt = heal.doctor(tmp_path, _cfg(), secret=None, online=False)
    assert rpt["ok"] is False
    assert any(c["name"]=="secret" and not c["ok"] for c in rpt["checks"])
```

- [ ] **Step 2-5:** FAIL -> implement -> PASS -> `git commit -am "conclave: self-healing doctor + state repair"`.

---

## Task 14: Compile pipeline (cadence + ledger + shards)

**Files:** Create `conclave/compile.py`, `tests/test_compile.py`.

**Interfaces:**
- Produces: `should_compile(root, cfg, project) -> bool` (>= every_days since last OR >= new_examples since last); `compile(root, cfg, project, *, embedder, store) -> dict{version, shards, num_examples}` -- gathers labeled runs since last compile, writes gzip corpus shards (<= shard_max_mib via `store.put`), embeds + writes vector shards, appends a `conclave-vN` ledger record to `ledger.jsonl`, advances `LATEST`. `eval_compiled(root, project, holdout) -> float` scores retrieval-augmented resolution on a held-out set.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_compile.py
import json
from conclave import compile as cc

def test_compile_shards_and_ledgers(tmp_path, fake_embedder, local_store, seed_runs):
    seed_runs(tmp_path, "proj", n=10)                 # 10 labeled runs on disk
    out = cc.compile(tmp_path, _cfg(shard_max_mib=1), "proj", embedder=fake_embedder, store=local_store)
    assert out["version"] == "conclave-v1" and out["num_examples"] == 10
    led = [json.loads(l) for l in (tmp_path/"ledger.jsonl").read_text().splitlines()]
    assert led[-1]["version"]=="conclave-v1" and all("sha256" in s for s in led[-1]["output_shards"])
    assert (tmp_path/"LATEST").read_text().strip()=="conclave-v1"

def test_should_compile_threshold(tmp_path):
    assert cc.should_compile(tmp_path, _cfg(new_examples=5), "proj") in (True, False)  # deterministic per seeded state
```

- [ ] **Step 2-5:** FAIL -> implement (shard writer respecting `shard_max_mib`, sha256 in ledger, gzip) -> PASS -> `git commit -am "conclave: knowledge-compilation pipeline (cadence, ledger, 90 MiB shards)"`.

---

## Task 15: CLI

**Files:** Create `conclave/cli.py`, `tests/test_cli.py`.

**Interfaces:**
- Produces: `main(argv=None) -> int` dispatching subcommands `ask|dispatch|recall|note|teach|compile|outcome|stats|doctor`, each wiring config + store + index + embedder + memory + metrics and printing results. `recall`/`note` are the cheap agent verbs (no panel). `ask`/`dispatch` run the harness.

- [ ] **Step 1: Write the failing test** (invoke `main` with argv, assert exit code + stdout)

```python
# tests/test_cli.py
from conclave import cli

def test_doctor_runs_offline(tmp_path, monkeypatch, capsys):
    monkeypatch.chdir(_make_repo(tmp_path))      # writes conclave.toml + secret.example
    rc = cli.main(["doctor"]); out = capsys.readouterr().out
    assert rc in (0,1) and "secret" in out

def test_recall_is_panel_free(tmp_path, monkeypatch, capsys):
    # recall must never call the panel
    monkeypatch.setattr("conclave.harness.panel.run", lambda *a,**k:(_ for _ in ()).throw(AssertionError("panel called")))
    monkeypatch.chdir(_make_repo(tmp_path))
    assert cli.main(["recall","race in X","--project","proj"]) == 0
```

- [ ] **Step 2-5:** FAIL -> implement argparse dispatch -> PASS -> `git commit -am "conclave: CLI (ask/dispatch/recall/note/teach/compile/outcome/stats/doctor)"`.

---

## Task 16: Impossible OS connector + rename + retire .fusion

**Files (in `impossible-os`):**
- Create: `.conclave/connector.sh` (thin: exports `CONCLAVE_HOME=~/conclave`, forwards to `python3 ~/conclave/conclave/cli.py "$@"`), `scripts/overnight/tests/test_conclave_connector.sh`.
- Modify: `.claude/skills/debug-session/SKILL.md`, `.claude/skills/review-todo-section/SKILL.md`, `.claude/skills/overnight-sequencer/SKILL.md`, `.claude/hooks/fusion_stuck_detect.py` -> rename to `conclave_stuck_detect.py` (and the `settings.json` + `MANIFEST.md` rows), `.claude/hooks/runner_status.py` (`fusion_jobs` -> `conclave_jobs`).
- Delete: `.fusion/` (engine now lives in `~/conclave`); keep nothing but the connector.

**Interfaces:**
- Consumes: the `conclave` CLI from Task 15.
- Produces: impossible-os calls `bash .conclave/connector.sh ask|recall|note|dispatch ...`; `CONCLAVE_ENABLED` replaces `FUSION_ENABLED` everywhere.

- [ ] **Step 1:** Write `test_conclave_connector.sh` asserting the connector forwards args and is gated (no key -> "no_key", non-zero only on misuse). Run -> FAIL.
- [ ] **Step 2:** Add the connector; rewire the three skills' ladder bullets and the stuck-detect hook to the `conclave` verbs; rename `FUSION_ENABLED` -> `CONCLAVE_ENABLED` (scoped by identifier, NOT substring -- do not touch `confusion`/`Fusion Drive`/`VMware Fusion`); update `settings.json` + `MANIFEST.md`. Run the sub-test + `bash scripts/test-tooling.sh` -> PASS.
- [ ] **Step 3:** `git rm -r .fusion` in impossible-os. Run `bash scripts/test.sh QUIET=1` (kernel tests unaffected) + `scripts/lint.sh` -> green.
- [ ] **Step 4: Commit (impossible-os)** `git commit -m "conclave: extract engine to ~/conclave; impossible-os becomes a client (connector + rename + retire .fusion)"`.

---

## Phasing + execution

- **Phase A (Tasks 1-9):** a working escalation engine with storage + flat-latency retrieval + memory. Shippable on its own.
- **Phase B (Tasks 10-16):** async, teaching, learning, healing, compile, CLI, and the impossible-os cutover.

Recommended: execute Phase A end-to-end and validate `recall` latency on synthetic data before Phase B.
