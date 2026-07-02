---
schema_version: 1
id: ai-driver-interchangeability
domain: 00-infrastructure
status: draft
title: "TODO-10 -- AI Workflow Evidence Ledger and Deterministic Gates"
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

# TODO-10 -- AI Workflow Evidence Ledger and Deterministic Gates

> **Validated:** 2026-06-29 | validate-todo-file clean (structure / IO table / XREF / test wiring)

> **Gap-audited:** 2026-06-30 | ai-workflow evidence manual | gap-audit + codex-gap-audit; 6 findings filed (lease race, forged/stale evidence, stale completed leases, lifecycle stamps, live lease lifecycle, dry-run context)

> **Re-scoped:** 2026-07-01 | removed the Codex-as-driver direction (no delegation, failover, or co-equal driver). This TODO now builds only the tool-neutral workflow protocol -- lease + evidence ledger + obligation resolver + deterministic stamps + shared gates -- with Codex as the required external reviewer. The obsolete codex-driver artifacts are removed in §8.

> **Goal:** Move the Impossible OS development workflow's truth out of Claude chat and hook memory into repo-owned deterministic state -- an active-mutator lease, an append-only evidence ledger, an obligation resolver, a deterministic stamp writer, and a shared gate library callable from both Claude hooks and git hooks -- so any session can resume from the last completed checkpoint and gates enforce identical rules regardless of entry point. Claude Code remains the mutator/orchestrator and Codex remains the required external reviewer. There is no AI "driver" abstraction.

> [!IMPORTANT]
> **Current state:** Current doctrine remains in force until this TODO ships. [`CLAUDE.md`](../../CLAUDE.md) and [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) define Claude Code as the master/orchestrator and Codex as the sole external reviewer. This TODO does not introduce any non-Claude mutating driver. It makes workflow evidence tool-neutral and durable so it survives across sessions and is enforced by shared scripts rather than Claude-only chat or hook state. Doctrine updates land only after the protocol and shared gates pass regression.

## Roles

Claude Code is the mutator and orchestrator. Codex is the required external reviewer (design / adversarial / consistency / perf / gap-audit), dispatched fresh-context through [`scripts/codex-dispatch.sh`](../../scripts/codex-dispatch.sh).

The workflow protocol under [`scripts/ai-workflow/`](../../scripts/ai-workflow/) is tool-neutral: it stores evidence, resolves obligations, writes stamps, and gates commits from deterministic repo state instead of chat memory. The same rules apply whether a check runs from a Claude hook or a git hook, and a new session can resume mid-flow from the ledger rather than a chat summary.

## Inputs

- [`CLAUDE.md`](../../CLAUDE.md) -- current doctrine, model roles, mandatory skill triggers, Codex invocation policy, commit policy
- [`AGENTS.md`](../../AGENTS.md) -- cross-tool pointer and current reviewer-only boundary for non-Claude tools
- [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) -- authority hierarchy, external-reviewer contract, hook routing matrix
- [`docs/infrastructure/ai-driver-interchangeability.md`](../../docs/infrastructure/ai-driver-interchangeability.md) -- canonical inventory and operations guide (rewritten to the tool-neutral protocol in §8)
- [`todo/00-infrastructure/TODO-02-ai-development-system.md`](TODO-02-ai-development-system.md) -- canonical AI-system ownership roadmap
- [`todo/00-infrastructure/TODO-08-automation-hardening.md`](TODO-08-automation-hardening.md) -- existing hook/evidence/stamp hardening work
- [`.claude/state/README.md`](../../.claude/state/README.md) -- current runtime state schemas: skill progress, Codex review history, review stamps
- [`.claude/skills/**/*.md`](../../.claude/skills/) -- canonical workflow step, prompt-template, and review-kind marker obligations
- [`.claude/hooks/`](../../.claude/hooks/) -- current Claude-harness enforcement hooks
- [`.githooks/`](../../.githooks/) -- git-time enforcement layer that must share the same gates
- [`scripts/codex-dispatch.sh`](../../scripts/codex-dispatch.sh) -- safe Codex review dispatch wrapper
- [`scripts/lint.sh`](../../scripts/lint.sh) -- Check 12 prompt-escaping and Check 15 dispatch-bundling telemetry for documented Codex dispatch examples
- [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh) -- tooling regression suite for hook and AI-workflow behavior
- [`scripts/todo-graph/`](../../scripts/todo-graph/) -- TODO cache, validation, backlink, and ready/blocked graph oracle
- -> XREF: [`00-infrastructure/TODO-06 TODO Metadata Layer`](TODO-06-todo-metadata-layer.md) -- stable TODO IDs and graph cache used by the obligation resolver
- -> XREF: [`00-infrastructure/TODO-07 LSP to MCP Bridge`](TODO-07-lsp-mcp-bridge.md) -- code-intelligence parity for the shared workflow scripts

