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

### Rule Files To Deliver

- `.cursor/rules/safety-build.mdc`
  Scope: code files plus build scripts and project-control docs; not always-on.
  Source: `.agents/rules/safety.md` and the build-system sections of `AGENTS.md`.
  Covers: workspace boundaries, dangerous command limits, mandatory `bash scripts/build.sh`, `build/build.log` as the authoritative build result, `command_status` as an optional convenience signal only, silent Git behavior, and first-pass secrets or sensitive-data handling.

- `.cursor/rules/freestanding-kernel-code.mdc`
  Scope: kernel C and header files where freestanding and allocator constraints apply; not always-on.
  Source: `.agents/rules/coding.md` and the freestanding build notes in `AGENTS.md`.
  Covers: `-nostdinc` reality, `kernel/types.h`, no `malloc()` or `printf()`, allocator rules, `uintptr_t` casting for PMM returns, and the highest-value kernel C gotchas.

- `.cursor/rules/bare-metal-assembly.mdc`
  Scope: `**/*.asm`; not always-on.
  Source: `.agents/rules/coding.md` and `AGENTS.md`.
  Covers: NASM-style x86-64 assembly assumptions, low-level assembly constraints, and the hardware rules that matter directly to boot and kernel assembly.

- `.cursor/rules/api-surface-direction.mdc`
  Scope: code and documentation where OS-visible APIs, paths, and product conventions are described; not always-on.
  Source: `.agents/rules/intelligence.md` and `AGENTS.md`.
  Covers: Win32-native direction, Windows-style paths, `.cpl` applets, and long-term API and design direction.

- `.cursor/rules/doc-sync-discipline.mdc`
  Scope: code and documentation changes that can invalidate project guidance; not always-on.
  Source: `.agents/rules/intelligence.md`.
  Covers: mandatory updates to tracked project guidance when conventions, paths, APIs, workflows, or architecture assumptions change.

- `.cursor/rules/mcp-usage-discipline.mdc`
  Scope: code and documentation work where Srclight MCP behavior affects agent decisions; not always-on.
  Source: `.agents/rules/intelligence.md`, local MCP policy decisions, and the AI system TODO.
  Covers: when to use Srclight, the Srclight-only MCP baseline, what must never live only in local MCP state, and how MCP guidance stays subordinate to tracked repo truth.

- `.cursor/rules/todo-markdown-style.mdc`
  Scope: `todo/**/*.md`.
  Source: current project rule.
  Covers: lean TODO markdown, no unnecessary hard wraps, and compact-table guidance.

### Rule Migration Deliverables

- [x] Lock the first-pass specification for `safety-build.mdc` before creating the file:
  - Planned scope: code files plus build scripts and project-control docs.
  - Must include: workspace boundaries and explicit prohibition on leaving the workspace or touching host Windows paths such as `/mnt/c/`.
  - Must include: dangerous command limits, including no raw `make`, no real-device `dd` or `mkfs`, no writes to `/dev/sd*` or `/dev/nvme*`, no system-wide package installs without approval, and no external network requests unless instructed.
  - Must include: mandatory `bash scripts/build.sh`, `tail -1 build/build.log` as the authoritative build check, `command_status` as a convenience signal rather than the source of truth, and the note that silent Git output does not imply a hang.
  - Must include: first-pass handling of secrets and sensitive data in prompts, logs, examples, and tracked files; never auto-type or store credentials, and pause for manual auth when `sudo` or other credentials are required.
  - Must include: any repo `.githooks/` behavior that changes commit-time expectations and the rule that headless QEMU plus serial output is a supported automation path rather than an edge case.
  - Must exclude: Win32 and POSIX API direction, Windows runtime path conventions, MCP guidance, and freestanding C specifics.
  - Include one tiny example that shows the preferred build verification pattern.
- [x] Lock the first-pass specification for `api-surface-direction.mdc` before creating the file:
  - Planned scope: code and documentation, but not general build tooling files.
  - Must include: Win32-native API surface, POSIX as Linux-compat only, Windows-style paths, Control Panel `.cpl` conventions, and the production-grade design standard.
  - Must exclude: Srclight MCP behavior, doc-sync workflow, sandbox and build behavior, and low-level coding constraints.
  - Include one tiny example that contrasts a canonical Windows-style path with a Unix-style path.
- [x] Lock the first-pass specification for `doc-sync-discipline.mdc` before creating the file:
  - Planned scope: code and documentation changes that can invalidate tracked project guidance.
  - Must include: update `AGENTS.md`, relevant Cursor rules, relevant Cursor skills, and compatibility docs whenever code or conventions make them inaccurate.
  - Must include: examples of common triggers such as allocator changes, boot-sequence changes, API renames, naming shifts, or workflow changes.
  - Must exclude: Srclight MCP operating guidance, general API direction, sandbox/build safety rules, and low-level coding constraints.
  - Include one tiny example that shows a code or convention change requiring a same-task doc update.
