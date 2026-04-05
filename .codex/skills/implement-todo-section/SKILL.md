---
name: implement-todo-section
description: Execute one bounded TODO section, resolve XREF dependencies, verify with build/runtime evidence, and update TODO state with strict proof-gated completion and cross-TODO synchronization.
---

# Implement TODO Section

## Workflow

1. Read the exact section and local notes end to end.
2. Resolve dependencies first.
   - Follow every `→ XREF:` line affecting the scoped section.
   - If prerequisites conflict with current reality, stop and surface the conflict.
   - If only part of the section is implementable now, implement only that subset and keep blocked items open (`[ ]` or `[/]`) with explicit blocker notes.
3. Explore the codebase before editing.
   - Trace symbols, call paths, and integration points in source before changing code.
4. Run code-quality checks before writing.
   - If a domain-specific quality skill exists (for example `kernel-code-quality`), run it first.
5. Implement only the bounded section scope.
   - Keep freestanding-kernel constraints and existing project conventions.
   - For boot-path/hardware changes, add temporary `POST16()` diagnostics in the `0xD000–0xDFFF` debug range after checking for conflicts.
6. Verify with evidence.
   - Build: `bash scripts/build.sh` (or `bash scripts/build.sh clean` when needed).
   - Build pass criteria: `tail -1 build/build.log` contains `=== BUILD OK ===`.
   - Runtime/debug as needed: `bash scripts/build.sh run`, `llvm-addr2line-19`, `llvm-objdump-19`.
7. Update TODO content for the implemented section.
   - Correct stale checklist state, notes, and checkpoint wording when evidence contradicts current text.
   - Keep edits scoped to the section executed.
   - **Done proof gate:** set `[x]` only when implementation exists, is wired, and normal path works.
   - Never mark done for `*_stub`, placeholder flow, or normal-path `STATUS_NOT_IMPLEMENTED`.
   - **SSDT consistency rule:** when SSDT claims are present, verify service number/index ↔ Nt function ↔ registration/dispatch entry ↔ SSDT Master Table row consistency before TODO edits.
   - If work is blocked, keep the item open and add missing unchecked prerequisite/ownership items in logical order with owner scope (`src/...`/symbol) and `→ XREF` where external.
   - Preserve document/table formatting (including header icons, table structure, and section headers); edit only evidence-backed status/content cells.
8. Synchronize referenced TODOs when needed.
   - If this section references/satisfies external TODO requirements, update directly referenced TODO section(s) in the same run so both sides stay consistent.
   - **Cross-TODO auto-closure protocol:** propagate `[ ]` -> `[x]` only when:
     1. explicit mapping exists (`ID:` source item and `SATISFIES:` target item ID), and
     2. evidence proves full target acceptance-criteria coverage.
   - If either condition fails, keep target items open/partial and add concrete missing work.
9. Update implementation-tracking tables.
   - Update `Implementation Order` status to match evidence (`[x]` full, `[/]` partial).
   - Update OS comparison status/content only for rows backed by evidence; preserve icon headers and column order.
10. Run adversarial review for this section.
   - Use `$codex-adversarial-review-section` against the implemented section scope.
   - Fix findings and re-review in a loop (max 3 rounds) until no unresolved High/Critical findings remain.
11. Run targeted section validation.
   - Use `$verify-todo-section` on this section after fixes.
   - Ensure section state, checklist status, and any cross-TODO sync/auto-closure edits still match evidence.
12. Commit and push after section completion.
   - Use the section `Commit:` message.
   - Stage code + tests + TODO updates together.
   - Push to `origin/main` and then mark the TODO commit checklist item `[x]` when that push succeeded.

## Guardrails

- Do not batch unrelated sections into one implementation.
- Do not mark `[x]` from intent, partial progress, or stubs.
- If prerequisites are missing, keep work open/partial and add prerequisite ownership tasks before dependents.
- Do not auto-close referenced TODO items from plain `→ XREF` text alone; require `ID`/`SATISFIES` mapping plus full proof.
- Do not skip section adversarial review and targeted section validation before commit.
- Do not widen into broad roadmap cleanup.

## Additional Resources

- [build-evidence.md](build-evidence.md)