## Outcome

- A repo-owned workflow protocol under `scripts/ai-workflow/` stores evidence, resolves obligations, writes stamps, and gates commits from deterministic state, callable from Claude hooks and git hooks.
- Exactly one active mutator holds a lease per TODO section, so an interactive session and the headless overnight runner cannot race the same section or produce conflicting stamps.
- Review/build/test/stamp evidence lives in a tool-neutral append-only ledger keyed by TODO target, kind, git HEAD, source blob SHAs, run ID, and role.
- An obligation resolver reports satisfied vs missing before work starts, so a resumed session continues from the last completed checkpoint instead of replaying the workflow.
- A deterministic stamp writer adds `Verified:` / `Quality reviewed:` / `Deferred:` / `Accepted:` / `Validated:` / `Gap-audited:` stamps only from ledger-backed evidence.
- Shared gates prove that forged evidence, stale source blobs, stale completed leases, and stale provenance cannot satisfy shipping obligations.
- Codex remains the required reviewer; review evidence is bound to the reviewed source blobs and a distinct reviewer run, trusted from dispatch/receipt metadata rather than prompt text.
- The obsolete Codex-as-driver artifacts (`codex-driver.sh`, the codex overnight supervisor, the `.codex/overnight/` tree, the `--driver codex` launch path) are removed; no model-specific instruction tree is introduced.
- Doctrine updates land only after the protocol enforces the same safety properties as the current Claude-first flow.

## Implementation Order

| ⭐  | Order | Deliverable                                                           | Depends On | Status |
| --- | :---: | --------------------------------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Current-state inventory of Claude-only assumptions and gate inputs     | --         |  [x]   |
| 💎  |   2   | Active-mutator lease schema and lock                                  | §1         |  [/]   |
| 💎  |   3   | Tool-neutral evidence ledger and importer for existing state           | §1         |  [x]   |
| 💎  |   4   | Obligation resolver to avoid duplicate work                            | §3         |  [x]   |
| 💎  |   5   | Deterministic stamp writer                                             | §3-4       |  [x]   |
| 💎  |   6   | Shared gate library used by Claude hooks and git hooks                 | §3-5       |  [/]   |
| ⭐  |   7   | Review-evidence integrity and fresh-context independence checks        | §3-6       |  [/]   |
| 💎  |   8   | Retire obsolete Codex-driver artifacts                                 | §1         |  [x]   |
| 💎  |   9   | Migration, docs, and rollout toggles                                   | §1-8       |  [/]   |
| 💎  |   10  | Regression suite and pilot section                                     | §1-9       |  [ ]   |

---

## 1. Current-State Inventory of Claude-Only Assumptions and Gate Inputs

Identify every place the current system keeps workflow truth in Claude-only state (mutator, dispatcher, committer, stamp author) rather than durable repo evidence.

- [x] Inventory Claude-harness-only state readers/writers in [`.claude/hooks/`](../../.claude/hooks/): step-gate, Codex dispatch/receipt, specialist-agent, heuristic/audit, session, transcript, overnight, sequencer, and tool-telemetry state.
- [x] Inventory commit gates that currently depend on Claude tool-call history rather than repository evidence.
- [x] Inventory every stamp shape that is freehand-written by a model today: `Verified:`, `Quality reviewed:`, `Deferred:`, `Accepted:`, `Gap-audited:`, and `Validated:`.
- [x] Inventory the required review kinds for each workflow: design, adversarial-impl, adversarial, consistency, perf, test-coverage, re-adversarial, gap-audit.
- [x] Inventory Codex review dispatch surfaces in the canonical inventory doc: foreground wrapper, fallback wrappers, direct companion review/task, bare review, plugin slash commands/hooks, and parser assumptions.
- [x] Identify which state is durable evidence and which is merely session telemetry. Durable evidence graduates to the shared ledger; telemetry stays Claude-adapter-specific.
- [x] Produce `docs/infrastructure/ai-driver-interchangeability.md` with the inventory table and migration notes.
- [x] Commit: "ai-workflow: inventory Claude-only assumptions"