- [x] Lock the first-pass specification for `mcp-usage-discipline.mdc` before creating the file:
  - Planned scope: code and documentation work where Srclight MCP meaningfully influences agent decisions.
  - Must include: start with Srclight orientation, prefer `hybrid_search()` for most queries, and treat Srclight as the only supported MCP integration for the first pass.
  - Must include: the rule that agents must not rely on Memory or filesystem MCP as part of the supported baseline.
  - Must include: the rule that tracked repo files remain canonical even when MCP state exists locally.
  - Must exclude: Win32 and path-direction guidance, build-safety behavior, and general documentation-sync workflow.
  - Include one tiny example showing a correct Srclight-first lookup for code understanding before broader manual research.
- [x] Lock the first-pass specification for `freestanding-kernel-code.mdc` before creating the file:
  - Planned scope: kernel C and header files, but not general build tooling docs or unrelated markdown.
  - Must include: no standard-library or angle-bracket headers, use `#include "kernel/types.h"`, no `malloc()`, no `printf()`, `kmalloc()` only for small allocations, and `pmm_alloc_contiguous()` for large or data-heavy allocations.
  - Must include: the cast rule for PMM returns, such as `(void *)(uintptr_t)pmm_alloc_contiguous(n)`.
  - Must include: the highest-value concise gotchas that belong in the rule itself, especially the `-nostdinc` reality and the 4 KB allocator boundary.
  - Must include: kernel-crash debugging guidance that treats `llvm-addr2line-19` and `llvm-objdump-19` as first-class tools for BSODs, panics, page faults, and RIP-based crash analysis when symbols are available.
  - Must exclude: build workflow, API and path conventions, MCP behavior, documentation-sync workflow, and assembly-specific hardware constraints.
  - Include one tiny example showing a correct include and a correct allocation choice or PMM cast.
- [x] Lock the first-pass specification for `bare-metal-assembly.mdc` before creating the file:
  - Planned scope: all `.asm` files involved in boot or kernel-level assembly work.
  - Must include: NASM syntax assumptions, x86-64 and UEFI-era environment assumptions, and the assembly-relevant hardware constraints.
  - Must include: no BIOS interrupt assumptions, no VGA text-mode assumptions, and no new PIC-routing assumptions.
  - Must include: concise guidance to comment non-obvious register, MMIO, interrupt, and hardware interactions.
  - Must exclude: C header and allocator rules, build workflow, Win32 and path-direction guidance, MCP behavior, and general documentation-sync workflow.
  - Include one tiny example that contrasts a disallowed BIOS or VGA assumption with an allowed UEFI or APIC-era assumption.
- [x] Create the exact Cursor rule files listed above under `.cursor/rules/`.
- [x] Map each legacy rule file in `.agents/rules/` to one or more Cursor replacements so nothing important is dropped during migration.
  - `safety.md` -> `safety-build.mdc`
  - `coding.md` -> `freestanding-kernel-code.mdc` and `bare-metal-assembly.mdc`
  - `intelligence.md` -> `api-surface-direction.mdc`, `doc-sync-discipline.mdc`, and `mcp-usage-discipline.mdc`
  - Existing `todo-markdown-style.mdc` remains the TODO markdown rule and is not a migration target from `.agents/rules/`
- [x] Decide the final scope for each rule, including which ones are always-on and which ones are file-scoped.
  - Decision: first-pass Cursor rules remain file-scoped and use `alwaysApply: false` with narrow globs instead of introducing a new always-on rule layer.
  - Exception: `todo-markdown-style.mdc` remains scoped to `todo/**/*.md` and also stays `alwaysApply: false`.
- [x] Keep the first pass project-rule-first: Team Rules may exist later as optional overlays, but the repo must not depend on dashboard-only rules to remain usable.
  - Decision: repo-owned `.cursor/rules/` is the canonical Cursor rule surface for this first pass; dashboard-only rules may exist later as optional overlays only.
- [x] Decide whether secrets and sensitive-data handling belongs entirely inside `safety-build.mdc` for the first pass or needs a dedicated future safety rule if the content becomes too large.
  - Decision: keep secrets and sensitive-data handling inside `safety-build.mdc` for the first pass; split it only if the rule becomes too large to stay clear.
