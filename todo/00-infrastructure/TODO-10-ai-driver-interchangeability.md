---
schema_version: 1
id: ai-driver-interchangeability
domain: 00-infrastructure
status: draft
title: "TODO-10 -- AI Driver Interchangeability"
file_patterns:
  - ".claude/skills/*.md"
  - ".claude/skills/**/*.md"
  - "CLAUDE.md"
  - "AGENTS.md"
  - "docs/infrastructure/ai-system.md"
  - "docs/infrastructure/ai-driver-interchangeability.md"
  - "docs/infrastructure/mcp-usage.md"
  - ".claude/settings.json"
  - ".claude/hooks/**"
  - ".claude/state/README.md"
  - ".githooks/**"
  - ".gitignore"
  - "scripts/ai-workflow/**"
  - "scripts/codex-*.sh"
  - "scripts/lint.sh"
  - "scripts/overnight/**"
  - "scripts/test-tooling.sh"
---

# TODO-10 -- AI Driver Interchangeability

> **Validated:** 2026-06-29 | validate-todo-file clean (structure / IO table / XREF / test wiring)

> **Gap-audited:** 2026-06-30 | ai-workflow evidence manual | gap-audit + codex-gap-audit; 6 findings filed (lease race, forged/stale evidence, stale completed leases, lifecycle stamps, live lease lifecycle, dry-run context)

> **Re-scoped:** 2026-06-30 | direction changed from co-equal interchangeable drivers to Claude-master + Codex-delegate/failover (1:4 Claude:Codex effort target); design spec at [docs/superpowers/specs/2026-06-30-claude-master-codex-failover.md](../../docs/superpowers/specs/2026-06-30-claude-master-codex-failover.md). The prior separate-driver overnight artifacts are retired in §14.

> **Goal:** Keep Claude Code as the permanent master driver while offloading the bulk of the work (target 1:4 Claude:Codex effort, Claude ~20%) to Codex, and let Codex resume from shared deterministic state when Claude becomes unavailable (usage limit, crash, hang). Codex remains the required reviewer. The implementation preserves the existing flow, section stamps, review gates, and no-double-work behavior by moving mutable workflow evidence into repo-owned scripts and a deterministic ledger instead of model-specific chat memory, so a handoff carries on from the last completed checkpoint rather than a chat summary.

> [!IMPORTANT]
> **Current state:** Current doctrine remains in force until this TODO ships. Today [`CLAUDE.md`](../../CLAUDE.md) and [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) define Claude Code as the master/orchestrator and Codex as the sole external reviewer. This TODO keeps that authority hierarchy: Claude stays the master driver and Codex stays subordinate. What changes is that Codex gains two new subordinate capabilities -- executing delegated phases while Claude is alive, and resuming leased work when Claude is unavailable -- both behind the shared workflow protocol, gates, and regression tests that prove a subordinate driver cannot skip review, stamp incorrectly, duplicate completed work, or self-certify its own implementation. Doctrine updates land only after those proofs pass.

## Model-Roles Target

Claude Code is the permanent master driver. Codex is always the reviewer, and now also a subordinate executor -- never a co-equal or self-certifying driver.

| Mode                        | Master                       | Subordinate executor                                              | Required reviewer                       | Shipping rule                                                                                          |
| ---                         | ---                          | ---                                                               | ---                                     | ---                                                                                                   |
| Claude solo                 | Claude Code                  | --                                                                | Codex fresh review runs                 | Current flow, backed by the shared ledger.                                                             |
| Delegated (Claude alive)    | Claude Code (design + judgment) | Codex (exploration, typing, build/test, fix-apply, stamp, commit) | Codex fresh review runs (distinct IDs)  | Claude ratifies; Codex executes through the shared gates.                                              |
| Failover (Claude unavailable) | Claude lease, held         | Codex resumes from the ledger at the last completed checkpoint    | Codex fresh review runs (distinct IDs)  | docs/host-tooling: Codex ships through gates. kernel/boot: Codex holds the commit for Claude to bless. |

Target effort split is **1:4 Claude:Codex** (Claude ~20%, Codex ~80%). Claude's reserved share is judgment only: design ratification, Fix/Reject/Accept classification, and kernel/boot commit blessing. Codex carries exploration, implementation, build/test, fix-apply, stamping, gated commits, and the fresh-context reviews. The load-bearing pattern is **Codex proposes, Claude ratifies** -- Codex always returns a recommended approach or per-finding classification with evidence so Claude's step stays a low-cost ratify/override rather than a full re-derivation.

Codex is both the subordinate executor and the required reviewer, so the two roles must use distinct runs:

```text
codex-executor             -> reads/edits/builds/tests/applies fixes/stamps/commits under the lease
codex-reviewer-design      -> fresh-context design review
codex-reviewer-adversary   -> fresh-context adversarial review
codex-reviewer-consistency -> fresh-context consistency audit
codex-reviewer-perf        -> fresh-context performance review
```

A Codex executor run can never satisfy a Codex reviewer obligation with the same run ID.

## Inputs

