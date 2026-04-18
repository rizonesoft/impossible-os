# TODO-02 -- AI Development System

> **Goal:** Turn the repo's AI workflow into an explicit, maintainable system instead of a pile of overlapping instructions. Impossible OS is **Claude Code-only** as of 2026-04-18; Cursor was removed because maintaining a parallel skill set under `.cursor/` created clutter without a corresponding productivity win. This TODO defines the canonical source-of-truth map for the Claude + Copilot + Codex surfaces that remain, the skill lifecycle, hook policy, and a regression check that proves the AI layer still matches repo doctrine.

> [!IMPORTANT]
> **Current state:** The repository's AI surface today is `CLAUDE.md`, `.github/copilot-instructions.md`, `.claude/settings.json`, and `.claude/skills/` (25+ skills covering TODO workflow, implementation, review, Codex dispatch, domain code-quality gates). Codex participates as an external adversarial reviewer invoked from Claude skills via the OpenAI Codex plugin, not as a first-party instruction layer. Copilot is used as an external review CLI (`scripts/copilot-review.sh`) in the same "reviewer, not authority" role. No canonical infrastructure TODO owns the multi-agent system itself today: the ownership map lives scattered across `CLAUDE.md` prose and individual skill READMEs, the hook policy lives only inside a large `settings.json` blob, and no regression pack checks that the completion-first / domain-routing doctrine is still enforced.

## Inputs

- [`CLAUDE.md`](../../CLAUDE.md) -- Claude Code source-of-truth doctrine and workflow rules
- [`.github/copilot-instructions.md`](../../.github/copilot-instructions.md) -- Copilot CLI repo instructions (external-reviewer role)
- [`.claude/settings.json`](../../.claude/settings.json) -- Claude Code hooks, permissions, and reminders
- [`.claude/skills/README.md`](../../.claude/skills/README.md) -- Claude skill catalog
- [`.claude/skills/`](../../.claude/skills/) -- skill implementations and templates
- [`scripts/copilot-review.sh`](../../scripts/copilot-review.sh) -- Copilot CLI review entry point
- [`todo/TODO-00-INDEX.md`](../TODO-00-INDEX.md) -- root roadmap ownership
- -> XREF: [`00-infrastructure/TODO-01 §5`](TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle) -- git hook lifecycle and CI/tooling contract (distinct from Claude Code harness hooks in `.claude/settings.json`)
- -> XREF: [`00-infrastructure/TODO-03`](TODO-03-kernel-test-harness.md) -- kernel test harness (separate from the AI workflow regression pack owned here)

## Outcome

- One explicit ownership map for instructions, skills, hooks, and permissions across the Claude + Copilot + Codex surfaces that the repo actually uses.
- A stable skill lifecycle: scaffold, document, validate, cross-link, and retire without drift -- and a clear "Claude-only" stance so no one ports skills sideways to an abandoned tool.
- Hook routing and policy rules that are intentional and reviewable instead of encoded only in a large `settings.json` blob.
- External-reviewer contract: Codex and Copilot are adversarial reviewers invoked from Claude skills, not competing instruction layers. Doctrine lives in `CLAUDE.md` alone.
- An AI workflow regression pack that proves the repo still enforces completion-first behavior and required domain handoffs (complements [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh) which already covers wrapper/hook/YAML drift).

## Implementation Order

| ⭐  | Order | Deliverable                                      | Depends On   | Status |
| --- | :---: | ------------------------------------------------ | ------------ | :----: |
| 💎  |   1   | Canonical ownership and source-of-truth map      | --           |  [ ]   |
| 💎  |   2   | Skill lifecycle, templates, and catalog rules    | §1           |  [ ]   |
| 💎  |   3   | Hook routing and policy contract                 | §1           |  [ ]   |
| 💎  |   4   | External-reviewer contract (Codex, Copilot)      | §1-§3        |  [ ]   |
| 💎  |   5   | MCP, permissions, and extension boundary         | §1, §3, §4   |  [ ]   |
| ⭐  |   6   | AI workflow regression suite                     | §1-§5        |  [ ]   |

> 💎 = parity work: mature engineering repos document ownership, automation rules, and policy boundaries.
> ⭐ = exclusive work: Impossible OS can treat its AI workflow as a first-class subsystem with regression checks, not as untracked prompt folklore.

---

## 1. Canonical Ownership and Source-of-Truth Map

The first job is to state clearly which file owns what.

