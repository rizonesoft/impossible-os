# Overnight Runner -- Context-Cost Reduction (Phase A)

> Design spec. Status: approved 2026-06-27. Author: interactive Claude Code session
> with operator Derick Payne. Scope: reduce the Claude token cost of the overnight
> sequencer by offloading reads to throwaway subagent contexts, without weakening
> any quality gate or the guard/fixpoint machinery.

## 1. Problem

The overnight sequencer drives every `todo/` file to completion on the Opus 4.8
(1M-context) main loop. In a multi-hour run the dominant cost is not per-call
price; it is the **main context, re-billed on every turn**. Every file the main
loop reads to explore a section, every serial/build log it swallows to diagnose a
failure, every parity web-search result it pulls in -- all of it persists in the
main context and is paid for again on each subsequent turn until compaction.

The repo already ships five read-only specialist agents (`kernel-explorer`,
`kernel-quality-auditor`, `boot-quality-auditor`, `parity-research-analyst`,
`review-evidence-mapper`) whose entire purpose is to absorb those reads in a
throwaway context and return only a digest. But each is gated behind a
"large/unfamiliar section only" condition, so in practice they rarely fire and the
Opus main loop does the reading in-context. The agents are built; they are just
not used -- and because the dispatch is skill-doc text rather than a hard gate, it
is silently bypassable (the documented "skill invocation drift" failure mode).

### What this is NOT

A separate goal was raised (SQL, embeddings/RAG, OpenRouter, a srclight-style
semantic-search MCP). Assessment:

- **SQL** does not reduce token cost. Context is token-budget, not disk. The
  runner's state already lives in JSON + the todo-graph oracle. SQL only matters
  as an embeddings backing store, which is itself deferred.
- **Embeddings / semantic-search MCP** overlaps tools that already exist
  (lsp-bridge for canonical symbol answers; explorer subagents for
  read-many-return-digest). srclight specifically has a documented footgun: it
  repoints `core.hooksPath` and silently kills the `.githooks` chain (lint +
  COUNT.md) the runner depends on -- the literal "break it like last time"
  scenario.
- **OpenRouter** is a model-price lever (cheap-model digester backend), not a
  context lever, and adds a dependency + a new headless failure mode.

None of these ship in Phase A. They are gated behind the Phase-A measurement
(Workstream 3): the heavy infra is justified by data or rejected. This is the
deliberate "don't break it" path -- ship the zero-dependency context wins first,
measure, then decide.

## 2. Hard invariants (must not regress)

These are the safety contract. Every workstream below preserves them:

1. **Subagents stay read-only supplements.** Tools restricted to `Read`/`Grep`/
   `Glob` (parity analyst adds `WebSearch`/`WebFetch`). Enforced by
   `scripts/lint.sh` Check 14.
2. **Main session remains the sole mutator / committer / Codex-dispatcher.** No
   subagent edits, builds, commits, or dispatches Codex.
3. **The Phase-1 evidence gate still counts main-session reads**, not subagent
   reads. Agents spare the main loop the *exploration* reads; they do not satisfy
   the gate. The main session still performs its mandatory >= 2 reads per section.
4. **Guard phase machine, fixpoint oracle, section-commit gate, and the spiral
   budget are untouched.**
5. **No Sonnet output is self-certified.** Every Sonnet (or any cheaper-model)
   result is structurally backstopped -- verified by a deterministic script,
   applied/spot-checked by the Opus main loop, or caught by a downstream Codex
   gate. "The agent checked its own work twice" is NOT a backstop (same model,
   same blind spot, double cost). See WS8 for the mechanism.

If a change cannot hold all five, it does not ship.

## 3. Workstreams

### WS1 -- Default-on the existing read-only agents

Lower the firing threshold in the two pipeline skills so the agents fire by
default on non-trivial sections, with a cheap skip only for genuinely tiny work
(docs-only, stamp-only, single-function plumbing, < ~5 candidate files).