- [`CLAUDE.md`](../../CLAUDE.md) -- current doctrine, model roles, mandatory skill triggers, Codex invocation policy, commit policy
- [`AGENTS.md`](../../AGENTS.md) -- cross-tool pointer and current reviewer-only boundary for non-Claude tools
- [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) -- authority hierarchy, external-reviewer contract, hook routing matrix
- [`docs/infrastructure/ai-driver-interchangeability.md`](../../docs/infrastructure/ai-driver-interchangeability.md) -- canonical section-1 inventory and Codex-driver operations guide
- [`todo/00-infrastructure/TODO-02-ai-development-system.md`](TODO-02-ai-development-system.md) -- canonical AI-system ownership roadmap
- [`todo/00-infrastructure/TODO-08-automation-hardening.md`](TODO-08-automation-hardening.md) -- existing hook/evidence/stamp hardening work
- [`.claude/state/README.md`](../../.claude/state/README.md) -- current runtime state schemas: skill progress, Codex review history, review stamps
- [`.claude/skills/**/*.md`](../../.claude/skills/) -- canonical workflow step, prompt-template, and review-kind marker obligations
- [`.claude/hooks/`](../../.claude/hooks/) -- current Claude-harness enforcement hooks
- [`.githooks/`](../../.githooks/) -- git-time enforcement layer that must protect non-Claude drivers
- [`scripts/codex-dispatch.sh`](../../scripts/codex-dispatch.sh) -- current safe Codex review dispatch wrapper
- [`scripts/lint.sh`](../../scripts/lint.sh) -- Check 12 prompt-escaping telemetry and Check 15 dispatch-bundling telemetry for documented Codex dispatch examples
- [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh) -- tooling regression suite for hook and AI-system behavior
- [`scripts/todo-graph/`](../../scripts/todo-graph/) -- TODO cache, validation, backlink, and ready/blocked graph oracle
- -> XREF: [`00-infrastructure/TODO-06 TODO Metadata Layer`](TODO-06-todo-metadata-layer.md) -- stable TODO IDs and graph cache used by the obligation resolver
- -> XREF: [`00-infrastructure/TODO-07 LSP to MCP Bridge`](TODO-07-lsp-mcp-bridge.md) -- code-intelligence parity for Claude and Codex drivers

## Outcome

- A repo-owned AI workflow protocol exists under `scripts/ai-workflow/`; it is callable from Claude hooks, the Codex executor script, git hooks, and overnight automation.
- Exactly one active mutating driver holds a lease for a TODO section or review target at a time; the master Claude lease can be handed to the Codex executor on delegation or failover via an audited transfer.
- Review/build/test/stamp evidence is stored in a tool-neutral ledger keyed by TODO target, review kind, git HEAD, source blob SHAs, run ID, and role.
- A deterministic obligation resolver reports what is already satisfied and what is still missing before any model does work, so a resumed run continues from the last completed checkpoint.
- A deterministic stamp writer adds `Verified:` / `Quality reviewed:` / `Deferred:` / `Accepted:` stamps only from ledger-backed evidence.
- Commit gates read the shared evidence ledger instead of Claude-only chat or hook state.
- Claude offloads exploration and the mechanical tail to Codex so the master Claude stays near the ~20% effort target; a per-run effort meter reports the realized Claude:Codex ratio.
- When Claude becomes unavailable (usage limit, crash, hang), Codex resumes the active lease from the ledger at the last completed checkpoint, reverting any half-finished in-flight edit first, and reads the Claude-authored intent plan so it continues with design intent rather than a chat summary.
- Failover honors a risk tier: Codex ships docs/host-tooling sections through the gates; kernel/boot commits wait for Claude to bless on return.
- Codex remains the mandatory reviewer in every mode; delegated and failover execution use fresh reviewer runs with run IDs distinct from the Codex executor run.
- No parallel skill tree is introduced. Shared mechanics live in scripts, docs, hooks, and derived runtime state, not a model-specific instruction directory.
- All drivers write reports/metrics/logs into the shared overnight namespace tagged by backend; the separate `.codex/overnight/` tree is retired. Governance state remains shared through the sequencer cursor, driver lease, evidence ledger, stamp writer, and commit gates.
- Shared gates prove that concurrent driver races, forged evidence, stale completed leases, and stale provenance cannot satisfy shipping obligations.
- `stamp.py` can generate both section-local shipping stamps and file-level lifecycle stamps consumed by the overnight sequencer.
- Doctrine updates land only after the implementation proves the shared protocol can enforce the same safety properties as the current Claude-first flow.

## Implementation Order

| ⭐  | Order | Deliverable                                                           | Depends On    | Status |
| --- | :---: | --------------------------------------------------------------------- | ------------- | :----: |
| 💎  |   1   | Current-state inventory of Claude-only assumptions and gate inputs     | --            |  [x]   |
| 💎  |   2   | Driver lease schema and active-mutator lock                            | §1            |  [/]   |
| 💎  |   3   | Tool-neutral evidence ledger and importer for existing state           | §1            |  [x]   |
| 💎  |   4   | Obligation resolver to avoid duplicate work                            | §3            |  [x]   |
| 💎  |   5   | Deterministic stamp writer                                             | §3-4          |  [ ]   |
| 💎  |   6   | Shared gate library used by Claude hooks, git hooks, and Codex driver  | §3-5          |  [ ]   |
| ⭐  |   7   | Codex reviewer-role normalization and fresh-context independence checks | §3-6          |  [ ]   |
| ⭐  |   8   | Codex executor dry-run adapter and intent handoff plan                 | §2, §4, §6-7 |  [/]   |
| ⭐  |   9   | Codex executor live adapter and failover resume                        | §7-8          |  [/]   |
| 💎  |   10  | Namespace consolidation, migration, docs, doctrine, rollout toggles    | §1-9          |  [/]   |
| 💎  |   11  | Regression suite and pilots (delegation / failover / tiered-hold)      | §1-10         |  [ ]   |
| ⭐  |   12  | Codex exploration-brief producer                                      | §3-4          |  [ ]   |
| 💎  |   13  | Effort-ratio meter (1:4 Claude:Codex)                                 | §3            |  [ ]   |
| 💎  |   14  | Retire separate-driver direction artifacts                            | §9-10         |  [ ]   |

---

## 1. Current-State Inventory of Claude-Only Assumptions and Gate Inputs

Identify every place the current system assumes Claude Code is the mutator, dispatcher, committer, or stamp author.