- [ ] Write an ownership matrix under `docs/infrastructure/` (or as a `docs/infrastructure/ai-system.md` companion to [`development-tooling.md`](../../docs/infrastructure/development-tooling.md)) covering `CLAUDE.md`, `.github/copilot-instructions.md`, `.claude/settings.json`, `.claude/skills/`, and `scripts/copilot-review.sh`
- [ ] Define which doctrine is global (lives in `CLAUDE.md`, owns the product north star, SMP-safe-by-default, bare-metal-first, POST16/boot-path discipline, no Unicode dashes, no live boot infra in tests) and which is tool-local (Claude-Code-specific skill mechanics, Copilot CLI invocation syntax)
- [ ] Document the **Claude Code-only** stance explicitly: "no `.cursor/`, no parallel skill sets; doctrine lives in `CLAUDE.md`; skills live in `.claude/skills/`; external reviewers (Codex, Copilot) are invoked from inside Claude skills, not from separate instruction layers"
- [ ] Replace any stale references in root docs/indexes that still point at Cursor or at missing/superseded AI roadmap files
- [ ] Add explicit "edit here, not there" notes where duplicate concepts currently exist (e.g. doctrine restated in both `CLAUDE.md` and individual skill headers)
- [ ] Commit: `"docs/ai: define canonical ownership map for repo AI system"`

**Test checkpoint:** A contributor can answer "where do I change this rule?" for doctrine, hook behavior, skill content, and Copilot guidance without guessing. Root roadmap links resolve to real files. The "Claude Code-only" stance is stated in one canonical place.

---

## 2. Skill Lifecycle, Templates, and Catalog Rules

Skills need the same rigor as code: discoverable ownership, templates, and retirement rules.

- [ ] Define a standard lifecycle for adding or editing skills: scaffold/template, `description` field wording (auto-load trigger matters; document how to write it), CLAUDE.md Skills table entry, mandatory-trigger table entry when applicable, XREF expectations, and validation path
- [ ] Create or tighten a shared skill authoring template so new skills carry required sections consistently (frontmatter, "Use This Skill When", workflow, guardrails)
- [ ] Document catalog hygiene rules: every live skill must appear in the CLAUDE.md Skills table + the `.claude/skills/README.md` list. A skill whose directory exists but is missing from both indexes is dead weight.
- [ ] Document retirement/supersession rules for stale skills so old prompt blocks do not linger as false owners. Removal = delete the skill directory AND both index entries in the same commit.
- [ ] Document the **"no parallel skill trees" rule** (Cursor-removal learning 2026-04-18): skills live under `.claude/skills/` only; do not create `.cursor/`, `.codex/`, `.other-tool/` skill directories; external tools invoke the Claude skill via the canonical skill entry
- [ ] Commit: `"docs/ai: define skill lifecycle, template, and catalog rules"`

**Test checkpoint:** A new skill can be scaffolded and registered by following one documented path. CLAUDE.md Skills table + `.claude/skills/README.md` + actual skill directories stay in sync. A skill directory without matching index entries is flagged by the regression pack (§6).

---

## 3. Hook Routing and Policy Contract

Hooks are part of the AI system, not invisible glue.

- [ ] Audit `.claude/settings.json` hooks into a readable routing matrix: trigger (`PreToolUse`, `PostToolUse`, `Skill`, `Bash(git commit:*)`), matcher, owner, and intended reminder/gate behavior. One-line-per-hook summary rendered as a table in `docs/infrastructure/ai-system.md`.
- [ ] Document mandatory-trigger hooks separately from advisory reminders: which hooks BLOCK (exit 2) the tool call, which emit `systemMessage` reminders only, which run post-hoc validation (post-commit unit test, post-commit boot smoke)
- [ ] Split large hook concerns into named policy blocks in the routing matrix so edits do not require blind JSON surgery. The settings.json blob stays canonical; the matrix is the human-readable view
- [ ] Document the **Claude Code-only hook boundary**: `.claude/settings.json` hooks are harness hooks (run at tool-call time); `.githooks/` are git hooks (run at commit/push time); `scripts/copilot-review.sh` has no hook surface, it's invoked manually or from inside a skill
- [ ] Add explicit XREFs to [`00-infrastructure/TODO-01 §5`](TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle) for git hook installation/CI surfaces that live outside the AI system itself
- [ ] Commit: `"docs/ai: codify hook routing and policy contract"`

