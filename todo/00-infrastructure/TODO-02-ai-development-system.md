# TODO-02 -- AI Development System

> **Goal:** Turn the repo's AI workflow into an explicit, maintainable system instead of a pile of overlapping instructions. Impossible OS already carries substantial Claude, Cursor, and Copilot guidance, but ownership is fragmented across `CLAUDE.md`, `.github/copilot-instructions.md`, `.claude/`, `.cursor/`, and tool-specific hooks. This TODO defines the canonical source-of-truth map, skill lifecycle, hook policy, parity checks, and regression checks so the AI layer stays coherent as the project scales.

> [!IMPORTANT]
> **Current state:** The repository already has a serious AI surface: `CLAUDE.md`, `.github/copilot-instructions.md`, `.claude/settings.json`, `.claude/skills/`, `.cursor/hooks.json`, `.cursor/rules/`, and `.cursor/skills/`. Recent work strengthened the completion-first philosophy and several Claude skills, but there is still no canonical infrastructure TODO that owns the multi-agent system itself. `todo/TODO-00-INDEX.md` currently points at a missing "AI Development System" file, and no existing infrastructure TODO owns instruction-layer drift, skill templates, hook routing policy, or a regression pack that proves the AI workflow still matches repo doctrine.

## Inputs

- [`CLAUDE.md`](../../CLAUDE.md) -- Claude source-of-truth doctrine and workflow rules
- [`.github/copilot-instructions.md`](../../.github/copilot-instructions.md) -- Copilot CLI repo instructions
- [`.claude/settings.json`](../../.claude/settings.json) -- Claude hooks, permissions, and reminders
- [`.claude/skills/README.md`](../../.claude/skills/README.md) -- Claude skill catalog
- [`.claude/skills/`](../../.claude/skills/) -- skill implementations and templates
- [`.cursor/hooks.json`](../../.cursor/hooks.json) -- Cursor hook entry points
- [`.cursor/rules/`](../../.cursor/rules/) -- Cursor rule set
- [`.cursor/skills/README.md`](../../.cursor/skills/README.md) -- Cursor skill scope and limits
- [`todo/TODO-00-INDEX.md`](../TODO-00-INDEX.md) -- root roadmap ownership
- -> XREF: `T01 §5, §6` -- hook lifecycle and CI/tooling contract live in the developer-tooling roadmap
- -> XREF: `T03 §4` -- deferred test-gap ownership should remain reachable by AI workflow guidance, not duplicated

## Outcome

- One explicit ownership map for instructions, skills, hooks, permissions, and tool-specific behavior across Claude, Cursor, and Copilot.
- A stable skill lifecycle: scaffold, document, validate, cross-link, and retire without drift.
- Hook routing and policy rules that are intentional and reviewable instead of encoded only in large JSON blobs.
- Cross-agent parity checks that catch doctrine drift between `CLAUDE.md`, Copilot instructions, Claude skills, and Cursor rules.
- An AI workflow regression pack that proves the repo still enforces completion-first behavior and required domain handoffs.

## Implementation Order

| ⭐  | Order | Deliverable                                      | Depends On   | Status |
| --- | :---: | ------------------------------------------------ | ------------ | :----: |
| 💎  |   1   | Canonical ownership and source-of-truth map      | --           |  [ ]   |
| 💎  |   2   | Skill lifecycle, templates, and catalog rules    | §1           |  [ ]   |
| 💎  |   3   | Hook routing and policy contract                 | §1           |  [ ]   |
| 💎  |   4   | Cross-agent parity and drift audit               | §1-§3        |  [ ]   |
| 💎  |   5   | MCP, permissions, and extension boundary         | §1, §3, §4   |  [ ]   |
| ⭐  |   6   | AI workflow regression suite                     | §1-§5        |  [ ]   |

> 💎 = parity work: mature engineering repos document ownership, automation rules, and policy boundaries.
> ⭐ = exclusive work: Impossible OS can treat its AI workflow as a first-class subsystem with regression checks, not as untracked prompt folklore.

---

## 1. Canonical Ownership and Source-of-Truth Map

