# AI Development System -- Ownership Map

> Canonical reference for who-owns-what across the Impossible OS AI surface. `CLAUDE.md` links here from its "Skills" section; [`docs/infrastructure/index.md`](index.md) lists this doc so it is discoverable from the Infrastructure landing page. Roadmap ownership lives in the [AI Development System roadmap](../../todo/00-infrastructure/TODO-02-ai-development-system.md).

## Authority Hierarchy (read this first)

> **Claude Code is the primary interactive orchestrator.** Doctrine files tell Claude what to do, skills tell Claude how to do it, and external reviewers tell Claude what might be wrong. There is no sibling executor in this repo. When this page uses the term "source of truth" it always identifies WHICH file or tool owns a particular kind of authority.

| Layer                             | Role                                             | Authority over                                                                                                                     |
| --------------------------------- | ------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------- |
| **Claude Code (tool)**            | Primary interactive orchestrator                 | Interactive code edits, commits, skill invocations, reviewer dispatches                                                            |
| `CLAUDE.md`                       | Doctrine source-of-truth (file)                  | Product north star, workflow rules, safety constraints, policy                                                                     |
| `.claude/skills/`                 | Workflow source-of-truth (directory)             | How Claude executes a specific task (implement, review, verify, diagnose)                                                          |
| `.claude/settings.json`           | Harness policy source-of-truth (file)            | Permissions, hook reminders, pre/post-tool-use gates                                                                               |
| Codex review mode                 | External reviewer                                | Adversarial findings only; invoked from Claude skills; findings go through receiving-code-review discipline before action |
| `.githooks/`                      | Git-time guards (distinct layer; see [Git Hooks and Local Automation Lifecycle](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle)) | Pre-commit lint, post-commit COUNT, opt-in pre-push, opt-in shared AI-workflow gate                                              |

### Hierarchy invariants

1. **Doctrine lives in `CLAUDE.md`. Nowhere else.** Skill headers, tool instructions, and regression messages reference doctrine but do not redefine it. Edits go to `CLAUDE.md` first, then propagate.
2. **Skills live in `.claude/skills/` only.** No parallel skill trees (`.cursor/skills/`, `.codex/skills/`, `.other-tool/skills/` etc.).
3. **External reviewers return findings, never edits.** Codex review output is information the active orchestrator reads and judges. Each finding is verified at file:line and classified Fix / Reject / Accept-XREF before action.
4. **CLAUDE.md wins on conflict.** If a skill, a hook message, or an external-tool config contradicts `CLAUDE.md`, `CLAUDE.md` is right and the other layer is the bug. Fix the drift, do not fork the doctrine.
5. **Claude Code remains the only implementation agent.** A human operator talks to Claude for normal work. External reviewers return findings only.

---

## Claude / Reviewer Boundary

Impossible OS is Claude Code-only for implementation and overnight execution. Codex remains available only as an external reviewer invoked by Claude skills.

