# AI Workflow Evidence Ledger and Deterministic Gates

This document describes the TODO-10 implementation shape for moving the
Impossible OS development workflow's truth into repo-owned deterministic state.
Claude Code is the sole mutator and orchestrator; Codex is the required external
reviewer. There is no AI "driver" abstraction.

## Roles

Claude Code is the mutator and orchestrator. Codex is the required reviewer in
every mode. The workflow protocol is tool-neutral: workflow truth lives in the
ledger and shared gates, not in chat memory, so any session can resume from the
last completed checkpoint and gates enforce the same rules from Claude hooks and
git hooks.

The important invariant is role separation: review evidence must come from a
fresh reviewer run distinct from the mutating session, bound to the reviewed
source blobs.

## Shared Protocol

The shared workflow lives under [`scripts/ai-workflow/`](../../scripts/ai-workflow/):

| Script | Purpose |
| --- | --- |
| `lease.py` | Owns the one-active-mutator lease for a TODO section. |
| `evidence.py` | Append-only evidence ledger for reviews, builds, stamps, and validations. |
| `obligations.py` | Reports which workflow obligations are satisfied or missing. |
| `stamp.py` | Generates TODO stamps from ledger evidence instead of model prose. |
| `gates.py` | Shared gate checks for Claude hooks and git hooks. |
| `common.py` | Shared helpers + canonical constants: `REVIEW_KINDS` (the 8 review kinds) and `REQUIRED_SHIP_REVIEW_KINDS` (`adversarial`/`consistency`/`perf`), imported by the scripts above. |

Generated state lives under `.ai-workflow/`, which is ignored as derived
workflow state. There is no repo-owned `.codex` tree.

## Inventory

The current Claude-first workflow has three classes of state:

| Surface | Current owner | Migration handling |
| --- | --- | --- |
| `AGENTS.md` | Cross-tool pointer and current non-Claude boundary | Current text tells non-Claude tools they are subordinate reviewer/reader roles and must not edit, commit, or make scope decisions. Tool-neutral rollout updates this only after the shared protocol is proven; the reviewer-only boundary remains canonical. |
| `.claude/state/skill-progress.json` | Claude step gate state | Stays Claude adapter state until shared gates replace the blocking decision. Not accepted as cross-session proof. |
| `.claude/state/skill-progress-skip.log` | Claude step-gate audit trail | Audit telemetry only. Not accepted as proof. |
| `.claude/state/last-review-stamps.json` | Claude Codex dispatch observer | Dispatch telemetry only. Import as warning/compatibility data, not as received-review proof for shipping gates. |
| `.claude/state/codex-review-history.jsonl` | Claude Codex review observer | Imported as received-review compatibility data only when it records a received review with review kind, TODO target, review run ID, and non-empty source binding. Unsourced or run-id-less entries import as telemetry and cannot satisfy shipping review obligations. |
| `.claude/state/last-codex-review.json` | Claude receiving-review gate | Stays Claude adapter state until hook refactor. |
| `.claude/state/last-agent-dispatch.json` | Claude specialist-agent backstop | Adapter gate state for agent-dispatch heuristics. Not accepted as shipping proof. |
| `.claude/state/heuristic-misses.jsonl` / `.claude/state/heuristic-misses.jsonl.1` | Claude heuristic WARN dataset | Telemetry for WARN-to-ERROR promotion, including the rotated log. Not accepted as proof. |
| `.claude/state/session.json` | Claude session metadata | Adapter/session telemetry. May help correlate trusted wrapper state later, but is not proof by itself. |
| `.claude/state/acknowledged-but-skipped.log` | Stop/subagent audit trail | Audit telemetry only. Not accepted as proof. |
| `.claude/state/subagent-log.jsonl` | Subagent audit trail | Audit telemetry only. Not accepted as proof. |
| `.claude/state/transcript-scan-cache.json` | Design-review cache | Adapter cache. Must be invalidated by Claude hooks, not imported as proof. |
| `.claude/state/.compaction-snapshots/` | Claude pre-compaction safety snapshots | Recovery telemetry for post-compaction hook state. Retained for diagnosis and stale-entry avoidance; not accepted as cross-session proof. |
| `.claude/state/overnight-run.json` | Interactive overnight fallback guard | Legacy adapter state retained until the phase guard fully supersedes it. |
| `.claude/state/live-gotchas.md` | Curated overnight awareness notes | Read by `runner_status.py` / session brief injection for human/model orientation. Advisory telemetry only; not shared gate evidence. |
| `.claude/state/sequencer-armed` / `.claude/state/sequencer-run.json` / `.claude/state/sequencer-fixpoint` | Overnight phase guard | Shared sequencer control state. It drives phases and FIXPOINT detection but is not build/review/stamp evidence. |
| `.claude/hooks/run_phase_guard.py` | Shared overnight phase gate | Live BLOCK gate for the repo-owned sequencer phase machine. All sessions must use this path for cursor and phase transitions; it is control state, not shipping proof. |
| `.claude/hooks/sequencer_triage.py` + `build/todo-cache.json` + TODO Verified-stamp scan | Overnight traversal and FIXPOINT oracle | Derived traversal control used by `run_phase_guard.py --next` / `fixpoint` to prevent premature completion. It guides cursor/FIXPOINT decisions but is not build/review/stamp evidence for a section. |
| `.claude/hooks/overnight_plugin_skill_block.py` | Claude Skill BLOCK gate for overnight arming | Blocks unguarded plugin overnight skills so runs arm through the repo-owned sequencer. Preserve this protection for non-Claude tools by keeping overnight launch behind `run_phase_guard.py` and shared scripts. |
| `.claude/hooks/overnight_guard.py` | Claude legacy interactive overnight guard | Interactive fallback that tracks `.claude/state/overnight-run.json`; retained as legacy adapter behavior until the sequencer path fully supersedes it. Not cross-session proof. |
| `.claude/state/tool-history.jsonl` / `.claude/state/tool-history.jsonl.1` | Claude tool telemetry and hook input | Adapter telemetry used by several Claude hooks, with one rotated file read by the commit gate. Not accepted as cross-session proof. |
| `.claude/state/codex-review-debug.jsonl` | Claude Codex receipt debug log | Optional missing-write diagnostic emitted by `codex_review_completed.py`. Debug telemetry only; never accepted as received-review proof. |
| `.claude/state/skip-log.jsonl` | Claude section-commit opt-out state transition | Audit and reset telemetry for `SKIP_REVIEW_HOOK`; rewrites `last-codex-review.json` to prevent stale received-review reuse. Import only as warning/transition context, not shipping proof. |
| Stateless policy BLOCK hooks (`unicode_dash_block.py`, `numeric_todo_shorthand.py`, `scope_gap_marker.py`, `test_side_effect_ban.py`, `bare_section_refs.py`, `citation_block.py`, `notes_bloat_check.py`, `todo_item_line_length.py`, `accepted_xref_block.py`, `codex_model_flag_block.py`) | Claude deterministic policy gates | Direct command/file-content gates with no durable proof value. Keep as Claude adapter protection and mirror through lint/git/shared scripts where non-Claude tools need the same block. |
| `.claude/hooks/design_review_required.py` | Claude design-review gate | Live STATE+BLOCK gate that uses transcript/session state to require design-review dispatch before implementation edits. Claude-adapter gate until a shared pre-implementation review obligation exists. |
| `.claude/hooks/receiving_review_required.py` | Claude review-receipt gate | Live STATE+BLOCK gate that requires explicit receiving-code-review handling after Codex output. Claude-adapter gate until shared receipt/classification evidence replaces it. |
| `.claude/hooks/section_review_required.py` | Claude section-review gate | Live STATE+BLOCK gate that requires section review workflow state before section commits. Claude-adapter gate until shared obligations and ledger evidence fully replace it. |
| `.claude/hooks/skill_step_block.py` | Claude skill-step gate | Live STATE+BLOCK gate that enforces flagship skill step ordering from `skill-progress.json` and skip state. Claude-adapter gate until shared sequenced obligations exist. |
| `.claude/hooks/section_commit_gate.py` | Claude Bash/git section gate | Live gate input aggregator for build evidence, Codex receipt, receiving-review state, re-adversarial and impl-side adversarial reviews, smoke markers, test-wiring checks, and heuristic WARN logs. Shared gates must replace the blocking decision before non-Claude commits can ship. |
| `.claude/hooks/phase1_evidence_gate.py` | Claude pre-dispatch review gate | Enforces transcript-scoped read/grep preparation before adversarial review dispatch. This remains Claude adapter gating until equivalent shared pre-review evidence exists. |
| `.claude/hooks/step5_quality_gate.py` | Claude pre-edit quality gate | Enforces quality-skill invocation before `src/` / `include/` edits in Claude implementation flows. Codex parity requires a shared or adapter-level equivalent; the hook state is not ledger proof. |
| `.claude/hooks/todo_graph_auto_rewrite.py` | Claude TODO line-number side-effect mutator | PostToolUse hook that runs `validate.py --fix-line-numbers --write` after TODO edits. Claude adapter freshness helper, not proof; non-Claude tools must run todo-graph validation/fix explicitly before stamp or commit gates consume TODO XREFs. |
| `.claude/hooks/_codex_dispatch.py` / `codex_review_completed.py` | Claude Codex dispatch classifier and receiver | Recognizes direct `codex-companion.mjs` review forms, the `scripts/codex-dispatch.sh` / `scripts/codex-bg-dispatch.sh` wrappers, and bare `codex review`; the non-review `codex e` / `codex exec` aliases are excluded. Receipt-generated trusted run ids, gap-audit file-scoping, and background-telemetry semantics shipped with the review-evidence integrity section. |
| `scripts/codex-bg-dispatch.sh` | Claude background Codex dispatch fallback | Fallback wrapper for long-running review work. Recognized by `_codex_dispatch.py` as a BACKGROUND trigger: its receipts mirror to the ledger as telemetry only (never `result=received`) and never populate the four-dispatch stamp proof -- a background launch proves a job started, not that a review completed. |
| `scripts/codex-dispatch-with-files.sh` | Claude file-scoped Codex dispatch fallback | Current dispatch surface referenced by implementation-side review guidance. It is not directly classified as the canonical `scripts/codex-dispatch.sh` wrapper today, so §7 owns receipt/evidence normalization before gates can trust it. |
| Codex prompt escaping (`scripts/codex-dispatch.sh` examples + `scripts/lint.sh` Check 12) | Claude/Codex dispatch safety policy | Load-bearing single-quoted prompt discipline prevents shell expansion, command substitution, and redirection before the reviewer sees the prompt. Current Check 12 coverage is WARN-only and skip-able with `SKIP_LINT_PROMPT_ESCAPING=1`; it covers the canonical foreground wrapper examples but does not cover fallback wrapper examples today. Treat it as inventory/telemetry until §6 shared gates and §11 regression coverage make any blocking decision explicit; argv-time wrapper validation is not enough by itself. |
| Codex dispatch bundling (`scripts/lint.sh` Check 15) | Claude/Codex dispatch-shape telemetry | WARN-only check for documented examples under `scripts`, `.claude/skills`, and `docs` where multiple `codex-dispatch.sh` / `codex-bg-dispatch.sh` calls are joined by `&`, `&&`, or `;`. Skip-able with `SKIP_LINT_CODEX_BUNDLE=1`; it does not cover every fallback wrapper shape and is not shipping proof. |
| `.claude/skills/codex-prompt-shape.md` and prompt templates | Review-kind marker source | Defines the canonical review-kind markers, prompt-shape expectations, and matcher/test source-of-truth consumed by Codex skill prompts. Shared ledger and receipt gates must track this source, not only `SKILL.md` files. |
| Prompt-source trust caveats (`codex e`, background receive integrity) | Prompt-source drift | `codex e` is excluded from review classification and background dispatches are telemetry-only in the ledger; completion-bound background receipts remain future work. |
| Direct `codex-companion.mjs task --background` | Claude direct background Codex review trigger | Classified as a BACKGROUND dispatch: attribution is recorded, but receipts mirror as telemetry only and never populate stamp proof (a receive can fire before the background job completes). Completion-bound background receipts remain future work. |
| Bare `codex task` | Codex background job/control CLI | Covered by the invocation policy and model-flag hook as a Codex command surface, but not accepted today by `_codex_dispatch.py` as a reviewer receipt shape. Treat as non-shipping command telemetry unless §7 normalizes it. |
| `codex exec` / `codex e` | Codex non-review automation launcher (historical driver-era surface) | `codex e` is the CLI alias for `exec`. Neither is a review dispatch: `_codex_dispatch.py` excludes both, so they can never mint reviewer evidence through the receipt path. |
| Codex plugin slash commands (`/codex:*`) | Installed Codex plugin command surface | Invocation-policy surface that can call Codex companion flows outside the repo wrappers. Not accepted as shipping review proof today; §7 must bind plugin command dispatch/receipt to trusted wrapper or session metadata before gates can trust it. |
| Codex plugin hooks (`session-lifecycle-hook.mjs`, `stop-review-gate-hook.mjs`) | Installed Codex plugin hook surface | Auto-loaded plugin state and optional stop-time review gate. Adapter telemetry/control only today; not shared ledger evidence and not a substitute for `scripts/codex-dispatch.sh` reviewer evidence unless §7 normalizes it. |
| `.githooks/pre-commit` | Git-time enforcement | Calls existing lint and section-commit gate today; future work can delegate section checks to `gates.py`. |
| `.remember/` plugin buffers (now.md / recent.md / archive.md / today-*.md) | Remember-plugin session memory | SessionStart loads these buffers and PostToolUse appends to them, so workflow context can come from Claude-only memory the ledger does not import. Claude-adapter telemetry/non-proof; scrub stale direction notes during rollout. |
| Claude projects memory store (`~/.claude/projects/.../memory/`) | Claude auto-memory | Per-project memory loaded each session; advisory context, never shipping proof. |
| TODO stamps | Roadmap source | New stamps should be generated by `stamp.py` from ledger evidence. |