- [x] Inventory Claude-harness-only state readers/writers in [`.claude/hooks/`](../../.claude/hooks/) using the hook manifest and state scan: step-gate state, Codex dispatch/receipt state, specialist-agent state, heuristic/audit logs, session metadata, transcript caches, interactive overnight fallback state, sequencer control files, and tool telemetry.
- [x] Inventory commit gates that currently depend on Claude tool-call history rather than repository evidence.
- [x] Inventory every stamp shape that is freehand-written by a model today: `Verified:`, `Quality reviewed:`, `Deferred:`, `Accepted:`, `Gap-audited:`, and `Validated:`.
- [x] Inventory the required review kinds for each workflow: design, adversarial-impl, adversarial, consistency, perf, test-coverage, re-adversarial, gap-audit.
- [x] Inventory Codex dispatch surfaces in the canonical inventory doc: foreground wrapper, fallback wrappers, direct companion review/task, bare review/task, driver exec/e, plugin slash commands/hooks, and parser assumptions.
- [x] Identify which state is durable evidence and which is merely session telemetry. Durable evidence graduates to the shared ledger; telemetry stays model-specific.
- [x] Produce `docs/infrastructure/ai-driver-interchangeability.md` with the inventory table and migration notes.
- [x] Commit: "ai-workflow: inventory Claude-only assumptions"

**Test checkpoint:** A maintainer can point at each gate/stamp and say whether it is already tool-neutral, needs a shared-script wrapper, or stays Claude-specific telemetry.

> **Test runner:** 2026-06-29 | `bash scripts/test-tooling.sh` | 423/423 PASS
>
> **Notes:**
> - Inventory source lives in [`docs/infrastructure/ai-driver-interchangeability.md`](../../docs/infrastructure/ai-driver-interchangeability.md).
> - §1 records current-state evidence only; doctrine remains unchanged until rollout sections ship.
> - Adversarial findings were classified Fix; inventory now covers AGENTS.md reviewer-only boundary, Claude state files, live gates, stateless blockers, dispatch surfaces, Codex prompt escaping / lint Check 12 and dispatch-bundling Check 15 as WARN-only telemetry (not proof and not complete fallback-wrapper coverage), prompt-source trust caveats (`codex e`, background receive integrity) tagged non-trusted, sequencer/FIXPOINT control, telemetry, stamps, and review kinds derived from canonical `.claude/skills/**/*.md` workflow and prompt-shape sources.
> - Owner-section findings are filed, not hidden here: §7 owns reviewer trust, parser, dispatch completion, and `codex e`; §6/§11 own legacy import and stale-evidence rejection.
> - Build and graph validation are ledger-backed; use `evidence.py explain --todo <path> --section 1` for current event IDs.
> - Scope boundary: no doctrine or rollout implementation ships in §1; dependent gate and classifier fixes ship in later sections.
>
> **Verified:** 2026-06-29 | working tree validation
> - `bash scripts/test-tooling.sh` -> 423/423 PASS.
> - `bash scripts/todo-graph/build-and-validate.sh --keep-cache` -> 8/8 PASS.

---

## 2. Driver Lease Schema and Active-Mutator Lock

Prevent Claude and Codex from both mutating the same section or producing conflicting stamps.

- [x] Add `scripts/ai-workflow/lease.py` with `acquire`, `renew`, `release`, `status`, and `force-release --reason` subcommands.
- [x] Store lease state in a gitignored derived-state location that is not a model-specific instruction tree. Preferred shape: `.ai-workflow/active-lease.json`.
- [x] Lease key includes `todo_path`, `section`, `driver_backend`, `driver_run_id`, `head_sha`, `started_at`, `expires_at`, and `allowed_mutations`.
- [ ] Add a monotonic lease generation/fence token to the lease key so delegate, failover, and hand-back transfers use compare-and-swap and a stale holder is rejected before any edit, stamp, or commit.
- [x] A driver cannot acquire a lease if another live lease exists for the same TODO section unless the existing lease is expired or explicitly force-released with a reason.
- [x] Serialize `lease.py` active-lease and lease-history mutations with a repo-local lock (`fcntl.flock` or an `O_EXCL` lockfile) so two concurrent drivers cannot both observe an empty lease and race through `acquire` (`scripts/ai-workflow/lease.py:53` reads, checks, then writes at `:94` today).
- [x] Claude hooks consult the lease before first implementation edit when a TODO-section flow is active.
- [x] Codex driver adapter acquires the same lease before any edit, build, test, or commit attempt.
- [/] Git commit gate refuses a section-ship commit when no matching active lease or completed lease record exists for the staged TODO target. The shared staged gate enforces this under `AI_WORKFLOW_ENFORCE_SHARED_GATES=1`; default enforcement waits for Claude lease acquisition.
- [x] Add a concurrent-acquire fixture that launches two same-section drivers and asserts exactly one winner plus one durable conflict event.
- [x] Add `scripts/test-tooling.sh` coverage for acquire/release and same-section conflict.
- [ ] Commit: "ai-workflow: add driver lease"

**Test checkpoint:** Two simulated drivers cannot both edit or commit the same section. A stale lease can be recovered with an auditable reason.

---

## 3. Tool-Neutral Evidence Ledger and Importer for Existing State

Make evidence belong to the workflow, not to Claude or Codex chat history.

- [x] Add `scripts/ai-workflow/evidence.py` with `record`, `query`, `explain`, `import-legacy`, and `gc` subcommands.
- [x] Store ledger events in a structured append-only JSONL file under `.ai-workflow/evidence.jsonl`.
- [x] Event schema includes: `event_id`, `task_id`, `todo_path`, `section`, `role`, `backend`, `run_id`, `kind`, `head_sha`, `source_blobs`, `result`, `summary_path`, `created_at`, and `expires_at`.
- [x] Review evidence is keyed by review kind and source blob SHAs, not by path alone.
- [x] Build/test/smoke evidence records the command, exit code, log path, final marker, HEAD, and freshness rule.
- [x] Stamp evidence records the generated stamp text hash and the source evidence IDs consumed.
- [x] Importer reads existing `.claude/state/last-review-stamps.json`, `codex-review-history.jsonl`, and relevant TODO stamps into compatibility events.
- [x] Importer marks legacy records as `legacy_import: true` so gates can warn during transition without silently trusting incomplete evidence.
- [x] Add a small `evidence explain <todo> --section N` view for humans and model drivers.
- [x] Commit: "ai-workflow: add evidence ledger"

