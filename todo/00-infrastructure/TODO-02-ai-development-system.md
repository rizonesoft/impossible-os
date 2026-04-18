# TODO-02 -- AI Development System

> **Goal:** Turn the repo's AI workflow into an explicit, maintainable system instead of a pile of overlapping instructions. Impossible OS is **Claude Code-only** as of 2026-04-18; Cursor was removed because maintaining a parallel skill set under `.cursor/` created clutter without a corresponding productivity win. This TODO defines the canonical source-of-truth map for the Claude + Copilot + Codex surfaces that remain, the skill lifecycle, hook policy, and a regression check that proves the AI layer still matches repo doctrine.

## Authority Hierarchy (read this first)

> **Claude Code is the master.** Everything else in the AI surface is subordinate: doctrine files tell Claude what to do, skills tell Claude how to do it, external reviewers tell Claude what might be wrong. Nothing outside Claude Code edits code, commits, or makes scope decisions autonomously. When this TODO uses the term "source of truth" it always identifies WHICH file or tool owns a particular kind of authority, never implies anything is co-equal with Claude Code.

| Layer                     | Role                                           | Authority over                                      |
| ------------------------- | ---------------------------------------------- | --------------------------------------------------- |
| **Claude Code (tool)**    | MASTER / orchestrator                          | All code edits, commits, skill invocations, reviewer dispatches |
| `CLAUDE.md`               | Doctrine source-of-truth (file)                | Product north star, workflow rules, safety constraints, policy |
| `.claude/skills/`         | Workflow source-of-truth (directory)           | How Claude executes a specific task (implement, review, verify, diagnose) |
| `.claude/settings.json`   | Harness policy source-of-truth (file)          | Permissions, hook reminders, pre/post-tool-use gates |
| Codex (OpenAI plugin)     | Subordinate reviewer                           | Adversarial findings only; invoked from inside `codex-*` Claude skills; findings go through `receiving-code-review` before action |
| Copilot CLI               | Subordinate reviewer                           | External PR-style review; invoked via `scripts/copilot-review.sh`; same `receiving-code-review` discipline |
| `.github/copilot-instructions.md` | Copilot-CLI repo instructions        | Only configures how Copilot answers when invoked; does NOT add new doctrine |
| `.githooks/`              | Git-time guards (distinct layer, see TODO-01 §5) | Pre-commit lint, post-commit COUNT, opt-in pre-push |

