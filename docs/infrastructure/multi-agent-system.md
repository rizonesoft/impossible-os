# Multi-Agent Development System

> How Cursor, Claude Code, Antigravity, and GitHub Copilot work together on Impossible OS.

---

## The Problem Being Solved

Each AI coding tool has its own way of loading project context:

| Tool | Reads from | Slash commands | Rules |
|------|-----------|---------------|-------|
| **Cursor** | Native IDE context | `.cursor/skills/` | `.cursor/rules/*.mdc` |
| **Claude Code** | `CLAUDE.md` | `.claude/commands/` + `.claude/skills/` | Inline |
| **Antigravity** | `AGENTS.md` | `.agents/workflows/` | Inline |
| **Copilot** | `.github/copilot-instructions.md` | None | Inline |

Without coordination, each agent operates from a different set of rules and procedures — drift accumulates, commands are named differently, and conventions enforced in one tool are invisible to another.

---

## The Solution: `.impossible/`

A tool-neutral directory that is the **single source of truth** for all rules and workflows. Every agent reads from here via a thin adapter. When a convention changes, it changes in one place.

```
.impossible/
├── rules/          ← Plain markdown. No tool-specific frontmatter.
└── workflows/      ← Plain markdown. Tool-neutral procedures.
```

### Design Principles

1. **Plain markdown only** — files in `.impossible/` have no agent-specific YAML frontmatter, blobs, or special syntax. Any tool, IDE, or human can read them.
2. **Adapters are thin** — each tool's native config (`.cursor/rules/*.mdc`, `CLAUDE.md`, etc.) does the minimum needed to surface the content. Heavy guidance lives in `.impossible/`, not in adapters.
3. **Naming is unified** — slash command names are identical across all tools. `/implement-todo-section` in Cursor, Claude Code, and Antigravity runs the same workflow.
4. **Skills stay native** — Cursor skills (`.cursor/skills/`) and Claude Code skills (`.claude/skills/`) keep their native format and tool-specific discovery mechanism. Only the procedural content is shared via `.impossible/workflows/`.

---

## Slash Command Consistency

All tools expose the same set of `/commands`. The name is derived from the Cursor skill naming convention (descriptive, dash-separated):

| Command | Cursor | Claude Code | Antigravity |
|---------|--------|-------------|-------------|
| `/build` | — | ✅ | ✅ |
| `/create-todo` | ✅ | ✅ | ✅ |
| `/implement-todo-section` | ✅ | ✅ | ✅ |
| `/validate-todo-file` | ✅ | ✅ | ✅ |
| `/verify-todo-section` | ✅ | ✅ | ✅ |
| `/improve-implementation-order` | ✅ | ✅ | — |
| `/sync-ai-system` | ✅ | ✅ | ✅ |
| `/add-asset` | — | ✅ | ✅ |
| `/docs-convert-todo` | — | ✅ | ✅ |
| `/docs-validate` | — | ✅ | ✅ |
| `/specs-create` | — | ✅ | ✅ |
| `/specs-fact-check` | — | ✅ | ✅ |
| `/test-fs-fat32` | — | ✅ | ✅ |
| `/test-hardware` | — | ✅ | ✅ |
| `/todo-done-check` | — | ✅ | ✅ |
| `/todo-master-sync` | — | ✅ | ✅ |
| `/todo-table-format` | — | ✅ | ✅ |
| `/release` | — | ✅ | ✅ |

Cursor skills are the **authoritative procedure** for all shared commands. Claude Code commands and Antigravity workflows point to `.impossible/workflows/` which summarizes the Cursor skill.

---

## Rules Flow

```
.impossible/rules/build.md          ← Edit this to change the rule
         │
         ├── .cursor/rules/safety-build.mdc       (Cursor adapter)
         ├── CLAUDE.md (inline summary)             (Claude Code)
         ├── .github/copilot-instructions.md        (Copilot)
         └── AGENTS.md (inline summary)             (Antigravity)
```

The `.mdc` adapters in `.cursor/rules/` can reference the `.impossible/rules/` content for Cursor-specific surfacing (glob triggers, `alwaysApply`, etc.) while the prose content lives in `.impossible/`.

---

## When to Use Which Tool

| Situation | Best tool |
|-----------|-----------|
| Primary development | **Cursor + Claude Code** (source of truth) |
| Quick autocomplete / symbol lookup | **Copilot** |
| Long autonomous tasks (multi-file, multi-step) | **Antigravity** |
| Building and testing | Any — `/build` works everywhere |
| Creating/validating TODO files | Any — `/create-todo`, `/validate-todo-file` |
| Committing and pushing | **Cursor** (pre-commit hooks run) |

---

## Maintaining the System

### Adding a New Rule

1. Create `.impossible/rules/<name>.md` — plain markdown, no frontmatter
2. Create `.cursor/rules/<name>.mdc` — thin adapter with globs and a summary
3. Add a summary line to `CLAUDE.md` and `.github/copilot-instructions.md`
4. Update `.impossible/index.md` rules table
5. Run `/sync-ai-system` to verify nothing was missed

### Adding a New Workflow/Command

1. Create `.impossible/workflows/<name>.md` — summary + canonical source reference
2. Create `.agents/workflows/<name>.md` — full procedure for Antigravity
3. Create `.claude/commands/<name>.md` — thin delegate for Claude Code
4. For Cursor native support, create `.cursor/skills/<name>/SKILL.md`
5. Update `.impossible/index.md` workflows table and `CLAUDE.md` commands table
6. Update `AGENTS.md` Antigravity Workflows list

### Updating an Existing Convention

1. Edit `.impossible/rules/<name>.md` first
2. Check all adapters (`.cursor/rules/`, `CLAUDE.md`, `AGENTS.md`, `copilot-instructions.md`) for stale inline summaries
3. Use `/sync-ai-system` for guidance on what to touch

### Keeping `context.md` Fresh

Update `.impossible/context.md` after:
- A subsystem reaches a milestone (change status row)
- A new active work area starts
- A known blocker is discovered or resolved
- Agent infrastructure changes (like this setup)

---

## Adding a New AI Tool

1. Find its instruction file format (e.g. `TOOLNAME.md`, `.tool/instructions.md`)
2. Create that file with the critical rules inline + a pointer to `.impossible/`
3. Find its slash command format and create commands pointing to `.impossible/workflows/`
4. Add it to the adapter table in `AGENTS.md` and this document
5. Run `/sync-ai-system`

---

## File Ownership

| File | Who edits it | Who reads it |
|------|-------------|-------------|
| `.impossible/rules/*.md` | Developer / any agent | All agents |
| `.impossible/workflows/*.md` | Developer / any agent | All agents |
| `.impossible/context.md` | Developer / any agent | All agents (session start) |
| `.cursor/rules/*.mdc` | Developer | Cursor only |
| `.cursor/skills/*/SKILL.md` | Developer | Cursor + Claude Code |
| `CLAUDE.md` | Developer | Claude Code |
| `.claude/commands/*.md` | Developer | Claude Code |
| `AGENTS.md` | Developer | Antigravity + humans |
| `.agents/workflows/*.md` | Developer | Antigravity |
| `.github/copilot-instructions.md` | Developer | GitHub Copilot |
