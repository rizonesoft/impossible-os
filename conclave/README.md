# Conclave

A self-improving, cross-project **reasoning harness** that behaves like its own model:
one stable interface (`ask`), internally a model-agnostic pipeline driving a swappable
panel of cloud models plus an Opus judge, with memory, automatic learning, and
self-healing. Every project plugs in to both **teach** it and **use** it.

Conclave is the **apex escalation tier** -- invoked when an agent (Claude) and its
reviewer (Codex) are both stuck, or for explicit high-stakes review. Its intelligence is
the harness (retrieval, memory, structured process, learned policies), **not** a trained
weight file -- so the core needs no base model and no GPU.

## Design

- Spec: [`docs/specs/2026-06-27-conclave-self-improving-model-design.md`](docs/specs/2026-06-27-conclave-self-improving-model-design.md)
- Predecessor (`.fusion` lineage): the escalation-ladder spec + plans under `docs/`.

## Layout

```
conclave.toml             config: panel / judge / embedders / cadence
secret                    OpenRouter key (GITIGNORED; copy from secret.example)
docs/specs/ docs/plans/   design + implementation docs
conclave/                 engine code (arrives during implementation)
data/                     the learning data (see below)
policies/                 learned policies (panel selection, prompt variants, routing)
```

## How the learning data is stored

**All learning data stays local.** This repo ships only code and docs — including the
Conclave self-knowledge in `docs/about-conclave/` (the one corpus meant to be public).
Every clone grows its own `data/` and learned `policies/`; none of it is committed, so a
public repo never leaks anyone's project or personal knowledge. Seed your install by
teaching it your own material (`conclave teach --source <path>`), starting with the
bundled docs.

The data spans three tiers, so it starts at ~5 MB with zero infra and scales to 200 GB+
at flat query latency: small "brain-state" JSON/JSONL; bulk artifacts in a pluggable
**STORE** (`ArtifactStore`); the query index is a rebuildable embedded engine. All of it
is gitignored.

| Path | Tier | Note |
|---|---|---|
| `data/projects/<proj>/outcomes.jsonl` | local | resolved/unresolved labels (the reward signal) |
| `data/projects/<proj>/metrics.jsonl`, `totals.json` | local | per-run + cumulative metrics |
| `data/projects/<proj>/lessons/*.md` | local | distilled lessons (memory) |
| `policies/*.json`, `ledger.jsonl` | local | learned policies + compile provenance + STORE manifest |
| `data/projects/<proj>/runs/YYYY-MM.jsonl.gz` | STORE | raw transcripts, compressed monthly shards |
| `data/corpus/conclave-vN.part-*.gz` | STORE | compiled corpus shards |
| `data/vectors/conclave-vN.part-*.f16` | STORE | embedded vectors |
| `data/index/` | local | embedded ANN + FTS engine (the query path); rebuilt from corpus |
| `docs/about-conclave/` | **tracked** | the public Conclave self-knowledge (teach it to seed) |
| `secret` | ignore | never commit |

**Performance contract:** `recall` p99 **< 50 ms from 5 MB to 200 GB**. The query path
hits only a quantized HNSW + BM25 index (sub-linear, RAM/mmap-resident), never the raw
data -- so latency is independent of total size. `ArtifactStore` + `IndexBackend` are
pluggable (LanceDB / FAISS; git+Releases / S3 / R2), spanning the whole range by config.

## Status

Design complete; implementation pending (see `docs/plans/`). Off by default
(`CONCLAVE_ENABLED=1` + a key in `secret`).