The exhaustive per-hook registry is [`.claude/hooks/MANIFEST.md`](../../.claude/hooks/MANIFEST.md); the table above classifies the state-bearing and live-gate surfaces rather than repeating every stateless reminder. State-*writer* hooks not called out individually above: `skill_step_observer.py` (writes `.claude/state/skill-progress.json`), `tool_history_writer.py` (writes `tool-history.jsonl`), `agent_dispatch_recorder.py` (writes `last-agent-dispatch.json`) with `agent_dispatch_required.py` as consumer, `pre_compact_flush.py` (writes `.compaction-snapshots/`), `session_start.py` / `session_brief_inject.py` (read session/brief state), and `post_commit_smoketest.py` / `post_commit_smoketest_boot.py` (post-commit smoke advisories). Root `.claude/state/*.lock` files (`last-review-stamps.lock`, `skill-progress*.lock`) and `.orphan.*` markers are concurrency/control state, not shipping proof.

Stamp shapes currently authored by model sessions are `Verified:`, `Quality
reviewed:`, `Deferred:`, `Accepted:`, `Validated:`, and `Gap-audited:`. TODO-10
only makes the generated `Verified:` path executable in the first slice; the
other stamp types are intentionally modeled in the CLI before being wired into
the shipping gates.

Review kinds recognized by the shared ledger are `design`, `adversarial-impl`,
`adversarial`, `consistency`, `perf`, `test-coverage`, `gap-audit`, and
`re-adversarial`. The minimum ship gate currently requires fresh-context Codex
`adversarial`, `consistency`, and `perf` evidence.