- [x] Decide whether `.cursor/hooks.json` is part of the first-pass repo contract or an intentionally deferred guardrail layer; the baseline system must be documented either way.
  - Decision: `.cursor/hooks.json` is intentionally deferred in the first pass. If it exists locally later, it may act only as an advisory guardrail layer and must never be required for correctness.
- [x] Decide whether repo `.githooks/` is part of the first-pass supported workflow, and if so document installation via `git config core.hooksPath .githooks` and the relationship between repo git hooks and Cursor hooks.
  - Decision: repo `.githooks/` are a supported but optional current workflow surface. If installed via `git config core.hooksPath .githooks`, rules and skills must account for pre-commit staged-file lint failures and post-commit `COUNT.md` auto-generation or amend behavior, but correctness must not depend on hooks being installed.
- [x] Keep each rule narrowly focused and concise instead of copying large prose blocks from `AGENTS.md`.
- [x] Define what remains canonical in `AGENTS.md` versus what must also be present in Cursor rules for reliable agent behavior.
  - `AGENTS.md` remains the human-facing overview for philosophy, architecture, repo layout, toolchain, and testing posture.
  - Cursor rules must contain the concise, enforceable guidance agents need during edits and command execution without copying large narrative sections wholesale.
- [ ] Ensure the rule and skill system exposes the supported tooling contract from [TODO-02 Developer Tooling Stack](./TODO-02-developer-tooling-stack.md), including required baseline tools, recommended tools, GitHub workflow surfaces, and debug symbolication utilities such as `llvm-addr2line-19`.
- [x] If `AGENTS.md` is refined, keep the lead philosophy explicit: Impossible OS is a production-grade operating system, not a hobby project or prototype, and the name `Impossible` must never be treated as a development constraint or an excuse for shortcuts, lower standards, or "can't be done" thinking.
- [x] Verify the resulting rule set covers safety, freestanding kernel coding, assembly constraints, API direction, doc-sync discipline, MCP-usage discipline, and TODO markdown style without overlap or contradiction.

## 3. Cursor Skills And Workflow Equivalents

Replace prompt-heavy TODO procedure with a specific first-pass skill pack under `.cursor/skills/`.

### Core Skill Pack To Deliver

- `.cursor/skills/create-todo/`
  Source workflows: `todo-create.md` plus only the reusable marker and compact-table rules from `todo-table-format.md`.
  Responsibility: create one new lean TODO file, choose the right domain, assign numbering, write the required sections, set up the canonical implementation-order and comparison tables correctly, wire dependencies and XREFs, and update indexes correctly.

- `.cursor/skills/implement-todo-section/`
  Source workflow: `implement-todo.md`.
  Responsibility: execute one TODO section or clearly bounded subset, resolve XREF dependencies, choose Cursor Plan mode or direct execution as appropriate, implement work, build and test, and update that section when implementation reality differs from the current TODO text.

- `.cursor/skills/validate-todo-file/`
  Source workflows: the structural gap-analysis parts of `todo-validate.md`; if any file-level checklist fragments are reused from `docs-verify-todo.md`, they must be narrowly adapted because the current on-disk body of `docs-verify-todo.md` is section-level verification behavior.
  Responsibility: validate TODO-file completeness, implementation order, references, overlaps, and handoffs so the file can be followed without planning gaps, including continuity with adjacent related TODO files.

- `.cursor/skills/sync-ai-system/`
  Source material: `intelligence.md`, `AGENTS.md`, and this TODO.
  Responsibility: sync rules, skills, docs, and Antigravity compatibility notes when shared AI conventions change.

- `.cursor/skills/verify-todo-section/`
  Source workflows: `todo-done-check.md` plus the current section-verification behavior in `docs-verify-todo.md`.
  Responsibility: verify a TODO section that is marked done or in progress against actual code, build, and test evidence, and correct that section when reality does not match the current checklist or notes.

### Skill Boundary Rules

- `create-todo` must not implement code or reconcile existing checkbox state against the codebase.
- `implement-todo-section` must not create new TODO files, rewrite broader roadmap structure, or perform broad markdown audits beyond the scoped section it is executing.
- `validate-todo-file` must not absorb `verify-todo-section`; it owns file-level gap analysis and adjacent-file continuity checks, but not code-truth verification for supposedly completed sections.
- `sync-ai-system` must not be overloaded with TODO roadmap syncing; it stays focused on AI-facing project guidance.
- `verify-todo-section` must not become the general markdown validator; it is about section checklist truth versus code, build, and test reality.
- `implement-todo-section` and `verify-todo-section` must both correct the scoped section's checklist items and notes when implementation evidence shows the current TODO text is wrong, stale, or overstated.
- Skills should own reusable multi-step workflows; if a flow is lightweight, explicit, and one-shot, prefer a command instead of inflating the skill set.