**Test checkpoint:** A maintainer can point at each gate/stamp and say whether it is already tool-neutral, needs a shared-script wrapper, or stays Claude-specific telemetry.

> **Test runner:** 2026-07-01 | `bash scripts/test-tooling.sh` | 459/459 PASS
>
> **Notes:**
> - Inventory source lives in [`docs/infrastructure/ai-driver-interchangeability.md`](../../docs/infrastructure/ai-driver-interchangeability.md).
> - §1 records current-state evidence only; doctrine remains unchanged until rollout sections ship.
> - Inventory covers the AGENTS.md reviewer boundary, Claude state files, live gates, stateless blockers, review dispatch surfaces, prompt-escaping/bundling lint telemetry, sequencer control, stamps, and review kinds.
> - Durable-evidence findings graduate to the shared ledger; Claude-adapter state stays telemetry.
> - Reviewed 2026-07-01 (review-todo-section, Codex 3x): fixed inventory-completeness gaps -- missing hook state-writers + `.claude/state` locks, the `Skill(codex-*)` trigger surface, the `gap-audit` receiver mismatch, the evidence-kind subset caveat, and the `common.py` row; re-adversarial skipped (docs-only fixes).
>
> **Verified:** 2026-07-01 | review re-verify | 7/7 items | build N/A (docs-only) | tooling 459/459 PASS, todo-graph 8/8
> **Quality reviewed:** 2026-07-01 | Codex 3x (adversarial, consistency, perf) | 3H+2M fixed | scope: N/A (docs-only inventory)

---

## 2. Active-Mutator Lease Schema and Lock

Prevent two sessions (for example an interactive session and the headless overnight runner) from mutating the same section or producing conflicting stamps.

- [x] Add `scripts/ai-workflow/lease.py` with `acquire`, `renew`, `release`, `status`, and `force-release --reason` subcommands.
- [x] Store lease state in a gitignored derived-state location that is not a model-specific instruction tree. Preferred shape: `.ai-workflow/active-lease.json`.
- [x] Lease key includes `todo_path`, `section`, `driver_backend`, `driver_run_id`, `head_sha`, `started_at_ns`, `expires_at_ns`, and `allowed_mutations`.
- [x] A holder cannot acquire a lease if another live lease exists for the same TODO section unless the existing lease is expired or explicitly force-released with a reason.
- [x] Serialize `lease.py` active-lease and lease-history mutations with a repo-local lock (`fcntl.flock` or `O_EXCL`) so two concurrent sessions cannot both observe an empty lease and race through `acquire`.
- [x] Claude hooks consult the lease before the first implementation edit when a TODO-section flow is active; the consult binds OWNERSHIP (session run-id) and auto-acquires when no live lease exists.
- [/] Git commit gate refuses a section-ship commit when no ACTIVE lease exists for the staged TODO target (ownership-bound via `AI_WORKFLOW_RUN_ID`). Enforced under `AI_WORKFLOW_ENFORCE_SHARED_GATES=1`; default-on is the §9 rollout toggle.
- [x] Enforce lease OWNERSHIP (not presence) in the consumers before default-on: `step5_quality_gate` + `gates.staged_commit` match the session `driver_run_id`; Claude auto-acquires a lease with that run-id.
- [x] Add a concurrent-acquire fixture that launches two same-section holders and asserts exactly one winner plus one durable conflict event.
- [x] Add `scripts/test-tooling.sh` coverage for acquire/release and same-section conflict.
- [x] Commit: "ai-workflow: add active-mutator lease"

**Test checkpoint:** Two simulated sessions cannot both edit or commit the same section. A stale lease can be recovered with an auditable reason.

> **Test runner:** 2026-07-01 | `bash scripts/test-tooling.sh` | 462/462 PASS
>
> **Notes:**
> - `lease.py` (acquire/renew/release/complete/force-release/status) serializes all state mutations under `common.workflow_lock`; state lives in gitignored `.ai-workflow/`; malformed/zero expiry counts as expired everywhere.
> - Review hardening: renew/release/complete require `--run-id` and enforce ownership + reject expired leases; `force-release --reason` is the only non-owner recovery path.
> - Consumers enforce OWNERSHIP: `step5_quality_gate` binds the session run-id + auto-acquires (session-bound); `gates.staged_commit` requires the holder identity and fails closed under enforcement.
> - Review deferrals resolved: lease ownership shipped in Phase 2; stamp-write atomicity shipped in §5. Default-on enforcement is the §9 rollout toggle.
> - Canonical doc: [docs/infrastructure/ai-driver-interchangeability.md](../../docs/infrastructure/ai-driver-interchangeability.md).
>
> **Verified:** 2026-07-01 | 7/10 items | build N/A (Python tooling) | tests 462/462 PASS (concurrent one-winner + ownership + expired-complete)
> **Quality reviewed:** 2026-07-01 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2H+1M+1L fixed, 2Critical deferred (both since resolved), 1M accepted-XREF (resolved in §5) | scope: N/A (Python workflow tooling)