The first job is to state clearly which file owns what.

- [ ] Write an ownership matrix under `docs/infrastructure/` or equivalent covering `CLAUDE.md`, `.github/copilot-instructions.md`, `.claude/settings.json`, `.claude/skills/`, `.cursor/rules/`, `.cursor/hooks.json`, and `.cursor/skills/`
- [ ] Define which doctrine is global and which is tool-local: product philosophy, repo workflow, domain quality gates, permissions, hook messages, and skill execution steps
- [ ] Replace any stale references in root docs/indexes that still point at missing or superseded AI roadmap files
- [ ] Add explicit "edit here, not there" notes where duplicate concepts currently exist across tool layers
- [ ] Commit: `"docs/ai: define canonical ownership map for repo AI system"`

**Test checkpoint:** A contributor can answer "where do I change this rule?" for doctrine, hook behavior, skill content, and Copilot guidance without guessing. Root roadmap links resolve to real files.

---

## 2. Skill Lifecycle, Templates, and Catalog Rules

Skills need the same rigor as code: discoverable ownership, templates, and retirement rules.

- [ ] Define a standard lifecycle for adding or editing skills: scaffold/template, README/index entry, trigger description, XREF expectations, and validation path
- [ ] Create or tighten a shared skill authoring template so new skills carry required sections consistently
- [ ] Document how Claude-only, Cursor-only, and shared concepts are represented without pretending the two systems are mirrors
- [ ] Add catalog hygiene rules: every live skill must appear in the local README/index and must name its owning workflow clearly
- [ ] Document retirement/supersession rules for stale skills so old prompt blocks do not linger as false owners
- [ ] Commit: `"docs/ai: define skill lifecycle, template, and catalog rules"`

**Test checkpoint:** A new skill can be scaffolded and registered by following one documented path. README/index entries and actual skill directories stay in sync.

---

## 3. Hook Routing and Policy Contract

Hooks are part of the AI system, not invisible glue.

- [ ] Audit `.claude/settings.json`, `.cursor/hooks.json`, and related helper scripts into a readable routing matrix: trigger, matcher, owner, and intended reminder/gate behavior
- [ ] Document mandatory triggers for domain-quality skills, TODO workflows, and scope-gap handling
- [ ] Split large hook concerns into named policy blocks or documented sections so edits do not require blind JSON surgery
- [ ] Define which hook behaviors are reminders, which are hard blocks, and which are informational only
- [ ] Add explicit XREFs to `T01 §5` for hook installation/CI surfaces that live outside the AI system itself
- [ ] Commit: `"docs/ai: codify hook routing and policy contract"`

**Test checkpoint:** For any given edit path or skill invocation, the responsible hook and its intended effect are documented. A maintainer can tell whether a behavior is a reminder or a block without reading minified JSON logic.

---

## 4. Cross-Agent Parity and Drift Audit

The repo uses multiple AI tool systems. Drift must be a tracked problem, not a surprise.

- [ ] Define the parity set that must stay aligned across Claude, Copilot, and Cursor: product north star, completion-first doctrine, domain routing, TODO workflow expectations, and safety constraints
- [ ] Add a repeatable drift-audit checklist or script that compares the shared doctrine surfaces and flags missing or stale counterparts
- [ ] Document intentional divergence: where Claude is richer, where Cursor is intentionally smaller, and where Copilot instructions are higher-level by design
- [ ] Add reciprocal references between the doctrine documents so a maintainer can move from one tool layer to the others
- [ ] Commit: `"docs/ai: add cross-agent parity and drift audit"`

**Test checkpoint:** A parity audit produces a short actionable list of shared doctrine matches, intentional divergences, and broken drifts. A new doctrine change can be propagated without missing one tool layer.

---

## 5. MCP, Permissions, and Extension Boundary

Permissions and extensions are part of the architecture. They need clear boundaries and safe defaults.