- `implement-todo-section` step 3 (explorer): fires by default; skip only tiny.
- `review-todo-section` step 1 (`review-evidence-mapper`): default-on for
  non-trivial sections.
- `review-todo-section` step 7 (`kernel-quality-auditor` / `boot-quality-auditor`):
  default-on for any section touching the owning paths.
- `review-todo-section` step 9 (`parity-research-analyst`): default-on for
  feature sections (the WebSearch cost already lives in the subagent context, so
  firing it more does not grow the main context -- it shrinks it relative to the
  main loop doing the research itself).

Net effect: the main loop receives digests instead of reading 20 files. No gate
change; the main session still does its own >= 2 evidence reads.

Edit surface: `.claude/skills/implement-todo-section/SKILL.md`,
`.claude/skills/review-todo-section/SKILL.md`. Skill-doc edits affect interactive
use too, so they are validated with one interactive section run + build + lint
before being trusted headless.

### WS1b -- Enforce the dispatch (so it cannot be bypassed)

WS1 by itself is skill-doc text, and skill-doc text is the exact thing that gets
silently skipped here. Enforcement parity with the other mandatory triggers means
a **hook**, modeled on `design_review_required.py`:

- A PreToolUse hook (`agent_dispatch_required.py`) that, during the SECTIONS phase
  on a **non-trivial** section, BLOCKs the first implementation Edit/Write until an
  explorer/auditor agent dispatch for the current section is recorded.
- **Triviality carve-out is mandatory, not optional.** Forcing an agent onto a
  2-line stamp/docs/single-function section would *raise* cost -- the opposite of
  the goal. The hook reuses the sequencer triage classifier
  (`sequencer_triage.py --classify`) / candidate-file count to decide trivial vs
  non-trivial; trivial sections are allowed straight through.
- **Recording.** Agent dispatches are observed the same way Codex dispatches are
  (a small state file, e.g. `.claude/state/last-agent-dispatch.json`, written by a
  PostToolUse observer keyed on the `Task`/`Agent` tool + `subagent_type`). The
  gate reads `dispatched_for_section == current` within a freshness TTL.
- **Opt-out.** `SKIP_AGENT_DISPATCH_HOOK=1` on the same call + a logged one-line
  reason in the next message (same shape as the other SKIP envs). For genuine
  cases the model judged tiny-but-misclassified, or a resume where the dispatch
  already happened pre-compaction.
- **Graduated rollout.** Ship first as a WARN through the existing
  `skill_step_observer.py` (emit a reminder, feed the WARN->ERROR promotion
  dataset) for one full file run, then promote to a hard BLOCK once the
  triviality classifier is shown not to false-positive on real sections. This
  mirrors how the repo hardens gates and avoids a day-one false-trap.

Edit surface: new `.claude/hooks/agent_dispatch_required.py`, an observer hook for
recording, `.claude/settings.json` wiring, and a sub-test in
`scripts/test-tooling.sh` (carve-out cases + bypass-shape cases, mirroring the
codex-flag-block test).

### WS2 -- New read-only agent: `diagnostic-digester`

The one genuinely uncovered context sink is **failure-log digestion**. On a
build/test/smoke/Codex failure the main loop currently reads the entire serial /
build log (can be hundreds of KB) into context to diagnose. Exploration is already
covered (kernel-explorer + Explore fallback); failure-log digestion is not.

- **Agent:** `.claude/agents/diagnostic-digester.md`, model **sonnet**
  (backstopped -- the main loop validates the hypothesis before fixing, per
  `superpowers:systematic-debugging`), tools `Read`/`Grep`/`Glob`.
- **Contract:** given a log path (+ optional failing-test name), returns a
  structured digest: candidate root-cause hypotheses, offending `file:line`
  references, and the minimal relevant log slice. It does NOT propose or apply a
  fix (read-only; the main loop owns the fix).
- **Wired into:** `implement-todo-section` fix loop (step 15),
  `review-todo-section` build-fail (step 6), and the `debug-session` /
  `diagnose-serial-log` skills as an optional pre-digest pass.