Current workflow review obligations are:

| Workflow source | Required Codex review kinds | Conditional / skippable kinds |
| --- | --- | --- |
| `implement-todo-section` | Pre-code `design` unless a documented skip case applies; implementation-time `adversarial` is canonical; post-commit review pipeline via `review-todo-section` requires `adversarial`, `consistency`, and `perf` | `test-coverage` after tests unless the suite is trivial; `adversarial-impl` is accepted as an implementation-time adversarial alias/variant; `re-adversarial` after non-trivial fix diffs |
| `implement-todo-item` | `adversarial` for the single-item diff | `adversarial-impl` is accepted as an implementation-time adversarial alias/variant; when the item closes a section/IO row it inherits `review-todo-section` obligations (`adversarial`, `consistency`, `perf`, conditional `re-adversarial`) |
| `implement-ssdt-range` | `adversarial` for the SSDT handler range | Re-review focuses on previous findings when the fix loop changes security-sensitive code |
| `review-todo-section` | `adversarial`, `consistency`, `perf` | `re-adversarial` when the cumulative fix diff touches risk triggers |
| `verify-todo-section` | Same review-kind set as `review-todo-section`: `adversarial`, `consistency`, `perf` | `re-adversarial` under the same triggers; audit-mode/downgrade-only stance changes outcomes, not review kinds |
| `quality-review-section` | `adversarial` quality review dispatch | No stamp-shipping proof by itself; it is an improvement/audit workflow layered after review |
| `gap-audit-todo` | `gap-audit` | Skip only for docs-only sweeps with zero new sections, ownership changes, or parity claims |
| `todo-pipeline` | `gap-audit` through Stage 2 `gap-audit-todo` | Stage 1/3 `validate-todo-file` are structural validation, not Codex review |
| `overnight-sequencer` | Inherits `gap-audit` in GAP_AUDIT plus child `implement-todo-section`, `implement-todo-item`, `review-todo-section`, and `complete-todo-file` obligations while driving the phase machine | `implement-todo-item` carries item adversarial review and section-close inherited review; the guard/lease/evidence protocol attributes child workflow proof to the active TODO section or file phase |
| `overnight-todo-runner` | Inherits child `implement-todo-section`, `review-todo-section`, and `complete-todo-file` obligations for each overnight slice | Legacy stop-hook runner; no new review kind beyond child workflow dispatches |
| `implement-unit-tests` | `test-coverage` | Skip for trivial test suites with fewer than three assertions |
| `debug-session` | `adversarial` before non-trivial fixes | Diagnostic-only or no-fix debugging can stop before review; any fix path still needs received/classified review evidence |
| `diagnose-serial-log` | `adversarial` for fix-producing serial-log diagnosis | `--skip-fix` / diagnosis-only mode does not produce shipping proof |
| `complete-todo-file` | `test-coverage` indirectly through `implement-unit-tests` in full close-out mode | Sweep-only mode consumes prior evidence; full close-out delegates Unit Tests work before verification |
| `validate-todo-file` | None | Structural validation consumes existing evidence and does not add a Codex review kind today |