**Hierarchy invariants:**
1. **Doctrine lives in `CLAUDE.md`. Nowhere else.** Skill headers, tool instructions, and regression messages reference doctrine but do not redefine it. Edits go to `CLAUDE.md` first, then propagate.
2. **Skills live in `.claude/skills/` only.** No parallel skill trees (`.cursor/`, `.codex/`, `.other-tool/` etc.). External tools that want to participate do so through a Claude skill that dispatches them.
3. **External reviewers return findings, never edits.** Codex and Copilot output is information Claude reads and judges. The commit/edit decision stays with Claude under `superpowers:receiving-code-review` discipline.
4. **CLAUDE.md wins on conflict.** If a skill, a hook message, or an external-tool config contradicts `CLAUDE.md`, `CLAUDE.md` is right and the other layer is the bug. Fix the drift, don't fork the doctrine.
5. **Claude Code is also the interactive agent.** A human operator talks to Claude; Claude dispatches subordinates. Treating Codex or Copilot as a direct-edit or direct-commit tool violates the hierarchy.

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
- [`AGENTS.md`](../../AGENTS.md) -- cross-tool pointer (ships in §6; referenced here so Inputs is self-consistent after §6 lands)
- [`CONTRIBUTING.md`](../../CONTRIBUTING.md) -- contributor-facing doc; candidate home for the §7 zero-trailer commit policy
- -> XREF: [`00-infrastructure/TODO-01 §5`](TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle) -- git hook lifecycle and CI/tooling contract (distinct from Claude Code harness hooks in `.claude/settings.json`)
- -> XREF: [`00-infrastructure/TODO-03`](TODO-03-kernel-test-harness.md) -- kernel test harness (separate from the AI workflow regression pack owned here)
- External reference: [Linux kernel Documentation/process/coding-assistants.rst](https://docs.kernel.org/process/coding-assistants.html) -- 2025-12 precedent for `Assisted-by:` trailers; §7 documents Impossible OS's deliberate divergence.
- External reference: [AGENTS.md open standard](https://agents.md/) -- Linux Foundation-stewarded cross-tool instruction file; §6 adopts the convention.

## Outcome

- One explicit ownership map for instructions, skills, hooks, and permissions across the Claude + Copilot + Codex surfaces that the repo actually uses.
- A stable skill lifecycle: scaffold, document, validate, cross-link, and retire without drift -- and a clear "Claude-only" stance so no one ports skills sideways to an abandoned tool.
- Hook routing and policy rules that are intentional and reviewable instead of encoded only in a large `settings.json` blob.
- External-reviewer contract: Codex and Copilot are adversarial reviewers invoked from Claude skills, not competing instruction layers. Doctrine lives in `CLAUDE.md` alone.
- Cross-tool pointer file (`AGENTS.md`) that makes the Authority Hierarchy discoverable from non-Claude tool surfaces (Codex CLI, Aider, Continue, Gemini-CLI, Zed) without duplicating doctrine.
- Explicit AI-assist commit disclosure policy: zero-trailer stance documented in contrast to the Linux kernel 2025-12 `Assisted-by:` convention, with the stance-change condition named so the policy is author-by-decision, not oversight.
- Autonomous-agent boundary policy: the repo explicitly states it does NOT accept autonomous coding-agent PRs (Copilot coding-agent, Devin, Cognition) and does NOT ship the files those workflows require (`copilot-setup-steps.yml`, `.github/agents/`, `.github/chatmodes/`). The refusal is documented, not implicit.
- An AI workflow regression pack that proves the repo still enforces completion-first behavior, required domain handoffs, and the three new policy boundaries (cross-tool pointer, zero-trailer, autonomous-agent refusal). Complements [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh) which already covers wrapper/hook/YAML drift.

## Implementation Order

| ⭐  | Order | Deliverable                                        | Depends On   | Status |
| --- | :---: | -------------------------------------------------- | ------------ | :----: |
| 💎  |   1   | Canonical ownership and source-of-truth map        | --           |  [x]   |
| 💎  |   2   | Skill lifecycle, templates, and catalog rules      | §1           |  [x]   |
| 💎  |   3   | Hook routing and policy contract                   | §1           |  [ ]   |
| 💎  |   4   | External-reviewer contract (Codex, Copilot)        | §1-§3        |  [ ]   |
| 💎  |   5   | MCP, permissions, and extension boundary           | §1, §3, §4   |  [ ]   |
| 💎  |   6   | `AGENTS.md` cross-tool pointer file                | §1, §4       |  [ ]   |
| 💎  |   7   | AI-assist commit disclosure policy                 | §1           |  [ ]   |
| ⭐  |   8   | Autonomous-agent boundary policy                   | §4, §5       |  [ ]   |
| ⭐  |   9   | AI workflow regression suite                       | §1-§8        |  [ ]   |

> 💎 = parity work: mature engineering repos document ownership, automation rules, and policy boundaries.
> ⭐ = exclusive work: Impossible OS can treat its AI workflow as a first-class subsystem with regression checks, not as untracked prompt folklore.

---

## 1. Canonical Ownership and Source-of-Truth Map

The first job is to state clearly which file owns what.

- [x] Wrote ownership matrix at [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) covering `CLAUDE.md`, `.github/copilot-instructions.md`, `.claude/settings.json`, `.claude/skills/`, `scripts/copilot-review.sh`, `.githooks/`, Codex plugin, and Copilot CLI. The doc opens with the Authority Hierarchy table mirrored from the top of this TODO (same rows, same roles, same authority scope; wording normalized for a docs reader -- "this page" in place of "this TODO" in one sentence). Claude Code = MASTER; CLAUDE.md = doctrine SoT; Codex/Copilot = subordinate reviewers.
- [x] Split doctrine into Global (lives in `CLAUDE.md`, 11 rows covering north star, bare metal first, SMP-from-day-one, POST16 boot-path-only, no Unicode dashes, freestanding kernel, Win32 API, test no-live-boot, Bare Metal Gotchas, Safety Gates, Mandatory Skill Triggers) vs Tool-Local (5 rows: skill SKILL.md files, `settings.json`, Copilot instructions, Codex plugin templates, `copilot-review.sh`). Every global-doctrine row links to the exact `CLAUDE.md` anchor that owns it.
- [x] "Claude Code-Only Stance" section in `ai-system.md` explicitly lists: no `.cursor/`, no parallel skill sets; doctrine in `CLAUDE.md`; skills in `.claude/skills/`; external reviewers (Codex, Copilot) invoked from inside Claude skills; any future AI tool goes through the external-reviewer contract (§4) with `receiving-code-review` discipline.
- [x] "Hierarchy Invariants" block in `ai-system.md` restates the 5 invariants verbatim from this TODO's Authority Hierarchy block, so the invariants are discoverable from both locations.
- [x] Fixed stale `todo/TODO-00-INDEX.md` line 42 ("Claude, Cursor, and Copilot" -> "Claude Code master; Codex + Copilot subordinate reviewers"). No other stale Cursor or Antigravity references remain in root docs (README.md/CLAUDE.md "cursor" hits are the graphical mouse cursor, not the AI tool; verified via grep).
- [x] "Edit-Here-Not-There Rules" table in `ai-system.md` names 7 concepts with their canonical edit location and the downstream consumers that must NOT be edited to change the concept (doctrine, skill workflow, harness policy, Copilot guidance, Codex dispatch templates, git-hook lifecycle, TODO workflow). Also added a link to `ai-system.md` from `CLAUDE.md` "Skills" section so contributors reach the matrix from the doctrine file.
- [x] Added `ai-system.md` row to [`docs/infrastructure/index.md`](../../docs/infrastructure/index.md) so the doc is discoverable from the Infrastructure landing page.
- [x] Commit: `"docs/ai: define canonical ownership map for repo AI system"`

**Test checkpoint:** A contributor can answer "where do I change this rule?" for doctrine, hook behavior, skill content, and Copilot guidance without guessing. Root roadmap links resolve to real files. The "Claude Code-only" stance is stated in one canonical place ([`docs/infrastructure/ai-system.md` Claude Code-Only Stance](../../docs/infrastructure/ai-system.md#claude-code-only-stance)).

> **Test runner:** N/A (docs-only) | validation: markdown links resolve, 4 stale refs removed (copilot-instructions.md `.cursor/` row, TODO-00-INDEX.md line 42, 00-infrastructure/INDEX.md line 32, CLAUDE.md Skills section updated with matrix link); §9 regression suite will add automated checks when it ships.

> **Verified:** 2026-04-19 | commit `52b2a440` | 7/7 items | build N/A (docs-only) | 8 files changed | 5 stale Cursor refs swept
> **Quality reviewed:** 2026-04-19 | Codex 3x (adversarial x2, consistency) | 4M+2L fixed (Cursor row in copilot-instructions, fake scripts/codex-companion.mjs path, stale 13-count dropped, verbatim->mirrored stamp, false README link claim, duplicate invariant #5 in §8), 0 open | scope: N/A (docs-only)

---

## 2. Skill Lifecycle, Templates, and Catalog Rules

Skills need the same rigor as code: discoverable ownership, templates, and retirement rules.

- [x] 5-step lifecycle documented in [`docs/infrastructure/skill-authoring.md`](../../docs/infrastructure/skill-authoring.md): (1) scaffold from `TEMPLATE.md`; (2) write `description` field as action-verb + concrete triggers (auto-load signal is the description; document "Auto-loads when..." when a harness hook invokes it); (3) add row to CLAUDE.md Skills table; (4) add Mandatory Skill Triggers row + settings.json hook when the skill MUST run under specific conditions; (5) add row to `.claude/skills/README.md` catalog.
- [x] Canonical template at [`.claude/skills/TEMPLATE.md`](../../.claude/skills/TEMPLATE.md): frontmatter (`name`, `description`), H1 title, `## Use This Skill When`, `## Workflow` (or Pipeline), `## Guardrails` (or Rules). Optional: `## Execution Discipline`, `## Additional Resources`, `## Relationship to <other-skill>`. Inline reminders (ASCII `--` only, no doctrine restatement, no `[Opus]`/`[Sonnet]` tags) live in a strip-before-commit HTML comment block.
- [x] Catalog hygiene rules documented in `skill-authoring.md` "Catalog Hygiene" section: three canonical locations (`.claude/skills/<slug>/`, CLAUDE.md Skills table, `.claude/skills/README.md`) must stay in sync; a directory without matching index rows is drift. Ran the sync check -- 5 missing CLAUDE.md rows fixed in the same commit (added `audit-ssdt`, `codex-adversarial-review-section`, `gap-analysis-todo`, `kernel-code-quality`, `validate-todo-section`). Expanded `.claude/skills/README.md` from 3-line stub to full catalog grouping all 28 live skills.
- [x] Retirement / supersession sweep documented as 7-step process in `skill-authoring.md` "Retirement / Supersession": `git rm -rf <slug>` + delete CLAUDE.md row + delete README.md row + delete Mandatory Skill Triggers row + delete settings.json hook + grep-and-clean XREF fallout + land in one commit. Renames = retire-old + add-new, not in-place edit.
- [x] "No parallel skill trees" rule stated as a hard rule at the top of `skill-authoring.md`, linking to Authority Hierarchy invariant #2 in `ai-system.md`. Cross-referenced from CLAUDE.md Skills section via the new "Adding, editing, or retiring a skill" pointer line below the table.
- [x] Commit: `"docs/ai: define skill lifecycle, template, and catalog rules"`

**Test checkpoint:** A new skill can be scaffolded and registered by following one documented path ([`skill-authoring.md`](../../docs/infrastructure/skill-authoring.md) Lifecycle steps 1-5). CLAUDE.md Skills table + `.claude/skills/README.md` + actual skill directories stay in sync (verified: 28 directories, 28 CLAUDE.md rows, 28 README.md rows as of this commit). A skill directory without matching index entries will be flagged by the regression pack ([§9](#9-ai-workflow-regression-suite)).

> **Test runner:** N/A (docs + catalog sync) | validation: `ls .claude/skills/ | wc -l` == rows in CLAUDE.md Skills table == rows in `.claude/skills/README.md`; §9 regression suite will assert this automatically when it ships.

> **Verified:** 2026-04-19 | 6/6 items | build N/A (docs-only) | 7 files changed | 28/28/28 skill sync
> **Quality reviewed:** 2026-04-19 | Codex 1x (adversarial) | 2M fixed (retirement sweep missed bare-name forms; `## Use This Skill When` contract contradicted 2 live high-rigor skills), 0 open | scope: N/A (docs-only)

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

## 6. `AGENTS.md` Cross-Tool Pointer File

The `AGENTS.md` convention emerged 2025 and was stewarded by the Agentic AI Foundation under the Linux Foundation (Aug 2025). It is tool-neutral and read by Codex, Copilot, Aider, Continue, Gemini-CLI, Zed, and others. Impossible OS uses `CLAUDE.md` as authoritative doctrine (Authority Hierarchy row 2), so `AGENTS.md` exists as a thin stub that (a) tells cross-tool readers where to find the real doctrine, (b) surfaces the subordinate-reviewer contract so tools used as reviewers see our rules, and (c) keeps the Claude-Code-only stance discoverable from the cross-tool entry point. `AGENTS.md` NEVER redefines doctrine; it points at `CLAUDE.md`.

- [ ] Create `AGENTS.md` at repo root. First paragraph states: "Impossible OS uses Claude Code as its master/orchestrator. Doctrine lives in `CLAUDE.md` -- read it first. This file is a pointer for non-Claude tools (Codex, Copilot, Aider, Continue, Gemini-CLI, Zed) used in subordinate reviewer or reader roles."
- [ ] Second paragraph: verbatim copy of the Authority Hierarchy one-sentence master statement + a compact 3-bullet summary of the 5 invariants (doctrine in `CLAUDE.md` only; reviewers return findings not edits; `CLAUDE.md` wins on conflict). Full table lives in `CLAUDE.md` + this TODO; `AGENTS.md` references both.
- [ ] Third paragraph: "If you are an autonomous coding agent, stop here -- see §8 Autonomous-agent boundary policy. Impossible OS commits are authored by human + Claude Code only."
- [ ] Link back to this TODO (`todo/00-infrastructure/TODO-02-ai-development-system.md`) as the authoritative AI-workflow roadmap.
- [ ] Limit `AGENTS.md` to ~60 lines. Any new doctrine text that tempts addition here is a signal it belongs in `CLAUDE.md` instead.
- [ ] Add regression-pack check (§9): `AGENTS.md` exists, points at `CLAUDE.md`, does not restate doctrine verbatim (drift guard).
- [ ] Commit: `"docs/ai: add AGENTS.md cross-tool pointer to CLAUDE.md"`

**Test checkpoint:** A Codex/Aider/Continue user pointed at the repo root reads `AGENTS.md`, follows the pointer to `CLAUDE.md`, and understands their role as subordinate reviewer without needing to re-learn doctrine. `AGENTS.md` remains under 60 lines at every commit.

---

## 7. AI-Assist Commit Disclosure Policy

The Linux kernel merged `Documentation/process/coding-assistants.rst` on 2025-12-23 (Sasha Levin, approved by Corbet) requiring AI-assisted patches to carry an `Assisted-by: AGENT_NAME:MODEL_VERSION [TOOL]` trailer alongside the human `Signed-off-by:`. Fedora adopted a similar policy October 2025. Impossible OS is a kernel project and operates in the same space but has an explicit `feedback_no_coauthor` user preference: commits do NOT carry `Co-Authored-By: Claude` or equivalent attribution. This section documents the project's stance explicitly so it is neither accidental nor ambiguous, and names the condition under which the stance would change.

- [ ] Create a short "AI-assisted commit policy" block in `CLAUDE.md` (or in `CONTRIBUTING.md` if a contributor-facing file exists) stating the current stance: (a) the project is Claude Code-orchestrated by design -- attribution is implicit in the project identity; (b) commits are NOT to carry `Co-Authored-By:`, `Assisted-by:`, or similar AI-attribution trailers; (c) this differs from the Linux-kernel 2025-12 convention intentionally; (d) the policy is authored-by-decision, not by oversight.
- [ ] State the "stance change condition": if Impossible OS ever accepts external AI-assisted contributions from non-project-members, the project adopts `Assisted-by: TOOL:MODEL` trailers per the Linux kernel grammar. Until then, the zero-trailer stance stands.
- [ ] Document the telltale-AI-patch screening bar: generic commits that don't match our stamp format, phantom helpers, pointless refactors "for consistency" without a named caller, commits that skip the Codex adversarial-review step when a section was implemented -- all caught by the existing review workflow (implement-todo-section steps 13-18). Name this as the project's equivalent of the Linux "AI slop" screening.
- [ ] Cross-reference Linux's [`Documentation/process/coding-assistants.rst`](https://docs.kernel.org/process/coding-assistants.html) so a future maintainer comparing conventions sees the precedent and the diff.
- [ ] Regression-pack check (§9): `CLAUDE.md` or `CONTRIBUTING.md` contains the zero-trailer policy AND the stance-change condition. If either disappears, fail with a specific repair pointer.
- [ ] Commit: `"docs/ai: document zero-trailer AI-assist commit policy + stance-change condition"`

**Test checkpoint:** A reviewer reading any recent commit sees no AI-attribution trailer. A future contributor asking "why no Co-Authored-By? why no Assisted-by?" finds a direct written answer in doctrine (CLAUDE.md / CONTRIBUTING.md), linked to the Linux precedent and the stance-change condition.

---

## 8. Autonomous-Agent Boundary Policy

Copilot's coding-agent (the autonomous "file the issue, get a PR" agent), Devin, and equivalent autonomous agents operate by running tasks in sandboxes and opening PRs without per-step human authorship. Microsoft ships `.github/workflows/copilot-setup-steps.yml` + a default-on firewall allowlist for this mode. Authority Hierarchy invariant #5 (see top of this TODO) owns the interactive-agent boundary; this section documents how that boundary is enforced as repo policy and what the repo explicitly does NOT ship as a consequence.

- [ ] Document in `CLAUDE.md` and `docs/infrastructure/ai-system.md` (ships with §1): Impossible OS does NOT use autonomous coding agents. No Copilot coding-agent, no Devin, no Cognition-style agents open PRs on this repo. Every commit is authored by a human + Claude Code interactive session.
- [ ] State the reasons: (a) kernel-critical code requires the `implement-todo-section` Steps 13-18 review pipeline that autonomous agents don't follow; (b) the no-parallel-skill-trees invariant means autonomous agents would run without the domain code-quality gates + `receiving-code-review` discipline that are central to the project; (c) the `feedback_no_substandard_code` memory explicitly rejects "works on QEMU" shortcuts that autonomous agents produce by default.
- [ ] Explicitly state what the repo does NOT ship as a consequence: no `.github/workflows/copilot-setup-steps.yml`, no Copilot coding-agent firewall allowlist, no `.github/agents/*.agent.md` profiles. These are legitimate files in repos that accept autonomous agents; their absence here is deliberate.
- [ ] Name the "if this changes" condition: autonomous-agent support lands as a new TODO with its own review pipeline, firewall allowlist, and `copilot-setup-steps.yml` equivalent. Until then, any PR that appears to be autonomous-agent-authored fails review.
- [ ] Name the MCP server policy as a corollary: MCP servers that can autonomously commit, push, or execute long-running tasks without per-step human approval are forbidden from this repo's `.claude/settings.json` and any user-local config used against this repo. Read-only MCP servers (filesystem, git-read, github-read) are fine.
- [ ] Regression-pack check (§9): absence of `.github/workflows/copilot-setup-steps.yml`, absence of `.github/agents/` directory, absence of `.github/chatmodes/` directory. Presence of any of these three paths triggers a policy-drift finding.
- [ ] Commit: `"docs/ai: document autonomous-agent boundary policy -- interactive only"`

> [!TIP]
> This is competitive work. Mature Win11/Linux repos document whether they accept autonomous-agent PRs; fewer document WHY and what they refuse to ship as a consequence. Making the refusal explicit (and linking it to the Authority Hierarchy) keeps the Claude-Code-only stance enforceable long-term.

**Test checkpoint:** A maintainer can state the project's autonomous-agent stance in one sentence with a direct citation. A contributor attempting to enable Copilot coding-agent or Devin against this repo finds a documented refusal path before they waste cycles trying. The regression pack catches a drift where someone drops a `copilot-setup-steps.yml` into `.github/workflows/`.

---

## 9. AI Workflow Regression Suite

This is the refinement step: test the workflow itself.

> [!TIP]
> Public Windows and Linux projects may publish contribution rules, but they rarely ship a regression pack that checks whether the AI assistance layer still enforces the project's own doctrine. The existing `scripts/test-tooling.sh` already covers wrapper/hook/YAML drift; this pack complements it by covering skill catalog + doctrine surfaces specifically.

- [ ] Add an AI workflow regression pack (`scripts/test-ai-system.sh`) that validates key invariants:
  - **Authority Hierarchy present**: `todo/00-infrastructure/TODO-02-ai-development-system.md` and `docs/infrastructure/ai-system.md` both contain the "Claude Code is the master" statement and the 5 hierarchy invariants (grep for each invariant's first phrase). Drift or deletion = fail with a pointer to this section.
  - **Claude-Code-only declaration in CLAUDE.md**: `CLAUDE.md` names "Claude Code-only" and references the removal of `.cursor/` on 2026-04-18. If this line disappears, the authority doctrine has silently fragmented and the regression catches it.
  - **`AGENTS.md` pointer file (§6)**: `AGENTS.md` exists at repo root, contains the phrase "Claude Code" and a link to `CLAUDE.md`, stays under 60 lines, and does NOT restate doctrine (byte-compare: `AGENTS.md` must be ~10x shorter than `CLAUDE.md` and must not copy any of `CLAUDE.md`'s doctrine paragraphs verbatim). Drift guard: a future edit that copy-pastes doctrine into `AGENTS.md` fails the length check.
  - **Zero-trailer commit policy (§7)**: `CLAUDE.md` or `CONTRIBUTING.md` contains the "no `Co-Authored-By:` / no `Assisted-by:`" policy statement AND the stance-change condition. A scan of the last 100 commits via `git log --format=%B HEAD~100..HEAD` flags any commit body containing `Co-Authored-By:` / `Assisted-by:` / `AI-Author:` as a policy violation.
  - **Autonomous-agent boundary (§8)**: `.github/workflows/copilot-setup-steps.yml` does NOT exist, `.github/agents/` does NOT exist, `.github/chatmodes/` does NOT exist. Presence of any of these three triggers a policy-drift finding with a pointer to §8.
  - **Skill catalog consistency**: every directory under `.claude/skills/` has a `SKILL.md` AND an entry in the CLAUDE.md Skills table AND (if auto-loading) an entry in `.claude/skills/README.md` / the system-reminder skill list. Orphan directories = fail.
  - **Doctrine presence**: CLAUDE.md contains the completion-first wording, the bare-metal-first paragraph, the no-Unicode-dashes rule, the no-live-boot-infra-in-tests rule, the SMP-from-day-one rule. Drop-word check against a short canonical list. (Why: these are the doctrine lines `CLAUDE.md` authoritatively owns; if any disappear, the hierarchy is hollow -- Claude-Code-the-master has no doctrine to enforce.)
  - **Root-index link integrity**: all AI-system links in `todo/TODO-00-INDEX.md`, `todo/00-infrastructure/INDEX.md`, and `CLAUDE.md` resolve to real files.
  - **No-Cursor-residue check**: `.cursor/` does not exist; no `.cursor/`-prefixed path is referenced in any tracked file outside explicit history notes marked `removed 2026-04-18`.
  - **No-parallel-skill-tree check**: no directory matching `.codex/skills/`, `.copilot/skills/`, `.other-tool/skills/` etc. exists. The "no parallel skill trees" invariant (hierarchy #2) must stay enforced mechanically, not just in prose.
  - **Hook JSON parse**: `.claude/settings.json` is valid JSON (python3 json.load check).
  - **Copilot instructions present**: `.github/copilot-instructions.md` exists, references the external-reviewer contract from §4, and explicitly states its subordinate role (does not contain doctrine that contradicts or competes with `CLAUDE.md`).
- [ ] Make regression output actionable: print exactly which doctrine surface drifted and where to repair it. Same `t_pass` / `t_fail` pattern as `scripts/test-tooling.sh`.
- [ ] Wire the regression pack into `make test-ai-system` + `.github/workflows/build.yml` so AI workflow regressions are caught before contributors hit them manually
- [ ] Document how to extend the regression suite when new tools/skill layers are added. The pack should be as easy to extend as `test-tooling.sh` was.
- [ ] Commit: `"test/ai: add AI workflow regression suite"`

**Test checkpoint:** On a healthy repo, the AI regression suite passes. If CLAUDE.md loses a doctrine paragraph, a skill directory appears without a Skills-table entry, or a `.cursor/` path sneaks back in via a copy-paste, the suite fails with a specific repair path.

---

## OS Comparison

| ⭐ | Feature                         | 🪟 Win11                                    | 🐧 Linux                                   | 🚀 Impossible OS                                         |
| --- | ------------------------------- | ------------------------------------------- | ------------------------------------------- | -------------------------------------------------------- |
| 💎 | Repo-codified AI instructions   | ✅ `.github/copilot-instructions.md`        | ⚠️ AGENTS.md emerging                      | ✅ §1 ownership map + `CLAUDE.md` authority              |
| 💎 | Skill/catalog ownership         | ⚠️ `.github/chatmodes/` + `/prompts/`       | ❌ Ad hoc                                  | ✅ §2 skill lifecycle; no parallel trees                 |
| 💎 | Hook policy matrix              | ⚠️ `.vscode/mcp.json` + IDE settings        | ⚠️ Implicit                                | ⬜ §3 routing; reminder vs block tiers                   |
| 💎 | External-reviewer contract      | ❌ Rare                                     | ❌ Rare                                    | ⬜ §4 Codex + Copilot reviewer-not-authority             |
| 💎 | Permissions/extension boundary  | ⚠️ Varies                                   | ⚠️ Varies                                  | ⬜ §5 allow/deny tiers + `.local.json` split             |
| 💎 | Cross-tool `AGENTS.md` pointer  | ⚠️ awesome-copilot stub                     | ✅ LF-backed (Aug 2025)                    | ⬜ §6 pointer to `CLAUDE.md`; no duplication             |
| 💎 | AI-assist commit disclosure     | ❌ No convention                            | ✅ kernel `Assisted-by:` trailer (2025-12) | ⬜ §7 zero-trailer policy + stance-change condition      |
| ⭐ | Autonomous-agent boundary       | ⚠️ coding-agent + firewall allowlist        | ❌ No formal policy                        | ⬜ §8 interactive-only; no Devin / setup-steps           |
| ⭐ | AI workflow regression suite    | ❌ Rare                                     | ❌ Rare (Promptfoo/Guardrails; AI apps)    | ⬜ §9 catalog + hierarchy + trailer + boundary checks    |

> **After §1-§5:** Impossible OS documents its (Claude Code-only) AI workflow as first-class infrastructure: explicit ownership, documented hooks, and a clear external-reviewer contract.
> **After §6-§7:** reaches parity with the 2025-2026 Linux Foundation `AGENTS.md` standard and the Linux kernel's AI-assist commit policy, while preserving its zero-trailer stance explicitly.
> **After §8-§9:** the repo surpasses typical public practice by stating its autonomous-agent boundary (what it refuses to ship as a consequence of the Authority Hierarchy) and by testing the AI workflow itself as maintained infrastructure.

## Unit Tests

> AI workflow checks are host-side regression checks, not kernel `test_runner_init()` suites. They complement the wrapper/hook/YAML checks already in `scripts/test-tooling.sh` (owned by [TODO-01 §10](TODO-01-developer-tooling-stack.md#10-smoke-test-post16-assertions) Unit Tests) by covering the AI-specific surfaces.

- [ ] Create `scripts/test-ai-system.sh` with:
  - skill catalog consistency (every `.claude/skills/*` dir is indexed in CLAUDE.md + README)
  - doctrine-word-presence checks across `CLAUDE.md` (completion-first, bare-metal-first, SMP-from-day-one, no-Unicode-dashes, no-live-boot-infra-in-tests)
  - Authority Hierarchy present in TODO-02 + `docs/infrastructure/ai-system.md` (ships with §1)
  - `AGENTS.md` exists + under 60 lines + points to CLAUDE.md + does not restate doctrine (§6 regression)
  - Zero-trailer policy documented + last-100-commits scan for `Co-Authored-By:` / `Assisted-by:` violations (§7 regression)
  - Autonomous-agent boundary enforced: `.github/workflows/copilot-setup-steps.yml` / `.github/agents/` / `.github/chatmodes/` do NOT exist (§8 regression)
  - root/index link resolution for all AI-system roadmap files
  - no-`.cursor/`-residue check (removed 2026-04-18; should not reappear via copy-paste)
  - no-parallel-skill-tree check (`.codex/skills/`, `.copilot/skills/`, etc. do not exist)
  - `.claude/settings.json` valid JSON
  - `.github/copilot-instructions.md` exists and references external-reviewer contract
- [ ] Wire the AI regression pack into `make test-ai-system` + `.github/workflows/build.yml`
- [ ] Commit: `"test/ai: add regression checks for repo AI workflow"`

## Verification

- [ ] **Authority Hierarchy is stated at the top of this TODO**, in `docs/infrastructure/ai-system.md` (once §1 lands), and in a one-sentence reference from `CLAUDE.md`. A contributor asking "is Claude Code the master?" finds a direct "yes, and here's what 'master' means" answer within 30 seconds.
- [ ] The 5 hierarchy invariants (doctrine in `CLAUDE.md` only; skills in `.claude/skills/` only; reviewers return findings, not edits; `CLAUDE.md` wins on conflict; Claude Code is also the interactive agent) appear in both `TODO-02` and `ai-system.md`; `scripts/test-ai-system.sh` asserts both locations.
- [ ] Root roadmap and `00-infrastructure/INDEX.md` point only at real AI/tooling TODO files
- [ ] Ownership matrix clearly maps doctrine, skills, hooks, and permissions -- and opens with the Authority Hierarchy table before any other row
- [ ] External-reviewer contract is documented and linked from every `codex-*` skill
- [ ] `scripts/test-ai-system.sh` fails on missing links, skill-catalog drift, `.cursor/`-residue, hierarchy-block deletion, or parallel-skill-tree creation; passes on a healthy repo
- [ ] Hook policy documentation explains which behaviors are reminders versus hard blocks
- [ ] The Claude Code-only stance is stated once in `CLAUDE.md` and referenced from this TODO; no parallel skill tree exists under `.cursor/`, `.codex/`, or similar
- [ ] `AGENTS.md` exists at repo root, under 60 lines, points to `CLAUDE.md`, does not duplicate doctrine (§6)
- [ ] Zero-trailer AI-assist commit policy is documented in `CLAUDE.md` or `CONTRIBUTING.md`, with the stance-change condition named explicitly (§7). A commit-log scan finds zero `Co-Authored-By:` / `Assisted-by:` trailers in recent history.
- [ ] Autonomous-agent boundary is documented: repo does NOT ship `.github/workflows/copilot-setup-steps.yml`, `.github/agents/`, or `.github/chatmodes/`; the refusal-to-ship is explicit, not accidental (§8)

> **Test runner:** N/A (host-side shell + docs only; no kernel test surface) | validation: `bash scripts/test-ai-system.sh` (ships in §9) + [`build.yml`](../../.github/workflows/build.yml) CI step once §9 lands. Complements `bash scripts/test-tooling.sh` (owned by [TODO-01 §10](TODO-01-developer-tooling-stack.md#10-smoke-test-post16-assertions)).