**Test checkpoint:** A section with existing review/build evidence can be explained from the ledger without reading chat history. Editing a reviewed source file invalidates the relevant review evidence through blob mismatch.

> **Test runner:** 2026-06-29 | `bash scripts/test-tooling.sh` | 423/423 PASS
>
> **Notes:**
> - Ledger events are derived state under `.ai-workflow/`, not model instruction state.
> - Legacy Claude state is imported as compatibility evidence and is marked so gates can warn during transition.
>
> **Verified:** 2026-06-29 | working tree validation
> - `bash scripts/test-tooling.sh` -> 423/423 PASS.
> - `bash scripts/todo-graph/build-and-validate.sh --keep-cache` -> 8/8 PASS.

---

## 4. Obligation Resolver To Avoid Duplicate Work

Every driver starts by asking what is missing, not by replaying the whole workflow.

- [x] Add `scripts/ai-workflow/obligations.py`.
- [x] Resolver inputs: TODO path, section number, workflow kind (`implement`, `review`, `verify`, `complete-file`), staged diff, HEAD, and evidence ledger.
- [x] Resolver outputs machine-readable JSON plus a concise text view.
- [/] Required obligations include domain quality preflight, design review, implementation edit, test wiring, build, unit tests, smoke when boot-path touched, Codex review kinds, receiving/triage, fix loop, TODO stamp, todo-graph validation, and commit.
- [x] Satisfied obligations include the evidence IDs that prove satisfaction.
- [x] Missing obligations include the next acceptable action and the exact command/script to run when deterministic.
- [x] Resolver dedupes repeated Codex review kinds by content hash. Same review kind against unchanged blobs is not rerun unless TTL, HEAD ancestry, or policy requires it.
- [x] Resolver detects unsafe reuse: same Codex run ID used as both driver and reviewer, stale source blobs, stale build after source edit, or stamp generated from legacy-only evidence.
- [/] Add fixtures for current Claude-mode sections, stamp-only reviews, docs-only sections, boot-path smoke-required sections, and Codex-exclusive role separation.
- [x] Commit: "ai-workflow: add obligation resolver"

**Test checkpoint:** Running the resolver twice after satisfying one obligation reports one fewer missing item and never asks for already-covered review work.

> **Test runner:** 2026-06-29 | `bash scripts/test-tooling.sh` | 423/423 PASS
>
> **Notes:**
> - Resolver output is intentionally driver-neutral and keyed by evidence, HEAD, staged diff, and workflow kind.
> - Remaining fixture breadth is tracked inside this section without marking the rollout complete.
>
> **Verified:** 2026-06-29 | working tree validation
> - `bash scripts/test-tooling.sh` -> 423/423 PASS.
> - `bash scripts/todo-graph/build-and-validate.sh --keep-cache` -> 8/8 PASS.

---

## 5. Deterministic Stamp Writer

Stop relying on model-authored stamp prose for workflow truth.

- [x] Add `scripts/ai-workflow/stamp.py`.
- [x] Supported stamp commands: `verified`, `quality-reviewed`, `deferred`, `accepted`, `validated`, `gap-audited`.
- [x] `verified` and `quality-reviewed` stamps require ledger evidence for the relevant build/test/review obligations.
- [x] Stamp text is generated from evidence IDs and short human summaries; models may provide a summary field, but not the proof fields.
- [x] Stamp writer refuses to add a stamp when required evidence is missing, stale, legacy-only where fresh evidence is required, or produced by a disallowed role combination.
- [x] Stamp writer preserves existing TODO formatting and inserts in the section-local location used by the current roadmap style.
- [ ] Add a file-level lifecycle insertion mode for `validated` and `gap-audited` that writes under the H1 and above `> **Goal:**`, because `.claude/hooks/sequencer_triage.py:127` only recognizes those stamps in the preamble while `scripts/ai-workflow/stamp.py:47` only matches numbered section headings today.
- [x] Stamp writer records a `stamp.generated` ledger event with the exact text hash.
- [x] Add `--dry-run` and `--explain-missing` modes for both Claude and Codex drivers.
- [/] Add regression fixtures for multiline notes, existing stamp updates, stamp-only commits, file-level lifecycle preamble placement, and no-bare-XREF enforcement.
- [ ] Commit: "ai-workflow: add deterministic stamp writer"

**Test checkpoint:** A model cannot create a successful section stamp unless the ledger proves the required workflow happened.

---

## 6. Shared Gate Library Used By Claude Hooks, Git Hooks, and Codex Driver

Make the enforcement layer reusable outside Claude Code.