---

## 3. Tool-Neutral Evidence Ledger and Importer for Existing State

Make evidence belong to the workflow, not to Claude chat history.

- [x] Add `scripts/ai-workflow/evidence.py` with `record`, `query`, `explain`, `import-legacy`, and `gc` subcommands.
- [x] Store ledger events in a structured append-only JSONL file under `.ai-workflow/evidence.jsonl`.
- [x] Event schema includes: `event_id`, `task_id`, `todo_path`, `section`, `role`, `backend`, `run_id`, `kind`, `head_sha`, `source_blobs`, `result`, `summary_path`, `created_at`, and `expires_at`.
- [x] Review evidence is keyed by review kind and source blob SHAs, not by path alone.
- [x] Build/test/smoke evidence records the command, exit code, log path, final marker, HEAD, and freshness rule.
- [x] Stamp evidence records the generated stamp text hash and the source evidence IDs consumed.
- [x] Importer reads existing `.claude/state/last-review-stamps.json`, `codex-review-history.jsonl`, and relevant TODO stamps into compatibility events.
- [x] Importer marks legacy records as `legacy_import: true` so gates can warn during transition without silently trusting incomplete evidence.
- [x] Add a small `evidence explain <todo> --section N` view for humans and tools.
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

Every session starts by asking what is missing, not by replaying the whole workflow.

- [x] Add `scripts/ai-workflow/obligations.py`.
- [x] Resolver inputs: TODO path, section number, workflow kind (`implement`, `review`, `verify`, `complete-file`), staged diff, HEAD, and evidence ledger.
- [x] Resolver outputs machine-readable JSON plus a concise text view.
- [/] Required obligations include quality preflight, design review, implementation edit, test wiring, build, unit tests, smoke on boot-path, Codex review kinds, receiving/triage, fix loop, stamp, todo-graph validation, and commit.
- [x] Satisfied obligations include the evidence IDs that prove satisfaction.
- [x] Missing obligations include the next acceptable action and the exact command/script to run when deterministic.
- [x] Resolver dedupes repeated Codex review kinds by content hash. Same review kind against unchanged blobs is not rerun unless TTL, HEAD ancestry, or policy requires it.
- [x] Resolver detects unsafe reuse: stale source blobs, stale build after source edit, or stamp generated from legacy-only evidence.
- [/] Add fixtures for current sections, stamp-only reviews, docs-only sections, boot-path smoke-required sections, and cross-session resume.
- [x] Commit: "ai-workflow: add obligation resolver"

**Test checkpoint:** Running the resolver twice after satisfying one obligation reports one fewer missing item and never asks for already-covered review work.

> **Test runner:** 2026-06-29 | `bash scripts/test-tooling.sh` | 423/423 PASS
>
> **Notes:**
> - Resolver output is intentionally tool-neutral and keyed by evidence, HEAD, staged diff, and workflow kind.
> - Remaining fixture breadth is tracked inside this section without marking the rollout complete.
>
> **Verified:** 2026-06-29 | working tree validation
> - `bash scripts/test-tooling.sh` -> 423/423 PASS.
> - `bash scripts/todo-graph/build-and-validate.sh --keep-cache` -> 8/8 PASS.

---

## 5. Deterministic Stamp Writer

> **Folded 2026-07-01, landed 2026-07-02:** the stamp writer shipped in commit 92b80346; its post-ship review and the open XREF-grammar findings were folded into the XREF-grammar unification plan (docs/superpowers/plans/2026-07-01-todo10-xref-grammar-unification-plan.md) and landed as Phase 1 (shared `xref.py` grammar, writer/hook subset property, atomic stamp+ledger critical section, linear clause scan; commits 2c23f910 through the Phase 1 triad close).

Stop relying on model-authored stamp prose for workflow truth.

