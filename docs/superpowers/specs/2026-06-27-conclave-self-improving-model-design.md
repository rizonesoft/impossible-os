# Conclave -- a self-improving cross-project meta-model (design)

> Status: DESIGN / brainstorming output. Supersedes the in-repo "rename `.fusion/` to
> `.conclave/`" idea: Conclave becomes a standalone home-level system with its own
> GitHub repo, and Impossible OS becomes its first client. Date: 2026-06-27.

## Goal

Turn Conclave from a shell-script-that-calls-six-APIs into a thing that **behaves like
its own model**: one stable interface (problem in, answer out), internally a
**model-agnostic reasoning harness** that drives a swappable panel of cloud models plus
an Opus judge, and that also **remembers, learns from what worked, heals when parts fail,
and grows a project-only knowledge corpus**. Conclave's intelligence is the harness --
memory, retrieval, decomposition, structured process, and learned policies -- not a
trained weight file. It gets smarter over time via better context and process, not via
gradient descent.

The local piece is never used standalone (always alongside the panel), so the core needs
**no base model and no GPU**. A distilled local model is demoted to an optional future
"offline" add-on (see Phase 2), and the corpus is kept in a shape that would still
support it if "offline" ever becomes a goal.

Every project plugs in to both **teach** Conclave (contribute outcome-labeled records)
and **use** it (escalate hard problems). Conclave lives at `~/conclave`
(`/home/derickpayne/conclave`), backed by its own private GitHub repository
`https://github.com/rizonetech/Conclave`.

## Non-goals (v1)

- Not a general chat assistant. Conclave is the **apex escalation tier** -- invoked only
  when the calling agent (Claude) and its subordinate reviewer (Codex) are both stuck,
  or for explicit high-stakes review. Low volume by design.
- Not a trained model in the core. Conclave's intelligence is the reasoning harness +
  memory + retrieval + learned policies, not weights. (A distilled local student is an
  optional future "offline" add-on only -- see Phase 2 -- and is never required because
  the local piece never answers alone.)
- Retrieval over training: project knowledge is injected as context to the panel rather
  than baked into weights. Retrieval-injecting a project's accumulated knowledge achieves
  most of what a project-specialized fine-tune would, without a GPU or a base-model ceiling.
- The panel never does web-search (empirically the dominant cost/latency driver and the
  source of hallucinated errata). Pure reasoning over the supplied brief plus memory.

## Architecture overview

```
   project A ---\                          ~/conclave  (own git repo)
   project B ----+--[connector]--> ask --> +-------------------------------------+
   project C ---/                          |  REASONING HARNESS (one interface)  |
                                           |   retrieve -> decompose -> prompt   |
                                           |   panel(swappable) -> judge -> verify|
                                           |        ^             |              |
                                           |     MEMORY        SELF-HEAL         |
                                           |    (recall)      (recover)          |
                                           |        ^             |              |
                                           |   SELF-LEARN  <--  outcomes         |
                                           +----------|--------------------------+
                                                      v
                                           DATA ENGINE (tiered storage)
                                              git: learned state + policies (KB)
                                              shards: transcripts (<=90 MiB, gz)
                                              archive: cold transcripts (Release/LFS/bucket)
                                                      |
                                           KNOWLEDGE COMPILATION (tooled+tested day one)
                                              corpus -> retrieval index -> learned policies
                                              cadence trigger + ledger + 90 MiB shards
                                              (optional future: -> distilled offline model)
```

The calling project sees exactly one verb: `ask`. Everything else is internal.

## Components

Each component is a separately testable unit with a narrow interface.

### Reasoning harness (the core)

The intelligence of Conclave. A model-agnostic pipeline that wraps the swappable panel
and makes every model in it reason better:

1. **Retrieve** -- pull relevant past problems, solutions, and lessons from memory and
   inject them as context (cold models become project-aware models).
2. **Decompose** -- when a brief is large/multi-factor, split it into sub-questions the
   panel answers better.
3. **Structured prompt** -- the per-mode system prompts (no web-search) over brief +
   retrieved context + any caller-supplied prior analyses.
4. **Panel + judge** -- the existing DIY engine (panel via OpenRouter chat-completions,
   parallel, Opus discard-synthesis judge). Panel membership is config, not code.
5. **Verify** -- optional re-ask / cross-check loop on low-confidence syntheses before
   returning.

Exposed as the single stable entry point `conclave ask` (sync) and `conclave dispatch`
(async job queue -- the existing `ladder.py` model). Returns the judged synthesis plus
the per-run metrics already defined (latency, cost, tokens, panel health). Off by default
(`CONCLAVE_ENABLED` + a key), two-layer spend cap (per-run counter + live balance floor),
fail-open for the caller / fail-closed on spend. The harness always needs the cloud panel
-- there is no standalone local answerer in the core (accepted trade-off).

### Plug-in protocol (teach + use)

Projects connect through a **thin connector**, not a copy of the engine:

- A project carries a small `conclave.toml` (or env) naming the project id and the
  `~/conclave` home. No engine code in the project.
- **use:** `conclave ask --project impossible-os --brief-file F [--prior ...]` ->
  async job -> synthesis. (Impossible OS wires this into `debug-session`,
  `review-todo-section`, and the overnight sequencer exactly where `.fusion` is wired
  today.)
- **teach (two sources):** (1) *experience* -- every escalation run is recorded under the
  project's namespace and labeled by outcome (see Self-learning); (2) *corpus* --
  proactively ingest existing project knowledge with
  `conclave teach --project P --source <path> [--distill]` (e.g. teach it
  `specs/`). Both feed the same retrieval index + lesson store.
- **Isolation:** records are namespaced per project so the corpus is partitionable
  ("project-only" training data), while cross-project memory is opt-in.

### Data engine + storage tiering

The binding GitHub constraint is the **hard 100 MiB-per-file push block**, which bites a
naive single growing `.jsonl` at ~1,000 full-transcript runs -- long before any repo-size
limit. Tiering is therefore a v1 requirement, not an optimization:

- **Git, tiny, versioned:** metrics, totals, outcomes, run index, learned policies,
  memory summaries, the compile ledger. Kilobytes; this is the "smart" state.
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
- **Retrieval index:** **hybrid BM25 + vector** over past briefs/syntheses + ingested
  corpus, so a new brief is answered with relevant prior solutions injected as context
  (the cheap, immediately-useful core of the harness, and the substitute for any future
  offline model). Embedders run **via OpenRouter** (same key, no separate provider):
  the set is `qwen3-embedding-8b` (default primary, 32K context, $0.01/M), `bge-m3`
  (open, hybrid dense+sparse, $0.01/M), and `text-embedding-3-large` (OpenAI, $0.13/M) --
  three families for a strong ensemble. Default queries the primary; **RRF-ensemble across
  all three is opt-in** (different models = different vector spaces, so the ensemble is N
  indexes fused by Reciprocal Rank Fusion, not one blended index). Embedding cost is
  one-time + incremental and trivial (~$0.38 to embed a 10 MB corpus with all three;
  ~$0.0001/query). Embedder set is swappable behind the index interface (a local
  open-model backend stays a future option if full on-box privacy is ever wanted).
- Memory is fed by `outcome`-labeled runs; unresolved runs are remembered as
  open/avoid-this signals, not as answers.

### Teaching a corpus (proactive ingestion)

`conclave teach --project P --source <path> [--distill]` ingests existing files so the
harness knows the project before any escalation. Walk -> chunk -> optional LLM distill
(summarize each file into key facts + signatures) -> embed -> add to the retrieval index
+ lesson store. Each chunk records source path + content hash, so re-teaching **updates**
rather than duplicates and prunes chunks whose source was deleted/changed. Two drivers:
**Claude-driven** ("teach Conclave the specs" -> Claude runs the command) and
**automatic on connector sync** (a hook re-ingests changed docs/specs on plug-in / file
change). Same artifacts as experience-teaching; just a different input.

### Self-learning (automatic)

Nothing learns without a reward signal. Labels come from three sources, increasingly
hands-off: **explicit** (`conclave outcome <id> resolved|unresolved`); **implicit** --
the connector auto-emits the outcome from the project's own success signals (in Impossible
OS, whether `build`/`test`/`smoke` went green after applying the suggested fix), which is
what makes learning automatic with no human in the loop; and **recurrence** (a signature
that never returns is a delayed positive, a recurrence a negative).

Two update loops consume the labels: **online** (each outcome instantly updates memory +
policy counters) and **scheduled** (the `compile` cadence rebuilds index + policies over
the full corpus). What actually improves (software that tunes itself; no weight updates):

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
  that pauses compilation/runs and reports when credits are low.
- **State integrity:** validate/repair the learned-state + ledger files on startup
  (atomic writes, checksums, snapshot-on-write); never let a corrupt counter wedge runs.
- **Health command:** `conclave doctor` -- checks key, balance, model reachability, state
  integrity, storage headroom (distance to the 100 MiB/file and 1 GB/repo lines).

### Knowledge-compilation pipeline (tooled and tested from day one)

"Compile" the raw outcome-labeled corpus into the artifacts the harness consumes. No GPU,
no weights. Two artifacts built as one chain, plus an optional future third:

1. **Curated corpus** (jsonl): dedup/clean/format the outcome-labeled records into
   compiled shards -- the canonical knowledge base.
2. **Retrieval index** (embeddings over the corpus): what the harness queries at `ask`
   time to inject relevant prior solutions (see Memory). This is the immediately-useful
   artifact and the thing that makes any model smarter.
3. **(Optional, future)** a **distilled offline model** (weights) built from the same
   corpus -- only if "offline/standalone" ever becomes a goal. Not in the core; the
   corpus is kept compatible with it so the door stays open at zero cost now.