### Supporting Files Per Skill

- Each skill should start with `SKILL.md` plus at most one or two one-level-deep supporting references when the main skill would otherwise become too long.
- `create-todo`: a compact TODO template reference and an implementation-order/XREF/index-update reference.
- `implement-todo-section`: a short build and test verification reference.
- `validate-todo-file`: a gap-analysis, continuity, and validation checklist reference.
- `sync-ai-system`: an AI sync checklist reference.
- `verify-todo-section`: a status-classification and section-verification reference.

### Mode And Invocation Guidance

- [x] Document when Cursor Plan mode is mandatory, such as ambiguous requirements, architecture changes, risky operations, or multi-file work with meaningful trade-offs.
  - Decision: Plan mode is mandatory when the scope is ambiguous, the work changes architecture or public interfaces, the operation is risky, or multiple valid approaches require an explicit trade-off decision before editing.
- [x] Document when Ask mode or other read-only exploration should be used before editing, especially for codebase understanding, TODO gap analysis, and pre-implementation research.
  - Decision: Ask-mode or equivalent read-only exploration is the default for codebase orientation, TODO gap analysis, and pre-implementation research when no edit is justified yet.
- [x] Document when Agent execution is appropriate for bounded implementation or documentation work after the plan is clear.
  - Decision: Agent execution is appropriate once the section scope is bounded, dependencies are understood, and the expected verification path is clear.
- [x] Document when Debug mode should replace normal edit-build iteration, especially for hard-to-reproduce bugs, regressions, performance issues, or failures that require runtime evidence.
  - Decision: Debug mode replaces normal iteration for runtime-only failures, regressions, performance investigations, flaky behavior, or any issue that needs live evidence rather than static reasoning.
- [x] Document that tool-first debugging is mandatory for kernel crashes, BSODs, panics, and raw RIP analysis: if symbols are available, use `llvm-addr2line-19` and related LLVM tools before speculating about the faulting code.
- [x] Document that headless QEMU with serial output is the preferred low-interaction execution path for autonomous debugging and test loops; graphical QEMU should not be assumed when terminal-visible or captured serial evidence is available.
- [x] Decide whether lightweight explicit workflows belong in Cursor commands instead of skills when they do not need automatic invocation or large supporting references.
  - Decision: lightweight explicit workflows may become Cursor commands later, but the first-pass rollout keeps reusable multi-step logic in skills and leaves commands deferred.

### Skill Specification Deliverables

- [x] Keep the first-pass skill set at five core skills with non-confusing names: `create-todo`, `implement-todo-section`, `validate-todo-file`, `sync-ai-system`, and `verify-todo-section`.
- [x] Rename the older draft concepts so responsibilities stay unambiguous: `implement-todo` -> `implement-todo-section`, `verify-todo` -> `validate-todo-file`, and `todo-done-check` -> `verify-todo-section`.
- [x] Lock the first-pass specification for `create-todo` before creating the skill:
  - Planned scope: create one new lean TODO file, choose the domain, assign numbering, write the required sections, establish exact implementation order, and update indexes.
  - Must include: source workflow `todo-create.md`, the reusable structure and marker rules from `todo-table-format.md`, and the numbering, XREF, and index-update behavior needed for new TODO creation; support both spec-driven and non-spec-driven TODO creation.
  - Must include: the new lean TODO standard rather than the old giant prompt model.
  - Must include: creation of the canonical `Implementation Order` table with a leading `⭐/💎` column, exact dependency order, and any required overlap or handoff references at creation time.
  - Must include: replacement of legacy phase-table, formatting-only cleanup, and MCP-memory language with the current `Implementation Order`, repo-truth, and Srclight-only first-pass model.
  - Must exclude: code implementation, codebase-vs-checkbox reconciliation, and broad cross-file roadmap synchronization beyond the new TODO's initial handoff and index updates.
  - Supporting refs: a compact TODO template reference and an implementation-order/XREF/index-update reference.
  - Include one tiny example showing a lean TODO heading plus a minimal implementation-order row or commit line.