- **Registration:** CLAUDE.md specialist-agents table row, `.claude/skills/
  README.md` row, `scripts/lint.sh` Check 14 read-only allowlist entry.

Exactly one new agent. Avoiding agent sprawl is itself a maintenance-cost goal --
every agent is a Check 14 entry, a CLAUDE.md row, and a context-doc to keep in
sync.

### WS3 -- Instrumentation (the measurement gate)

We cannot currently see the cost breakdown: the report pipeline is
`claude --output-format stream-json | stream-report.py | tee run-*.log`, and
`stream-report.py` digests the raw stream (which carries `usage` token fields)
down to human text before saving -- the token data is discarded.

- Capture per-section metrics to a sidecar under `.claude/overnight/metrics/`:
  input / output / cache-read / cache-creation tokens, subagent-dispatch count,
  and lsp-vs-grep call ratio. Implemented either by teeing the raw stream-json to
  a sidecar or by having `stream-report.py` emit a metrics line per section
  boundary (preferred -- no extra disk firehose).
- A small analyzer script renders a before/after table.
- **No new external dependency.** This is what decides whether OpenRouter /
  embeddings (Phase B/C) are ever built, and it also gives WS1b a measurable
  before/after (did default-on actually cut main-context tokens, or just move
  spend into agents?).

Edit surface: `scripts/overnight/stream-report.py` (+ a new analyzer script).

### WS4 -- lsp-bridge reliability (dependent, not parallel)

lsp-bridge is the sanctioned context-saver (CLAUDE.md "MCP Usage": prefer
`definition`/`references` over grep -- one canonical answer vs N grep matches that
each cost context to disambiguate). It was observed disconnecting mid-session; when
it is down the main loop falls back to grep and pays the context tax.

`scripts/lsp-mcp/bridge.py` is already +84/-39 modified in the working tree
(timestamp 2026-06-27 05:38) -- a rework appears in flight. Therefore:

- Do **not** start fresh or edit `bridge.py` as part of WS1-WS3.
- Treat WS4 as a dependent task: once the in-flight `bridge.py` change is
  committed, diagnose the disconnect root cause via `superpowers:systematic-
  debugging` and add reconnect / health-check hardening. Until then, WS4 is
  blocked on that commit.

### WS5 -- Effort tiering

Reasoning effort (the thinking-token budget) is the **same model and the same
capability ceiling** at every level -- but thinking tokens bill at the output rate
and are where multi-step planning / SMP-hazard reasoning / self-correction happen.
So lowering effort is a large, direct cost lever whose downside is *realized*
quality on hard tasks, not raw intelligence.

**Mechanism constraint (verified 2026-06-27).** Per-agent *effort* is NOT a knob
the runner can use: the standalone `Agent` tool exposes no `effort` parameter and
agent-definition frontmatter supports only `name`/`description`/`model`/`tools`.
The only place an `effort` override exists is inside a `Workflow` `agent()` call,
and the runner's skills dispatch agents directly (not through a Workflow).
Re-plumbing every dispatch through a Workflow harness is out of scope (heavy,
opt-in, architecture churn). Therefore effort is controllable only at the
**main-loop session level** (set at the headless `claude -p` launch), and the only
per-subagent cost knob is **`model`**.

- **WS5a -- main-loop Medium-vs-High A/B (the real lever; gated on WS3, measured
  not flipped).** The main loop is where the expensive thinking happens. Run one
  comparable file at Medium and one at High; compare *both* main-context output
  tokens *and* end-to-end quality proxies: Codex review-finding count, fix-loop
  rounds, rebuilds, and deferrals. Adopt Medium only if quality holds -- a
  lower-effort implementation that misses a hazard gets caught downstream and
  costs *more* in fix loops than it saved. Never a blind switch. Edits no code
  until a decision is reached; the switch is a one-line launch-flag change.
