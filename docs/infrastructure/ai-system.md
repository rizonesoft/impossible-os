# AI Development System -- Ownership Map

> Canonical reference for who-owns-what across the Impossible OS AI surface. `CLAUDE.md` links here from its "Skills" section; [`docs/infrastructure/index.md`](index.md) lists this doc so it is discoverable from the Infrastructure landing page. Roadmap ownership lives in the [AI Development System roadmap](../../todo/00-infrastructure/TODO-02-ai-development-system.md).

## Authority Hierarchy (read this first)

> **Claude Code is the master.** Everything else in the AI surface is subordinate: doctrine files tell Claude what to do, skills tell Claude how to do it, external reviewers tell Claude what might be wrong. Nothing outside Claude Code edits code, commits, or makes scope decisions autonomously. When this page uses the term "source of truth" it always identifies WHICH file or tool owns a particular kind of authority, never implies anything is co-equal with Claude Code.

| Layer                             | Role                                             | Authority over                                                                                                                     |
| --------------------------------- | ------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------- |
| **Claude Code (tool)**            | MASTER / orchestrator                            | All code edits, commits, skill invocations, reviewer dispatches                                                                    |
| `CLAUDE.md`                       | Doctrine source-of-truth (file)                  | Product north star, workflow rules, safety constraints, policy                                                                     |
| `.claude/skills/`                 | Workflow source-of-truth (directory)             | How Claude executes a specific task (implement, review, verify, diagnose)                                                          |
| `.claude/settings.json`           | Harness policy source-of-truth (file)            | Permissions, hook reminders, pre/post-tool-use gates                                                                               |
| Codex (OpenAI plugin)             | Subordinate reviewer                             | Adversarial findings only; invoked from inside Claude skills (17 dispatchers: 9 angle-owner `codex-*` + 8 workflow-consumer); findings go through `receiving-code-review` before action |
| Copilot CLI                       | Subordinate reviewer                             | External PR-style review; invoked via `scripts/copilot-review.sh`; same `receiving-code-review` discipline                         |
| `.github/copilot-instructions.md` | Copilot-CLI repo instructions                    | Only configures how Copilot answers when invoked; does NOT add new doctrine                                                        |
| `.githooks/`                      | Git-time guards (distinct layer; see [Git Hooks and Local Automation Lifecycle](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle)) | Pre-commit lint, post-commit COUNT, opt-in pre-push                                                                                |

### Hierarchy invariants

1. **Doctrine lives in `CLAUDE.md`. Nowhere else.** Skill headers, tool instructions, and regression messages reference doctrine but do not redefine it. Edits go to `CLAUDE.md` first, then propagate.
2. **Skills live in `.claude/skills/` only.** No parallel skill trees (`.cursor/`, `.codex/`, `.other-tool/` etc.). External tools that want to participate do so through a Claude skill that dispatches them.
3. **External reviewers return findings, never edits.** Codex and Copilot output is information Claude reads and judges. The commit/edit decision stays with Claude under `superpowers:receiving-code-review` discipline.
4. **CLAUDE.md wins on conflict.** If a skill, a hook message, or an external-tool config contradicts `CLAUDE.md`, `CLAUDE.md` is right and the other layer is the bug. Fix the drift, do not fork the doctrine.
5. **Claude Code is also the interactive agent.** A human operator talks to Claude; Claude dispatches subordinates. Treating Codex or Copilot as a direct-edit or direct-commit tool violates the hierarchy.

---

## Claude Code-Only Stance

As of 2026-04-18, Impossible OS is **Claude Code-only** for AI-assisted development. This is a deliberate design choice, documented here so the decision is discoverable when a contributor wonders why there is no `.cursor/` tree or `.windsurf/` tree or Aider config checked in.

