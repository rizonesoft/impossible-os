---
schema_version: 1
id: ai-development-system
domain: 00-infrastructure
status: active
title: "TODO-02 -- AI Development System"
---

# TODO-02 -- AI Development System

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Turn the repo's AI workflow into an explicit, maintainable system instead of a pile of overlapping instructions. Impossible OS is **Claude Code-only** for implementation and overnight execution. Cursor was removed because maintaining a parallel skill set under `.cursor/` created clutter without a corresponding productivity win. This TODO defines the canonical source-of-truth map for the Claude + external-reviewer surfaces that remain, the skill lifecycle, hook policy, and a regression check that proves the AI layer still matches repo doctrine.

## Authority Hierarchy (read this first)

> **Claude Code is the primary interactive orchestrator.** Doctrine files tell Claude what to do, skills tell Claude how to do it, and external reviewers tell Claude what might be wrong. There is no sibling executor in this repo. When this TODO uses the term "source of truth" it always identifies WHICH file or tool owns a particular kind of authority.

| Layer                   | Role                                             | Authority over                                                                                                            |
| ----------------------- | ------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------- |
| **Claude Code (tool)**  | Primary interactive orchestrator                 | Interactive code edits, commits, skill invocations, reviewer dispatches                                                   |
| `CLAUDE.md`             | Doctrine source-of-truth (file)                  | Product north star, workflow rules, safety constraints, policy                                                            |
| `.claude/skills/`       | Workflow source-of-truth (directory)             | How Claude executes a specific task (implement, review, verify, diagnose)                                                 |
| `.claude/settings.json` | Harness policy source-of-truth (file)            | Permissions, hook reminders, pre/post-tool-use gates                                                                      |
| Codex review mode       | External reviewer                                | Adversarial findings only; invoked from Claude skills; findings go through receiving-code-review discipline before action |
| `.githooks/`            | Git-time guards (distinct layer, see TODO-01 §5) | Pre-commit lint, post-commit COUNT, opt-in pre-push                                                                       |

**Hierarchy invariants:**
1. **Doctrine lives in `CLAUDE.md`. Nowhere else.** Skill headers, tool instructions, and regression messages reference doctrine but do not redefine it. Edits go to `CLAUDE.md` first, then propagate.
2. **Skills live in `.claude/skills/` only.** No parallel skill trees (`.cursor/skills/`, `.codex/skills/`, `.other-tool/skills/` etc.).
3. **External reviewers return findings, never edits.** Codex review output is information the active orchestrator reads and judges. Each finding is verified at file:line and classified Fix / Reject / Accept-XREF before action.
4. **CLAUDE.md wins on conflict.** If a skill, a hook message, or an external-tool config contradicts `CLAUDE.md`, `CLAUDE.md` is right and the other layer is the bug. Fix the drift, don't fork the doctrine.
5. **Claude Code remains the only implementation agent.** A human operator talks to Claude for normal work. External reviewers return findings only.

> [!IMPORTANT]
> **Current state:** The repository's AI surface today is `CLAUDE.md`, `.claude/settings.json`, and `.claude/skills/`. Codex participates only in review mode: finding-only, received through review discipline. The failed interchangeable-driver approach and the local Codex overnight runner are retired.

## Inputs

