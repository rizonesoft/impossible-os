# AI Development System Layout

> Canonical reference for the multi-agent AI development setup in Impossible OS.
> Covers the checked-in layout, source-of-truth boundaries, agent adapters, and update rules.
>
> See also: [Multi-Agent Guide](multi-agent-system.md)

---

## Overview

Impossible OS uses a layered AI development system with a **single canonical source** and thin tool-specific adapters:

```
.impossible/            ← Single source of truth (tool-neutral)
├── rules/              ← Coding conventions (plain markdown)
├── workflows/          ← Procedures and workflows (plain markdown)
├── index.md            ← Master orientation
└── context.md          ← Current project state

─── Thin adapters per tool ─────────────────────────────────────

AGENTS.md               ← Antigravity + project overview (reads .impossible/)
CLAUDE.md               ← Claude Code instructions (reads .impossible/)
.github/copilot-instructions.md  ← GitHub Copilot (reads .impossible/)
.cursor/rules/*.mdc     ← Cursor rule adapters → .impossible/rules/
.cursor/skills/         ← Cursor skills (/.cmd, native format)
.agents/workflows/      ← Antigravity workflows (/.cmd)
.claude/commands/       ← Claude Code commands (/.cmd)
.claude/skills/         ← Claude Code skills (SKILL.md format)
```

---

## Checked-In Layout

### `.impossible/` — Single Source of Truth

```
.impossible/
├── index.md            ← Master orientation for any agent
├── context.md          ← Living project state (update after major changes)
├── rules/
│   ├── build.md        ← Safety boundaries, build workflow, command discipline
│   ├── freestanding.md ← Kernel C: no stdlib, allocator rules, crash debugging
│   ├── assembly.md     ← NASM x86-64, UEFI-era assembly constraints
│   ├── api-surface.md  ← Win32-first, Windows-style paths, product direction
│   ├── doc-sync.md     ← Update docs in same task as code changes
│   ├── mcp-usage.md    ← Srclight-only MCP; repo truth over MCP state
│   └── todo-style.md   ← Lean TODO markdown, compact tables, section tags
└── workflows/
    ├── build.md
    ├── create-todo.md
    ├── implement-todo-section.md
    ├── validate-todo-file.md
    ├── verify-todo-section.md
    ├── improve-implementation-order.md
    ├── sync-ai-system.md
    ├── add-asset.md
    ├── docs-convert-todo.md
    ├── docs-validate.md
    ├── specs-create.md
    ├── specs-fact-check.md
    ├── test-fs-fat32.md
    ├── test-hardware.md
    ├── todo-done-check.md
    ├── todo-master-sync.md
    ├── todo-table-format.md
    └── release.md
```

### `.cursor/` — Cursor-Specific

```
.cursor/
├── rules/              ← Thin MDC adapters that surface .impossible/rules/ content
│   ├── safety-build.mdc
│   ├── freestanding-kernel-code.mdc
│   ├── bare-metal-assembly.mdc
│   ├── api-surface-direction.mdc
│   ├── doc-sync-discipline.mdc
│   ├── mcp-usage-discipline.mdc
│   └── todo-markdown-style.mdc
└── skills/             ← Native Cursor skills (trigger as /command-name)
    ├── create-todo/SKILL.md
    ├── implement-todo-section/SKILL.md
    ├── validate-todo-file/SKILL.md
    ├── verify-todo-section/SKILL.md
    ├── improve-implementation-order/SKILL.md
    └── sync-ai-system/SKILL.md
```

### `.claude/` — Claude Code–Specific

```
.claude/
├── commands/           ← Slash commands (/command-name in Claude Code)
│   └── *.md            ← 18 commands, mirrors .impossible/workflows/ naming
└── skills/             ← SKILL.md format (same structure as .cursor/skills/)
```

### `.agents/` — Antigravity-Specific

```
.agents/
├── workflows/          ← Slash commands (/command-name in Antigravity)
│   └── *.md            ← 18 workflows, same naming as .cursor/skills/
└── rules/              ← Legacy only — content migrated to .impossible/rules/
```