- [x] Lock the first-pass specification for `implement-todo-section` before creating the skill:
  - Planned scope: execute one TODO section or a clearly bounded subset of a TODO.
  - Must include: read the section fully, resolve XREF dependencies, choose Cursor Plan mode or direct execution based on risk and scope, implement, build, test, and update the TODO section state.
  - Must include: if code, build, or test evidence shows the current checklist, notes, or completion state are inaccurate, correct the scoped section before finishing.
  - Must include: build verification via `bash scripts/build.sh` and `build/build.log`, while following the future lean TODO standard instead of assuming the old prompt-rewrite workflow verbatim.
  - Must include: use the supported tooling contract from [TODO-02 Developer Tooling Stack](./TODO-02-developer-tooling-stack.md) instead of inventing alternate build or debug paths, especially when GitHub workflow behavior or local tooling availability matters.
  - Must include: if BSOD, panic, page-fault, or crash-RIP debugging is involved and symbols are available, use `llvm-addr2line-19` and `llvm-objdump-19` before reasoning about the fault site.
  - Must include: prefer headless QEMU with serial output for autonomous implement-test-debug-fix loops; if a script captures serial to a file for stability, read and use that evidence instead of waiting for a GUI-only signal.
  - Must include: if a commit is requested and repo `.githooks/` are installed, anticipate staged C or H lint failures from `pre-commit` and post-commit `COUNT.md` regeneration or commit amendment behavior instead of treating those changes as unexpected drift.
  - Must include: stop and ask when requirements conflict, repo state is unexpected, or verification fails instead of silently widening scope.
  - Must exclude: GUI-only QEMU assumptions, mandatory `clean run` on every iteration, and any dependency on Memory MCP.
  - Must exclude: new TODO authoring, broad cross-file roadmap maintenance, and broad markdown-only validation passes.
  - Supporting refs: a short build and verification reference.
  - Include one tiny example showing an XREF dependency block or a copied commit line.
- [x] Lock the first-pass specification for `validate-todo-file` before creating the skill:
  - Planned scope: validate one TODO file for completeness, ordering, references, gap-free execution coverage, and adjacent-file continuity.
  - Must include: structural completeness checks for required sections, exact implementation-order validation, dependency validation, XREF validation, overlap checks, adjacent-file handoff checks, and gap analysis.
  - Must include: confirmation that the TODO can be followed without missing work inside the file and without gaps between this TODO file and adjacent related TODO files.
  - Must include: correction of file-level planning inconsistencies, broken references, or stale roadmap wording when they are discovered during validation.
  - Must include: use `todo-validate.md` only for structural and continuity behavior; the current section-level build or QEMU verification flow in `docs-verify-todo.md` belongs to `verify-todo-section` instead of this skill.
  - Must exclude: `verify-todo-section` codebase completion sync, direct implementation work, broad roadmap or index restructuring, and formatting-only cleanup that should have been handled during TODO creation.
  - Supporting refs: a gap-analysis, continuity, and validation checklist reference.
  - Include one tiny example showing a missing dependency, stale XREF, or corrected handoff note.
- [x] Lock the first-pass specification for `sync-ai-system` before creating the skill:
  - Planned scope: update AI-facing project truth when shared conventions change.
  - Must include: updates to `AGENTS.md`, relevant Cursor rules, relevant Cursor skills, and Antigravity compatibility docs when AI-system assumptions change.
  - Must include: preservation of the top-level `AGENTS.md` posture that Impossible OS is production-grade, not a hobby OS or prototype, and that difficulty is not a reason to lower standards or treat work as impossible.
  - Must include: updates to any explicit boundary guidance for local vs cloud vs team-managed features and any supported mode or automation policy that affects how agents operate in the repo.
  - Must include: synchronization of supported-tool awareness from [TODO-02 Developer Tooling Stack](./TODO-02-developer-tooling-stack.md) whenever the required, recommended, deferred, or GitHub-enforced tool contract changes.
  - Must exclude: TODO roadmap sync, checkbox truth-vs-code checks, and general implementation work.
  - Supporting refs: an AI sync checklist reference.
  - Include one tiny example showing a code or convention change that requires synchronized AI docs updates.
- [x] Lock the first-pass specification for `verify-todo-section` before creating the skill:
  - Planned scope: verify one TODO section that is marked done or in progress against actual code, build, and test evidence.
  - Must include: classification of section state into `[x]`, `[/]`, `[ ]`, and mismatch states based on actual code evidence.
  - Must include: conservative marking rules and correction of stale descriptions, checklist items, notes, or verification wording when implementation names, paths, behavior, or completion state differ.
  - Must include: section-scoped evidence for what was checked, what passed, what failed, and why the section state was updated if reality does not match the prior TODO text.
  - Must include: use of the supported tooling contract for verification, including `llvm-addr2line-19` and `llvm-objdump-19` when checking crash fixes or other RIP-based fault sections.
  - Must include: verification behavior that accepts headless QEMU serial output, whether shown directly in the terminal or captured by an approved script and surfaced back as log evidence.
  - Must include: absorption of the current section-verification flow in `docs-verify-todo.md` alongside `todo-done-check.md`, while not assuming a master-and-child TODO cascade by default.
  - Must exclude: broad markdown validation, cross-file continuity audits, and new TODO authoring.
  - Supporting refs: a status-classification and section-verification reference.
  - Include one tiny example distinguishing a completed item from a partial or mismatched item.
