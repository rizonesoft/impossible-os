# AI Driver Interchangeability

This document describes the TODO-10 implementation shape for making the
Impossible OS development driver interchangeable while keeping Codex as the
required reviewer.

## Roles

Codex remains the reviewer in every supported mode. The interchangeable part is
the mutating driver:

| Mode | Driver | Reviewer |
| --- | --- | --- |
| Claude development | Claude Code | Codex fresh review runs |
| Codex-exclusive development | Codex driver run | Separate Codex fresh review runs |
| Mixed development | One active driver at a time | Codex fresh review runs |

The important invariant is not model branding. It is role separation. A Codex
driver run cannot satisfy a Codex reviewer obligation with the same run ID.

## Shared Protocol

The shared workflow lives under [`scripts/ai-workflow/`](../../scripts/ai-workflow/):

| Script | Purpose |
| --- | --- |
| `lease.py` | Owns the one-active-mutator lease for a TODO section. |
| `evidence.py` | Append-only evidence ledger for reviews, builds, stamps, and validations. |
| `obligations.py` | Reports which workflow obligations are satisfied or missing. |
| `stamp.py` | Generates TODO stamps from ledger evidence instead of model prose. |
| `gates.py` | Shared gate checks for adapters and future hook refactors. |

Generated state lives under `.ai-workflow/`, which is ignored as derived
workflow state. Codex-driver overnight reports live under `.codex/overnight/`
so they do not mix with `.claude/overnight/` reports. There is no repo-owned
`.codex` skill tree; `.codex/overnight/` is runtime output only.

## Inventory

The current Claude-first workflow has three classes of state:

| Surface | Current owner | Migration handling |
| --- | --- | --- |
| `.claude/state/skill-progress.json` | Claude step gate state | Stays Claude adapter state until shared gates replace the blocking decision. Not accepted as cross-driver proof. |
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
| `.claude/state/.compaction-snapshots/` | Claude pre-compaction safety snapshots | Recovery telemetry for post-compaction hook state. Retained for diagnosis and stale-entry avoidance; not accepted as cross-driver proof. |
| `.claude/state/overnight-run.json` | Interactive overnight fallback guard | Legacy adapter state retained until the phase guard fully supersedes it. |
| `.claude/state/conclave-stuck.json` | Claude overnight escalation telemetry | Counts repeated same-target build/test/smoke failures for the warning-only Conclave stuck detector. Codex-driver runs should ignore it for shipping proof and rely on the shared phase guard/defer flow instead of inheriting stale escalation state. |
| `.claude/state/live-gotchas.md` | Curated overnight awareness notes | Read by `runner_status.py` / session brief injection for human/model orientation. Advisory telemetry only; not shared gate evidence. |
| `.claude/state/sequencer-armed` / `.claude/state/sequencer-run.json` / `.claude/state/sequencer-fixpoint` | Overnight phase guard | Shared sequencer control state. It drives phases and FIXPOINT detection but is not build/review/stamp evidence. |
| `.claude/hooks/run_phase_guard.py` | Shared overnight phase gate | Live BLOCK gate for the repo-owned sequencer phase machine. Claude and Codex drivers must use this path for cursor and phase transitions; it is control state, not shipping proof. |
| `.claude/hooks/sequencer_triage.py` + `build/todo-cache.json` + TODO Verified-stamp scan | Overnight traversal and FIXPOINT oracle | Derived traversal control used by `run_phase_guard.py --next` / `fixpoint` to prevent premature completion. It guides cursor/FIXPOINT decisions but is not build/review/stamp evidence for a section. |
| `.claude/hooks/overnight_plugin_skill_block.py` | Claude Skill BLOCK gate for overnight arming | Blocks unguarded plugin overnight skills so runs arm through the repo-owned sequencer. Preserve this protection for non-Claude drivers by keeping overnight launch behind `run_phase_guard.py` and shared scripts. |
| `.claude/hooks/overnight_guard.py` | Claude legacy interactive overnight guard | Interactive fallback that tracks `.claude/state/overnight-run.json`; retained as legacy adapter behavior until the sequencer path fully supersedes it. Not cross-driver proof. |
| `.claude/state/tool-history.jsonl` / `.claude/state/tool-history.jsonl.1` | Claude tool telemetry and hook input | Adapter telemetry used by several Claude hooks, with one rotated file read by the commit gate. Not accepted as cross-driver proof. |
| `.claude/state/codex-review-debug.jsonl` | Claude Codex receipt debug log | Optional missing-write diagnostic emitted by `codex_review_completed.py`. Debug telemetry only; never accepted as received-review proof. |
| `.claude/state/skip-log.jsonl` | Claude section-commit opt-out state transition | Audit and reset telemetry for `SKIP_REVIEW_HOOK`; rewrites `last-codex-review.json` to prevent stale received-review reuse. Import only as warning/transition context, not shipping proof. |
| Stateless policy BLOCK hooks (`unicode_dash_block.py`, `numeric_todo_shorthand.py`, `scope_gap_marker.py`, `test_side_effect_ban.py`, `bare_section_refs.py`, `citation_block.py`, `notes_bloat_check.py`, `todo_item_line_length.py`, `accepted_xref_block.py`, `codex_model_flag_block.py`) | Claude deterministic policy gates | Direct command/file-content gates with no durable proof value. Keep as Claude adapter protection and mirror through lint/git/shared scripts where non-Claude drivers need the same block. |
| `.claude/hooks/design_review_required.py` | Claude design-review gate | Live STATE+BLOCK gate that uses transcript/session state to require design-review dispatch before implementation edits. Claude-adapter gate until a shared pre-implementation review obligation exists. |
| `.claude/hooks/receiving_review_required.py` | Claude review-receipt gate | Live STATE+BLOCK gate that requires explicit receiving-code-review handling after Codex output. Claude-adapter gate until shared receipt/classification evidence replaces it. |
| `.claude/hooks/section_review_required.py` | Claude section-review gate | Live STATE+BLOCK gate that requires section review workflow state before section commits. Claude-adapter gate until shared obligations and ledger evidence fully replace it. |
| `.claude/hooks/skill_step_block.py` | Claude skill-step gate | Live STATE+BLOCK gate that enforces flagship skill step ordering from `skill-progress.json` and skip state. Claude-adapter gate until shared sequenced obligations exist. |
| `.claude/hooks/section_commit_gate.py` | Claude Bash/git section gate | Live gate input aggregator for build evidence, Codex receipt, receiving-review state, re-adversarial and impl-side adversarial reviews, smoke markers, test-wiring checks, and heuristic WARN logs. Shared gates must replace the blocking decision before non-Claude commits can ship. |
| `.claude/hooks/phase1_evidence_gate.py` | Claude pre-dispatch review gate | Enforces transcript-scoped read/grep preparation before adversarial review dispatch. This remains Claude adapter gating until equivalent shared pre-review evidence exists. |
| `.claude/hooks/step5_quality_gate.py` | Claude pre-edit quality gate | Enforces quality-skill invocation before `src/` / `include/` edits in Claude implementation flows. Codex parity requires a shared or adapter-level equivalent; the hook state is not ledger proof. |
| `.claude/hooks/todo_graph_auto_rewrite.py` | Claude TODO line-number side-effect mutator | PostToolUse hook that runs `validate.py --fix-line-numbers --write` after TODO edits. Claude adapter freshness helper, not proof; non-Claude drivers must run todo-graph validation/fix explicitly before stamp or commit gates consume TODO XREFs. |
| `.claude/hooks/_codex_dispatch.py` / `codex_review_completed.py` | Claude Codex dispatch classifier and receiver | Recognizes direct `codex-companion.mjs`, wrapper `scripts/codex-dispatch.sh`, and bare `codex review` review dispatch shapes. It also currently misclassifies bare `codex e` as review even though the CLI aliases it to `exec`; §7 owns trusted receipt, kind parsing, and role-separation normalization. |
| `scripts/codex-bg-dispatch.sh` | Claude background Codex dispatch fallback | Current fallback wrapper for long-running review work. The wrapper command itself is not one of the wrapper shapes directly recognized by `_codex_dispatch.py` today, even though it invokes the companion background-task path; §7 owns wrapper/receipt alignment. |
| `scripts/codex-dispatch-with-files.sh` | Claude file-scoped Codex dispatch fallback | Current dispatch surface referenced by implementation-side review guidance. It is not directly classified as the canonical `scripts/codex-dispatch.sh` wrapper today, so §7 owns receipt/evidence normalization before gates can trust it. |
| Direct `codex-companion.mjs task --background` | Claude direct background Codex review trigger | The current dispatch classifier treats the companion `task` subcommand as a review trigger. That can set review dispatch/receipt state before the background job output is source-bound; §7 owns completion binding and parser correction. |
| Bare `codex task` | Codex background job/control CLI | Covered by the invocation policy and model-flag hook as a Codex command surface, but not accepted today by `_codex_dispatch.py` as a reviewer receipt shape. Treat as non-shipping command telemetry unless §7 normalizes it. |
| `codex exec` / `codex e` | Codex driver / automation launcher | Current automation surface for Codex driver runs and overnight launch; `codex e` is the CLI alias for `exec`. Neither should be trusted reviewer-receipt proof, even though the current dispatch classifier wrongly accepts `codex e` as review. |
| Codex plugin slash commands (`/codex:*`) | Installed Codex plugin command surface | Invocation-policy surface that can call Codex companion flows outside the repo wrappers. Not accepted as shipping review proof today; §7 must bind plugin command dispatch/receipt to trusted wrapper or session metadata before gates can trust it. |
| Codex plugin hooks (`session-lifecycle-hook.mjs`, `stop-review-gate-hook.mjs`) | Installed Codex plugin hook surface | Auto-loaded plugin state and optional stop-time review gate. Adapter telemetry/control only today; not shared ledger evidence and not a substitute for `scripts/codex-dispatch.sh` reviewer evidence unless §7 normalizes it. |
| `.githooks/pre-commit` | Git-time enforcement | Calls existing lint and section-commit gate today; future work can delegate section checks to `gates.py`. |
| TODO stamps | Roadmap source | New stamps should be generated by `stamp.py` from ledger evidence. |

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

