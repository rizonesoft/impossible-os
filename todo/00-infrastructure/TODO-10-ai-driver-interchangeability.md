---
schema_version: 1
id: ai-driver-interchangeability
domain: 00-infrastructure
status: draft
title: "TODO-10 -- AI Driver Interchangeability"
file_patterns:
  - "CLAUDE.md"
  - "AGENTS.md"
  - "docs/infrastructure/ai-system.md"
  - "docs/infrastructure/mcp-usage.md"
  - ".claude/settings.json"
  - ".claude/hooks/**"
  - ".claude/state/README.md"
  - ".githooks/**"
  - ".gitignore"
  - "scripts/ai-workflow/**"
  - "scripts/codex-*.sh"
  - "scripts/overnight/**"
  - "scripts/test-tooling.sh"
---

# TODO-10 -- AI Driver Interchangeability

> **Validated:** 2026-06-29 | validate-todo-file clean (structure / IO table / XREF / test wiring)

> **Gap-audited:** 2026-06-30 | ai-workflow evidence manual | gap-audit + codex-gap-audit; 6 findings filed (lease race, forged/stale evidence, stale completed leases, lifecycle stamps, live lease lifecycle, dry-run context)

> **Goal:** Make the Impossible OS development workflow run with either Claude Code or Codex as the active development driver while Codex remains the required reviewer. The implementation must preserve the existing flow, section stamps, review gates, and no-double-work behavior by moving mutable workflow evidence into repo-owned scripts and deterministic state instead of model-specific chat memory.

> [!IMPORTANT]
> **Current state:** Current doctrine remains in force until this TODO ships. Today [`CLAUDE.md`](../../CLAUDE.md) and [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) define Claude Code as the master/orchestrator and Codex as the sole external reviewer. Codex-only development is therefore not enabled by current policy. This TODO is the roadmap for changing the implementation safely first, then updating doctrine only after the shared workflow protocol, gates, and regression tests prove that a non-Claude driver cannot skip review, stamp incorrectly, or duplicate completed work.

## Model-Roles Target

Codex is always the reviewer. The driver is the interchangeable part.

| Mode                        | Active driver                       | Required reviewer slots                    | Shipping rule                                                          |
| ---                         | ---                                 | ---                                        | ---                                                                    |
| Claude development          | Claude Code                         | Codex design/adversarial/consistency/perf | Current flow, backed by shared evidence.                               |
| Codex-exclusive development | Codex driver session                | Separate fresh-context Codex reviewers     | Allowed only when reviewer evidence is independent from the driver run. |
| Mixed development           | Claude Code or Codex, one at a time | Codex reviewer sessions                    | Handoffs use the driver lease and evidence ledger, not chat summaries.  |

Codex-exclusive means two roles, not one self-certifying session:

```text
codex-driver               -> explores, edits, builds, tests, prepares commit
codex-reviewer-design      -> fresh-context design review
codex-reviewer-adversary   -> fresh-context adversarial review
codex-reviewer-consistency -> fresh-context consistency audit
codex-reviewer-perf        -> fresh-context performance review
```

## Inputs

- [`CLAUDE.md`](../../CLAUDE.md) -- current doctrine, model roles, mandatory skill triggers, Codex invocation policy, commit policy
- [`AGENTS.md`](../../AGENTS.md) -- cross-tool pointer and current reviewer-only boundary for non-Claude tools
- [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) -- authority hierarchy, external-reviewer contract, hook routing matrix
- [`todo/00-infrastructure/TODO-02-ai-development-system.md`](TODO-02-ai-development-system.md) -- canonical AI-system ownership roadmap
- [`todo/00-infrastructure/TODO-08-automation-hardening.md`](TODO-08-automation-hardening.md) -- existing hook/evidence/stamp hardening work
- [`.claude/state/README.md`](../../.claude/state/README.md) -- current runtime state schemas: skill progress, Codex review history, review stamps
- [`.claude/hooks/`](../../.claude/hooks/) -- current Claude-harness enforcement hooks
- [`.githooks/`](../../.githooks/) -- git-time enforcement layer that must protect non-Claude drivers
- [`scripts/codex-dispatch.sh`](../../scripts/codex-dispatch.sh) -- current safe Codex review dispatch wrapper
- [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh) -- tooling regression suite for hook and AI-system behavior
- [`scripts/todo-graph/`](../../scripts/todo-graph/) -- TODO cache, validation, backlink, and ready/blocked graph oracle
- -> XREF: [`00-infrastructure/TODO-06 TODO Metadata Layer`](TODO-06-todo-metadata-layer.md) -- stable TODO IDs and graph cache used by the obligation resolver
- -> XREF: [`00-infrastructure/TODO-07 LSP to MCP Bridge`](TODO-07-lsp-mcp-bridge.md) -- code-intelligence parity for Claude and Codex drivers