- [x] Build a migration table covering every existing file in `.agents/workflows/` and mark each one as migrate to skill, keep as reference doc, retire, or absorb into another skill; explicitly flag old phase-table, master-sync, or formatting-pass behavior for selective absorption or retirement.
- [x] Decide whether any small useful pieces of legacy `todo-master-sync.md` should be absorbed into `create-todo` or `validate-todo-file`, with the remainder retired.
  - Decision: absorb only the useful index-update, cross-file continuity, and XREF consistency ideas into `create-todo` and `validate-todo-file`; retire the standalone master-sync model and its baseline master-subfile assumptions.
- [x] Verify that no new skill duplicates large chunks of `AGENTS.md` or other tracked docs unnecessarily.
  - Decision: keep each `SKILL.md` concise, rely on one-level-deep references for fragile detail, and leave broad human-facing context in tracked docs instead of copying it into every skill.

### Legacy Workflow Migration Decisions

| Workflow                | Decision             | Target                                    | Notes                                 |
| ----------------------- | -------------------- | ----------------------------------------- | ------------------------------------- |
| `todo-create.md`        | Migrate              | `create-todo`                             | Primary TODO authoring flow           |
| `todo-table-format.md`  | Absorb, then retire  | `create-todo`, `validate-todo-file`       | Keep markers and compact-table rules  |
| `todo-master-sync.md`   | Absorb, then retire  | `create-todo`, `validate-todo-file`       | Keep index and XREF continuity only   |
| `implement-todo.md`     | Migrate              | `implement-todo-section`                  | Section-scoped implementation         |
| `todo-done-check.md`    | Migrate              | `verify-todo-section`                     | Code-truth and status classification  |
| `docs-verify-todo.md`   | Absorb partially     | `verify-todo-section`                     | Current body is section verification  |
| `todo-validate.md`      | Absorb partially     | `validate-todo-file`                      | Keep structure and gap-analysis only  |
| `build.md`              | Keep as reference    | `implement-todo-section`, `verify-todo-section` | Thin build-evidence reference   |
| `docs-convert-todo.md`  | Future skill or ref  | Future specialized skill                  | Not first-pass core                   |
| `docs-validate.md`      | Keep as reference    | Docs maintenance                          | Not core TODO lifecycle               |
| `add-asset.md`          | Future skill or ref  | Future specialized skill                  | Subsystem-specific workflow           |
| `test-hardware.md`      | Keep as reference    | Hardware testing                          | Manual and user-gated                 |
| `test-fs-fat32.md`      | Keep as reference    | Filesystem testing                        | Subsystem-specific raw `make` path    |
| `specs-create.md`       | Future skill or ref  | Future specialized skill                  | Spec authoring                        |
| `specs-fact-check.md`   | Future skill or ref  | Future specialized skill                  | Spec QA                               |
| `release.md`            | Keep as reference    | Release process                           | Not core TODO lifecycle               |

### Canonical TODO Table Convention For Skills

- [x] Use one canonical convention across `create-todo` and `validate-todo-file`.
- [x] Replace `Phase-by-Phase` with an `Implementation Order` table that uses a leading `⭐/💎` column.
- [x] The `Implementation Order` table must show the exact order of implementation, key deliverable or scope, dependencies, and status.
- [x] If overlap, XREF, or handoff detail would make the table too wide, keep the table compact and move that detail into a short adjacent bullet block or note block.
- [x] `create-todo` must set up tables, references, overlap XREFs, and dependency order correctly during creation instead of relying on a later formatting-only cleanup pass.
- [x] `validate-todo-file` must run gap analysis against the TODO itself and adjacent related TODO files, including missing steps, missing dependency links, missing overlap references, and missing handoff coverage.
- [x] `verify-todo-section` may correct section state and notes when evidence conflicts with the current TODO text, but it must not become the owner of file-wide table normalization or roadmap structure.
- [x] If optional parent or aggregate TODOs are introduced later, they must inherit this same convention instead of defining a separate table model.
- [x] `OS Comparison` tables may keep a leading `⭐` column only when they remain compact enough to stay readable in wrapped editors; otherwise split them into smaller readable blocks.
- [x] Any future migrated formatting behavior, including `todo-table-format`, must be absorbed into `create-todo` or rewritten to support `Implementation Order` instead of reintroducing phase tables or a required post-creation cleanup step.

### Secondary Workflow Decisions

- [x] Decide whether `.agents/workflows/build.md` becomes a dedicated Cursor skill or remains a plain reference document.
  - Decision: keep `build.md` as a plain reference document for now and absorb only a thin build-evidence excerpt into `implement-todo-section` and `verify-todo-section`.