- [x] Add `scripts/ai-workflow/gates.py` with callable checks for lease, obligations, review evidence, build freshness, smoke freshness, stamp validity, and commit eligibility.
- [ ] Refactor Claude hooks to call `gates.py` where feasible instead of duplicating logic.
- [/] Refactor `.githooks/pre-commit` to call the same checks for section-ship and stamp-only commits. It now calls `gates.py staged-commit` behind `AI_WORKFLOW_ENFORCE_SHARED_GATES=1`; default-on enforcement waits for Claude lease acquisition.
- [ ] Harden shipping evidence validation so `gates.py` / `obligations.py` reject forged or stale events: review evidence must come from the receipt path with distinct `review_run_id`, current non-empty source blobs and current HEAD; build/test/smoke evidence must carry exit code, log path, and final marker; stamp evidence must point at current source evidence; todo-graph validation must be current for the TODO blob (`scripts/ai-workflow/obligations.py:118` currently accepts kind/result/role-prefix alone and `:142` disables current-source checks for graph validation).
- [ ] Make `evidence.py import-legacy` treat `.claude/state/last-review-stamps.json` as non-shipping dispatch telemetry, not `result: received` review proof; obligations must reject `legacy_import` or empty-source reviewer evidence for shipping unless an explicit transition flag allows it (`scripts/ai-workflow/evidence.py:296` currently imports those stamps as reviewer events).
- [ ] Remove stale completed-lease reuse from `staged-commit`, or bind completed leases to the exact final diff/evidence and consume them once; `scripts/ai-workflow/gates.py:53` currently accepts any historical `complete` action for the same TODO section.
- [x] Add a Codex-driver preflight command that calls the same checks before editing, before stamping, and before committing.
- [x] Preserve existing opt-out shapes where policy already permits them, but record all skips in a shared skip ledger.
- [ ] Keep Claude-specific reminders as UX sugar only. The blocking decision must be made by shared scripts or git hooks.
- [/] Add tests proving the same staged diff receives the same verdict when invoked from Claude hook mode, git-hook mode, and direct CLI mode. Direct mode, git-hook mode, and Claude-hook mode now share the same `gates.py commit` verdict; full staged-diff parity remains behind the rollout flag.
- [ ] Commit: "ai-workflow: share gates across drivers"

**Test checkpoint:** A direct shell commit attempt and a Claude `git commit` attempt are blocked or allowed for the same reason.

---

## 7. Codex Reviewer-Role Normalization and Fresh-Context Independence Checks

Codex is always the reviewer, including when Codex is also the subordinate executor. This independence is load-bearing under the 1:4 split, because Codex is now both executor and reviewer on most sections.

- [x] Define canonical reviewer roles: `codex-reviewer-design`, `codex-reviewer-adversarial-impl`, `codex-reviewer-adversarial`, `codex-reviewer-consistency`, `codex-reviewer-perf`, `codex-reviewer-test-coverage`, `codex-reviewer-gap-audit`, and `codex-reviewer-re-adversarial`.
- [/] Update Codex dispatch prompts so each role records `role`, `review_kind`, `driver_run_id`, and `review_run_id`. The receipt hook now records these fields when present and generates `review_run_id` for legacy dispatches; prompt-template rollout remains.
- [ ] Derive trusted `driver_run_id` / `review_run_id` for gate satisfaction from the driver lease, dispatch wrapper/session state, or receipt process metadata; prompt-authored run IDs are correlation hints only and must not prove reviewer independence (`.claude/hooks/codex_review_completed.py:601` currently reads them from prompt text and `:617` fabricates a reviewer ID).
- [ ] Add `gap-audit` to the receipt hook prompt parser and tests so `[review-kind: gap-audit]` dispatches mirror to the shared ledger; `.claude/hooks/codex_review_completed.py:475` omits it today even though the ledger and skill map recognize it.
- [ ] Normalize bare Codex CLI review detection so `codex review` remains a review receipt shape but `codex e` is treated as the `codex exec` alias, not reviewer proof; `_CODEX_BARE_SUBCMDS = ("review", "e")` in [`.claude/hooks/_codex_dispatch.py`](../../.claude/hooks/_codex_dispatch.py) currently makes the alias a trusted review trigger.
- [x] Review evidence is rejected when `review_run_id == driver_run_id`.
- [x] Review evidence is warned or rejected when the reviewer prompt contains implementor self-summary language instead of evidence-first scope.
- [x] The obligation resolver treats fresh-context Codex review as required even when the driver backend is Codex.
- [/] Add a reviewer-output receipt step that classifies findings as Fix / Reject / Accept-XREF with file-line evidence, independent of which driver will apply fixes. The receipt hook now mirrors received Codex reviews into the shared ledger; finding classification remains with the existing receiving-code-review workflow.
- [x] Preserve the no-model-flag policy: Codex model and effort remain controlled centrally by Codex config, not per dispatch.
- [ ] Commit: "ai-workflow: normalize Codex reviewer roles"

**Test checkpoint:** A Codex executor run can satisfy review obligations only with distinct reviewer runs. A Codex executor cannot self-certify its own implementation.

---

## 8. Codex Executor Dry-Run Adapter and Intent Handoff Plan

Introduce Codex as an executor without allowing it to mutate code at first, and make the plan artifact double as the intent-handoff record a delegated or failover Codex run resumes from.

- [x] Add `scripts/codex-driver.sh --dry-run`.
- [/] Dry-run adapter reads `AGENTS.md`, `CLAUDE.md`, the target TODO, obligations, and code context; bounded content capture remains open (`scripts/codex-driver.sh:74` / `:84`).
- [ ] Capture bounded dry-run context with source hashes or snippets for the doctrine files, the target TODO, the obligation JSON, and relevant code files so the advisory plan proves what it inspected.
- [x] Dry-run adapter may propose an implementation plan and list deterministic commands, but must not edit, build, test, stamp, or commit.
- [x] Dry-run output is written to `.ai-workflow/runs/<run-id>/plan.md`.
- [ ] Promote `plan.md` to the canonical intent-handoff artifact the master Claude writes at section start, so a delegated or failover Codex run resumes with design intent (consumed by §9).
- [x] Claude-mode workflow can consume the dry-run plan as advisory input without treating it as evidence.
- [x] Add a regression that verifies dry-run cannot produce ledger events that satisfy shipping obligations.
- [/] Run one pilot dry-run on a small docs-only infrastructure section and one kernel section, then record gaps.
- [x] Commit: "codex: add dry-run driver adapter"