---

## Source-of-Truth Table

| Layer | Location | Tracked? | Authoritative? |
|-------|----------|:--------:|:--------------:|
| Rule content | `.impossible/rules/` | ✅ Yes | ✅ Yes |
| Workflow content | `.impossible/workflows/` | ✅ Yes | ✅ Yes |
| Project brief | `AGENTS.md` | ✅ Yes | ✅ Yes |
| Claude Code instructions | `CLAUDE.md` | ✅ Yes | ✅ Yes |
| Copilot instructions | `.github/copilot-instructions.md` | ✅ Yes | ✅ Yes |
| Cursor rule adapters | `.cursor/rules/*.mdc` | ✅ Yes | ✅ Yes (adapters) |
| Cursor skills | `.cursor/skills/*/SKILL.md` | ✅ Yes | ✅ Yes |
| Claude Code commands | `.claude/commands/*.md` | ✅ Yes | ✅ Yes |
| Antigravity workflows | `.agents/workflows/*.md` | ✅ Yes | ✅ Yes |
| Execution roadmap | `todo/` | ✅ Yes | ✅ Yes |
| Arch / infra docs | `docs/` | ✅ Yes | ✅ Yes |
| Srclight code index | `.srclight/` (gitignored) | ❌ No | ❌ No |
| Local IDE state | Cursor, Claude Code local config | ❌ No | ❌ No |
| Antigravity local config | `~/.gemini/antigravity/` | ❌ No | ❌ No |

**The rule:** if guidance matters for future work, it must be in a tracked repo file.

---

## Boundary Model

```
┌──────────────────────────────────────────────────────────────────────┐
│  CANONICAL  (git-tracked, travels with every clone)                  │
│                                                                      │
│  .impossible/rules/  ·  .impossible/workflows/  ·  .impossible/*.md │
│  AGENTS.md  ·  CLAUDE.md  ·  .github/copilot-instructions.md        │
│  .cursor/   ·  .claude/   ·  .agents/           ·  docs/  ·  todo/  │
└──────────────────────────┬───────────────────────────────────────────┘
                           │ feeds
┌──────────────────────────▼───────────────────────────────────────────┐
│  LOCAL / MACHINE-SPECIFIC  (gitignored or personal)                  │
│                                                                      │
│  .srclight/           — disposable index; rebuild any time           │
│  Cursor user rules    — per-developer preferences                    │
│  Cursor MCP auth      — local credentials                            │
│  Antigravity config   — ~/... paths; local only                      │
│  build/               — generated artifacts                          │
└──────────────────────────────────────────────────────────────────────┘
```

---

## What Must Always Be Written Back to Tracked Files

When any of the following change, update the relevant tracked files **in the same task**:

| Change type | Update target |
|-------------|--------------|
| New or changed coding convention | `.impossible/rules/` + thin adapters |
| New or changed build / tooling flow | `.impossible/rules/build.md`, `AGENTS.md` |
| New or changed API, ABI, or path convention | `.impossible/rules/api-surface.md`, `AGENTS.md` |
| New or changed allocator or memory rule | `.impossible/rules/freestanding.md` |
| New MCP server or MCP policy change | `.impossible/rules/mcp-usage.md` |
| New skill, command, or workflow | `.impossible/workflows/`, `.impossible/index.md`, all adapter dirs |
| AI system layout change | This file + `multi-agent-system.md` |
| Naming or domain/TODO structure change | `todo/TODO-00-INDEX.md`, domain `INDEX.md` |

Use the `/sync-ai-system` workflow when conventions change across the system.

---

## Srclight Index Lifecycle

`.srclight/` is a disposable local acceleration cache — never project truth.

**Refresh when:** large file renames, branch switches with structural changes, or repeated disagreement between Srclight and `rg` / direct file reads.

**Recovery:**
1. Fall back to `rg`, direct file reads, repo-grounded evidence
2. Run `srclight index` (add `--embed qwen3-embedding` for hybrid search)
3. Re-run health check before using results as evidence