**Test checkpoint:** For any given edit path or skill invocation, the responsible hook and its intended effect are documented. A maintainer can tell whether a behavior is a reminder or a block without reading minified JSON. The harness/git hook boundary is clear.

---

## 4. External-Reviewer Contract (Codex, Copilot)

Codex and Copilot participate in the AI workflow as adversarial reviewers, not as authoritative instruction layers. That contract must be explicit so doctrine drift stays impossible.

- [ ] Document the external-reviewer role: Codex is invoked from within Claude skills (via the OpenAI Codex plugin at `/home/<user>/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs`); Copilot is invoked via `scripts/copilot-review.sh`. Both return findings that Claude applies `superpowers:receiving-code-review` discipline to. Neither edits doctrine.
- [ ] Document the nine Claude skills that dispatch Codex: `codex-adversarial-review-section`, `codex-consistency-audit`, `codex-dead-code`, `codex-design-review`, `codex-fix-review`, `codex-impact-analysis`, `codex-perf-review`, `codex-review-todo`, `codex-test-coverage`. Each has a specific adversarial angle; the skill knows which angles belong in its prompt.
- [ ] Document Copilot's narrower scope: PR-style review comments, mostly for docs and non-kernel code paths. Not used as a first-class reviewer for kernel-critical changes because it lacks the deep-context read Codex gives.
- [ ] State the "reviewer, not authority" invariant: `receiving-code-review` discipline says Codex can be wrong, findings must be technically verified before acted on, false positives rejected with code evidence. This applies identically to Copilot.
- [ ] Add XREFs from every Codex-invoking Claude skill back to this section so the role is discoverable from the skills themselves
- [ ] Commit: `"docs/ai: define external-reviewer contract for Codex and Copilot"`

**Test checkpoint:** A maintainer reading any `codex-*` skill can trace back to the canonical external-reviewer contract. Codex/Copilot findings are always applied via `receiving-code-review`, never blindly. Neither tool is documented as "authority" anywhere in the repo.

---

## 5. MCP, Permissions, and Extension Boundary

Permissions and extensions are part of the architecture. They need clear boundaries and safe defaults.

- [ ] Document the current Claude Code permission model (`allow`/`deny`/`ask` tiers) and how the repo's `.claude/settings.json` uses it: read-only ops auto-allowed, write ops ask, destructive ops explicitly denied
- [ ] Define where project-local extensions or MCP-style integrations belong and where they do not. MCP servers live in user-local config, not repo-tracked files.
- [ ] Add policy notes for secrets, local-only settings, and non-committed machine-specific overrides. `.claude/settings.local.json` is user-local and git-ignored; `.claude/settings.json` is repo-tracked shared policy.
- [ ] Review the boundary between repo-tracked instructions and user-local configuration so shared policy does not leak into personal state
- [ ] Document the `claude-code-guide` subagent and how it maps to the repo's Claude Code-only stance (it's the docs-only guide; agents that modify code live in `Explore`, `general-purpose`, or skill invocations)
- [ ] Add extension/permission maintenance notes to the ownership matrix from §1
- [ ] Commit: `"docs/ai: define MCP, permissions, and extension boundary"`

**Test checkpoint:** A maintainer can tell which settings are safe to commit, which are local-only, and where to add new project-level AI capabilities without violating policy boundaries. The `.local` vs shared split is explicit.

---

## 6. AI Workflow Regression Suite

This is the refinement step: test the workflow itself.

> [!TIP]
> Public Windows and Linux projects may publish contribution rules, but they rarely ship a regression pack that checks whether the AI assistance layer still enforces the project's own doctrine. The existing `scripts/test-tooling.sh` already covers wrapper/hook/YAML drift; this pack complements it by covering skill catalog + doctrine surfaces specifically.

- [ ] Add an AI workflow regression pack (`scripts/test-ai-system.sh`) that validates key invariants:
  - **Skill catalog consistency**: every directory under `.claude/skills/` has a `SKILL.md` AND an entry in the CLAUDE.md Skills table AND (if auto-loading) an entry in `.claude/skills/README.md` / the system-reminder skill list. Orphan directories = fail.
  - **Doctrine presence**: CLAUDE.md contains the completion-first wording, the bare-metal-first paragraph, the no-Unicode-dashes rule, the no-live-boot-infra-in-tests rule, the SMP-from-day-one rule. Drop-word check against a short canonical list.
  - **Root-index link integrity**: all AI-system links in `todo/TODO-00-INDEX.md`, `todo/00-infrastructure/INDEX.md`, and `CLAUDE.md` resolve to real files.
  - **No-Cursor-residue check**: `.cursor/` does not exist; no `.cursor/`-prefixed path is referenced in any tracked file outside explicit history notes marked `removed 2026-04-18`.
  - **Hook JSON parse**: `.claude/settings.json` is valid JSON (python3 json.load check).
  - **Copilot instructions present**: `.github/copilot-instructions.md` exists and references the external-reviewer contract from §4.
