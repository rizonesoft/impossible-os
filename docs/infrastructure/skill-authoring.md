# Skill Authoring Lifecycle

> Canonical how-to for adding, editing, and retiring Claude Code skills in this repo. Authority for WHERE skills live and WHICH tool owns them lives in [`ai-system.md`](ai-system.md); this page owns the HOW. Roadmap ownership is [TODO-02 §2](../../todo/00-infrastructure/TODO-02-ai-development-system.md#2-skill-lifecycle-templates-and-catalog-rules).

## No Parallel Skill Trees (hard rule)

Skills live **only** under [`.claude/skills/`](../../.claude/skills/). Do not create `.cursor/`, `.codex/`, `.windsurf/`, or any other tool's skill tree in this repo. This is Authority Hierarchy invariant #2 (see [`ai-system.md`](ai-system.md#hierarchy-invariants)); external tools that want to participate do so via a Claude skill that dispatches them, not via a parallel instruction tree. The `.cursor/` tree was removed 2026-04-18 because maintaining it created clutter without a corresponding productivity win.

---

## Lifecycle

Every new or edited skill goes through the same five steps.

### 1. Scaffold from the template

Copy [`.claude/skills/TEMPLATE.md`](../../.claude/skills/TEMPLATE.md) into a new directory `.claude/skills/<slug>/SKILL.md` and rename. The slug is `kebab-case`, matches the `name:` frontmatter field, and is what the user types as `/slug` to invoke.

### 2. Write the description so auto-load works

The `description:` field is what Claude reads to decide whether a skill is relevant. Claude does not open the body of SKILL.md to decide relevance -- the description is the entire signal. Structure:

```
description: <one-sentence verb-phrase of what this skill does>. Use when <concrete trigger conditions>.
```

- **Start with the action.** "Validate ...", "Create ...", "Run adversarial review for ...". Not "This skill ...".
- **Name concrete triggers.** "Use when a TODO file was just created", not "Use when appropriate".
- **Mention auto-triggers explicitly if the harness hook will invoke this.** Example: "Auto-loads when writing or modifying kernel source files (.c, .h, .asm) in src/ or include/." (see [`.claude/skills/kernel-code-quality/SKILL.md`](../../.claude/skills/kernel-code-quality/SKILL.md)).
- **Auto-loading by description is unreliable.** If the skill MUST run under certain conditions, also add a row to the Mandatory Skill Triggers table in [`CLAUDE.md`](../../CLAUDE.md#mandatory-skill-triggers) (step 4 below).

### 3. Add to the CLAUDE.md Skills table

The Skills table in [`CLAUDE.md`](../../CLAUDE.md#skills) is the user-visible catalog. Every live skill MUST have a row. One line per row, format:

```
| `/slug` | Short one-line description (max ~80 chars) |
```

Keep the one-line description shorter than the frontmatter description -- the catalog is a quick-reference, not the full spec.

### 4. Add a Mandatory Skill Triggers row (if applicable)

If the skill MUST run under specific conditions (file-path edit, Codex dispatch completion, test-file creation), add a row to the Mandatory Skill Triggers table in [`CLAUDE.md`](../../CLAUDE.md#mandatory-skill-triggers) and wire a PostToolUse / PreToolUse hook in [`.claude/settings.json`](../../.claude/settings.json) that injects a reminder. Skip if the skill is purely user-invoked.

### 5. Update `.claude/skills/README.md` catalog listing

The README under `.claude/skills/` is the in-tree catalog. Every live skill MUST also appear there. The README is where a contributor browsing the file tree discovers what skills exist; keep it sync'd with the CLAUDE.md Skills table.

---

## Skill Template (canonical sections)

Two canonical shapes cover every skill in the repo. Pick the one that matches your skill's purpose.

**Shape A -- Workflow skill** (most skills: TODO workflow, review, Codex dispatch, debugging). A mechanical procedure Claude executes on demand.

1. **Frontmatter** (`name`, `description`) -- structure described in step 2 of Lifecycle above. The description is the canonical trigger surface; if it states concrete invocation conditions ("Use when..."), the `## Use This Skill When` section is optional.
2. **`# Human Title`** -- H1 heading matching the skill's purpose.
3. **`## Use This Skill When`** (required, with two exceptions below) -- bullet list of concrete triggers.
   - **Exception 1 -- frontmatter trigger carry.** High-rigor skills that lead with `## Execution Discipline` and carry full trigger language in the frontmatter `description` MAY omit this section. Current exceptions: `implement-todo-section`, `review-todo-section`, `quality-review-section`.
   - **Exception 2 -- auto-triggered.** Skills invoked by a harness hook (auto-load via `description` matcher or a PostToolUse hook reminder) have their triggers in `.claude/settings.json`; this section is still useful as human-readable context but is optional.
4. **`## Workflow`** or **`## Pipeline`** -- numbered steps. The mechanical procedure.
5. **`## Guardrails`** or **`## Rules`** -- what the skill must NOT do, what it must always do. Optional for skills whose workflow steps already encode the constraints inline (current: `debug-session`).

**Shape B -- Code-quality skill** (5 skills: `boot-code-quality`, `kernel-code-quality`, `desktop-code-quality`, `shell-code-quality`, `userland-code-quality`). Pre-write/post-write gates for a specific source-tree area. Auto-loads on path match via `.claude/settings.json` hooks; never user-invoked.

1. **Frontmatter** (`name`, `description`) -- `description` MUST end with "Auto-loads when editing <path>/ files." so the harness hook has a clear invariant to enforce.
2. **`# Human Title`**.
3. **`## When This Applies`** -- names the exact path-glob that triggers auto-load (e.g. "Every time you create or modify a `.c`/`.h`/`.asm` file under `src/boot/`.").
4. **`## Pre-Write Checklist`** -- numbered gates (Gate 1, Gate 2, ...) that must pass before writing code. Each gate has its own `###` subheading with a checkbox list.
5. **`## Post-Write Verification`** (optional) -- checks to run after code is written.
6. **`## Self-Update Protocol`** (optional) -- rules for how the skill itself should be extended when new lessons land.

Both shapes share universal constraints:

- **No restated doctrine.** Link to CLAUDE.md sections instead; invariant is that CLAUDE.md is the only place a rule is defined.
- **No Unicode en/em dashes** (U+2013, U+2014). ASCII `--` only, per the project rule in [`CLAUDE.md`](../../CLAUDE.md#no-unicode-dashes-enem-ascii-only).
- **No `[Opus]` / `[Sonnet]` model tags** on headings; they drift as models change. Model Roles in CLAUDE.md owns the policy.

If your skill needs a shape neither A nor B covers, update this document BEFORE shipping the skill -- the TODO-02 §9 regression suite enforces shape compliance, so unlisted shapes will be flagged.

**Optional sections** (add when load-bearing, not for padding):

- **`## Execution Discipline`** at the top -- for high-rigor skills that have been corner-cut in the past. Prose callout naming the specific failure modes the skill guards against.
- **`## Additional Resources`** at the bottom -- links to shared files under the same skill directory (e.g., `build-evidence.md`, `scope-gap-protocol.md`) or to related skills.
- **`## Relationship to <other-skill>`** -- when two skills overlap (e.g., verify vs implement), name the boundary and why both exist.

**Forbidden patterns:**

- No restated doctrine. Skills reference `CLAUDE.md` sections instead. Invariant: the one rule that `CLAUDE.md` owns, the skill must not redefine.
- No Unicode en/em dashes (U+2013, U+2014) anywhere in SKILL.md. ASCII `--` only, per the project rule in [`CLAUDE.md`](../../CLAUDE.md#no-unicode-dashes-enem-ascii-only).
- No `[Opus]` / `[Sonnet]` model tags on headings; they drift as models change. The Model Roles section of CLAUDE.md names the policy.

---

## Catalog Hygiene

Three canonical locations must stay in sync. A skill that exists in only one is drift.

| Location                                              | Purpose                                                          |
| ----------------------------------------------------- | ---------------------------------------------------------------- |
| [`.claude/skills/<slug>/`](../../.claude/skills/)     | Source of truth: the SKILL.md file and any shared helpers        |
| [`CLAUDE.md` Skills table](../../CLAUDE.md#skills)    | User-visible catalog in the doctrine file                        |
| [`.claude/skills/README.md`](../../.claude/skills/README.md) | In-tree catalog for file-tree browsers                    |

**Sync invariants:**

- Every directory under `.claude/skills/` (except `README.md` itself) is a live skill with a `SKILL.md`.
- Every live skill has exactly one row in the CLAUDE.md Skills table AND one row in `.claude/skills/README.md`.
- A skill directory without matching index rows is dead weight; either index it or retire it. No silent orphans.
- `§9` regression suite (TODO-02 §9) enforces this sync automatically.

---

## Retirement / Supersession

When a skill is no longer needed (superseded, merged into another, or the workflow it enforces is retired):

1. **Delete the skill directory**: `git rm -rf .claude/skills/<slug>/`.
2. **Delete the CLAUDE.md Skills table row** for `/slug`.
3. **Delete the `.claude/skills/README.md` entry** for `/slug`.
4. **Delete any Mandatory Skill Triggers row** that referenced the skill.
5. **Delete any PostToolUse / PreToolUse hook** in `.claude/settings.json` that invoked the retired skill.
6. **Search for XREF fallout across invocation AND bare-name forms:** skills are referenced in many shapes, so a single `/<slug>` grep misses most of them. Run all of:
   - `rg -n "/<slug>\b" CLAUDE.md .claude/ todo/ docs/ scripts/` -- slash-invocation form (user-typed `/slug`).
   - `rg -n "\b<slug>\b" CLAUDE.md .claude/ todo/ docs/ scripts/` -- bare name: Mandatory Skill Triggers rows, stamp prose, skill-to-skill prose links.
   - `rg -n "\.claude/skills/<slug>" CLAUDE.md .claude/ todo/ docs/ scripts/` -- markdown path links.
   - `rg -n "<slug>" .claude/settings.json` -- embedded hook strings (matcher names, reminder text, Python `if name == 'slug'` checks).
   Every hit is stale; remove or redirect to the superseding skill. Count check: for `kernel-code-quality` the bare form yields ~50x more hits than the slash form -- relying on `/<slug>` alone would miss nearly everything.
7. **Land all of the above in one commit.** Partial retirement (directory gone, catalog row remaining) is the exact drift the catalog hygiene rule forbids.

The same 7-step sweep applies to renames: treat a rename as retire-old + add-new, not an in-place edit.

---

## See Also

- [`ai-system.md`](ai-system.md) -- Authority Hierarchy, Claude Code-Only Stance, global-vs-tool-local doctrine split. This page is its how-to companion.
- [TODO-02 AI Development System](../../todo/00-infrastructure/TODO-02-ai-development-system.md) -- roadmap ownership; §2 owns this document, §3 owns hook routing, §9 owns the regression suite that enforces the sync invariant.
- [`.claude/skills/TEMPLATE.md`](../../.claude/skills/TEMPLATE.md) -- the copy-pasteable skeleton referenced in step 1.
- [`CLAUDE.md`](../../CLAUDE.md) -- doctrine; Skills table and Mandatory Skill Triggers table both live there.
