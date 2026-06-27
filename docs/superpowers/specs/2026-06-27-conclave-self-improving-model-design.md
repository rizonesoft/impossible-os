# Conclave -- a self-improving cross-project meta-model (design)

> Status: DESIGN / brainstorming output. Supersedes the in-repo "rename `.fusion/` to
> `.conclave/`" idea: Conclave becomes a standalone home-level system with its own
> GitHub repo, and Impossible OS becomes its first client. Date: 2026-06-27.

## Goal

Turn Conclave from a shell-script-that-calls-six-APIs into a thing that **behaves like
its own model**: one stable interface (problem in, answer out), internally running the
6-model panel plus an Opus judge, that also **remembers, learns from what worked, heals
when parts fail, and grows a project-only training corpus** that is distilled -- on a
cadence -- toward a real local student model.

Every project plugs in to both **teach** Conclave (contribute outcome-labeled records)
and **use** it (escalate hard problems). Conclave lives at `~/conclave`
(`/home/derickpayne/conclave`) with its own GitHub repository.

## Non-goals (v1)

- Not a general chat assistant. Conclave is the **apex escalation tier** -- invoked only
  when the calling agent (Claude) and its subordinate reviewer (Codex) are both stuck,
  or for explicit high-stakes review. Low volume by design.
- Not a from-scratch trained foundation model. The 6 cloud models are **teachers**; the
  local student model (Phase 2) is distilled from their outcome-labeled output, not
  trained from zero.
- The panel never does web-search (empirically the dominant cost/latency driver and the
  source of hallucinated errata). Pure reasoning over the supplied brief plus memory.

## Architecture overview

```
   project A ---\                          ~/conclave  (own git repo)
   project B ----+--[connector]--> ask --> +-------------------------------+
   project C ---/                          |  RUNTIME (one interface)      |
                                           |    panel(6) -> Opus judge     |
                                           |        ^            |         |
                                           |     MEMORY       SELF-HEAL    |
                                           |    (recall)     (recover)     |
                                           |        ^            |         |
                                           |   SELF-LEARN <-- outcomes     |
                                           +----------|--------------------+
                                                      v
                                           DATA ENGINE (tiered storage)
                                              git: learned state (KB)
                                              shards: transcripts (<=90 MiB, gz)
                                              archive: cold transcripts (Release/LFS/bucket)
                                                      |
                                           DISTILL PIPELINE (tooled+tested day one)
                                              dataset -> index -> student model
                                              cadence trigger + ledger + 90 MiB shards
```

The calling project sees exactly one verb: `ask`. Everything else is internal.

## Components

Each component is a separately testable unit with a narrow interface.

### Runtime / orchestrator

The current DIY engine (panel of 6 via OpenRouter chat-completions, parallel, Opus
discard-synthesis judge), promoted to the single stable entry point `conclave ask`
(sync) and `conclave dispatch` (async job queue -- the existing `ladder.py` model). It
returns the judged synthesis plus the per-run metrics already defined (latency, cost,
tokens, panel health). Off by default (`CONCLAVE_ENABLED` + a key), two-layer spend cap
(per-run counter + live balance floor), fail-open for the caller / fail-closed on spend.

### Plug-in protocol (teach + use)

Projects connect through a **thin connector**, not a copy of the engine:

- A project carries a small `conclave.toml` (or env) naming the project id and the
  `~/conclave` home. No engine code in the project.
- **use:** `conclave ask --project impossible-os --brief-file F [--prior ...]` ->
  async job -> synthesis. (Impossible OS wires this into `debug-session`,
  `review-todo-section`, and the overnight sequencer exactly where `.fusion` is wired
  today.)
- **teach:** every run is recorded under the project's namespace; the validating agent
  closes the loop with `conclave outcome <id> resolved|unresolved`, which is the
  outcome label that makes the record training-grade.
- **Isolation:** records are namespaced per project so the corpus is partitionable
  ("project-only" training data), while cross-project memory is opt-in.

### Data engine + storage tiering

The binding GitHub constraint is the **hard 100 MiB-per-file push block**, which bites a
naive single growing `.jsonl` at ~1,000 full-transcript runs -- long before any repo-size
limit. Tiering is therefore a v1 requirement, not an optimization:

