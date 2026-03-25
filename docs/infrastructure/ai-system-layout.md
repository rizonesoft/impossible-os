# AI Development System Layout

> Canonical reference for the Cursor-primary AI development setup.
> Covers the checked-in layout, source-of-truth boundaries, and update rules.

---

## Checked-In Layout

All durable AI-facing project guidance lives in the repo and travels with every clone.

```
.cursor/
├── rules/                     ← Project-scoped Cursor rules (.mdc)
│   ├── api-surface-direction.mdc
│   ├── bare-metal-assembly.mdc
│   ├── doc-sync-discipline.mdc
│   ├── freestanding-kernel-code.mdc
│   ├── mcp-usage-discipline.mdc
│   ├── safety-build.mdc
│   └── todo-markdown-style.mdc
└── skills/                    ← Project-scoped Cursor skills
    ├── create-todo/
    │   ├── SKILL.md            ← Skill entry point (always SKILL.md)
    │   ├── todo-template.md    ← Supporting reference
    │   └── implementation-order.md
    ├── implement-todo-section/
    │   ├── SKILL.md
    │   └── build-verification.md
    ├── validate-todo-file/
    │   ├── SKILL.md
    │   └── validation-checklist.md
    ├── verify-todo-section/
    │   ├── SKILL.md
    │   └── status-evidence.md
    └── sync-ai-system/
        ├── SKILL.md
        └── ai-sync-checklist.md

docs/infrastructure/
├── ai-system-layout.md        ← This file — canonical layout reference
├── development-tooling.md     ← Build system, tools, QEMU, CI
└── github-setup.md

AGENTS.md                      ← Human-facing project brief + philosophy
todo/                          ← Execution roadmap (domain-based)
```

### Layout Rules

- Every rule lives as a single `.mdc` file directly inside `.cursor/rules/`. No subdirectories.
- Every skill lives in its own subdirectory under `.cursor/skills/<name>/` with a `SKILL.md` entry point.
  Supporting reference files (checklists, templates, examples) live alongside `SKILL.md` in the same directory.
- Rules use `alwaysApply: false` and file-scoped globs. No new always-on rules unless the scope demands it.
- Skills own reusable multi-step workflows. Single-step or one-shot flows belong in a command or explicit task, not a skill.
- `AGENTS.md` is the human-facing overview. Rules contain concise enforceable guidance only.
  Long narrative context belongs in `AGENTS.md` or `docs/`, not duplicated in every rule.

---

## Source-of-Truth Table

| Layer                         | Location                                           | Tracked? | Authoritative?     |
| ----------------------------- | -------------------------------------------------- | :------: | :----------------: |
| Project brief and philosophy  | `AGENTS.md`                                        | ✅ Yes   | ✅ Yes            |
| Cursor project rules          | `.cursor/rules/*.mdc`                              | ✅ Yes   | ✅ Yes            |
| Cursor project skills         | `.cursor/skills/*/SKILL.md` + support files        | ✅ Yes   | ✅ Yes            |
| Infrastructure and arch docs  | `docs/`                                            | ✅ Yes   | ✅ Yes            |
| Execution roadmap             | `todo/`                                            | ✅ Yes   | ✅ Yes            |
| Legacy agent rules/workflows  | `.agents/rules/`, `.agents/workflows/`             | ✅ Yes   | ⚠️ Reference only |
| Srclight code index           | `.srclight/` (gitignored)                          | ❌ No    | ❌ No             |
| Cursor user rules/skills      | Local Cursor user settings                         | ❌ No    | ❌ No             |
| Cursor MCP auth / state       | Local IDE state                                    | ❌ No    | ❌ No             |
| Antigravity local config      | `~/.gemini/antigravity/`, `~/.antigravity-server/` | ❌ No    | ❌ No             |
| Team Rules (Cursor dashboard) | Cursor cloud (optional)                            | ❌ No    | ❌ Overlay only   |
| Bugbot / PR review rules      | Cursor dashboard (optional)                        | ❌ No    | ❌ Overlay only   |
| Cloud agents / automations    | Cursor dashboard (optional)                        | ❌ No    | ❌ Overlay only   |

**The rule:** if guidance matters for future work, it must be in a tracked repo file.
Local state and cloud dashboards may accelerate work but must never be the only place a convention is recorded.

---

## Boundary Model