- [ ] Make regression output actionable: print exactly which doctrine surface drifted and where to repair it. Same `t_pass` / `t_fail` pattern as `scripts/test-tooling.sh`.
- [ ] Wire the regression pack into `make test-ai-system` + `.github/workflows/build.yml` so AI workflow regressions are caught before contributors hit them manually
- [ ] Document how to extend the regression suite when new tools/skill layers are added. The pack should be as easy to extend as `test-tooling.sh` was.
- [ ] Commit: `"test/ai: add AI workflow regression suite"`

**Test checkpoint:** On a healthy repo, the AI regression suite passes. If CLAUDE.md loses a doctrine paragraph, a skill directory appears without a Skills-table entry, or a `.cursor/` path sneaks back in via a copy-paste, the suite fails with a specific repair path.

---

## OS Comparison

| ⭐ | Feature                         | 🪟 Win11 projects       | 🐧 Linux projects      | 🚀 Impossible OS |
| --- | ------------------------------- | ---------------------- | ---------------------- | ---------------- |
| 💎 | Repo-codified AI instructions   | ⚠️ Emerging practice   | ⚠️ Emerging practice   | ⬜ §1            |
| 💎 | Skill/catalog ownership         | ❌ Often ad hoc        | ❌ Often ad hoc        | ⬜ §2            |
| 💎 | Hook policy matrix              | ⚠️ Usually implicit    | ⚠️ Usually implicit    | ⬜ §3            |
| 💎 | External-reviewer contract      | ❌ Rare                | ❌ Rare                | ⬜ §4            |
| 💎 | Permissions/extension boundary  | ⚠️ Varies by repo      | ⚠️ Varies by repo      | ⬜ §5            |
| ⭐ | AI workflow regression suite    | ❌ Rare in practice    | ❌ Rare in practice    | ⬜ §6            |

> **After §1-§5:** Impossible OS documents its (Claude Code-only) AI workflow as first-class infrastructure: explicit ownership, documented hooks, and a clear external-reviewer contract.
> **After §6:** the repo surpasses typical public practice by testing the AI workflow itself as maintained infrastructure.

## Unit Tests

> AI workflow checks are host-side regression checks, not kernel `test_runner_init()` suites. They complement the wrapper/hook/YAML checks already in `scripts/test-tooling.sh` (owned by [TODO-01 §10](TODO-01-developer-tooling-stack.md#10-smoke-test-post16-assertions) Unit Tests) by covering the AI-specific surfaces.

- [ ] Create `scripts/test-ai-system.sh` with:
  - skill catalog consistency (every `.claude/skills/*` dir is indexed in CLAUDE.md + README)
  - doctrine-word-presence checks across `CLAUDE.md` (completion-first, bare-metal-first, SMP-from-day-one, no-Unicode-dashes, no-live-boot-infra-in-tests)
  - root/index link resolution for all AI-system roadmap files
  - no-`.cursor/`-residue check (removed 2026-04-18; should not reappear via copy-paste)
  - `.claude/settings.json` valid JSON
  - `.github/copilot-instructions.md` exists and references external-reviewer contract
- [ ] Wire the AI regression pack into `make test-ai-system` + `.github/workflows/build.yml`
- [ ] Commit: `"test/ai: add regression checks for repo AI workflow"`

## Verification

- [ ] Root roadmap and `00-infrastructure/INDEX.md` point only at real AI/tooling TODO files
- [ ] Ownership matrix clearly maps doctrine, skills, hooks, and permissions
- [ ] External-reviewer contract is documented and linked from every `codex-*` skill
- [ ] `scripts/test-ai-system.sh` fails on missing links, skill-catalog drift, or `.cursor/`-residue and passes on a healthy repo
- [ ] Hook policy documentation explains which behaviors are reminders versus hard blocks
- [ ] The Claude Code-only stance is stated once in `CLAUDE.md` and referenced from this TODO; no parallel skill tree exists under `.cursor/`, `.codex/`, or similar