- **No `.cursor/`, no parallel skill sets.** Doctrine lives in `CLAUDE.md`; skills live in `.claude/skills/`. The previous `.cursor/` tree was removed because maintaining a parallel skill set under it created clutter without a corresponding productivity win.
- **External reviewers are invoked from inside Claude skills, not from separate instruction layers.** Codex runs through the OpenAI Codex plugin when any of the 17 dispatching skills ([External-Reviewer Contract](#external-reviewer-contract-codex-copilot) names them: 9 angle-owner `codex-*` + 8 workflow-consumer) invokes it; Copilot runs through `scripts/copilot-review.sh` when Claude asks for a PR-style review. Neither tool reads its own instruction tree in this repo.
- **If a new AI tool is added in the future, it goes through the [External-Reviewer Contract](../../todo/00-infrastructure/TODO-02-ai-development-system.md#4-external-reviewer-contract-codex-copilot)** and applies `receiving-code-review` discipline to its findings. New tools do not get their own instruction tree.

---

## Global Doctrine vs Tool-Local Doctrine

Global doctrine lives in `CLAUDE.md` and binds every layer (Claude, skills, hooks, Codex, Copilot). Tool-local doctrine lives in a specific layer and governs only that layer's mechanics.

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
| `.github/copilot-instructions.md` | Copilot CLI answer style; repo conventions stated for an external reviewer's benefit                          |
| Codex plugin (external)           | Codex-specific review templates; each `codex-*` skill owns the prompt it dispatches, invoking the external OpenAI Codex plugin binary (`codex-companion.mjs` lives under the installed plugin tree, not in this repo) |
| `scripts/copilot-review.sh`       | Copilot CLI invocation shape; PR-style review entry point                                                     |

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
| Copilot CLI guidance                             | [`.github/copilot-instructions.md`](../../.github/copilot-instructions.md)            | `scripts/copilot-review.sh` (invocation only); never restate doctrine in the instructions file                                                   |
| Codex dispatch templates                         | Individual `codex-*` skills under [`.claude/skills/`](../../.claude/skills/)          | Codex plugin config (external); never restate doctrine in plugin docs                                                                            |
| Git-hook lifecycle (pre-commit lint, post-commit COUNT, pre-push) | [Git Hooks and Local Automation Lifecycle](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle) + [`.githooks/`](../../.githooks/) | `CONTRIBUTING.md` "Enable Git Hooks" section (copy-pastable install commands only); `CLAUDE.md` "Git Hooks" section (one-line pointer) |
| TODO workflow (validate, gap-analysis, implement, review) | [`.claude/skills/`](../../.claude/skills/) (the individual skill files are the SoT)   | `/todo-pipeline` orchestrator (references the individual skills; never inlines their content)                                                    |

**When in doubt:** if the same fact appears in two places and they drift, the downstream consumer is the bug.

---

## Hook Routing Matrix

Hooks are part of the AI system, not invisible glue. This matrix is the human-readable view of [`.claude/settings.json`](../../.claude/settings.json); the JSON blob stays canonical but is not the place to read from.

### Two hook layers -- do not confuse them

- **Harness hooks (Claude Code)** -- run at tool-call time (per Edit / Write / Bash / Skill invocation). Config: [`.claude/settings.json`](../../.claude/settings.json). Owned here (§3 of [TODO-02](../../todo/00-infrastructure/TODO-02-ai-development-system.md#3-hook-routing-and-policy-contract)).
- **Git hooks** -- run at commit / push time. Config: [`.githooks/pre-commit`, `post-commit`, `pre-push`](../../.githooks/). Owned by [Git Hooks and Local Automation Lifecycle](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle).
- [`scripts/copilot-review.sh`](../../scripts/copilot-review.sh) has no hook surface -- it is invoked manually or from inside a Claude skill; it is not a hook layer.

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
| 11 | Post: Bash    | copilot-review.sh / codex advers. | Apply `superpowers:receiving-code-review` to every finding (verify at file:line, Fix/Reject/Accept, never blind-implement).     |
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

---

## External-Reviewer Contract (Codex, Copilot)

Codex and Copilot participate in the AI workflow as **adversarial reviewers**, not authoritative instruction layers. The hierarchy invariant #3 ("External reviewers return findings, never edits") pins the rule; this section names the contract concretely so a contributor or a new reviewer tool can be wired without guessing.

### Reviewer, not authority (the invariant)

- **Codex** (OpenAI plugin) returns adversarial findings to Claude. Claude reads the findings, applies [`superpowers:receiving-code-review`](https://github.com/anthropics/superpowers) discipline to each one (verify at file:line, classify Fix / Reject / Accept, never blind-implement), and decides what ships. Codex never edits repo state directly.
- **Copilot CLI** (`scripts/copilot-review.sh`) provides PR-style review for docs and non-kernel code paths. Same `receiving-code-review` gate on output. Copilot is NOT used as a first-class reviewer for kernel-critical changes because it lacks the deep-context read Codex gives.
- Neither tool edits doctrine. Neither tool is documented as "authority" anywhere in the repo. If a future config file surfaces contradicting this, the other layer is the bug ([Authority Hierarchy invariant #4](#hierarchy-invariants)).

### Codex dispatch surface

Codex is invoked from inside Claude skills via the OpenAI Codex plugin binary at `~/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs`. Three surfaces: nine **angle-owner** `codex-*` skills that each own one adversarial angle, seven **workflow-consumer** skills that dispatch Codex directly as part of a larger pipeline (e.g. step 13 of `/implement-todo-section`), and one **inheritor** (`verify-todo-section`) that inherits Codex dispatches through `review-todo-section` without calling `codex-companion.mjs` itself. Total: 9 + 7 + 1 = 17 skills carry the External-Reviewer Contract pointer.

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

**Workflow-consumer skills that dispatch Codex directly (7) + inheritor (1):**

| Skill                                                                                        | Why it dispatches Codex                                                              |
| -------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------ |
| [`implement-todo-section`](../../.claude/skills/implement-todo-section/)                    | Step 13 adversarial review, step 20 quality dispatch (consistency + perf). |
| [`review-todo-section`](../../.claude/skills/review-todo-section/)                          | Phase 2 mandatory adversarial, Phase 3 mandatory quality.                            |
| [`quality-review-section`](../../.claude/skills/quality-review-section/)                    | Deep quality review dispatch (standards / optimization angles).                      |
| [`verify-todo-section`](../../.claude/skills/verify-todo-section/)                          | Inherits review-todo-section's pipeline under audit-mode overrides.                  |
| [`implement-unit-tests`](../../.claude/skills/implement-unit-tests/)                        | Step 7 test coverage gap analysis.                                                   |
| [`implement-ssdt-range`](../../.claude/skills/implement-ssdt-range/)                        | Per-entry adversarial review during SSDT range implementation.                       |
| [`debug-session`](../../.claude/skills/debug-session/)                                      | Hypothesis validation dispatch before fixing.                                        |
| [`diagnose-serial-log`](../../.claude/skills/diagnose-serial-log/)                          | Codex adversarial review on every fix per step 21.                                   |

Every skill in both tables carries the `> **External-Reviewer Contract:**` pointer blockquote at the top of its SKILL.md, so a maintainer can one-link-trace to this section from any reviewer entry point.

### Copilot invocation surface

[`scripts/copilot-review.sh`](../../scripts/copilot-review.sh) is a 33-line wrapper. If GitHub Copilot CLI is installed (`npm install -g @github/copilot`), it runs the prompt non-interactively via `copilot --allow-all -p`; otherwise it prints how to install + falls back to pointing at the Codex plugin path. Usage:

```bash
bash scripts/copilot-review.sh "Full review prompt with file paths and severity rubric"
```

No dedicated Claude skill dispatches Copilot (9 skills dispatch Codex; zero dispatch Copilot). This is deliberate: Copilot runs only when a human explicitly asks for a PR-style second opinion, typically on docs or non-kernel code. Kernel-critical review goes through the `codex-*` skills.

### Adding a new reviewer tool

If a future AI tool (Aider, Continue, Gemini-CLI, etc.) joins the reviewer set, it goes through the same contract:

1. **No parallel instruction tree.** The tool does not get its own `.cursor/` / `.codex/` / `.<tool>/` skill directory ([Authority Hierarchy invariant #2](#hierarchy-invariants)).
2. **Invocation from inside a Claude skill.** The tool is dispatched from a new `tool-*` skill under `.claude/skills/`, or from a shell script under `scripts/` that follows the same contract as `scripts/copilot-review.sh`.
3. **Output through `receiving-code-review`.** Every finding is verified at file:line by Claude before any edit. Same Fix / Reject / Accept classification.
4. **Subordinate-reviewer row added to the Authority Hierarchy table.** `docs/infrastructure/ai-system.md` table grows a row; the tool's role is pinned as "Subordinate reviewer", not "authority".
5. **Roadmap ownership.** The adoption ships as a new section in [TODO-02](../../todo/00-infrastructure/TODO-02-ai-development-system.md) with the same review pipeline.

If a proposal does NOT fit this contract (e.g. a tool that wants to commit directly), reject it at the roadmap stage. The [Autonomous-Agent Boundary Policy](../../todo/00-infrastructure/TODO-02-ai-development-system.md#8-autonomous-agent-boundary-policy) owns that refusal explicitly.

### Discoverability back from skills

Every `codex-*` skill under `.claude/skills/` carries a one-line pointer back to this contract section (see each skill's "External-Reviewer Contract" note near the top). A maintainer reading `codex-consistency-audit/SKILL.md` can trace the "reviewer, not authority" rule back to this section in one link.

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
- **Codex CLI side:** Codex CLI 0.125 has no per-project equivalent of `.mcp.json`. The expected TOML block content is repo-tracked at [`docs/infrastructure/codex-mcp.config.toml`](codex-mcp.config.toml); the installer at [`scripts/codex-mcp-install.sh`](../../scripts/codex-mcp-install.sh) merges the fixture into the user's `~/.codex/config.toml` (idempotent; `--check` mode is a dry-run). The interactive `codex` TUI then sees both servers and tool calls work normally because the user approves each call manually.
- **Drift detection:** the `mcp_drift` sub-test in [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh) asserts both that the Claude `.mcp.json` and the Codex fixture name the same server set AND that the user's actual `~/.codex/config.toml` matches the fixture key-for-key (delegates to the installer's `--check`). A mismatch fails fast with an actionable message pointing at the installer. On a fresh dev environment with no `~/.codex/config.toml`, the cross-side parity check still runs (against the repo fixture) and the codex-side check skip-with-PASS with a hint to run the installer.
- **Known upstream bug -- ALL non-TUI Codex MCP paths auto-cancel:** Codex CLI 0.118 through at least 0.126.0-alpha.3 reject every MCP tool call under any **non-interactive** Codex path (`codex exec`, `codex app-server` -- which is what [codex-companion.mjs](https://github.com/openai/codex) and our `/codex-*` review skills use, and presumably any IDE plugin that drives Codex non-interactively) as `"user cancelled MCP tool call"`. The MCP subprocess is never spawned -- the rejection happens client-side because non-interactive modes hit an MCP elicitation / `RequestUserInput` path that has no interactive surface to satisfy. Codex log line that confirms it: `request_user_input is not supported in exec mode`. Live-verified this turn against `codex-companion.mjs adversarial-review` -- both `[codex] Tool todo-graph/ready failed.` and `[codex] Tool lsp-bridge/hover failed.` fired identically. Tracked upstream at [openai/codex#16685](https://github.com/openai/codex/issues/16685); fix in flight at [openai/codex#16632](https://github.com/openai/codex/pull/16632) (skip default approval for custom MCP tools without usable annotations) -- **OPEN, blocked on security team review** as of 2026-04-26. Last-known-good Codex version is 0.116.0, but it hard-rejects `gpt-5.5`. Interactive `codex` TUI may still work (the MCP elicitation has a real interactive surface to satisfy in TUI mode) -- presumed-working but **NOT live-verified** in this repo. Current stance: stay on 0.125 + gpt-5.5; reviews keep completing via shell-tool fallback (`rg` / `nl` / `cat` / `git diff`); MCP context is registered-but-unused until upstream ships the fix.
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

Autonomous coding agents (Copilot coding-agent, Devin, Cognition, equivalent tools that run tasks in sandboxes and open PRs without per-step human authorship) are **refused** here. Impossible OS accepts commits only from human operators working interactively with Claude Code. This is Authority Hierarchy invariant #5 ("Claude Code is also the interactive agent. A human operator talks to Claude; Claude dispatches subordinates.") enforced as repo policy rather than implied.

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

These paths exist in the repo for other reasons. Their presence does NOT imply acceptance of autonomous-agent PRs. If GitHub-side Copilot cloud-agent is ever enabled (repo-level setting in GitHub UI, not a file), it would consume these inputs in reviewer-mode only; autonomous-PR authorship is still refused under §8.

| Allowed path                          | Purpose here                                                                                 | NOT enablement because...                                                                                  |
| ------------------------------------- | -------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| [`.github/copilot-instructions.md`](../../.github/copilot-instructions.md) | Copilot-CLI reviewer-mode instructions (see Authority Hierarchy table).  | Reviewer contract (§4), not authorship; output goes through `receiving-code-review`.                        |
| [`AGENTS.md`](../../AGENTS.md)        | Cross-tool pointer file (§6); Linux Foundation AGENTS.md standard.                           | Explicit "Autonomous-agent stop sign" sub-section tells autonomous agents to stop before opening a PR.     |
| [`CLAUDE.md`](../../CLAUDE.md)        | Doctrine source-of-truth.                                                                    | Read by Claude Code + cross-tool readers; does not authorize autonomous PR creation.                        |

### GitHub-side enablement note (regression pack cannot detect)

Copilot cloud-agent can be enabled at the **repository or organization level in the GitHub UI** (Settings -> Copilot -> Access policies, or equivalent). That enablement is NOT a file in the repo; the regression suite cannot detect it. Enforcement is procedural: the project owner (`rizonesoft/impossible-os` org admin) keeps cloud-agent access policies set to "disabled for this repo" and audits the Settings pane whenever the org-wide Copilot configuration changes. A cloud-agent-authored PR appearing on the repo despite this policy is a process bug, not a repo bug; close the PR with a citation to this section and re-check the Settings pane.

### MCP-server corollary

MCP servers that can autonomously commit, push, create PRs, or execute long-running tasks without per-step human approval are **forbidden** from `.claude/settings.json` and from any user-local config used against this repo. They are the same autonomous-agent pattern in a different wrapper.

Read-only MCP servers (filesystem read, git read, GitHub read, docs search, Microsoft Learn) are fine -- they return findings that Claude reads under `receiving-code-review` discipline, same as Codex and Copilot. See [MCP server boundary](#mcp-server-boundary) in §5 for the full shared-vs-user-local split.

### Stance-change condition

If Impossible OS ever opts in to autonomous-agent support, adoption ships as a **new top-level TODO** with:

- Its own review pipeline (equivalent to `/implement-todo-section` steps 13-18 running inside the autonomous-agent sandbox before a PR is opened).
- A `.github/workflows/copilot-setup-steps.yml` (or equivalent) that wires the agent to the domain code-quality gates and mandatory Codex dispatches.
- An explicit firewall allowlist for the agent's network access.
- A revised [Authority Hierarchy](#authority-hierarchy-read-this-first) row acknowledging the new autonomous class (Claude Code stays master; the autonomous agent would be a subordinate contributor, not an authority).

Until that TODO ships, every autonomous-agent-authored PR fails review. Any reviewer can cite this section and close the PR with the refusal reason.

### Why this is a competitive edge

Mature Windows and Linux repos document whether they accept autonomous-agent PRs; **fewer document WHY and what they refuse to ship as a consequence**. Making the refusal explicit (and linking it to the Authority Hierarchy) keeps the Claude-Code-only stance enforceable long-term rather than degrading silently one reviewer-accepts-a-PR at a time.

---

## See Also

- [AI Development System roadmap](../../todo/00-infrastructure/TODO-02-ai-development-system.md) -- roadmap ownership, [Skill Lifecycle, Templates, and Catalog Rules](../../todo/00-infrastructure/TODO-02-ai-development-system.md#2-skill-lifecycle-templates-and-catalog-rules), [Hook Routing and Policy Contract](../../todo/00-infrastructure/TODO-02-ai-development-system.md#3-hook-routing-and-policy-contract), [External-Reviewer Contract](../../todo/00-infrastructure/TODO-02-ai-development-system.md#4-external-reviewer-contract-codex-copilot), [MCP, Permissions, and Extension Boundary](../../todo/00-infrastructure/TODO-02-ai-development-system.md#5-mcp-permissions-and-extension-boundary), [`AGENTS.md` Cross-Tool Pointer File](../../todo/00-infrastructure/TODO-02-ai-development-system.md#6-agentsmd-cross-tool-pointer-file), [AI-Assist Commit Disclosure Policy](../../todo/00-infrastructure/TODO-02-ai-development-system.md#7-ai-assist-commit-disclosure-policy), [Autonomous-Agent Boundary Policy](../../todo/00-infrastructure/TODO-02-ai-development-system.md#8-autonomous-agent-boundary-policy), and [AI Workflow Regression Suite](../../todo/00-infrastructure/TODO-02-ai-development-system.md#9-ai-workflow-regression-suite).
- [Skill Authoring Lifecycle](skill-authoring.md) -- how to add, edit, or retire a skill; canonical SKILL.md template; catalog hygiene sync rules.
- [Git Hooks and Local Automation Lifecycle](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle) -- `.githooks/` are a separate layer from Claude Code harness hooks.
- [Development Tooling](development-tooling.md) -- build system, test framework, host bootstrap contract. Complements this document: development-tooling.md owns the CI/build surface; ai-system.md owns the AI surface.
- [`CLAUDE.md`](../../CLAUDE.md) -- the doctrine itself. This document is an index into CLAUDE.md, not a replacement.