- [x] Add `scripts/ai-workflow/stamp.py`.
- [x] Supported stamp commands: `verified`, `quality-reviewed`, `deferred`, `accepted`, `validated`, `gap-audited`.
- [x] `verified` and `quality-reviewed` stamps require ledger evidence for the relevant build/test/review obligations.
- [x] Stamp text is generated from evidence IDs and short human summaries; callers may provide a summary field, but not the proof fields.
- [x] Stamp writer refuses to add a stamp when required evidence is missing, stale, legacy-only where fresh evidence is required, or produced by a disallowed role combination.
- [x] Section-local stamps land in the canonical bottom block: `_insert_stamp` joins an existing Verified/Accepted/Deferred/Quality group, else places after Notes/Test-runner with the `>` separator, never abutting the heading.
- [x] File-level mode: `validated`/`gap-audited` auto-route to `_insert_preamble_stamp` (replace-in-place; hard-fail no H1); `--section` optional; gated on `todo-graph-validate`/`gap-audit` evidence with an `--allow-missing` escape.
- [x] `accepted`/`deferred` stamps accept only the graph-consumable XREF clause (arrow + adjacent section + item paren) via the shared grammar module `scripts/ai-workflow/xref.py` the git hook also uses.
- [x] Stamp writer records a `stamp.generated` ledger event with the exact text hash.
- [x] Make stamp writing atomic vs ledger failure: preflight the workflow lock before `path.write_text` so a lock failure / fail-closed no-fcntl cannot leave a TODO stamp with no backing `stamp.generated` evidence.
- [x] Add `--dry-run` and `--explain-missing` modes.
- [x] Regression fixtures in `test-tooling.sh`: file-level preamble placement + sequencer recognition, file-scoped evidence, replace-in-place re-stamp, no-H1 fail, bare/concrete XREF, and section-local bottom placement with multiline Notes.
- [x] Commit: "ai-workflow: add deterministic stamp writer"

**Test checkpoint:** A caller cannot create a successful section stamp unless the ledger proves the required workflow happened.

> **Test runner:** 2026-07-02 | `bash scripts/test-tooling.sh` | 504/504 PASS
>
> **Notes:**
> - `stamp.py` writes file-level lifecycle stamps (`_insert_preamble_stamp`) and canonical bottom-block section stamps (`_insert_stamp`); `accepted`/`deferred` accept ONLY the graph-consumable XREF clause (arrow + adjacent section + item paren) via `scripts/ai-workflow/xref.py`.
> - The `accepted_xref_block.py` git hook shares the same grammar module and stays the authoritative backstop; file-level evidence is file-scoped (`section=file`).
> - Phase 1 hardening: writer/hook subset property (fuzz-proven), one workflow_lock critical section with atomic rollback, linear clause scan (hook DoS fixed).
> - Downstream: `sequencer_triage.file_lifecycle` recognizes generated `Validated:`/`Gap-audited:` stamps so the sequencer skips mature files.
> - Scope boundary: §6 owns shared-gate consumption of stamp evidence; §7 owns reviewer-evidence integrity.
>
> **Verified:** 2026-07-02 | plan-driven Phase 1 close | 13/13 items | build OK | tooling 504/504 PASS, xref corpus 1976 lines + 20k fuzz 0 mismatches
> **Quality reviewed:** 2026-07-02 | Codex 10x (adversarial+consistency+perf triad + 7 confirmation rounds to convergence) | 6H+5M+2L fixed | scope: N/A (Python workflow tooling)

---

## 6. Shared Gate Library Used By Claude Hooks and Git Hooks

Make the enforcement layer reusable outside the Claude harness.

- [x] Add `scripts/ai-workflow/gates.py` with callable checks for lease, obligations, review evidence, build freshness, smoke freshness, stamp validity, and commit eligibility.
- [/] Refactor Claude hooks to call `gates.py` where feasible; the step5 lease consult now shares gates' ownership semantics, remaining hook routing lands with the §9 default-on rollout.
- [/] Refactor `.githooks/pre-commit` to call `gates.py staged-commit` (ownership-bound via `AI_WORKFLOW_RUN_ID`) behind `AI_WORKFLOW_ENFORCE_SHARED_GATES=1`; default-on is the §9 rollout toggle.
- [/] Reject forged/stale shipping evidence: reviews need distinct run id + current blobs; build needs exit code + log + final marker; stamps need current source; legacy never ships. Residual: graph-validate blob currency (§7).
- [x] Make `evidence.py import-legacy` treat `last-review-stamps.json` as dispatch telemetry; obligations reject `legacy_import` or empty-source reviewer evidence for shipping.
- [x] Remove stale completed-lease reuse from `staged-commit`: only an ACTIVE unexpired lease (ownership-bound when identity supplied) authorizes a staged section flip.
- [x] Add a preflight command callers run before editing, before stamping, and before committing.
- [x] Preserve existing opt-out shapes where policy already permits them, but record all skips in a shared skip ledger.
- [ ] Keep Claude-specific reminders as UX sugar only. The blocking decision must be made by shared scripts or git hooks.
- [/] Add tests proving the same staged diff receives the same verdict from Claude hook mode, git-hook mode, and direct CLI mode. The three modes now share the same `gates.py commit` verdict; full staged-diff parity remains behind the rollout flag.
- [ ] Commit: "ai-workflow: share gates across hooks"

