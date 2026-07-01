# Using Conclave (the CLI)

All verbs run from the Conclave repo root: `conclave <verb> ...` (or
`python -m conclave.cli <verb> ...`). `--project <name>` namespaces all data and
policies; it defaults to `default`. Cheap verbs (`recall`, `note`) never touch the
panel; `ask`/`dispatch` run the full harness; the rest are lifecycle/operations.

## ask — the stable interface

Run the harness synchronously and print the synthesis.

```
conclave ask --project <name> --mode stuck|review --target <signature> \
  --source brief.md          # or pass the brief as positional text
```

Sets `CONCLAVE_ENABLED=1` for the call, retrieves memory, runs panel + judge, records
metrics, and prints `[status]` then the output. Exit code 0 on `ok`/`panel_only`.

## dispatch — asynchronous escalation

Queue an escalation as a **detached background job** that survives the calling process;
collect the durable result later. Prints a job id.

```
conclave dispatch --project <name> --mode stuck --target <sig> --source brief.md
```

The worker runs `harness.escalate`, writes `result.txt` + job metadata under
`data/projects/<proj>/jobs/`, and records the transcript. (Job status is read back via
the jobs module's `poll`/`list`.)

## recall — cheap retrieval (no panel)

Embed the query and return verified lessons / taught corpus from the hybrid index.

```
conclave recall --project <name> "how do we fix the smp race on the runqueue"
```

## note — store a verified lesson

```
conclave note --project <name> --signature "<key>" --lesson "<lesson>" --verified
```

`--verified` is mandatory: without it the lesson is rejected. Verified lessons are
embedded into the index and written to `data/projects/<proj>/lessons/*.md`.

## teach — ingest a project's corpus

```
conclave teach --project <name> --source <file-or-dir> [--distill]
```

Ingests existing files so the harness knows the project *before* any escalation.
Incremental by content hash: re-teaching only re-embeds added/changed chunks.
`--distill` first runs each file through an LLM to compress it into key facts and
signatures; omit it to ingest verbatim. Prints `added= updated= pruned=`.

Use `--corpus` instead of `--project` to teach the **shared corpus** — general,
project-agnostic knowledge available to every project (see connecting-a-project).

## outcome — record a verdict and auto-learn

```
conclave outcome --project <name> --id <job-id> --verdict resolved|unresolved
```

Records the verdict and triggers the learning loop (see data-and-learning): updates
panel credit + routing policy, marks the transcript, and — if `resolved` — adds the
synthesis as a verified lesson.

## compile — knowledge compilation

```
conclave compile --project <name>
```

Distills resolved transcripts into the curated corpus + embedded vectors as gzipped
shards (≤ `shard_max_mib`), appends a provenance record to `ledger.jsonl`, and advances
the `LATEST` version pointer.

## stats — per-project metrics

```
conclave stats --project <name>
```

Prints run count, total cost, tokens, resolved/unresolved counts, and a per-mode
breakdown.

## doctor — self-healing health check

```
conclave doctor [--online]
```

Checks that `secret` is present and git-untracked, state files parse, and storage is
within limits (90 MiB/file, ~1 GB/repo). With `--online`, also checks the credit
balance floor and panel reachability. Exit 0 when healthy.
