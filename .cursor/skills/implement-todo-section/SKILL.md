---
name: implement-todo-section
description: Execute one bounded TODO section, resolve XREF dependencies, run embedded adversarial review/fix/re-review, run embedded section verification, and update the section state when code or test evidence changes it.
---

# Implement TODO Section

## Workflow

1. Read the exact section and its local notes end to end.
2. Resolve dependencies first.
   - Follow every `→ XREF:` line that affects the scoped section.
   - Stop and ask if a prerequisite is incomplete or the scope conflicts with current reality.
   - If only part of the section is implementable, execute the unblocked part and keep blocked checklist items open (`[ ]` or `[/]`) with explicit blocker notes.
3. Choose the right mode before editing.
   - Use read-only exploration for orientation.
   - Use Plan mode for risky, ambiguous, architectural, or multi-file trade-off work.
   - Use Debug mode when runtime evidence is the real bottleneck.
4. **Run the applicable code quality skill BEFORE writing code.**
   - If touching files under `src/kernel/`, `include/kernel/`, `src/boot/`, `src/desktop/`, or `src/shell/`: the `kernel-code-quality` skill applies. Walk through its gates before writing.
   - For non-kernel code (future user-mode libraries, tools, scripts): follow whatever quality skill applies to that domain.
   - **Do not skip this step.** Past incidents: SMP race, memory leak, wrong test assertion.
5. Implement only the bounded section scope.
   - Follow the active `.cursor/rules/`.
   - Use the supported tooling contract from `todo/00-infrastructure/TODO-02-developer-tooling-stack.md`.
6. Verify with supported evidence.
   - Use the build and runtime flow in [build-verification.md](build-verification.md).
   - For crash analysis, use `llvm-addr2line-19` and `llvm-objdump-19` before speculating.
   - **Build timeouts:** run incremental builds with `block_until_ms: 60000` and clean builds with `block_until_ms: 120000` so output stays in the foreground and progress is visible live. Never background a build just to avoid waiting.
7. Update the TODO section before finishing.
   - Correct stale checklist state, notes, and verification wording when evidence contradicts the current text.
   - Keep TODO edits scoped to the section you actually executed.
   - **Done proof gate for checklist updates:** only change `[ ]` -> `[x]` when the item is functionally implemented and wired in the real call path.
   - Do not mark done for `*_stub` handlers or normal-path `STATUS_NOT_IMPLEMENTED` placeholders.
   - For SSDT-related claims, verify **service number/index ↔ Nt function ↔ registration/dispatch entry ↔ SSDT Master Table row** consistency before TODO edits.
   - If an item is still blocked by prerequisites, do not force completion; leave it open and add missing unchecked prerequisite/ownership items in prerequisite-first order, with owner scope (`src/...`/symbol) and `→ XREF` where external.
   - **Cross-TODO sync rule:** when this section references or satisfies external TODO requirements, update directly referenced TODO section(s) in the same run so both sides stay consistent.
   - **Cross-TODO auto-closure rule:** propagate `[ ]` -> `[x]` in referenced TODOs only when both conditions are met:
     1. explicit mapping metadata (`ID:` source item and `SATISFIES:` target item ID), and
     2. full target acceptance criteria are proven by evidence.
     If either condition is missing, keep target items open/partial and add concrete missing work.
   - Preserve existing document/table formatting while editing: keep icons, headers, and column structure intact; only update evidence-backed status/content cells.
8. Update the Implementation Order table.
   - Find the row(s) whose `Deliverable` maps to the section you just implemented.
   - Change the `Status` cell to `[x]` (fully done) or `[/]` (in progress) to match the evidence.
   - Do not change `Order`, `Deliverable`, or `Depends On` cells unless the implementation revealed they were wrong.
9. Update the OS Comparison table.
   - Find the row(s) whose `Feature` maps to what the section delivers.
   - Replace the placeholder text in the `🚀 Impossible OS` cell with a concrete description of what was implemented and the section reference, e.g. `✅ Done -- §N; brief description`.
   - If a row was `⬜ Planned` and is now fully working, change `⬜` to `✅`; if partial, use `🔄`.
   - Preserve the OS Comparison table format exactly (including header icons and column order); edit only the relevant status/content cells.
   - Do not change the Windows or Linux cells.
10. Run embedded adversarial review for this section (required).
   - Scope review to this section (`§N`) and changed files/symbols only.
   - Challenge section claims explicitly:
     - Concurrency/SMP safety
     - Error-path handling and rollback behavior
     - Wiring/registration correctness (SSDT/dispatch/hooks where applicable)
     - Boundary and ABI assumptions
   - Produce findings by severity: `Critical`, `High`, `Medium`, `Low`.
