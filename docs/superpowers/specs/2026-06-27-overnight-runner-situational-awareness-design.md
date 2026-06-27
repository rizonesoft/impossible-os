# Overnight Runner -- Situational-Awareness Layer

> Design spec. Status: approved 2026-06-27. Operator: Derick Payne. Scope: keep the
> overnight main loop on-rails (no drift, no gate-surprise, no forgotten decisions)
> by aggregating already-existing scattered run-state into a compact brief the loop
> sees at the right moments. This is a COST lever: a focused loop wastes fewer
> turns, and every wasted turn re-bills the whole growing main context.

## 1. Problem

Over a multi-hour headless run the Opus main loop loses situational awareness in
three observed ways (operator-confirmed 2026-06-27):

1. **Drift** -- loses the thread of the current task, wanders into adjacent work.
2. **Gate surprise** -- blocked by a hook/gate it did not anticipate (e.g. the dead
   `codex_review_completed` recorder false-firing twice in one session), or unaware
   of its open obligations.
3. **Forgotten decisions** -- re-derives or contradicts decisions already made,
   especially after context compaction.

The unifying cause: the relevant state already EXISTS but is scattered across the
guard cursor, the gate state-files, git, and the Run Log -- and the loop does not
proactively consult it. A fix that merely tells the loop to "remember to check"
fails for the same reason the problem exists (the loop is not looking); the repo
already learned this as "skill-invocation drift," whose answer was hooks that push,
not docs that ask.

**This is a cost lever, not just a quality one.** Drift recovery, gate-surprise
recovery, and wrong-premise rework each burn many Opus turns, and each turn
re-bills the growing context. A small fixed awareness cost prevents large variable
waste -- net token-positive.

## 2. Design principle

**Computed state beats self-maintained state.** A drifting loop cannot be trusted
to journal its own memory, so the brief is AGGREGATED from sources that already
exist and cannot go stale (guard, gate state-files, git, Run Log). The only curated
input is a small transient-hazard registry (component 3), which is append-only and
time-stamped so staleness is visible.

## 3. Hard invariants

1. **No change to gate semantics, the guard phase machine, or runner control flow.**
   The layer is read-only aggregation plus additive output; it never blocks, never
   mutates run state, never alters a gate's verdict.
2. **Compact and cheap.** The pushed anchor is ONE line; the full brief is bounded
   (~30 lines, < 1 s). The push fires only at key moments, never every turn.
3. **Interactive sessions stay quiet.** The push parts gate on the headless
   discriminator (`OVERNIGHT_SEQUENCER_RUN`) / guard-active, so interactive
   sessions are not spammed. The `runner-status` pull works anywhere on demand.
4. **Fail-open.** A missing/corrupt source degrades that one line of the brief to
   "(unknown)"; the layer never errors out of a hook.

## 4. Components

### C1 -- `runner-status` aggregator (the PULL)

A stdlib CLI (`scripts/overnight/runner-status.py`) that prints a compact brief by
aggregating existing state:

- **WHERE:** cursor + phase from `.claude/state/sequencer-run.json` (guard).
- **GIT:** `HEAD` one-line + dirty-file count.
- **OPEN OBLIGATIONS:** computed from the gate state-files and git -- e.g. an
  unreceived Codex review (`last-codex-review.json` `received:false` within TTL), a
  pushed `[x]` IO-row flip with no `**Verified:**` stamp (the review-todo trigger),
  a pending design review. Each obligation is one line naming the unmet gate.
- **LIVE GOTCHAS:** the unexpired entries of component 3.
- **RECENT DECISIONS:** the last N Run-Log lines from the doctrine file.

Usable anywhere (interactive or headless); it only reads.

### C2 -- One-line anchor (the PUSH)

Extend `run_phase_guard.py status` and `phase` to also print a one-line anchor:
`cursor <domain/file> | phase <PHASE> | obligations:N | gotchas:M`. The loop
already calls the guard at every phase transition, so this is a free push at
exactly the right moment with no new hook. When the anchor shows
`obligations:>0` or `gotchas:>0`, the loop pulls the full `runner-status`.

### C3 -- `live-gotchas.md` transient-hazard registry (the only new state)

A curated append-only file (`.claude/state/live-gotchas.md`), one hazard per line
with a date and optional expiry: e.g.
`2026-06-14 codex_review_completed recorder dead -> expect false review-gate blocks; manual-record (project_codex_review_hook_dead)`.
Operator and loop append; `runner-status` reads it, dropping entries past their
expiry. This is what stops "gate surprise": the hazard is in the brief BEFORE the
loop hits it. Seeded from the known live gotchas already in operator memory.

### C4 -- Post-compaction re-orient

A `PreCompact` hook snapshots the current brief to a known path; the
overnight-sequencer skill is instructed to read that snapshot immediately after a
compaction so the loop rebuilds awareness instead of flailing. Compaction is the
single highest-risk moment for lost situational awareness.

## 5. Out of scope / deferred

- **OpenRouter Fusion as an escalation reviewer.** Fusion is an ensemble
  deliberation (a panel of models + a judge), priced as the sum of all underlying
  completions -- a "spend more to be more correct" tool. It does NOT fix focus. Its
  only honest fit is a rare, opt-in BREAK-GLASS reviewer for the highest-stakes
  calls (an SMP/lock-order verdict, an ABI change, a security-sensitive section)
  where a miss crashes bare metal and the extra cost is justified. It needs an
  OpenRouter dependency + API key (stop-and-ask) and is anti-cost everywhere else.
  Tracked here as a deferred option, not part of this layer.
- Auto-curating the gotchas registry from memory recall (manual seed for now).

## 6. Success criteria

- `runner-status` prints WHERE / GIT / OBLIGATIONS / GOTCHAS / DECISIONS in <= ~30
  lines, < 1 s, fail-open on any missing source.
- `run_phase_guard.py status`/`phase` emit the one-line anchor when the run is
  active; silent/normal otherwise.
- The obligations computation correctly flags at least: unreceived Codex review,
  pushed `[x]` flip lacking a `**Verified:**` stamp.
- `live-gotchas.md` exists, is read into the brief, and expired entries are dropped.
- PreCompact snapshot is written and the skill reads it post-compaction.
- Measurable via WS3: turns-per-section and re-read counts trend down on a run with
  the layer vs a baseline run without it (the focus->cost claim, validated).