**Test checkpoint:** A direct shell commit attempt and a Claude `git commit` attempt are blocked or allowed for the same reason.

---

## 7. Review-Evidence Integrity and Fresh-Context Independence Checks

Codex is the required reviewer; its evidence must be trustworthy and independent of the mutating session.

- [x] Define canonical reviewer roles: `codex-reviewer-{design, adversarial-impl, adversarial, consistency, perf, test-coverage, gap-audit, re-adversarial}`.
- [x] The receipt hook records `role`, `review_kind`, and a trusted `review_run_id` for every dispatch; prompt-authored ids are recorded as correlation hints only.
- [x] Derive trusted `review_run_id` from receipt process metadata, not prompt text; the receipt hook always generates it and demotes prompt ids to `prompt_review_run_id_hint`.
- [x] Recognize `gap-audit` in `codex_review_completed.py` (`_REVIEW_KIND_RE`, `_record_stamp`, `CODEX_TRIGGER_SKILLS`, skill map) + tests; gap-audit receipts normalize to the `file` scope for the ledger index.
- [x] Bind review evidence to the reviewed source blobs and current HEAD so a review of stale source cannot satisfy a shipping obligation (receipt mirrors trigger blobs + HEAD; obligations reject non-current blobs).
- [x] Review evidence is warned or rejected when the reviewer prompt contains implementor self-summary language instead of evidence-first scope.
- [x] The obligation resolver treats fresh-context Codex review as required for shipping review kinds.
- [/] Reviewer-output receipt step classifying findings Fix/Reject/Accept-XREF: the receipt hook mirrors received reviews to the ledger; per-finding classification stays in receiving-code-review prose (no durable schema yet).
- [x] Preserve the no-model-flag policy: Codex model and effort remain controlled centrally by Codex config, not per dispatch.
- [x] Harden `sequencer_triage.file_lifecycle`: a manual `--allow-missing` lifecycle stamp is trusted only with matching non-legacy ledger evidence; hand-written historical stamps stay trusted.
- [x] Commit: "ai-workflow: harden review-evidence integrity" (landed as the Phase 3 commit series)

**Test checkpoint:** A review of stale source or a prompt-forged review id cannot satisfy a shipping review obligation; a fresh reviewer dispatch can.

> **Test runner:** 2026-07-02 | `bash scripts/test-tooling.sh` | 532/532 PASS
>
> **Notes:**
> - Receipt hook: gap-audit recognized + file-scope normalized; run-ids receipt-generated (prompt ids are hints); background dispatches (argv-detected) mirror as telemetry, never stamp proof.
> - Reviewer ledger records are receipt-only (`evidence.py record` refuses reviewer roles without the fixture escape); `codex e`/`exec` excluded from review classification.
> - File-level lifecycle evidence is scope-bound (`todo#file`); manual `--allow-missing` stamps are distrusted without matching ledger evidence.
> - Canonical doc: [docs/infrastructure/ai-driver-interchangeability.md](../../docs/infrastructure/ai-driver-interchangeability.md).
> - Scope boundary: durable per-finding classification schema stays with the receiving-code-review workflow ([/] item above).
>
> **Verified:** 2026-07-02 | plan-driven Phase 3 close | 10/11 items, 1 honest [/] | build N/A (Python hooks/tooling) | tooling 532/532 PASS
> **Quality reviewed:** 2026-07-02 | Codex 9x (adversarial+consistency+perf triad + 6 confirmation rounds to convergence) | 5H+4M fixed | scope: N/A (Python workflow tooling)

---

## 8. Retire Obsolete Codex-Driver Artifacts

Remove the abandoned Codex-as-driver implementation built before the 2026-07-01 re-scope. None of it is part of the tool-neutral protocol.