Reviewer run IDs in prompts are correlation hints only, and the receipt hook
enforces that: `codex_review_completed.py` ALWAYS generates the trusted
`review_run_id` at receipt-process time and demotes a prompt-authored id to
`prompt_review_run_id_hint`. The prompt-derived `driver_run_id` is kept as-is
because it only ever EXCLUDES same-run evidence from satisfying review
obligations. Reviewer-role ledger records are receipt-only: `evidence.py
record` refuses `codex-reviewer-*` roles unless
`AI_WORKFLOW_ALLOW_REVIEWER_RECORD=1` (fixture/repair escape).

`codex_review_completed.py` now mirrors received Codex review state into the
shared evidence ledger when the receive step runs. Legacy `.claude/state` files
remain the compatibility source for the current Claude hard gates, but the same
review can be resumed by other drivers through `evidence.py`.

Codex dispatch surfaces remain the existing direct, wrapper, fallback, bare CLI,
and installed-plugin paths: direct `codex-companion.mjs`, foreground
`scripts/codex-dispatch.sh`, fallback `scripts/codex-bg-dispatch.sh` /
`scripts/codex-dispatch-with-files.sh`, direct background
`codex-companion.mjs task --background`, bare `codex task`, bare
`codex review`, driver-side `codex exec` / `codex e`, Codex plugin slash
commands, and Codex plugin hooks. The live classifier recognizes direct
companion review/task forms, the foreground wrapper, and bare `codex review`
ONLY -- the non-review `codex e` / `codex exec` automation aliases are
excluded so they can never mint reviewer evidence through the receipt path.
Fallback wrappers, background task completion, and plugin command/hook
receipt remain unrecognized surfaces. Bare `codex task` and plugin stop-gate
output are not trusted receipt proof. TODO-10 does not add a new prompt tree.