- **WS5b -- subagent model tiering (model not effort).** The available subagent
  knob is `model`, decided per agent by backstop strength:
  - `parity-research-analyst` -> **Sonnet**. Misses are filed follow-up gaps, not
    crashes, and it already runs Sonnet in its gap-audit/todo-plan mode.
  - `kernel-explorer` -> **Sonnet, flagged for WS3 measurement**. Strong backstop
    (main loop independently re-reads under the evidence gate, then the full
    review pipeline runs). A weaker map only costs the main loop a few more reads.
  - `kernel-quality-auditor` -> **stays Opus**. It is the dedicated SMP /
    lock-order / bare-metal net -- the project's highest-risk dimension -- and the
    only things downstream of it (general Codex adversarial, author self-review)
    are weaker substitutes for that specific bug class. Not a place to save.

  Expect modest savings: these are short-lived digesters whose token volume is
  small next to the main loop's multi-hour context. WS5a is the larger lever.

### WS6 -- Mechanical work: deterministic-first, Sonnet for the rest

The cheapest model is **no model**. Much of the runner's "mechanical" work is
already deterministic script (triage/stamp-classification via
`sequencer_triage.py`, todo-graph rebuild via `build-and-validate.sh`,
build/test/lint), and that must stay scripted -- routing Sonnet at it would replace
a free deterministic check with a paid probabilistic one (a regression).

- **Deterministic-first.** Audit the pipeline for LLM-judgment steps that are
  actually rule-checkable and push them into scripts: table-vs-checklist
  reconciliation *checks*, XREF target-existence checks, stamp-format checks,
  test-count drift checks. The main loop reads a script verdict instead of
  reasoning it out at Opus.
- **Sole-mutator wall.** The remaining mechanical *edits* (writing stamps,
  reconciling Implementation-Order / OS-Comparison tables, Run Log append) are
  edits and stay with the main session. They cannot be offloaded to a read-only
  agent or run at a different model mid-session.
- **WS6 Sonnet close-out auditor (new read-only agent).** For the low-stakes
  *analysis* the main loop currently does itself -- `complete-todo-file` loose-end
  sweep, stale-XREF / drifted-test-count hygiene, table-reconciliation analysis --
  add a read-only **Sonnet** `todo-hygiene-auditor` that returns a punch-list; the
  main loop applies it (sole-mutator preserved). Same registration burden as WS2
  (CLAUDE.md row, README row, Check-14 entry).

### WS7 -- Agent-discipline rules (parallelism + no-bypass)

Two standing rules, enforced not just documented:

- **Serial by default; parallel only when provably independent.** Read-only
  subagents in *separate* tool calls are safe to parallelize (independent throwaway
  contexts, no mutation). The hazard is **&-bundling Codex dispatches into one Bash
  call**, which records only the first review-kind and breaks the section-commit
  gate (already a documented incident). Rule: never &-bundle Codex; parallelize
  only independent read-only work; anything that mutates or shares state runs
  serial. Codify in the runner doctrine + lint-check the &-bundled-codex shape.
- **No silent agent bypass.** This is WS1b. The main session may skip a mandated
  agent dispatch ONLY via the explicit `SKIP_AGENT_DISPATCH_HOOK=1` + a logged
  one-line reason; the WARN-first ramp exists only to validate the triviality
  classifier, and the destination is a hard BLOCK. "Forgot" is not a valid reason.

### WS8 -- Sonnet trust contract (the safety guardrail)

The worry: a Sonnet punch-list (WS6) or digest (WS2) is wrong or incomplete.
"Check it twice" with the same agent is rejected -- same model + same context
rubber-stamps itself, helps wrongness barely, helps incompleteness not at all, and
doubles cost. Instead, two targeted defenses keyed to the two failure modes:

- **Wrongness (false positives) -> deterministic disposal.** Each Sonnet output
  item must be *cheaply checkable*. The auditor *proposes*; a script or the Opus
  main loop *verifies each item before acting* (XREF target exists? test-count
  diff real? placeholder actually present?). Items that fail verification are
  dropped. The verifier does not share Sonnet's blind spots.
