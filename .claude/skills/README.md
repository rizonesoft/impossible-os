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
| [`gap-analysis-todo`](gap-analysis-todo/) | Deep Win11/Linux parity + cross-TODO overlap + code-truth audit             |
| [`implement-todo-section`](implement-todo-section/) | Execute one section end-to-end with HARD GATE review pipeline     |
| [`implement-ssdt-range`](implement-ssdt-range/) | Implement + wire a range of SSDT entries, mark Done `[x]`             |
| [`implement-unit-tests`](implement-unit-tests/) | Ship a TODO's Unit Tests section end-to-end                           |
| [`complete-todo-file`](complete-todo-file/) | Finalize a TODO OR sweep an active one for loose ends: stale XREFs / drifted counts / unfilled placeholders / `[/]` platform-list narrowing / reversible `[ ]` manual demos. Full mode also dispatches `implement-unit-tests`, runs Verification items, flags manual-only, commits |

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