A further dispatch surface is the direct `Skill(codex-*)` trigger set recognized by
`codex_review_completed.py` (`CODEX_TRIGGER_SKILLS`): `codex-adversarial-review-section`,
`codex-review-todo`, `codex-design-review`, `codex-gap-audit`, `codex-impact-analysis`,
`codex-test-coverage`, `codex-consistency-audit`, `codex-perf-review`, and
`codex-fix-review`. The receiver is aligned with `common.REVIEW_KINDS`: `gap-audit`
is recognized on both paths (skill map and `[review-kind: gap-audit]` prompt marker),
recorded by `_record_stamp`, and mirrored to the ledger with its section
UNCONDITIONALLY normalized to the `file` scope (gap audits are file-level lifecycle
evidence; a prompt-quoted section ref survives only as `prompt_section_hint`
metadata).

Prompt escaping is part of that dispatch surface, not prose style. Dispatch
examples must keep the single-quoted `scripts/codex-dispatch.sh` prompt shape.
`scripts/lint.sh` Check 12 documents and warns on unsafe examples, but it is
WARN-only today and can be skipped with `SKIP_LINT_PROMPT_ESCAPING=1`; shared
gates must not treat that advisory coverage as dispatch-safety proof. It also
does not cover fallback wrapper examples (`scripts/codex-bg-dispatch.sh` /
`scripts/codex-dispatch-with-files.sh`) today, so §6/§7/§11 own any future
normalization before those paths can satisfy trusted reviewer evidence. The
wrapper can reject multi-argv calls, but it cannot reconstruct a prompt after
the shell has already expanded it.

Dispatch bundling is a separate telemetry surface: `scripts/lint.sh` Check 15
warns when documented examples join multiple `codex-dispatch.sh` /
`codex-bg-dispatch.sh` calls in one shell command, because only the first review
kind may be recorded. It is also WARN-only and skip-able today, so shared gates
must not treat it as proof.

## Evidence Kinds

The first implementation recognizes these shipping evidence kinds:

- `build`
- `todo-graph-validate`
- `adversarial`
- `consistency`
- `perf`
- `stamp.generated`
- `stamp.verified`

This is the obligation-gated shipping subset, NOT the full set the ledger accepts.
`evidence.py record --kind` accepts any string; the full review-kind vocabulary is
`common.REVIEW_KINDS` (`design`, `adversarial-impl`, `adversarial`, `consistency`, `perf`,
`test-coverage`, `gap-audit`, `re-adversarial`), and `stamp.py` emits `stamp.generated`
plus a `stamp.<kind>` event per stamp shape (`verified`/`quality-reviewed`/`deferred`/
`accepted`/`validated`/`gap-audited`). Shared shipping gates key off the subset above.

Reviewer evidence must use role names like `codex-reviewer-adversarial` and must
be recorded with a run ID distinct from the mutating session run.

Build/test/smoke evidence can carry deterministic metadata such as command,
exit code, log path, final marker, and source blob bindings. Generated stamps
carry `stamp_text_sha256` plus the evidence IDs consumed by the stamp writer.

## Migration Notes

Current Claude hook state remains useful as telemetry and can be imported into
the shared ledger with:

```bash
python3 scripts/ai-workflow/evidence.py import-legacy
```

Legacy imports are marked `legacy_import: true`. `last-review-stamps.json`
entries import with `result=telemetry`, not `received`, so they remain audit
context and cannot satisfy shipping review obligations. `codex-review-history`
entries without a review run ID or non-empty source binding also import as
telemetry.

Skip decisions from the current section-commit gate are mirrored to
`.ai-workflow/skip-log.jsonl` so opt-outs remain visible to non-Claude
drivers.

## Doctrine Boundary

Until TODO-10 fully ships and is reviewed, the existing `CLAUDE.md` authority
hierarchy remains canonical. The shared scripts are the implementation path that
lets doctrine change later without creating a parallel `.codex` rule tree.