- [x] Decide whether `.agents/workflows/docs-convert-todo.md` becomes a dedicated Cursor skill or remains a plain reference document.
  - Decision: defer it as a future specialized skill or keep it as a reference doc; it is not part of the first-pass five-skill pack.
- [x] Decide whether `.agents/workflows/add-asset.md`, `test-hardware.md`, and `test-fs-fat32.md` become dedicated specialized skills or stay as task-specific docs.
  - Decision: keep them as task-specific reference docs for now, with the option to promote them into specialized skills later if repeated usage justifies it.
- [x] Decide whether `.agents/workflows/specs-create.md`, `specs-fact-check.md`, `docs-validate.md`, and `release.md` belong in Cursor skills, repo docs, or retirement.
  - Decision: keep `docs-validate.md` and `release.md` as repo reference docs for now; defer `specs-create.md` and `specs-fact-check.md` as future specialized skills or reference docs rather than part of the first-pass core pack.

## 4. Lean TODO Standard

Define a new TODO format that stays readable as the backlog grows.

- [ ] Design a lean TODO template for the new `todo/` tree.
- [ ] Treat leaf TODOs as the default. Only introduce a parent or aggregate TODO when one topic truly needs multiple leaf files or shared verification.
- [ ] Remove the old requirement that every TODO section carry a long mutable prompt.
- [ ] Define the minimum required parts of a TODO file, such as goal, `Implementation Order`, dependencies, checklist, references, overlap XREFs, verification or exit criteria, and any needed handoff notes.
- [ ] Require every TODO file to be complete within its declared scope so that following it produces a gap-free subsystem slice: no missing implementation steps, no missing dependency links, and no unaccounted gap between this TODO file and the next related TODO file.
- [ ] Require correct implementation order and explicit cross references wherever TODO sections overlap or depend on work defined elsewhere.
- [ ] Define a minimal per-change definition-of-done pattern that records what changed, what was verified, and any known remaining limit or follow-up.
- [ ] Require supposedly complete TODO sections to call out unresolved limitations or deferred work explicitly instead of leaving silent ambiguity.
- [ ] Update TODO creation guidance so it matches the new `00/01/02` domain numbering and local file naming.
- [ ] Adopt a no-unnecessary-hard-wraps rule for TODO markdown; keep prose and checklist items on single logical lines unless structure genuinely needs a break.
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
- [x] Decide whether `.cursor/hooks.json` and hooks-based guardrails are in scope for the first pass or intentionally deferred; if deferred, document that the system must remain safe without hooks.
  - Decision: hooks are intentionally deferred in the first pass and may exist only as advisory local optimization later; the documented repo workflow must remain correct without them.
- [x] Decide whether repo `.githooks/` are required, recommended, or optional for contributors and agents; if supported, document installation, expected behavior, and how they differ from Cursor hooks.
  - Decision: repo `.githooks/` are optional but supported. They remain the current commit-time enforcement surface when installed, while Cursor hooks stay advisory-only and must not become a hidden dependency.
- [ ] Decide whether the current `post-commit` auto-regeneration of `COUNT.md` with an automatic amend remains acceptable, or whether it should move to a safer explicit command or CI-style workflow because it changes commit hashes and surprises tooling.
- [ ] Decide whether worktrees and parallel agents need a repo policy for isolation, build setup, shared state, and conflicting TODO edits; if adopted, note the current LSP or lint limitations in worktrees.
- [ ] Decide whether Bugbot or PR review automation is part of the supported workflow, and if so how repo-owned review guidance will be tracked rather than left only in dashboard settings.
- [ ] Decide whether cloud agents and automations are part of the supported operating model or intentionally deferred until environment setup, secrets, network access, and reproducibility are documented.
- [ ] Record that any adopted commands, hooks, review bots, or cloud and dashboard features remain downstream of tracked repo guidance instead of becoming the canonical owner of project behavior.

### Cursor Hooks Policy