## Outcome

- A repo-owned AI workflow protocol exists under `scripts/ai-workflow/`; it is callable from Claude hooks, Codex driver scripts, git hooks, and overnight automation.
- Exactly one active mutating driver holds a lease for a TODO section or review target at a time.
- Review/build/test/stamp evidence is stored in a tool-neutral ledger keyed by TODO target, review kind, git HEAD, source blob SHAs, run ID, and role.
- A deterministic obligation resolver reports what is already satisfied and what is still missing before any model does work.
- A deterministic stamp writer adds `Verified:` / `Quality reviewed:` / `Deferred:` / `Accepted:` stamps only from ledger-backed evidence.
- Commit gates read the shared evidence ledger instead of Claude-only chat or hook state.
- Codex remains the mandatory reviewer in every mode; Codex-exclusive development uses fresh reviewer runs distinct from the Codex driver run.
- No parallel skill tree is introduced. Shared mechanics live in scripts, docs, hooks, and derived runtime state, not `.codex/` or another model-specific instruction directory.
- Codex overnight runs write reports/metrics under `.codex/overnight/`, while governance state remains shared through the existing sequencer cursor, driver lease, evidence ledger, stamp writer, and commit gates.
- Shared gates prove that concurrent driver races, forged evidence, stale completed leases, and stale provenance cannot satisfy shipping obligations.
- `stamp.py` can generate both section-local shipping stamps and file-level lifecycle stamps consumed by the overnight sequencer.
- Doctrine updates land only after the implementation proves the shared protocol can enforce the same safety properties as the current Claude-first flow.

## Implementation Order

| ⭐  | Order | Deliverable                                                           | Depends On    | Status |
| --- | :---: | --------------------------------------------------------------------- | ------------- | :----: |
| 💎  |   1   | Current-state inventory of Claude-only assumptions and gate inputs     | --            |  [x]   |
| 💎  |   2   | Driver lease schema and active-mutator lock                            | §1            |  [ ]   |
| 💎  |   3   | Tool-neutral evidence ledger and importer for existing state           | §1            |  [x]   |
| 💎  |   4   | Obligation resolver to avoid duplicate work                            | §3            |  [x]   |
| 💎  |   5   | Deterministic stamp writer                                             | §3-4          |  [ ]   |
| 💎  |   6   | Shared gate library used by Claude hooks, git hooks, and Codex driver  | §3-5          |  [ ]   |
| ⭐  |   7   | Codex reviewer-role normalization and fresh-context independence checks | §3-6          |  [ ]   |
| ⭐  |   8   | Codex driver dry-run adapter                                           | §2, §4, §6-7 |  [/]   |
| ⭐  |   9   | Codex driver live adapter                                              | §8            |  [ ]   |
| 💎  |   10  | Migration, docs, doctrine, and rollout toggles                         | §1-9          |  [ ]   |
| 💎  |   11  | Regression suite and pilot section                                     | §1-10         |  [ ]   |

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
> - Adversarial findings were classified Fix; inventory now covers Claude state files, live gates, stateless blockers, dispatch surfaces, sequencer/FIXPOINT control, telemetry, stamps, and review kinds.
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
- [x] A driver cannot acquire a lease if another live lease exists for the same TODO section unless the existing lease is expired or explicitly force-released with a reason.
- [ ] Serialize `lease.py` active-lease and lease-history mutations with a repo-local lock (`fcntl.flock` or an `O_EXCL` lockfile) so two concurrent drivers cannot both observe an empty lease and race through `acquire` (`scripts/ai-workflow/lease.py:53` reads, checks, then writes at `:94` today).
- [ ] Claude hooks consult the lease before first implementation edit when a TODO-section flow is active.
- [x] Codex driver adapter acquires the same lease before any edit, build, test, or commit attempt.
- [/] Git commit gate refuses a section-ship commit when no matching active lease or completed lease record exists for the staged TODO target. The shared staged gate enforces this under `AI_WORKFLOW_ENFORCE_SHARED_GATES=1`; default enforcement waits for Claude lease acquisition.
- [ ] Add a concurrent-acquire fixture that launches two same-section drivers and asserts exactly one winner plus one durable conflict event.
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

