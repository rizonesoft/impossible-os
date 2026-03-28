---
name: create-todo
description: Create lean project TODO files under todo/, choose the correct domain and local number, build the canonical Implementation Order table, wire XREFs, and update indexes. Use when the user asks to create, author, scaffold, or derive a new TODO roadmap from a feature, spec, or subsystem.
---

# Create TODO

## Use This Skill When

- The user wants a new TODO file under `todo/`.
- A feature, subsystem, or spec needs a tracked execution roadmap.
- A stale draft needs to be rewritten into the current lean TODO format.

## Workflow

1. Find the canonical home.
   - Read `todo/TODO-00-INDEX.md`, the target domain `INDEX.md`, and adjacent TODOs.
   - Prefer one canonical TODO home and cross-link instead of duplicating scope.
2. Gather the primary inputs.
   - If a spec exists, treat it as the primary structural input.
   - Otherwise use current code, docs, and related TODOs to define scope.
   - Use Grep/Glob to discover existing implementations, related symbols, and prior art.
   - Use `git log` to discover recent work that may already cover planned scope.
3. Choose the next local filename.
   - Follow the live domain naming pattern such as `todo/00-infrastructure/TODO-03-short-name.md`.
   - Do not revive legacy `TODO-NNN.NN-*` naming from `todo-old/`.
4. Draft the TODO in the current lean format.
   - Default to a leaf TODO.
   - Create a parent TODO only when the topic truly needs multiple child files or shared verification.
5. Set up execution order at creation time.
   - Use `Implementation Order`, not `Phase-by-Phase`.
   - Mark every row with `💎` (parity — matches Windows/Linux) or `⭐` (exclusive — Impossible OS superior).
   - Add dependencies, overlap notes, handoffs, and `→ XREF:` links while creating the file.
6. Do NOT add model tags to section headings.
   - No `[Opus]` or `[Sonnet]` postfixes — model selection is handled by the harness, not the TODO.
   - Section headings should be clean: `## 1. Section Title` — no tags, no annotations.
7. Add an OS Comparison table (compact format).
   - Every TODO must include an `## OS Comparison` section before Verification.
   - Columns: `⭐ | Feature | Win11 | Linux | Impossible OS`
   - Keep cells short: status emoji + max 5 words per cell. No full sentences.
   - Mark each row `💎` (parity) or `⭐` (exclusive/superior).
   - Show current Impossible OS state: ✅ Done, ⚠️ Partial, ⬜ Planned, ❌ Not applicable.
   - Add a short summary note after the table.
8. Add test checkpoints to every section.
   - Each section must end with a `**Test checkpoint:**` block.
   - Checkpoints must have concrete pass/fail criteria (exact POST codes, serial strings, screen output).
   - Checkpoints must specify which platforms to test on (QEMU WHPX, TCG, VBox, bare metal).
9. Add regression risk notes to high-risk sections.
   - Sections that touch interrupt path, page tables, GDT/TSS, timer, or boot order must include:
     - A "Regression risk:" note identifying what could break.
     - A rollback strategy: "If this breaks, revert [change] and fall back to [behavior]."
10. Update indexes in the same task.
   - Update the domain `INDEX.md`.
   - Update `todo/TODO-00-INDEX.md` only when the new TODO changes root-visible scope.
8. Update indexes in the same task.
   - Update the domain `INDEX.md`.
   - Update `todo/TODO-00-INDEX.md` only when the new TODO changes root-visible scope.

## Guardrails

- Do not implement code or reconcile checkbox truth against the codebase.
- Do not add giant mutable prompt blocks, formatting-only cleanup steps, or MCP-memory instructions.
- Keep tables compact and split wide planning content into bullets or short subsections.

## Additional Resources

- [todo-template.md](todo-template.md)
- [implementation-order.md](implementation-order.md)