- [x] Define the allowed first-pass scope for `.cursor/hooks.json`, explicitly separating advisory hooks from enforcement hooks so the repo does not depend on hidden local automation.
- [x] Allow hooks to optimize workflow only when they are fast, deterministic, idempotent, and safe to miss; if hooks are disabled, the documented repo workflow must still remain correct.
- [x] Decide which hook events are worth supporting first, such as `sessionStart` for loading project context, `afterFileEdit` for cheap reminders or lint nudges, and `beforeShellExecution` for warning on dangerous commands or reminding build and test contracts.
- [x] Forbid hooks from becoming hidden write automation for tracked project truth: no silent TODO rewrites, commits, pushes, destructive commands, or mutation of canonical docs without an explicit user-directed workflow.
- [x] Require any enforcement-style hook to surface clear local output and to fail safe when tools are unavailable, configs drift, or the hook would block legitimate work for non-policy reasons.
- [x] Define how Cursor hooks relate to repo `.githooks/`: Cursor hooks may guide and preflight local actions, while repo git hooks remain the only supported commit-time enforcement unless a later decision expands scope.
- [x] Decide whether hook-driven reminders should surface key workflow optimizations such as build-log sentinel usage, headless QEMU plus serial debugging, TODO-state discipline, and stop or ask triggers for risky operations.
- [x] Document how any adopted hook configuration is shared, reviewed, and versioned while keeping machine-local secrets, credentials, and absolute paths out of repo-tracked definitions.

## 7. MCP Policy

Keep MCP powerful, but subordinate it to tracked project truth.

> [!NOTE]
> **Current Antigravity MCP baseline:**
> - `srclight` enabled via `srclight serve --transport stdio --workspace dev-workspace`
>
> Supported first-pass MCP model: Srclight only. Drop `memory` and `filesystem` from the intended baseline unless a later TODO explicitly reintroduces them with a strong justification.
>
> Treat this as the current-state compatibility baseline and validation target, not as the canonical source of project truth.

- [x] Document Srclight as the preferred code intelligence path and define the fallback when it is unavailable.
  - Decision: prefer Srclight for orientation and lookup, but fall back to `rg`, direct file reads, and other repo-grounded tools whenever Srclight is unavailable, stale, or contradicted by tracked files.
- [x] Record the decision that the first-pass supported MCP set is Srclight only, and that Memory and filesystem MCP are intentionally out of scope unless re-approved later.
- [ ] Decide how project-level MCP config, user-level Cursor MCP config, and any team or cloud-managed MCP setup relate, while keeping tracked repo guidance canonical.
- [x] Capture the Srclight-only MCP baseline in a portable documented form without making machine-specific absolute paths normative.
- [x] Remove stale references to Memory or filesystem MCP from the intended Cursor-primary model and document them only as deprecated or historical local config if they still exist.
- [x] Record that clangd is LSP-only and must not be configured as an MCP server.
- [ ] Add a validation checklist for local MCP setup so Cursor and Antigravity can be verified against the intended model.

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

- [x] Confirm Cursor has a project-native rules and skills layer in the repo.
- [ ] Confirm the boundary model clearly distinguishes canonical repo truth from local, cloud, and dashboard-managed state.
- [x] Confirm mode guidance exists for Plan, Ask, Agent, and Debug rather than leaving those choices implicit.
- [ ] Confirm secrets and sensitive-data handling are explicit and compatible with MCP, local tooling, and tracked docs.
- [ ] Confirm high-risk AI-assisted work still has mandatory human-review gates and stop-or-escalate behavior.
- [ ] Confirm the new TODO format is usable without giant prompt sections or a separate formatting-only cleanup pass.
- [x] Confirm no first-pass skill assumes a baseline master/sub-file TODO architecture.
- [x] Confirm `validate-todo-file` owns the current cross-file continuity and handoff checks.
- [x] Confirm `implement-todo-section` and `verify-todo-section` both correct section checklist state when code, build, or test evidence conflicts with the current TODO text.
- [ ] Confirm the new TODO format captures exact implementation order, dependencies, and gap-free handoffs across related TODO files.
- [x] Confirm repo `.githooks/` behavior is either explicitly supported and documented, or intentionally treated as optional or deprecated rather than left ambiguous.
- [x] Confirm any supported commit workflow accounts for staged-file lint failures and any `COUNT.md` auto-generation or amend behavior.
- [ ] Confirm advanced Cursor features such as commands, hooks, worktrees, Bugbot, and cloud agents are either explicitly deferred or covered by repo-owned guidance.
- [x] Confirm any adopted Cursor hooks policy improves safety or speed without making hidden local automation the canonical owner of repo behavior.
- [ ] Confirm repo docs and local configs do not contradict each other.
- [ ] Confirm Antigravity can operate from the documented compatibility path.
- [ ] Confirm the Srclight lifecycle guidance covers refresh, validation, stale-index symptoms, and recovery without treating `.srclight/` as authoritative project state.
- [ ] Confirm no important project truth exists only in ignored local state.

## Notes For Refinement

- Keep this TODO as the umbrella epic for the AI tooling system.
- If a section becomes too large, split it into child TODO files under `00-infrastructure/` and leave this file as the parent roadmap.
- Prefer concrete checklists and references over narrative prompts when refining later.