Codex is always the reviewer, including Codex-exclusive development.

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

**Test checkpoint:** Codex-exclusive mode can satisfy review obligations only with distinct reviewer runs. A Codex driver cannot self-certify its own implementation.

---

## 8. Codex Driver Dry-Run Adapter

Introduce Codex as a driver without allowing it to mutate code at first.

- [x] Add `scripts/codex-driver.sh --dry-run`.
- [/] Dry-run adapter reads `AGENTS.md`, `CLAUDE.md`, the target TODO, the obligation resolver output, and relevant code context. Current dry-run output lists the doctrine/TODO paths and prints obligations, but bounded content capture remains open (`scripts/codex-driver.sh:74` writes the plan and `:84` starts the file-name list).
- [ ] Capture bounded dry-run context with source hashes or snippets for `AGENTS.md`, `CLAUDE.md`, the target TODO, the obligation JSON, and relevant code files so the advisory plan proves what it actually inspected.
- [x] Dry-run adapter may propose an implementation plan and list deterministic commands, but must not edit, build, test, stamp, or commit.
- [x] Dry-run output is written to `.ai-workflow/runs/<run-id>/plan.md`.
- [x] Claude-mode workflow can consume the dry-run plan as advisory input without treating it as evidence.
- [x] Add a regression that verifies dry-run cannot produce ledger events that satisfy shipping obligations.
- [/] Run one pilot dry-run on a small docs-only infrastructure section and one kernel section, then record gaps.
- [x] Commit: "codex: add dry-run driver adapter"

**Test checkpoint:** Codex can understand the workflow and produce a useful plan, but no gate accepts that plan as proof of completed work.

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

## 9. Codex Driver Live Adapter

Enable Codex to be the active mutating driver under the shared gates.

- [x] Add `scripts/codex-driver.sh --live` behind an explicit environment gate such as `AI_WORKFLOW_CODEX_DRIVER=1`.
- [x] Live adapter acquires a driver lease before any mutation.
- [x] Live adapter runs the obligation resolver before each phase and after each fix.
- [/] Live adapter may edit files only while the lease is active and the next obligation allows mutation.
- [x] Overnight launcher accepts `--driver codex` and runs Codex through a bounded supervisor that invokes `codex --ask-for-approval never exec --json --sandbox danger-full-access` per phase/section step.
- [x] Codex overnight reports and metrics are isolated under `.codex/overnight/`, with `latest.log` for monitoring.
- [x] Codex arming uses the same `arm-sequencer.sh` / `overnight-arm.sh` path, same `sequencer-armed` marker, same `OVERNIGHT_SEQUENCER_RUN=1` discriminator, and same shared gate env.
- [x] Codex overnight launch is supervised by a repo-owned bounded phase loop instead of one unlimited `codex exec`; the supervisor owns phase transitions, section-progress checks, per-step timeouts, and no-progress blocking under `.codex/overnight/`.
- [ ] Live adapter dispatches Codex reviewer roles through the same `scripts/codex-dispatch.sh` wrapper and records reviewer evidence separately from driver evidence.
- [ ] Live adapter runs build/test/smoke commands through the same deterministic scripts as Claude mode.
- [ ] Live adapter records build/test/smoke/todo-graph evidence through `scripts/ai-workflow/evidence.py` with command, exit code, log path, final marker, HEAD, and source blobs before stamp or commit gates consume it.
- [/] Live adapter uses `stamp.py`, never freehand stamp text.
- [x] Live adapter attempts commits only through the same shared gate path as Claude mode.
- [/] Live adapter manages lease lifecycle explicitly: release and complete checkpoints exist; renew-during-long-phase and complete-only-after-successful-commit enforcement remain open.
- [/] Add bounded fix-loop behavior matching existing section workflows: `codex-sequencer-supervisor.sh` now limits one Codex invocation to one phase/section, blocks repeated no-progress loops, and Codex re-arm clears stale supervisor block/progress runtime state; section-local defer/fix-loop parity still needs live-pilot validation.
- [ ] Pilot on one docs-only TODO section, then one host-tooling section, before any kernel/boot implementation.
- [ ] Commit: "codex: add live driver adapter"

**Test checkpoint:** Codex live mode can ship a low-risk section through the same gates without Claude Code acting as mutator.

---

## 10. Migration, Docs, Doctrine, and Rollout Toggles

