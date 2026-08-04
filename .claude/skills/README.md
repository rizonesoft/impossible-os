# Claude Code skills (source of truth)

Skills in this directory are for **Claude Code**: implementation, review, Codex dispatch, domain checklists. Impossible OS is Claude Code-only; Cursor was removed 2026-04-18 because maintaining a parallel skill set under `.cursor/` created clutter without a corresponding productivity win.

- **Ownership map:** [`../../docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) (Authority Hierarchy, Claude Code-Only Stance, edit-here-not-there rules).
- **Adding/editing/retiring a skill:** [`../../docs/infrastructure/skill-authoring.md`](../../docs/infrastructure/skill-authoring.md) (lifecycle, template, catalog hygiene).
- **Scaffold:** copy [`TEMPLATE.md`](TEMPLATE.md) into `<slug>/SKILL.md`.

Every live skill below has a matching row in [`../../CLAUDE.md`](../../CLAUDE.md#skills). A directory without matching rows is drift -- fix by adding the rows (authoring lifecycle step 3-5) or retiring the skill (authoring lifecycle retirement sweep).

## Catalog

### TODO workflow

| Skill                          | Purpose                                                                               |
| ------------------------------ | ------------------------------------------------------------------------------------- |
| [`create-todo`](create-todo/)  | Scaffold a new TODO file with canonical structure and XREFs                           |
| [`todo-pipeline`](todo-pipeline/) | 3-stage prep: validate -> gap analysis -> validate (before implementation)         |
| [`validate-todo-file`](validate-todo-file/) | Structural audit: numbering, XREFs, OS-Comparison, test checkpoints      |
| [`gap-audit-todo`](gap-audit-todo/) | Deep Win11/Linux parity + cross-TODO overlap + code-truth audit (mandatory secondary Codex pass) |
| [`codex-gap-audit`](codex-gap-audit/) | Codex red-team of a TODO gap-audit inventory (mandatory in `gap-audit-todo` Phase 3.5) |
| [`implement-todo-section`](implement-todo-section/) | Execute one section end-to-end with HARD GATE review pipeline     |
| [`implement-todo-item`](implement-todo-item/) | Execute a single `[ ]` item; auto-promotes to section-ship when it closes the last `[ ]` |
| [`implement-ssdt-range`](implement-ssdt-range/) | Implement + wire a range of SSDT entries, mark Done `[x]`             |
| [`implement-unit-tests`](implement-unit-tests/) | Ship a TODO's Unit Tests section end-to-end                           |
| [`complete-todo-file`](complete-todo-file/) | Finalize a TODO OR sweep an active one for loose ends: stale XREFs / drifted counts / unfilled placeholders / `[/]` platform-list narrowing / reversible `[ ]` manual demos. Full mode also dispatches `implement-unit-tests`, runs Verification items, flags manual-only, commits |
| [`overnight-sequencer`](overnight-sequencer/) | Unattended whole-repo completion driver (headless, armed via `arm-sequencer.sh`): fixpoint loop over every `todo/` file (triage -> validate -> gap-audit -> per-section implement/review -> close -> advance), hard-enforced by `run_phase_guard.py`. Source of truth: `todo/TODO-Claude-Overnight-Runner.md`. |
| [`close-canary-run`](close-canary-run/) | Post-run close-out: triage and implement what a finished canary filed in the newest `token-saver` + `overnight-runner-improvements` versions, record a verdict for EVERY item (resolved / not-reproduced / carried), then close those versions and open the next. Enforced at arm time -- `arm-sequencer.sh` refuses while the newest capture file is CLOSED. |
| [`overnight-todo-runner`](overnight-todo-runner/) | (Superseded by `overnight-sequencer`; interactive fallback pending retirement after first live sequencer run.) Drive one TODO file to completion interactively; Stop-hook blocks final-answer between section ships |

### Review + verification

| Skill                                        | Purpose                                                                 |
| -------------------------------------------- | ----------------------------------------------------------------------- |
| [`review-todo-section`](review-todo-section/)| Post-implementation review with MANDATORY Codex adversarial + quality   |
| [`verify-todo-section`](verify-todo-section/)| Audit-mode wrapper over review -- downgrade-only, never promotes to `[x]` |
| [`quality-review-section`](quality-review-section/) | Is it done RIGHT? -- standards, optimization, Win11/Linux parity |
| [`audit-ssdt`](audit-ssdt/)                  | SSDT registration vs master tables; insert missing prerequisite items   |

### Codex dispatch

| Skill                                                        | Purpose                                                      |
| ------------------------------------------------------------ | ------------------------------------------------------------ |
| [`codex-design-review`](codex-design-review/)                | Pre-implementation design review -- catch plan flaws early   |
| [`codex-review-todo`](codex-review-todo/)                    | Adversarial review of all sections in a TODO                 |
| [`codex-adversarial-review-section`](codex-adversarial-review-section/) | Single-section adversarial review loop (<= 3 rounds)   |
| [`codex-fix-review`](codex-fix-review/)                      | Fix findings and re-review until clean                       |
| [`codex-test-coverage`](codex-test-coverage/)                | Test-coverage gap analysis                                   |
| [`codex-impact-analysis`](codex-impact-analysis/)            | Dependency impact analysis for refactors                     |
| [`codex-consistency-audit`](codex-consistency-audit/)        | Cross-file consistency (struct offsets, API contracts)       |
| [`codex-perf-review`](codex-perf-review/)                    | Performance hot-path review                                  |

### Domain code quality (auto-loads on path match)

| Skill                                            | Applies to                             |
| ------------------------------------------------ | -------------------------------------- |
| [`boot-code-quality`](boot-code-quality/)        | `src/boot/` UEFI bootloader code       |
| [`kernel-code-quality`](kernel-code-quality/)    | `src/kernel/`, `include/kernel/`       |
| [`desktop-code-quality`](desktop-code-quality/)  | `src/desktop/` compositor (placeholder)|
| [`shell-code-quality`](shell-code-quality/)      | `src/shell/` cmd.exe (placeholder)     |
| [`userland-code-quality`](userland-code-quality/)| `user/`, `src/apps/` (placeholder)     |

### Debugging

| Skill                                        | Purpose                                                                           |
| -------------------------------------------- | --------------------------------------------------------------------------------- |
| [`debug-session`](debug-session/)            | Structured hypothesis-driven debugging with Codex validation                      |
| [`diagnose-serial-log`](diagnose-serial-log/)| Serial log audit: crashes, bugs, races, leaks, perf, POLICY/ACCURACY/REGRESSION   |

## Specialist agents (advisory, read-only)

Subagents in [`../agents/`](../agents/) that the skills above delegate work to. Two classes, both enforced by `scripts/lint.sh` Check 14: ANALYSTS are read-only (no `Edit`/`Write`/`Bash`/`Skill`) and return findings as text; RUNNERS (marked `<!-- agent-class: runner -->`) add constrained `Bash` for named idempotent commands (verification scripts, read-only git/gh) but never edit, never mutate git/GitHub, and never touch Codex. The main session does all edits, commits, and Codex dispatches, and re-quotes on-disk artifacts itself before claiming success. Design: the 2026-06-20 specialist-agents design doc, retired from the tree 2026-07-03 (git history: `docs/superpowers/specs/2026-06-20-overnight-specialist-agents-design.md`).

<!-- Agent rows use [name](path), NOT [`name`](path): the skill-catalog check
     (scripts/test-ai-system.sh) counts any `| [`name`]` row as a skill, so the
     backtick-wrapped link form would falsely register these agents as skills. -->

| Agent | Model | Dispatched by |
| ----- | ----- | ------------- |
| [kernel-explorer](../agents/kernel-explorer.md) | sonnet | `implement-todo-section` step 3 + `debug-session` step 4 |
| [kernel-quality-auditor](../agents/kernel-quality-auditor.md) | opus | `review-todo-section` step 7 |
| [boot-quality-auditor](../agents/boot-quality-auditor.md) | sonnet | `review-todo-section` step 7 |
| [parity-research-analyst](../agents/parity-research-analyst.md) | sonnet | `gap-audit-todo` Phase 2-3 + `review-todo-section` steps 9-12 + `create-todo` research |
| [review-evidence-mapper](../agents/review-evidence-mapper.md) | sonnet | `review-todo-section` Phase 1 + `quality-review-section` step 1 |
| [diagnostic-digester](../agents/diagnostic-digester.md) | sonnet | `implement-todo-section` fix loop + `review-todo-section` build-fail |
| [todo-hygiene-auditor](../agents/todo-hygiene-auditor.md) | sonnet | `complete-todo-file` close-out |
| [ssdt-auditor](../agents/ssdt-auditor.md) | sonnet | `audit-ssdt` scan + `implement-ssdt-range` pre-read |
| [serial-log-auditor](../agents/serial-log-auditor.md) | sonnet | `diagnose-serial-log` step 2 + `debug-session` multi-log evidence |
| [test-coverage-mapper](../agents/test-coverage-mapper.md) | sonnet | `implement-unit-tests` step 1 |
| [doc-sync-auditor](../agents/doc-sync-auditor.md) | sonnet | `complete-todo-file` close-out |
| [spec-research-analyst](../agents/spec-research-analyst.md) | sonnet | `implement-todo-section` step 3 + `debug-session` spec lookups |
| [web-research-analyst](../agents/web-research-analyst.md) | sonnet | `implement-todo-section` step 3 + `debug-session` toolchain/emulator/CI research |
| [xref-dependency-mapper](../agents/xref-dependency-mapper.md) | sonnet | `implement-todo-section` step 2 (3+ XREFs) |
| [overnight-log-explorer](../agents/overnight-log-explorer.md) | sonnet | ad-hoc + overnight-runner-improvements backlog (run-transcript cost/behavior digest) |
| [todo-validation-mapper](../agents/todo-validation-mapper.md) | sonnet | `validate-todo-file` / `todo-pipeline` legwork + ad-hoc TODO structure checks |
| [checks-runner](../agents/checks-runner.md) | sonnet (runner) | `implement-todo-section` step 16 + `review-todo-section` build + `complete-todo-file` verification |
| [git-historian](../agents/git-historian.md) | sonnet (runner) | `debug-session` + `diagnose-serial-log` regression pass |
| [gh-query-runner](../agents/gh-query-runner.md) | sonnet (runner) | `complete-todo-file` post-push CI check + GitHub state queries |