- [x] Remove `scripts/codex-driver.sh` (the abandoned dry-run/live driver adapter).
- [x] Remove `scripts/overnight/codex-sequencer-supervisor.sh` and the `--driver codex` branch in the overnight launcher / arming path.
- [x] Remove the `.codex/overnight/` report/metric tree and any `.codex/` runtime-output references.
- [x] Rewrite `docs/infrastructure/ai-driver-interchangeability.md` so it documents the tool-neutral workflow protocol only (no Codex mutating driver, no delegation/failover, no `.codex/overnight`).
- [x] Remove `overnight_launch_driver` tests and any `scripts/test-tooling.sh` assertions tied to the Codex-driver / `.codex/overnight` paths; keep the shared `ai_workflow_*` tests.
- [x] Grep the repo for `codex driver`, `--driver codex`, `.codex/overnight`, `Codex-exclusive`, and `co-equal` and remove stale references outside explicit historical notes.
- [x] Commit: "ai-workflow: retire obsolete codex-driver artifacts"

**Test checkpoint:** No code path, doc, or test references a Codex mutating driver or `.codex/overnight`, and `bash scripts/test-tooling.sh` passes.

> **Test runner:** 2026-07-01 | `bash scripts/test-tooling.sh` | 442/442 PASS
>
> **Notes:**
> - Deleted `codex-driver.sh`, `codex-sequencer-supervisor.sh`, `test_launch_driver_mode.py`, and the obsolete master/failover design spec.
> - Made the overnight launcher/arm/sequencer/monitor Claude-only; removed the `--driver codex` plumbing, codex systemd dropins, and `.codex/overnight` report base.
> - Removed the `codex_driver_*` / `overnight_launch_driver` test blocks and swapped lease-test holder labels to claude; repo sweep clean of driver refs outside `todo/`.
> - Removal-only section: verified by the tooling suite, no Codex quality dispatch.
>
> **Verified:** 2026-07-01 | commit `c63c1471` | 7/7 items | tests 442/442 PASS
> **Quality reviewed:** 2026-07-01 | light-close, no Codex quality dispatch (removal-only) | 0H+0M+0L | scope: N/A (deletion verified by test-tooling 442/442)

---

## 9. Migration, Docs, and Rollout Toggles

Only update doctrine after the shared protocol works.

- [x] Add `docs/infrastructure/ai-driver-interchangeability.md` as the human-readable design and operations guide (rewritten to the tool-neutral protocol in §8).
- [ ] Update [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) to describe the tool-neutral workflow protocol (workflow truth in the ledger; Claude mutator, Codex reviewer) only after §1-8 pass.
- [ ] Update [`CLAUDE.md`](../../CLAUDE.md) Model Roles and Skills sections to reference the shared workflow protocol; no driver-backend language.
- [ ] Update [`AGENTS.md`](../../AGENTS.md) reviewer-only boundary so non-Claude tools remain reviewer/reader roles, not mutating drivers.
- [x] Preserve the autonomous-agent boundary: no cloud-agent PRs, no autonomous GitHub PR authoring, no unattended non-repo-guarded mutator.
- [x] Preserve the zero AI-attribution trailer policy.
- [x] Document the protocol scripts and the `AI_WORKFLOW_ENFORCE_SHARED_GATES` toggle.
- [/] Document the protocol, monitor commands, and shared-gate toggle, updated for the tool-neutral model.
- [x] Add rollback instructions: leave the shared scripts in place and disable enforcement to return to Claude-hook-only gating without deleting evidence.
- [ ] Commit: "docs: document AI workflow protocol rollout"

**Test checkpoint:** A maintainer can enable or disable shared-gate enforcement without deleting evidence, breaking stamps, or changing commit policy.

---

## 10. Regression Suite and Pilot Section

Prove the tool-neutral workflow protocol before relying on it.

