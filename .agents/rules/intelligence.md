---
description: API design direction and code intelligence tools (Srclight + Memory MCP)
---

# Impossible OS — Intelligence & Design Rules

## API Surface

- **Win32 is the native API.** User-space programs are PE32+ executables using Win32-style APIs (CreateFile, ReadFile, CreateProcess). POSIX APIs (open, read, fork) are secondary — for the Linux compat layer only.
- **Windows paths are canonical.** Use `C:\Impossible\System32\`, not `/usr/bin/`. Use `C:\Program Files\`, not `/usr/local/`.
- **Control Panel uses .cpl applets.** Settings are exposed via Windows-standard Control Panel Library applets (CPlApplet interface, .cpl extension).

## Code Intelligence

- **Srclight is available.** The `.srclight/` index at the repo root provides deep code indexing via MCP. Use it for symbol search, call graph navigation, type hierarchy, semantic search, git blame, and hotspot analysis.
- **Start sessions with `codebase_map()`** to orient before navigating the codebase.
- **Prefer `hybrid_search()`** for most queries — it combines keyword + semantic search via RRF fusion.
- **Memory MCP is available** for persistent cross-session knowledge. Use it to:
  - **Read the graph at session start** (`read_graph`) to recall prior context — architectural decisions, known bugs, in-progress work, and component relationships.
  - **Create entities** for significant discoveries: hardware quirks, driver behavior, subsystem interfaces, and design decisions that future sessions will need. Use descriptive entity types (`bug`, `design_decision`, `driver`, `subsystem`, `gotcha`, `pattern`).
  - **Add observations** to existing entities as you learn more — don't duplicate entities, enrich them.
  - **Create relations** to link entities (e.g., `virtio_blk` → `uses` → `blkdev_interface`, `ahci_driver` → `depends_on` → `pci_subsystem`). Use active-voice relation types: `uses`, `depends_on`, `implements`, `conflicts_with`, `fixed_by`, `discovered_in`.
  - **Search nodes** (`search_nodes`) before creating new entities to avoid duplicates.
  - **Don't store transient data** — build errors, one-off debugging notes, or anything already in the TODO system. Memory is for hard-won knowledge that would be costly to rediscover.

## Documentation Consistency

- **Fix inconsistencies as you find them.** When a code change, new convention, or bug fix contradicts information in `AGENTS.md`, `.agents/rules/`, or `.agents/workflows/`, update the affected docs as part of your current work — don't leave it for later.
- **Common triggers:** new allocator patterns, changed boot sequence, new driver conventions, renamed APIs, new TODO sections, new build flags.
- **Commit doc fixes separately** as `agent: sync docs` when the fix is non-trivial, or bundle with the related code commit when it's a one-liner.