- [ ] Document the current permission model and escalation boundaries for tool use, hooks, and local settings across Claude and Copilot surfaces
- [ ] Define where project-local extensions or MCP-style integrations belong and where they do not
- [ ] Add policy notes for secrets, local-only settings, and non-committed machine-specific overrides
- [ ] Review the boundary between repo-tracked instructions and user-local configuration so shared policy does not leak into personal state
- [ ] Add extension/permission maintenance notes to the ownership matrix from §1
- [ ] Commit: `"docs/ai: define MCP, permissions, and extension boundary"`

**Test checkpoint:** A maintainer can tell which settings are safe to commit, which are local-only, and where to add new project-level AI capabilities without violating policy boundaries.

---

## 6. AI Workflow Regression Suite

This is the refinement step: test the workflow itself.

> [!TIP]
> Public Windows and Linux projects may publish contribution rules, but they rarely ship a regression pack that checks whether the AI assistance layer still enforces the project's own doctrine.

- [ ] Add a lightweight AI workflow regression pack (`scripts/test-ai-system.sh` or equivalent) that validates key invariants: required docs exist, expected skill directories exist, root/index links resolve, and parity-check rules still pass
- [ ] Add fixture-style checks for high-value workflow promises: completion-first wording present, domain routing docs present, missing-skill/index drift detected, and stale root links caught
- [ ] Make regression output actionable: print exactly which doctrine surface drifted and where to repair it
- [ ] Wire the regression pack into a lightweight local/CI path so AI workflow regressions are caught before contributors hit them manually
- [ ] Document how to extend the regression suite when new tools/skill layers are added
- [ ] Commit: `"test/ai: add AI workflow regression suite"`

**Test checkpoint:** On a healthy repo, the AI regression suite passes. If a root roadmap link points at a missing file or a required doctrine block disappears, the suite fails with a specific repair path.

---

## OS Comparison

| ⭐ | Feature                         | 🪟 Win11 projects       | 🐧 Linux projects      | 🚀 Impossible OS |
| --- | ------------------------------- | ---------------------- | ---------------------- | ---------------- |
| 💎 | Repo-codified AI instructions   | ⚠️ Emerging practice   | ⚠️ Emerging practice   | ⬜ §1            |
| 💎 | Skill/catalog ownership         | ❌ Often ad hoc        | ❌ Often ad hoc        | ⬜ §2            |
| 💎 | Hook policy matrix              | ⚠️ Usually implicit    | ⚠️ Usually implicit    | ⬜ §3            |
| 💎 | Cross-tool drift audit          | ❌ Rare                | ❌ Rare                | ⬜ §4            |
| 💎 | Permissions/extension boundary  | ⚠️ Varies by repo      | ⚠️ Varies by repo      | ⬜ §5            |
| ⭐ | AI workflow regression suite    | ❌ Rare in practice    | ❌ Rare in practice    | ⬜ §6            |

> **After §1-§5:** Impossible OS reaches the baseline a mature multi-tool AI workflow should have: explicit owners, documented hooks, and controlled drift.
> **After §6:** the repo surpasses typical public practice by testing the AI workflow itself as maintained infrastructure.

## Unit Tests

> AI workflow checks are host-side regression checks, not kernel `test_runner_init()` suites.

- [ ] Create `scripts/test-ai-system.sh` with:
  - root/index link checks for all AI-system roadmap files
  - doctrine-parity checks across `CLAUDE.md`, Copilot instructions, and documented shared rules
  - skill catalog checks: README/index entries match live skill directories for the owned tool layers
  - hook config sanity checks for required files and documented matchers
- [ ] Wire the AI regression pack into an existing lightweight local/CI path
- [ ] Commit: `"test/ai: add regression checks for repo AI workflow"`

## Verification

- [ ] Root roadmap and `00-infrastructure/INDEX.md` point only at real AI/tooling TODO files
- [ ] Ownership matrix clearly maps doctrine, skills, hooks, permissions, and local settings
- [ ] A parity/drift audit can explain intended divergence between Claude, Cursor, and Copilot surfaces
- [ ] `scripts/test-ai-system.sh` fails on missing links or broken parity expectations and passes on a healthy repo
- [ ] Hook policy documentation explains which behaviors are reminders versus hard blocks