Driver and reviewer run IDs in prompts are correlation hints only. Shipping
gates must eventually derive trusted role separation from the driver lease,
dispatch wrapper/session state, or receipt process metadata rather than trusting
prompt-authored `driver_run_id` / `review_run_id` strings.

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
commands, and Codex plugin hooks. The live classifier currently recognizes
direct companion review/task forms, the foreground wrapper, bare `codex review`,
and incorrectly bare `codex e`; fallback wrappers, background task completion,
plugin command/hook receipt, and the `codex e` alias need §7 receipt/evidence
normalization. Bare `codex task` and plugin stop-gate output are not trusted
receipt proof, and `codex exec` / `codex e` are driver automation. TODO-10 does
not add a new prompt tree.

## Codex Adapter

[`scripts/codex-driver.sh`](../../scripts/codex-driver.sh) is the Codex-facing
adapter.

Dry run:

```bash
bash scripts/codex-driver.sh --dry-run --todo todo/00-infrastructure/TODO-10-ai-driver-interchangeability.md --section 8
```

Dry-run mode writes a plan and obligation snapshot under
`.ai-workflow/runs/<run-id>/` and records no shipping evidence.

Live mode:

```bash
AI_WORKFLOW_CODEX_DRIVER=1 bash scripts/codex-driver.sh --live --todo <todo> --section <n>
AI_WORKFLOW_CODEX_DRIVER=1 bash scripts/codex-driver.sh --live --checkpoint pre-commit --todo <todo> --section <n>
```

Live mode is explicit. It acquires the shared driver lease and then uses the
same gate checks as other drivers. The default checkpoint is `pre-edit`, which
verifies the lease and prints missing obligations without pretending the section
is commit-ready. `pre-stamp` and `pre-commit` run the stronger shared gates
after evidence has been recorded. It does not bypass missing review, build, or
stamp obligations.

## Overnight Codex Driver

The unattended sequencer can be armed with Codex as the active driver:

```bash
bash .claude/skills/overnight-sequencer/arm-sequencer.sh --driver codex
```