Only update doctrine after the shared protocol works.

- [x] Add `docs/infrastructure/ai-driver-interchangeability.md` as the human-readable design and operations guide.
- [ ] Update [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) to distinguish workflow authority from driver backend only after §1-9 pass.
- [ ] Update [`CLAUDE.md`](../../CLAUDE.md) Model Roles and Skills sections only after Codex live pilot succeeds.
- [ ] Update [`AGENTS.md`](../../AGENTS.md) so non-Claude tools understand when they are reviewer-only versus explicitly leased as active driver.
- [x] Preserve the autonomous-agent boundary: no cloud-agent PRs, no autonomous GitHub PR authoring, no unattended non-repo-guarded driver.
- [x] Preserve the zero AI-attribution trailer policy.
- [x] Document supported modes and required environment toggles.
- [x] Document Codex overnight reports, monitor command, approval policy, and sandbox override.
- [x] Add rollback instructions to return to Claude-driver-only mode by disabling Codex live driver while keeping shared evidence scripts available.
- [ ] Commit: "docs: document AI driver rollout"

**Test checkpoint:** A maintainer can enable or disable Codex driver mode without deleting evidence, breaking stamps, or changing commit policy.

---

## 11. Regression Suite and Pilot Section

Prove the interchangeable-driver workflow before declaring it available.

- [x] Add `ai_workflow_lease` tests to `scripts/test-tooling.sh`.
- [/] Add `ai_workflow_evidence` tests for blob binding, stale HEAD, legacy import, and review-role independence.
- [x] Add `ai_workflow_obligations` tests for no-double-work behavior across Claude-driver and Codex-driver modes.
- [/] Add `ai_workflow_stamp` tests for generated stamps, missing evidence failures, and stamp-only commits.
- [/] Add `ai_workflow_gates` tests proving Claude hook mode, git-hook mode, and direct CLI mode return identical verdicts.
- [x] Add `codex_driver_dry_run` tests proving dry-run cannot mutate or satisfy evidence.
- [x] Add `overnight_launch_driver` tests proving Claude/Codex driver selection, Codex no-prompt full-access execution through the bounded supervisor smoke path, separate `.codex/overnight` reports, and report formatting.
- [/] Add `codex_driver_live_fixture` tests using a temporary fixture repo or test TODO section.
- [ ] Add a lease-race fixture that starts two concurrent same-section `lease.py acquire` calls and proves exactly one winner.
- [ ] Add forged/stale evidence fixtures: self-recorded Codex review events, legacy-only shipping evidence, stale source blobs, stale HEAD, missing build final marker, and stale todo-graph validation must not satisfy obligations.
- [ ] Add a legacy-import fixture proving `last-review-stamps.json` imports do not satisfy adversarial, consistency, or perf review obligations.
- [ ] Add a stale completed-lease fixture proving a historical `complete` action cannot authorize a later staged TODO row flip or stamp.
- [ ] Add a lifecycle-stamp fixture proving `stamp.py validated` / `gap-audited` writes only the file preamble location consumed by `sequencer_triage.py`.
- [ ] Add dry-run context-capture and live-adapter lease-failure fixtures covering the §8 and §9 regressions filed by this gap audit.
- [ ] Run one real pilot in Claude-driver mode using the new shared ledger, with Codex as reviewer.
- [ ] Run one real pilot in Codex-driver dry-run mode.
- [ ] Run one real pilot in Codex-driver live mode on a low-risk infrastructure section.
- [ ] After all pilots pass, flip this TODO's doctrine/doc-sync items to done and mark Codex-exclusive development as supported.
- [ ] Commit: "test: cover AI driver interchangeability"

**Test checkpoint:** The same TODO section can be resumed by Claude or Codex from the ledger without repeating completed reviews, losing stamp state, or bypassing gates.

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
| ⭐ | Codex as active driver     | ❌ Not a Windows dev model  | ⚠️ Scriptable agents vary   | ⬜ Planned -- §8-9      |
| ⭐ | Reviewer/driver separation | ⚠️ Process convention       | ⚠️ Review policy convention | ⬜ Planned -- §7        |

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
- [x] Add or keep `overnight_launch_driver` coverage proving Codex overnight launch has no prompts, full-access sandbox, separate logs, and bounded-supervisor governance invariants.
- [ ] Add or keep `codex_driver_live_fixture` coverage using a temporary fixture repo or low-risk test TODO section, including release on failure and complete only after a gated commit.
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
