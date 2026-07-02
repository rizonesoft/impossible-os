# TODO-10 Completion Plan: XREF-Grammar Unification + Tool-Neutral Workflow Protocol

- **Created:** 2026-07-01
- **Owner TODO:** [`todo/00-infrastructure/TODO-10-ai-driver-interchangeability.md`](../../../todo/00-infrastructure/TODO-10-ai-driver-interchangeability.md)
- **Status:** active
- **Mode:** plan-driven (deliberately NOT the per-section `/implement-todo-section` + `/review-todo-section` pipeline)

Section names below refer to the Owner TODO's Implementation Order rows by capability, not by
number, because this file lives outside `todo/` where numeric section shorthand is forbidden.

## Decision (2026-07-01): keep TODO-10, this plan is the execution source

Deleting the Owner TODO was considered and rejected: `TODO-10-ai-driver-interchangeability.md`
is the **test subject** of the `ai_workflow_lease` / `evidence` / `obligations` / `gates`
suites in `scripts/test-tooling.sh` (~50+ references from line 6124, including `--source`
blob-binding and direct file reads), so deleting it would break the tooling suite and require
repointing every reference to a synthetic fixture. It is also the audit trail for the shipped
workflow-protocol scripts. Instead: TODO-10 stays as the tracker/test-subject, and THIS plan is
the single self-contained execution source. The plan references capability names + real file
paths only (no TODO-10 section-number shorthand), so it needs no reference juggling. (Repointing
those tests to synthetic fixtures is a legitimate future cleanup, not required here.)

## Why this plan exists (root problem)

TODO-10 builds the very workflow machinery the review pipeline runs on -- the active-mutator
lease, the append-only evidence ledger, the obligation resolver, the deterministic stamp
writer, and the shared gate library. Driving it through the per-section pipeline is
**recursive**: reviewing a change to the stamp writer runs the stamp / XREF / gate hooks
against the change itself.

The concrete failure this surfaced during the Deterministic Stamp Writer review is that
**"a valid Accepted/Deferred XREF" is defined three inconsistent ways**:

1. `.claude/hooks/accepted_xref_block.py` (git hook) -- loose: uppercase `XREF:` marker + any
   concrete keyword anywhere (`item:` / `at line` / `retrofit` / `helper;`); bare-paren check only.
2. `scripts/todo-graph/build.py` `XREF_CLAUSE_RE` -- strict: needs `XREF: <target_path>` with a
   section marker and an optional quoted `item:` inside the parenthetical.
3. `.claude/skills/review-todo-section/SKILL.md` step 15/16 -- strictest documented form:
   `XREF: NN-domain/TODO-XX <section-marker> (item: "..." at line N)`.

Each Codex review round on `stamp.py` found another seam between these three (marker outside the
parenthetical; prose `at line`; `()` inside quoted item names; mixed valid + ownerless clauses;
a bare TODO ref with no section marker). That is not convergence -- it is whack-a-mole against a
moving target. The durable fix is to define the grammar **once** and have all consumers call it.

## Working-tree state at plan creation

Uncommitted (folded into this plan, NOT yet committed):

- `scripts/ai-workflow/stamp.py` -- stamp-writer hardening: canonical stamp ordering in
  `_insert_stamp` (`_STAMP_ORDER` stable-sort of the group + `_BQ_LABEL_RE` continuation guard);
  accepted/deferred gate requires a concrete TODO XREF; `_file_evidence_ids` reverted to
  section-agnostic (real producers record an empty section).
- `scripts/ai-workflow/common.py` -- `bare_xrefs` / `has_concrete_todo_xref` / `_clause_concrete`
  (marker-inside-parenthetical, case-sensitive to mirror the git hook).
- `scripts/test-tooling.sh` -- 9 new `ai_workflow_stamp` fixtures (462/462 tooling tests green).

**Committed (Phase 0, done):** `41553830` downgraded the Deterministic Stamp Writer section
`[x] -> [/]` with a fold note, to quiet the recursive `section_review_required` gate. No review
stamps were asserted.

> **Fragility note:** the working-tree hardening is uncommitted. Phase 1 step 1 commits it as
> the base so it is not lost across sessions.

## Open review findings to carry (from the stamp-writer review, verified at file:line)