```
┌─────────────────────────────────────────────────────────────────────┐
│  CANONICAL  (git-tracked, travels with every clone)                 │
│                                                                     │
│  AGENTS.md  ·  .cursor/rules/  ·  .cursor/skills/                   │
│  docs/      ·  todo/           ·  .agents/ (reference only)         │
└───────────────────────────┬─────────────────────────────────────────┘
                            │ feeds
┌───────────────────────────▼─────────────────────────────────────────┐
│  LOCAL / MACHINE-SPECIFIC  (gitignored or personal)                 │
│                                                                     │
│  .srclight/           — disposable index; rebuild any time          │
│  Cursor user rules    — per-developer preferences                   │
│  Cursor MCP auth      — local credentials                           │
│  Antigravity config   — ~/... paths; local only                     │
│  build/               — generated artifacts                         │
└───────────────────────────┬─────────────────────────────────────────┘
                            │ optional overlay
┌───────────────────────────▼─────────────────────────────────────────┐
│  CLOUD / DASHBOARD  (Cursor cloud, team, or dashboard-managed)      │
│                                                                     │
│  Team Rules   — may overlay project rules; never replace them       │
│  Bugbot        — PR review automation; downstream of repo guidance  │
│  Cloud agents  — must treat repo files as authority                 │
└─────────────────────────────────────────────────────────────────────┘
```

Anything adopted from the Cloud/Dashboard layer that becomes normative must also be
recorded in the Canonical layer. Dashboard features are downstream of tracked guidance,
not upstream of it.

---

## What Must Always Be Written Back to Tracked Files

When any of the following change, update the relevant tracked files **in the same task**
before the work is considered complete:

| Change type | Update target |
| ----------- | ------------- |
| New or changed coding convention | `.cursor/rules/` and/or `AGENTS.md` |
| New or changed build / tooling flow | `docs/infrastructure/development-tooling.md`, `safety-build.mdc` |
| New or changed API, ABI, or path convention | `AGENTS.md`, `api-surface-direction.mdc` |
| New or changed allocator or memory rule | `freestanding-kernel-code.mdc`, `AGENTS.md` |
| New MCP server or MCP policy change | `mcp-usage-discipline.mdc`, this file |
| New skill or rule added or retired | This file, `TODO-01-ai-development-system.md` |
| Antigravity compatibility change | `docs/infrastructure/ai-system-layout.md` §Antigravity |
| Naming, domain, or TODO structure change | `todo/TODO-00-INDEX.md`, domain `INDEX.md` |

---

## Update Path For Architecture Changes

When conventions, tooling, or AI guidance changes:

1. Update the tracked canonical source first.
2. Verify that no other tracked file contradicts the new convention.
3. If `.agents/rules/` or `.agents/workflows/` contain the old guidance, update or retire them.
4. If a rule or skill becomes stale, update it; do not leave a known contradiction for later.
5. Summarize the change in the commit message so future reviewers can trace it.

Use the `sync-ai-system` skill for guidance on what to touch when AI conventions change.

---

## Cursor-Primary Declaration

Cursor is the primary AI development environment. All rules and skills are
Cursor-native (`.cursor/rules/`, `.cursor/skills/`). Antigravity is a supported
secondary consumer of the same repo-tracked guidance; it does not own a parallel
rule system.

---

## Antigravity Compatibility Path

Antigravity works from the same project docs and rules. It does not need a separate
rule system. The local Antigravity setup (`mcp_config.json`, `settings.json`) is a
machine-local configuration; it is not tracked and not authoritative.

**Minimum compatible Antigravity setup:**
- Srclight MCP enabled: `srclight serve --transport stdio --workspace dev-workspace`
- Memory MCP: disabled (not in the supported baseline)
- Filesystem MCP: disabled (not in the supported baseline)
- Project context: read from `AGENTS.md`, `.cursor/rules/`, `docs/`, and `todo/`

When Antigravity guidance and Cursor guidance conflict, the tracked repo files win.
Antigravity should be re-oriented to repo truth, not the other way around.

---

## Srclight Index Lifecycle

`.srclight/` is a disposable local acceleration cache. It is never project truth.

**Refresh when:**
- Large file renames or directory moves
- Branch or worktree switch with significant structural changes
- Repeated disagreement between Srclight results and `rg` / direct file reads
- Tooling or dependency changes that affect the indexed symbol set

**Health check:**
- Known symbols and current file paths can be found accurately
- Recently changed code appears with correct content
- `codebase_map()` reports expected project stats

**Stale-index symptoms:**
- Missing files or deleted symbols still appearing
- Outdated paths or wrong implementations returned
- Srclight results consistently disagree with `rg` or direct reads

**Recovery path:**
1. Stop trusting the bad result immediately
2. Fall back to `rg`, direct file reads, and repo-grounded evidence
3. Run `srclight index` (add `--embed qwen3-embedding` for hybrid search)
4. Restart the MCP session if needed
5. Re-run the health check above before using Srclight results as evidence again