- **Git, tiny, versioned:** metrics, totals, outcomes, run index, learned policies,
  memory summaries, the distillation ledger. Kilobytes; this is the "smart" state.
- **Sharded transcripts:** full briefs + 6 panel analyses + judge synthesis, compressed
  (gzip/zstd) and **month-sharded** so no single file approaches 100 MiB.
- **Cold archive:** old shards move out of the working tree to GitHub Releases, Git LFS,
  or an external bucket. Memory keeps the distilled lesson, not the raw transcript,
  forever.

Growth math (apex-tier, a handful to a few dozen runs/day across all projects): raw
~100 KB/run hits a single-file wall at ~1k runs; compressed+sharded ~20-30 KB/run keeps
the working repo under the soft 1 GB recommendation for tens of thousands of runs (years).

### Memory

Persistent recall so Conclave does not re-derive a problem it has already solved:

- **Lesson store:** distilled, human-and-machine-readable summaries keyed by
  symptom/signature (per project + an opt-in cross-project tier).
- **Retrieval index:** embeddings over past briefs/syntheses so a new brief is answered
  with relevant prior solutions injected as context (the cheap, immediately-useful
  precursor to the distilled model).
- Memory is fed by `outcome`-labeled runs; unresolved runs are remembered as
  open/avoid-this signals, not as answers.

### Self-learning

What actually improves from outcomes (software that tunes itself; no weight updates in
v1):

- **Panel selection:** promote/demote models by credited-insight rate and resolved-rate
  per problem class (the per-model metrics already capture cost/sec/chars/tokens; add a
  judge "credited model" signal).
- **Prompt + judge policy:** maintain candidate system-prompt / judge-prompt variants and
  prefer the variants with the better resolved-rate (bandit-style, logged).
- **Routing:** learn which problem signatures need the full panel vs. a cheaper subset.
- All learning is driven by the `outcome` label and recorded in `totals`/policy files so
  it is auditable and reversible.

### Self-healing

Recovery from the failure modes a long-lived service actually hits:

- **Model faults:** timeouts, invalid model ids, empty/garbage output -> drop that model,
  proceed on the rest; if the judge fails, hand back the raw panel (`panel_only`, already
  implemented). Auto-quarantine a model that fails repeatedly and surface it.
- **Spend/balance:** balance-floor + per-run cap already fail-closed; add a self-check
  that pauses distillation/runs and reports when credits are low.
- **State integrity:** validate/repair the learned-state + ledger files on startup
  (atomic writes, checksums, snapshot-on-write); never let a corrupt counter wedge runs.
- **Health command:** `conclave doctor` -- checks key, balance, model reachability, state
  integrity, storage headroom (distance to the 100 MiB/file and 1 GB/repo lines).

### Distillation pipeline (tooled and tested from day one)

The pipeline exists, is tested, and is versioned in v1 even though the *useful* student
model only emerges as the corpus grows. Three artifacts, built as one chain:

1. **Curated dataset** (jsonl, no GPU): dedup/clean/format the outcome-labeled corpus
   into training-ready shards. Always the input to the rest.
2. **Retrieval/memory index** (embeddings): the immediately-useful artifact (see Memory).
3. **Student model** (weights -- LoRA/fine-tune of an open base): the literal "Conclave
   is a model you run." Hardware/runtime path is an open decision (below).

- **Cadence:** `conclave distill --check` runs on a schedule and goes when
  `(>= N days since last) OR (>= M new labeled examples)`. The data engine tracks
  "new examples since last distill" so stale re-distills and missed busy weeks are both
  avoided.
- **Tracking / provenance (ledger, git):** one record per distill run --
  `conclave-vN { date, trigger, corpus_snapshot_sha, num_examples, base_model,
  hyperparams, eval_scores, output_shards:[{name,bytes,sha256}] }` + a `LATEST` pointer.
  Fully reproducible; eval-vs-previous visible.
- **Output sharding:** the artifact (dataset or weights) splits into
  `conclave-vN.part-001-of-0NN` parts, each <= 90 MiB, plus a manifest with checksums and
  a merge tool -- so every file clears the 100 MiB push block **without** LFS. (LFS is the
  alternative for binary weights but carries its own 1 GB free quota + bandwidth caps;
  manual 90 MiB sharding sidesteps that.)
