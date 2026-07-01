# Data, storage tiers, and the learning loop

## All learning data is local

Conclave's learning data stays on the machine that runs it — none of it is committed.
A public repository ships only code and docs (including this Conclave self-knowledge);
every install grows its own knowledge and never publishes anyone's project or personal
data. The data spans tiers — small brain-state JSON/JSONL, bulk artifacts in a pluggable
`ArtifactStore` (`LocalStore` now; `S3Store` at scale), and a rebuildable query index —
but all of it lives under `data/` (and learned `policies/`), which is gitignored.

## Two namespaces: corpus vs projects

Knowledge is stored in one of two namespaces:

- **`data/corpus/`** — the shared, **project-agnostic** corpus: general, reusable
  knowledge (for example, this documentation about Conclave itself). Teach it with
  `conclave teach --corpus`. It is available to every project's recall.
- **`data/projects/<project>/`** — a specific project's own learning: its lessons,
  metrics, outcomes, and run transcripts.

The shared corpus is the natural unit to make public (teach it from tracked source docs);
project namespaces hold the local, project-specific material.

## The query index and the performance contract

Retrieval is a `HybridIndex`: a **vector ANN** arm plus a **lexical full-text (FTS)**
arm, fused by Reciprocal Rank Fusion. Both arms and their `text`/`meta` live as
persisted columns inside one LanceDB table, so a fresh process (every CLI invocation)
queries the on-disk indexes directly without rebuilding anything. The query path touches
only the indexes (sub-linear) and reads back only the k matched rows — so **recall
latency is independent of total corpus size**. Indexing cost is paid by the writer
(`teach`/`note`), never the reader. A vector ANN index is built past a row threshold;
below it, exact KNN is used (same results).

## Memory: teach vs note

- **`note(signature, lesson, verified=True)`** stores a single verified lesson. Each
  lesson is embedded into the index and written as a markdown file. Unverified lessons
  are refused.
- **`teach(source, …)`** ingests whole files as corpus chunks (~400 words, overlapping),
  keyed by content hash so re-teaching is incremental (add / update / prune). Each chunk
  carries its text in `meta` so `recall` can render it. This is how a project's existing
  knowledge becomes retrievable before any escalation.

`recall(query, k)` is the cheap read path used both directly (the CLI verb) and inside
`escalate()` to inject prior knowledge.

## The learning loop (self-improvement without weight updates)

Conclave improves by tuning **policies and memory** from outcome labels — never by
updating model weights. The loop, triggered by `outcome`:

1. **Label.** A run's verdict is `resolved` or `unresolved` (entered explicitly, or
   inferred from the project's build/test success signal).
2. **Credit the panel.** `learn.update_policies` adjusts each contributing model's
   running credit (+1.0 resolved, −0.25 unresolved) in `policies/<proj>.json`.
3. **Route.** On a resolved run, the problem signature is mapped to the set of models
   that succeeded, so similar future problems can prefer them.
4. **Remember.** On `resolved`, the synthesis is stored as a verified lesson, making it
   retrievable next time.

Policies are plain JSON — auditable and reversible — and written atomically.

## Knowledge compilation (cadence)

`compile` periodically distills the accumulated resolved transcripts into the curated
corpus + embedded vectors, sharded to stay under the per-file size limit, with a
provenance record appended to `ledger.jsonl` and the `LATEST` pointer advanced.
`should_compile` gates this: run weekly (`compile_every_days`) **or** after enough new
labeled examples (`compile_new_examples`). `eval_compiled` reports the resolved-rate —
the ground-truth quality signal.

## Self-healing

`doctor` is the health check a long-lived service trips on: secret present and
untracked, state files parse, storage headroom (90 MiB/file, ~1 GB/repo), and — when
online — the balance floor and panel reachability. `repair_state` restores a corrupt
JSON state file from its `.snapshot` sibling.