- **Incompleteness (false negatives) -> deterministic enumeration owns
  completeness.** For the rule-checkable classes (stale XREFs, drifted test
  counts, missing Deferred stamps, unfilled placeholders) a script produces the
  *complete* set. Completeness is then guaranteed by the script, not the model;
  Sonnet only adds judgment on the genuinely-fuzzy residue (prose-vs-code
  mismatch). A model cannot have the blind spot a full enumerator lacks.
- **Fuzzy residue only -> diverse, not duplicate.** Where a second LLM pass is
  still wanted on the non-enumerable residue, use a *different lens / prompt* (or
  an Opus spot-check) and take the **union** of passes (union raises recall; two
  diverse passes surface more than either alone). Never same-agent-twice.

This contract applies to WS2 (`diagnostic-digester`: its root-cause hypothesis is
*validated by the main loop before any fix* -- never applied blindly) and WS6
(`todo-hygiene-auditor`: script-verified items, script-enumerated completeness).
It is the structural expression of invariant 5.

Edit surface: the per-class enumerator/verifier scripts (extend
`complete-todo-file`'s Verification commands), and the auditor/digester prompts
(emit checkable, itemized output rather than prose).

## 4. Rollout safety

The overnight runner depends on the exact skills WS1/WS2 edit. Land WS1-WS3 only
while the runner is idle (guard `active: false`, systemd unit inactive, no
`launch.lock`) -- verified empty at design time on 2026-06-27. If a run is later
armed, do not edit these skills mid-run.

WS1b carries the highest break-the-runner risk in this phase (a new BLOCK hook in
the SECTIONS path). That is precisely why it ships WARN-first and is gated on the
triviality classifier proving itself over a full file run before it can hard-block.

## 5. Success criteria

- WS1: agents fire on >= 80% of non-trivial sections (measured via WS3 dispatch
  count) without any new gate failure; build + lint green; one interactive section
  validated end-to-end.
- WS1b: WARN phase emits on every non-trivial section that skipped its dispatch and
  is silent on trivial ones (zero false-traps) across one full file run, before
  promotion to BLOCK; `test-tooling.sh` sub-test green.
- WS2: `diagnostic-digester` registered, Check-14 clean, exercised on at least one
  real failure path, returns a usable digest the main loop acts on.
- WS3: per-section metrics sidecar populated for a full file run; before/after
  table shows the main-context token trend.
- WS4 (deferred): lsp-bridge survives a full file run without a disconnect, or the
  disconnect is root-caused and gated.
- WS5a: A/B run produces a Medium-vs-High table on main-context tokens +
  finding-count + fix-loop rounds + deferrals; decision recorded with evidence
  (adopt Medium only if quality proxies do not regress). WS5b: parity + explorer
  retiered to Sonnet show no rise in re-dispatch / bad-digest rate over a full file
  run; kernel-quality-auditor remains Opus.
- WS6: at least one LLM-judgment mechanical check moved to a deterministic script
  with no behavior change; `todo-hygiene-auditor` registered + Check-14 clean +
  produces a punch-list the main loop applies on one close-out.
- WS7: lint-check rejects &-bundled Codex dispatch; doctrine states the
  serial-by-default rule; WS1b BLOCK reachable only via the logged-reason opt-out.
- WS8: every Sonnet output path has a named structural backstop (script verifier,
  Opus application, or downstream Codex); the hygiene enumerator returns the
  complete checkable-class set on a known-dirty fixture; no Sonnet result is
  applied without verification. Invariant 5 holds across the design.

## 6. Out of scope / future phases

- Phase B: OpenRouter-backed cheap-model digester MCP (with Claude-subagent
  fallback), gated on WS3 data showing log/large-file digestion is a top cost.
- Phase C: embeddings + vector store semantic code search, gated on WS3 data
  showing retrieval (not lsp + subagents) is the bottleneck, and on a srclight
  hooks-path-safe integration design.
- Main-loop *model* tier-down (running the main loop on a smaller model, distinct
  from WS5 effort) and Codex-dispatch dedup: not pursued (operator ranked cost
  primary but did not select these; both touch the quality pipeline).