The arming path is intentionally the same repo-owned scheduler used for Claude:
`arm-sequencer.sh` writes the same `sequencer-armed` marker, uses
`scripts/overnight/overnight-arm.sh`, and launches
`scripts/overnight/overnight-launch.sh`. The Codex branch changes only the
driver process and runtime log root. Codex does not run as one unbounded agent
process; [`scripts/overnight/codex-sequencer-supervisor.sh`](../../scripts/overnight/codex-sequencer-supervisor.sh)
owns the outer phase loop, invokes Codex for one validation, gap-audit, section,
or file-close step at a time, and verifies section-level progress before
continuing:

| Surface | Claude driver | Codex driver |
| --- | --- | --- |
| Launcher | `claude -p ...` | supervisor -> bounded `codex --ask-for-approval never exec --json ...` |
| Approval mode | Claude `bypassPermissions` | Codex `--ask-for-approval never` |
| Sandbox | Claude permission mode | Codex `--sandbox danger-full-access` by default |
| Reports | `.claude/overnight/reports/latest.log` | `.codex/overnight/reports/latest.log` |
| Metrics | `.claude/overnight/metrics/` | `.codex/overnight/metrics/` |
| Governance state | `.claude/state/sequencer-run.json` | same |
| Shipping gates | Claude hooks + shared scripts | shared scripts + git hooks |

`OVERNIGHT_CODEX_SANDBOX` can override the Codex sandbox to `read-only`,
`workspace-write`, or `danger-full-access`; the default is
`danger-full-access` so unattended runs do not stall on approval prompts. The
governance boundary remains the same: Codex must follow
`todo/TODO-Claude-Overnight-Runner.md`, use `run_phase_guard.py` for the phase
cursor, acquire section leases through `scripts/codex-driver.sh --live`, satisfy
fresh reviewer evidence through separate Codex review runs, and pass the shared
stamp/commit gates.

Because Codex can be both the active driver and the required reviewer backend,
the supervisor installs a temporary `codex` PATH guard inside each driver step.
Raw nested Codex automation (`codex exec`, `codex e`, `codex task`, and
unmarked availability probes) is blocked so the driver cannot recursively spawn
itself or self-certify its own work. Repo-owned reviewer wrappers
(`scripts/codex-dispatch.sh`, `scripts/codex-dispatch-with-files.sh`, and
`scripts/codex-bg-dispatch.sh`) set `CODEX_REVIEWER_DISPATCH=1`; only then does
the guard chain to the real Codex binary so the installed plugin can run
`codex --version`, `codex app-server --help`, and the reviewer app-server path.
The marker is wrapper-owned dispatch metadata, not shipping evidence.

The supervisor adds a hard progress budget around Codex-driver mode:

| Setting | Default | Purpose |
| --- | ---: | --- |
| `OVERNIGHT_CODEX_STEP_TIMEOUT_SECONDS` | `5400` | Max wall time for one bounded Codex step. |
| `OVERNIGHT_CODEX_SUPERVISOR_STEPS` | `8` | Max bounded steps per systemd launch before the watchdog relaunches. |
| `OVERNIGHT_CODEX_NO_PROGRESS_LIMIT` | `3` | Same phase/file/section attempts without section-level progress before writing `.codex/overnight/codex-supervisor.block`. |

When the block file exists, watchdog relaunches no-op instead of burning an
overnight session on the same stuck target. Remove both
`.codex/overnight/codex-supervisor.block` and `.codex/overnight/codex-no-progress`
after inspecting the report and fixing the underlying blocker. Re-arming with
`arm-sequencer.sh --driver codex` clears those two supervisor block/progress
files as runtime state, but leaves `.codex/overnight/reports/` and metrics in
place for post-run review.

Monitor a Codex overnight run with:

```bash
bash scripts/overnight/overnight-monitor.sh codex
```

The git hook can opt into the shared staged-commit lease check with:

```bash
AI_WORKFLOW_ENFORCE_SHARED_GATES=1 git commit
```

The flag is deliberately explicit until Claude hooks automatically acquire
leases for section flows.

## Evidence Kinds

The first implementation recognizes these shipping evidence kinds:

- `build`
- `todo-graph-validate`
- `adversarial`
- `consistency`
- `perf`
- `stamp.generated`
- `stamp.verified`

Reviewer evidence must use role names like `codex-reviewer-adversarial` and must
be recorded with a run ID distinct from the driver run ID.

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