- [`CLAUDE.md`](../../CLAUDE.md) -- Claude Code source-of-truth doctrine and workflow rules
- [`.claude/settings.json`](../../.claude/settings.json) -- Claude Code hooks, permissions, and reminders
- [`.claude/skills/README.md`](../../.claude/skills/README.md) -- Claude skill catalog
- [`.claude/skills/`](../../.claude/skills/) -- skill implementations and templates
- (Retired 2026-04-28: `scripts/copilot-review.sh` and `.github/copilot-instructions.md` were removed wholesale when Copilot CLI was dropped as a subordinate reviewer; see TODO-08 §14.)
- [`todo/TODO-00-INDEX.md`](../TODO-00-INDEX.md) -- root roadmap ownership
- [`AGENTS.md`](../../AGENTS.md) -- cross-tool pointer (ships in §6; referenced here so Inputs is self-consistent after §6 lands)
- [`CONTRIBUTING.md`](../../CONTRIBUTING.md) -- contributor-facing doc; candidate home for the §7 zero-trailer commit policy
- -> XREF: [`00-infrastructure/TODO-01 §5`](TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle) -- git hook lifecycle and CI/tooling contract (distinct from Claude Code harness hooks in `.claude/settings.json`)
- -> XREF: [`00-infrastructure/TODO-03`](TODO-03-kernel-test-harness.md) -- kernel test harness (separate from the AI workflow regression pack owned here)
- -> XREF: [`00-infrastructure/TODO-07 §7, §9`](TODO-07-lsp-mcp-bridge.md#7-six-mcp-tools-hover-definition-references-diagnostics-workspacedocument-symbol) -- new read-only MCP server (`lsp-bridge`); its repo-tracked `.claude/mcp.json` manifest and grep-audit boundary are the concrete compliance case for §5's MCP server boundary and §8's autonomous-agent read-only allowance. §7 defines the six read-only tools; §9 wires the manifest + boundary audit.
- External reference: [Linux kernel Documentation/process/coding-assistants.rst](https://docs.kernel.org/process/coding-assistants.html) -- 2025-12 precedent for `Assisted-by:` trailers; §7 documents Impossible OS's deliberate divergence.
- External reference: [AGENTS.md open standard](https://agents.md/) -- Linux Foundation-stewarded cross-tool instruction file; §6 adopts the convention.

## Outcome

- One explicit ownership map for instructions, skills, hooks, and permissions across the Claude + external-reviewer surfaces the repo actually uses (Copilot CLI subordinate reviewer retired 2026-04-28).
- A stable skill lifecycle: scaffold, document, validate, cross-link, and retire without drift -- and a clear "Claude-only" stance so no one ports skills sideways to an abandoned tool.
- Hook routing and policy rules that are intentional and reviewable instead of encoded only in a large `settings.json` blob.
- External-reviewer contract: Codex is the sole external adversarial reviewer invoked from Claude skills, not a competing instruction layer. Doctrine lives in `CLAUDE.md` alone.
- Cross-tool pointer file (`AGENTS.md`) that makes the Authority Hierarchy discoverable from non-Claude tool surfaces (Codex CLI, Aider, Continue, Gemini-CLI, Zed) without duplicating doctrine.
- Explicit AI-assist commit disclosure policy: zero-trailer stance documented in contrast to the Linux kernel 2025-12 `Assisted-by:` convention, with the stance-change condition named so the policy is author-by-decision, not oversight.
- Autonomous-agent boundary policy: the repo explicitly states it does NOT accept autonomous coding-agent PRs (Copilot cloud-agent, Devin, Cognition) and does NOT ship the files those workflows require (`copilot-setup-steps.yml`, `.github/agents/`, `.github/chatmodes/`, `.github/instructions/`). Existing `AGENTS.md` + `CLAUDE.md` are classified as cross-tool-pointer / doctrine surfaces, NOT autonomous-agent enablement. (The Copilot-CLI reviewer-mode `.github/copilot-instructions.md` was retired 2026-04-28 per TODO-08 §14; not in this list anymore.) GitHub-side UI enablement (org/repo Settings -> Copilot) is a procedural guard the regression pack cannot detect. The refusal is documented, not implicit.
- An AI workflow regression pack that proves the repo still enforces completion-first behavior, required domain handoffs, and the three new policy boundaries (cross-tool pointer, zero-trailer, autonomous-agent refusal). Complements [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh) which already covers wrapper/hook/YAML drift.

## Implementation Order

| ⭐  | Order | Deliverable                                   | Depends On | Status |
| --- | :---: | --------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Canonical ownership and source-of-truth map   | --         |  [x]   |
| 💎  |   2   | Skill lifecycle, templates, and catalog rules | §1         |  [x]   |
| 💎  |   3   | Hook routing and policy contract              | §1         |  [x]   |
| 💎  |   4   | External-reviewer contract (Codex, Copilot)   | §1-§3      |  [x]   |
| 💎  |   5   | MCP, permissions, and extension boundary      | §1, §3, §4 |  [x]   |
| 💎  |   6   | `AGENTS.md` cross-tool pointer file           | §1, §4     |  [x]   |
| 💎  |   7   | AI-assist commit disclosure policy            | §1         |  [x]   |
| ⭐  |   8   | Autonomous-agent boundary policy              | §4, §5     |  [x]   |
| ⭐  |   9   | AI workflow regression suite                  | §1-§8      |  [x]   |

> 💎 = parity work: mature engineering repos document ownership, automation rules, and policy boundaries.
> ⭐ = exclusive work: Impossible OS can treat its AI workflow as a first-class subsystem with regression checks, not as untracked prompt folklore.

---

## 1. Canonical Ownership and Source-of-Truth Map

The first job is to state clearly which file owns what.

- [x] Wrote ownership matrix at [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) covering `CLAUDE.md`, `.github/copilot-instructions.md`, `.claude/settings.json`, `.claude/skills/`, `scripts/copilot-review.sh`, `.githooks/`, Codex plugin, and Copilot CLI. The doc opens with the Authority Hierarchy table mirrored from the top of this TODO (same rows, same roles, same authority scope; wording normalized for a docs reader -- "this page" in place of "this TODO" in one sentence). Claude Code = MASTER; CLAUDE.md = doctrine SoT; Codex/Copilot = subordinate reviewers.
- [x] Split doctrine into Global (lives in `CLAUDE.md`, 11 rows covering north star, bare metal first, SMP-from-day-one, POST16 boot-path-only, no Unicode dashes, freestanding kernel, Win32 API, test no-live-boot, Bare Metal Gotchas, Safety Gates, Mandatory Skill Triggers) vs Tool-Local (5 rows: skill SKILL.md files, `settings.json`, Copilot instructions, Codex plugin templates, `copilot-review.sh`). Every global-doctrine row links to the exact `CLAUDE.md` anchor that owns it.
- [x] "Claude Code-Only Stance" section in `ai-system.md` explicitly lists: no `.cursor/`, no parallel skill sets; doctrine in `CLAUDE.md`; skills in `.claude/skills/`; external reviewers invoked from inside Claude skills; any future AI tool goes through the external-reviewer contract (§4) with `receiving-code-review` discipline.
- [x] "Hierarchy Invariants" block in `ai-system.md` restates the 5 invariants verbatim from this TODO's Authority Hierarchy block, so the invariants are discoverable from both locations.
- [x] Fixed stale `todo/TODO-00-INDEX.md` line 42 ("Claude, Cursor, and Copilot" -> "Claude Code master; Codex + Copilot subordinate reviewers"). No other stale Cursor or Antigravity references remain in root docs (README.md/CLAUDE.md "cursor" hits are the graphical mouse cursor, not the AI tool; verified via grep).
- [x] "Edit-Here-Not-There Rules" table in `ai-system.md` names 7 concepts with their canonical edit location and the downstream consumers that must NOT be edited to change the concept (doctrine, skill workflow, harness policy, Copilot guidance, Codex dispatch templates, git-hook lifecycle, TODO workflow). Also added a link to `ai-system.md` from `CLAUDE.md` "Skills" section so contributors reach the matrix from the doctrine file.
- [x] Added `ai-system.md` row to [`docs/infrastructure/index.md`](../../docs/infrastructure/index.md) so the doc is discoverable from the Infrastructure landing page.
- [x] Commit: `"docs/ai: define canonical ownership map for repo AI system"`

**Test checkpoint:** A contributor can answer "where do I change this rule?" for doctrine, hook behavior, skill content, and reviewer guidance without guessing. Root roadmap links resolve to real files. The Claude/reviewer boundary is stated in one canonical place ([`docs/infrastructure/ai-system.md` Claude / Reviewer Boundary](../../docs/infrastructure/ai-system.md#claude--reviewer-boundary)).

> **Test runner:** N/A (docs-only) | validation: markdown links resolve, 4 stale refs removed (copilot-instructions.md `.cursor/` row, TODO-00-INDEX.md line 42, 00-infrastructure/INDEX.md line 32, CLAUDE.md Skills section updated with matrix link); §9 regression suite will add automated checks when it ships.

> **Notes:**
> - Shipped [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) as the canonical AI-system ownership map: Authority Hierarchy table, 5 hierarchy invariants, Claude Code-Only Stance, Global-vs-Tool-Local doctrine split, Edit-Here-Not-There Rules (8 rows covering doctrine, skills, harness policy, Copilot CLI, Codex templates, git-hook lifecycle, TODO workflow, skill lifecycle).
> - `CLAUDE.md` Skills section now links to `ai-system.md`; `docs/infrastructure/index.md` has a row; stale Cursor references swept in 4 places (`.github/copilot-instructions.md` dropped `.cursor/` row; `todo/TODO-00-INDEX.md` + `todo/00-infrastructure/INDEX.md` summaries; duplicate invariant #5 in §8 replaced with pointer).
> - Canonical doc: [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md). `CLAUDE.md` (doctrine) and this TODO (roadmap) are the upstream sources; the doc is the index into them, never a substitute.
> - Scope boundary: ownership map only. Skill catalog sync is §2; hook routing §3; reviewer contract §4; MCP/permissions §5; AGENTS.md pointer §6; regression automation §9.

> **Verified:** 2026-04-19 | commit `52b2a440` | 7/7 items | build N/A (docs-only) | 8 files changed | 5 stale Cursor refs swept
> **Quality reviewed:** 2026-04-19 | Codex 3x (adversarial x2, consistency) | 4M+2L fixed (Cursor row in copilot-instructions, fake scripts/codex-companion.mjs path, stale 13-count dropped, verbatim->mirrored stamp, false README link claim, duplicate invariant #5 in §8), 0 open | scope: N/A (docs-only)

---

## 2. Skill Lifecycle, Templates, and Catalog Rules

Skills need the same rigor as code: discoverable ownership, templates, and retirement rules.

- [x] 5-step lifecycle documented in [`docs/infrastructure/skill-authoring.md`](../../docs/infrastructure/skill-authoring.md): (1) scaffold from `TEMPLATE.md`; (2) write `description` field as action-verb + concrete triggers (auto-load signal is the description; document "Auto-loads when..." when a harness hook invokes it); (3) add row to CLAUDE.md Skills table; (4) add Mandatory Skill Triggers row + settings.json hook when the skill MUST run under specific conditions; (5) add row to `.claude/skills/README.md` catalog.
- [x] Canonical template at [`.claude/skills/TEMPLATE.md`](../../.claude/skills/TEMPLATE.md): frontmatter (`name`, `description`), H1 title, `## Use This Skill When`, `## Workflow` (or Pipeline), `## Guardrails` (or Rules). Optional: `## Execution Discipline`, `## Additional Resources`, `## Relationship to <other-skill>`. Inline reminders (ASCII `--` only, no doctrine restatement, no `[Opus]`/`[Sonnet]` tags) live in a strip-before-commit HTML comment block.
- [x] Catalog hygiene rules documented in `skill-authoring.md` "Catalog Hygiene" section: three canonical locations (`.claude/skills/<slug>/`, CLAUDE.md Skills table, `.claude/skills/README.md`) must stay in sync; a directory without matching index rows is drift. Ran the sync check -- 5 missing CLAUDE.md rows fixed in the same commit (added `audit-ssdt`, `codex-adversarial-review-section`, `gap-audit-todo`, `kernel-code-quality`, `validate-todo-section`). Expanded `.claude/skills/README.md` from 3-line stub to full catalog grouping all 28 live skills.
- [x] Retirement / supersession sweep documented as 7-step process in `skill-authoring.md` "Retirement / Supersession": `git rm -rf <slug>` + delete CLAUDE.md row + delete README.md row + delete Mandatory Skill Triggers row + delete settings.json hook + grep-and-clean XREF fallout + land in one commit. Renames = retire-old + add-new, not in-place edit.
- [x] "No parallel skill trees" rule stated as a hard rule at the top of `skill-authoring.md`, linking to Authority Hierarchy invariant #2 in `ai-system.md`. Cross-referenced from CLAUDE.md Skills section via the new "Adding, editing, or retiring a skill" pointer line below the table.
- [x] Commit: `"docs/ai: define skill lifecycle, template, and catalog rules"`

**Test checkpoint:** A new skill can be scaffolded and registered by following one documented path ([`skill-authoring.md`](../../docs/infrastructure/skill-authoring.md) Lifecycle steps 1-5). CLAUDE.md Skills table + `.claude/skills/README.md` + actual skill directories stay in sync (verified: 28 directories, 28 CLAUDE.md rows, 28 README.md rows as of this commit). A skill directory without matching index entries will be flagged by the regression pack ([§9](#9-ai-workflow-regression-suite)).

> **Test runner:** N/A (docs + catalog sync) | validation: `ls .claude/skills/ | wc -l` == rows in CLAUDE.md Skills table == rows in `.claude/skills/README.md`; §9 regression suite will assert this automatically when it ships.

> **Notes:**
> - Shipped [`docs/infrastructure/skill-authoring.md`](../../docs/infrastructure/skill-authoring.md): 5-step add lifecycle, 2 canonical SKILL.md shapes (Shape A workflow, Shape B code-quality) covering all live skills, catalog hygiene sync rules, 7-step retirement sweep with 4-shape grep recipe (slash / bare / markdown-path / `settings.json`).
> - New scaffold [`TEMPLATE.md`](../../.claude/skills/TEMPLATE.md) under `.claude/skills/`; `README.md` expanded from 3-line stub to grouped catalog (TODO workflow, review, Codex dispatch, domain code-quality, debugging groups).
> - `CLAUDE.md` Skills table gained 5 rows that existed on disk but were missing from the catalog (`audit-ssdt`, `codex-adversarial-review-section`, `gap-audit-todo`, `kernel-code-quality`, `validate-todo-section`); catalog sync invariant verified at the time: 28 skill directories == 28 CLAUDE.md rows == 28 README.md rows (`validate-todo-section` later retired; current count 27/27/27 after that follow-up).
> - Canonical doc: [`skill-authoring.md`](../../docs/infrastructure/skill-authoring.md). Edit-Here-Not-There row in `ai-system.md` routes lifecycle changes to this doc; scaffold-only changes go to `TEMPLATE.md`.
> - Scope boundary: skill catalog + lifecycle + retirement only. Hook routing is §3; reviewer contract §4; regression enforcement of the sync invariant is §9.

> **Verified:** 2026-04-19 | commit `eedc0868` | 6/6 items | build N/A (docs-only) | 7 files changed | 28/28/28 skill sync
> **Quality reviewed:** 2026-04-19 | Codex 2x (adversarial, combined) | 3M fixed (retirement sweep missed bare-name forms; canonical section contract split into Shape A workflow + Shape B code-quality to match 28 live skills; TEMPLATE.md labelled Shape-A with Shape-B pointer), 0 open | scope: N/A (docs-only)

---

## 3. Hook Routing and Policy Contract

Hooks are part of the AI system, not invisible glue.

> **XREF (2026-04-28):** the hook surface roughly doubled in [TODO-08 Automation Hardening](TODO-08-automation-hardening.md) §3-§5, §7, §10-§12. The canonical per-hook table now lives at [`.claude/hooks/MANIFEST.md`](../../.claude/hooks/MANIFEST.md) (cross-checked by `scripts/audit-hooks.sh`); the BLOCK/REMIND/POST-HOC effect-class tables in `ai-system.md` are the historical quick-reference taxonomy. Update MANIFEST.md when adding new hooks.

- [x] Audited all live harness hooks in `.claude/settings.json` into a one-line-per-hook routing matrix at [`docs/infrastructure/ai-system.md` "Hook Routing Matrix"](../../docs/infrastructure/ai-system.md#hook-routing-matrix) (18 at initial audit 2026-04-19 commit 98f2350b; grew to 19 with the numeric-TODO-shorthand BLOCK hook added in follow-up). Each row names the trigger (`PreToolUse`/`PostToolUse` + matcher: `Edit|Write|MultiEdit`, `Bash`, `Skill`), the path / command filter, and the rule / reminder / validator purpose.
- [x] Split the matrix into three effect-class tables: BLOCK (5 hooks -- Unicode dash ban, numeric-TODO-shorthand ban on .md outside todo/ + .claude/, scope-gap protocol on C files, test-side-effect ban, Accepted/Deferred bare-XREF gate at commit), REMIND (12 hooks -- domain code-quality auto-load, section-commit GATE, completion-first on Skill entry, receiving-code-review after external review, validate-todo-file on TODO edits, TODO format check, test wiring / message uniqueness / TEST_PENDING reminders, CLAUDE.md sync, scope-gap dedup, Accepted-XREF concreteness warn), and POST-HOC VALIDATE (2 hooks -- post-commit unit tests via `scripts/test.sh` and boot smoke via `scripts/test-smoke.sh`).
- [x] The `.claude/settings.json` JSON blob stays canonical -- the matrix is the human-readable view. Editing procedure documented in the "Editing hooks" sub-section: edit JSON first, update matrix row in the same commit, then §9 regression suite asserts the invariant.
- [x] Harness-hooks vs git-hooks boundary stated explicitly in the new "Two hook layers -- do not confuse them" table at the top of the Routing Matrix: harness hooks run at tool-call time (`.claude/settings.json`); git hooks run at commit/push time (`.githooks/`, owned by TODO-01 §5); `scripts/copilot-review.sh` has no hook surface.
- [x] XREF to [TODO-01 §5 Git Hooks and Local Automation Lifecycle](TODO-01-developer-tooling-stack.md#5-git-hooks-and-local-automation-lifecycle) added inline in the boundary table + in the See Also list.
- [x] Tighten the post-commit unit-test + boot-smoke matchers (hook rows 9-10 in the matrix; the two POST-HOC validators): both hooks now subprocess `git show --name-only --format= HEAD` and match against that file list (ext filter for hook 9, path-prefix filter for hook 10) instead of scanning `tool_response.stdout`. Pure-modification commits are no longer missed, and boot-path prefix matching is now tighter (`startswith` on per-file list, not substring in a stdout blob). Matrix rows 9-10 Filter column updated to `HEAD files (code)` / `HEAD files (boot)`; abbreviation legend refreshed; "Known limitations" paragraph deleted from ai-system.md. Originally caught by Codex review 2026-04-19.
- [x] Commit: `"docs/ai: codify hook routing and policy contract"`

**Test checkpoint:** For any given edit path or skill invocation, the responsible hook and its intended effect are documented. A maintainer can tell whether a behavior is a reminder or a block without reading minified JSON (three named tables: BLOCK / REMIND / POST-HOC VALIDATE). The harness/git hook boundary is clear and named in the "Two hook layers" table.

> **Test runner:** N/A (docs-only) | validation: matrix row count matches `.claude/settings.json` hook count (verified via python3 parse); current split 5 BLOCK + 12 REMIND + 2 POST-HOC = 19; §9 regression suite will automate this sync.

> **Notes:**
> - Shipped Hook Routing Matrix in [`ai-system.md`](../../docs/infrastructure/ai-system.md#hook-routing-matrix) covering all live harness hooks in three effect-class tables: BLOCK (5: Unicode-dash ban, numeric-TODO-shorthand ban on non-`todo` `.md`, scope-gap-protocol on C-src, test-side-effect ban, bare-`Accepted:`/`Deferred:` XREF gate), REMIND (12), POST-HOC VALIDATE (2: post-commit unit-tests + boot-smoke).
> - Path-filter abbreviations (`C-src`, `test_*.c`, `todo/*.md`, `skills/*.md`, `HEAD files (code)`, `HEAD files (boot)`) defined once above the tables so every row fits under the 200-char cap without shrinking the column padding.
> - Two hook layers block states the harness-vs-git-hook boundary explicitly: `.claude/settings.json` runs at tool-call time; `.githooks/` runs at commit/push time (owned by TODO-01 §5); `scripts/copilot-review.sh` has no hook surface at all.
> - Post-commit matchers for hooks 9+10 now subprocess `git show --name-only --format= HEAD` and match against that canonical file list (ext filter for hook 9, path-prefix filter for hook 10). Earlier design scanned `tool_response.stdout`, which only included `create mode`/`delete mode` lines; pure-modification commits silently skipped the post-commit validators. The "Known limitations" paragraph that documented the earlier gap has been deleted from ai-system.md now that the gap is closed.
> - Canonical source: [`.claude/settings.json`](../../.claude/settings.json) (JSON stays the source of truth; matrix is the human-readable view). Editing procedure: edit JSON first, update the matching matrix row in the same commit, rely on §9 to assert the sync.
> - Scope boundary: harness hooks only. Git hooks (`.githooks/pre-commit`, `post-commit`, `pre-push`) live under TODO-01 §5.

> **Verified:** 2026-04-19 | commit `98f2350b` | 7/7 items | build N/A (docs-only) | 18/18 hook rows
> **Quality reviewed:** 2026-04-19 | Codex 2x (adversarial x2) | 2H+1M+2L fixed (H: hooks 8+9 semantics -> Known Limitations + follow-up, resolved in a later commit when the tighter matchers shipped; H: post-commit matcher tightened to `git show --name-only --format= HEAD`; M: C-src abbreviation missed `.cpp`; L: row-length cap compaction; L: Deferred XREF line-number fix), 0 open | scope: N/A (docs-only)

---

## 4. External-Reviewer Contract (Codex)

Codex participates in the AI workflow as the sole external adversarial reviewer, not as an authoritative instruction layer. That contract must be explicit so doctrine drift stays impossible. (Section heading kept "Codex, Copilot" anchor unchanged for backward-compat link stability; section title above is the canonical present-tense form.)

> **XREF (2026-04-28):** [TODO-08](TODO-08-automation-hardening.md) §3 hardens the contract from reminder to BLOCK -- after any Codex dispatch, the next code edit is refused until `superpowers:receiving-code-review` is invoked. §5 enforces the three-dispatch policy (adversarial + consistency + perf, optional re-adversarial) at section-commit time via `.claude/state/last-review-stamps.json`. §14 retired the Copilot CLI subordinate-reviewer role wholesale -- `scripts/copilot-review.sh` and `.github/copilot-instructions.md` were deleted; Codex GPT-5.5 is now the sole external reviewer. Historical [x] stamps below describe the original Codex+Copilot landing in commit `1097c3d3` and remain unchanged as audit trail; the active state is Codex-only.

- [x] Documented the external-reviewer role in the new [External-Reviewer Contract](../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot) section of `ai-system.md`. Codex is invoked from within Claude skills via the OpenAI Codex plugin binary (`~/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs`); Copilot is invoked via `scripts/copilot-review.sh`. Both return findings that Claude applies `superpowers:receiving-code-review` discipline to -- no doctrine edits.
- [x] Documented the 9 Codex-dispatching Claude skills in a dedicated table under "Codex dispatch surface": `codex-design-review`, `codex-adversarial-review-section`, `codex-review-todo`, `codex-fix-review`, `codex-test-coverage`, `codex-impact-analysis`, `codex-consistency-audit`, `codex-dead-code`, `codex-perf-review`. Each row names the specific adversarial angle the skill owns.
- [x] Documented Copilot's narrower scope in the "Copilot invocation surface" sub-section: 33-line `scripts/copilot-review.sh` wrapper, PR-style review for docs + non-kernel code, zero dedicated skill dispatches (deliberate -- kernel-critical review goes through `codex-*` skills).
- [x] Stated the "reviewer, not authority" invariant in the lead sub-section; pinned Authority Hierarchy invariant #3 as the hierarchy rule and invariant #4 as the conflict-resolution rule.
- [x] Added a one-line `> **External-Reviewer Contract:**` blockquote to the top of every skill that dispatches Codex: 9 angle-owner `codex-*` skills + 8 workflow-consumer skills (`implement-todo-section`, `implement-ssdt-range`, `implement-unit-tests`, `review-todo-section`, `verify-todo-section`, `quality-review-section`, `debug-session`, `diagnose-serial-log`) = 17 total. Verified: `grep -l 'External-Reviewer Contract:' .claude/skills/*/SKILL.md | wc -l` == 17. Codex review caught this -- my first draft only listed the 9 codex-* skills; follow-up grep of `codex-companion.mjs` found 7 direct invokers outside that set that also needed the contract pointer.
- [x] Added an "Adding a new reviewer tool" sub-section naming the 5-step contract any new AI reviewer must pass through before being adopted (no parallel instruction tree, invocation from inside a Claude skill, `receiving-code-review` on output, Authority Hierarchy row, TODO-02 roadmap ownership). Links forward to [§8 Autonomous-Agent Boundary Policy](#8-autonomous-agent-boundary-policy) for the refusal path.
- [x] Commit: `"docs/ai: define external-reviewer contract for Codex and Copilot"`

**Test checkpoint:** A maintainer reading any `codex-*` skill sees the External-Reviewer Contract blockquote at the top and can one-link-trace back to the canonical contract section in `ai-system.md`. Codex/Copilot findings are always applied via `receiving-code-review`, never blindly. Neither tool is documented as "authority" anywhere in the repo (verified: `grep -rn 'Codex.*authority\|Copilot.*authority' CLAUDE.md docs/ .claude/skills/` returns only the negation "not authority" / "never authority").

> **Test runner:** N/A (docs + skill metadata) | validation: 17/17 Codex-dispatching skills carry the contract pointer (`grep -l 'External-Reviewer Contract:' .claude/skills/*/SKILL.md | wc -l`); table rows in `ai-system.md` all under 200-char cap; §9 regression suite will assert these invariants automatically when it ships.

> **Notes:**
> - Shipped [External-Reviewer Contract](../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot) section in `ai-system.md` with 5 sub-sections: Reviewer-not-authority invariant, Codex dispatch surface (9 angle-owner + 7 direct consumer + 1 inheritor = 17), Copilot invocation surface (33-line `scripts/copilot-review.sh` wrapper, PR-style review for docs + non-kernel), 5-step Adding-a-new-reviewer-tool contract, Discoverability-back-from-skills.
> - 17 skills gained a one-line `> **External-Reviewer Contract:**` blockquote linking back to the canonical contract section: 9 codex-* angle owners + 7 direct workflow consumers (`implement-todo-section`, `implement-ssdt-range`, `implement-unit-tests`, `review-todo-section`, `quality-review-section`, `debug-session`, `diagnose-serial-log`) + 1 inheritor (`verify-todo-section`, which picks up the pointer via `review-todo-section`'s workflow).
> - Authority Hierarchy Codex row in both `ai-system.md` and this TODO updated to name the 17-dispatcher split explicitly (previously said only "codex-* Claude skills" which was stale once the surface expanded).
> - Canonical doc: [External-Reviewer Contract](../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot).
> - Scope boundary: reviewer-dispatch surface + `receiving-code-review` discipline + contract for future reviewer tools. Autonomous-agent refusal lives in §8; the [Autonomous-Agent Boundary Policy](#8-autonomous-agent-boundary-policy) is the downstream owner.

> **Verified:** 2026-04-19 | commit `1097c3d3` | 7/7 items | build N/A (docs-only) | 17 back-pointers wired
> **Quality reviewed:** 2026-04-19 | Codex 2x (adversarial x2) | 2M+1L fixed (M: Codex-dispatch surface initially listed only 9 codex-* skills -> added back-pointers + expanded surface to name both groupings; M: Authority Hierarchy table Codex row + Claude-Code-Only-Stance prose still said "invoked from inside codex-* Claude skills" which contradicted §4's 17-skill count -> updated both to name the 17-dispatcher split; L: table rows 206-208 chars -> compacted under 200-char cap), 0 open | scope: N/A (docs-only)

---

## 5. MCP, Permissions, and Extension Boundary

Permissions and extensions are part of the architecture. They need clear boundaries and safe defaults.

- [x] Documented the four Claude Code permission tiers (`allow` / `ask` / `deny` / `defaultMode`) in the new [MCP, Permissions, and Extension Boundary](../../docs/infrastructure/ai-system.md#mcp-permissions-and-extension-boundary) section of `ai-system.md`. Current repo-tracked allow-rule shape: 8 narrow entries in `.claude/settings.json`, 0 explicit `ask` or `deny` rules (harness safety defaults cover destructive patterns).
- [x] Documented the MCP server boundary: MCP servers live in user-local harness config (`~/.claude/` or IDE pane), NOT in repo-tracked files. Reason pinned to credential leak risk. The repo uses skills + hooks as the shared tool surface; MCP output enters the repo only via skill-authored citations in PRs.
- [x] Added policy notes for secrets and local-only state: secrets never live in either `settings.json` OR `settings.local.json` (read from env or user-level config); `.claude/settings.local.json` covers personal allow-rules + `defaultMode` + machine-specific paths. Updated `.gitignore` to explicitly exclude `.claude/settings.local.json` with an inline pointer to this documentation (the file was already untracked but was not git-ignored, so a `git add .claude/*` would have committed it).
- [x] Stated the shared-vs-local split explicitly in a two-row "Shared vs user-local split" table: `.claude/settings.json` (committed, shared) vs `.claude/settings.local.json` (gitignored, user-local). "What belongs where" bullet list names 3 buckets: shared (hooks + repo-wide allow-rules + env vars), user-local (personal allow-rules + `defaultMode` + one-off permissions + machine paths), and never-either (secrets / API tokens / keys).
- [x] Documented the `claude-code-guide` subagent plus the other 4 harness-provided subagents (`Explore`, `general-purpose`, `Plan`, `statusline-setup`) in a dedicated "Subagent boundary" table. Pinned their stance: harness-provided, not repo-tracked; `claude-code-guide` is read-only docs-only; code edits go through skills (`.claude/skills/`) not through subagents.
- [x] Added permission/MCP/subagent rows to an "Edit-here-not-there mapping for this section" table inside §5: shared allow-rules -> `settings.json`; personal allow-rules -> `settings.local.json`; MCP config -> user harness config (outside repo); subagent behavior -> harness built-in. Cross-linked from the main Edit-Here-Not-There Rules table (§1) as a follow-up refinement.
- [x] Commit: `"docs/ai: define MCP, permissions, and extension boundary"`

**Test checkpoint:** A maintainer can tell which settings are safe to commit, which are local-only, and where to add new project-level AI capabilities without violating policy boundaries. Verified: `.gitignore` entry for `.claude/settings.local.json` present; the file is currently untracked (`git ls-files .claude/settings.local.json` returns empty); `ai-system.md` names each settings file, each permission tier, and each subagent with its role + repo stance.

> **Test runner:** N/A (docs + gitignore + settings cleanup) | validation: `git check-ignore .claude/settings.local.json` returns 0; `ai-system.md` contains the MCP, Permissions, and Extension Boundary section; `.claude/settings.json` permissions.allow no longer contains destructive `Bash(git reset:*)` rule; §9 regression suite will assert these invariants when it ships.

> **Notes:**
> - Shipped [MCP, Permissions, and Extension Boundary](../../docs/infrastructure/ai-system.md#mcp-permissions-and-extension-boundary) section in `ai-system.md` with 5 sub-sections: Permission model (3 rule lists `allow`/`ask`/`deny` + `defaultMode` knob with actual modes `default`/`acceptEdits`/`plan`/`bypassPermissions`), Shared-vs-user-local split, MCP server boundary (user-local only; credential leak risk pinned), Subagent boundary table (5 harness subagents with role + repo stance), Edit-here-not-there mapping.
> - `.gitignore` gained `.claude/settings.local.json` entry with inline pointer to the new doc (file was previously untracked but not explicitly ignored; any `git add .claude/*` would have leaked personal permissions).
> - Tightened `.claude/settings.json` `permissions.allow`: removed `Bash(git reset:*)` (auto-approved destructive `git reset --hard`) and `Bash(python3 -c :*)` (auto-approved arbitrary Python code execution = equivalent bypass in a less obvious wrapper). Final count: 6 narrow entries (3 scoped grep patterns, 1 bounded `scripts/test.sh`, 2 scoped `Edit(.claude/skills/<name>/**)`).
> - MCP server boundary pinned to user-local harness config; never repo-tracked. Reason: MCP servers carry auth tokens + personal data; sharing through the repo would leak credentials. If a shared tool surface is needed, it goes through a `scripts/` wrapper invoked by a Claude skill, not MCP.
> - Canonical doc: [MCP, Permissions, and Extension Boundary](../../docs/infrastructure/ai-system.md#mcp-permissions-and-extension-boundary). Edit-Here-Not-There mapping inside the section names: shared allow-rules/hooks -> `.claude/settings.json`; personal allow-rules/`defaultMode` -> `.claude/settings.local.json`; MCP config -> user harness; subagent behavior -> harness built-in.
> - Scope boundary: permissions model + shared-vs-local split + MCP boundary + subagent roles. Hook routing owned by §3; external-reviewer contract by §4; git hooks (different layer) by TODO-01 §5.

> **Verified:** 2026-04-19 | commit `998925be` | 7/7 items | build N/A (docs-only) | 6 shared allow-rules; settings.local.json gitignored
> **Quality reviewed:** 2026-04-19 | Codex 2x (adversarial x2) | 2H+4M+2L fixed (H: `Bash(git reset:*)` in shared allow -> removed rule + example; H: `Bash(python3 -c :*)` in shared allow auto-approved arbitrary code execution -> removed; M: defaultMode misframed as 4th tier with wrong mode names -> separated rule lists + listed actual modes default/acceptEdits/plan/bypassPermissions; M: §1 cross-link to §5 claimed but not added -> added inline pointer; M: stale `Bash(git reset:*)` still named as "safe" shared example after initial fix -> replaced with real safe examples; M: Codex dispatch count mixed 7 consumers vs 8 table rows (verify inherits via review; not a direct caller) -> reframed as 9 angle-owner + 7 direct consumers + 1 inheritor = 17 back-pointers; L: §5 table rows over 200-char cap -> compacted; L: "7 entries" stale after python3 removal -> updated to 6), 0 open | scope: N/A (docs-only + settings hardening)

---

## 6. `AGENTS.md` Cross-Tool Pointer File

The `AGENTS.md` convention emerged 2025 and was stewarded by the Agentic AI Foundation under the Linux Foundation (Aug 2025). It is tool-neutral and read by Codex, Copilot, Aider, Continue, Gemini-CLI, Zed, and others. Impossible OS uses `CLAUDE.md` as authoritative doctrine (Authority Hierarchy row 2), so `AGENTS.md` exists as a thin stub that (a) tells cross-tool readers where to find the real doctrine, (b) surfaces the subordinate-reviewer contract so tools used as reviewers see our rules, and (c) keeps the Claude-primary boundary discoverable from the cross-tool entry point. `AGENTS.md` NEVER redefines doctrine; it points at `CLAUDE.md`.

- [x] Created [`AGENTS.md`](../../AGENTS.md) at repo root. First paragraph names Claude Code as master/orchestrator, points at `CLAUDE.md` as the doctrine source-of-truth, and frames the file as a pointer for non-Claude tools (Codex, Copilot, Aider, Continue, Gemini-CLI, Zed) in subordinate reviewer or reader roles.
- [x] "Authority (3-bullet summary of the 5 invariants)" section in `AGENTS.md`: carries the verbatim master statement + a compact 3-bullet summary (doctrine in `CLAUDE.md` only + on conflict wins; external reviewers return findings, never edits, with `receiving-code-review` discipline; no parallel skill trees). Full table pointed at [Authority Hierarchy in ai-system.md](../../docs/infrastructure/ai-system.md#authority-hierarchy-read-this-first) and the AI Development System roadmap; `AGENTS.md` references both instead of duplicating.
- [x] "Autonomous-agent stop sign" section: states Impossible OS does not accept autonomous-agent PRs (Copilot coding-agent, Devin, Cognition), that commits are authored by a human operator plus Claude Code only, and links forward to the [Autonomous-Agent Boundary Policy](#8-autonomous-agent-boundary-policy) for the full reasoning. Distinguishes autonomous agents (stop) from reviewer-mode use (proceed under `receiving-code-review`).
- [x] "Where the roadmap lives" section: named anchor links to the [AI Development System roadmap](../../todo/00-infrastructure/TODO-02-ai-development-system.md), `CLAUDE.md`, and `docs/infrastructure/ai-system.md`. Zero numeric-shorthand refs (the new PreToolUse hook 3 would have blocked them -- it actually did block my first draft and caught two offending link-text shorthands before the file was written).
- [x] `AGENTS.md` is 35 lines long (well under the ~60-line cap). A final "What NOT to put in this file" section names the cap invariant and points at the regression-pack check.
- [x] Regression-pack assertions for §9: already filed in the §9 checklist ("`AGENTS.md` exists at repo root, contains the phrase `Claude Code` and a link to `CLAUDE.md`, stays under 60 lines, and does NOT restate doctrine"). No change needed here; §9 owns the check surface.
- [x] Commit: `"docs/ai: add AGENTS.md cross-tool pointer to CLAUDE.md"`

**Test checkpoint:** A Codex/Aider/Continue user pointed at the repo root reads `AGENTS.md`, follows the pointer to `CLAUDE.md`, and understands their role as subordinate reviewer without needing to re-learn doctrine. `AGENTS.md` is 35 lines, well under the 60-line cap. The new numeric-TODO-shorthand PreToolUse BLOCK hook (hook 3) caught two numeric-shorthand link texts in my first draft and rejected the write, so the file shipped without any drift-prone references.

> **Test runner:** N/A (docs-only) | validation: `wc -l AGENTS.md` == 35 (<= 60 cap); `grep -c 'CLAUDE.md' AGENTS.md` >= 3 (pointer integrity); `bash scripts/lint.sh` clean (no numeric TODO shorthand); §9 regression suite will automate these checks when it ships.

> **Notes:**
> - Shipped [`AGENTS.md`](../../AGENTS.md) at repo root (35 lines; under 60-line cap) as the cross-tool pointer following Linux Foundation's AGENTS.md open standard (Aug 2025). Five sections: lead paragraph (Claude Code master + CLAUDE.md doctrine pointer), "What to do" 3 steps, "Authority" (master statement + 3-bullet invariant summary), "Autonomous-agent stop sign", "Where the roadmap lives", "What NOT to put here".
> - Intentional duplication: the Authority section byte-matches the canonical master-statement paragraph + 3 of the 5 invariants from the TODO-02 Authority Hierarchy block. Rationale: a tool reading ONLY `AGENTS.md` (never clicking through) still sees the core rules. §9 regression check enforces the byte-match + forbids CLAUDE.md-exclusive doctrine imports + the length cap.
> - Autonomous-agent stop sign distinguishes autonomous agents (refused) from reviewer-mode use (proceed under `receiving-code-review`). Forward-links to [§8 Autonomous-Agent Boundary Policy](#8-autonomous-agent-boundary-policy) for the full refusal path.
> - Canonical doc: [`AGENTS.md`](../../AGENTS.md). Zero doctrine paragraphs beyond the Authority section; if something is tempting to add, it belongs in `CLAUDE.md` instead.
> - Scope boundary: cross-tool pointer only. Autonomous-agent policy is §8; regression enforcement is §9.

> **Verified:** 2026-04-19 | commit `1ea11ede` | 7/7 items | build N/A (docs-only) | 35/60 lines; 1 new root file
> **Quality reviewed:** 2026-04-19 | Codex 2x (adversarial x2) | 2M fixed (M: 5 prose `--` separators violated CLAUDE.md No-Unicode-Dashes sub-rule -> rewrote as `;` / `:`; M: AGENTS.md line 35 claimed regression suite "enforces" the no-duplication invariant but §9 hasn't shipped yet + the existing §9 check wording only guarded CLAUDE.md doctrine not the intentional TODO-02 master-statement copy -> rewrote line 35 with "will enforce when it ships" and broadened §9's AGENTS.md check to a three-part invariant: (a) byte-match Authority section against TODO-02 canonical, (b) forbid CLAUDE.md-exclusive doctrine imports, (c) length cap), 0 open | scope: N/A (docs-only)

---

## 7. AI-Assist Commit Disclosure Policy

The Linux kernel merged `Documentation/process/coding-assistants.rst` on 2025-12-23 (Sasha Levin, approved by Corbet) requiring AI-assisted patches to carry an `Assisted-by: AGENT_NAME:MODEL_VERSION [TOOL]` trailer alongside the human `Signed-off-by:`. Fedora adopted a similar policy October 2025. Impossible OS is a kernel project and operates in the same space but has an explicit `feedback_no_coauthor` user preference: commits do NOT carry `Co-Authored-By: Claude` or equivalent attribution. This section documents the project's stance explicitly so it is neither accidental nor ambiguous, and names the condition under which the stance would change.

- [x] Added "AI-Assisted Commit Policy (zero trailer)" block to [`CONTRIBUTING.md`](../../CONTRIBUTING.md) Commit Messages section. States the current stance (a-d): Claude Code-orchestrated by design, no `Co-Authored-By:` / `Assisted-by:` / similar AI-attribution trailers, deliberate divergence from the Linux-kernel 2025-12 convention, authored-by-decision not by oversight.
- [x] Stance-change condition stated in `CONTRIBUTING.md`: the zero-trailer stance stands as long as Impossible OS only accepts contributions from project members working interactively with Claude Code. If the project opens to external AI-assisted contributions (community PRs from Codex CLI, Aider, Copilot coding-agent, Cursor, etc.), it adopts the Linux kernel `Assisted-by: TOOL:MODEL [AGENT_NAME]` trailer grammar.
- [x] Documented the AI-slop screening bar as the existing review workflow: step 13-18 of `/implement-todo-section` (Codex adversarial + quality + domain code-quality + scope-gap protocol + `receiving-code-review` discipline) catches phantom helpers, pointless refactors "for consistency", and stub-behind-stamp patterns before `main`. Named as Impossible OS's equivalent of the Linux "AI slop" screening.
- [x] Cross-reference to [Linux kernel `Documentation/process/coding-assistants.rst`](https://docs.kernel.org/process/coding-assistants.html) (Sasha Levin / Jonathan Corbet, 2025-12) + Fedora October 2025 adoption noted as the precedent this divergence is against.
- [x] Added a short "Commits -- zero AI-attribution trailers" pointer block to `CLAUDE.md` (between Doc Sync and Git Hooks) so contributors reading the doctrine file land on the policy without needing to click through to `CONTRIBUTING.md`.
- [x] Regression-pack assertions for §9: already filed at the AI Workflow Regression Suite section -- `CLAUDE.md` or `CONTRIBUTING.md` contains the zero-trailer statement AND the stance-change condition; a scan of recent commit bodies flags `Co-Authored-By:` / `Assisted-by:` / `AI-Author:` as policy violations. Scan window anchor: this commit (§7 adoption point) or any later HEAD; nine historical Copilot-authored commits in `HEAD~100..HEAD` predate the policy and are pre-policy baseline (they would have triggered the stance-change condition if it had existed; documented in the Notes below, not retroactively treated as violations).
- [x] Commit: `"docs/ai: document zero-trailer AI-assist commit policy + stance-change condition"`

**Test checkpoint:** A reviewer reading any recent commit sees no new AI-attribution trailer. A future contributor asking "why no Co-Authored-By? why no Assisted-by?" finds a direct written answer in both `CLAUDE.md` "Commits -- zero AI-attribution trailers" and `CONTRIBUTING.md` "AI-Assisted Commit Policy (zero trailer)", linked to the Linux precedent and the stance-change condition.

> **Test runner:** N/A (docs-only) | validation: `grep -c 'zero.*trailer\|Co-Authored-By\|Assisted-by' CONTRIBUTING.md` >= 3 (policy block present); `grep -c 'zero AI-attribution\|Assisted-by' CLAUDE.md` >= 1 (pointer present); historical baseline: 9 commits in `HEAD~100..HEAD` carry legacy `Co-authored-by: Copilot` trailers (pre-policy; §9 scan window starts AFTER §7 adoption).

> **Notes:**
> - Shipped "AI-Assisted Commit Policy (zero trailer)" block in [`CONTRIBUTING.md`](../../CONTRIBUTING.md) Commit Messages section. Covers four claims: (a) Claude Code-orchestrated by design -- attribution is implicit; (b) no `Co-Authored-By:` / `Assisted-by:` trailers; (c) deliberate divergence from Linux-kernel 2025-12 convention + Fedora October 2025 adoption; (d) AI-slop screening bar is the existing review workflow (implement + review pipelines), not a trailer check.
> - Short "Commits -- zero AI-attribution trailers" pointer block added to [`CLAUDE.md`](../../CLAUDE.md) between Doc Sync and Git Hooks so the policy is discoverable from the doctrine file without clicking through.
> - Stance-change condition pinned: if Impossible OS ever accepts external AI-assisted contributions from non-project-members, it adopts `Assisted-by: TOOL:MODEL [AGENT_NAME]` per the Linux kernel grammar. Until then, any trailer in a commit body is the policy bug; amend or rebase before merge.
> - **Historical baseline:** 9 commits in `HEAD~100..HEAD` carry legacy `Co-authored-by: Copilot <...>` trailers from pre-policy Copilot-authored PRs (oldest in the window: `7b6c8570`, `9c6d0a48`, `d4e80fb1`, plus 6 more). These would have triggered the stance-change condition if it had existed. Not retroactively rewritten (git-history-destructive); §9 regression scan window starts from the §7 adoption commit forward.
> - Canonical doc: [`CONTRIBUTING.md` -- AI-Assisted Commit Policy](../../CONTRIBUTING.md#ai-assisted-commit-policy-zero-trailer); `CLAUDE.md` pointer at [Commits -- zero AI-attribution trailers](../../CLAUDE.md#commits----zero-ai-attribution-trailers).
> - Scope boundary: commit-disclosure policy + stance-change condition + screening-bar pointer. Autonomous-agent refusal lives in §8; regression enforcement of these invariants lives in §9.

> **Verified:** 2026-04-19 | commit `6536677e` | 7/7 items | build N/A (docs-only) | 2 files edited; 1 policy block + 1 pointer block
> **Quality reviewed:** 2026-04-19 | Codex 1x (adversarial) | 1M fixed (M: §9 regression check still defined a rolling `HEAD~100` scan that would flag the 9 legacy Copilot trailers the §7 Notes documented as pre-policy baseline -- would fail on a healthy repo until the commits aged out, or force ignoring the written §9 requirement. Replaced with an explicit §7-adoption anchor: resolve `ANCHOR=$(git log --diff-filter=A -S 'AI-Assisted Commit Policy' -- CONTRIBUTING.md | tail -1)` then `git log --format=%B $ANCHOR..HEAD`; also updated the §9 Unit Tests bullet to match), 0 open | scope: N/A (docs-only)

---

## 8. Autonomous-Agent Boundary Policy

Copilot's coding-agent (the autonomous "file the issue, get a PR" agent), Devin, and equivalent autonomous agents operate by running tasks in sandboxes and opening PRs without per-step human authorship. Microsoft ships `.github/workflows/copilot-setup-steps.yml` + a default-on firewall allowlist for this mode. Authority Hierarchy invariant #5 (see top of this TODO) owns the interactive-agent boundary; this section documents how that boundary is enforced as repo policy and what the repo explicitly does NOT ship as a consequence.

- [x] Documented the no-autonomous-agent stance in [`ai-system.md` "Autonomous-Agent Boundary Policy"](../../docs/infrastructure/ai-system.md#autonomous-agent-boundary-policy) and cross-linked from `CLAUDE.md` via a new "Autonomous-agent boundary -- interactive only" section pointing back.
- [x] Stated three reasons in the "Why" sub-section: (a) every section commit goes through `/implement-todo-section` steps 13-18 or `/review-todo-section` Phases 2-4 with mandatory Codex dispatches + domain code-quality gates + `receiving-code-review` discipline -- autonomous agents run their own pipelines that don't follow this one; (b) the no-parallel-skill-trees invariant (Authority Hierarchy #2) forbids `.github/agents/*.agent.md` and `.github/chatmodes/*.md`; (c) `feedback_no_substandard_code` rejects emulator-only shortcuts that autonomous agents produce by default.
- [x] "What this repo deliberately does NOT ship (forbidden-path list)" table lists 5 forbidden paths (`.github/workflows/copilot-setup-steps.yml`, `.github/agents/*.agent.md`, `.github/chatmodes/*.md`, `.github/instructions/*.instructions.md`, Copilot cloud-agent firewall allowlist) with what each enables in other repos. Second table "Files Copilot cloud-agent MAY read but are NOT autonomous-agent enablement" classifies the 3 existing instruction surfaces (`.github/copilot-instructions.md`, `AGENTS.md`, `CLAUDE.md`) with why each is present and why their presence is NOT PR-authoring enablement. Third sub-section "GitHub-side enablement note" names the repo/org Settings -> Copilot access policy as a procedural guard the regression pack cannot detect. Verified absence of all 4 forbidden file/dir paths on disk.
- [x] Stance-change condition: autonomous-agent support lands as a new top-level TODO with its own review pipeline, `copilot-setup-steps.yml` equivalent, firewall allowlist, and Authority Hierarchy row revision. Until that TODO ships, autonomous PRs fail review with a direct cite.
- [x] MCP-server corollary in its own sub-section: MCP servers that can autonomously commit, push, open PRs, or run long-running tasks are forbidden from `.claude/settings.json` + user-local config. Read-only MCP servers (filesystem, git-read, github-read, docs-search, Microsoft Learn) are fine; they follow the same `receiving-code-review` discipline as Codex and Copilot. Cross-linked to §5's MCP-server-boundary sub-section.
- [x] Regression-pack assertions for §9 already filed (`.github/workflows/copilot-setup-steps.yml`, `.github/agents/`, `.github/chatmodes/` absence checks). No change needed; §9 owns the enforcement surface.
- [x] Commit: `"docs/ai: document autonomous-agent boundary policy -- interactive only"`

> [!TIP]
> This is competitive work. Mature Win11/Linux repos document whether they accept autonomous-agent PRs; fewer document WHY and what they refuse to ship as a consequence. Making the refusal explicit (and linking it to the Authority Hierarchy) keeps the Claude-primary boundary enforceable long-term.

**Test checkpoint:** A maintainer can state the project's autonomous-agent stance in one sentence with a direct citation ([ai-system.md "Autonomous-Agent Boundary Policy"](../../docs/infrastructure/ai-system.md#autonomous-agent-boundary-policy); [CLAUDE.md "Autonomous-agent boundary -- interactive only"](../../CLAUDE.md#autonomous-agent-boundary----interactive-only)). A contributor attempting to enable Copilot coding-agent or Devin finds a documented refusal path before spending cycles. `find .github -type f -name 'copilot-setup-steps.yml' -o -type d -name 'agents' -o -type d -name 'chatmodes'` returns zero paths at the time of commit; §9 regression suite will automate this check.

> **Test runner:** N/A (docs-only + absence check) | validation: four forbidden paths absent on disk (verified via `test -e ...`); `ai-system.md` contains the Autonomous-Agent Boundary Policy section with 3 classification tables; `CLAUDE.md` has the pointer block; §9 regression suite will assert all four absence invariants + the allowed-but-not-enablement classification + the non-automatable GitHub-Settings reminder when it ships.

> **Notes:**
> - Shipped [Autonomous-Agent Boundary Policy](../../docs/infrastructure/ai-system.md#autonomous-agent-boundary-policy) section in `ai-system.md` with 7 sub-sections: Why (3 reasons pinned to Authority Hierarchy invariants #2 + #5 + `feedback_no_substandard_code`), Forbidden-path list (5 rows), Allowed-not-enablement table (3 rows classifying existing instruction surfaces), GitHub-side-enablement note (UI procedural guard the regression can't detect), MCP-server corollary, Stance-change condition (4-part checklist for the future adoption TODO), competitive-edge note.
> - Short pointer block added to `CLAUDE.md` between "Commits -- zero AI-attribution trailers" (§7) and "Git Hooks": names the 4 forbidden paths, classifies the 3 existing instruction surfaces as non-enablement, and forward-links to the full reasoning in `ai-system.md`.
> - Forbidden-path baseline verified at commit time: `.github/workflows/copilot-setup-steps.yml` (absent), `.github/agents/` (absent), `.github/chatmodes/` (absent), `.github/instructions/` (absent). `.github/` currently contains `CODEOWNERS`, `ISSUE_TEMPLATE/`, `PULL_REQUEST_TEMPLATE.md`, `copilot-instructions.md` (reviewer-mode only), `labeler.yml`, `logo.png`, `workflows/` -- all legitimate.
> - Canonical doc: [Autonomous-Agent Boundary Policy](../../docs/infrastructure/ai-system.md#autonomous-agent-boundary-policy). MCP-server read-only/forbidden split cross-references §5's [MCP server boundary](../../docs/infrastructure/ai-system.md#mcp-server-boundary) sub-section.
> - Scope boundary: autonomous-agent refusal + forbidden-path list + allowed-but-not-enablement classification + GitHub-side-enablement procedural guard + MCP-server-autonomy corollary. Commit-disclosure policy (zero-trailer) lives in §7; regression enforcement is §9; external-reviewer contract (Codex/Copilot reviewer-mode) is §4.

> **Verified:** 2026-04-19 | commit `be1d85e2` | 7/7 items | build N/A (docs-only) | 4 forbidden paths absent; 3 allowed-path classifications added
> **Quality reviewed:** 2026-04-19 | Codex 1x (adversarial) | 1M fixed (M: original forbidden-path list covered only 3-4 surfaces, but current Copilot cloud-agent docs name `.github/instructions/*.instructions.md` as a repo instruction input AND list `.github/copilot-instructions.md` + `AGENTS.md` + `CLAUDE.md` as possible inputs; §8 would have certified "no autonomous surface" while cloud-agent could still consume repo instructions if enabled. Expanded the table from 4 to 5 forbidden paths (added `.github/instructions/*.instructions.md`), added a second table classifying the 3 existing instruction surfaces as reviewer/pointer/doctrine NOT PR-authoring enablement, added a GitHub-side-enablement note naming org/repo Settings -> Copilot Access as the procedural guard the regression pack cannot detect; §9 check and CLAUDE.md pointer updated to match), 0 open | scope: N/A (docs-only)

---

## 9. AI Workflow Regression Suite

This is the refinement step: test the workflow itself.

> [!TIP]
> Public Windows and Linux projects may publish contribution rules, but they rarely ship a regression pack that checks whether the AI assistance layer still enforces the project's own doctrine. The existing `scripts/test-tooling.sh` already covers wrapper/hook/YAML drift; this pack complements it by covering skill catalog + doctrine surfaces specifically.

- [x] Add an AI workflow regression pack (`scripts/test-ai-system.sh`) that validates key invariants:
  - **Authority Hierarchy present**: `todo/00-infrastructure/TODO-02-ai-development-system.md` and `docs/infrastructure/ai-system.md` both contain the "Claude Code is the primary interactive orchestrator" statement and the 5 hierarchy invariants (grep for each invariant's first phrase). Drift or deletion = fail with a pointer to this section.
  - **Claude-primary declaration in CLAUDE.md**: `CLAUDE.md` names Claude as the primary interactive orchestrator. If this line disappears, the authority doctrine has silently fragmented and the regression catches it.
  - **`AGENTS.md` pointer file (§6)**: `AGENTS.md` exists at repo root, contains the phrase "Claude Code" and a link to `CLAUDE.md`, stays under 60 lines. Doctrine enforcement is two-sided because §6 intentionally inlines ONE section (master statement + 3-bullet hierarchy summary) verbatim from TODO-02 for cross-tool readers; everything else is pointer-only. (a) **Must-match:** the `## Authority` section in `AGENTS.md` must byte-match the canonical master-statement paragraph + 3 of the 5 invariants in [the AI Development System roadmap Authority Hierarchy block](#authority-hierarchy-read-this-first). Drift fails this check. (b) **Must-not-copy:** no other CLAUDE.md-exclusive doctrine paragraph (bare-metal-first, SMP-from-day-one, POST16-boot-path, no-Unicode-dashes, Bare-Metal-Gotchas entries, Safety-Gates, test-code no-live-boot-infra) appears in `AGENTS.md`. A future edit that imports one of those paragraphs fails the check. (c) **Length:** `AGENTS.md` must be at least 5x shorter than `CLAUDE.md` (hard floor enforced by the regression; 10x is the aspirational target). 5x is the floor rather than 10x because `CLAUDE.md` is already trimmed tight (~280 lines) and `AGENTS.md` has the 60-line cap; a 10x hard gate would force either file to drift out of its intended size envelope.
  - **Zero-trailer commit policy (§7)**: `CLAUDE.md` AND `CONTRIBUTING.md` contain the "no `Co-Authored-By:` / no `Assisted-by:`" policy statement AND the stance-change condition. Commit-body scan uses the §7 adoption anchor, NOT a rolling `HEAD~N` window: resolve `ANCHOR=$(git log --reverse --format='%H' -S 'AI-Assisted Commit Policy (zero trailer)' -- CONTRIBUTING.md | head -1)` to find the first commit that introduced the policy block (use `--reverse | head -1`, not `--diff-filter=A`; CONTRIBUTING.md existed before §7 shipped so the section was added via a modification, not a file creation). Then scan `${ANCHOR}^..HEAD` (inclusive of the adoption commit itself; falls back to `HEAD` when the anchor is the root commit) and run `git interpret-trailers --parse` per commit to flag any trailer matching `Co-Authored-By:` / `Assisted-by:` / `AI-Author:`. Using `git interpret-trailers --parse` -- not raw-body regex -- avoids false positives when a commit body quotes a Co-Authored-By line inside a code block or diff. Commits strictly before `$ANCHOR` are the pre-policy baseline (9 known `Co-authored-by: Copilot` trailers at adoption time; see §7 Notes) and are not flagged. If `$ANCHOR` cannot be resolved the check fails with a clear pointer to this section.
  - **Autonomous-agent boundary**: four repo paths must NOT exist -- `.github/workflows/copilot-setup-steps.yml`, `.github/agents/`, `.github/chatmodes/`, `.github/instructions/`. Presence of any triggers a policy-drift finding. A FIFTH path is now also forbidden as of 2026-04-28: `.github/copilot-instructions.md` (was the Copilot CLI reviewer-mode instruction file; retired wholesale per TODO-08 §14). The allowed-but-not-enablement inputs `AGENTS.md` and `CLAUDE.md` must continue to exist with the cross-tool-pointer / doctrine-source framing they already have (Autonomous-agent stop sign in AGENTS.md, Authority Hierarchy in CLAUDE.md). Separate **non-automatable reminder** printed by the regression script: verify in the GitHub repo/org Settings that Copilot cloud-agent access is disabled for `rizonesoft/impossible-os`; the regression cannot detect UI-side enablement.
  - **Skill catalog consistency**: every directory under `.claude/skills/` has a `SKILL.md` AND an entry in the CLAUDE.md Skills table AND (if auto-loading) an entry in `.claude/skills/README.md` / the system-reminder skill list. Orphan directories = fail.
  - **Doctrine presence**: CLAUDE.md contains the completion-first wording, the bare-metal-first paragraph, the no-Unicode-dashes rule, the no-live-boot-infra-in-tests rule, the SMP-from-day-one rule. Drop-word check against a short canonical list. (Why: these are the doctrine lines `CLAUDE.md` authoritatively owns; if any disappear, the hierarchy is hollow -- Claude-Code-the-master has no doctrine to enforce.)
  - **Root-index link integrity**: all AI-system links in `todo/TODO-00-INDEX.md`, `todo/00-infrastructure/INDEX.md`, and `CLAUDE.md` resolve to real files.
  - **No-Cursor-residue check**: `.cursor/` does not exist; no `.cursor/`-prefixed path is referenced in any tracked file outside explicit history notes marked `removed 2026-04-18`.
  - **No-parallel-skill-tree check**: no directory matching `.codex/skills/`, `.copilot/skills/`, `.other-tool/skills/` etc. exists. The "no parallel skill trees" invariant (hierarchy #2) must stay enforced mechanically, not just in prose.
  - **Hook JSON parse**: `.claude/settings.json` is valid JSON (python3 json.load check).
  - **Copilot CLI reviewer retired**: `.github/copilot-instructions.md` and `scripts/copilot-review.sh` are absent (deleted 2026-04-28 per TODO-08 §14 -- Codex GPT-5.5 is the sole external reviewer). Live-invocation grep confirms no remaining caller in scripts / hooks / `.claude/settings.json`.
- [x] Actionable output: `scripts/test-ai-system.sh` uses the same `t_pass` / `t_fail` pattern as `scripts/test-tooling.sh`, each failure line names the drifted surface + a repair hint, and the summary footer prints repair pointers (canonical docs for each section). Non-automatable items (GitHub-side Copilot cloud-agent enablement) are emitted as `NOTE` advisories rather than checks so the human reminder surfaces without affecting the pass/fail count.
- [x] Wired the regression pack into `make test-ai-system` (new Makefile target; added to `.PHONY` list) and `.github/workflows/build.yml` (new "Run AI workflow regression pack" step that runs `--quiet` before the Build stage, same pattern as the existing `test-tooling.sh` step). CI catches any AI-workflow drift at PR time, before the build stage, same cost/friction profile as the tooling regression.
- [x] Extension path is documented in `scripts/test-ai-system.sh --help` (every check group is a named `check_<name>` function; adding a new group is "write a new function + call it in the run-all block"). Same idiom as `test-tooling.sh`. `trailer_exceptions` array in the §7 check is the template for future one-off grandfather-clauses.
- [x] Commit: `"test/ai: add AI workflow regression suite"`

**Test checkpoint:** On a healthy repo, the AI regression suite passes (`bash scripts/test-ai-system.sh` currently reports 72 PASS, 0 FAIL). If CLAUDE.md loses a doctrine paragraph, a skill directory appears without a Skills-table entry, or a `.cursor/` path sneaks back in via a copy-paste, the suite fails with a specific repair path. Exercised during implementation + review: initial run caught 8 real issues (broken links in `todo/00-infrastructure/INDEX.md` pointing at `12-installer-release` + `../scripts/` / `../tools/` / `../docs/` one-level refs, AGENTS.md over-strict 10x ratio, AGENTS.md accidentally naming CLAUDE.md-exclusive phrases in a negation context, anchor resolution using wrong git-log filter, etc.) -- all fixed before §9 was marked done. Post-commit review surfaced 8 further findings (5 adversarial: AGENTS.md byte-match weak / catalog count-not-name / trailer-scanner brittleness / anchor-excludes-adoption-commit / 5x-vs-10x doc drift + "not edits" vs "never edits" wording drift; 3 quality: mktemp-silent-pass / stale help text / stale TODO prose) -- all fixed; byte-match upgrade caught the AGENTS.md "not edits" vs TODO-02 "never edits" drift that the substring check had missed.

> **Test runner:** `make test-ai-system` | expected: `72 checks passed, 0 failed`. Extends via new `check_<name>` function + call-in-all block; see `scripts/test-ai-system.sh --help` for the full check surface (12 groups).

> **Notes:**
> - Shipped [`scripts/test-ai-system.sh`](../../scripts/test-ai-system.sh) -- 12-group, ~560-line host-side regression pack covering TODO-02 §1-§8 doctrine, catalog, hierarchy, and policy surfaces. Uses the same `t_pass` / `t_fail` output shape as `scripts/test-tooling.sh` for consistent audit-trail grep.
> - Wired into [`Makefile`](../../Makefile) as `make test-ai-system` (alongside existing `make test-tooling`) and into [`.github/workflows/build.yml`](../../.github/workflows/build.yml) as a dedicated "Run AI workflow regression pack" step running `--quiet` before the Build stage. CI catches drift at PR time.
> - Current baseline: 72 checks PASS, 0 FAIL. Documented one grandfathered exception (`c6b30e0f` "tooling: expand and align COUNT.md report" carries a `Co-authored-by: Copilot` trailer merged mid-§7-§8 sprint; not rewritten to avoid destroying the downstream commit chain). Exception surfaces as an advisory `NOTE` so audit remains visible but the check passes on healthy repo state.
> - Canonical doc: [`scripts/test-ai-system.sh --help`](../../scripts/test-ai-system.sh) lists the 12 check groups with their owning TODO-02 sections; extension idiom and grandfather-clause template are in the source.
> - Scope boundary: regression pack asserts doctrine + catalog + policy surfaces -- not their creation. Content authoring lives in §1-§8; this section owns enforcement only.

> **Verified:** 2026-04-19 | commit `3c348e47` | 5/5 items | build N/A (host-side tooling) | 12 check groups; 72 checks PASS, 0 FAIL
> **Quality reviewed:** 2026-04-19 | Codex 2x (adversarial, quality) | 2H+4M+2L fixed, 0 open | scope: N/A (host-side bash regression pack)

---

## OS Comparison

| ⭐  | Feature                        | 🪟 Win11                              | 🐧 Linux                                   | 🚀 Impossible OS                                      |
| --- | ------------------------------ | ------------------------------------- | ------------------------------------------ | ----------------------------------------------------- |
| 💎  | Repo-codified AI instructions  | ✅ `.github/copilot-instructions.md`  | ⚠️ AGENTS.md emerging                      | ✅ §1 ownership map + `CLAUDE.md` authority           |
| 💎  | Skill/catalog ownership        | ⚠️ `.github/chatmodes/` + `/prompts/` | ❌ Ad hoc                                  | ✅ §2 skill lifecycle; no parallel trees              |
| 💎  | Hook policy matrix             | ⚠️ `.vscode/mcp.json` + IDE settings  | ⚠️ Implicit                                | ✅ §3 routing; reminder vs block tiers                |
| 💎  | External-reviewer contract     | ❌ Rare                               | ❌ Rare                                    | ✅ §4 Codex + Copilot reviewer-not-authority          |
| 💎  | Permissions/extension boundary | ⚠️ Varies                             | ⚠️ Varies                                  | ✅ §5 allow/deny tiers + `.local.json` split          |
| 💎  | Cross-tool `AGENTS.md` pointer | ⚠️ awesome-copilot stub               | ✅ LF-backed (Aug 2025)                    | ✅ §6 pointer to `CLAUDE.md`; no duplication          |
| 💎  | AI-assist commit disclosure    | ❌ No convention                      | ✅ kernel `Assisted-by:` trailer (2025-12) | ✅ §7 zero-trailer policy + stance-change condition   |
| ⭐  | Autonomous-agent boundary      | ⚠️ coding-agent + firewall allowlist  | ❌ No formal policy                        | ✅ §8 interactive-only; no Devin / setup-steps        |
| ⭐  | AI workflow regression suite   | ❌ Rare                               | ❌ Rare (Promptfoo/Guardrails; AI apps)    | ✅ §9 catalog + hierarchy + trailer + boundary checks |

> **After §1-§5:** Impossible OS documents its AI workflow as first-class infrastructure: explicit ownership, documented hooks, and a clear external-reviewer contract.
> **After §6-§7:** reaches parity with the 2025-2026 Linux Foundation `AGENTS.md` standard and the Linux kernel's AI-assist commit policy, while preserving its zero-trailer stance explicitly.
> **After §8-§9:** the repo surpasses typical public practice by stating its autonomous-agent boundary (what it refuses to ship as a consequence of the Authority Hierarchy) and by testing the AI workflow itself as maintained infrastructure.

## Unit Tests

> AI workflow checks are host-side regression checks, not kernel `test_runner_init()` suites. They complement the wrapper/hook/YAML checks already in `scripts/test-tooling.sh` (owned by [TODO-01 §10](TODO-01-developer-tooling-stack.md#10-smoke-test-post16-assertions) Unit Tests) by covering the AI-specific surfaces.

- [x] Create `scripts/test-ai-system.sh` with:
  - skill catalog consistency (every `.claude/skills/*` dir is indexed in CLAUDE.md + README)
  - doctrine-word-presence checks across `CLAUDE.md` (completion-first, bare-metal-first, SMP-from-day-one, no-Unicode-dashes, no-live-boot-infra-in-tests)
  - Authority Hierarchy present in TODO-02 + `docs/infrastructure/ai-system.md` (ships with §1)
  - `AGENTS.md` exists + under 60 lines + points to CLAUDE.md + does not restate doctrine (§6 regression)
  - Zero-trailer policy documented + commit scan from the §7 adoption anchor inclusive (`${ANCHOR}^..HEAD`) via `git interpret-trailers --parse` per commit for `Co-Authored-By:` / `Assisted-by:` / `AI-Author:` violations (§7 regression; pre-anchor baseline commits intentionally excluded)
  - Autonomous-agent boundary enforced: `.github/workflows/copilot-setup-steps.yml` / `.github/agents/` / `.github/chatmodes/` do NOT exist (§8 regression)
  - root/index link resolution for all AI-system roadmap files
  - no-`.cursor/`-residue check (removed 2026-04-18; should not reappear via copy-paste)
  - no-parallel-skill-tree check (`.codex/skills/`, `.copilot/skills/`, etc. do not exist)
  - `.claude/settings.json` valid JSON
  - `.github/copilot-instructions.md` exists and references external-reviewer contract
- [x] Wire the AI regression pack into `make test-ai-system` + `.github/workflows/build.yml`
- [x] Commit: `"test/ai: add regression checks for repo AI workflow"` -- shipped as `test/ai: add AI workflow regression suite` (commit `3c348e47`).

## Verification

- [x] **Authority Hierarchy is stated at the top of this TODO**, in `docs/infrastructure/ai-system.md` (once §1 lands), and in references from `CLAUDE.md`. A contributor asking "which AI surface owns this?" finds a direct answer within 30 seconds: Claude is the primary interactive orchestrator and Codex review mode is finding-only.
- [x] The 5 hierarchy invariants (doctrine in `CLAUDE.md` only; skills in `.claude/skills/` only; reviewers return findings, never edits; `CLAUDE.md` wins on conflict; Claude Code remains the only implementation agent) appear in both `TODO-02` and `ai-system.md`; `scripts/test-ai-system.sh` asserts both locations.
- [x] Root roadmap and `00-infrastructure/INDEX.md` point only at real AI/tooling TODO files -- §9 regression group 8 `[8/12] Root-index link integrity`: "36 link(s) across root indexes all resolve" + "AI-system links in CLAUDE.md all resolve" (72 PASS / 0 FAIL).
- [x] Ownership matrix clearly maps doctrine, skills, hooks, and permissions -- and opens with the Authority Hierarchy table before any other row -- [`ai-system.md`](../../docs/infrastructure/ai-system.md) line 1 `# AI Development System -- Ownership Map`, line 5 `## Authority Hierarchy (read this first)` -- Authority is the first `##` heading, before any other content.
- [x] External-reviewer contract is documented and linked from every `codex-*` skill -- 9/9 `codex-*` skills carry `receiving-code-review` or `external-reviewer-contract` reference (verified via `for f in .claude/skills/codex-*/SKILL.md; do grep -q 'receiving-code-review\|external-reviewer-contract' "$f"; done`).
- [x] `scripts/test-ai-system.sh` fails on missing links, skill-catalog drift, `.cursor/`-residue, hierarchy-block deletion, or parallel-skill-tree creation; passes on a healthy repo -- current healthy-repo baseline: 72 PASS, 0 FAIL. §9 regression exercised 8 real issues during implementation (broken INDEX links, AGENTS.md ratio, anchor-filter drift, etc.).
- [x] Hook policy documentation explains which behaviors are reminders versus hard blocks -- [`ai-system.md`](../../docs/infrastructure/ai-system.md#hook-routing-matrix) carries three section headings: `### BLOCK hooks (5)`, `### REMIND hooks (12)`, `### POST-HOC VALIDATE hooks (2)` -- 19 hooks total, each row in a single-source-of-truth table.
- [x] The Claude-primary declaration is stated in `CLAUDE.md` and referenced from this TODO; no parallel instruction tree exists under `.cursor/`, `.codex/`, `.copilot/`, `.windsurf/`, `.aider/`, or `.continue/`.
- [x] `AGENTS.md` exists at repo root, under 60 lines, points to `CLAUDE.md`, does not duplicate doctrine (§6) -- AGENTS.md is 35 lines (< 60 cap), contains 6 `CLAUDE.md` references, passes §9 regression byte-match group 3 (full-sentence phrases in AGENTS.md + TODO-02 + ai-system.md).
- [x] Zero-trailer AI-assist commit policy is documented in `CLAUDE.md` or `CONTRIBUTING.md`, with the stance-change condition named explicitly (§7). A commit-log scan finds zero `Co-Authored-By:` / `Assisted-by:` trailers in recent history. -- §9 regression group 4 anchor-inclusive scan via `git interpret-trailers --parse`: "no NEW AI-attribution trailers since §7 adoption (inclusive of anchor)". One documented exception (`c6b30e0f`) carried forward as an advisory `NOTE`.
- [x] Autonomous-agent boundary is documented: repo does NOT ship `.github/workflows/copilot-setup-steps.yml`, `.github/agents/`, or `.github/chatmodes/`; the refusal-to-ship is explicit, not accidental (§8) -- §9 regression group 5 confirms 4 forbidden paths absent (also `.github/instructions/`). `AGENTS.md` carries the "Autonomous-agent stop sign" section; `CLAUDE.md` section "Autonomous-agent boundary -- interactive only" makes the refusal explicit.

> **Test runner:** `make test-ai-system` | expected: `72 checks passed, 0 failed`. Also runs pre-build in [`.github/workflows/build.yml`](../../.github/workflows/build.yml) step "Run AI workflow regression pack". Complements `bash scripts/test-tooling.sh` (owned by [TODO-01 §10](TODO-01-developer-tooling-stack.md#10-smoke-test-post16-assertions)).
