---
name: implement-todo-section
description: Execute one bounded TODO section, resolve XREF dependencies, use the supported build and debug workflow, and update the section state when code or test evidence changes it. Use when implementing a specific TODO section or a clearly scoped subset of a TODO.
---

# Implement TODO Section

## Workflow

1. Read the exact section and its local notes end to end.
2. Resolve dependencies first.
   - Follow every `→ XREF:` line that affects the scoped section.
   - Stop and ask if a prerequisite is incomplete or the scope conflicts with current reality.
3. Choose the right mode before editing.
   - Use read-only exploration for orientation.
   - Use Plan mode for risky, ambiguous, architectural, or multi-file trade-off work.
   - Use Debug mode when runtime evidence is the real bottleneck.
4. Implement only the bounded section scope.
   - Follow the active `.cursor/rules/`.
   - Use the supported tooling contract from `todo/00-infrastructure/TODO-02-developer-tooling-stack.md`.
5. Verify with supported evidence.
   - Use the build and runtime flow in [build-verification.md](build-verification.md).
   - For crash analysis, use `llvm-addr2line-19` and `llvm-objdump-19` before speculating.
   - **Build timeouts:** run incremental builds with `block_until_ms: 60000` and clean builds with `block_until_ms: 120000` so output stays in the foreground and progress is visible live. Never background a build just to avoid waiting.
6. Update the TODO section before finishing.
   - Correct stale checklist state, notes, and verification wording when evidence contradicts the current text.
   - Keep TODO edits scoped to the section you actually executed.
7. Update the Implementation Order table.
   - Find the row(s) whose `Deliverable` maps to the section you just implemented.
   - Change the `Status` cell to `[x]` (fully done) or `[/]` (in progress) to match the evidence.
   - Do not change `Order`, `Deliverable`, or `Depends On` cells unless the implementation revealed they were wrong.
8. Update the OS Comparison table.
   - Find the row(s) whose `Feature` maps to what the section delivers.
   - Replace the placeholder text in the `🚀 Impossible OS` cell with a concrete description of what was implemented and the section reference, e.g. `✅ Done — §N; brief description`.
   - If a row was `⬜ Planned` and is now fully working, change `⬜` to `✅`; if partial, use `🔄`.
   - Do not change the Windows or Linux cells.
9. Commit and push after the section is complete and the build passes.
   - Use the section's `Commit:` line as the commit message.
   - Stage all changed source files, headers, and the updated TODO file together.
   - Account for repo `.githooks/` if installed (pre-commit lint must pass).
   - Always push to `origin/main` immediately after a successful commit (`git push`).

## Guardrails

- Do not create new TODO files here.
- Do not turn this into a broad file-wide roadmap cleanup pass.
- Do not silently widen scope when requirements, repo state, or verification evidence conflict.

## Code Intelligence — Srclight MCP

The `user-srclight` MCP is available for fast symbol search, call-graph navigation, and cross-file dependency analysis.  Use it **instead of grep/Shell search commands** whenever you are navigating unfamiliar code or need semantic understanding — not just pattern matching.

**Use Srclight (not grep) for:**
- Finding where a function is called from — `get_callers(symbol, project)`
- Understanding what a function depends on — `get_callees(symbol, project)`
- Impact analysis before changing a signature or struct — `get_dependents(symbol, project)`
- Locating a symbol when you don't know which file it's in — `get_symbol(name)`
- Natural-language queries like "where is X initialized after Y" — `hybrid_search(query)`

**grep is fine for:**
- Checking if a specific literal string or `#define` exists in a known file
- Listing `#include` lines in a known file
- Existence checks on exact token patterns within a file you already have open

Useful entry points:
- `codebase_map()` — project stats, language breakdown, directory structure (run once per session for orientation)
- `hybrid_search(query)` — best general search; combines keyword + semantic via RRF fusion (e.g. `"where is pmm_alloc_contiguous called"`)
- `get_symbol(name)` — full source of a function or type by name
- `get_callers(symbol, project)` — who calls a function (impact analysis before changing a signature)
- `get_callees(symbol, project)` — what a function calls (understand dependencies before refactoring)
- `get_dependents(symbol, project)` — what breaks if this symbol changes
- `symbols_in_file(path, project)` — all functions/types defined in a file

Pass `project="impossible-os"` when prompted (workspace mode requires it for graph tools).

## Additional Resources

- [build-verification.md](build-verification.md)