- **No parallel skill sets.** Doctrine lives in `CLAUDE.md`; Claude skills live in `.claude/skills/`. `.codex/skills/` remains forbidden.
- **Claude owns runner files and state.** Runner mechanics stay under `.claude/skills/overnight-sequencer/`, `.claude/hooks/`, and `scripts/overnight/`.
- **Codex review mode remains finding-only.** When Codex is used as a reviewer, its output goes through receiving-code-review discipline.
- **Other AI tools still go through the [External-Reviewer Contract](../../todo/00-infrastructure/TODO-02-ai-development-system.md#4-external-reviewer-contract-codex)** before adoption.

---

## Global Doctrine vs Tool-Local Doctrine

Global doctrine lives in `CLAUDE.md` and binds every layer (Claude, skills, hooks, Codex). Tool-local doctrine lives in a specific layer and governs only that layer's mechanics.

### Global doctrine (CLAUDE.md is the single owner)

| Rule                                                  | Canonical section                                                                                                                                                          |
| ----------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Product north star (Win11-compatible, complete, better) | [`CLAUDE.md` -- Product North Star](../../CLAUDE.md#product-north-star----complete-compatible-better)                                                                     |
| Bare metal first; VMs are convenience                 | [`CLAUDE.md` -- Development Strategy](../../CLAUDE.md#development-strategy----bare-metal-first-smp-from-day-one)                                                          |
| SMP-safe by default; no single-CPU assumptions        | [`CLAUDE.md` -- Development Strategy](../../CLAUDE.md#development-strategy----bare-metal-first-smp-from-day-one)                                                          |
| POST16 is for boot-path code ONLY                     | [`CLAUDE.md` -- Bare Metal Gotchas](../../CLAUDE.md#bare-metal-gotchas) + [Smoke-Test POST16 Assertions](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md#10-smoke-test-post16-assertions)   |
| No Unicode en/em dashes in tracked files              | [`CLAUDE.md` -- No Unicode Dashes](../../CLAUDE.md#no-unicode-dashes-enem-ascii-only)                                                                                     |
| Freestanding kernel (no stdlib, `kernel/types.h`)     | [`CLAUDE.md` -- Freestanding Kernel](../../CLAUDE.md#freestanding-kernel----no-stdlib)                                                                                    |
| Win32 is the native API; POSIX via compat only        | [`CLAUDE.md` -- API Surface](../../CLAUDE.md#api-surface----win32-native)                                                                                                 |
| Test code must NOT call live boot infrastructure      | [`CLAUDE.md` -- Test Code](../../CLAUDE.md#test-code----no-live-boot-infrastructure-calls)                                                                                |
| Bare Metal Gotchas list                               | [`CLAUDE.md` -- Bare Metal Gotchas](../../CLAUDE.md#bare-metal-gotchas)                                                                                                   |
| Safety Gates (GDT order, user-bit, guard pages, scheduler) | [`CLAUDE.md` -- Safety Gates](../../CLAUDE.md#safety-gates)                                                                                                          |
| Mandatory skill triggers (domain code-quality, receiving-code-review) | [`CLAUDE.md` -- Mandatory Skill Triggers](../../CLAUDE.md#mandatory-skill-triggers)                                                                 |

Every row above is load-bearing across ALL layers. If a skill, hook message, or external-tool config contradicts one of these rules, the other layer is the bug.

### Tool-local mechanics (each tool owns its own)

| Layer                             | Local concern (not doctrine)                                                                                  |
| --------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| `.claude/skills/*/SKILL.md`       | Step-by-step Claude workflows; invoke-in-chat names; when-to-use heuristics                                   |
| `.claude/settings.json`           | Permission allow/deny lists; hook matchers; environment variables; pre/post-tool-use reminders                |
| Codex review mode                 | Codex-specific review prompts and output; findings only, received through Fix / Reject / Accept-XREF triage |

Tool-local mechanics are implementation details. They can change independently as long as they continue to respect global doctrine. Global doctrine, by contrast, only changes via a `CLAUDE.md` edit and propagates from there.

---

## Edit-Here-Not-There Rules

Preserve the hierarchy by editing the ONE canonical location when a concept has a duplicate restatement.

| Concept                                          | Canonical edit location                                                               | Downstream consumers (do NOT edit here)                                                                                                          |
| ------------------------------------------------ | ------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| Doctrine (north star, SMP, bare metal, testing rules) | [`CLAUDE.md`](../../CLAUDE.md)                                                        | Any skill header that mentions the same rule (read-only reference); the ai-system.md Global-Doctrine table above (update the link target only)   |
| Skill workflow (steps, gates, guardrails)        | [`.claude/skills/<skill>/SKILL.md`](../../.claude/skills/)                            | Skill catalog table in `CLAUDE.md` (update the one-line description only when a skill is added/renamed/retired)                                  |
| Skill lifecycle (how to add/edit/retire)         | [`docs/infrastructure/skill-authoring.md`](skill-authoring.md)                        | [`.claude/skills/TEMPLATE.md`](../../.claude/skills/TEMPLATE.md) (scaffold only -- structural changes go to skill-authoring.md first)            |
| Harness policy (hooks, permissions)              | [`.claude/settings.json`](../../.claude/settings.json) + [§5 boundary](#mcp-permissions-and-extension-boundary) | Mandatory-Skill-Triggers table in `CLAUDE.md` (update when a hook is added that enforces a new rule)                                             |
| Codex review templates in Claude workflow        | Individual `codex-*` skills under [`.claude/skills/`](../../.claude/skills/)          | Codex plugin config (external); never restate doctrine in plugin docs                                                                            |
| Git-hook lifecycle (pre-commit lint, post-commit COUNT, pre-push) | [Git Hooks and Local Automation Lifecycle](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle) + [`.githooks/`](../../.githooks/) | `CONTRIBUTING.md` "Enable Git Hooks" section (copy-pastable install commands only); `CLAUDE.md` "Git Hooks" section (one-line pointer) |
| TODO workflow (validate, gap-analysis, implement, review) | [`.claude/skills/`](../../.claude/skills/) (the individual skill files are the SoT)   | `/todo-pipeline` orchestrator (references the individual skills; never inlines their content)                                                    |

**When in doubt:** if the same fact appears in two places and they drift, the downstream consumer is the bug.

---

## Hook Routing Matrix

Hooks are part of the AI system, not invisible glue. This matrix is the human-readable view of [`.claude/settings.json`](../../.claude/settings.json); the JSON blob stays canonical but is not the place to read from. The full per-hook table (kind, matcher, wrap.sh-eligibility, opt-out env, purpose) lives at [`.claude/hooks/MANIFEST.md`](../../.claude/hooks/MANIFEST.md), which `scripts/audit-hooks.sh` cross-checks against the in-tree files; the matrix below is a quick-reference subset, NOT the canonical source of truth.

> **Opt-out env vars ([Unified SKIP-env Scanner](../../todo/00-infrastructure/TODO-08-automation-hardening.md#23-unify-skip_review_hook-opt-out-scanner-across-pretooluse-gates), 2026-04-29).** Every PreToolUse gate that honors a `SKIP_*` env reads it through the shared [`_skip_env.py`](../../.claude/hooks/_skip_env.py) helper. The helper merges TWO sources -- inline cmd env-prefix (`SKIP_FOO=1 git commit ...`, `env SKIP_FOO=1 ...`, `sudo SKIP_FOO=1 ...`) AND `os.environ` -- with inline-wins on key collision. **Both shapes work for every gate without a wrapper.** Each consumer passes an explicit key list (not a prefix) so suffixed env vars cannot accidentally be captured. Pre-unification the gates split into env-only vs inline-scanner camps and required `scripts/commit-with-skip.sh` (now retired) to bridge them.

> **Review-pipeline passthrough policy ([PreToolUse Prefix-Allowlist Standardization](../../todo/00-infrastructure/TODO-08-automation-hardening.md#29-standardize-pretooluse-prefix-allowlist-across-gates), 2026-04-29).** Review-pipeline gates (currently `section_review_required.py`; future review-gates inherit the same policy) consume the shared [`_review_pipeline_passthrough.py`](../../.claude/hooks/_review_pipeline_passthrough.py) helper to short-circuit on review-tooling Bash commands (`git `, `bash scripts/`, `node `, `python3 `, `grep `, `awk `, `sed `, `wc `, `head `, `tail `, `ls `, `cat `, `find `, `rg `, `cd `). **POLICY-SCOPED: this list is NOT a generic "trusted tooling" primitive.** It permits broad code-execution prefixes (`node`, `python3`, `bash scripts/`) that are safe inside the post-ship review pipeline (which legitimately runs Codex dispatches, inline scripted helpers, and tooling scripts) but are NOT safe to inherit blindly into write-sensitive gates. Future gates with different threat models define their OWN policy-scoped helper (e.g. `_kernel_touching_passthrough.py`); they do NOT consume `_review_pipeline_passthrough.py`. `scripts/audit-hooks.sh` flags drift -- a gate with its own multi-prefix `cmd.startswith((...))` tuple instead of consuming the helper.

> **TODO-08 expansion (2026-04-28).** The hook surface roughly doubled in TODO-08:
> - **§3 receiving-review hard gate:** `receiving_review_required.py` (BLOCK) + `codex_review_completed.py` (STATE; the `block-via: warning-only` STATE hook that records the gate state for §3).
> - **§4 section-commit gate:** `section_commit_gate.py` (BLOCK) -- replaces the prior reminder.
> - **§5 four-dispatch enforcement:** `last-review-stamps.json` per-kind state read by §4.
> - **§7 hook system audit:** [`MANIFEST.md`](../../.claude/hooks/MANIFEST.md) + [`scripts/audit-hooks.sh`](../../scripts/audit-hooks.sh) (5-check drift detector).
> - **§10 skill step-state telemetry:** `skill_step_observer.py` (PostToolUse STATE), `skill_step_block.py` (PreToolUse BLOCK on `git commit` / `Skill(review-todo-section)`).
> - **§11 hook event surface expansion:** `session_start.py` (SessionStart), `user_prompt_doctrine.py` (UserPromptSubmit), `stop_audit.py` (Stop), `subagent_audit.py` (SubagentStop), `pre_compact_flush.py` (PreCompact), `tool_history_writer.py` (PostToolUse `*`).
> - **§12 AI-Slop content gates:** `scripts/lint.sh` Checks 6/7/8 + `scripts/lint/check_tautological_test.py`. (Lint, not a harness hook -- listed for completeness.)
> - **§16 compaction resilience:** `pre_compact_flush.py` marks every active `.claude/state/skill-progress.json` entry as `compaction_orphaned: true` (with `orphan_ts_ns` + `orphan_reason`) on PreCompact firing; `skill_step_block.py` and `skill_step_observer.py` skip orphaned entries. Without this, the PostToolUse step-observer cannot run during summary generation -- the entry's `steps_observed` list freezes pre-compaction and the skill-step-block gate would BLOCK every post-compaction commit indefinitely (PreToolUse-vs-PostToolUse catch-22). The orphan-skip events are appended to `.claude/state/skill-progress-skip.log` (200-line JSONL ring buffer) for audit. Same hook also invalidates `.claude/state/transcript-scan-cache.json` (the design-review hook's offset cache) on each compaction since the semantic window changes.
> - **§17 review-pipeline pre-Codex enforcement:** `phase1_evidence_gate.py` (PreToolUse Bash BLOCK) refuses adversarial Codex dispatches when an active `review-todo-section` skill has fewer than 2 prior `Read`/`Grep`/`Glob` events scoped to `src/` or `include/` -- closes the "jumped straight to Phase 2" failure observed on the 2026-04-28 Boot UX Polish review. `section_commit_gate.py::_re_adversarial_trigger_check` (BLOCK extension) refuses commits whose staged C/H diff matches step-13.5 triggers (`spinlock_t|atomic_t|atomic\d+_t|mutex_t` locking, `boot_halt|panic|KeBugCheck*|fault_handler|exception_*` faultable, `\w*_(alloc|free|refcount)` lifecycle, >50 LOC) without a recent `re-adversarial` stamp in `last-review-stamps.json`; fires on both real-section commits and pure-source fix-loop commits when active skill state provides TODO attribution. `codex_review_completed.py` extended to attribute `[review-kind: re-adversarial]` dispatches into the same stamp file. Doctrine: `feedback_re_adversarial_small_fix` memory.
> - **§22 review-pipeline right-sizing (partial retraction 2026-04-29):** the original §22 shipped a `RISK_TIER` doctrine that right-sized the Codex pipeline by tier and a 4th `Defer-to-tier` triage option. Both were retracted because the tier classifier removed value from `implement-todo-section` enforcement -- a task declared `trivial` would skip Codex dispatches the section deserved. **Every section now runs the full Codex pipeline** (design + adversarial + consistency + perf + conditional re-adversarial + fix loop). Surviving infrastructure: `_bootstrap_mode.py` helper lets new gate hooks downgrade BLOCK to WARN on the commit that ships the gate itself (closes the chicken-and-egg loop seen during §17 implementation); `skill_step_observer.py` spiral-check fires a one-shot `systemMessage` reminder at 2x and 4x of a uniform 60-min wall-clock budget; `codex_review_completed.py` `CODEX_REVIEW_DEBUG=1` env-gated log diagnoses missing-write incidents on `last-codex-review.json` / `last-review-stamps.json`. Triage doctrine: 3 options (Fix / Reject / Accept-XREF) per [`docs/infrastructure/triage-codex-finding.md`](triage-codex-finding.md).
> - **Codex prompt-scope helper for re-reviews:** when a re-review dispatches against already-committed work and the working tree is clean, Codex's default working-tree-diff scope returns "approve / no findings" because the diff is empty. The wrapper [`scripts/codex-dispatch-with-files.sh`](../../scripts/codex-dispatch-with-files.sh) is a drop-in replacement for direct `codex-companion.mjs adversarial-review` invocation -- when the prompt names files (via the canonical `[review-kind:]` + TODO-path + section-marker convention) AND the working tree is clean, it embeds `git show HEAD -- <files>` content into the prompt before invoking Codex. Dirty-tree dispatches pass through unchanged. **Use this wrapper whenever the dispatch is re-reviewing already-committed work** (prompt mentions a commit hash, "the last commit", or "the section-N ship"). Caps: 8 files / 200 lines per file. Plugin file is NOT modified -- it lives in plugin-cache and gets overwritten on plugin update.
> - **[Implement-pipeline section-commit gates](../../todo/00-infrastructure/TODO-08-automation-hardening.md#20-implement-pipeline-section-commit-gates-steps-581316):** four `implement-todo-section`-aware gates that fire only when the skill is the active flow in `skill-progress.json` (so non-skill workflows are not gated). Step-5 [`step5_quality_gate.py`](../../.claude/hooks/step5_quality_gate.py) (PreToolUse Edit/Write/MultiEdit BLOCK) refuses the first edit on `src`/`include` until a domain-matching `<domain>-code-quality` Skill is invoked. `section_commit_gate.py` adds three commit-time extensions: step-8 test-wiring (require staged or HEAD `test_*.c` or `**Note:** No <surface> test surface` exemption), step-13 impl-side adversarial (canonical `[review-kind: adversarial]`; `[review-kind: adversarial-impl]` is accepted as an implementation-time alias/variant), step-16 boot-path smoke test (`build/smoke-test.stripped.log` exists, fresher than staged file mtime, contains `Boot complete` + `C:\>` serial markers). All honor `_bootstrap_mode` self-shipping bypass; step-16 has its own `SKIP_SMOKE_GATE=1` opt-out (separate from `SKIP_REVIEW_HOOK`).
>
> The static tables below are kept for the historical "BLOCK / REMIND / POST-HOC" three-class taxonomy. Update the canonical [`MANIFEST.md`](../../.claude/hooks/MANIFEST.md) when adding new hooks; the Markdown matrix below is supplementary.

### Two hook layers -- do not confuse them

- **Harness hooks (Claude Code)** -- run at tool-call time (per Edit / Write / Bash / Skill invocation). Config: [`.claude/settings.json`](../../.claude/settings.json). Owned here (§3 of [TODO-02](../../todo/00-infrastructure/TODO-02-ai-development-system.md#3-hook-routing-and-policy-contract)).
- **Git hooks** -- run at commit / push time. Config: [`.githooks/pre-commit`, `post-commit`, `pre-push`](../../.githooks/). Owned by [Git Hooks and Local Automation Lifecycle](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle).

### Effect classes + path-filter abbreviations

Every harness hook falls into one of three classes:

- **BLOCK** -- hook exits non-zero (`sys.exit(2)` or `permissionDecision=deny`) and stops the tool call. Reserved for state-corrupting rules.
- **REMIND** -- hook emits `systemMessage`; tool call proceeds. Workflow nudges and sync invariants.
- **POST-HOC** -- hook runs `scripts/test.sh` or `scripts/test-smoke.sh` after a successful tool call and reports the result. Advisory; never rolls back.

Path-filter abbreviations used in the tables below:

- `C-src` = `.c`/`.h`/`.cpp`/`.asm`/`.S` under `src/kernel/`, `include/kernel/`, `src/boot/`, `src/desktop/`, `src/shell/`, `user/`, `src/apps/`.
- `test_*.c` = `src/kernel/test/test_*.c` (excluding `test_runner.c`, `test_main.c`).
- `todo/*.md` = any markdown file under `todo/`.
- `skills/*.md` = any markdown under `.claude/skills/`.
- `HEAD files (code)` / `HEAD files (boot)` = files listed by `git show --name-only --format= HEAD` (post-commit), filtered by extension (`.c`/`.h`/`.asm`/`.ld`) or boot-path prefix. Canonical commit file list; catches pure-modification commits that `git commit` stdout omits.

### BLOCK hooks (5)

| #  | Trigger       | Filter                            | Rule                                                                                                                            |
| -- | ------------- | --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| 2  | Pre: Edit     | any content                       | Reject em/en dash (U+2014, U+2013). [`CLAUDE.md`](../../CLAUDE.md#no-unicode-dashes-enem-ascii-only).                           |
| 3  | Pre: Edit     | `.md` outside `todo/`/`.claude/`  | Reject numeric TODO shorthand (`TODO-NN section N`, `DNN TNN section N`). Mirror of `scripts/lint.sh` Check 4 at edit time.    |
| 4  | Pre: Edit     | C-src                             | Reject new `TODO`/`FIXME`/`HACK`/stub markers without scope-gap resolution (see protocol under implement-todo-section/). |
| 5  | Pre: Edit     | `test_*.c`                        | Reject live boot-infra calls (`boot_progress`, `vpd_*`, `panic`, subsystem `_init`). See CLAUDE.md "Test Code".            |
| 6  | Pre: Bash     | `git commit` with staged `todo/`  | Reject bare `Accepted:`/`Deferred:` XREF. See [review step 15](../../.claude/skills/review-todo-section/SKILL.md). Soft XREFs = WARN. |

### REMIND hooks (12)

| #  | Trigger       | Filter                            | Reminder                                                                                                                        |
| -- | ------------- | --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| 1  | Pre: Edit     | C-src                             | Auto-load matching domain code-quality skill (boot / kernel / desktop / shell / userland).                                      |
| 7  | Pre: Bash     | `git commit` with src + TODO      | Section-commit GATE: confirm steps 13-18 of `/implement-todo-section` (Codex + build + validate) ran.                           |
| 8  | Pre: Skill    | implement / review / quality / create / complete-todo-file | Completion-first radar at skill entry (correctness, completeness, wiring, parity, superiority, ownership; for complete-todo-file: no PASS without execution). |
| 11 | Post: Bash    | codex adversarial-review          | Apply `superpowers:receiving-code-review` to every finding (verify at file:line, Fix/Reject/Accept, never blind-implement).     |
| 12 | Post: Edit    | `todo/*.md`                       | If structural edit, run `/validate-todo-file`.                                                                                  |
| 13 | Post: Edit    | `todo/*.md`                       | TODO format CHECK (oversize C-blocks, `(N.M Title)` prefixes). validate-todo-file step 15.                                      |
| 14 | Post: Edit    | `test_*.c`                        | Test wiring: every test fn must call `test_suite_register_cat()` and use an existing `TEST_CAT_*`.                              |
| 15 | Post: Edit    | `test_*.c`                        | Test message uniqueness: `TEST_ASSERT` with literal msg in `for`/`while` must use per-iteration `snprintf`.                     |
| 16 | Post: Edit    | `test_*.c`                        | `TEST_PENDING` REMINDER: tests asserting `STATUS_NOT_IMPLEMENTED` as expected must use `TEST_PENDING`, not `TEST_ASSERT`.       |
| 17 | Post: Edit    | `skills/*.md`                     | CLAUDE.md sync: new/renamed skill needs rows in CLAUDE.md Skills table AND `.claude/skills/README.md`.                          |
| 18 | Post: Edit    | `todo/*.md`                       | Scope-gap dedup: before filing new section/TODO, grep existing TODOs for overlap (scope-gap Branches C/D).                      |
| 19 | Post: Edit    | `todo/*.md`                       | Accepted-XREF concreteness: new `Accepted:`/`Deferred:` must carry `(item: "..." at line N)`. Counterpart to hook 6 BLOCK.     |

### POST-HOC VALIDATE hooks (2)

| #  | Trigger       | Filter                            | Validator                                                                                                                       |
| -- | ------------- | --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| 9  | Post: Bash    | `git commit`; HEAD files (code)   | Runs `scripts/test.sh QUIET=1`; reports pass/fail. [`CLAUDE.md`](../../CLAUDE.md#testing----category-based-test-infrastructure). |
| 10 | Post: Bash    | `git commit`; HEAD files (boot)   | Runs `scripts/test-smoke.sh`; reports `SMOKE TEST PASSED` or fail. [`CLAUDE.md`](../../CLAUDE.md#smoke-test----end-to-end-boot-validation). |

### Reading the matrix

- **"Did my edit fail because of a hook?"** Check the BLOCK table first -- those are the only hooks that can reject a tool call. REMIND hooks never fail an edit; they attach a message.
- **"What happens when I commit kernel code?"** Hook 7 reminds about the section-commit GATE at PreToolUse (pre-commit text), hook 6 may BLOCK if your TODO has bare XREFs, and if the commit succeeds hooks 9 + (conditionally) 10 run tests and smoke.
- **"Did I add a new skill correctly?"** Hook 17 reminds you to sync the CLAUDE.md Skills table; the authoring lifecycle in [skill-authoring.md](skill-authoring.md) has the full 5-step process.

### Editing hooks

`.claude/settings.json` is still the canonical source. When adding or removing a hook:

1. Edit `.claude/settings.json`.
2. Update the relevant table above in the same commit (row add / remove / modify). The matrix and the JSON must stay in sync -- the [AI Workflow Regression Suite](../../todo/00-infrastructure/TODO-02-ai-development-system.md#9-ai-workflow-regression-suite) asserts this invariant.
3. If the hook BLOCKS, document the exact failure signature (tag line + exit code) so a reader can recognize a block-fail in their terminal.

### Hook Promotion Pipeline

Some `implement-todo-section` steps are inherently judgment-driven (e.g. step 6 implementation, step 14 table reconciliation) and have no usable enforcement signal. Others have a useful tool-call shape that USUALLY proves the step ran but a measurable false-positive rate -- WARN-first heuristics fit there. The promotion path:

1. **Ship as WARN-only.** The hook detects a heuristic miss, emits a `[heuristic-step-N] WARN -- ...` line to stderr, AND appends one structured JSON line to `.claude/state/heuristic-misses.jsonl` via the shared [`_heuristic_misses.emit_warn`](../../.claude/hooks/_heuristic_misses.py) helper. The hook MUST NOT exit 2; the user / agent gets the warning, the workflow continues.
2. **Observe.** `bash scripts/heuristic-misses-report.sh` summarizes the log per step (count, false-positive ratio over the last N entries). False-positive flagging is retroactive: `bash scripts/heuristic-mark-false-positive.sh <step> [<n>]` rewrites the Nth-most-recent entry's `false_positive_user_flagged: true` field after the user / agent confirms the WARN was wrong.
3. **Promote to ERROR (BLOCK)** when the per-heuristic graduation criterion is met. Documented criteria for the heuristics shipped today:
   - **Step 1** (todo Read before first src/ Edit): `<5% FP over 20 sections`.
   - **Step 2** (XREF target reads): `<5% FP over 20 sections`.
   - **Step 3** (>=3 explore queries before first Edit): `<10% FP over 30 sections`. Counts `Grep`, `Glob`, `mcp__lsp-bridge__*`, `mcp__todo-graph__*` per [CLAUDE.md "MCP-first defaults"](../../CLAUDE.md#mcp-usage).
   - **Step 4** (Phase 1 evidence-gate `>=2 Read/Grep on src/include/.claude/scripts/docs` before adversarial Codex): `<5% FP over 30 sections`. Lower FP tolerance because Phase 1 is foundational -- if the evidence gate misfires the rest of the review pipeline runs blind. Already shipped as a hard BLOCK in [`phase1_evidence_gate.py`](../../.claude/hooks/phase1_evidence_gate.py); the WARN is additive (BLOCK+WARN), the miss-log is the tuning surface for `PHASE1_MIN_READS` rather than a promotion path.
   - **Step 9** (Codex test-coverage on test-touching commits): `<10% FP over 20 sections`.
   - **Step 15** (Edit between latest Codex and section commit): `<10% FP over 20 sections`.
   - **Step 17** (>=2 Read/Grep between latest Codex and commit): `<10% FP over 20 sections`.
   - **Step 18** (TODO/FIXME/HACK/STATUS_NOT_IMPLEMENTED Grep before commit): `<10% FP over 20 sections`.

The promotion itself is a follow-up TODO -- once a heuristic clears its criterion, file an item to flip its `emit_warn` to a hard `sys.stderr.write` + `sys.exit(2)` BLOCK with the same documented opt-out shape (`SKIP_<NAME>_GATE=1` + reason).

The doctrine match is intentional: §3 of TODO-08 walks "advisory hook reminders → hard gates where critical steps keep getting skipped" for the legacy reminder set; this pipeline is the same shape applied to NEW gates that need an empirical false-positive rate before shipping as ERROR.

---

## External-Reviewer Contract (Codex)

Codex participates in the AI workflow as the **sole adversarial reviewer**, not an authoritative instruction layer. The hierarchy invariant #3 ("External reviewers return findings, never edits") pins the rule; this section names the contract concretely so a contributor or a new reviewer tool can be wired without guessing. The previous Copilot CLI subordinate-reviewer wrapper (`scripts/copilot-review.sh` + `.github/copilot-instructions.md`) was retired wholesale 2026-04-28; Codex GPT-5.5 is now the only external reviewer the repo wires to a Claude skill.

### Reviewer, not authority (the invariant)

- **Codex** (OpenAI plugin) returns adversarial findings to Claude. Claude reads the findings, applies [`superpowers:receiving-code-review`](https://github.com/anthropics/superpowers) discipline to each one (verify at file:line, classify Fix / Reject / Accept, never blind-implement), and decides what ships. Codex never edits repo state directly.
- Codex does not edit doctrine. It is not documented as "authority" anywhere in the repo. If a future config file surfaces contradicting this, the other layer is the bug ([Authority Hierarchy invariant #4](#hierarchy-invariants)).

### Codex dispatch surface

Codex is invoked from inside Claude skills via the OpenAI Codex plugin binary at `~/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs`. Four surfaces: nine **angle-owner** `codex-*` skills that each own one review angle, nine **workflow-consumer** skills that dispatch Codex directly as part of a larger pipeline (e.g. step 13 of `/implement-todo-section`), one **inheritor** (`verify-todo-section`) that inherits Codex dispatches through `review-todo-section` without calling `codex-companion.mjs` itself, and four **indirect workflow consumers** (`complete-todo-file`, `todo-pipeline`, `overnight-sequencer`, `overnight-todo-runner`) that invoke Codex through required child skills instead of directly calling the wrapper. Total: 9 + 9 + 1 + 4 = 23 skills carry the External-Reviewer Contract pointer. (The legacy `codex-dead-code-review` skill was retired 2026-04-25 per the rationale in `review-todo-section/SKILL.md` retirement note.)

**Angle-owner skills (9):**

| Skill                                                                        | Adversarial angle                                                       |
| ---------------------------------------------------------------------------- | ----------------------------------------------------------------------- |
| [`codex-design-review`](../../.claude/skills/codex-design-review/)           | Pre-impl plan review: feasibility, edge cases, SMP hazards, arch.       |
| [`codex-adversarial-review-section`](../../.claude/skills/codex-adversarial-review-section/) | Single-section adversarial loop (<=3 rounds) until no unresolved H/C. |
| [`codex-review-todo`](../../.claude/skills/codex-review-todo/)               | Adversarial review across all implemented sections of a TODO.           |
| [`codex-fix-review`](../../.claude/skills/codex-fix-review/)                 | Fix findings and re-run until clean (<=3 iterations).                   |
| [`codex-test-coverage`](../../.claude/skills/codex-test-coverage/)           | Untested error paths, missing boundaries, uncovered branches.           |
| [`codex-impact-analysis`](../../.claude/skills/codex-impact-analysis/)       | What breaks if you change function X / struct Y / constant Z?           |
| [`codex-consistency-audit`](../../.claude/skills/codex-consistency-audit/)   | Struct offsets, constants, API contracts, registration tables.          |
| [`codex-perf-review`](../../.claude/skills/codex-perf-review/)               | ISR paths, spinlock hold times, alloc-in-loop, O(n^2) algorithms.       |
| [`codex-gap-audit`](../../.claude/skills/codex-gap-audit/)                   | Gap-analysis red-team: missing inventory, stale XREFs, false completeness. |

**Workflow-consumer skills that dispatch Codex directly (9) + inheritor (1) + indirect workflow consumers (4):**

| Skill                                                                                        | Why it dispatches Codex                                                              |
| -------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------ |
| [`implement-todo-section`](../../.claude/skills/implement-todo-section/)                    | Step 13 adversarial review, step 20 quality dispatch (consistency + perf). |
| [`implement-todo-item`](../../.claude/skills/implement-todo-item/)                          | Default mandatory item adversarial review; section-close path inherits review-todo-section obligations. |
| [`gap-audit-todo`](../../.claude/skills/gap-audit-todo/)                                    | Phase 3.5 mandatory gap-audit red-team before TODO edits are accepted.      |
| [`review-todo-section`](../../.claude/skills/review-todo-section/)                          | Phase 2 mandatory adversarial, Phase 3 mandatory quality.                            |
| [`quality-review-section`](../../.claude/skills/quality-review-section/)                    | Deep quality review dispatch (standards / optimization angles).                      |
| [`verify-todo-section`](../../.claude/skills/verify-todo-section/)                          | Inherits review-todo-section's pipeline under audit-mode overrides.                  |
| [`implement-unit-tests`](../../.claude/skills/implement-unit-tests/)                        | Step 7 test coverage gap analysis.                                                   |
| [`implement-ssdt-range`](../../.claude/skills/implement-ssdt-range/)                        | Per-entry adversarial review during SSDT range implementation.                       |
| [`debug-session`](../../.claude/skills/debug-session/)                                      | Hypothesis validation dispatch before fixing.                                        |
| [`diagnose-serial-log`](../../.claude/skills/diagnose-serial-log/)                          | Codex adversarial review on every fix per step 21.                                   |
| [`complete-todo-file`](../../.claude/skills/complete-todo-file/)                            | Indirect full-close-out path invokes implement-unit-tests, which owns the test-coverage dispatch. |
| [`todo-pipeline`](../../.claude/skills/todo-pipeline/)                                      | Indirect Stage 2 path invokes gap-audit-todo, which owns the mandatory gap-audit dispatch. |
| [`overnight-sequencer`](../../.claude/skills/overnight-sequencer/)                          | Indirect unattended phase machine invokes gap-audit, section/item implementation, review, and file close-out child skills. |
| [`overnight-todo-runner`](../../.claude/skills/overnight-todo-runner/)                      | Indirect legacy overnight runner invokes section implementation/review and file close-out child skills. |

Every skill in both tables carries the `> **External-Reviewer Contract:**` pointer blockquote at the top of its SKILL.md, so a maintainer can one-link-trace to this section from any reviewer entry point.

### Adding a new reviewer tool

If a future AI tool (Aider, Continue, Gemini-CLI, etc.) joins the reviewer set, it goes through the same contract:

1. **No parallel instruction tree.** The tool does not get its own `.cursor/` / `.codex/` / `.<tool>/` skill directory ([Authority Hierarchy invariant #2](#hierarchy-invariants)).
2. **Invocation from inside a Claude skill.** The tool is dispatched from a new `tool-*` skill under `.claude/skills/`, or from a shell script under `scripts/` that follows the External-Reviewer Contract (returns findings as JSON or markdown text, never edits repo state).
3. **Output through `receiving-code-review`.** Every finding is verified at file:line by Claude before any edit. Same Fix / Reject / Accept classification.
4. **Subordinate-reviewer row added to the Authority Hierarchy table.** `docs/infrastructure/ai-system.md` table grows a row; the tool's role is pinned as "Subordinate reviewer", not "authority".
5. **Roadmap ownership.** The adoption ships as a new section in [TODO-02](../../todo/00-infrastructure/TODO-02-ai-development-system.md) with the same review pipeline.

If a proposal does NOT fit this contract (e.g. a tool that wants to commit directly), reject it at the roadmap stage. The [Autonomous-Agent Boundary Policy](../../todo/00-infrastructure/TODO-02-ai-development-system.md#8-autonomous-agent-boundary-policy) owns that refusal explicitly.

### Discoverability back from skills

Every `codex-*` skill under `.claude/skills/` carries a one-line pointer back to this contract section (see each skill's "External-Reviewer Contract" note near the top). A maintainer reading `codex-consistency-audit/SKILL.md` can trace the "reviewer, not authority" rule back to this section in one link.

### Plugin skill catalog (superpowers)

The `superpowers@claude-plugins-official` plugin ships 14 skills, three of which are MANDATORY in this repo (`receiving-code-review`, `verification-before-completion`, `systematic-debugging`) and one of which carries a hard scope distinction (`subagent-driven-development` is FORBIDDEN as a substitute for the Codex four-dispatch on `src/kernel/`, `src/boot/`, `include/kernel/`). Per-skill verdicts, doctrine-conflict suppression rules, and the kernel/boot scope distinction live in [`docs/infrastructure/superpowers-policy.md`](superpowers-policy.md). CLAUDE.md "Mandatory Skill Triggers" carries the agent-facing prompt rows; the policy doc carries the rationale.

### AI-Slop Content Gates

Build green and tests green do not catch the most common AI-coauthor failure modes; CodeRabbit's December 2025 PR study reports AI-coauthored changes ship roughly 1.7x more major issues than human-authored code, and the leak is content-level rather than compile-level. [`scripts/lint.sh`](../../scripts/lint.sh) Checks 6 and 7 target two AI-slop patterns CLAUDE.md prohibits in prose. Both are live ERROR gates:

| Check | Pattern | Marker (allowlist) | Skip env |
|---|---|---|---|
| 6: Tautological-test | `TEST_ASSERT_EQ(X, X, ...)`, `TEST_ASSERT(true, ...)`, `TEST_ASSERT(1, ...)` in `src/kernel/test/test_*.c` | `/* TEST-TAUTOLOGY-OK: <reason> */` (same line) | `SKIP_LINT_TAUTOLOGY=1` |
| 7: Stub-behind-stamp | TODO `[x]` item names a function whose body is `<= 3 LOC return-constant`; live via [Per-item stamped_items cache extension](../../todo/00-infrastructure/TODO-06-todo-metadata-layer.md#9-per-item-stamped_items-cache-extension) | `/* INTENTIONAL-STUB: <reason> */` (same line) | `SKIP_LINT_STUB_BEHIND_STAMP=1` |

Pre-existing tautological tests are tracked in a per-check legacy allowlist (mirrors Check 5's `BARE_SECTION_LEGACY_FILES`) and emit WARN, not ERROR; new tautology in any non-allowlisted file is ERROR. Every skip env var emits a visible WARN line so bypasses are auditable. (An originally-planned Check 8 phantom-include lint via lsp-bridge clangd diagnostics was attempted and dropped 2026-05-02 -- false-positive rate too high on this freestanding kernel; rationale lives in the [AI-slop content lints](../../todo/00-infrastructure/TODO-08-automation-hardening.md#12-ai-slop-content-lints-tautological-test--stub-behind-stamp) section history.)

---

## MCP, Permissions, and Extension Boundary

Permissions and extensions are part of the AI surface. They need explicit boundaries so shared policy stays reviewable and personal state never leaks into the repo.

### Permission model (Claude Code harness)

Claude Code distinguishes three rule lists from one separate fallback mode:

| Concept       | Kind           | Meaning                                                                          |
| ------------- | -------------- | -------------------------------------------------------------------------------- |
| `allow`       | Rule list      | Matching tool call proceeds without prompting.                                   |
| `ask`         | Rule list      | Matching tool call prompts for approval each time.                               |
| `deny`        | Rule list      | Matching tool call is always blocked.                                            |
| `defaultMode` | Single setting | Fallback posture when no rule matches. Documented modes: `default`, `acceptEdits`, `plan`, `bypassPermissions`. |

A specific rule in `allow`/`ask`/`deny` always beats `defaultMode`. The three rule lists AND `defaultMode` can each appear in either `.claude/settings.json` (shared) or `.claude/settings.local.json` (user-local); `defaultMode` is only set per-user so posture does not silently change for other contributors.

**Current repo allow-rule shape:** 6 entries in `.claude/settings.json` `permissions.allow`, narrowly scoped (read-only grep patterns, bounded `scripts/test.sh` invocation, scoped `Edit(.claude/skills/<name>/**)`). No shared `ask` or `deny` rules (harness safety defaults cover destructive patterns). Additions go through the same review as any other code change; destructive command families (`git reset`, `rm`, `mv` across tracked files) AND arbitrary-code-execution wrappers (`python3 -c :*`, `bash -c :*`) do NOT belong in shared `allow` -- route them through `ask` so the user confirms intent per call.

### Shared vs user-local split

Two settings files live under `.claude/`. They must never mix.

| File                                                            | Tracked?        | Purpose                                                                                  |
| --------------------------------------------------------------- | --------------- | ---------------------------------------------------------------------------------------- |
| [`.claude/settings.json`](../../.claude/settings.json)          | ✅ committed    | Shared repo policy: hooks, narrow allow-rules, env vars every contributor needs.          |
| `.claude/settings.local.json`                                   | ❌ gitignored   | User-local: personal allow-rules, `defaultMode`, machine-specific paths. Never shared.    |

The `.gitignore` explicitly excludes `.claude/settings.local.json`. A CI check (future work, see [AI Workflow Regression Suite](../../todo/00-infrastructure/TODO-02-ai-development-system.md#9-ai-workflow-regression-suite)) will assert the file stays untracked.

**What belongs where:**

- **Shared (`settings.json`):** hook definitions (`.claude/settings.json` hooks run for every contributor), hook commands, narrow read-only allow-rules (e.g. specific grep patterns, bounded `Bash(TIMEOUT=30 bash scripts/test.sh QUIET=1:*)` invocations, scoped `Edit(.claude/skills/<name>/**)`), environment variables needed by hooks (if any).
- **User-local (`settings.local.json`):** personal allow-rules for commands you use often (broad `Bash(python3:*)` or `Bash(python3 -c :*)`, domain-specific grep patterns), `defaultMode` posture (supported modes: `default`, `acceptEdits`, `plan`, `bypassPermissions`), one-off session-driven permissions, machine-specific paths (`/home/<user>/...`).
- **Never either:** secrets (API tokens, keys). The harness reads those from the environment or from user-level config outside this repo.

### MCP server boundary

MCP (Model Context Protocol) servers come in two categories:

1. **User-local credential-bearing servers** (Ahrefs, Gmail, Notion, Microsoft Learn, etc.) -- live in the user's Claude Code harness config (typically `~/.claude/` or the IDE extension's config pane), NOT in the repo. The repo neither installs nor depends on them. Sharing them through the repo would leak auth tokens and personal data.
2. **Repo-tracked credential-free read-only servers** -- a narrow allowlist of MCP servers that the repo ships AS infrastructure. Currently `todo-graph` (TODO metadata query surface) and `lsp-bridge` ([TODO-07 LSP-MCP bridge](../../todo/00-infrastructure/TODO-07-lsp-mcp-bridge.md)). These DO live in [`.mcp.json`](../../.mcp.json) at repo root.

A repo-tracked MCP server is permitted ONLY when ALL of:

- **Zero credentials.** No auth tokens, API keys, OAuth flows, personal-data store handles. The server proxies only public/local infrastructure that is already visible to anyone with repo read access.
- **Read-only by design.** The server exposes ONLY query/inspection tools. NO method that mutates external state, writes to remote services, runs untrusted code, or applies edits to the workspace. Verified at runtime AND at source-audit time: `lsp-bridge` enforces this via `_FORBIDDEN_LSP_METHODS` runtime gate + `bash scripts/lsp-mcp/tests/test_boundary.sh` source audit.
- **Proxies only public/local infrastructure.** Either local code already in the repo (`todo-graph` reads `todo/`) or public LSP processes invoked locally (`lsp-bridge` spawns clangd / asm-lsp / etc.). No reach-out to network services.

Bullets that still apply to user-local credentialed MCP servers:

- **No repo-tracked credentialed MCP config.** Do not commit credentialed MCP server definitions to `.claude/settings.json`, `.mcp.json`, or any other tracked file. Reason: token leak.
- **Findings through skills.** If a credentialed MCP server is used during development (e.g. fetching Microsoft Learn docs while researching a Win32 API), the finding enters the repo via a Claude skill's output (doc text, citation in a PR) -- not via the MCP server itself being committed.
- **Shared tool surface beyond skills and hooks.** If the repo needs a NEW shared tool surface, prefer the repo-tracked-MCP carve-out (zero creds, read-only, proxies local/public infra) -- write the server under `scripts/<name>/` with a self-test harness + boundary audit, register in `.mcp.json`, document in [`docs/infrastructure/development-tooling.md`](development-tooling.md). Otherwise: write a shell script under `scripts/` that reads from stdin / returns structured output, invoke from a skill.

**Adding a new repo-tracked MCP server (procedural gate).** Growing the `.mcp.json` allowlist beyond the current two entries (`todo-graph`, `lsp-bridge`) requires:
1. **A new TODO file or section** that documents the server's three-criteria proof: zero credentials (cite the auth surface or its absence), read-only by design (cite the equivalent of `_FORBIDDEN_LSP_METHODS` runtime gate + a source-audit harness), and which public/local infrastructure it proxies.
2. **A self-test harness** (host-side or kernel-side) that exits non-zero on regression of any of the three criteria. Add the harness to `scripts/test-tooling.sh` if host-side.
3. **Standard PR review.** The `.mcp.json` change is reviewed alongside the server source + harness, NOT in isolation. A PR that grows `.mcp.json` without the supporting TODO + harness is rejected.

The two existing entries each have a corresponding TODO -- [todo-graph MCP transport](../../todo/00-infrastructure/TODO-06-todo-metadata-layer.md#8-mcp-server-ai-agent-transport-over-the-cache) and [LSP-MCP bridge](../../todo/00-infrastructure/TODO-07-lsp-mcp-bridge.md); both ship harnesses and boundary audits. Any third entry follows the same shape.

**Cross-tool MCP server set (Claude Code + Codex CLI).** The same two repo-tracked servers are wired into both AI clients so reviewer / rescue / task runs from EITHER tool have the same dependency-graph + LSP-grade context:

- **Claude Code side:** [`.mcp.json`](../../.mcp.json) at repo root (auto-loaded by the harness when working under this repo).
- **Codex CLI side:** Codex CLI (through at least 0.144.0) has no per-project equivalent of `.mcp.json`. The expected TOML block content is repo-tracked at [`docs/infrastructure/codex-mcp.config.toml`](codex-mcp.config.toml); the installer at [`scripts/codex-mcp-install.sh`](../../scripts/codex-mcp-install.sh) merges the fixture into the user's `~/.codex/config.toml` (idempotent; `--check` mode is a dry-run). The interactive `codex` TUI then sees both servers and tool calls work normally because the user approves each call manually.
- **Drift detection:** the `mcp_drift` sub-test in [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh) asserts both that the Claude `.mcp.json` and the Codex fixture name the same server set AND that the user's actual `~/.codex/config.toml` matches the fixture key-for-key (delegates to the installer's `--check`). A mismatch fails fast with an actionable message pointing at the installer. On a fresh dev environment with no `~/.codex/config.toml`, the cross-side parity check still runs (against the repo fixture) and the codex-side check skip-with-PASS with a hint to run the installer.
- **Known upstream bug -- ALL non-TUI Codex MCP paths auto-cancel:** Codex CLI 0.118 through at least 0.144.0 (current installed version, re-verified 2026-07-10 with `todo-graph/stats` under `codex exec` on gpt-5.6-sol; previously live-verified 2026-05-01 on 0.128.0 with `todo-graph/stats` + `lsp-bridge/workspace_symbol`) reject every MCP tool call under any **non-interactive** Codex path (`codex exec`, `codex app-server` -- which is what [codex-companion.mjs](https://github.com/openai/codex) and our `/codex-*` review skills use, and presumably any IDE plugin that drives Codex non-interactively) as `"user cancelled MCP tool call"`. The MCP subprocess is never spawned -- the rejection happens client-side because non-interactive modes hit an MCP elicitation / `RequestUserInput` path that has no interactive surface to satisfy. Codex log line that confirms it: `request_user_input is not supported in exec mode`. Live-verified this turn against `codex-companion.mjs adversarial-review` -- both `[codex] Tool todo-graph/ready failed.` and `[codex] Tool lsp-bridge/hover failed.` fired identically. Tracked upstream at [openai/codex#16685](https://github.com/openai/codex/issues/16685); fix in flight at [openai/codex#16632](https://github.com/openai/codex/pull/16632) (skip default approval for custom MCP tools without usable annotations) -- **OPEN, blocked on security team review** as of 2026-04-26. Last-known-good Codex version is 0.116.0, but it hard-rejects `gpt-5.5`. Interactive `codex` TUI may still work (the MCP elicitation has a real interactive surface to satisfy in TUI mode) -- presumed-working but **NOT live-verified** in this repo. Current stance: stay on current CLI (0.144.0 as of 2026-07-10) + the pinned model in [codex-config-policy.toml](codex-config-policy.toml) (gpt-5.6-sol since the 2026-07-09 GPT-5.6 GA); reviews keep completing via shell-tool fallback (`rg` / `nl` / `cat` / `git diff`); MCP context is registered-but-unused until upstream ships the fix.
- **What this means for reviews today:** every Codex review that runs via `/codex-adversarial-review`, `/codex-consistency-audit`, `/codex-perf-review`, `/codex-design-review`, `/codex-test-coverage`, etc. has the MCP tools registered (`codex mcp list` shows both servers `enabled`), but Codex's first MCP-tool attempt auto-cancels and the model falls back to shell tools (`rg -n`, `sed`, `cat`, `git diff`, `nl`) for the rest of the session. Reviews still find + cite code at file:line and raise real findings -- they just don't get the structured `todo-graph backlinks` / `lsp-bridge definition` / `lsp-bridge references` context MCP would have provided. Quality impact is small (Codex was already shell-first for code finding even on the rare 0.125-alpha.3 vscode session that did register MCP tools but never called them). Revisit when [PR #16632](https://github.com/openai/codex/pull/16632) merges.
- **Why no per-workspace wrapper:** an earlier revision shipped `scripts/codex.sh` that injected the fixture into every `codex` invocation via `-c mcp_servers.<name>={...}` overrides. Removed because (a) it does NOT fix the upstream MCP-call rejection above (verified by spawn-shim probe -- subprocess never launches regardless of config source), and (b) it's a band-aid that violates the [no-bandaids principle](../../CLAUDE.md). The fixture + installer pair handles the only path that might still work today (interactive TUI -- still unverified); when upstream ships a fix we re-evaluate without inventing a parallel config-injection mechanism.
- **When upstream PR #16632 ships:** we expect everything to "just work" without code changes on our side. The npm-distributed `@openai/codex` upgrade is a single command; existing `~/.codex/config.toml` (installer-managed) and `.mcp.json` already point Codex + Claude at the same MCP servers; the `mcp_drift` validator keeps both sides honest; the next `codex-companion.mjs adversarial-review` invocation should successfully call MCP instead of auto-cancelling. Trigger the swap by re-running `bash scripts/codex-mcp-install.sh --check` (no-op if no fixture drift) and probing one MCP tool call; flip the `[/]` checklist item in [Codex MCP Wiring + Cross-Config Drift Validator](../../todo/00-infrastructure/TODO-08-automation-hardening.md#1-codex-mcp-wiring--cross-config-drift-validator) to `[x]` once verified. One caveat: even with MCP working, Codex tends to prefer shell tools for code finding (observed across multiple session rollouts in 2026-04). If MCP-tool adoption stays low post-fix, the `/codex-*` skill prompt templates may need a small "prefer `mcp__lsp-bridge__definition` over `rg -n` for symbol lookup" hint -- separate optimization, not a blocker.
- **Canonical TODO:** [Codex MCP Wiring + Cross-Config Drift Validator](../../todo/00-infrastructure/TODO-08-automation-hardening.md#1-codex-mcp-wiring--cross-config-drift-validator). Future cross-tool MCP wiring (third client, fourth server, etc.) extends the same fixture + installer + drift-validator triplet rather than introducing a parallel mechanism.

### Subagent boundary

Claude Code ships a handful of built-in subagents (`claude-code-guide`, `Explore`, `general-purpose`, `Plan`, `statusline-setup`). These are harness-provided and do NOT count as repo-tracked AI surface.

| Subagent            | Role                                                                                                      | Repo stance                                                      |
| ------------------- | --------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------- |
| `claude-code-guide` | Read-only guide for "how do I use Claude Code / the Claude API / the SDK?" questions                      | Not used for repo work. Never edits code.                        |
| `Explore`           | Fast exploration agent (Glob / Grep / Read / WebFetch / WebSearch) for codebase questions                 | Useful for open-ended "how does X work?" queries. Read-only.     |
| `general-purpose`   | Full-tool-access agent for multi-step research or lookups that would otherwise burn parent context        | Used when a task legitimately needs >3 search-rounds in isolation. |
| `Plan`              | Software-architect agent for designing implementation strategies                                          | Overlaps with `codex-design-review`; prefer Codex on kernel code. |
| `statusline-setup`  | One-shot status line configuration helper                                                                 | Cosmetic only.                                                   |

Skills (`.claude/skills/`) are the primary repo-tracked surface for workflows; subagents complement them for exploration and context preservation. Neither bypasses the `receiving-code-review` gate on external-reviewer output.

### Edit-here-not-there mapping for this section

| Concept                              | Canonical edit location                                   | Downstream consumers (do NOT edit here)                                            |
| ------------------------------------ | --------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| Shared allow-rules / hooks           | [`.claude/settings.json`](../../.claude/settings.json)    | [Hook Routing Matrix](#hook-routing-matrix) (update row; JSON stays canonical).    |
| Personal allow-rules / `defaultMode` | `.claude/settings.local.json` (user-local; gitignored)    | Never propagate to shared config.                                                  |
| MCP server config                    | User harness config (outside the repo)                    | Never commit to `.claude/settings.json`.                                           |
| Subagent behavior                    | Claude Code harness (built-in)                            | Do not override in repo; use skills for repo-specific workflows.                   |

---

## Autonomous-Agent Boundary Policy

Autonomous coding agents (Copilot coding-agent, Devin, Cognition, equivalent tools that run tasks in sandboxes and open PRs without per-step human authorship) are **refused** here. Impossible OS accepts commits only from human operators working interactively with Claude Code. This is Authority Hierarchy invariant #5 ("Claude Code remains the only implementation agent. A human operator talks to Claude for normal work. External reviewers return findings only.") enforced as repo policy rather than implied.

### Why

- **Kernel-critical review pipeline.** Every section commit goes through `/implement-todo-section` steps 13-18 or `/review-todo-section` Phases 2-4: mandatory Codex adversarial + quality dispatches, `superpowers:receiving-code-review` discipline on every finding, domain code-quality gates (boot, kernel, desktop, shell, userland), scope-gap protocol, self-review radar. Autonomous agents run their own pipelines that do not follow this one; their output cannot be accepted as final-state code.
- **No-parallel-skill-trees invariant.** Hierarchy invariant #2 says skills live only under `.claude/skills/`. Autonomous agents like Copilot coding-agent read `.github/agents/*.agent.md` or `.github/chatmodes/*.md` as their instruction surface; shipping either would violate the invariant and create a parallel doctrine layer.
- **"Works on QEMU" rejection.** The `feedback_no_substandard_code` rule rejects emulator-only shortcuts and platform-specific workarounds. Autonomous agents produce those patterns by default because they cannot test on bare metal, VirtualBox, or WHPX the way the project requires.

### What this repo deliberately does NOT ship (forbidden-path list)

The following files would enable autonomous-agent workflows; their **absence is policy, not omission**. A future edit that adds any of these paths will fail the [AI Workflow Regression Suite](../../todo/00-infrastructure/TODO-02-ai-development-system.md#9-ai-workflow-regression-suite).

| Forbidden path                                | What it enables in other repos                                                                 |
| --------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| `.github/workflows/copilot-setup-steps.yml`   | Copilot cloud-agent environment setup (dependency install, build, test, network allowlist).   |
| `.github/agents/*.agent.md`                   | Agent-mode profiles (personas, tool allowlists, domain scopes) for Copilot cloud-agent.       |
| `.github/chatmodes/*.md`                      | Copilot Chat custom modes shared across the repo.                                              |
| `.github/instructions/*.instructions.md`      | Per-topic repo instructions consumed by Copilot cloud-agent + IDE-side Copilot.               |
| Copilot cloud-agent firewall allowlist        | Outbound network policy for the autonomous agent's sandbox.                                    |

### Files Copilot cloud-agent MAY read but are NOT autonomous-agent enablement

These paths exist in the repo for other reasons. Their presence does NOT imply acceptance of autonomous-agent PRs. If GitHub-side Copilot cloud-agent is ever enabled (repo-level setting in GitHub UI, not a file), it would consume these inputs in reviewer-mode only; autonomous-PR authorship is still refused under the Autonomous-Agent Boundary Policy.

| Allowed path                          | Purpose here                                                                                 | NOT enablement because...                                                                                  |
| ------------------------------------- | -------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| [`AGENTS.md`](../../AGENTS.md)        | Cross-tool pointer file (Linux Foundation AGENTS.md standard).                               | Explicit "Autonomous-agent stop sign" sub-section tells autonomous agents to stop before opening a PR.     |
| [`CLAUDE.md`](../../CLAUDE.md)        | Doctrine source-of-truth.                                                                    | Read by Claude Code + cross-tool readers; does not authorize autonomous PR creation.                        |

(`.github/copilot-instructions.md` was previously listed here for the retired Copilot-CLI subordinate-reviewer role. The file was deleted 2026-04-28 with the Copilot-CLI removal sweep; cross-tool readers now have only `AGENTS.md` + `CLAUDE.md` as repo-level pointer surfaces.)

### GitHub-side enablement note (regression pack cannot detect)

Copilot cloud-agent can be enabled at the **repository or organization level in the GitHub UI** (Settings -> Copilot -> Access policies, or equivalent). That enablement is NOT a file in the repo; the regression suite cannot detect it. Enforcement is procedural: the project owner (`rizonesoft/impossible-os` org admin) keeps cloud-agent access policies set to "disabled for this repo" and audits the Settings pane whenever the org-wide Copilot configuration changes. A cloud-agent-authored PR appearing on the repo despite this policy is a process bug, not a repo bug; close the PR with a citation to this section and re-check the Settings pane.

### MCP-server corollary

MCP servers that can autonomously commit, push, create PRs, or execute long-running tasks without per-step human approval are **forbidden** from `.claude/settings.json` and from any user-local config used against this repo. They are the same autonomous-agent pattern in a different wrapper.

Read-only MCP servers (filesystem read, git read, GitHub read, docs search, Microsoft Learn) are fine -- they return findings that Claude reads under `receiving-code-review` discipline, same as Codex. See [MCP server boundary](#mcp-server-boundary) in §5 for the full shared-vs-user-local split.

### Stance-change condition

If Impossible OS ever opts in to autonomous-agent support, adoption ships as a **new top-level TODO** with:

- Its own review pipeline (equivalent to `/implement-todo-section` steps 13-18 running inside the autonomous-agent sandbox before a PR is opened).
- A `.github/workflows/copilot-setup-steps.yml` (or equivalent) that wires the agent to the domain code-quality gates and mandatory Codex dispatches.
- An explicit firewall allowlist for the agent's network access.
- A revised [Authority Hierarchy](#authority-hierarchy-read-this-first) row acknowledging the new autonomous class (Claude Code stays primary interactive orchestrator; the autonomous agent would be a subordinate contributor, not an authority).

Until that TODO ships, every autonomous-agent-authored PR fails review. Any reviewer can cite this section and close the PR with the refusal reason.

### Why this is a competitive edge

Mature Windows and Linux repos document whether they accept autonomous-agent PRs; **fewer document WHY and what they refuse to ship as a consequence**. Making the refusal explicit (and linking it to the Authority Hierarchy) keeps the Claude-primary boundary enforceable long-term rather than degrading silently one reviewer-accepts-a-PR at a time.

---

## See Also

- [AI Development System roadmap](../../todo/00-infrastructure/TODO-02-ai-development-system.md) -- roadmap ownership, [Skill Lifecycle, Templates, and Catalog Rules](../../todo/00-infrastructure/TODO-02-ai-development-system.md#2-skill-lifecycle-templates-and-catalog-rules), [Hook Routing and Policy Contract](../../todo/00-infrastructure/TODO-02-ai-development-system.md#3-hook-routing-and-policy-contract), [External-Reviewer Contract](../../todo/00-infrastructure/TODO-02-ai-development-system.md#4-external-reviewer-contract-codex), [MCP, Permissions, and Extension Boundary](../../todo/00-infrastructure/TODO-02-ai-development-system.md#5-mcp-permissions-and-extension-boundary), [`AGENTS.md` Cross-Tool Pointer File](../../todo/00-infrastructure/TODO-02-ai-development-system.md#6-agentsmd-cross-tool-pointer-file), [AI-Assist Commit Disclosure Policy](../../todo/00-infrastructure/TODO-02-ai-development-system.md#7-ai-assist-commit-disclosure-policy), [Autonomous-Agent Boundary Policy](../../todo/00-infrastructure/TODO-02-ai-development-system.md#8-autonomous-agent-boundary-policy), and [AI Workflow Regression Suite](../../todo/00-infrastructure/TODO-02-ai-development-system.md#9-ai-workflow-regression-suite).
- [Skill Authoring Lifecycle](skill-authoring.md) -- how to add, edit, or retire a skill; canonical SKILL.md template; catalog hygiene sync rules.
- [Git Hooks and Local Automation Lifecycle](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle) -- `.githooks/` are a separate layer from Claude Code harness hooks.
- [Development Tooling](development-tooling.md) -- build system, test framework, host bootstrap contract. Complements this document: development-tooling.md owns the CI/build surface; ai-system.md owns the AI surface.
- [`CLAUDE.md`](../../CLAUDE.md) -- the doctrine itself. This document is an index into CLAUDE.md, not a replacement.
