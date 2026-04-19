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
- `code-commit stdout` / `boot-commit stdout` = see [Known limitations](#known-limitations-post-hoc-validators) below.

### BLOCK hooks (4)

| #  | Trigger       | Filter                            | Rule                                                                                                                            |
| -- | ------------- | --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| 2  | Pre: Edit     | any content                       | Reject em/en dash (U+2014, U+2013). [`CLAUDE.md`](../../CLAUDE.md#no-unicode-dashes-enem-ascii-only).                           |
| 3  | Pre: Edit     | C-src                             | Reject new `TODO`/`FIXME`/`HACK`/stub markers without scope-gap resolution (see protocol under implement-todo-section/). |
| 4  | Pre: Edit     | `test_*.c`                        | Reject live boot-infra calls (`boot_progress`, `vpd_*`, `panic`, subsystem `_init`). See CLAUDE.md "Test Code".            |
| 5  | Pre: Bash     | `git commit` with staged `todo/`  | Reject bare `Accepted:`/`Deferred:` XREF. See [review step 15](../../.claude/skills/review-todo-section/SKILL.md). Soft XREFs = WARN. |

### REMIND hooks (12)

| #  | Trigger       | Filter                            | Reminder                                                                                                                        |
| -- | ------------- | --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| 1  | Pre: Edit     | C-src                             | Auto-load matching domain code-quality skill (boot / kernel / desktop / shell / userland).                                      |
| 6  | Pre: Bash     | `git commit` with src + TODO      | Section-commit GATE: confirm steps 13-18 of `/implement-todo-section` (Codex + build + validate) ran.                           |
| 7  | Pre: Skill    | implement/review/quality/create   | Completion-first radar at skill entry (correctness, completeness, wiring, parity, superiority, ownership).                      |
| 10 | Post: Bash    | copilot-review.sh / codex advers. | Apply `superpowers:receiving-code-review` to every finding (verify at file:line, Fix/Reject/Accept, never blind-implement).     |
| 11 | Post: Edit    | `todo/*.md`                       | If structural edit, run `/validate-todo-file`.                                                                                  |
| 12 | Post: Edit    | `todo/*.md`                       | TODO format CHECK (oversize C-blocks, `(N.M Title)` prefixes). validate-todo-file step 15.                                      |
| 13 | Post: Edit    | `test_*.c`                        | Test wiring: every test fn must call `test_suite_register_cat()` and use an existing `TEST_CAT_*`.                              |
| 14 | Post: Edit    | `test_*.c`                        | Test message uniqueness: `TEST_ASSERT` with literal msg in `for`/`while` must use per-iteration `snprintf`.                     |
| 15 | Post: Edit    | `test_*.c`                        | `TEST_PENDING` REMINDER: tests asserting `STATUS_NOT_IMPLEMENTED` as expected must use `TEST_PENDING`, not `TEST_ASSERT`.       |
| 16 | Post: Edit    | `skills/*.md`                     | CLAUDE.md sync: new/renamed skill needs rows in CLAUDE.md Skills table AND `.claude/skills/README.md`.                          |
| 17 | Post: Edit    | `todo/*.md`                       | Scope-gap dedup: before filing new section/TODO, grep existing TODOs for overlap (scope-gap Branches C/D).                      |
| 18 | Post: Edit    | `todo/*.md`                       | Accepted-XREF concreteness: new `Accepted:`/`Deferred:` must carry `(item: "..." at line N)`. Counterpart to hook 5 BLOCK.     |

### POST-HOC VALIDATE hooks (2)

| #  | Trigger       | Filter                            | Validator                                                                                                                       |
| -- | ------------- | --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| 8  | Post: Bash    | `git commit`; code-commit stdout  | Runs `scripts/test.sh QUIET=1`; reports pass/fail. [`CLAUDE.md`](../../CLAUDE.md#testing----category-based-test-infrastructure). |
| 9  | Post: Bash    | `git commit`; boot-commit stdout  | Runs `scripts/test-smoke.sh`; reports `SMOKE TEST PASSED` or fail. [`CLAUDE.md`](../../CLAUDE.md#smoke-test----end-to-end-boot-validation). |

### Known limitations (post-hoc validators)

**Hooks 8 and 9 match on `git commit` stdout, not the canonical commit file list.** `code-commit stdout` means "the git-commit stdout text contains any of `.c`/`.h`/`.asm`/`.ld`"; `boot-commit stdout` means "the stdout text contains any of the boot-path substrings (`src/boot/`, `src/kernel/main/boot_`, `src/kernel/idt.c`, ...)". Standard `git commit` output includes `create mode .../foo.c` / `delete mode` lines for added and removed files but does NOT list paths for pure modifications. Consequences:

- Commits that only MODIFY existing source files may silently skip hooks 8 and 9 even though the code changed.
- Commits that add a new boot-path source file always fire hook 9 because `create mode` includes the path.
- A tighter matcher (`git show --name-only --format= HEAD`) would fix this; tracked in the [Hook Routing and Policy Contract](../../todo/00-infrastructure/TODO-02-ai-development-system.md#3-hook-routing-and-policy-contract) roadmap section.

### Reading the matrix

- **"Did my edit fail because of a hook?"** Check the BLOCK table first -- those are the only hooks that can reject a tool call. REMIND hooks never fail an edit; they attach a message.
- **"What happens when I commit kernel code?"** Hook 6 reminds about the section-commit GATE at PreToolUse (pre-commit text), hook 5 may BLOCK if your TODO has bare XREFs, and if the commit succeeds hooks 8 + (conditionally) 9 run tests and smoke.
- **"Did I add a new skill correctly?"** Hook 16 reminds you to sync the CLAUDE.md Skills table; the authoring lifecycle in [skill-authoring.md](skill-authoring.md) has the full 5-step process.

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

Codex is invoked from inside Claude skills via the OpenAI Codex plugin binary at `~/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs`. Two surfaces: nine **angle-owner** `codex-*` skills that each own one adversarial angle, and seven **workflow-consumer** skills that dispatch Codex directly as part of a larger pipeline (e.g. step 13 of `/implement-todo-section`).

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
| [`codex-dead-code`](../../.claude/skills/codex-dead-code/)                   | Unreachable functions, unused defines, orphaned types, stale decls.     |
| [`codex-perf-review`](../../.claude/skills/codex-perf-review/)               | ISR paths, spinlock hold times, alloc-in-loop, O(n^2) algorithms.       |

**Workflow-consumer skills (7) that also dispatch Codex directly:**

| Skill                                                                                        | Why it dispatches Codex                                                              |
| -------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------ |
| [`implement-todo-section`](../../.claude/skills/implement-todo-section/)                    | Step 13 adversarial review, step 20 quality dispatch (dead-code + consistency + perf). |
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

If a proposal does NOT fit this contract (e.g. a tool that wants to commit directly), reject it at the roadmap stage. The [Autonomous-Agent Boundary Policy](../../todo/00-infrastructure/TODO-02-ai-development-system.md#8-autonomous-agent-boundary-policy) in TODO-02 §8 owns that refusal explicitly.

### Discoverability back from skills

Every `codex-*` skill under `.claude/skills/` carries a one-line pointer back to this contract section (see each skill's "External-Reviewer Contract" note near the top). A maintainer reading `codex-dead-code/SKILL.md` can trace the "reviewer, not authority" rule back to this section in one link.

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

**Current repo allow-rule shape:** 7 entries in `.claude/settings.json` `permissions.allow`, narrowly scoped. No shared `ask` or `deny` rules (harness safety defaults cover destructive patterns). Additions go through the same review as any other code change; destructive command families (`git reset`, `rm`, `mv` across tracked files) do NOT belong in shared `allow` -- route them through `ask` so the user confirms intent per call.

### Shared vs user-local split

Two settings files live under `.claude/`. They must never mix.

| File                                                            | Tracked?        | Purpose                                                                                  |
| --------------------------------------------------------------- | --------------- | ---------------------------------------------------------------------------------------- |
| [`.claude/settings.json`](../../.claude/settings.json)          | ✅ committed    | Shared repo policy: hooks, narrow allow-rules, env vars every contributor needs.          |
| `.claude/settings.local.json`                                   | ❌ gitignored   | User-local: personal allow-rules, `defaultMode`, machine-specific paths. Never shared.    |

The `.gitignore` explicitly excludes `.claude/settings.local.json`. A CI check (future work, see [TODO-02 §9](../../todo/00-infrastructure/TODO-02-ai-development-system.md#9-ai-workflow-regression-suite)) will assert the file stays untracked.

**What belongs where:**

- **Shared (`settings.json`):** hook definitions (`.claude/settings.json` hooks run for every contributor), hook commands, repo-wide allow-rules for patterns that are universally safe (`Bash(git reset:*)`), environment variables needed by hooks (if any).
- **User-local (`settings.local.json`):** personal allow-rules for commands you use often (`Bash(python3:*)`, domain-specific grep patterns), `defaultMode` posture (`plan` / `permissive` / `strict`), one-off session-driven permissions, machine-specific paths (`/home/<user>/...`).
- **Never either:** secrets (API tokens, keys). The harness reads those from the environment or from user-level config outside this repo.

### MCP server boundary

MCP (Model Context Protocol) servers (Ahrefs, Gmail, Notion, Microsoft Learn, etc.) are **user-local infrastructure**. They live in the user's Claude Code harness config (typically `~/.claude/` or the IDE extension's config pane), not in the repo. The repo neither installs nor depends on them.

- **No repo-tracked MCP config.** Do not commit MCP server definitions to `.claude/settings.json` or any other tracked file. Reason: MCP servers carry auth tokens and personal data; sharing them through the repo would leak credentials.
- **MCP findings through skills.** If an MCP server is used during development (e.g. fetching Microsoft Learn docs while researching a Win32 API), the finding enters the repo via a Claude skill's output (doc text, citation in a PR) -- not via the MCP server itself being committed.
- **If the repo ever needs a shared tool surface beyond skills and hooks**, the extension model is: write a shell script under `scripts/` that reads from standard input / returns structured output, invoke it from a skill. That pattern keeps the surface repo-tracked, reviewable, and independent of any specific MCP server.

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

## See Also

- [AI Development System roadmap](../../todo/00-infrastructure/TODO-02-ai-development-system.md) -- roadmap ownership, [Skill Lifecycle, Templates, and Catalog Rules](../../todo/00-infrastructure/TODO-02-ai-development-system.md#2-skill-lifecycle-templates-and-catalog-rules), [Hook Routing and Policy Contract](../../todo/00-infrastructure/TODO-02-ai-development-system.md#3-hook-routing-and-policy-contract), [External-Reviewer Contract](../../todo/00-infrastructure/TODO-02-ai-development-system.md#4-external-reviewer-contract-codex-copilot), [MCP, Permissions, and Extension Boundary](../../todo/00-infrastructure/TODO-02-ai-development-system.md#5-mcp-permissions-and-extension-boundary), [`AGENTS.md` Cross-Tool Pointer File](../../todo/00-infrastructure/TODO-02-ai-development-system.md#6-agentsmd-cross-tool-pointer-file), [AI-Assist Commit Disclosure Policy](../../todo/00-infrastructure/TODO-02-ai-development-system.md#7-ai-assist-commit-disclosure-policy), [Autonomous-Agent Boundary Policy](../../todo/00-infrastructure/TODO-02-ai-development-system.md#8-autonomous-agent-boundary-policy), and [AI Workflow Regression Suite](../../todo/00-infrastructure/TODO-02-ai-development-system.md#9-ai-workflow-regression-suite).
- [Skill Authoring Lifecycle](skill-authoring.md) -- how to add, edit, or retire a skill; canonical SKILL.md template; catalog hygiene sync rules.
- [Git Hooks and Local Automation Lifecycle](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle) -- `.githooks/` are a separate layer from Claude Code harness hooks.
- [Development Tooling](development-tooling.md) -- build system, test framework, host bootstrap contract. Complements this document: development-tooling.md owns the CI/build surface; ai-system.md owns the AI surface.
- [`CLAUDE.md`](../../CLAUDE.md) -- the doctrine itself. This document is an index into CLAUDE.md, not a replacement.