| Ref | Severity | Finding | Disposition in this plan |
|-----|----------|---------|--------------------------|
| F1  | High | `--allow-missing` lets `verified`/`quality-reviewed` mint a transparent `manual`-evidence stamp | Phase 2 -- Shared Gate Library empty-source shipping-evidence rejection (`gates.py`/`obligations.py`) |
| F3/C2 | Med | file-level `_file_evidence_ids` producers record an empty section (gap-audit receipt), so a section filter rejects valid evidence; lookup full-scans the ledger | Phase 3 -- Review-Evidence Integrity gap-audit receipt parser + file-scoping; index-backed lookup |
| RA-H1/RA2 | Med | non-canonical XREFs (marker outside paren, prose `at line`, `()` in item names, mixed ownerless clauses, bare TODO ref with no section marker) | Phase 1 -- unified grammar validator |
| P1 | Low | file-level evidence full ledger scan | Phase 3 -- migrate to `evidence.target_index_path` |

## Environmental constraints (not caused by this work)

- **`build.sh` is globally red:** the Microsoft 2011 UEFI CA expired 2026-06-30, so
  `scripts/sign-efi.sh` aborts EFI signing for every build (owned by the UEFI-hardening TODO in
  `01-boot-platform`). Kernel + bootloader compile clean. For this Python-only work,
  `bash scripts/test-tooling.sh` is the authoritative gate; do not claim "build OK" until the
  shim is repinned.
- **Recursive review gate:** now quiet (Phase 0). If a later phase flips a TODO section to
  `[x]`, the `section_review_required` gate re-arms until that section carries stamps. Land each
  section's `[x]` flip together with its review evidence, or keep it `[/]` until the phase's
  consolidated review.
- **DO NOT arm the overnight sequencer while TODO-10 is plan-driven (operator discipline).**
  `sequencer_triage.traversal_order` includes every implementation TODO and ignores frontmatter
  `status`; the only skip signals are a section `Deferred` stamp or full DONE (Verified +
  Quality-reviewed). TODO-10 has open sections (§2/§5/§6/§7/§9/§10), so an armed sequencer would
  classify it NEEDS_WORK and drive it through the exact recursive pipeline this plan replaces,
  colliding with plan-driven work and the uncommitted hardening. A file-level "hold" mechanism
  was considered and declined in favor of operator discipline: **arm the sequencer only after
  TODO-10 is complete** (or keep it disarmed while working this plan). Arming is a deliberate
  `arm-sequencer.sh` action, so this is enforced by not running that command.

## Goal

Finish the open TODO-10 items (Active-Mutator Lease, Deterministic Stamp Writer, Shared Gate
Library, Review-Evidence Integrity, Migration/Docs/Rollout, Regression Suite + pilot) as a small
number of coherent, well-tested commits, built on **one** XREF grammar, reviewed proportionately
(one consolidated Codex triad per phase on the risky pieces -- not per micro-item). Also fold in
and close out the overnight-runner situational-awareness layer, which consumes the same gate /
obligation state this protocol produces.

## Phases

### Phase 1 -- Unify the XREF grammar (enabling root fix)

> **Refinement (2026-07-01, execution critical-review):** a single STRICT grammar wired to
> the git hook would retroactively reject ~177 of 528 existing Accepted/Deferred stamps
> (only 66% carry the `(item: "..." at line N)` form; the rest use `(Section Title)`). So the
> shared validator encodes the git hook's existing TIERS -- `bare` (no paren) -> BLOCK,
> `soft` (paren, no concrete marker) -> WARN, `concrete` (`item:`/`at line`/`retrofit`/
> `helper;`) -> OK -- as ONE definition all consumers share. The stamp WRITER stays strict
> (emits canonical only); the git hook keeps block/warn (existing stamps survive); todo-graph
> parses `§N` + optional item. Unification = one grammar module, NOT a repo-wide tightening.

1. Commit the working-tree stamp-writer hardening as the base (so it is not lost). DONE (4907c3c8).
2. Add `scripts/ai-workflow/xref.py` with ONE shared grammar module. DONE (2c23f910):
   - `parse()` -> list of `XrefClause(target_path, domain_qualified, section, item_name, tier)`;
   - tiers `bare`/`soft`/`concrete` reproduced byte-for-byte from `accepted_xref_block.py`
     (verified 0 mismatches over 489 corpus clauses -> no stamp flips OK->BLOCK);
   - writer-strict predicates (`writer_bare_xrefs`/`writer_has_concrete`): marker INSIDE a
     parenthetical, markers `item:`/`retrofit`/`helper;`, every clause validated independently;
   - `canonical()` predicate (domain path + `§N` + `(item: "..." at line N)`) for the WRITER.
   - +12 `xref_grammar` unit tests.