**Test checkpoint:** Codex can understand the workflow and produce a useful plan, but no gate accepts that plan as proof of completed work. A failover Codex run can reconstruct design intent from `plan.md` alone.

> **Test runner:** 2026-06-29 | `bash scripts/test-tooling.sh` | 423/423 PASS
>
> **Notes:**
> - Dry-run output is advisory only and cannot satisfy shipping evidence.
> - Pilot execution remains tracked as rollout work before live driver support is declared available.
>
> **Verified:** 2026-06-29 | working tree validation
> - `bash scripts/test-tooling.sh` -> 423/423 PASS.
> - `bash scripts/todo-graph/build-and-validate.sh --keep-cache` -> 8/8 PASS.

---

## 9. Codex Executor Live Adapter and Failover Resume

Codex executes under the master Claude's lease, both for delegated phases (Claude alive) and failover resume (Claude unavailable). It never self-certifies and never commits kernel/boot work without Claude blessing.

Executor core:

- [x] Add `scripts/codex-driver.sh --live` behind an explicit environment gate such as `AI_WORKFLOW_CODEX_DRIVER=1`.
- [x] Live adapter acquires a driver lease before any mutation.
- [x] Live adapter runs the obligation resolver before each phase and after each fix.
- [/] Live adapter may edit files only while the lease is active and the next obligation allows mutation.
- [/] Live adapter dispatches Codex reviewer roles through `scripts/codex-dispatch.sh` and records reviewer evidence separately from executor evidence with distinct run IDs; live evidence recording still needs pilot validation.
- [ ] Block live and failover shipping on §7 trusted run-ID derivation: the `review_run_id != driver_run_id` check is necessary but not sufficient while IDs come from prompt text; forged, fabricated, and legacy IDs must be rejected first.
- [ ] Live adapter runs build/test/smoke commands through the same deterministic scripts as Claude mode.
- [ ] Live adapter records build/test/smoke/todo-graph evidence through `scripts/ai-workflow/evidence.py` with command, exit code, log path, final marker, HEAD, and source blobs before gates consume it.
- [/] Live adapter uses `stamp.py`, never freehand stamp text.
- [x] Live adapter attempts commits only through the same shared gate path as Claude mode.
- [/] Live adapter manages lease lifecycle explicitly: release and complete checkpoints exist; renew-during-long-phase and complete-only-after-successful-commit enforcement remain open.

Failover resume (Claude unavailable):

- [ ] The repo-owned launcher/supervisor (not Claude, which is unavailable) detects triggers and initiates handoff: usage-limit (launcher snooze detection) and crash/hang (supervisor watchdog). Manual handoff is out of scope.
- [ ] Define the lease-transfer state machine: fence token, compare-and-swap transfer, audited `handoff_reason` (`delegate`/`limit`/`crash`/`hang`/`return`), and Codex stop/ack at obligation boundaries so a return cannot race a live edit.
- [ ] Resume granularity is the last completed obligation from `obligations.py`; revert any half-finished in-flight edit to the last clean checkpoint before resuming.
- [ ] Define the clean checkpoint as a manifest (HEAD, worktree status, owned paths, source blobs, plan hash, obligation ID); revert touches only owned paths and aborts on a stale/missing plan or dirty unrelated files.
- [ ] Read the Claude-authored intent plan (`.ai-workflow/runs/<run-id>/plan.md`) so resume carries design intent, not just the remaining checklist.
- [ ] Honor the risk tier on resume: docs/host-tooling ships through the gates; kernel/boot stops before commit and writes a `ready-for-bless` marker for Claude.
- [ ] Clean hand-back: when Claude returns it reacquires the lease atomically (never both editing) and advances the cursor past sections Codex already shipped.
- [ ] Pilot delegation on one docs-only section, then failover-resume on one docs-only section, then one host-tooling section, before any kernel/boot work.
- [ ] Commit: "codex: add executor live adapter and failover resume"

**Test checkpoint:** Codex can ship a low-risk section through the same gates without Claude acting as mutator, and can resume a leased section from the ledger after Claude becomes unavailable, holding kernel/boot commits for Claude.

---

## 10. Namespace Consolidation, Migration, Docs, Doctrine, and Rollout Toggles

Only update doctrine after the shared protocol works.

- [x] Add `docs/infrastructure/ai-driver-interchangeability.md` as the human-readable design and operations guide.
- [ ] Consolidate the Codex overnight namespace into the shared overnight tree, tagging reports/metrics/logs by backend; the separate `.codex/overnight/` tree is removed in §14.
- [ ] Update [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) to distinguish the master driver from the subordinate executor (Claude master; Codex executor and reviewer) only after §1-9 pass.
- [ ] Update [`CLAUDE.md`](../../CLAUDE.md) Model Roles and Skills sections only after the Codex delegation and failover pilots succeed.
- [ ] Update [`AGENTS.md`](../../AGENTS.md) so non-Claude tools understand when they are reviewer-only versus leased as the subordinate executor under Claude.
- [x] Preserve the autonomous-agent boundary: no cloud-agent PRs, no autonomous GitHub PR authoring, no unattended non-repo-guarded driver.
- [x] Preserve the zero AI-attribution trailer policy.
- [/] Document supported modes and required environment toggles (re-scoped to master/delegate/failover; guide refresh owned by §14).
- [/] Document the overnight reports, monitor command, approval policy, and sandbox override, updated for the consolidated shared namespace.
- [x] Add rollback instructions to return to Claude-master-only mode by disabling the Codex executor while keeping shared evidence scripts available.
- [ ] Commit: "docs: document AI driver rollout"

**Test checkpoint:** A maintainer can enable or disable Codex executor mode without deleting evidence, breaking stamps, or changing commit policy.

---

## 11. Regression Suite and Pilot Section

Prove the master/delegate/failover workflow before declaring it available.