- **Cadence:** `conclave compile --check` runs on a schedule and goes when
  `(>= N days since last) OR (>= M new labeled examples)`. The data engine tracks
  "new examples since last compile" so stale recompiles and missed busy weeks are both
  avoided.
- **Tracking / provenance (ledger, git):** one record per compile run --
  `conclave-vN { date, trigger, corpus_snapshot_sha, num_examples, index_model,
  eval_scores, output_shards:[{name,bytes,sha256}] }` + a `LATEST` pointer. Fully
  reproducible; eval-vs-previous visible.
- **Output sharding:** the artifact (corpus + index) splits into
  `conclave-vN.part-001-of-0NN` parts, each <= 90 MiB, plus a manifest with checksums and
  a merge tool -- so every file clears the 100 MiB push block **without** LFS. (LFS carries
  its own 1 GB free quota + bandwidth caps; manual 90 MiB sharding sidesteps that.)
- **Eval harness:** a held-out set of past problems with known outcomes; each compiled
  version is scored (does retrieval-augmented answering resolve them?) so regressions are
  caught before `LATEST` advances.

## Data model

- **run record** (sharded transcript): `{id, project, mode, ts, brief, panel:[{model,
  content, cost, sec, tokens}], judge, synthesis, prior}`.
- **metrics record** (git, lean): the existing per-run `latency_s/cost_usd/tokens/
  intelligence/per_model` line plus `project` and `run_id`.
- **outcome record** (git): `{run_id, project, verdict, ts}` -- the training label.
- **memory/lesson record** (git): `{signature, project, lesson, links[run_ids],
  last_seen}`.
- **compile ledger record** (git): as in the knowledge-compilation pipeline.

## Security and boundaries

Hard lessons carried in by design:

- **Secrets never in tracked files.** The key lives only in `~/conclave/secret`
  (gitignored). No real key in any `*.example` template -- this is a scrubbed-incident
  rule, enforced by a pre-commit check in the Conclave repo.
- **Data boundary:** project source is sent to the panel only as the caller-authored
  brief; Conclave never auto-exfiltrates repo source. Briefs and outputs in the corpus
  are project-namespaced and stay in the project's partition unless cross-project memory
  is explicitly enabled.
- **Cloud-embedding exposure (accepted trade-off):** with OpenRouter embedders, the whole
  corpus (ingested specs + past briefs + syntheses) is sent to the embedding providers --
  a larger standing exposure than the per-query panel briefs. Accepted in exchange for one
  key + zero local infra; the swappable local-embedder backend is the escape hatch if
  full on-box privacy is ever required.
- **Off by default + spend caps** as today; compilation has its own budget guard.

## v1 scope vs Phase 2

- **v1 (complete from day one):** `~/conclave` repo + connector protocol; the **reasoning
  harness** (retrieve -> decompose -> structured prompt -> panel + judge -> verify), sync
  + async; data engine + storage tiering; memory (lessons + retrieval index);
  **teaching** (experience + `conclave teach` corpus ingestion); self-learning
  (panel/prompt/routing policies from outcomes, with **automatic outcome labeling** from
  the project's build/test/smoke signals -- no human in the loop); self-healing
  (`doctor` + graceful degradation + state repair); the **full knowledge-compilation
  pipeline tooled, tested, versioned, and sharded** -- producing the curated corpus and
  retrieval index, with the cadence trigger, ledger, and 90 MiB sharding all proven.
  Impossible OS migrated from `.fusion/` to the connector; Fusion -> Conclave rename
  folded in. No base model, no GPU.
- **Phase 2 (optional, data-and-need-gated):** a distilled **offline** model -- ONLY if
  standalone/no-internet answering becomes a goal. Triggered when the corpus is large
  enough AND offline is actually wanted; the model-path decision is made then. Same
  corpus, ledger, and eval harness already in place. May never be built, and that is fine.

## Open decisions (need user input; recommendations given)

The two big ones (model path, base model) are **dissolved** by the reasoning-harness
design -- no base model, no GPU in the core.

**Decided:**

- Retrieval = hybrid BM25 + vector; embedders via OpenRouter (same key) =
  `qwen3-embedding-8b` (default) + `bge-m3` + `text-embedding-3-large`; RRF-ensemble across
  all three is opt-in; embedder set swappable (local backend a future option).
- Repo = **private**, already created at `https://github.com/rizonetech/Conclave`, cloned
  to `~/conclave`.
- Compile cadence default = **weekly OR 200 new labeled examples**, whichever first
  (tunable in config after first real data).

What remains:

1. **(Deferred, not now)** the optional offline-model path -- left open until/unless
   offline answering is actually wanted (Phase 2).

## Testing strategy

- Per-component offline unit tests (mock the panel/judge/embedder), mirroring the current
  `.fusion` test pattern.
- Compilation **smoke test on tiny synthetic data** in CI: corpus -> index -> sharded
  artifact -> merge -> verify checksums. Proves the pipeline end-to-end without a GPU.
- Harness test: retrieval injects the right prior context for a known signature, and a
  low-confidence synthesis triggers the verify loop.
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