11. Run fix -> build/test -> re-review loop (max 3 rounds).
   - Fix all `Critical` and `High` findings.
   - Fix `Medium` unless explicitly accepted with a concrete technical reason.
   - Rebuild after each fix round:
     - `bash scripts/build.sh`
     - confirm `tail -1 build/build.log` is `=== BUILD OK ===`
   - Re-review the same section focusing on previous findings and changed files.
   - If unresolved `Critical`/`High` remain after round 3:
     - do not mark section complete,
     - keep `[/]` or `[ ]`,
     - add explicit follow-up checklist items with ownership and `→ XREF` where needed.
12. Run embedded targeted section verification (required).
   - Verify the section end-to-end against code/build/runtime evidence before final TODO status updates.
   - Classify each checklist item conservatively:
     - `[x]` only when implemented + wired + functional normal path,
     - never `[x]` for stubs/placeholders/normal-path `STATUS_NOT_IMPLEMENTED`.
   - For SSDT claims, verify service number/index ↔ function ↔ registration/dispatch ↔ table row consistency.
   - If blocked/incomplete work remains, keep `[ ]` or `[/]` and add missing prerequisite/ownership items in prerequisite-first order with owner scope and `→ XREF`.
   - Re-check cross-TODO synchronization and auto-closure gates (`ID`/`SATISFIES` + full acceptance coverage).
13. Commit and push after the section is complete and the build passes.
   - Use the section's `Commit:` line as the commit message.
   - Stage all changed source files, headers, and the updated TODO file together.
   - Account for repo `.githooks/` if installed (pre-commit lint must pass).
   - Always push to `origin/main` immediately after a successful commit (`git push`).
   - After a successful push, mark the section's `- [ ] Commit: "..."` checklist item `[x]` in the TODO file and stage + amend the commit so the final state of the TODO reflects a completed commit line. If amend is not safe (commit already pushed), make a follow-up fixup commit instead.

## Guardrails

- **Commit after each implementation.** Do not accumulate multiple implementations before committing. The post-commit hook updates COUNT.md and the user expects incremental progress.
- Do not mark checklist items `[x]` from intent, partial progress, or stubs; require implementation + wiring evidence first.
- For SSDT sections, do not edit checklist/table status until service number ↔ function ↔ registration ↔ master-table consistency is verified.
- If prerequisite work is missing, keep items open/partial and add explicit prerequisite ownership items before dependents.
- Do not auto-close referenced TODO items from plain `→ XREF` text alone; require `ID`/`SATISFIES` mapping plus full acceptance-criteria proof.
- Do not skip embedded adversarial review, fix loop, and embedded section verification before commit.
- Do not create new TODO files here.
- Do not turn this into a broad file-wide roadmap cleanup pass.
- Do not silently widen scope when requirements, repo state, or verification evidence conflict.

## Code Intelligence -- Srclight MCP

The `user-srclight` MCP is available for fast symbol search, call-graph navigation, and cross-file dependency analysis.  Use it **instead of grep/Shell search commands** whenever you are navigating unfamiliar code or need semantic understanding -- not just pattern matching.

**Use Srclight (not grep) for:**
- Finding where a function is called from -- `get_callers(symbol, project)`
- Understanding what a function depends on -- `get_callees(symbol, project)`
- Impact analysis before changing a signature or struct -- `get_dependents(symbol, project)`
- Locating a symbol when you don't know which file it's in -- `get_symbol(name)`
- Natural-language queries like "where is X initialized after Y" -- `hybrid_search(query)`

**grep is fine for:**
- Checking if a specific literal string or `#define` exists in a known file
- Listing `#include` lines in a known file
- Existence checks on exact token patterns within a file you already have open

Useful entry points:
- `codebase_map()` -- project stats, language breakdown, directory structure (run once per session for orientation)
- `hybrid_search(query)` -- best general search; combines keyword + semantic via RRF fusion (e.g. `"where is pmm_alloc_contiguous called"`)
- `get_symbol(name)` -- full source of a function or type by name
- `get_callers(symbol, project)` -- who calls a function (impact analysis before changing a signature)
- `get_callees(symbol, project)` -- what a function calls (understand dependencies before refactoring)
- `get_dependents(symbol, project)` -- what breaks if this symbol changes
- `symbols_in_file(path, project)` -- all functions/types defined in a file

Pass `project="impossible-os"` when prompted (workspace mode requires it for graph tools).

## Additional Resources

- [build-verification.md](build-verification.md)