- [x] Add `ai_workflow_lease` tests to `scripts/test-tooling.sh`.
- [/] Add `ai_workflow_evidence` tests for blob binding, stale HEAD, legacy import, and review-role independence.
- [x] Add `ai_workflow_obligations` tests for no-double-work behavior across Claude-driver and Codex-driver modes.
- [/] Add `ai_workflow_stamp` tests for generated stamps, missing evidence failures, and stamp-only commits.
- [/] Add `ai_workflow_gates` tests proving Claude hook mode, git-hook mode, and direct CLI mode return identical verdicts.
- [x] Add `codex_driver_dry_run` tests proving dry-run cannot mutate or satisfy evidence.
- [/] Add `overnight_launch_driver` tests proving Claude/Codex driver selection, bounded-supervisor smoke execution, and backend-tagged reports in the shared overnight namespace (was separate `.codex/overnight`).
- [/] Add `codex_driver_live_fixture` tests using a temporary fixture repo or test TODO section.
- [ ] Add a failover-resume fixture: Claude records obligations 1-3 then goes unavailable, and Codex resumes 4-N from the ledger without repeating completed reviews.
- [ ] Add dirty-worktree and stale/missing-plan fixtures proving failover aborts safely instead of discarding unrelated or untracked changes.
- [ ] Add a tiered-hold fixture proving Codex ships a docs/host-tooling section but stops before a kernel/boot commit, leaving a `ready-for-bless` marker.
- [ ] Add a lease-race fixture that starts two concurrent same-section `lease.py acquire` calls and proves exactly one winner.
- [ ] Add forged/stale evidence fixtures: self-recorded Codex review events, legacy-only shipping evidence, stale source blobs, stale HEAD, missing build final marker, and stale todo-graph validation must not satisfy obligations.
- [ ] Add a legacy-import fixture proving `last-review-stamps.json` imports do not satisfy adversarial, consistency, or perf review obligations.
- [ ] Add a stale completed-lease fixture proving a historical `complete` action cannot authorize a later staged TODO row flip or stamp.
- [ ] Add a lifecycle-stamp fixture proving `stamp.py validated` / `gap-audited` writes only the file preamble location consumed by `sequencer_triage.py`.
- [ ] Add dry-run context-capture and live-adapter lease-failure fixtures covering the §8 and §9 regressions filed by this gap audit.
- [ ] Run one real pilot in Claude-master delegated mode using the shared ledger, with Codex as executor and a separate Codex reviewer.
- [ ] Run one real pilot in Codex-executor dry-run mode.
- [ ] Run one real failover-resume pilot: interrupt Claude mid-section and let Codex finish a low-risk infrastructure section from the ledger.
- [ ] After all pilots pass and the effort report shows the realized ratio at or near 1:4, flip this TODO's doctrine/doc-sync items to done and mark Codex delegation + failover as supported.
- [ ] Commit: "test: cover AI driver interchangeability"

**Test checkpoint:** The same TODO section can be resumed by Claude or Codex from the ledger without repeating completed reviews, losing stamp state, or bypassing gates.

---

## 12. Codex Exploration-Brief Producer

Move bulk code reading off the master Claude so the 1:4 effort split is achievable. Claude subagents count against the same Claude limit, so exploration must run as Codex, not a Claude `Explore`/`kernel-explorer` agent.

- [ ] Add a Codex exploration mode (extend `scripts/codex-driver.sh` or a sibling) that reads the target subsystem and emits a bounded brief: file list, integration surface, lock/init-order notes, and a recommended approach.
- [ ] Brief output is advisory context written under `.ai-workflow/runs/<run-id>/`, never shipping evidence.
- [ ] Claude consumes the brief to ratify or redirect the design instead of reading every file itself.
- [ ] `implement-todo-section` step 3 routes large/unfamiliar surfaces to the Codex brief instead of the Claude explorer subagent when the Codex executor is enabled.
- [ ] Record brief provenance (source blobs inspected) so a stale brief is detectable after edits.
- [ ] Commit: "codex: add exploration-brief producer"

**Test checkpoint:** A section design can start from a Codex-produced brief with no master-Claude file reads, and the brief cannot satisfy any shipping obligation.

---

## 13. Effort-Ratio Meter (1:4 Claude:Codex)

Make the cost target measurable instead of aspirational.

- [ ] Record per-run Claude vs Codex step counts (and tokens where the backend exposes them) as ledger or run-log telemetry tagged by backend.
- [ ] Add a `report` view that prints the realized Claude:Codex ratio for a run or a date range.
- [ ] Surface the ratio in the overnight report so drift back to Claude-heavy is visible.
- [ ] Telemetry only -- the meter never gates per-section shipping.
- [ ] Define the ratification protocol: what evidence lets Claude accept a Codex finding/design without file-line re-derivation versus what must still be verified; make the realized ratio a §11 pilot-graduation criterion.
- [ ] Commit: "ai-workflow: add effort-ratio meter"

**Test checkpoint:** A completed run reports a Claude:Codex effort ratio, and the value moves toward 1:4 as delegation increases.

---

## 14. Retire Separate-Driver Direction Artifacts

Unwind the co-equal / separate-namespace work built before the 2026-06-30 re-scope, keeping only what the master/delegate/failover model uses.

- [ ] Audit the prior separate-driver artifacts: `codex-sequencer-supervisor.sh`, the `.codex/overnight/` tree, the `--driver codex` co-equal launch path, and docs framing Codex as a co-equal/Codex-exclusive driver.
- [ ] Keep what the executor/failover model reuses (the bounded per-step supervisor, arming path, nested-Codex guard); remove or rewire what only served the separate-namespace co-equal model.
- [ ] Redirect Codex overnight reports/metrics into the shared overnight namespace tagged by backend (pairs with §10 consolidation).
- [ ] Update `docs/infrastructure/ai-driver-interchangeability.md` so the operations guide describes master/delegate/failover, not co-equal Codex-exclusive development.
- [ ] Remove now-obsolete `overnight_launch_driver` assertions tied to the separate `.codex/overnight` path and repoint them at the shared namespace.
- [ ] Add a lint/test that fails on remaining co-equal / Codex-exclusive / `.codex/overnight` instructions in docs outside explicit historical notes.
- [ ] Commit: "codex: retire separate-driver artifacts"

