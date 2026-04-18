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
| Codex (OpenAI plugin)             | Subordinate reviewer                             | Adversarial findings only; invoked from inside `codex-*` Claude skills; findings go through `receiving-code-review` before action  |
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
- **External reviewers are invoked from inside Claude skills, not from separate instruction layers.** Codex runs through the OpenAI Codex plugin when a `codex-*` skill dispatches it; Copilot runs through `scripts/copilot-review.sh` when Claude asks for a PR-style review. Neither tool reads its own instruction tree in this repo.
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
| Harness policy (hooks, permissions)              | [`.claude/settings.json`](../../.claude/settings.json)                                | Mandatory-Skill-Triggers table in `CLAUDE.md` (update when a hook is added that enforces a new rule)                                             |
| Copilot CLI guidance                             | [`.github/copilot-instructions.md`](../../.github/copilot-instructions.md)            | `scripts/copilot-review.sh` (invocation only); never restate doctrine in the instructions file                                                   |
| Codex dispatch templates                         | Individual `codex-*` skills under [`.claude/skills/`](../../.claude/skills/)          | Codex plugin config (external); never restate doctrine in plugin docs                                                                            |
| Git-hook lifecycle (pre-commit lint, post-commit COUNT, pre-push) | [Git Hooks and Local Automation Lifecycle](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle) + [`.githooks/`](../../.githooks/) | `CONTRIBUTING.md` "Enable Git Hooks" section (copy-pastable install commands only); `CLAUDE.md` "Git Hooks" section (one-line pointer) |
| TODO workflow (validate, gap-analysis, implement, review) | [`.claude/skills/`](../../.claude/skills/) (the individual skill files are the SoT)   | `/todo-pipeline` orchestrator (references the individual skills; never inlines their content)                                                    |

**When in doubt:** if the same fact appears in two places and they drift, the downstream consumer is the bug.

---

## See Also

- [AI Development System roadmap](../../todo/00-infrastructure/TODO-02-ai-development-system.md) -- roadmap ownership, [Skill Lifecycle, Templates, and Catalog Rules](../../todo/00-infrastructure/TODO-02-ai-development-system.md#2-skill-lifecycle-templates-and-catalog-rules), [Hook Routing and Policy Contract](../../todo/00-infrastructure/TODO-02-ai-development-system.md#3-hook-routing-and-policy-contract), [External-Reviewer Contract](../../todo/00-infrastructure/TODO-02-ai-development-system.md#4-external-reviewer-contract-codex-copilot), [MCP, Permissions, and Extension Boundary](../../todo/00-infrastructure/TODO-02-ai-development-system.md#5-mcp-permissions-and-extension-boundary), [`AGENTS.md` Cross-Tool Pointer File](../../todo/00-infrastructure/TODO-02-ai-development-system.md#6-agentsmd-cross-tool-pointer-file), [AI-Assist Commit Disclosure Policy](../../todo/00-infrastructure/TODO-02-ai-development-system.md#7-ai-assist-commit-disclosure-policy), [Autonomous-Agent Boundary Policy](../../todo/00-infrastructure/TODO-02-ai-development-system.md#8-autonomous-agent-boundary-policy), and [AI Workflow Regression Suite](../../todo/00-infrastructure/TODO-02-ai-development-system.md#9-ai-workflow-regression-suite).
- [Skill Authoring Lifecycle](skill-authoring.md) -- how to add, edit, or retire a skill; canonical SKILL.md template; catalog hygiene sync rules.
- [Git Hooks and Local Automation Lifecycle](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle) -- `.githooks/` are a separate layer from Claude Code harness hooks.
- [Development Tooling](development-tooling.md) -- build system, test framework, host bootstrap contract. Complements this document: development-tooling.md owns the CI/build surface; ai-system.md owns the AI surface.
- [`CLAUDE.md`](../../CLAUDE.md) -- the doctrine itself. This document is an index into CLAUDE.md, not a replacement.
