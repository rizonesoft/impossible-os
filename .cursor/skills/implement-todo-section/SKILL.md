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
9. If a commit is requested, use the section's commit line and account for repo `.githooks/` if installed.

## Guardrails

- Do not create new TODO files here.
- Do not turn this into a broad file-wide roadmap cleanup pass.
- Do not silently widen scope when requirements, repo state, or verification evidence conflict.

## Additional Resources

- [build-verification.md](build-verification.md)