- [x] Add `ai_workflow_lease` tests to `scripts/test-tooling.sh`.
- [/] Add `ai_workflow_evidence` tests for blob binding, stale HEAD, legacy import, and forged-review rejection.
- [x] Add `ai_workflow_obligations` tests for no-double-work behavior and cross-session resume.
- [/] Add `ai_workflow_stamp` tests for generated stamps, missing evidence failures, and stamp-only commits.
- [/] Add `ai_workflow_gates` tests proving Claude hook mode, git-hook mode, and direct CLI mode return identical verdicts.
- [ ] Add a lease-race fixture that starts two concurrent same-section `lease.py acquire` calls and proves exactly one winner.
- [ ] Add forged/stale evidence fixtures: legacy-only shipping evidence, stale source blobs, stale HEAD, missing build final marker, and stale todo-graph validation must not satisfy obligations.
- [ ] Add a legacy-import fixture proving `last-review-stamps.json` imports do not satisfy adversarial, consistency, or perf review obligations.
- [ ] Add a stale completed-lease fixture proving a historical `complete` action cannot authorize a later staged TODO row flip or stamp.
- [ ] Add a lifecycle-stamp fixture proving `stamp.py validated` / `gap-audited` writes only the file preamble location consumed by `sequencer_triage.py`.
- [ ] Run one real pilot: implement one low-risk infrastructure section using the shared ledger and gates with Codex as reviewer, then interrupt and resume it from the ledger.
- [ ] After the pilot passes, flip this TODO's doctrine/doc-sync items to done.
- [ ] Commit: "test: cover AI workflow protocol"

**Test checkpoint:** The same TODO section can be resumed from the ledger without repeating completed reviews, losing stamp state, or bypassing gates.

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11                    | 🐧 Linux                    | 🚀 Impossible OS       |
|----|----------------------------|-----------------------------|-----------------------------|-------------------------|
| 💎 | Mutator lease              | ⚠️ Human/process locks      | ⚠️ Git hooks/scripts        | ⚠️ flock lease; enforce deferred |
| 💎 | Branch/status policy gates | ✅ Azure/GitHub policies    | ✅ CI/status checks         | ⬜ Planned -- §6        |
| 💎 | Durable evidence ledger    | ⚠️ CI/log fragments         | ⚠️ Build artifacts + CI     | ⬜ Planned -- §3        |
| 💎 | Provenance-bound evidence  | ✅ Artifact attestations    | ⚠️ Patch tags + CI          | ⬜ Planned -- §3/§6     |
| 💎 | Cross-session resume       | ⚠️ Manual / IDE state       | ⚠️ Branch + CI re-run       | ⬜ Planned -- §4        |
| 💎 | Deterministic stamps       | ❌ Not a native OS concern  | ❌ Not a native OS concern  | ✅ Done -- §5           |
| ⭐ | Reviewer-evidence integrity | ⚠️ Process convention       | ⚠️ Review policy convention | ⬜ Planned -- §7        |

Impossible OS treats AI workflow state as build-time infrastructure, not product behavior. The comparison exists to keep the host workflow explicit: the OS target gains a reproducible contributor pipeline with branch-policy, concurrency, provenance-grade evidence, and cross-session resume, not runtime AI features.

---

## Unit Tests

Host-side AI workflow tests live in `scripts/test-tooling.sh` because this TODO covers repository tooling, hooks, and the workflow protocol rather than kernel/runtime code.

- [ ] Add or keep `ai_workflow_lease` coverage for acquire/release, stale leases, same-section conflicts, and concurrent acquire races.
- [ ] Add or keep `ai_workflow_evidence` coverage for blob binding, stale HEAD, legacy import, forged review rejection, and build/test final-marker provenance.
- [ ] Add or keep `ai_workflow_obligations` coverage for no-double-work behavior and cross-session resume.
- [x] Add or keep `ai_workflow_stamp` coverage for generated stamps, file-level lifecycle preamble stamps, missing evidence failures, and stamp-only commits.
- [ ] Add or keep `ai_workflow_gates` coverage for shared verdicts across Claude hook, git hook, and direct CLI paths, stale completed-lease rejection, and current provenance checks.
- [ ] Commit: "test: cover AI workflow protocol"

**Test checkpoint:** `bash scripts/test-tooling.sh` reports the AI workflow group and fails on lease, evidence, obligation, stamp, or gate regressions.

---

## Verification

- [ ] `python3 scripts/todo-hygiene.py todo/00-infrastructure/TODO-10-ai-driver-interchangeability.md`
- [ ] `bash scripts/todo-graph/build-and-validate.sh --keep-cache`
- [ ] `python3 .claude/hooks/sequencer_triage.py --classify todo/00-infrastructure/TODO-10-ai-driver-interchangeability.md`
- [ ] `bash scripts/test-tooling.sh`
- [ ] `python3 scripts/ai-workflow/obligations.py --todo todo/00-infrastructure/TODO-10-ai-driver-interchangeability.md --section 5 --kind implement`
- [ ] Commit: "ai-workflow: validate workflow protocol TODO"
