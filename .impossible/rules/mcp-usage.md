# MCP Usage Discipline

> **Applies to:** All agents using MCP integrations.
> **Canonical location:** `.impossible/rules/mcp-usage.md`

## Supported MCP Baseline

- **Srclight** is the only supported MCP integration in the first pass.
- Do not use Memory MCP or filesystem MCP as part of the supported baseline. Do not use a repo-local `.memory/` store.
- `clangd` is LSP-only and must not be configured as an MCP server.

## How to Use Srclight

Prefer these Srclight tools in order:

- `hybrid_search(query)` — best for most lookups (keyword + semantic combined)
- `codebase_map()` — session-level orientation at the start of a session
- `symbols_in_file(path)` — enumerate all symbols in a file
- `get_callers` / `get_callees` / `get_dependents` — trace integration points
- `whats_changed()` — discover recent work that may already cover planned scope

## Repo Truth Over MCP State

- Treat `.srclight/` and other MCP state as **disposable local acceleration**, not project truth.
- If Srclight results conflict with tracked files, **trust the repo**.
- Fall back to `rg` (ripgrep), direct file reads, and other repo-grounded evidence when Srclight is unavailable or stale.
- When repeated mismatches suggest stale indexing, refresh or rebuild the local Srclight index before using it as evidence.

## Example

```
# Good: Srclight-first, then verify
1. hybrid_search("pmm_alloc_contiguous") → find likely impl
2. view_file() on the result → confirm against tracked file
3. Update code/TODO/docs based on verified truth

# Bad: Trust Srclight blindly when results seem off
```
