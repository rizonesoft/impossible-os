---
description: Legacy source material for API direction, Srclight usage, and doc-sync policy
---

# Impossible OS — Intelligence & Design Rules

> [!NOTE]
> This file is retained as migration source material. The active Cursor-primary rule equivalents live under `.cursor/rules/`.

## Design Quality

> **Nothing is impossible. Impossible OS aims to outperform Windows 11 and Linux — non-negotiable.**

- **Expansive mindset.** Think bigger than existing OS implementations. If Windows or Linux does something, we do it better. If neither does it, we invent it.
- **Production-level architecture.** Every API, data structure, and subsystem design must be deliberate, optimized, and built for the long term. No prototyping shortcuts.
- **No workarounds in design.** If an interface feels hacky, redesign it. Clean abstractions compound; hacks compound too — in the wrong direction.

## API Surface

- **Win32 is the native API.** User-space programs are PE32+ executables using Win32-style APIs (CreateFile, ReadFile, CreateProcess). POSIX APIs (open, read, fork) are secondary — for the Linux compat layer only.
- **Windows paths are canonical.** Use `C:\Impossible\System32\`, not `/usr/bin/`. Use `C:\Program Files\`, not `/usr/local/`.
- **Control Panel uses .cpl applets.** Settings are exposed via Windows-standard Control Panel Library applets (CPlApplet interface, .cpl extension).

## Code Intelligence

- **Srclight is the supported MCP baseline.** The `.srclight/` index at the repo root provides code indexing via MCP. Use it for symbol search, call graph navigation, type hierarchy, semantic search, git blame, and hotspot analysis.
- **Start sessions with `codebase_map()`** to orient before navigating the codebase.
- **Prefer `hybrid_search()`** for most queries — it combines keyword and semantic search via RRF fusion.
- **Tracked repo files remain canonical.** If Srclight results disagree with tracked files, trust the repo and treat the index as stale or incomplete until verified.
- **Memory MCP and filesystem MCP are not part of the supported first-pass baseline.** If local setups still expose them, treat them as deprecated local-only extras rather than canonical workflow.

## Documentation Consistency

- **Fix inconsistencies as you find them.** When a code change, new convention, or bug fix contradicts information in `AGENTS.md`, `.cursor/rules/`, future `.cursor/skills/`, compatibility notes, or any still-relevant legacy docs, update the affected guidance as part of your current work — don't leave it for later.
- **Common triggers:** new allocator patterns, changed boot sequence, new driver conventions, renamed APIs, new TODO sections, new build flags.
- **Commit doc fixes separately** as `agent: sync docs` when the fix is non-trivial, or bundle with the related code commit when it's a one-liner.