3. Point the two ENFORCEMENT consumers at the one grammar:
   - `accepted_xref_block.py` (git hook) -> `bare_clauses`/`soft_clauses`. DONE (2c23f910).
   - `common.py` (`bare_xrefs`/`has_concrete_todo_xref`, used by `stamp.py`) -> `writer_*`.
     DONE (2c905fa3); verified byte-identical over 541 summaries.
   - `todo-graph/build.py` `XREF_CLAUSE_RE` is DELIBERATELY NOT merged: it extracts graph
     EDGES (needs `-> XREF: <path> §N`, drops §-less clauses, captures broader item names);
     routing it through `parse()` would change the graph (488 vs 489 edges). The writer emits
     the canonical XREFs todo-graph consumes -> complementary, not drifted. Documented in
     `xref.py`. DONE (assessed 2026-07-02).
4. Fixtures: the `xref_grammar` unit tests cover `()` in item name (via quoted `[^"]+`), mixed
   valid+bare clauses, bare TODO ref (no paren), and marker-outside-paren. Prose-`at line` is
   covered by the writer's inside-paren rule. (No separate `ai_workflow_stamp` fixture migration
   needed -- the writer path is exercised by the parity verification + these tests.)
5. Re-close the Deterministic Stamp Writer section: flip `[/] -> [x]` with Verified/Quality-
   reviewed stamps, landed together with this phase's review evidence. DONE (dbbd574e).
6. **Review:** one Codex triad (adversarial + consistency + perf) on the unified grammar +
   stamp writer. DONE -- the triad plus 7 confirmation rounds to convergence (10 dispatches,
   6H+5M+2L fixed; commits c2b7d834, e671c2e1, dbbd574e). The writer bar ended STRICTER than
   planned: only the graph-consumable canonical clause (`->` arrow, domain path with adjacent
   section token, structural quote-aware item parenthetical) is writer-acceptable, with a
   fuzz-proven writer-accept-implies-hook-accept subset property and a fixture cross-check
   against todo-graph's XREF_CLAUSE_RE. Two out-of-diff regressions found by the rounds were
   filed with owners (TODO-02 shim fail-loud; TODO-08 staged-secret guard).

### Phase 2 -- Shared Gate Library (remaining items) -- DONE 2026-07-02

Shipped across commits 548f59a1, 63b8761f, 03c8fa79, 36b63464, 7e0fb239; triad
(adversarial + consistency + perf) plus two re-adversarial rounds ran to an explicit
APPROVE closure (9 findings fixed: 3H+1M adversarial, 1H+2M+1L consistency, 1M perf,
1H re-adversarial). What landed:

- Lease OWNERSHIP everywhere: `gates.staged_commit --driver-run-id` +
  `--require-run-id` fail-closed; step5 consult binds `claude-<session_id>` and
  auto-acquires ONLY for the session owning the skill entry; stale completed-lease
  reuse REMOVED (active-only); malformed/zero lease expiry = expired in every layer.
- Forged/stale evidence rejection: build needs source blobs + exit_code=0 +
  log_path + the PINNED `=== BUILD OK ===` sentinel as the log's final line;
  verified-stamps need current source blobs; legacy imports never ship; anonymous
  implement/verify resolution fails closed on lease + reviews.
- Perf: staged blob reads memoized (200 flips = one `git show`).
- Deferred to the rollout phase: routing the remaining Claude-hook checks through
  `gates.py` and flipping `AI_WORKFLOW_ENFORCE_SHARED_GATES` default-on
  (reminders-as-UX-sugar) -- both need the docs/rollout phase toggle.

### Phase 3 -- Review-Evidence Integrity (owns the file-level evidence findings)

- Add `gap-audit` to the receipt-hook prompt parser + tests -- **this is the C2/F3 root cause**:
  the gap-audit receipt records an empty section, so file-level lifecycle evidence must be
  normalized here.
