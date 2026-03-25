# TODO-01 - AI Development System

> **Goal:** Make Cursor the primary AI development environment for Impossible OS while keeping Antigravity usable through documented compatibility setup. Move durable agent behavior into project-scoped Cursor rules and skills, keep lasting knowledge in git-tracked docs, and treat Srclight as the only supported MCP accelerator rather than the source of project truth.
>
> This TODO also defines a lean replacement for the old prompt-heavy TODO style. Future TODO files should stay execution-focused and rely on reusable skills or workflows for detailed procedure instead of carrying oversized mutable prompts.

> [!IMPORTANT]
> **Primary model:** Cursor is canonical. The repository stores durable guidance. Antigravity consumes documented local setup and validation notes instead of owning a parallel checked-in rules system.

> [!CAUTION]
> **Local state is not authoritative.** `.srclight/`, Cursor MCP state, and Antigravity local config are machine-local or gitignored. Any convention that matters to future work must also be written into tracked repo files.

## Inputs

- [AGENTS.md](../../AGENTS.md)
- [.agents/rules/intelligence.md](../../.agents/rules/intelligence.md)
- [.agents/workflows/implement-todo.md](../../.agents/workflows/implement-todo.md)
- [.agents/workflows/todo-create.md](../../.agents/workflows/todo-create.md)
- [.githooks/pre-commit](../../.githooks/pre-commit)
- [.githooks/post-commit](../../.githooks/post-commit)
- [docs/infrastructure/development-tooling.md](../../docs/infrastructure/development-tooling.md)
- [TODO-02 Developer Tooling Stack](./TODO-02-developer-tooling-stack.md)
- [Antigravity MCP config](file:///home/derickpayne/.gemini/antigravity/mcp_config.json)
- [Antigravity machine settings](file:///home/derickpayne/.antigravity-server/data/Machine/settings.json)

## Target Outcome

- Cursor project rules live under `.cursor/rules/`.
- Cursor project skills live under `.cursor/skills/`.
- Durable AI guidance is stored in tracked repo files, not only in local IDE or MCP state.
- Local, cloud, and dashboard-managed AI features are documented as execution surfaces, not as canonical project truth.
- TODO files become lean and step-driven instead of prompt-heavy.
- Antigravity remains usable through a documented compatibility path without becoming the canonical system.
- Legacy `.agents/` material is either migrated, reduced to compatibility docs, or retired.

## 1. Canonical Architecture

Define the durable operating model before moving files around.

- [ ] Decide the exact checked-in layout for Cursor rules, Cursor skills, and AI tooling documentation.
- [ ] Define a source-of-truth table covering repo docs, project rules, project skills, optional team or dashboard features, local MCP state, and Antigravity local config.
- [ ] Add a boundary model that separates canonical git-tracked guidance, local developer-machine state, and optional cloud or team-managed surfaces.
- [ ] Record that repo-tracked files such as `AGENTS.md`, `.cursor/rules/`, `.cursor/skills/`, relevant docs, and the `todo/` tree are the only canonical project truth.
- [ ] Record that local state such as user rules, user skills, local MCP auth, machine settings, debug artifacts, and gitignored caches may accelerate work but must never become the only source of project guidance.
- [ ] Decide how cloud agents, Team Rules, Bugbot, and other dashboard-managed features fit the system without letting them become the primary owner of repo truth.
- [ ] Define what information must always be written back to tracked repo files and what may remain machine-local.
- [ ] Define the update path for architecture changes so rules, skills, docs, and compatibility notes stay in sync.
- [ ] Record the decision that Cursor is primary and Antigravity is a documented consumer of repo truth.

## 2. Cursor Project Rules

Move durable constraints into a specific checked-in rule set under `.cursor/rules/`.

### Rule Files Delivered

- [x] `.cursor/rules/safety-build.mdc` — workspace boundaries, build discipline, secrets handling, githooks awareness, headless QEMU
- [x] `.cursor/rules/freestanding-kernel-code.mdc` — `-nostdinc`, `kernel/types.h`, allocator rules, crash debug tools
- [x] `.cursor/rules/bare-metal-assembly.mdc` — NASM x86-64, UEFI-era constraints, no BIOS/VGA/PIC assumptions
- [x] `.cursor/rules/api-surface-direction.mdc` — Win32-native, Windows-style paths, production-grade standard
- [x] `.cursor/rules/doc-sync-discipline.mdc` — update tracked guidance when conventions change
- [x] `.cursor/rules/mcp-usage-discipline.mdc` — Srclight-only baseline, repo truth over MCP state
- [x] `.cursor/rules/todo-markdown-style.mdc` — lean TODO markdown, no hard wraps, compact tables

### Rule Decisions (Locked)

- [x] Legacy migration: `safety.md` → `safety-build.mdc`; `coding.md` → `freestanding-kernel-code.mdc` + `bare-metal-assembly.mdc`; `intelligence.md` → `api-surface-direction.mdc` + `doc-sync-discipline.mdc` + `mcp-usage-discipline.mdc`
- [x] All rules are file-scoped (`alwaysApply: false`); no new always-on rule layer in the first pass
- [x] `.cursor/rules/` is the canonical rule surface; Team Rules are optional overlays only
- [x] Secrets and sensitive-data handling stays in `safety-build.mdc` for the first pass
- [x] `.cursor/hooks.json` is deferred; if used later, advisory-only
- [x] Repo `.githooks/` are optional but supported; rules and skills account for pre-commit lint and post-commit `COUNT.md` amend
- [x] `AGENTS.md` is the human-facing overview; Cursor rules contain concise enforceable guidance only
- [x] `AGENTS.md` updated to keep the production-grade philosophy explicit
- [x] Rule set verified: covers safety, freestanding kernel, assembly, API direction, doc-sync, MCP discipline, and TODO markdown style without overlap
- [ ] Ensure the rule and skill system exposes the tooling contract from [TODO-02](./TODO-02-developer-tooling-stack.md), including `llvm-addr2line-19`

## 3. Cursor Skills And Workflow Equivalents

Replace prompt-heavy TODO procedure with a specific first-pass skill pack under `.cursor/skills/`.

### Skills Delivered

- [x] `.cursor/skills/create-todo/` — create lean TODO files, pick domain and numbering, set up Implementation Order, wire XREFs, update indexes
- [x] `.cursor/skills/implement-todo-section/` — execute one TODO section, resolve XREFs, build and test, update section state when reality differs
- [x] `.cursor/skills/validate-todo-file/` — validate TODO structure, ordering, references, gap-free coverage, adjacent-file continuity
- [x] `.cursor/skills/sync-ai-system/` — sync rules, skills, docs, and Antigravity compatibility notes when shared AI conventions change
- [x] `.cursor/skills/verify-todo-section/` — verify a done or in-progress section against code, build, and test evidence; correct section state conservatively

### Skill Boundary Rules

- `create-todo` must not implement code or reconcile existing checkbox state against the codebase.
- `implement-todo-section` must not create new TODO files, rewrite broader roadmap structure, or perform broad markdown audits beyond the scoped section it is executing.
- `validate-todo-file` must not absorb `verify-todo-section`; it owns file-level gap analysis and adjacent-file continuity checks, but not code-truth verification for supposedly completed sections.
- `sync-ai-system` must not be overloaded with TODO roadmap syncing; it stays focused on AI-facing project guidance.
- `verify-todo-section` must not become the general markdown validator; it is about section checklist truth versus code, build, and test reality.
- `implement-todo-section` and `verify-todo-section` must both correct the scoped section's checklist items and notes when implementation evidence shows the current TODO text is wrong, stale, or overstated.
- Skills should own reusable multi-step workflows; if a flow is lightweight, explicit, and one-shot, prefer a command instead of inflating the skill set.

### Mode And Invocation (Locked)

- [x] Plan mode: mandatory for ambiguous scope, architecture changes, risky operations, or multi-file trade-off work
- [x] Ask mode: default for codebase orientation, TODO gap analysis, and pre-implementation research
- [x] Agent execution: appropriate once scope is bounded, dependencies understood, and verification path is clear
- [x] Debug mode: replaces normal iteration for runtime-only failures, regressions, performance issues, or flaky behavior
- [x] Tool-first debugging: use `llvm-addr2line-19` + LLVM tools before speculating on crashes when symbols are available
- [x] Headless QEMU with serial output is the preferred low-interaction path for autonomous debug-fix loops
- [x] Lightweight explicit workflows deferred to Cursor commands; first-pass keeps reusable logic in skills

### Legacy Workflow Migration

| Workflow                | Decision             | Target                                          | Notes                                 |
| ----------------------- | -------------------- | ----------------------------------------------- | ------------------------------------- |
| `todo-create.md`        | Migrate              | `create-todo`                                   | Primary TODO authoring flow           |
| `todo-table-format.md`  | Absorb, then retire  | `create-todo`, `validate-todo-file`             | Keep markers and compact-table rules  |
| `todo-master-sync.md`   | Absorb, then retire  | `create-todo`, `validate-todo-file`             | Keep index and XREF continuity only   |
| `implement-todo.md`     | Migrate              | `implement-todo-section`                        | Section-scoped implementation         |
| `todo-done-check.md`    | Migrate              | `verify-todo-section`                           | Code-truth and status classification  |
| `docs-verify-todo.md`   | Absorb partially     | `verify-todo-section`                           | Current body is section verification  |
| `todo-validate.md`      | Absorb partially     | `validate-todo-file`                            | Keep structure and gap-analysis only  |
| `build.md`              | Keep as reference    | `implement-todo-section`, `verify-todo-section` | Thin build-evidence reference         |
| `docs-convert-todo.md`  | Future skill or ref  | Future specialized skill                        | Not first-pass core                   |
| `docs-validate.md`      | Keep as reference    | Docs maintenance                                | Not core TODO lifecycle               |
| `add-asset.md`          | Future skill or ref  | Future specialized skill                        | Subsystem-specific workflow           |
| `test-hardware.md`      | Keep as reference    | Hardware testing                                | Manual and user-gated                 |
| `test-fs-fat32.md`      | Keep as reference    | Filesystem testing                              | Subsystem-specific raw `make` path    |
| `specs-create.md`       | Future skill or ref  | Future specialized skill                        | Spec authoring                        |
| `specs-fact-check.md`   | Future skill or ref  | Future specialized skill                        | Spec QA                               |
| `release.md`            | Keep as reference    | Release process                                 | Not core TODO lifecycle               |

### Table Convention (Locked)

- [x] `Implementation Order` table replaces `Phase-by-Phase`; leading `⭐/💎` column shows exact order with dependencies and status
- [x] Wide table detail (XREF, overlap, handoff) moves to adjacent bullet blocks rather than extra columns
- [x] `create-todo` sets up tables, XREFs, and dependency order at creation time; no post-creation formatting pass
- [x] `validate-todo-file` runs gap analysis against the TODO and adjacent related files
- [x] Parent or aggregate TODOs inherit this same convention if introduced later

## 4. Lean TODO Standard

Define a new TODO format that stays readable as the backlog grows.

- [ ] Design a lean TODO template for the new `todo/` tree.
- [ ] Treat leaf TODOs as the default. Only introduce a parent or aggregate TODO when one topic truly needs multiple leaf files or shared verification.
- [ ] Remove the old requirement that every TODO section carry a long mutable prompt.
- [ ] Define the minimum required parts of a TODO file: goal, `Implementation Order`, dependencies, checklist, references, overlap XREFs, verification or exit criteria, and any needed handoff notes.
- [ ] Require every TODO file to be complete within its declared scope so that following it produces a gap-free subsystem slice: no missing implementation steps, no missing dependency links, and no unaccounted gap between this TODO file and the next related TODO file.
- [ ] Require correct implementation order and explicit cross references wherever TODO sections overlap or depend on work defined elsewhere.
- [ ] Define a minimal per-change definition-of-done pattern that records what changed, what was verified, and any known remaining limit or follow-up.
- [ ] Require supposedly complete TODO sections to call out unresolved limitations or deferred work explicitly instead of leaving silent ambiguity.
- [ ] Update TODO creation guidance so it matches the new `00/01/02` domain numbering and local file naming.
- [ ] Ensure future TODOs call out the relevant Cursor skill or workflow pattern instead of embedding all procedure inline.

## 5. Verification And Safety Coverage

Make AI-generated work reviewable, safe, and reproducible.

- [ ] Define a no-secrets rule for prompts, tracked docs, examples, commit messages, MCP config or logs, and agent-generated notes; credentials, tokens, and sensitive local values must never be copied into durable project guidance.
- [ ] Define mandatory human-review triggers for security-sensitive changes, destructive operations, public API or ABI changes, dependency additions, build or release tooling changes, and large refactors.
- [ ] Define stop/ask/escalate behavior for ambiguous requirements, conflicting docs, unexpected repo state, risky operations without approval, or verification that fails or produces contradictory evidence.
- [ ] Define a repeatable verification expectation beyond a single claimed local run whenever feasible, such as build-log proof, scripted test commands, smoke steps, QEMU reproduction notes, or equivalent evidence that another contributor can follow.
- [ ] Define what evidence must accompany a completed task or TODO section, including what changed, what was run, what passed or failed, and any known limitation, remaining risk, or next follow-up.

## 6. Collaboration And Automation Boundaries

Keep advanced Cursor features explicit instead of accidental.

- [ ] Decide whether the first-pass repo contract includes Cursor commands for lightweight explicit workflows, or whether commands remain deferred until the core skill pack settles.
- [x] `.cursor/hooks.json` deferred; hooks may exist only as advisory local optimization; repo workflow must remain correct without them
- [x] Repo `.githooks/` are optional but supported; they remain the commit-time enforcement surface when installed; Cursor hooks stay advisory-only
- [ ] Decide whether the current `post-commit` auto-regeneration of `COUNT.md` with an automatic amend remains acceptable, or whether it should move to a safer explicit command or CI-style workflow.
- [ ] Decide whether worktrees and parallel agents need a repo policy for isolation, build setup, shared state, and conflicting TODO edits.
- [ ] Decide whether Bugbot or PR review automation is part of the supported workflow, and if so how repo-owned review guidance will be tracked.
- [ ] Decide whether cloud agents and automations are part of the supported operating model or intentionally deferred.
- [ ] Record that any adopted commands, hooks, review bots, or cloud and dashboard features remain downstream of tracked repo guidance.

### Cursor Hooks Policy (Locked)

- [x] Hooks are advisory-only; fast, deterministic, idempotent, and safe to miss — repo workflow must remain correct when hooks are absent
- [x] Supported first-pass events: `sessionStart` (load project context), `afterFileEdit` (cheap reminders), `beforeShellExecution` (warn on dangerous commands)
- [x] Hooks must never silently rewrite TODOs, commit, push, or mutate canonical docs
- [x] Cursor hooks guide and preflight local actions; repo `.githooks/` remain the only supported commit-time enforcement
- [x] Hook configuration must be shareable without machine-local secrets, credentials, or absolute paths

## 7. MCP Policy

Keep MCP powerful, but subordinate it to tracked project truth.

> [!NOTE]
> **Current Antigravity MCP baseline:**
> - `srclight` enabled via `srclight serve --transport stdio --workspace dev-workspace`
>
> Supported first-pass MCP model: Srclight only. Drop `memory` and `filesystem` from the intended baseline unless a later TODO explicitly reintroduces them with a strong justification.
> Treat this as the current-state compatibility baseline and validation target, not as the canonical source of project truth.

- [x] Srclight is the preferred code intelligence path; fall back to `rg`, direct file reads, and repo-grounded tools when unavailable or stale
- [x] First-pass supported MCP set is Srclight only; Memory and filesystem MCP are out of scope unless re-approved
- [ ] Decide how project-level MCP config, user-level Cursor MCP config, and team or cloud-managed MCP setup relate
- [x] Srclight-only baseline captured in portable documented form; no machine-specific absolute paths made normative
- [x] Stale Memory and filesystem MCP references removed from intended model; documented as deprecated local config only
- [x] `clangd` is LSP-only and must not be configured as an MCP server
- [ ] Add a validation checklist for local MCP setup so Cursor and Antigravity can be verified against the intended model

### Srclight Index Lifecycle

- [ ] Document that `.srclight/` is a disposable local index and cache, not canonical project data, and must never become the only source of truth for architecture, workflow, or completion decisions.
- [ ] Define when the local Srclight index should be refreshed or rebuilt, such as after large renames, branch or worktree switches, tooling or dependency changes, or repeated evidence that search results no longer match tracked files.
- [ ] Define a lightweight validation checklist for a healthy Srclight index, such as confirming that known symbols, current file paths, and recently changed code can be found accurately from Cursor and Antigravity.
- [ ] Document stale-index symptoms, including missing files, outdated paths, deleted symbols still appearing, incorrect implementations being returned, or repeated disagreement between Srclight results and `rg` or direct file reads.
- [ ] Define the recovery path when the index appears stale: stop trusting the bad result, fall back to repo-grounded tools, refresh or rebuild the local index, restart the MCP session if needed, and rerun the validation checklist.
- [ ] Decide whether any repo-facing command or documented local script should exist to standardize Srclight refresh or health checks without making the generated index itself a tracked artifact.
- [ ] Require agents to treat Srclight as a performance aid rather than an authority: when results conflict with tracked files, trust the repo, and if recurring gaps are found update the AI-system guidance instead of normalizing stale-index behavior.

## 8. Antigravity Compatibility

Keep Antigravity usable without letting it drift into a second truth system.

> [!IMPORTANT]
> Antigravity should be judged by functional compatibility with the Cursor-primary model, not by byte-for-byte config mirroring. Local JSON examples may contain machine-specific paths and must remain reference material rather than canonical repo truth.

- [ ] Audit the existing Antigravity local setup against the new Cursor-primary model.
- [ ] Treat the current Antigravity MCP JSON as the baseline example and validation target, not as the authoritative system definition.
- [ ] Define the minimum Antigravity MCP and settings configuration required for practical parity: `srclight` on, with `memory` and `filesystem` removed from the intended baseline unless a later decision explicitly restores them.
- [ ] Document manual compatibility steps without making Antigravity the canonical owner of rules or workflows.
- [ ] Ensure Antigravity guidance points back to tracked repo docs for project truth.
- [ ] Remove or rewrite stale Antigravity-specific guidance that conflicts with the new model.

## 9. Legacy Cleanup And Sync Discipline

Reduce duplication and make future updates obvious.

- [ ] Audit the existing `.agents/rules/` and `.agents/workflows/` material.
- [ ] Decide what migrates into Cursor, what remains as plain documentation, what should be retired, and what should be absorbed into another skill instead of remaining a standalone workflow.
- [ ] Update repo docs that currently describe superseded AI setup, outdated TODO procedure, the old phase-based formatting pass, or a baseline master/sub-file TODO model that no longer applies.
- [ ] Define a simple sync checklist to use whenever architecture, naming, build flow, or AI tooling conventions change.
- [ ] Ensure there is one obvious maintenance path instead of multiple competing update surfaces.

## 10. Final Verification

Verify that the system works as a real development environment instead of a paper design.

- [x] Cursor has a project-native rules and skills layer in the repo
- [ ] Boundary model clearly distinguishes canonical repo truth from local, cloud, and dashboard-managed state
- [x] Mode guidance exists for Plan, Ask, Agent, and Debug
- [ ] Secrets and sensitive-data handling are explicit and compatible with MCP, local tooling, and tracked docs
- [ ] High-risk AI-assisted work has mandatory human-review gates and stop-or-escalate behavior
- [ ] New TODO format is usable without giant prompt sections or a separate formatting-only cleanup pass
- [x] No first-pass skill assumes a baseline master/sub-file TODO architecture
- [x] `validate-todo-file` owns cross-file continuity and handoff checks
- [x] `implement-todo-section` and `verify-todo-section` both correct section checklist state when evidence conflicts
- [ ] New TODO format captures exact implementation order, dependencies, and gap-free handoffs across related files
- [x] Repo `.githooks/` behavior is explicitly supported and documented as optional
- [x] Supported commit workflow accounts for staged-file lint failures and `COUNT.md` auto-generation or amend
- [ ] Advanced Cursor features (commands, hooks, worktrees, Bugbot, cloud agents) are either explicitly deferred or covered by repo-owned guidance
- [x] Adopted Cursor hooks policy improves safety or speed without making hidden local automation canonical
- [ ] Repo docs and local configs do not contradict each other
- [ ] Antigravity can operate from the documented compatibility path
- [ ] Srclight lifecycle guidance covers refresh, validation, stale-index symptoms, and recovery
- [ ] No important project truth exists only in ignored local state

## Notes For Refinement

- Keep this TODO as the umbrella epic for the AI tooling system.
- If a section becomes too large, split it into child TODO files under `00-infrastructure/` and leave this file as the parent roadmap.
- Prefer concrete checklists and references over narrative prompts when refining later.
