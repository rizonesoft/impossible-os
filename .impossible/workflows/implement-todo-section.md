---
description: Execute one bounded TODO section, resolve XREF dependencies, use the supported build and debug workflow, and update the section state when code or test evidence changes it.
---

# Implement TODO Section

## Workflow

1. Read the exact section and its local notes end to end.
2. Resolve dependencies first.
   - Follow every `→ XREF:` line that affects the scoped section.
   - Stop and ask if a prerequisite is incomplete or the scope conflicts with current reality.
3. Plan before editing.
   - Use read-only exploration for orientation.
   - Plan risky, ambiguous, architectural, or multi-file trade-off work before jumping in.
4. Implement only the bounded section scope.
   - Follow the project coding conventions in `.impossible/rules/`.
   - Use the supported tooling contract from `todo/00-infrastructure/TODO-02-developer-tooling-stack.md`.
5. Verify with supported evidence.
   - Build: `bash scripts/build.sh` → check `tail -1 build/build.log` for `=== BUILD OK ===`.
   - Runtime: `bash scripts/build.sh run` → verify serial output in QEMU.
   - For crash analysis, use `llvm-addr2line-19 -e build/kernel.exe -f <RIP>` and `llvm-objdump-19` before speculating.
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

## Supporting References

- [build-evidence.md](build-evidence.md) — build, test, and crash analysis guide
- [status-evidence.md](status-evidence.md) — status classification and evidence rules