- Derive trusted `review_run_id` from wrapper/receipt metadata, not prompt text.
- Bind review evidence to reviewed source blobs + current HEAD.
- Harden `sequencer_triage.file_lifecycle` to require ledger evidence, not just a preamble line
  (the stamp-writer F2 residual).
- File-scope the file-level evidence producers + migrate `_file_evidence_ids` to
  `evidence.target_index_path` (F3/C2 + P1).
- **Review:** one Codex triad on evidence provenance + fresh-context independence.

### Phase 4 -- Migration, docs, rollout toggles

- Update `docs/infrastructure/ai-system.md`, `CLAUDE.md`, `AGENTS.md` to the tool-neutral
  protocol only after the earlier sections pass. Document the protocol, monitor commands, and the
  `AI_WORKFLOW_ENFORCE_SHARED_GATES` shared-gate toggle.

### Phase 5 -- Regression suite + pilot

- Remaining `ai_workflow_evidence`/`stamp`/`gates` fixtures; lease-race, forged/stale evidence,
  legacy-import, stale completed-lease, lifecycle-stamp fixtures.
- Run one real pilot: implement a low-risk infra section via the shared ledger + gates with Codex
  as reviewer, interrupt, and resume from the ledger. Then flip doctrine items done.

### Phase 6 -- Overnight-runner situational awareness (folded spec; verify + close)

Folds [`docs/superpowers/specs/2026-06-27-overnight-runner-situational-awareness-design.md`](../specs/2026-06-27-overnight-runner-situational-awareness-design.md).
The spec's job: keep the headless main loop on-rails (no drift, gate-surprise, or forgotten
decisions) by aggregating existing scattered run-state into a compact brief. It consumes the
same gate / obligation state the workflow protocol above produces, so it belongs in this plan.

**Verified position (codebase audit 2026-07-01): the four components are already SHIPPED.**

- C1 aggregator -- `.claude/hooks/runner_status.py` prints WHERE / GIT / OBLIGATIONS / GOTCHAS /
  RECENT DECISIONS (`where`/`git_state`/`obligations`/`gotchas`/`recent_decisions`/`full_brief`)
  plus an `anchor_line`. (Spec named `scripts/overnight/runner-status.py`; it shipped as a hook,
  usable as a CLI -- a naming deviation, not a gap.)
- C2 one-line anchor -- `run_phase_guard.py` `_emit_anchor` calls `runner_status.anchor_line`
  from `status` and `phase`.
- C3 `live-gotchas.md` -- `.claude/state/live-gotchas.md` exists and is read by `runner_status`.
- C4 post-compaction re-orient -- `.claude/hooks/pre_compact_flush.py` wired in the `PreCompact`
  hook; `overnight-sequencer` skill instructs reading the brief post-compaction.
- Tests -- `scripts/overnight/tests/test_runner_status.py` exists.

**Remaining work (close-out, not new build):**

- Confirm the shipped layer against the spec's success criteria (brief <= ~30 lines / < 1 s /
  fail-open; anchor emits only when a run is active; obligations flag at least an unreceived
  Codex review + a pushed `[x]` flip lacking a `**Verified:**` stamp; expired gotchas dropped;
  PreCompact snapshot written and read).
- Reconcile the naming deviation: either rename to the spec's path or amend the spec to record
  the hook location as canonical.
- Optional (WS3): measure turns-per-section / re-read counts with vs without the layer to
  validate the focus->cost claim.
- Fusion break-glass reviewer stays deferred/out-of-scope per the spec.

## Review strategy

- One consolidated Codex triad (adversarial + consistency + perf) **per phase**, on the risky
  pieces, when the phase is coherent -- not recursively mid-edit, not per micro-item.
- `superpowers:receiving-code-review` on every finding (verify at file:line, Fix/Reject/Accept).
- The XREF-grammar unification (Phase 1) is what makes later phases stop generating XREF churn.

## Resumability

- Phase 0 is committed (`41553830`). Phase 1 step 1 commits the working-tree hardening.
- Each phase ends at a coherent, tested, committed checkpoint. A new session resumes by reading
  this plan + `git log` + the Owner TODO's Implementation Order table statuses.
- Authoritative gate for Python-only phases: `bash scripts/test-tooling.sh` (build is red
  repo-wide until the shim CA is repinned; that is out of scope here).