- **Eval harness:** a held-out set of past problems with known outcomes; each distilled
  version is scored so regressions are caught before `LATEST` advances.

## Data model

- **run record** (sharded transcript): `{id, project, mode, ts, brief, panel:[{model,
  content, cost, sec, tokens}], judge, synthesis, prior}`.
- **metrics record** (git, lean): the existing per-run `latency_s/cost_usd/tokens/
  intelligence/per_model` line plus `project` and `run_id`.
- **outcome record** (git): `{run_id, project, verdict, ts}` -- the training label.
- **memory/lesson record** (git): `{signature, project, lesson, links[run_ids],
  last_seen}`.
- **distill ledger record** (git): as above.

## Security and boundaries

Hard lessons carried in by design:

- **Secrets never in tracked files.** The key lives only in `~/conclave/secret`
  (gitignored). No real key in any `*.example` template -- this is a scrubbed-incident
  rule, enforced by a pre-commit check in the Conclave repo.
- **Data boundary:** project source is sent to the panel only as the caller-authored
  brief; Conclave never auto-exfiltrates repo source. Briefs and outputs in the corpus
  are project-namespaced and stay in the project's partition unless cross-project memory
  is explicitly enabled.
- **Off by default + spend caps** as today; distillation has its own budget guard.

## v1 scope vs Phase 2

- **v1 (complete from day one):** `~/conclave` repo + connector protocol; the runtime
  (sync + async); data engine + storage tiering; memory (lessons + retrieval index);
  self-learning (panel/prompt/routing policies from outcomes); self-healing
  (`doctor` + graceful degradation + state repair); the **full distillation pipeline
  tooled, tested, versioned, and sharded** -- producing the curated dataset and retrieval
  index, and running the student-model path end-to-end on whatever data exists (thin but
  proven). Impossible OS migrated from `.fusion/` to the connector; Fusion -> Conclave
  rename folded in.
- **Phase 2 (data-gated):** the student model becomes *good*. Triggered when the corpus
  passes a size/quality threshold and the model-path decision is made. Same tooling, more
  data; ledger + eval harness already in place.

## Open decisions (need user input; recommendations given)

1. **Model path (the big one).** Does the student model train/run on a **local GPU**, or
   via a **cloud fine-tune API** (Together/Fireworks/OpenAI fine-tuning)? And is the end
   goal **fully local/offline** inference, or is "still calls the cloud but behaves as one
   model" acceptable for a good while? *Recommendation:* build v1's distillation tooling
   provider-agnostic (a thin "trainer" interface) so the dataset/index/ledger/sharding are
   identical either way, and pick the concrete trainer (local vs cloud) at the Phase 2
   gate when the corpus justifies the spend.
2. **Base model** for the student (e.g. a small Qwen/Llama). Defer to Phase 2 gate.
3. **Embeddings provider** for the retrieval index (OpenRouter/local). *Recommendation:*
   start with a hosted embedding model, keep it swappable.
4. **Repo visibility:** public or private GitHub repo for `~/conclave`. *Recommendation:*
   private (it contains project-derived training data).

## Testing strategy

- Per-component offline unit tests (mock the panel/judge/embedder/trainer), mirroring the
  current `.fusion` test pattern.
- Distillation **smoke test on tiny synthetic data** in CI: dataset -> index -> trainer
  stub -> sharded artifact -> merge -> verify checksums. Proves the pipeline without a GPU.
- Storage guardrail test: assert no tracked file approaches 100 MiB and the repo stays
  under the soft 1 GB line (a `doctor` check, also run in CI).
- Eval-harness regression test on a fixed held-out problem set.

## Migration from `.fusion/`

The engine, tests, README, metrics/totals, and async ladder built in Impossible OS
`.fusion/` move into `~/conclave`. Impossible OS keeps only the connector + the
skill-wiring (debug-session / review-todo-section / overnight-sequencer) pointed at
`~/conclave`. Identifier rename (`FUSION_ENABLED` -> `CONCLAVE_ENABLED`, etc.) happens as
part of the extraction, scoped by identifier (not substring) to avoid the verified
false-positives (`confusion`, APFS "Fusion Drive", "VMware Fusion").