**Test checkpoint:** No code path or doc still presents Codex as a co-equal/Codex-exclusive driver, and `bash scripts/test-tooling.sh` passes against the consolidated namespace.

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11                    | 🐧 Linux                    | 🚀 Impossible OS       |
|----|----------------------------|-----------------------------|-----------------------------|-------------------------|
| 💎 | Mutator lease              | ⚠️ Human/process locks      | ⚠️ Git hooks/scripts        | ⬜ Planned -- §2        |
| 💎 | Branch/status policy gates | ✅ Azure/GitHub policies    | ✅ CI/status checks         | ⬜ Planned -- §6        |
| 💎 | CI concurrency groups      | ✅ Pipeline concurrency     | ✅ Workflow concurrency     | ⬜ Planned -- §2/§9     |
| 💎 | Durable evidence ledger    | ⚠️ CI/log fragments         | ⚠️ Build artifacts + CI     | ⬜ Planned -- §3        |
| 💎 | Provenance-bound evidence  | ✅ Artifact attestations    | ⚠️ Patch tags + CI          | ⬜ Planned -- §3/§6     |
| 💎 | Deterministic stamps       | ❌ Not a native OS concern  | ❌ Not a native OS concern  | ⬜ Planned -- §5        |
| ⭐ | Codex delegate/failover    | ❌ Not a Windows dev model  | ⚠️ Scriptable agents vary   | ⬜ Planned -- §8-9      |
| ⭐ | Failover continuation      | ⚠️ Manual operator restart  | ⚠️ CI retry/resume varies   | ⬜ Planned -- §9        |
| ⭐ | Reviewer/executor separation | ⚠️ Process convention     | ⚠️ Review policy convention | ⬜ Planned -- §7        |

Impossible OS treats AI workflow state as build-time infrastructure, not product behavior. The comparison exists to keep the host workflow explicit: the OS target gains a reproducible contributor pipeline with branch-policy, concurrency, and provenance-grade safeguards, not runtime AI features.

---

## Unit Tests

Host-side AI workflow tests live in `scripts/test-tooling.sh` because this TODO covers repository tooling, hooks, and driver orchestration rather than kernel/runtime code.

- [ ] Add or keep `ai_workflow_lease` coverage for acquire/release, stale leases, same-section conflicts, and concurrent acquire races.
- [ ] Add or keep `ai_workflow_evidence` coverage for blob binding, stale HEAD, legacy import, role independence, forged review rejection, and build/test final-marker provenance.
- [ ] Add or keep `ai_workflow_obligations` coverage for no-double-work behavior across driver modes.
- [ ] Add or keep `ai_workflow_stamp` coverage for generated stamps, file-level lifecycle preamble stamps, missing evidence failures, and stamp-only commits.
- [ ] Add or keep `ai_workflow_gates` coverage for shared verdicts across Claude hook, git hook, direct CLI paths, stale completed-lease rejection, and current provenance checks.
- [ ] Add or keep `codex_driver_dry_run` coverage proving dry-run cannot mutate files or satisfy shipping evidence and does capture bounded doctrine/TODO/code context.
- [/] Add or keep `overnight_launch_driver` coverage proving Codex overnight launch has no prompts, full-access sandbox, backend-tagged shared-namespace reports, and bounded-supervisor governance invariants.
- [ ] Add or keep `codex_driver_live_fixture` coverage using a temporary fixture repo or low-risk test TODO section, including release on failure and complete only after a gated commit.
- [ ] Add `codex_failover_resume` coverage proving Codex resumes a leased section from the ledger at the last completed checkpoint and reverts in-flight partial edits.
- [ ] Add `codex_tiered_hold` coverage proving Codex holds kernel/boot commits for Claude while shipping docs/host-tooling sections.
- [ ] Add `ai_workflow_effort_meter` coverage proving per-run Claude:Codex ratio reporting.
- [ ] Commit: "test: cover AI driver interchangeability"

**Test checkpoint:** `bash scripts/test-tooling.sh` reports the AI workflow group and fails on lease, evidence, obligation, stamp, gate, or Codex-driver regressions.

---

## Verification

- [ ] `python3 scripts/todo-hygiene.py todo/00-infrastructure/TODO-10-ai-driver-interchangeability.md`
- [ ] `bash scripts/todo-graph/build-and-validate.sh --keep-cache`
- [ ] `python3 .claude/hooks/sequencer_triage.py --classify todo/00-infrastructure/TODO-10-ai-driver-interchangeability.md`
- [ ] `python3 scripts/overnight/tests/test_launch_driver_mode.py`
- [ ] `bash scripts/test-tooling.sh`
- [ ] `AI_WORKFLOW_CODEX_DRIVER=1 bash scripts/codex-driver.sh --dry-run --todo todo/00-infrastructure/TODO-10-ai-driver-interchangeability.md --section 8`
- [ ] Run the live-driver pilot only after §9 and §11 fixture coverage pass.
- [ ] Commit: "ai-workflow: validate driver interchangeability TODO"

> **Test runner:** 2026-06-29 | host-side Python/Bash tooling; no kernel, user-mode, or desktop suite
> - `python3 scripts/overnight/tests/test_launch_driver_mode.py` -> PASS.
> - `bash scripts/test-tooling.sh` -> 426/426 PASS.
> - `bash scripts/todo-graph/build-and-validate.sh --keep-cache` -> 8/8 PASS.
